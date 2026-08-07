#include "web_server.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include "esp_log.h"
#include "esp_http_server.h"
#include "esp_timer.h"
#include "esp_system.h"
#include "cJSON.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "control_engine.h"
#include "settings.h"
#include "sys_logger.h"
#include "espnow_link.h"

#define TAG "web"

static httpd_handle_t s_server;
static int s_ws_fd = -1;

/* ------------------------------------------------------------------ */
/* Embedded dashboard HTML                                             */
/* ------------------------------------------------------------------ */
static const char INDEX_HTML[] =
"<!DOCTYPE html><html><head><meta charset='utf-8'>"
"<meta name='viewport' content='width=device-width,initial-scale=1'>"
"<title>22kW VFD Cutter</title><style>"
"body{font-family:system-ui;background:#111;color:#eee;margin:0;padding:16px}"
"h1{font-size:20px}.grid{display:grid;grid-template-columns:1fr 1fr;gap:12px}"
".card{background:#1c1c1c;border-radius:12px;padding:14px}"
".v{font-size:28px;font-weight:700}.u{font-size:12px;color:#999}"
".row{display:flex;justify-content:space-between;margin:6px 0}"
"button{background:#2d7;color:#000;border:0;border-radius:8px;padding:10px 16px;font-size:15px;margin:4px;cursor:pointer}"
"button.warn{background:#f80}button.danger{background:#f33;color:#fff}"
"input{background:#222;color:#eee;border:1px solid #444;border-radius:6px;padding:6px;margin:2px;width:180px}"
"select{background:#222;color:#eee;border-radius:6px;padding:6px}"
".state{font-size:16px;padding:4px 10px;border-radius:6px;background:#2d7;color:#000;display:inline-block}"
"#logs{font-family:monospace;font-size:11px;max-height:200px;overflow:auto;background:#000;padding:8px;border-radius:8px;white-space:pre-wrap}"
"</style></head><body>"
"<h1>22kW Diamond Wire Cutter <span class='state' id='state'>--</span></h1>"
"<div class='grid'>"
"<div class='card'><b>Main Motor (22kW)</b>"
"<div class='row'><span>Torque</span><span><span class='v' id='mt'>--</span><span class='u'>%</span></span></div>"
"<div class='row'><span>Set</span><span id='mts'>--</span></div>"
"<div class='row'><span>Freq</span><span><span id='mf'>--</span> Hz</span></div>"
"<div class='row'><span>Current</span><span><span id='mc'>--</span> A</span></div>"
"<div class='row'><span>Bus</span><span><span id='mb'>--</span> V</span></div></div>"
"<div class='card'><b>Travel Motor</b>"
"<div class='row'><span>Torque</span><span><span class='v' id='tt'>--</span><span class='u'>%</span></span></div>"
"<div class='row'><span>Set</span><span id='tts'>--</span></div>"
"<div class='row'><span>Freq</span><span><span id='tf'>--</span> Hz</span></div>"
"<div class='row'><span>Current</span><span><span id='tc'>--</span> A</span></div>"
"<div class='row'><span>Wire tension</span><span id='wt'>--</span></div></div>"
"<div class='card'><b>Control</b>"
"<button onclick='cmd(1)'>Start Cut</button>"
"<button onclick='cmd(2)'>Fresh Cut</button>"
"<button onclick='cmd(5)'>Tension Test</button><br>"
"<button class='warn' onclick='cmd(3)'>Stop</button>"
"<button onclick='cmd(6)'>Fault Reset</button>"
"<button class='danger' onclick='cmd(4)'>E-STOP</button></div>"
"<div class='card'><b>Link</b>"
"<div class='row'><span>Quality</span><span id='lq'>--</span></div>"
"<div class='row'><span>Alarm</span><span id='al'>--</span></div>"
"<div class='row'><span>Uptime</span><span id='up'>--</span></div>"
"<div class='row'><span>Battery</span><span id='bat'>--</span></div></div></div>"
"<div class='card' style='margin-top:12px'><b>Settings</b>"
"<div>WiFi SSID <input id='ssid'> Pass <input id='pass' type='password'> Channel (1-13) <input id='wchan' style='width:60px' type='number' min='1' max='13'></div>"
"<div>UDP IP <input id='udpip'> Port <input id='udpport' style='width:80px'></div>"
"<div>Upload URL <input id='upurl' style='width:320px'></div>"
"<div>OTA URL <input id='otaurl' style='width:320px'></div>"
"<div>Main addr <input id='am' style='width:60px'> Travel addr <input id='at' style='width:60px'> Baud <input id='baud' style='width:90px'></div>"
"<button onclick='save()'>Save</button> <span id='saveok'></span></div>"
"<div class='card' style='margin-top:12px'><b>Logs</b> <button onclick='loadlogs()'>Refresh</button><div id='logs'></div></div>"
"<script>"
"function $(i){return document.getElementById(i)}"
"function cmd(c){fetch('/api/cmd',{method:'POST',body:JSON.stringify({cmd:c,arg:0})})}"
"function save(){fetch('/api/settings',{method:'POST',body:JSON.stringify({wifi_ssid:$('ssid').value,wifi_pass:$('pass').value,wifi_channel:+$('wchan').value,udp_ip:$('udpip').value,udp_port:+$('udpport').value,upload_url:$('upurl').value,ota_url:$('otaurl').value,vfd_addr_main:+$('am').value,vfd_addr_travel:+$('at').value,rs485_baud:+$('baud').value})}).then(r=>r.json()).then(j=>{$('saveok').textContent=j.reboot?'Saved! Rebooting to new channel...':(j.ok?'Saved!':'Error')})}"
"function loadlogs(){fetch('/api/logs').then(r=>r.json()).then(j=>{$('logs').textContent=j.lines.join('\\n')})}"
"const ST=['IDLE','CONNECTING','READY','STARTING','CUTTING','FRESH_CUT','REVERSING','STOPPING','E-STOP','FAULT'];"
"let ws;"
"function connect(){ws=new WebSocket('ws://'+location.host+'/ws');"
"ws.onmessage=e=>{let d=JSON.parse(e.data);"
"$('state').textContent=ST[d.machine_state]||d.machine_state;"
"$('mt').textContent=(d.main_torque_pct/100).toFixed(1);"
"$('mts').textContent=(d.main_torque_set/100).toFixed(1);"
"$('mf').textContent=(d.main_freq_hz/100).toFixed(1);"
"$('mc').textContent=(d.main_current_a/10).toFixed(1);"
"$('mb').textContent=d.main_bus_v;"
"$('tt').textContent=(d.travel_torque_pct/100).toFixed(1);"
"$('tts').textContent=(d.travel_torque_set/100).toFixed(1);"
"$('tf').textContent=(d.travel_freq_hz/100).toFixed(1);"
"$('tc').textContent=(d.travel_current_a/10).toFixed(1);"
"$('wt').textContent=(d.wire_tension_raw/100).toFixed(1)+'%';"
"$('lq').textContent=d.link_quality+'%';"
"$('al').textContent=d.alarm;"
"$('up').textContent=Math.floor(d.uptime_ms/1000)+'s';};"
"ws.onclose=()=>setTimeout(connect,2000);}"
"connect();"
"fetch('/api/settings').then(r=>r.json()).then(s=>{$('ssid').value=s.wifi_ssid;$('wchan').value=s.wifi_channel;$('udpip').value=s.udp_ip;$('udpport').value=s.udp_port;$('upurl').value=s.upload_url;$('otaurl').value=s.ota_url;$('am').value=s.vfd_addr_main;$('at').value=s.vfd_addr_travel;$('baud').value=s.rs485_baud;});"
"</script></body></html>";

