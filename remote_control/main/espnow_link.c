#include "espnow_link.h"

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_wifi.h"
#include "esp_now.h"
#include "esp_mac.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "nvs_flash.h"
#include "nvs.h"

#include "board.h"
#include "settings.h"

#define TAG "espnow_r"

#define ESPNOW_MAX_PKT        250
#define NVS_NAMESPACE         "espnow"
#define NVS_KEY_PEER_MAC      "peer_mac"

/* ------------------------------------------------------------------ */
static struct {
    uint8_t  peer_mac[6];
    bool     peer_known;
    uint8_t  seq;
    uint8_t  channel;           /* single channel for ESP-NOW         */
    volatile bool scan_paused;  /* set while OTA wifi is active       */
    volatile bool rx_any;       /* any valid controller packet seen   */
    volatile uint8_t rx_channel;/* channel a valid packet arrived on  */

    /* latest telemetry */
    vfd_telemetry_t telem;
    bool            telem_valid;
    int64_t         telem_time_us;
    SemaphoreHandle_t mux;

    /* link quality */
    uint32_t rx_ok;
    uint32_t rx_total_expected;
    uint8_t  last_seq;
    bool     seq_init;

    espnow_telemetry_cb_t cb;
} s_link;

/* ------------------------------------------------------------------ */
static esp_err_t send_raw(vfd_packet_type_t type, const void *payload, size_t len)
{
    if (!s_link.peer_known) {
        return ESP_ERR_NOT_FOUND;
    }
    uint8_t buf[ESPNOW_MAX_PKT];
    int n = vfd_pkt_build(buf, sizeof(buf), type,
                          VFD_DEV_REMOTE, VFD_DEV_CONTROLLER,
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
void espnow_link_get_telemetry(vfd_telemetry_t *out, bool *valid)
{
    if (xSemaphoreTake(s_link.mux, pdMS_TO_TICKS(20)) == pdTRUE) {
        if (out) {
            *out = s_link.telem;
        }
        if (valid) {
            *valid = s_link.telem_valid;
        }
        xSemaphoreGive(s_link.mux);
    }
}

uint32_t espnow_link_age_ms(void)
{
    if (!s_link.telem_valid) {
        return 0xFFFFFFFFu;
    }
    int64_t age = esp_timer_get_time() - s_link.telem_time_us;
    return (age < 0) ? 0 : (uint32_t)(age / 1000);
}

uint8_t espnow_link_quality(void)
{
    if (s_link.rx_total_expected == 0) {
        return 0;
    }
    uint32_t q = (s_link.rx_ok * 100) / s_link.rx_total_expected;
    return (q > 100) ? 100 : (uint8_t)q;
}

void espnow_link_on_telemetry(espnow_telemetry_cb_t cb)
{
    s_link.cb = cb;
}

void espnow_link_scan_pause(bool pause)
{
    s_link.scan_paused = pause;
}

uint8_t espnow_link_channel(void)
{
    return s_link.channel;
}

/* ------------------------------------------------------------------ */
/* Switch the radio + peer to a new channel (runtime).                */
/* ------------------------------------------------------------------ */
static esp_err_t apply_channel(uint8_t ch)
{
    if (ch < 1 || ch > 13) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t err = esp_wifi_set_channel(ch, WIFI_SECOND_CHAN_NONE);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "set_channel(%d) failed: %s", ch, esp_err_to_name(err));
        return err;
    }
    s_link.channel = ch;

    /* Update peer channel if already known. */
    if (s_link.peer_known && esp_now_is_peer_exist(s_link.peer_mac)) {
        esp_now_peer_info_t p = {0};
        if (esp_now_get_peer(s_link.peer_mac, &p) == ESP_OK) {
            p.channel = ch;
            esp_now_mod_peer(&p);
        }
    }
    ESP_LOGI(TAG, "channel -> %d", ch);
    return ESP_OK;
}

/* ------------------------------------------------------------------ */
/* Fallback channel search: only if no controller packet arrives      */
/* within 10 s of boot (e.g. controller channel was changed while     */
/* this remote was powered off). Paused while OTA wifi is active.     */
/* ------------------------------------------------------------------ */
#define SCAN_GRACE_MS      10000
#define SCAN_DWELL_MS      120
#define SCAN_PASSES        3

static void scan_task(void *arg)
{
    vTaskDelay(pdMS_TO_TICKS(SCAN_GRACE_MS));

    for (int pass = 0; pass < SCAN_PASSES && !s_link.rx_any; pass++) {
        if (s_link.scan_paused) {
            vTaskDelay(pdMS_TO_TICKS(2000));
            pass--;                 /* don't count passes while OTA runs */
            continue;
        }
        ESP_LOGI(TAG, "no controller traffic; scanning channels (pass %d)",
                 pass + 1);
        for (uint8_t ch = 1; ch <= 13 && !s_link.rx_any; ch++) {
            if (s_link.scan_paused) {
                break;
            }
            esp_wifi_set_channel(ch, WIFI_SECOND_CHAN_NONE);
            s_link.channel = ch;
            vTaskDelay(pdMS_TO_TICKS(SCAN_DWELL_MS));
        }
    }

    if (s_link.rx_any) {
        uint8_t found = s_link.rx_channel;
        ESP_LOGI(TAG, "found controller on channel %d", found);
        apply_channel(found);
        settings_set_wifi_channel(found);
    } else {
        /* Give up; return to the configured channel. */
        apply_channel(settings_get()->wifi_channel);
        ESP_LOGW(TAG, "channel scan found nothing; back to ch %d",
                 s_link.channel);
    }
    vTaskDelete(NULL);
}

