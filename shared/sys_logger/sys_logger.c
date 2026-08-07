#include "sys_logger.h"

#include <string.h>
#include <stdio.h>
#include <stdarg.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/timers.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"

/* ------------------------------------------------------------------ */
/* Config                                                              */
/* ------------------------------------------------------------------ */
#define SYSLOG_RING_LINES   128
#define SYSLOG_LINE_MAX     160
#define SYSLOG_HEAP_PERIOD_US  (1000 * 1000)   /* 1 s */

/* ------------------------------------------------------------------ */
/* State                                                               */
/* ------------------------------------------------------------------ */
static char      s_ring[SYSLOG_RING_LINES][SYSLOG_LINE_MAX];
static int       s_head;          /* next write slot */
static int       s_count;         /* lines stored (<= RING_LINES) */
static SemaphoreHandle_t s_lock;
static esp_timer_handle_t s_heap_timer;
static vprintf_like_t s_prev_vprintf;
static bool s_inited;

static void syslog_store(const char *line)
{
    if (!s_lock) {
        return;
    }
    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(5)) != pdTRUE) {
        return;  /* never block logging */
    }
    strncpy(s_ring[s_head], line, SYSLOG_LINE_MAX - 1);
    s_ring[s_head][SYSLOG_LINE_MAX - 1] = '\0';
    s_head = (s_head + 1) % SYSLOG_RING_LINES;
    if (s_count < SYSLOG_RING_LINES) {
        s_count++;
    }
    xSemaphoreGive(s_lock);
}

/* ------------------------------------------------------------------ */
/* esp_log hook: forward to UART + capture into ring                   */
/* ------------------------------------------------------------------ */
static int syslog_vprintf(const char *fmt, va_list args)
{
    /* Print to UART first (original sink). */
    int n = 0;
    if (s_prev_vprintf) {
        va_list copy;
        va_copy(copy, args);
        n = s_prev_vprintf(fmt, copy);
        va_end(copy);
    }

    /* Capture into ring buffer. */
    char line[SYSLOG_LINE_MAX];
    va_list copy2;
    va_copy(copy2, args);
    vsnprintf(line, sizeof(line), fmt, copy2);
    va_end(copy2);
    syslog_store(line);

    return n;
}

/* ------------------------------------------------------------------ */
/* Heap monitor @1 Hz                                                  */
/* ------------------------------------------------------------------ */
static void heap_timer_cb(void *arg)
{
    char line[SYSLOG_LINE_MAX];
    uint32_t free_heap = heap_caps_get_free_size(MALLOC_CAP_DEFAULT);
    uint32_t min_heap  = heap_caps_get_minimum_free_size(MALLOC_CAP_DEFAULT);
    snprintf(line, sizeof(line),
             "I (%lu) syslog: free_heap=%lu min_heap=%lu\n",
             (unsigned long)(esp_timer_get_time() / 1000),
             (unsigned long)free_heap, (unsigned long)min_heap);
    syslog_store(line);
}

/* ------------------------------------------------------------------ */
/* Public API                                                          */
/* ------------------------------------------------------------------ */
esp_err_t sys_logger_init(void)
{
    if (s_inited) {
        return ESP_OK;
    }
    s_lock = xSemaphoreCreateMutex();
    if (!s_lock) {
        return ESP_ERR_NO_MEM;
    }
    s_head = 0;
    s_count = 0;

    s_prev_vprintf = esp_log_set_vprintf(syslog_vprintf);

    const esp_timer_create_args_t targs = {
        .callback = heap_timer_cb,
        .name = "syslog_heap",
    };
    esp_err_t err = esp_timer_create(&targs, &s_heap_timer);
    if (err == ESP_OK) {
        esp_timer_start_periodic(s_heap_timer, SYSLOG_HEAP_PERIOD_US);
    }

    s_inited = true;
    ESP_LOGI(SYS_LOG_TAG, "logger ready (ring=%d lines x %d bytes)",
             SYSLOG_RING_LINES, SYSLOG_LINE_MAX);
    return ESP_OK;
}

int sys_logger_drain(char *out, size_t out_cap, int max_lines)
{
    if (!s_inited || out == NULL || out_cap == 0) {
        return 0;
    }
    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(50)) != pdTRUE) {
        return 0;
    }

    int lines = 0;
    size_t used = 0;
    /* oldest index */
    int start = (s_head - s_count + SYSLOG_RING_LINES) % SYSLOG_RING_LINES;
    for (int i = 0; i < s_count && lines < max_lines; i++) {
        int idx = (start + i) % SYSLOG_RING_LINES;
        size_t l = strlen(s_ring[idx]);
        if (used + l + 1 > out_cap) {
            break;
        }
        memcpy(out + used, s_ring[idx], l);
        used += l;
        out[used++] = '\0';
        lines++;
    }

    /* remove drained lines */
    s_count -= lines;
    if (s_count < 0) {
        s_count = 0;
    }

    xSemaphoreGive(s_lock);
    return lines;
}

int sys_logger_pending_count(void)
{
    return s_count;
}

void sys_logger_get_mem_stat(sys_mem_stat_t *st)
{
    if (!st) {
        return;
    }
    st->free_heap     = heap_caps_get_free_size(MALLOC_CAP_DEFAULT);
    st->min_free_heap = heap_caps_get_minimum_free_size(MALLOC_CAP_DEFAULT);
#if CONFIG_SPIRAM
    st->free_spiram   = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
#else
    st->free_spiram   = 0;
#endif
}
