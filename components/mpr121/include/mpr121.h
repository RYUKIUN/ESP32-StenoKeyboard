/*
 * mpr121 - minimal reusable driver for the NXP MPR121 12-channel capacitive touch chip.
 *
 * Uses the ESP-IDF "new" I2C master driver (driver/i2c_master.h). One handle = one chip.
 * The driver can either create its own I2C bus (default) or share a bus you already own.
 *
 * This component has no dependency on any project config.h: everything is passed in
 * through mpr121_config_t.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define MPR121_NUM_ELECTRODES 12

/* Sensor tuning values. See config.h for a description of each field. */
typedef struct {
    uint8_t touch_threshold;
    uint8_t release_threshold;
    uint8_t debounce_touch;     /* 0..7 */
    uint8_t debounce_release;   /* 0..7 */
    uint8_t ffi;                /* 0..3 */
    uint8_t sfi;                /* 0..3 */
    uint8_t esi;                /* 0..7 */
    bool    autoconfig;         /* let the chip tune charge current/time */
    uint16_t vdd_mv;            /* supply voltage, used for autoconfig limits */
    uint8_t charge_current_ua;  /* 1..63, manual mode only */
    uint8_t charge_time_code;   /* 0..7, manual mode only */
    /* Baseline filter: rising / falling have 4 registers, touched has only 3 (no MHD). */
    uint8_t mhd_r, nhd_r, ncl_r, fdl_r;
    uint8_t mhd_f, nhd_f, ncl_f, fdl_f;
    uint8_t nhd_t, ncl_t, fdl_t;
} mpr121_tuning_t;

typedef struct {
    /* Bus. If `bus` is non-NULL it is used as-is (shared bus, not freed on destroy).
     * If NULL, a new bus is created from port/sda/scl and freed on destroy. */
    i2c_master_bus_handle_t bus;
    i2c_port_num_t port;
    gpio_num_t sda;
    gpio_num_t scl;
    bool internal_pullup;

    uint8_t  address;           /* 7-bit address: 0x5A..0x5D */
    uint32_t speed_hz;          /* e.g. 400000 */
    uint16_t electrode_mask;    /* which of ELE0..ELE11 are wired to a key */
    mpr121_tuning_t tuning;
} mpr121_config_t;

typedef struct mpr121_dev *mpr121_handle_t;

/* Create the device, reset the chip, apply the tuning and enter run mode.
 * Returns ESP_ERR_NOT_FOUND if no MPR121 answers on the bus. */
esp_err_t mpr121_create(const mpr121_config_t *cfg, mpr121_handle_t *out);

/* Free the device (and the bus, if this driver created it). Safe with NULL. */
void mpr121_destroy(mpr121_handle_t dev);

/* Read the touch bitmap. Bit n = electrode n touched. Already ANDed with electrode_mask. */
esp_err_t mpr121_read_touch(mpr121_handle_t dev, uint16_t *touched);

/* Read the 10-bit filtered values of all 12 electrodes (for tuning). */
esp_err_t mpr121_read_filtered(mpr121_handle_t dev, uint16_t out[MPR121_NUM_ELECTRODES]);

/* Read the baseline values of all 12 electrodes, already scaled to 10 bits (for tuning). */
esp_err_t mpr121_read_baseline(mpr121_handle_t dev, uint16_t out[MPR121_NUM_ELECTRODES]);

#ifdef __cplusplus
}
#endif