/*
 * hid_keys - portable (no ESP-IDF) conversion of text and Plover key-combo strings into
 * USB/BLE HID boot-keyboard reports (US layout).
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint8_t mods;      /* bit0 LCtrl bit1 LShift bit2 LAlt bit3 LGui bit4 RCtrl bit5 RShift bit6 RAlt bit7 RGui */
    uint8_t keys[6];   /* usage ids, 0 = empty */
} hid_report_t;

/* US-layout lookup for one ASCII char. Returns false if the char has no key. */
bool hid_ascii(char c, uint8_t *mods, uint8_t *usage);

/* Append the reports that type `text` (press + release per char). Non-ASCII bytes are skipped.
 * Returns number of reports written (never more than `max`). */
size_t hid_text_to_reports(const char *text, size_t len, hid_report_t *out, size_t max);

/* Expand a Plover key combo, e.g. "Control_L(c)", "Alt_L(Tab Tab)", "BackSpace".
 * Returns number of reports written, or -1 on a syntax error / unknown key name. */
int hid_combo_to_reports(const char *combo, hid_report_t *out, size_t max);

#ifdef __cplusplus
}
#endif
