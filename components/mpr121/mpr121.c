#include "mpr121.h"

#include <stdlib.h>
#include <string.h>

#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "mpr121";

#define I2C_TIMEOUT_MS 50

/* Register map (datasheet rev 5) */
#define REG_TOUCH_STATUS_L   0x00
#define REG_TOUCH_STATUS_H   0x01
#define REG_FILTERED_L(n)    (0x04 + 2 * (n))
#define REG_BASELINE(n)      (0x1E + (n))
#define REG_MHD_R            0x2B
#define REG_TOUCH_TH(n)      (0x41 + 2 * (n))
#define REG_RELEASE_TH(n)    (0x42 + 2 * (n))
#define REG_DEBOUNCE         0x5B
#define REG_CONFIG1          0x5C
#define REG_CONFIG2          0x5D
#define REG_ECR              0x5E
#define REG_AUTOCONFIG0      0x7B
#define REG_AUTOCONFIG1      0x7C
#define REG_UPLIMIT          0x7D
#define REG_LOWLIMIT         0x7E
#define REG_TARGETLIMIT      0x7F
#define REG_SOFTRESET        0x80

#define SOFTRESET_VALUE      0x63
#define CONFIG2_RESET_VALUE  0x24   /* used to detect that a real MPR121 is present */
#define STATUS_H_OVCF        0x80   /* over-current flag in touch status high byte */

struct mpr121_dev {
    i2c_master_bus_handle_t bus;
    i2c_master_dev_handle_t dev;
    bool owns_bus;
    uint16_t electrode_mask;
};

static esp_err_t reg_write(mpr121_handle_t d, uint8_t reg, uint8_t val)
{
    const uint8_t buf[2] = { reg, val };
    return i2c_master_transmit(d->dev, buf, sizeof(buf), I2C_TIMEOUT_MS);
}

static esp_err_t reg_read(mpr121_handle_t d, uint8_t reg, uint8_t *data, size_t len)
{
    return i2c_master_transmit_receive(d->dev, &reg, 1, data, len, I2C_TIMEOUT_MS);
}

#define TRY(expr)                    \
    do {                             \
        esp_err_t err_ = (expr);     \
        if (err_ != ESP_OK) {        \
            return err_;             \
        }                            \
    } while (0)

static esp_err_t apply_tuning(mpr121_handle_t d, const mpr121_tuning_t *t, uint16_t electrode_mask)
{
    /* Registers can only be written while the chip is stopped (ECR = 0). */
    TRY(reg_write(d, REG_ECR, 0x00));

    /* Baseline filter: rising 0x2B..0x2E, falling 0x2F..0x32, touched 0x33..0x35 (only NHD, NCL, FDL;
     * 0x36 is reserved). The three groups are contiguous. */
    const uint8_t filt[] = {
        t->mhd_r, t->nhd_r, t->ncl_r, t->fdl_r,
        t->mhd_f, t->nhd_f, t->ncl_f, t->fdl_f,
        t->nhd_t, t->ncl_t, t->fdl_t,
    };
    for (size_t i = 0; i < sizeof(filt); i++) {
        TRY(reg_write(d, (uint8_t)(REG_MHD_R + i), filt[i]));
    }

    for (int n = 0; n < MPR121_NUM_ELECTRODES; n++) {
        TRY(reg_write(d, REG_TOUCH_TH(n), t->touch_threshold));
        TRY(reg_write(d, REG_RELEASE_TH(n), t->release_threshold));
    }

    TRY(reg_write(d, REG_DEBOUNCE,
                  (uint8_t)(((t->debounce_release & 0x07) << 4) | (t->debounce_touch & 0x07))));

    if (t->autoconfig) {
        /* Start from the datasheet-typical values (CDC = 16 uA, CDT = 0.5 us) and let the chip tune
         * every electrode itself. Starting from 0 can make autoconfig misbehave. */
        TRY(reg_write(d, REG_CONFIG1, 0x10));
        TRY(reg_write(d, REG_CONFIG2, 0x20));
    } else {
        uint8_t cdc = (uint8_t)(t->charge_current_ua & 0x3F);
        uint8_t cdt = (uint8_t)(t->charge_time_code & 0x07);
        TRY(reg_write(d, REG_CONFIG1, (uint8_t)(((t->ffi & 0x03) << 6) | cdc)));
        TRY(reg_write(d, REG_CONFIG2,
                      (uint8_t)((cdt << 5) | ((t->sfi & 0x03) << 3) | (t->esi & 0x07))));
    }

    if (t->autoconfig) {
        /* Limits per AN3889: USL = (Vdd - 0.7) / Vdd * 256, TL = 0.9 * USL, LSL = 0.65 * USL */
        uint32_t vdd = t->vdd_mv ? t->vdd_mv : 3300;
        uint32_t usl = ((vdd - 700u) * 256u) / vdd;
        if (usl > 255u) {
            usl = 255u;
        }
        uint32_t tl  = (usl * 90u) / 100u;
        uint32_t lsl = (usl * 65u) / 100u;
        /* AFES follows FFI, BVA=10 (same as CL), ARE=1, ACE=1 */
        TRY(reg_write(d, REG_AUTOCONFIG0, (uint8_t)(((t->ffi & 0x03) << 6) | 0x0B)));
        TRY(reg_write(d, REG_AUTOCONFIG1, 0x00));
        TRY(reg_write(d, REG_UPLIMIT, (uint8_t)usl));
        TRY(reg_write(d, REG_LOWLIMIT, (uint8_t)lsl));
        TRY(reg_write(d, REG_TARGETLIMIT, (uint8_t)tl));
    } else {
        TRY(reg_write(d, REG_AUTOCONFIG0, 0x00));
    }

    /* Run mode: CL = 0b10 (baseline initialised from first reading), enable electrodes
     * ELE0..ELE(highest used). ELE_EN counts from ELE0 so enable up to the highest masked one. */
    uint8_t count = 0;
    for (int n = 0; n < MPR121_NUM_ELECTRODES; n++) {
        if (electrode_mask & (1u << n)) {
            count = (uint8_t)(n + 1);
        }
    }
    if (count == 0) {
        ESP_LOGW(TAG, "electrode_mask is 0, chip stays in stop mode");
        return ESP_OK;
    }
    TRY(reg_write(d, REG_ECR, (uint8_t)(0x80 | count)));
    return ESP_OK;
}

