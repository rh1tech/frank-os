/*
 * FRANK OS — DS3231MZ Real-Time Clock Driver
 * Copyright (c) 2026 Mikhail Matveev <xtreme@rh1.tech>
 * https://rh1.tech
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <stdio.h>
#include <string.h>

#include "pico/stdlib.h"
#include "pico/time.h"
#include "hardware/gpio.h"

#include "FreeRTOS.h"
#include "task.h"
#include "timers.h"
#include "semphr.h"

#include "board_config.h"
#include "settings.h"
#include "rtc.h"

/* DS3231 register map */
#define DS3231_REG_SECONDS  0x00
#define DS3231_REG_CONTROL  0x0E
#define DS3231_REG_STATUS   0x0F
#define DS3231_STATUS_OSF   0x80   /* oscillator-stop flag */
#define DS3231_CTRL_EOSC    0x80   /* enable oscillator (active low) */

/* Trigger a taskbar repaint when the displayed minute changes. */
extern void taskbar_invalidate(void);

/* ------------------------------------------------------------------------- */
/* Bus state                                                                 */
/* ------------------------------------------------------------------------- */

static uint8_t scl_pin = RTC_PIN_SCL;
static uint8_t sda_pin = RTC_PIN_SDA;
static bool    pins_ready;

static SemaphoreHandle_t bus_mutex;

/* Cached time/state for the taskbar (no I2C). */
static volatile bool    cached_present;
static volatile bool    cached_set;
static volatile uint8_t cached_h, cached_m, cached_s;
static uint8_t          last_min = 0xFF;

/* ------------------------------------------------------------------------- */
/* Bit-banged open-drain I2C                                                 */
/* ------------------------------------------------------------------------- */

static inline void i2c_delay(void) { busy_wait_us_32(4); }   /* ~100 kHz */

static inline void sda_release(void) { gpio_set_dir(sda_pin, GPIO_IN); }
static inline void sda_drive0(void)  { gpio_set_dir(sda_pin, GPIO_OUT); }
static inline void scl_release(void) { gpio_set_dir(scl_pin, GPIO_IN); }
static inline void scl_drive0(void)  { gpio_set_dir(scl_pin, GPIO_OUT); }
static inline bool sda_read(void)    { return gpio_get(sda_pin); }

/* Release SCL and wait for it to actually rise (clock stretching). */
static void scl_release_wait(void) {
    scl_release();
    for (int i = 0; i < 1000 && !gpio_get(scl_pin); i++)
        busy_wait_us_32(1);
}

static void i2c_pin_init(uint8_t pin) {
    gpio_init(pin);
    gpio_put(pin, 0);            /* preset latch low: OUT == drive 0 */
    gpio_set_dir(pin, GPIO_IN); /* released (input) by default */
    gpio_pull_up(pin);
}

static void i2c_bus_init(void) {
    i2c_pin_init(scl_pin);
    i2c_pin_init(sda_pin);
    /* Idle: both released high. */
    sda_release();
    scl_release();
    pins_ready = true;
}

static void i2c_start(void) {
    sda_release();
    scl_release_wait();
    i2c_delay();
    sda_drive0();
    i2c_delay();
    scl_drive0();
    i2c_delay();
}

static void i2c_stop(void) {
    sda_drive0();
    i2c_delay();
    scl_release_wait();
    i2c_delay();
    sda_release();
    i2c_delay();
}

static void i2c_write_bit(bool b) {
    if (b) sda_release();
    else   sda_drive0();
    i2c_delay();
    scl_release_wait();
    i2c_delay();
    scl_drive0();
    i2c_delay();
}

static bool i2c_read_bit(void) {
    sda_release();
    i2c_delay();
    scl_release_wait();
    i2c_delay();
    bool b = sda_read();
    scl_drive0();
    i2c_delay();
    return b;
}

/* Returns true if the slave ACKed. */
static bool i2c_write_byte(uint8_t v) {
    for (int i = 0; i < 8; i++) {
        i2c_write_bit((v & 0x80) != 0);
        v <<= 1;
    }
    return !i2c_read_bit();   /* ACK = SDA low */
}

static uint8_t i2c_read_byte(bool ack) {
    uint8_t v = 0;
    for (int i = 0; i < 8; i++)
        v = (v << 1) | (i2c_read_bit() ? 1 : 0);
    i2c_write_bit(!ack);      /* ACK = drive low */
    return v;
}

/* ------------------------------------------------------------------------- */
/* DS3231 register access (no locking — callers hold bus_mutex)              */
/* ------------------------------------------------------------------------- */

