#include "udp_broadcast.h"

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "lwip/sockets.h"
#include "lwip/netdb.h"

#include "settings.h"
#include "control_engine.h"
#include "espnow_link.h"
#include "wifi_manager.h"
#include "vfd_protocol.h"

#define TAG "udp_bcast"

static void udp_task(void *arg)
{
    int sock = -1;
    struct sockaddr_in dest = {0};
    char ip_cached[16] = {0};
    uint8_t pkt[sizeof(vfd_pkt_hdr_t) + sizeof(vfd_telemetry_t)];
    uint8_t seq = 0;

    for (;;) {
        /* Only send when STA is up. */
        if (!wifi_manager_sta_connected()) {
            if (sock >= 0) {
                close(sock);
                sock = -1;
            }
            vTaskDelay(pdMS_TO_TICKS(500));
            continue;
        }

        /* (Re)build socket when the target changes. */
        const app_settings_t *cfg = settings_get();
        if (sock < 0 || strcmp(ip_cached, cfg->udp_ip) != 0) {
            if (sock >= 0) {
                close(sock);
            }
            sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
            if (sock < 0) {
                vTaskDelay(pdMS_TO_TICKS(1000));
                continue;
            }
            int bcast = 1;
            setsockopt(sock, SOL_SOCKET, SO_BROADCAST, &bcast, sizeof(bcast));

            memset(&dest, 0, sizeof(dest));
            dest.sin_family = AF_INET;
            dest.sin_port = htons(cfg->udp_port);
            dest.sin_addr.s_addr = inet_addr(cfg->udp_ip);
            strncpy(ip_cached, cfg->udp_ip, sizeof(ip_cached) - 1);
            ESP_LOGI(TAG, "broadcasting to %s:%d", cfg->udp_ip, cfg->udp_port);
        }

        /* Grab current telemetry and send. */
        vfd_telemetry_t t;
        control_engine_get_telemetry(&t);
        t.link_quality = espnow_link_quality();

        int n = vfd_pkt_build(pkt, sizeof(pkt), VFD_PKT_TELEMETRY,
                              VFD_DEV_CONTROLLER, VFD_DEV_REMOTE,
                              seq++, &t, sizeof(t));
        if (n > 0) {
            sendto(sock, pkt, n, 0,
                   (struct sockaddr *)&dest, sizeof(dest));
        }

        vTaskDelay(pdMS_TO_TICKS(VFD_TELEM_PERIOD_MS));   /* 10 Hz */
    }
}

void udp_broadcast_start(void)
{
    if (xTaskCreate(udp_task, "udp_bcast", 4096, NULL, 4, NULL) != pdPASS) {
        ESP_LOGE(TAG, "task create failed");
    }
}