/* ------------------------------------------------------------------ */
esp_err_t espnow_link_set_peer(const uint8_t mac[6])
{
    if (!mac) {
        return ESP_ERR_INVALID_ARG;
    }
    memcpy(s_link.peer_mac, mac, 6);
    s_link.peer_known = true;

    /* (Re)add peer with LR rate config. */
    esp_now_peer_info_t p = {0};
    memcpy(p.peer_addr, mac, 6);
    p.channel = s_link.channel;
    p.ifidx   = WIFI_IF_STA;
    p.encrypt = false;
    if (!esp_now_is_peer_exist(mac)) {
        esp_now_add_peer(&p);
    }
    /* Force long-range TX rate to the peer. */
    esp_now_rate_config_t rc = {
        .phymode = WIFI_PHY_MODE_LR,
        .rate    = WIFI_PHY_RATE_LORA_250K,
        .ersu    = false,
        .dcm     = false,
    };
    esp_now_set_peer_rate_config(mac, &rc);

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
static void on_recv(const esp_now_recv_info_t *info, const uint8_t *data, int len)
{
    vfd_pkt_hdr_t hdr;
    const uint8_t *payload = NULL;
    size_t plen = 0;

    if (!vfd_pkt_parse(data, (size_t)len, &hdr, &payload, &plen)) {
        return;
    }
    if (!vfd_pkt_for_me(&hdr, VFD_DEV_REMOTE)) {
        return;
    }

    /* A valid controller packet arrived (used by fallback scan). */
    s_link.rx_any = true;
    if (info && info->rx_ctrl) {
        s_link.rx_channel = (uint8_t)info->rx_ctrl->channel;
    }

    /* Link quality accounting. */
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

    /* Auto-learn controller MAC from first valid packet. */
    if (!s_link.peer_known && info && info->src_addr) {
        espnow_link_set_peer(info->src_addr);
    }

    switch (hdr.type) {
    case VFD_PKT_TELEMETRY:
        if (plen >= sizeof(vfd_telemetry_t)) {
            if (xSemaphoreTake(s_link.mux, pdMS_TO_TICKS(10)) == pdTRUE) {
                memcpy(&s_link.telem, payload, sizeof(vfd_telemetry_t));
                s_link.telem_valid = true;
                s_link.telem_time_us = esp_timer_get_time();
                xSemaphoreGive(s_link.mux);
            }
            if (s_link.cb) {
                s_link.cb((const vfd_telemetry_t *)payload);
            }
        }
        break;
    case VFD_PKT_EVENT:
        /* events are carried in telemetry.alarm; nothing extra needed */
        break;
    case VFD_PKT_CHANNEL_SWITCH:
        if (plen >= sizeof(vfd_channel_switch_t)) {
            vfd_channel_switch_t cs;
            memcpy(&cs, payload, sizeof(cs));
            if (cs.channel >= 1 && cs.channel <= 13 &&
                cs.channel != s_link.channel) {
                ESP_LOGW(TAG, "channel switch request -> %d", cs.channel);
                settings_set_wifi_channel(cs.channel);
                /* ACK on the OLD channel so the controller hears it. */
                send_raw(VFD_PKT_ACK, NULL, 0);
                apply_channel(cs.channel);
            }
        }
        break;
    default:
        break;
    }
}

static void on_sent(const esp_now_send_info_t *info, esp_now_send_status_t status)
{
    (void)info;
    if (status != ESP_NOW_SEND_SUCCESS) {
        ESP_LOGD(TAG, "tx fail %d", status);
    }
}

/* ------------------------------------------------------------------ */
esp_err_t espnow_link_init(void)
{
    memset(&s_link, 0, sizeof(s_link));
    s_link.mux = xSemaphoreCreateMutex();
    s_link.channel = settings_get()->wifi_channel;

    /* WiFi STA only (remote has no AP). */
    if (esp_netif_create_default_wifi_sta() == NULL) {
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

    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());

    /* Pin channel + enable Long Range on STA. */
    ESP_ERROR_CHECK(esp_wifi_set_channel(s_link.channel,
                                         WIFI_SECOND_CHAN_NONE));
    ESP_ERROR_CHECK(esp_wifi_set_protocol(
        WIFI_IF_STA,
        WIFI_PROTOCOL_11B | WIFI_PROTOCOL_11G |
        WIFI_PROTOCOL_11N | WIFI_PROTOCOL_LR));

    /* ESP-NOW up. */
    ESP_ERROR_CHECK(esp_now_init());
    ESP_ERROR_CHECK(esp_now_register_recv_cb(on_recv));
    ESP_ERROR_CHECK(esp_now_register_send_cb(on_sent));

    /* Restore saved peer MAC. */
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

    /* Fallback channel search in case the controller moved channels
     * while this remote was powered off. */
    xTaskCreate(scan_task, "ch_scan", 3072, NULL, 4, NULL);

    ESP_LOGI(TAG, "remote esp-now LR ready (ch %d)", s_link.channel);
    return ESP_OK;
}
