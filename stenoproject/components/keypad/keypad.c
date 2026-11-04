#include "keypad.h"

#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "keypad";

#define KEYPAD_TASK_STACK 4096
#define KEYPAD_TASK_PRIO  (configMAX_PRIORITIES - 3)
#define ERROR_LOG_EVERY_MS 1000

typedef struct {
    mpr121_handle_t dev;
    const char *name;
    int shift;
    uint16_t last;
    int64_t last_err_log_us;
    uint32_t err_count;
} half_t;

static half_t s_half[2];
static keypad_config_t s_cfg;
static volatile uint32_t s_keys;
static bool s_started;

static void log_tune(half_t *h)
{
    uint16_t f[MPR121_NUM_ELECTRODES];
    uint16_t b[MPR121_NUM_ELECTRODES];
    if (h->dev == NULL || mpr121_read_filtered(h->dev, f) != ESP_OK ||
        mpr121_read_baseline(h->dev, b) != ESP_OK) {
        return;
    }
    char line[MPR121_NUM_ELECTRODES * 12 + 1];
    size_t pos = 0;
    for (int n = 0; n < MPR121_NUM_ELECTRODES && pos < sizeof(line); n++) {
        pos += (size_t)snprintf(line + pos, sizeof(line) - pos, "%3d:%3d/%3d ", n, (int)f[n],
                                (int)b[n]);
    }
    ESP_LOGI(TAG, "tune %s  ele:filtered/baseline  %s", h->name, line);
}

static void keypad_task(void *arg)
{
    (void)arg;
    TickType_t last_wake = xTaskGetTickCount();
    const TickType_t period = pdMS_TO_TICKS(s_cfg.poll_period_ms) ? pdMS_TO_TICKS(s_cfg.poll_period_ms) : 1;
    int64_t next_tune_us = 0;

    for (;;) {
        uint32_t keys = 0;

        for (int i = 0; i < 2; i++) {
            half_t *h = &s_half[i];
            if (h->dev == NULL) {
                continue;
            }
            uint16_t t = 0;
            esp_err_t err = mpr121_read_touch(h->dev, &t);
            if (err == ESP_OK) {
                h->last = t;
            } else {
                /* Keep the last known state, but tell the user (rate limited). */
                h->err_count++;
                int64_t now = esp_timer_get_time();
                if (now - h->last_err_log_us > (int64_t)ERROR_LOG_EVERY_MS * 1000) {
                    h->last_err_log_us = now;
                    ESP_LOGW(TAG, "%s read error: %s (total %u)", h->name, esp_err_to_name(err),
                             (unsigned)h->err_count);
                }
            }
            keys |= ((uint32_t)h->last) << h->shift;
        }

        uint32_t changed = keys ^ s_keys;
        if (changed) {
            s_keys = keys;
            if (s_cfg.on_event != NULL) {
                s_cfg.on_event(keys, changed, s_cfg.on_event_ctx);
            }
        }

        if (s_cfg.tune_log_period_ms) {
            int64_t now = esp_timer_get_time();
            if (now >= next_tune_us) {
                next_tune_us = now + (int64_t)s_cfg.tune_log_period_ms * 1000;
                log_tune(&s_half[0]);
                log_tune(&s_half[1]);
            }
        }

        vTaskDelayUntil(&last_wake, period);
    }
}

esp_err_t keypad_start(const keypad_config_t *cfg)
{
    if (cfg == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_started) {
        return ESP_ERR_INVALID_STATE;
    }
    s_cfg = *cfg;
    memset(s_half, 0, sizeof(s_half));
    s_half[0].name = "left";
    s_half[0].shift = KEYPAD_LEFT_SHIFT;
    s_half[1].name = "right";
    s_half[1].shift = KEYPAD_RIGHT_SHIFT;

    if (s_cfg.powerup_delay_ms) {
        ESP_LOGI(TAG, "waiting %u ms for MPR121 power-up (don't touch the pads)", (unsigned)s_cfg.powerup_delay_ms);
        vTaskDelay(pdMS_TO_TICKS(s_cfg.powerup_delay_ms));
    }

    int found = 0;
    const mpr121_config_t *cfgs[2] = { &s_cfg.left, &s_cfg.right };
    for (int i = 0; i < 2; i++) {
        esp_err_t err = mpr121_create(cfgs[i], &s_half[i].dev);
        if (err == ESP_OK) {
            found++;
        } else {
            ESP_LOGW(TAG, "%s MPR121 init failed (%s): that half will read as released",
                     s_half[i].name, esp_err_to_name(err));
            s_half[i].dev = NULL;
        }
    }
    if (found == 0) {
        ESP_LOGE(TAG, "no MPR121 found on either bus");
        return ESP_ERR_NOT_FOUND;
    }

    BaseType_t ok = xTaskCreate(keypad_task, "keypad", KEYPAD_TASK_STACK, NULL, KEYPAD_TASK_PRIO, NULL);
    if (ok != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    s_started = true;
    return ESP_OK;
}

uint32_t keypad_get_keys(void)
{
    return s_keys;
}