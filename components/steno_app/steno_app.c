#include "steno_app.h"

#include <string.h>

#include "esp_system.h"
#include "esp_log.h"
#include "esp_partition.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

static const char *TAG = "steno_app";

static steno_app_config_t s_cfg;
static QueueHandle_t s_q;
static steno_engine_t *s_engine;
static const uint8_t *s_map;     /* mmap'ed dictionary */
static steno_stroke_t s_chord;   /* keys seen since the last full release */

static const uint8_t *blob_read(void *ctx, uint32_t off, uint32_t len)
{
    (void)ctx; (void)len;
    return s_map + off;
}

static void steno_task(void *arg)
{
    (void)arg;
    steno_stroke_t s;
    for (;;) {
        if (xQueueReceive(s_q, &s, portMAX_DELAY) != pdTRUE) continue;
        steno_engine_stroke(s_engine, s);
    }
}

void steno_app_keys(uint32_t keys)
{
    if (!s_q) return;
    steno_stroke_t down = 0;
    for (int bit = 0; bit < STENO_APP_NUM_PADS; bit++) {
        if (!(keys & (1u << bit))) continue;
        uint8_t k = s_cfg.keymap[bit];
        if (k != STENO_KEY_NONE) down |= STENO_MASK(k);
    }
    s_chord |= down;
    if (keys == 0 && s_chord) {              /* everything released: the stroke is complete */
        steno_stroke_t done = s_chord;
        s_chord = 0;
        if (xQueueSend(s_q, &done, 0) != pdTRUE) ESP_LOGW(TAG, "stroke queue full, dropped");
    }
}

esp_err_t steno_app_start(const steno_app_config_t *cfg)
{
    s_cfg = *cfg;

    const esp_partition_t *part = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY,
                                                           cfg->dict_partition);
    if (!part) {
        ESP_LOGE(TAG, "partition '%s' not found - check partitions.csv", cfg->dict_partition);
        return ESP_ERR_NOT_FOUND;
    }
    esp_partition_mmap_handle_t h;
    const void *ptr = NULL;
    esp_err_t err = esp_partition_mmap(part, 0, part->size, ESP_PARTITION_MMAP_DATA, &ptr, &h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "mmap of dictionary failed: %s", esp_err_to_name(err));
        return err;
    }
    s_map = ptr;

    steno_blob_t blob = { .read = blob_read, .ctx = NULL, .size = part->size };
    steno_dict_t *dict = steno_dict_open(&blob);
    if (!dict) {
        ESP_LOGE(TAG, "dictionary partition is empty or invalid - flash dict/dict.bin to '%s'", cfg->dict_partition);
        return ESP_ERR_INVALID_STATE;
    }
    ESP_LOGI(TAG, "dictionary: %u entries, longest key %d strokes", (unsigned)steno_dict_entry_count(dict),
             steno_dict_longest_key(dict));

    s_engine = steno_engine_new(dict, &cfg->output, cfg->undo_levels);
    if (!s_engine) return ESP_ERR_NO_MEM;

    s_q = xQueueCreate(cfg->queue_len, sizeof(steno_stroke_t));
    if (!s_q) return ESP_ERR_NO_MEM;
    if (xTaskCreate(steno_task, "steno", cfg->task_stack, NULL, cfg->task_prio, NULL) != pdPASS)
        return ESP_ERR_NO_MEM;
    ESP_LOGI(TAG, "steno ready, free heap %u", (unsigned)esp_get_free_heap_size());
    return ESP_OK;
}
