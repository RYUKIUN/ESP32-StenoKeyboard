/*
 * ble_hid - BLE HID-over-GATT keyboard (NimBLE peripheral, bonding, Just Works pairing).
 * Appears to the host as an ordinary Bluetooth keyboard. Transport only: it knows nothing about steno.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    const char *device_name;        /* shown in the Bluetooth device list */
    uint16_t pnp_vendor_id;         /* Device Information PnP ID (USB-IF source) */
    uint16_t pnp_product_id;
    uint16_t pnp_version;
    uint16_t adv_itvl_min;          /* units of 0.625 ms */
    uint16_t adv_itvl_max;
    uint16_t conn_itvl_min;         /* preferred, units of 1.25 ms */
    uint16_t conn_itvl_max;
    uint16_t conn_latency;
    uint16_t conn_timeout;          /* units of 10 ms */
    bool clear_bonds_on_boot;       /* erase all stored pairings at startup */
} ble_hid_config_t;

/* NVS must already be initialised. Starts the stack and begins advertising. */
esp_err_t ble_hid_start(const ble_hid_config_t *cfg);

/* True when connected, encrypted and the host enabled input-report notifications. */
bool ble_hid_ready(void);

/* Notify one 8-byte boot-keyboard report {modifiers, 0, key1..key6}. false = not sent. */
bool ble_hid_send_report(const uint8_t report[8]);

/* Forget all bonded hosts (disconnects nothing; takes effect for the next pairing). */
void ble_hid_clear_bonds(void);

#ifdef __cplusplus
}
#endif
