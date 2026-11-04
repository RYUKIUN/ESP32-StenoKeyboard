/*
 * keypad - polls two MPR121 chips and merges them into one 24-bit key bitmap.
 *
 *   bit  0..11  -> left  module, electrode 0..11
 *   bit 12..23  -> right module, electrode 0..11
 *
 * A callback is invoked (from the keypad task) only when the bitmap changes.
 * This component knows nothing about Bluetooth; wire the callback to whatever transport you like.
 */
#pragma once

#include <stdint.h>

#include "esp_err.h"
#include "mpr121.h"

#ifdef __cplusplus
extern "C" {
#endif

#define KEYPAD_LEFT_SHIFT   0
#define KEYPAD_RIGHT_SHIFT  12
#define KEYPAD_NUM_KEYS     24

/* keys    = full current bitmap
 * changed = bits that differ from the previous call (1 = changed) */
typedef void (*keypad_event_cb_t)(uint32_t keys, uint32_t changed, void *ctx);

typedef struct {
    mpr121_config_t left;
    mpr121_config_t right;
    uint32_t powerup_delay_ms;     /* wait before the first I2C access so MPR121 supply settles */
    uint32_t poll_period_ms;       /* time between scans */
    uint32_t tune_log_period_ms;   /* 0 = off. Prints filtered/baseline values for tuning */
    keypad_event_cb_t on_event;    /* may be NULL */
    void *on_event_ctx;
} keypad_config_t;

/* Initialise both MPR121 chips and start the polling task.
 * If only one chip is found the keypad still runs with the other half held released,
 * and ESP_OK is returned (a warning is logged). If neither is found, ESP_ERR_NOT_FOUND. */
esp_err_t keypad_start(const keypad_config_t *cfg);

/* Latest merged bitmap. */
uint32_t keypad_get_keys(void);

#ifdef __cplusplus
}
#endif