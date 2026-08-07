#include "espnow_link.h"

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/timers.h"
#include "esp_wifi.h"
#include "esp_now.h"
#include "esp_mac.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "nvs_flash.h"
#include "nvs.h"

#include "control_engine.h"
#include "settings.h"

#define TAG "espnow"

#define ESPNOW_MAX_PKT        250
#define TELEM_TIMER_PERIOD_US (VFD_TELEM_PERIOD_MS * 1000)
#define NVS_NAMESPACE         "espnow"
#define NVS_KEY_PEER_MAC      "peer_mac"

/* ------------------------------------------------------------------ */
static struct {
    uint8_t  peer_mac[6];
    bool     peer_known;
    uint8_t  seq;
    uint8_t  channel;           /* single channel for AP+STA+ESP-NOW  */
    esp_timer_handle_t telem_timer;

    /* link quality */
    uint32_t rx_ok;
    uint32_t rx_total_expected;
    uint8_t  last_seq;
    bool     seq_init;

    /* remote heartbeat */
    vfd_heartbeat_t hb;
    bool            hb_valid;
    int64_t         hb_time_us;
} s_link;

/* ------------------------------------------------------------------ */
static esp_err_t send_raw(vfd_packet_type_t type, const void *payload, size_t len)
{
    if (!s_link.peer_known) {
        return ESP_ERR_NOT_FOUND;
    }
    uint8_t buf[ESPNOW_MAX_PKT];
    int n = vfd_pkt_build(buf, sizeof(buf), type,
                          VFD_DEV_CONTROLLER, VFD_DEV_REMOTE,
                          s_link.seq++, payload, len);
    if (n <= 0) {
        return ESP_ERR_INVALID_ARG;
    }
    return esp_now_send(s_link.peer_mac, buf, (size_t)n);
}

esp_err_t espnow_link_send(vfd_packet_type_t type, const void *payload, size_t len)
{
    return send_raw(type, payload, len);
}

/* ------------------------------------------------------------------ */
/* Telemetry @10 Hz                                                    */
/* ------------------------------------------------------------------ */
static void telem_timer_cb(void *arg)
{
    vfd_telemetry_t t;
    control_engine_get_telemetry(&t);
    t.link_quality = espnow_link_quality();
    send_raw(VFD_PKT_TELEMETRY, &t, sizeof(t));
}

/* ------------------------------------------------------------------ */
/* RX handling                                                         */
/* ------------------------------------------------------------------ */
static void on_recv(const esp_now_recv_info_t *info, const uint8_t *data, int len)
{
    vfd_pkt_hdr_t hdr;
    const uint8_t *payload = NULL;
    size_t plen = 0;

    if (!vfd_pkt_parse(data, (size_t)len, &hdr, &payload, &plen)) {
        return;
    }
    if (!vfd_pkt_for_me(&hdr, VFD_DEV_CONTROLLER)) {
        return;
    }

    /* Link is alive. */
    control_engine_link_alive();

    /* Sequence-based loss accounting. */
    s_link.rx_ok++;
    if (s_link.seq_init) {
        uint8_t expected = (uint8_t)(s_link.last_seq + 1);
        s_link.rx_total_expected += (hdr.seq >= expected)
            ? (uint32_t)(hdr.seq - expected + 1) : 1;
    } else {
        s_link.rx_total_expected = 1;
        s_link.seq_init = true;
    }
    s_link.last_seq = hdr.seq;

    /* Auto-learn peer MAC from first valid remote packet. */
    if (!s_link.peer_known && info && info->src_addr) {
        espnow_link_set_peer(info->src_addr);
    }

    switch (hdr.type) {
    case VFD_PKT_CMD:
        if (plen >= sizeof(vfd_cmd_t)) {
            vfd_cmd_t c;
            memcpy(&c, payload, sizeof(c));
            control_engine_cmd((vfd_cmd_code_t)c.cmd, c.arg2);
        }
        break;
    case VFD_PKT_TORQUE_SET:
        if (plen >= sizeof(vfd_torque_set_t)) {
            vfd_torque_set_t ts;
            memcpy(&ts, payload, sizeof(ts));
            control_engine_set_torque(ts.main_torque_set, ts.travel_torque_set);
        }
        break;
    case VFD_PKT_HEARTBEAT:
        if (plen >= sizeof(vfd_heartbeat_t)) {
            memcpy(&s_link.hb, payload, sizeof(vfd_heartbeat_t));
            s_link.hb_valid = true;
            s_link.hb_time_us = esp_timer_get_time();
        }
        break;
    default:
        break;
    }
}

static void on_sent(const esp_now_send_info_t *info, esp_now_send_status_t status)
{
    /* nothing needed; link quality tracked on RX side */
}

/* ------------------------------------------------------------------ */
esp_err_t espnow_link_set_peer(const uint8_t mac[6])
{
    memcpy(s_link.peer_mac, mac, 6);

    esp_now_peer_info_t peer = {0};
    memcpy(peer.peer_addr, mac, 6);
    peer.channel = s_link.channel;
    peer.ifidx   = WIFI_IF_STA;
    peer.encrypt = false;

    if (esp_now_is_peer_exist(mac)) {
        esp_now_del_peer(mac);
    }
    esp_err_t err = esp_now_add_peer(&peer);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "add_peer failed: %s", esp_err_to_name(err));
        return err;
    }

    /* Long-range per-peer rate: LR phymode, 250 kbps. */
    esp_now_rate_config_t rate = {
        .phymode = WIFI_PHY_MODE_LR,
        .rate    = WIFI_PHY_RATE_LORA_250K,
        .ersu    = false,
        .dcm     = false,
    };
    esp_err_t rerr = esp_now_set_peer_rate_config(mac, &rate);
    if (rerr != ESP_OK) {
        ESP_LOGW(TAG, "set LR rate failed: %s", esp_err_to_name(rerr));
    }

    s_link.peer_known = true;

    /* Persist peer MAC. */
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_blob(h, NVS_KEY_PEER_MAC, mac, 6);
        nvs_commit(h);
        nvs_close(h);
    }

    ESP_LOGI(TAG, "peer set %02X:%02X:%02X:%02X:%02X:%02X (LR)",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    return ESP_OK;
}