/* ------------------------------------------------------------------ */
static esp_err_t root_get(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, INDEX_HTML, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t status_get(httpd_req_t *req)
{
    vfd_telemetry_t t;
    control_engine_get_telemetry(&t);
    t.link_quality = espnow_link_quality();

    cJSON *j = cJSON_CreateObject();
    cJSON_AddNumberToObject(j, "machine_state", t.machine_state);
    cJSON_AddNumberToObject(j, "alarm", t.alarm);
    cJSON_AddNumberToObject(j, "main_torque_pct", t.main_torque_pct);
    cJSON_AddNumberToObject(j, "main_torque_set", t.main_torque_set);
    cJSON_AddNumberToObject(j, "main_freq_hz", t.main_freq_hz);
    cJSON_AddNumberToObject(j, "main_current_a", t.main_current_a);
    cJSON_AddNumberToObject(j, "main_bus_v", t.main_bus_v);
    cJSON_AddNumberToObject(j, "travel_torque_pct", t.travel_torque_pct);
    cJSON_AddNumberToObject(j, "travel_torque_set", t.travel_torque_set);
    cJSON_AddNumberToObject(j, "travel_freq_hz", t.travel_freq_hz);
    cJSON_AddNumberToObject(j, "travel_current_a", t.travel_current_a);
    cJSON_AddNumberToObject(j, "wire_tension_raw", t.wire_tension_raw);
    cJSON_AddNumberToObject(j, "link_quality", t.link_quality);
    cJSON_AddNumberToObject(j, "uptime_ms", t.uptime_ms);
    char *s = cJSON_PrintUnformatted(j);
    cJSON_Delete(j);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, s);
    free(s);
    return ESP_OK;
}

