#include <stdio.h>

#include "power_load.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "power_load";

static int64_t s_busy_us;
static TickType_t s_rest_ticks;

static void load_task(void *arg)
{
    (void)arg;
    volatile uint32_t acc = 1;      /* volatile: keeps the compiler from removing the loop */
    for (;;) {
        const int64_t end = esp_timer_get_time() + s_busy_us;
        do {
            for (int i = 0; i < 64; i++) acc = acc * 1664525u + 1013904223u;
        } while (esp_timer_get_time() < end);
        /* Always sleep at least one tick: the idle task (and its watchdog) must get some time. */
        vTaskDelay(s_rest_ticks);
    }
}

esp_err_t power_load_start(const power_load_config_t *cfg)
{
    if (!cfg || cfg->duty_percent == 0) return ESP_ERR_INVALID_ARG;

    int duty = cfg->duty_percent > 90 ? 90 : cfg->duty_percent;
    int period = cfg->period_ms < 4 ? 4 : (cfg->period_ms > 100 ? 100 : cfg->period_ms);
    s_busy_us = (int64_t)period * 1000 * duty / 100;
    s_rest_ticks = pdMS_TO_TICKS(period * (100 - duty) / 100);
    if (s_rest_ticks < 1) s_rest_ticks = 1;

    int cores = cfg->cores >= 2 ? 2 : 1;
#if CONFIG_FREERTOS_UNICORE
    cores = 1;
#endif
    for (int c = 0; c < cores; c++) {
        char name[8];
        snprintf(name, sizeof name, "load%d", c);
        BaseType_t ok = xTaskCreatePinnedToCore(load_task, name, 2048, NULL, tskIDLE_PRIORITY + 1, NULL, c);
        if (ok != pdPASS) {
            ESP_LOGE(TAG, "could not start load task on core %d", c);
            return ESP_ERR_NO_MEM;
        }
    }
    ESP_LOGI(TAG, "extra load on %d core(s): %d%% busy, %d ms period", cores, duty, period);
    return ESP_OK;
}