/* ------------------------------------------------------------------ */
void espnow_link_announce_channel(uint8_t new_channel)
{
    if (!s_link.peer_known) {
        ESP_LOGW(TAG, "no peer; cannot announce ch %d", new_channel);
        return;
    }
    vfd_channel_switch_t cs = { .channel = new_channel };
    /* Best effort: repeat on the current channel; the remote persists
     * the new channel on first reception. */
    for (int i = 0; i < 5; i++) {
        send_raw(VFD_PKT_CHANNEL_SWITCH, &cs, sizeof(cs));
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    ESP_LOGI(TAG, "announced channel switch -> %d", new_channel);
}

/* ------------------------------------------------------------------ */
static void wifi_event_handler(void *arg, esp_event_base_t base,
                              int32_t id, void *data)
{
    /* APSTA brings interfaces up; nothing specific required for esp-now. */
}

esp_err_t espnow_link_init(void)
{
    memset(&s_link, 0, sizeof(s_link));
    s_link.channel = settings_get()->wifi_channel;

    /* TCP/IP stack + default event loop (idempotent, may already exist). */
    esp_err_t nerr = esp_netif_init();
    if (nerr != ESP_OK && nerr != ESP_ERR_INVALID_STATE) {
        return nerr;
    }
    esp_err_t lerr = esp_event_loop_create_default();
    if (lerr != ESP_OK && lerr != ESP_ERR_INVALID_STATE) {
        return lerr;
    }

    /* WiFi: AP + STA on the same channel. */
    if (esp_netif_create_default_wifi_sta() == NULL ||
        esp_netif_create_default_wifi_ap() == NULL) {
        ESP_LOGE(TAG, "netif create failed");
        return ESP_FAIL;
    }

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    /* Allow channels 1..13 (default country "01" only allows 1..11). */
    wifi_country_t country = {
        .cc = "EU",
        .schan = 1,
        .nchan = 13,
        .policy = WIFI_COUNTRY_POLICY_MANUAL,
    };
    ESP_ERROR_CHECK(esp_wifi_set_country(&country));

    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                               &wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));

    /* Soft-AP config (for dashboard). */
    wifi_config_t ap_cfg = {0};
    snprintf((char *)ap_cfg.ap.ssid, sizeof(ap_cfg.ap.ssid), "VFD-CUT-22KW");
    ap_cfg.ap.ssid_len       = strlen((char *)ap_cfg.ap.ssid);
    ap_cfg.ap.channel        = s_link.channel;
    ap_cfg.ap.authmode       = WIFI_AUTH_OPEN;
    ap_cfg.ap.max_connection = 4;
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_cfg));

    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_ERROR_CHECK(esp_wifi_set_channel(s_link.channel,
                                         WIFI_SECOND_CHAN_NONE));

    /* Enable Long Range protocol on both interfaces (runtime). */
    esp_err_t err = esp_wifi_set_protocol(WIFI_IF_STA,
        WIFI_PROTOCOL_11B | WIFI_PROTOCOL_11G | WIFI_PROTOCOL_11N | WIFI_PROTOCOL_LR);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "LR on STA failed: %s", esp_err_to_name(err));
    }
    err = esp_wifi_set_protocol(WIFI_IF_AP,
        WIFI_PROTOCOL_11B | WIFI_PROTOCOL_11G | WIFI_PROTOCOL_11N | WIFI_PROTOCOL_LR);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "LR on AP failed: %s", esp_err_to_name(err));
    }

    /* ESP-NOW. */
    ESP_ERROR_CHECK(esp_now_init());
    ESP_ERROR_CHECK(esp_now_register_recv_cb(on_recv));
    ESP_ERROR_CHECK(esp_now_register_send_cb(on_sent));

    /* Re-add saved peer now that esp-now is up. */
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) == ESP_OK) {
        uint8_t mac[6];
        size_t sz = sizeof(mac);
        if (nvs_get_blob(h, NVS_KEY_PEER_MAC, mac, &sz) == ESP_OK && sz == 6) {
            nvs_close(h);
            espnow_link_set_peer(mac);
        } else {
            nvs_close(h);
        }
    }

    /* Telemetry timer @10 Hz. */
    const esp_timer_create_args_t targs = {
        .callback = telem_timer_cb,
        .name = "telem",
    };
    ESP_ERROR_CHECK(esp_timer_create(&targs, &s_link.telem_timer));
    ESP_ERROR_CHECK(esp_timer_start_periodic(s_link.telem_timer,
                                             TELEM_TIMER_PERIOD_US));

    ESP_LOGI(TAG, "esp-now LR link initialized (ch %d)", s_link.channel);
    return ESP_OK;
}

void espnow_link_get_remote_hb(vfd_heartbeat_t *out, bool *valid)
{
    if (out) {
        *out = s_link.hb;
    }
    if (valid) {
        /* heartbeat considered stale after 3 s */
        *valid = s_link.hb_valid &&
                 (esp_timer_get_time() - s_link.hb_time_us) < 3000000LL;
    }
}

uint8_t espnow_link_quality(void)
{
    if (s_link.rx_total_expected == 0) {
        return 0;
    }
    uint32_t q = (s_link.rx_ok * 100u) / s_link.rx_total_expected;
    return (uint8_t)(q > 100 ? 100 : q);
}
