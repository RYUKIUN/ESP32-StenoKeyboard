/*
 * hid_kbd - turns "type this text / press this combo / backspace N times" requests into a paced
 * stream of 8-byte HID keyboard reports. The transport (BLE HOGP, USB, ...) is a callback, so this
 * component does not depend on Bluetooth.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "hid_keys.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Send one 8-byte report {mods, 0, k0..k5}. Return true when accepted, false if the link is
 * not ready (the report is then dropped and the queue is flushed). */
typedef bool (*hid_kbd_send_fn)(void *ctx, const uint8_t report[8]);
/* Optional: true while a host is connected and ready; reports are dropped when false. */
typedef bool (*hid_kbd_ready_fn)(void *ctx);

typedef struct {
    hid_kbd_send_fn send;
    hid_kbd_ready_fn ready;     /* may be NULL */
    void *ctx;
    uint32_t report_interval_ms; /* delay after every report (pacing) */
    uint32_t queue_len;          /* number of reports that can be buffered */
    uint32_t task_stack;
    int task_prio;
} hid_kbd_config_t;

esp_err_t hid_kbd_start(const hid_kbd_config_t *cfg);

/* All three may block briefly while the queue is full. */
void hid_kbd_backspaces(int count);
void hid_kbd_text(const char *text, size_t len);
void hid_kbd_combo(const char *combo);

/* Drop everything that is queued and release all keys. */
void hid_kbd_flush(void);

#ifdef __cplusplus
}
#endif