static esp_err_t settings_get_handler(httpd_req_t *req)
{
    const app_settings_t *s = settings_get();
    cJSON *j = cJSON_CreateObject();
    cJSON_AddStringToObject(j, "wifi_ssid", s->wifi_ssid);
    cJSON_AddStringToObject(j, "wifi_pass", "");
    cJSON_AddNumberToObject(j, "wifi_channel", s->wifi_channel);
    cJSON_AddStringToObject(j, "udp_ip", s->udp_ip);
    cJSON_AddNumberToObject(j, "udp_port", s->udp_port);
    cJSON_AddStringToObject(j, "upload_url", s->upload_url);
    cJSON_AddStringToObject(j, "ota_url", s->ota_url);
    cJSON_AddNumberToObject(j, "rs485_baud", s->rs485_baud);
    cJSON_AddNumberToObject(j, "vfd_addr_main", s->vfd_addr_main);
    cJSON_AddNumberToObject(j, "vfd_addr_travel", s->vfd_addr_travel);
    char *str = cJSON_PrintUnformatted(j);
    cJSON_Delete(j);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, str);
    free(str);
    return ESP_OK;
}

static esp_err_t settings_post(httpd_req_t *req)
{
    char buf[512];
    int n = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (n <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "no body");
        return ESP_FAIL;
    }
    buf[n] = '\0';
    cJSON *j = cJSON_Parse(buf);
    if (!j) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad json");
        return ESP_FAIL;
    }

    app_settings_t s = *settings_get();
    cJSON *it;
    if ((it = cJSON_GetObjectItem(j, "wifi_ssid")) && cJSON_IsString(it))
        strncpy(s.wifi_ssid, it->valuestring, sizeof(s.wifi_ssid) - 1);
    if ((it = cJSON_GetObjectItem(j, "wifi_pass")) && cJSON_IsString(it) && it->valuestring[0])
        strncpy(s.wifi_pass, it->valuestring, sizeof(s.wifi_pass) - 1);
    if ((it = cJSON_GetObjectItem(j, "wifi_channel")) && cJSON_IsNumber(it)) {
        int ch = (int)it->valuedouble;
        if (ch < 1 || ch > 13) {
            cJSON_Delete(j);
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                                "wifi_channel must be 1..13");
            return ESP_FAIL;
        }
        s.wifi_channel = (uint8_t)ch;
    }
    if ((it = cJSON_GetObjectItem(j, "udp_ip")) && cJSON_IsString(it))
        strncpy(s.udp_ip, it->valuestring, sizeof(s.udp_ip) - 1);
    if ((it = cJSON_GetObjectItem(j, "udp_port")) && cJSON_IsNumber(it))
        s.udp_port = (uint16_t)it->valuedouble;
    if ((it = cJSON_GetObjectItem(j, "upload_url")) && cJSON_IsString(it))
        strncpy(s.upload_url, it->valuestring, sizeof(s.upload_url) - 1);
    if ((it = cJSON_GetObjectItem(j, "ota_url")) && cJSON_IsString(it))
        strncpy(s.ota_url, it->valuestring, sizeof(s.ota_url) - 1);
    if ((it = cJSON_GetObjectItem(j, "rs485_baud")) && cJSON_IsNumber(it))
        s.rs485_baud = (uint32_t)it->valuedouble;
    if ((it = cJSON_GetObjectItem(j, "vfd_addr_main")) && cJSON_IsNumber(it))
        s.vfd_addr_main = (uint8_t)it->valuedouble;
    if ((it = cJSON_GetObjectItem(j, "vfd_addr_travel")) && cJSON_IsNumber(it))
        s.vfd_addr_travel = (uint8_t)it->valuedouble;
    cJSON_Delete(j);

    /* Channel change requested? */
    const app_settings_t *old = settings_get();
    bool channel_changed = (s.wifi_channel != old->wifi_channel);

    if (channel_changed) {
        /* Never switch channel mid-cut. */
        vfd_machine_state_t st = control_engine_get_state();
        if (st == VFD_STATE_STARTING || st == VFD_STATE_CUTTING ||
            st == VFD_STATE_FRESH_CUT || st == VFD_STATE_REVERSING ||
            st == VFD_STATE_STOPPING) {
            httpd_resp_set_type(req, "application/json");
            httpd_resp_sendstr(req,
                "{\"ok\":false,\"error\":\"cutting active\"}");
            return ESP_OK;
        }
    }

    esp_err_t err = settings_save(&s);
    httpd_resp_set_type(req, "application/json");

    if (err == ESP_OK && channel_changed) {
        httpd_resp_sendstr(req, "{\"ok\":true,\"reboot\":true}");
        ESP_LOGW(TAG, "channel change %d -> %d; rebooting",
                 old->wifi_channel, s.wifi_channel);
        /* Tell the remote on the CURRENT channel, then reboot. */
        espnow_link_announce_channel(s.wifi_channel);
        vTaskDelay(pdMS_TO_TICKS(500));
        esp_restart();
        /* not reached */
    }

    httpd_resp_sendstr(req, err == ESP_OK ? "{\"ok\":true}" : "{\"ok\":false}");
    return ESP_OK;
}

