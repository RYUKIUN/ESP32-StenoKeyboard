/*
 * config.h - ALL project-specific settings live here.
 *
 * Components never include this file; main.c reads it and passes the values in.
 * Search for "CHANGE" to find the values you most likely need to edit.
 */
#pragma once

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "steno_core.h"

/* ========================================================================
 * 0. SUPPLY CURRENT BOOST  (for charger / power-bank modules that switch off at low load)
 *
 * Some modules cut the output ~30 s after power-on if the board draws less than ~50..70 mA.
 * Two cheap ways to draw more: CPU at 240 MHz (sdkconfig: CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ_240)
 * and BLE radio at maximum TX power (below). Set to 0 to use the stack's default power.
 * ====================================================================== */

#define CFG_BLE_MAX_TX_POWER  1    /* +9 dBm on advertising and connection */

/* ========================================================================
 * 1. BLUETOOTH KEYBOARD  (CHANGE: name if you like)
 * ====================================================================== */

/* Name shown in the Windows "Add a device" list. Max ~29 chars. */
#define CFG_BLE_DEVICE_NAME "Steno Keyboard"

/* Device Information PnP ID (cosmetic; Windows uses it to pick the device icon/driver). */
#define CFG_BLE_VENDOR_ID   0x303A   /* Espressif */
#define CFG_BLE_PRODUCT_ID  0x4001
#define CFG_BLE_VERSION     0x0100

/* Advertising interval in units of 0.625 ms (0x30 = 30 ms). */
#define CFG_BLE_ADV_ITVL_MIN 0x30
#define CFG_BLE_ADV_ITVL_MAX 0x50

/* Preferred connection interval in units of 1.25 ms (6 = 7.5 ms ... 12 = 15 ms). Windows decides. */
#define CFG_BLE_CONN_ITVL_MIN 6
#define CFG_BLE_CONN_ITVL_MAX 12
#define CFG_BLE_CONN_LATENCY  0
#define CFG_BLE_CONN_TIMEOUT  400    /* units of 10 ms */

/* Set to 1 for ONE boot to erase all stored Bluetooth pairings, then set back to 0 and re-flash.
 * (Normally not needed: if you remove the keyboard in Windows, the ESP32 accepts re-pairing by itself.) */
#define CFG_BLE_CLEAR_BONDS_ON_BOOT 0

/* ========================================================================
 * 1b. TYPING OUTPUT (HID)
 * ====================================================================== */

/* Pause after every HID report in ms. Each typed character = 2 reports (press + release).
 * 8 ms ~ 60 chars/s, reliable on Windows. Lower = faster typing but may drop/garble characters
 * if Windows negotiates a long connection interval. */
#define CFG_HID_REPORT_INTERVAL_MS 8
#define CFG_HID_QUEUE_LEN          512

/* ========================================================================
 * 1c. STENO ENGINE
 * ====================================================================== */

#define CFG_DICT_PARTITION   "dict"    /* must match partitions.csv */
#define CFG_STENO_UNDO_LEVELS 100      /* how many strokes `*` can undo */
#define CFG_STENO_TASK_STACK  12288
#define CFG_STENO_TASK_PRIO   5
#define CFG_STENO_QUEUE_LEN   64

/* ========================================================================
 * 2. I2C / MPR121 WIRING  (CHANGE: pins to match YOUR wiring)
 *
 * Two completely separate I2C buses, one MPR121 on each.
 * Both modules keep the default address 0x5A (ADDR pin to GND / unconnected).
 * ====================================================================== */

/* FIRST module -> I2C bus 0 */
#define CFG_PAD1_I2C_PORT   I2C_NUM_0
#define CFG_PAD1_I2C_SDA    GPIO_NUM_27
#define CFG_PAD1_I2C_SCL    GPIO_NUM_26
#define CFG_PAD1_MPR_ADDR   0x5A

/* SECOND module -> I2C bus 1 */
#define CFG_PAD2_I2C_PORT   I2C_NUM_1
#define CFG_PAD2_I2C_SDA    GPIO_NUM_19
#define CFG_PAD2_I2C_SCL    GPIO_NUM_5
#define CFG_PAD2_MPR_ADDR   0x5A

/* I2C clock. 100 kHz is the safe value (the MPR121 supports up to 400 kHz, but weak pull-ups and
 * long wires make fast clocks unreliable). */
#define CFG_I2C_SPEED_HZ    100000

/* Use the ESP32's internal pull-ups on SDA/SCL? Some keypad-style MPR121 boards have weak or NO
 * pull-ups, so this is ON. Internal ones are weak (~45k): if you still see I2C errors, add 4.7k
 * resistors from SDA and from SCL to 3V3 on each bus. Set to 0 if your board has good pull-ups. */
#define CFG_I2C_INTERNAL_PULLUP 1

/* ========================================================================
 * 3. KEY POLLING
 * ====================================================================== */

/* Wait this long after boot before the first I2C access so the MPR121 supply settles.
 * Do NOT touch the pads while it boots: the chip learns its baseline at start-up. */
#define CFG_MPR_POWERUP_DELAY_MS 500

/* How often to read both MPR121 chips, in ms. 5 ms = 200 Hz. */
#define CFG_POLL_PERIOD_MS 5

/* Which of the 12 electrodes on each module are actually wired to a key.
 * Bit n = electrode n (ELE0..ELE11). 0xFFF = all 12.
 * CHANGE: e.g. if you only wired ELE0..ELE7 on the left use 0x0FF. Unused
 * electrodes floating in the air cause phantom touches, so mask them off. */
