/*
 * Stenokeyboard: 2x MPR121 pads -> chord detector -> Plover-compatible steno engine (in the ESP32)
 * -> BLE HID keyboard. The PC only sees an ordinary Bluetooth keyboard.
 *
 * All settings are in config.h.
 */
#include <stdint.h>
#include <string.h>

#include "ble_hid.h"
#include "config.h"
#include "esp_log.h"
#include "hid_kbd.h"
#include "keypad.h"
#include "nvs_flash.h"
#include "steno_app.h"

static const char *TAG = "main";

/* ---- steno output -> HID ---- */
static void out_backspaces(void *ctx, int n) { (void)ctx; hid_kbd_backspaces(n); }
static void out_string(void *ctx, const char *t, size_t len) { (void)ctx; hid_kbd_text(t, len); }
static void out_combo(void *ctx, const char *c) { (void)ctx; hid_kbd_combo(c); }
static void out_enabled(void *ctx, bool on)
{
    (void)ctx;
    ESP_LOGI(TAG, "steno output %s", on ? "ON" : "OFF (suspended)");
}

static bool hid_send(void *ctx, const uint8_t rpt[8]) { (void)ctx; return ble_hid_send_report(rpt); }
static bool hid_ready(void *ctx) { (void)ctx; return ble_hid_ready(); }

static void on_keys(uint32_t keys, uint32_t changed, void *ctx)
{
    (void)changed; (void)ctx;
    steno_app_keys(keys);
}

static mpr121_tuning_t make_tuning(void)
{
    const mpr121_tuning_t t = {
        .touch_threshold = CFG_MPR_TOUCH_THRESHOLD,
        .release_threshold = CFG_MPR_RELEASE_THRESHOLD,
        .debounce_touch = CFG_MPR_DEBOUNCE_TOUCH,
        .debounce_release = CFG_MPR_DEBOUNCE_RELEASE,
        .ffi = CFG_MPR_FFI,
        .sfi = CFG_MPR_SFI,
        .esi = CFG_MPR_ESI,
        .autoconfig = CFG_MPR_AUTOCONFIG,
        .vdd_mv = CFG_MPR_VDD_MV,
        .charge_current_ua = CFG_MPR_CHARGE_CURRENT_UA,
        .charge_time_code = CFG_MPR_CHARGE_TIME_CODE,
        .mhd_r = CFG_MPR_MHD_RISING,  .nhd_r = CFG_MPR_NHD_RISING,
        .ncl_r = CFG_MPR_NCL_RISING,  .fdl_r = CFG_MPR_FDL_RISING,
        .mhd_f = CFG_MPR_MHD_FALLING, .nhd_f = CFG_MPR_NHD_FALLING,
        .ncl_f = CFG_MPR_NCL_FALLING, .fdl_f = CFG_MPR_FDL_FALLING,
        .nhd_t = CFG_MPR_NHD_TOUCHED, .ncl_t = CFG_MPR_NCL_TOUCHED,
        .fdl_t = CFG_MPR_FDL_TOUCHED,
    };
    return t;
}

void app_main(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    /* --- Bluetooth HID keyboard --- */
    const ble_hid_config_t ble_cfg = {
        .device_name = CFG_BLE_DEVICE_NAME,
        .pnp_vendor_id = CFG_BLE_VENDOR_ID,
        .pnp_product_id = CFG_BLE_PRODUCT_ID,
        .pnp_version = CFG_BLE_VERSION,
        .adv_itvl_min = CFG_BLE_ADV_ITVL_MIN,
        .adv_itvl_max = CFG_BLE_ADV_ITVL_MAX,
        .conn_itvl_min = CFG_BLE_CONN_ITVL_MIN,
        .conn_itvl_max = CFG_BLE_CONN_ITVL_MAX,
        .conn_latency = CFG_BLE_CONN_LATENCY,
        .conn_timeout = CFG_BLE_CONN_TIMEOUT,
        .clear_bonds_on_boot = CFG_BLE_CLEAR_BONDS_ON_BOOT,
    };
    ESP_ERROR_CHECK(ble_hid_start(&ble_cfg));

    /* --- HID report queue --- */
    const hid_kbd_config_t hid_cfg = {
        .send = hid_send,
        .ready = hid_ready,
        .ctx = NULL,
        .report_interval_ms = CFG_HID_REPORT_INTERVAL_MS,
        .queue_len = CFG_HID_QUEUE_LEN,
        .task_stack = 3072,
        .task_prio = 6,
    };
    ESP_ERROR_CHECK(hid_kbd_start(&hid_cfg));

    /* --- Steno engine (dictionary is read from the "dict" partition) --- */
    steno_app_config_t sa = {
        .keymap = CFG_KEYMAP,
        .dict_partition = CFG_DICT_PARTITION,
        .undo_levels = CFG_STENO_UNDO_LEVELS,
        .task_stack = CFG_STENO_TASK_STACK,
        .task_prio = CFG_STENO_TASK_PRIO,
        .queue_len = CFG_STENO_QUEUE_LEN,
        .output = { out_backspaces, out_string, out_combo, out_enabled, NULL },
    };
    err = steno_app_start(&sa);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "steno_app_start failed: %s. Bluetooth stays up but nothing will be typed.", esp_err_to_name(err));
    }

    /* --- Keypad (two MPR121 on two separate I2C buses) --- */
    const keypad_config_t kp_cfg = {
        .left = {       /* keypad bits 0..11  = FIRST module  = RIGHT-hand pad */
            .bus = NULL,
            .port = CFG_PAD1_I2C_PORT, .sda = CFG_PAD1_I2C_SDA, .scl = CFG_PAD1_I2C_SCL,
            .internal_pullup = CFG_I2C_INTERNAL_PULLUP, .address = CFG_PAD1_MPR_ADDR,
            .speed_hz = CFG_I2C_SPEED_HZ, .electrode_mask = CFG_PAD1_ELECTRODE_MASK,
            .tuning = make_tuning(),
        },
        .right = {      /* keypad bits 12..23 = SECOND module = LEFT-hand pad */
            .bus = NULL,
            .port = CFG_PAD2_I2C_PORT, .sda = CFG_PAD2_I2C_SDA, .scl = CFG_PAD2_I2C_SCL,
            .internal_pullup = CFG_I2C_INTERNAL_PULLUP, .address = CFG_PAD2_MPR_ADDR,
            .speed_hz = CFG_I2C_SPEED_HZ, .electrode_mask = CFG_PAD2_ELECTRODE_MASK,
            .tuning = make_tuning(),
        },
        .powerup_delay_ms = CFG_MPR_POWERUP_DELAY_MS,
        .poll_period_ms = CFG_POLL_PERIOD_MS,
        .tune_log_period_ms = CFG_TUNE_LOG_PERIOD_MS,
        .on_event = on_keys,
        .on_event_ctx = NULL,
    };
    err = keypad_start(&kp_cfg);
    if (err != ESP_OK) ESP_LOGE(TAG, "keypad_start failed: %s", esp_err_to_name(err));
}