static esp_err_t cmd_post(httpd_req_t *req)
{
    char buf[64];
    int n = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (n <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "no body");
        return ESP_FAIL;
    }
    buf[n] = '\0';
    cJSON *j = cJSON_Parse(buf);
    if (!j) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad json");
        return ESP_FAIL;
    }
    int cmd = cJSON_GetNumberValue(cJSON_GetObjectItem(j, "cmd"));
    int arg = cJSON_GetNumberValue(cJSON_GetObjectItem(j, "arg"));
    cJSON_Delete(j);

    esp_err_t err = control_engine_cmd((vfd_cmd_code_t)cmd, (uint32_t)arg);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, err == ESP_OK ? "{\"ok\":true}" : "{\"ok\":false}");
    return ESP_OK;
}

static esp_err_t logs_get(httpd_req_t *req)
{
    static char drain[8192];
    int lines = sys_logger_drain(drain, sizeof(drain), 200);

    cJSON *j = cJSON_CreateObject();
    cJSON *arr = cJSON_AddArrayToObject(j, "lines");
    const char *p = drain;
    for (int i = 0; i < lines; i++) {
        cJSON_AddItemToArray(arr, cJSON_CreateString(p));
        p += strlen(p) + 1;
    }
    char *s = cJSON_PrintUnformatted(j);
    cJSON_Delete(j);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, s);
    free(s);
    return ESP_OK;
}