static bool ds_probe(void) {
    if (!pins_ready) return false;
    i2c_start();
    bool ack = i2c_write_byte((RTC_I2C_ADDR << 1) | 0);
    i2c_stop();
    return ack;
}

static bool ds_read(uint8_t reg, uint8_t *buf, int n) {
    if (!pins_ready) return false;
    i2c_start();
    if (!i2c_write_byte((RTC_I2C_ADDR << 1) | 0)) { i2c_stop(); return false; }
    if (!i2c_write_byte(reg))                     { i2c_stop(); return false; }
    i2c_start();   /* repeated start */
    if (!i2c_write_byte((RTC_I2C_ADDR << 1) | 1)) { i2c_stop(); return false; }
    for (int i = 0; i < n; i++)
        buf[i] = i2c_read_byte(i < n - 1);   /* ACK all but the last byte */
    i2c_stop();
    return true;
}

static bool ds_write(uint8_t reg, const uint8_t *buf, int n) {
    if (!pins_ready) return false;
    i2c_start();
    if (!i2c_write_byte((RTC_I2C_ADDR << 1) | 0)) { i2c_stop(); return false; }
    if (!i2c_write_byte(reg))                     { i2c_stop(); return false; }
    for (int i = 0; i < n; i++)
        if (!i2c_write_byte(buf[i]))              { i2c_stop(); return false; }
    i2c_stop();
    return true;
}

/* ------------------------------------------------------------------------- */
/* BCD helpers                                                               */
/* ------------------------------------------------------------------------- */

static inline uint8_t bcd2bin(uint8_t v) { return (v >> 4) * 10 + (v & 0x0F); }
static inline uint8_t bin2bcd(uint8_t v) { return ((v / 10) << 4) | (v % 10); }

/* ------------------------------------------------------------------------- */
/* Locking helpers                                                           */
/* ------------------------------------------------------------------------- */

static bool bus_lock(TickType_t to) {
    if (!bus_mutex) return false;
    return xSemaphoreTake(bus_mutex, to) == pdTRUE;
}
static void bus_unlock(void) {
    if (bus_mutex) xSemaphoreGive(bus_mutex);
}

/* ------------------------------------------------------------------------- */
/* Public API                                                                */
/* ------------------------------------------------------------------------- */

void rtc_set_pins(uint8_t new_scl, uint8_t new_sda) {
    if (!bus_lock(pdMS_TO_TICKS(200))) return;
    /* Release the old pins. */
    if (pins_ready) {
        gpio_set_pulls(scl_pin, false, false);
        gpio_set_dir(scl_pin, GPIO_IN);
        gpio_set_pulls(sda_pin, false, false);
        gpio_set_dir(sda_pin, GPIO_IN);
    }
    scl_pin = new_scl;
    sda_pin = new_sda;
    i2c_bus_init();
    cached_present = false;
    cached_set = false;
    bus_unlock();
}

bool rtc_detect(void) {
    if (!bus_lock(pdMS_TO_TICKS(200))) return false;
    bool ok = ds_probe();
    cached_present = ok;
    if (!ok) cached_set = false;
    bus_unlock();
    return ok;
}

bool rtc_get_time(rtc_time_t *out) {
    if (!out) return false;
    if (!bus_lock(pdMS_TO_TICKS(200))) return false;
    uint8_t r[7];
    bool ok = ds_read(DS3231_REG_SECONDS, r, 7);
    bus_unlock();
    if (!ok) return false;
    out->second = bcd2bin(r[0] & 0x7F);
    out->minute = bcd2bin(r[1] & 0x7F);
    out->hour   = bcd2bin(r[2] & 0x3F);   /* assume 24-hour mode */
    out->day    = bcd2bin(r[4] & 0x3F);
    out->month  = bcd2bin(r[5] & 0x1F);
    out->year   = 2000 + bcd2bin(r[6]);
    return true;
}