/* A slave that was reset in the middle of a transfer may hold SDA low forever. Clock SCL 9 times and
 * generate a STOP (bit-banged, open-drain) to release it before the I2C driver takes the pins. */
static void bus_recover(gpio_num_t sda, gpio_num_t scl, bool pullup)
{
    const gpio_config_t io = {
        .pin_bit_mask = (1ULL << sda) | (1ULL << scl),
        .mode = GPIO_MODE_INPUT_OUTPUT_OD,
        .pull_up_en = pullup ? GPIO_PULLUP_ENABLE : GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&io);
    gpio_set_level(sda, 1);
    gpio_set_level(scl, 1);
    esp_rom_delay_us(10);
    const int sda_before = gpio_get_level(sda);
    for (int i = 0; i < 9; i++) {
        gpio_set_level(scl, 0);
        esp_rom_delay_us(5);
        gpio_set_level(scl, 1);
        esp_rom_delay_us(5);
    }
    /* STOP: SDA low -> high while SCL is high */
    gpio_set_level(sda, 0);
    esp_rom_delay_us(5);
    gpio_set_level(scl, 1);
    esp_rom_delay_us(5);
    gpio_set_level(sda, 1);
    esp_rom_delay_us(5);
    if (!sda_before) {
        ESP_LOGW(TAG, "SDA (GPIO%d) was held low before recovery - a slave was stuck or SDA has no pull-up",
                 (int)sda);
    }
    gpio_reset_pin(sda);
    gpio_reset_pin(scl);
}