/* ------------------------------------------------------------------ */
/* WebSocket: push telemetry ~5 Hz                                     */
/* ------------------------------------------------------------------ */
static esp_err_t ws_handler(httpd_req_t *req)
{
    if (req->method == HTTP_GET) {
        s_ws_fd = httpd_req_to_sockfd(req);
        return ESP_OK;
    }
    /* ignore incoming ws frames */
    char buf[64];
    httpd_ws_frame_t pkt = {
        .type = HTTPD_WS_TYPE_TEXT,
        .payload = (uint8_t *)buf,
        .len = sizeof(buf),
    };
    httpd_ws_recv_frame(req, &pkt, sizeof(buf));
    return ESP_OK;
}

static void ws_push_task(void *arg)
{
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(200));   /* 5 Hz */
        if (s_ws_fd < 0 || s_server == NULL) {
            continue;
        }
        vfd_telemetry_t t;
        control_engine_get_telemetry(&t);
        t.link_quality = espnow_link_quality();

        char payload[512];
        int len = snprintf(payload, sizeof(payload),
            "{\"machine_state\":%d,\"alarm\":%d,"
            "\"main_torque_pct\":%d,\"main_torque_set\":%d,"
            "\"main_freq_hz\":%d,\"main_current_a\":%d,\"main_bus_v\":%d,"
            "\"travel_torque_pct\":%d,\"travel_torque_set\":%d,"
            "\"travel_freq_hz\":%d,\"travel_current_a\":%d,"
            "\"wire_tension_raw\":%d,\"link_quality\":%d,\"uptime_ms\":%lu}",
            t.machine_state, t.alarm,
            t.main_torque_pct, t.main_torque_set,
            t.main_freq_hz, t.main_current_a, t.main_bus_v,
            t.travel_torque_pct, t.travel_torque_set,
            t.travel_freq_hz, t.travel_current_a,
            t.wire_tension_raw, t.link_quality,
            (unsigned long)t.uptime_ms);

        httpd_ws_frame_t frame = {
            .type = HTTPD_WS_TYPE_TEXT,
            .payload = (uint8_t *)payload,
            .len = (size_t)len,
        };
        if (httpd_ws_send_frame_async(s_server, s_ws_fd, &frame) != ESP_OK) {
            s_ws_fd = -1;   /* client gone */
        }
    }
}

/* ------------------------------------------------------------------ */
void web_server_start(void)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = 80;
    config.max_uri_handlers = 8;

    if (httpd_start(&s_server, &config) != ESP_OK) {
        ESP_LOGE(TAG, "failed to start httpd");
        return;
    }

    httpd_uri_t uris[] = {
        { .uri = "/",             .method = HTTP_GET,  .handler = root_get },
        { .uri = "/api/status",   .method = HTTP_GET,  .handler = status_get },
        { .uri = "/api/settings", .method = HTTP_GET,  .handler = settings_get_handler },
        { .uri = "/api/settings", .method = HTTP_POST, .handler = settings_post },
        { .uri = "/api/cmd",      .method = HTTP_POST, .handler = cmd_post },
        { .uri = "/api/logs",     .method = HTTP_GET,  .handler = logs_get },
        { .uri = "/ws",           .method = HTTP_GET,  .handler = ws_handler,
          .is_websocket = true },
    };
    for (size_t i = 0; i < sizeof(uris) / sizeof(uris[0]); i++) {
        httpd_register_uri_handler(s_server, &uris[i]);
    }

    xTaskCreate(ws_push_task, "ws_push", 4096, NULL, 4, NULL);
    ESP_LOGI(TAG, "web server started on :80");
}