bool rtc_set_time(const rtc_time_t *t) {
    if (!t) return false;
    if (!bus_lock(pdMS_TO_TICKS(200))) return false;

    uint8_t r[7];
    r[0] = bin2bcd(t->second);
    r[1] = bin2bcd(t->minute);
    r[2] = bin2bcd(t->hour);            /* bit6 = 0 -> 24-hour mode */
    r[3] = 1;                           /* day-of-week (unused) */
    r[4] = bin2bcd(t->day);
    r[5] = bin2bcd(t->month);           /* century bit 0 -> 20xx */
    r[6] = bin2bcd((uint8_t)(t->year % 100));

    bool ok = ds_write(DS3231_REG_SECONDS, r, 7);

    if (ok) {
        /* Ensure the oscillator is enabled (clear EOSC). */
        uint8_t ctrl;
        if (ds_read(DS3231_REG_CONTROL, &ctrl, 1)) {
            ctrl &= ~DS3231_CTRL_EOSC;
            ds_write(DS3231_REG_CONTROL, &ctrl, 1);
        }
        /* Clear the oscillator-stop flag to mark the time as valid. */
        uint8_t st;
        if (ds_read(DS3231_REG_STATUS, &st, 1)) {
            st &= ~DS3231_STATUS_OSF;
            ds_write(DS3231_REG_STATUS, &st, 1);
        }
    }
    bus_unlock();

    if (ok) {
        cached_present = true;
        cached_set = true;
        cached_h = t->hour;
        cached_m = t->minute;
        cached_s = t->second;
        last_min = t->minute;
        taskbar_invalidate();
    }
    return ok;
}

bool rtc_present(void)     { return cached_present; }
bool rtc_time_is_set(void) { return cached_present && cached_set; }

bool rtc_get_cached(uint8_t *h, uint8_t *m, uint8_t *s) {
    if (!cached_present || !cached_set) return false;
    if (h) *h = cached_h;
    if (m) *m = cached_m;
    if (s) *s = cached_s;
    return true;
}

/* ------------------------------------------------------------------------- */
/* 1 Hz cache timer                                                          */
/* ------------------------------------------------------------------------- */
/* Dedicated low-priority polling task                                       */
/*                                                                           */
/* The RTC is read from its own task (NOT the shared FreeRTOS timer service  */
/* task) so that the blocking bit-bang I2C never runs at the timer task's    */
/* high priority. At a low priority it is preempted by input/mouse handling, */
/* so a slow or stuck bus can't starve the rest of the system.               */
/* ------------------------------------------------------------------------- */

static void rtc_poll_once(void) {
    if (!bus_lock(pdMS_TO_TICKS(50)))
        return;

    bool present = ds_probe();
    bool set = false;
    uint8_t h = 0, m = 0, s = 0;
    if (present) {
        uint8_t r[7], st;
        if (ds_read(DS3231_REG_SECONDS, r, 7) &&
            ds_read(DS3231_REG_STATUS, &st, 1)) {
            set = !(st & DS3231_STATUS_OSF);
            s = bcd2bin(r[0] & 0x7F);
            m = bcd2bin(r[1] & 0x7F);
            h = bcd2bin(r[2] & 0x3F);
        }
    }
    bus_unlock();

    cached_present = present;
    cached_set = present && set;
    if (cached_set) {
        cached_h = h;
        cached_m = m;
        cached_s = s;
        if (m != last_min) {
            last_min = m;
            taskbar_invalidate();
        }
    }
}

static void rtc_task(void *params) {
    (void)params;
    for (;;) {
        rtc_poll_once();
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

/* Diagnostic / safety switch: when 0, the RTC does NO automatic I2C activity
 * at boot or in the background — the bus is only touched when the user opens
 * the Clock app and presses Test. Set to 1 for full automatic operation. */
#ifndef RTC_AUTOSTART
#define RTC_AUTOSTART 1
#endif

void rtc_init(void) {
    settings_t *cfg = settings_get();
    if (cfg && cfg->rtc_scl_pin) scl_pin = cfg->rtc_scl_pin;
    if (cfg && cfg->rtc_sda_pin) sda_pin = cfg->rtc_sda_pin;

    bus_mutex = xSemaphoreCreateMutex();

#if !RTC_AUTOSTART
    /* Diagnostic mode: leave GPIO 28/29 untouched until the Clock app's Test
     * button explicitly configures and probes the bus. */
    printf("RTC: autostart disabled (no boot probe / polling)\n");
    return;
#else
    i2c_bus_init();
    printf("RTC: DS3231 bit-bang I2C on SCL=%u SDA=%u\n", scl_pin, sda_pin);

    /* Probe once at boot so the taskbar can decide what to show. */
    rtc_detect();
    if (cached_present) {
        uint8_t st;
        if (bus_lock(pdMS_TO_TICKS(200))) {
            if (ds_read(DS3231_REG_STATUS, &st, 1))
                cached_set = !(st & DS3231_STATUS_OSF);
            bus_unlock();
        }
        printf("RTC: DS3231 present, time %s\n", cached_set ? "set" : "not set");
    } else {
        printf("RTC: no DS3231 detected\n");
    }

    /* Poll on a dedicated low-priority task (same priority as the netcard
     * task) so the bit-bang I2C never blocks input/mouse processing. */
    xTaskCreate(rtc_task, "rtc", 512, NULL, 1, NULL);
#endif
}
