#include "data_log.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <sys/stat.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "wear_levelling.h"

#include "board.h"

#define TAG "data_log"

#define MOUNT_POINT   "/logstore"
#define LOG_FILE      MOUNT_POINT "/telemetry.csv"
#define MAX_LOG_BYTES (1024 * 1024)   /* rotate at ~1 MB */

static wl_handle_t s_wl = WL_INVALID_HANDLE;
static SemaphoreHandle_t s_mux;
static bool s_mounted;
static size_t s_file_bytes;

/* Write buffer: batch flushes to reduce FAT wear (10 Hz samples). */
static char s_buf[2048];
static int  s_buf_len;

void data_log_init(void)
{
    s_mux = xSemaphoreCreateMutex();

    esp_vfs_fat_mount_config_t mc = {
        .max_files = 2,
        .format_if_mount_failed = true,
        .allocation_unit_size = CONFIG_WL_SECTOR_SIZE,
    };
    esp_err_t err = esp_vfs_fat_spiflash_mount_rw_wl(MOUNT_POINT,
                                                     "logstore", &mc, &s_wl);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "mount failed: %s", esp_err_to_name(err));
        return;
    }
    s_mounted = true;

    struct stat st;
    if (stat(LOG_FILE, &st) == 0) {
        s_file_bytes = (size_t)st.st_size;
    } else {
        /* header row */
        FILE *f = fopen(LOG_FILE, "w");
        if (f) {
            fprintf(f, "ts,state,alarm,mtq,mtqs,mfq,mca,mbv,"
                       "ttq,ttqs,tfq,tca,tension,depth,lq\n");
            fclose(f);
        }
        s_file_bytes = 0;
    }
    ESP_LOGI(TAG, "mounted, %u bytes pending", (unsigned)s_file_bytes);
}

static void rotate_if_needed_locked(void)
{
    if (s_file_bytes < MAX_LOG_BYTES) {
        return;
    }
    remove(MOUNT_POINT "/telemetry.old.csv");
    rename(LOG_FILE, MOUNT_POINT "/telemetry.old.csv");
    FILE *f = fopen(LOG_FILE, "w");
    if (f) {
        fprintf(f, "ts,state,alarm,mtq,mtqs,mfq,mca,mbv,"
                   "ttq,ttqs,tfq,tca,tension,depth,lq\n");
        fclose(f);
    }
    s_file_bytes = 0;
}

static void flush_locked(void)
{
    if (s_buf_len == 0 || !s_mounted) {
        return;
    }
    rotate_if_needed_locked();
    FILE *f = fopen(LOG_FILE, "a");
    if (f) {
        int w = fwrite(s_buf, 1, s_buf_len, f);
        fclose(f);
        if (w > 0) {
            s_file_bytes += (size_t)w;
        }
    }
    s_buf_len = 0;
}

void data_log_write(const vfd_telemetry_t *t)
{
    if (!s_mounted || !t) {
        return;
    }
    if (xSemaphoreTake(s_mux, pdMS_TO_TICKS(10)) != pdTRUE) {
        return;
    }
    int n = snprintf(s_buf + s_buf_len, sizeof(s_buf) - s_buf_len,
        "%lu,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d\n",
        (unsigned long)t->uptime_ms, t->machine_state, t->alarm,
        t->main_torque_pct, t->main_torque_set,
        t->main_freq_hz, t->main_current_a, t->main_bus_v,
        t->travel_torque_pct, t->travel_torque_set,
        t->travel_freq_hz, t->travel_current_a,
        t->wire_tension_raw, t->cut_depth_mm, t->link_quality);
    if (n > 0) {
        s_buf_len += n;
    }
    /* flush ~every 1 s worth of samples (10 Hz) or when buffer is full */
    static int s_cnt;
    if (++s_cnt >= 10 || s_buf_len > (int)sizeof(s_buf) - 200) {
        s_cnt = 0;
        flush_locked();
    }
    xSemaphoreGive(s_mux);
}

int data_log_read(char *buf, int buf_len, int max_lines)
{
    if (!s_mounted || !buf || buf_len <= 0) {
        return 0;
    }
    if (xSemaphoreTake(s_mux, pdMS_TO_TICKS(100)) != pdTRUE) {
        return 0;
    }
    flush_locked();   /* ensure everything is on disk */

    int lines = 0;
    int used = 0;
    FILE *f = fopen(LOG_FILE, "r");
    if (f) {
        char line[256];
        /* skip header */
        if (!fgets(line, sizeof(line), f)) {
            line[0] = '\0';
        }
        while (lines < max_lines && fgets(line, sizeof(line), f)) {
            int l = strlen(line);
            while (l > 0 && (line[l - 1] == '\n' || line[l - 1] == '\r')) {
                line[--l] = '\0';
            }
            if (l == 0) {
                continue;
            }
            if (used + l + 1 > buf_len) {
                break;
            }
            memcpy(buf + used, line, l + 1);
            used += l + 1;
            lines++;
        }
        fclose(f);
    }
    xSemaphoreGive(s_mux);
    return lines;
}

void data_log_consume(int lines)
{
    if (!s_mounted || lines <= 0) {
        return;
    }
    if (xSemaphoreTake(s_mux, pdMS_TO_TICKS(100)) != pdTRUE) {
        return;
    }

    /* Rewrite the file without the first `lines` data lines. */
    FILE *in = fopen(LOG_FILE, "r");
    if (!in) {
        xSemaphoreGive(s_mux);
        return;
    }
    FILE *out = fopen(MOUNT_POINT "/tmp.csv", "w");
    if (!out) {
        fclose(in);
        xSemaphoreGive(s_mux);
        return;
    }
    char line[256];
    int skipped = 0;
    bool header_done = false;
    while (fgets(line, sizeof(line), in)) {
        if (!header_done) {           /* keep header */
            header_done = true;
            fputs(line, out);
            continue;
        }
        if (skipped < lines) {
            skipped++;
            continue;
        }
        fputs(line, out);
    }
    fclose(in);
    fclose(out);

    remove(LOG_FILE);
    rename(MOUNT_POINT "/tmp.csv", LOG_FILE);

    struct stat st;
    s_file_bytes = (stat(LOG_FILE, &st) == 0) ? (size_t)st.st_size : 0;
    xSemaphoreGive(s_mux);
}

int data_log_pending(void)
{
    return (int)(s_file_bytes / 60);   /* ~60 bytes per CSV row */
}
