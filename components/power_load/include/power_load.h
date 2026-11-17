/*
 * power_load - burns a steady amount of CPU on purpose so the board draws more current.
 *
 * Some USB power banks / power modules switch off when the load is too small (the board alone
 * can be below their "keep awake" current). A low-priority task per core keeps the CPU busy for
 * a fraction of every period; everything else (keypad, steno, Bluetooth) runs at a higher
 * priority and preempts it, so typing is not affected.
 *
 * This is a rough load: the real current depends on the board, the supply voltage and the
 * Bluetooth activity. Measure it with a USB power meter and adjust the duty cycle.
 */
#pragma once

#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint8_t cores;          /* 1 = core 0 only, 2 = both cores */
    uint8_t duty_percent;   /* busy share of each period per core, 1..90 (clamped) */
    uint8_t period_ms;      /* length of one busy+rest cycle in ms, 4..100 */
} power_load_config_t;

esp_err_t power_load_start(const power_load_config_t *cfg);

#ifdef __cplusplus
}
#endif
