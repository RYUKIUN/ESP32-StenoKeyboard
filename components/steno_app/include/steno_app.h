/*
 * steno_app - glue between the keypad and steno_core:
 *   key bitmap -> chord detector (stroke ends when ALL keys are released) -> queue
 *   -> steno task -> steno_engine -> output callbacks (typically hid_kbd_*).
 * The dictionary is memory-mapped from a flash partition.
 */
#pragma once

#include <stdint.h>

#include "esp_err.h"
#include "steno_core.h"

#ifdef __cplusplus
extern "C" {
#endif

#define STENO_APP_NUM_PADS 24

typedef struct {
    /* keymap[bit] = STENO_KEY_* assigned to keypad bit `bit` (STENO_KEY_NONE = ignore). */
    uint8_t keymap[STENO_APP_NUM_PADS];
    const char *dict_partition;     /* data partition holding dict.bin */
    int undo_levels;                /* how many strokes `*` can undo */
    uint32_t task_stack;            /* steno task stack bytes */
    int task_prio;
    uint32_t queue_len;             /* strokes buffered while the engine is busy */
    steno_output_t output;          /* where text / combos go */
} steno_app_config_t;

esp_err_t steno_app_start(const steno_app_config_t *cfg);

/* Call whenever the keypad bitmap changes (keypad callback). Cheap; safe from the keypad task. */
void steno_app_keys(uint32_t keys);

#ifdef __cplusplus
}
#endif