#define CFG_PAD1_ELECTRODE_MASK   0x0FFF
#define CFG_PAD2_ELECTRODE_MASK  0x0FFF

/* ========================================================================
 * 4. MPR121 SENSOR TUNING  (fine tune here after you see real behaviour)
 *
 * Touch/release thresholds are in raw counts of (baseline - filtered).
 * Rule: touch threshold > release threshold (hysteresis). Typical start: 12 / 6.
 * Bigger touch value = needs a firmer/closer touch; smaller = more sensitive.
 * Watch the serial log "tune" output (CFG_TUNE_LOG_PERIOD_MS) to pick values.
 * ====================================================================== */

#define CFG_MPR_TOUCH_THRESHOLD    12
#define CFG_MPR_RELEASE_THRESHOLD   6

/* Debounce: number of consecutive samples (0..7) needed to register a change.
 * Each sample takes (SFI * ESI) ms, see below. 0 = off. */
#define CFG_MPR_DEBOUNCE_TOUCH      0
#define CFG_MPR_DEBOUNCE_RELEASE    0

/* Charge current for each electrode, in uA (1..63) and charge time (0..7).
 * Only used when CFG_MPR_AUTOCONFIG = 0. */
#define CFG_MPR_CHARGE_CURRENT_UA  16
#define CFG_MPR_CHARGE_TIME_CODE    1   /* 0=off,1=0.5us,2=1us,3=2us,4=4us,5=8us,6=16us,7=32us */

/* First filter iterations: 0=6, 1=10, 2=18, 3=34 samples. */
#define CFG_MPR_FFI                 0
/* Second filter iterations: 0=4, 1=6, 2=10, 3=18 samples. */
#define CFG_MPR_SFI                 0
/* Electrode sample interval: 0=1ms,1=2ms,2=4ms,3=8ms,4=16ms,5=32ms,6=64ms,7=128ms. */
#define CFG_MPR_ESI                 0

/* Auto-configuration of charge current/time per electrode (recommended). When 1, the chip
 * tunes CDC/CDT itself using the voltage values below, and the manual
 * CFG_MPR_CHARGE_* values above are ignored. */
#define CFG_MPR_AUTOCONFIG          1

/* Supply voltage of the MPR121 module in millivolts. Used to compute autoconfig limits.
 * CHANGE: 3300 if powered from the ESP32 3V3 pin (normal). Use 2800 if your module has an
 * LDO and you power it from 5V (check your module's datasheet). */
#define CFG_MPR_VDD_MV              3300

/* Baseline filter registers (leave as is unless you know the MPR121 datasheet AN3891). */
#define CFG_MPR_MHD_RISING   0x01
#define CFG_MPR_NHD_RISING   0x01
#define CFG_MPR_NCL_RISING   0x0E
#define CFG_MPR_FDL_RISING   0x00
#define CFG_MPR_MHD_FALLING  0x01
#define CFG_MPR_NHD_FALLING  0x05
#define CFG_MPR_NCL_FALLING  0x01
#define CFG_MPR_FDL_FALLING  0x00
#define CFG_MPR_NHD_TOUCHED  0x00
#define CFG_MPR_NCL_TOUCHED  0x00
#define CFG_MPR_FDL_TOUCHED  0x00

/* Periodically print raw filtered/baseline values for ELE0..ELE11 of both modules so
 * you can pick thresholds. 0 = disabled. */
#define CFG_TUNE_LOG_PERIOD_MS      0

/* ========================================================================
 * 5. PAD -> STENO KEY MAP  (CHANGE only if your electrodes are wired differently)
 *
 * keypad bit 0..11  = FIRST  module (I2C bus 0), electrode 0..11  = RIGHT-hand pad
 * keypad bit 12..23 = SECOND module (I2C bus 1), electrode 0..11  = LEFT-hand pad
 *
 * RIGHT pad (electrode numbers as seen from the user, 4 columns x 3 rows):
 *       e0=F  e1=P  e2=L  e3=T
 *       e4=R  e5=B  e6=G  e7=S
 *       e8=D  e9=Z  e10=E e11=U
 * LEFT pad (mirrored numbering):
 *       e11=S e10=T e9=P  e8=H
 *       e7=S  e6=K  e5=W  e4=R
 *       e3=#  e2=*  e1=A  e0=O
 * The two S pads on the left both give S-.
 * ====================================================================== */
#define CFG_KEYMAP { \
    /* bit  0..11 : FIRST module = RIGHT hand, electrode 0..11 */ \
    STENO_KEY_F_R, STENO_KEY_P_R, STENO_KEY_L_R, STENO_KEY_T_R, \
    STENO_KEY_R_R, STENO_KEY_B_R, STENO_KEY_G_R, STENO_KEY_S_R, \
    STENO_KEY_D_R, STENO_KEY_Z_R, STENO_KEY_E_R, STENO_KEY_U_R, \
    /* bit 12..23 : SECOND module = LEFT hand, electrode 0..11 */ \
    STENO_KEY_O_L, STENO_KEY_A_L, STENO_KEY_STAR, STENO_KEY_NUM, \
    STENO_KEY_R_L, STENO_KEY_W_L, STENO_KEY_K_L, STENO_KEY_S_L, \
    STENO_KEY_H_L, STENO_KEY_P_L, STENO_KEY_T_L, STENO_KEY_S_L }