esp_err_t mpr121_create(const mpr121_config_t *cfg, mpr121_handle_t *out)
{
    if (cfg == NULL || out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    *out = NULL;

    mpr121_handle_t d = calloc(1, sizeof(*d));
    if (d == NULL) {
        return ESP_ERR_NO_MEM;
    }
    d->electrode_mask = cfg->electrode_mask & 0x0FFF;

    esp_err_t err;
    if (cfg->bus != NULL) {
        d->bus = cfg->bus;
        d->owns_bus = false;
    } else {
        bus_recover(cfg->sda, cfg->scl, cfg->internal_pullup);
        const i2c_master_bus_config_t bus_cfg = {
            .i2c_port = cfg->port,
            .sda_io_num = cfg->sda,
            .scl_io_num = cfg->scl,
            .clk_source = I2C_CLK_SRC_DEFAULT,
            .glitch_ignore_cnt = 7,
            .flags.enable_internal_pullup = cfg->internal_pullup,
        };
        err = i2c_new_master_bus(&bus_cfg, &d->bus);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "i2c_new_master_bus failed: %s", esp_err_to_name(err));
            free(d);
            return err;
        }
        d->owns_bus = true;
    }

    const i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = cfg->address,
        .scl_speed_hz = cfg->speed_hz,
    };
    err = i2c_master_bus_add_device(d->bus, &dev_cfg, &d->dev);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "add_device 0x%02x failed: %s", cfg->address, esp_err_to_name(err));
        goto fail;
    }

    /* Soft reset, then check that CONFIG2 has its documented reset value. */
    err = reg_write(d, REG_SOFTRESET, SOFTRESET_VALUE);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "no ACK from 0x%02x (SDA=%d SCL=%d): %s. Check wiring/power/address.",
                 cfg->address, (int)cfg->sda, (int)cfg->scl, esp_err_to_name(err));
        err = ESP_ERR_NOT_FOUND;
        goto fail;
    }
    vTaskDelay(pdMS_TO_TICKS(10));
    /* Make sure the chip is in stop mode before touching any other register. */
    err = reg_write(d, REG_ECR, 0x00);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "0x%02x: ECR=0 after reset failed: %s", cfg->address, esp_err_to_name(err));
        goto fail;
    }

    uint8_t cfg2 = 0;
    err = reg_read(d, REG_CONFIG2, &cfg2, 1);
    if (err != ESP_OK || cfg2 != CONFIG2_RESET_VALUE) {
        ESP_LOGE(TAG, "0x%02x answered but does not look like an MPR121 (CONFIG2=0x%02x)",
                 cfg->address, cfg2);
        err = ESP_ERR_NOT_FOUND;
        goto fail;
    }

    err = apply_tuning(d, &cfg->tuning, d->electrode_mask);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "tuning write failed: %s", esp_err_to_name(err));
        goto fail;
    }

    /* Read back ECR / CONFIG1 to prove the chip really entered run mode with our settings. */
    vTaskDelay(pdMS_TO_TICKS(10));
    uint8_t ecr = 0, c1 = 0;
    uint8_t want_ecr = 0;
    if (d->electrode_mask) {
        uint8_t count = 0;
        for (int n = 0; n < MPR121_NUM_ELECTRODES; n++) {
            if (d->electrode_mask & (1u << n)) count = (uint8_t)(n + 1);
        }
        want_ecr = (uint8_t)(0x80 | count);
    }
    err = reg_read(d, REG_ECR, &ecr, 1);
    if (err == ESP_OK) err = reg_read(d, REG_CONFIG1, &c1, 1);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "0x%02x: read-back failed: %s", cfg->address, esp_err_to_name(err));
        goto fail;
    }
    if (ecr != want_ecr) {
        ESP_LOGE(TAG, "0x%02x: ECR=0x%02x, expected 0x%02x (chip did not enter run mode)", cfg->address, ecr,
                 want_ecr);
        err = ESP_FAIL;
        goto fail;
    }
    ESP_LOGI(TAG, "MPR121 ready at 0x%02x (SDA=%d SCL=%d) ECR=0x%02x CONFIG1=0x%02x", cfg->address,
             (int)cfg->sda, (int)cfg->scl, ecr, c1);
    *out = d;
    return ESP_OK;

fail:
    mpr121_destroy(d);
    return err;
}

void mpr121_destroy(mpr121_handle_t d)
{
    if (d == NULL) {
        return;
    }
    if (d->dev != NULL) {
        i2c_master_bus_rm_device(d->dev);
    }
    if (d->owns_bus && d->bus != NULL) {
        i2c_del_master_bus(d->bus);
    }
    free(d);
}

esp_err_t mpr121_read_touch(mpr121_handle_t d, uint16_t *touched)
{
    if (d == NULL || touched == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    uint8_t b[2];
    esp_err_t err = reg_read(d, REG_TOUCH_STATUS_L, b, sizeof(b));
    if (err != ESP_OK) {
        return err;
    }
    if (b[1] & STATUS_H_OVCF) {
        ESP_LOGW(TAG, "over-current flag set (an electrode may be shorted), clearing");
        (void)reg_write(d, REG_TOUCH_STATUS_H, STATUS_H_OVCF);
    }
    uint16_t raw = (uint16_t)(b[0] | ((b[1] & 0x0F) << 8));
    *touched = raw & d->electrode_mask;
    return ESP_OK;
}

esp_err_t mpr121_read_filtered(mpr121_handle_t d, uint16_t out[MPR121_NUM_ELECTRODES])
{
    if (d == NULL || out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    uint8_t b[2 * MPR121_NUM_ELECTRODES];
    esp_err_t err = reg_read(d, REG_FILTERED_L(0), b, sizeof(b));
    if (err != ESP_OK) {
        return err;
    }
    for (int n = 0; n < MPR121_NUM_ELECTRODES; n++) {
        out[n] = (uint16_t)((b[2 * n] | (b[2 * n + 1] << 8)) & 0x03FF);
    }
    return ESP_OK;
}

esp_err_t mpr121_read_baseline(mpr121_handle_t d, uint16_t out[MPR121_NUM_ELECTRODES])
{
    if (d == NULL || out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    uint8_t b[MPR121_NUM_ELECTRODES];
    esp_err_t err = reg_read(d, REG_BASELINE(0), b, sizeof(b));
    if (err != ESP_OK) {
        return err;
    }
    for (int n = 0; n < MPR121_NUM_ELECTRODES; n++) {
        out[n] = (uint16_t)(b[n] << 2);
    }
    return ESP_OK;
}