#include "hid_kbd.h"

#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

static const char *TAG = "hid_kbd";

static hid_kbd_config_t s_cfg;
static QueueHandle_t s_q;

static void push(const hid_report_t *r, size_t n)
{
    if (!s_q) return;
    for (size_t i = 0; i < n; i++) {
        /* Block up to 2 s when the queue is full (host slower than we type). */
        if (xQueueSend(s_q, &r[i], pdMS_TO_TICKS(2000)) != pdTRUE) {
            ESP_LOGW(TAG, "queue full, dropping report");
            return;
        }
    }
}

static void pacing_task(void *arg)
{
    (void)arg;
    hid_report_t r;
    static const uint8_t zero[8] = { 0 };
    for (;;) {
        if (xQueueReceive(s_q, &r, portMAX_DELAY) != pdTRUE) continue;
        if (s_cfg.ready && !s_cfg.ready(s_cfg.ctx)) {
            xQueueReset(s_q);           /* nobody listening: forget the backlog */
            continue;
        }
        uint8_t rpt[8] = { r.mods, 0 };
        memcpy(&rpt[2], r.keys, 6);
        int tries = 0;
        while (!s_cfg.send(s_cfg.ctx, rpt)) {
            if (++tries > 50) {             /* ~0.5 s of retries then give up */
                ESP_LOGW(TAG, "send failed, flushing");
                xQueueReset(s_q);
                s_cfg.send(s_cfg.ctx, zero);
                break;
            }
            vTaskDelay(pdMS_TO_TICKS(10));
        }
        vTaskDelay(pdMS_TO_TICKS(s_cfg.report_interval_ms ? s_cfg.report_interval_ms : 1));
    }
}

esp_err_t hid_kbd_start(const hid_kbd_config_t *cfg)
{
    s_cfg = *cfg;
    s_q = xQueueCreate(cfg->queue_len, sizeof(hid_report_t));
    if (!s_q) return ESP_ERR_NO_MEM;
    if (xTaskCreate(pacing_task, "hid_pace", cfg->task_stack, NULL, cfg->task_prio, NULL) != pdPASS)
        return ESP_ERR_NO_MEM;
    return ESP_OK;
}

void hid_kbd_backspaces(int count)
{
    static const hid_report_t pair[2] = { { 0, { 0x2A } }, { 0, { 0 } } };
    while (count-- > 0) push(pair, 2);
}

void hid_kbd_text(const char *text, size_t len)
{
    hid_report_t buf[32];
    while (len) {
        size_t chunk = len > 16 ? 16 : len;
        size_t n = hid_text_to_reports(text, chunk, buf, 32);
        push(buf, n);
        text += chunk;
        len -= chunk;
    }
}

void hid_kbd_combo(const char *combo)
{
    hid_report_t buf[64];
    int n = hid_combo_to_reports(combo, buf, 64);
    if (n < 0) { ESP_LOGW(TAG, "bad key combo '%s'", combo); return; }
    push(buf, (size_t)n);
}

void hid_kbd_flush(void)
{
    if (s_q) xQueueReset(s_q);
    static const hid_report_t zero = { 0 };
    push(&zero, 1);
}
