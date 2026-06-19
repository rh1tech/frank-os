/*
 * FRANK OS — DS3231MZ Real-Time Clock Driver
 * Copyright (c) 2026 Mikhail Matveev <xtreme@rh1.tech>
 * https://rh1.tech
 *
 * Bit-banged I2C client for the DS3231MZ RTC on user-configurable GPIO pins
 * (defaults SCL=29, SDA=28). A 1 Hz software timer caches the current time so
 * the taskbar can read it without touching the I2C bus on every repaint.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef RTC_H
#define RTC_H

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    uint16_t year;    /* full year, e.g. 2026 */
    uint8_t  month;   /* 1-12 */
    uint8_t  day;     /* 1-31 */
    uint8_t  hour;    /* 0-23 */
    uint8_t  minute;  /* 0-59 */
    uint8_t  second;  /* 0-59 */
} rtc_time_t;

/* Initialise the RTC driver: configures the I2C GPIO pins from settings and
 * starts the 1 Hz cache timer. Safe to call once at boot. */
void rtc_init(void);

/* Reconfigure the I2C bus to use new SCL/SDA GPIO pins (does not persist —
 * the caller saves them to settings). Pins must be in 16..47 and distinct. */
void rtc_set_pins(uint8_t scl_pin, uint8_t sda_pin);

/* Probe the bus for the DS3231 (blocking, ~1 ms). Updates cached presence. */
bool rtc_detect(void);

/* Read the current date/time from the chip (blocking). */
bool rtc_get_time(rtc_time_t *out);

/* Write date/time to the chip and clear the oscillator-stop flag (blocking). */
bool rtc_set_time(const rtc_time_t *t);

/* Cached state (updated by the 1 Hz timer; no I2C access). */
bool rtc_present(void);       /* chip answered its address */
bool rtc_time_is_set(void);   /* present AND oscillator never stopped */

/* Cached wall-clock time for the taskbar. Returns false if time isn't set. */
bool rtc_get_cached(uint8_t *hour, uint8_t *minute, uint8_t *second);

#endif /* RTC_H */
