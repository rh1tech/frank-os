/*
 * FRANK OS — Clock Settings Internal App
 * Copyright (c) 2026 Mikhail Matveev <xtreme@rh1.tech>
 * https://rh1.tech
 *
 * DS3231 RTC configuration: I2C pin setup, chip detection, and time setting.
 * Accessible from Start -> Settings -> Clock.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef CLOCK_APP_H
#define CLOCK_APP_H

#include "window.h"

/* Create (or focus) the Clock settings window. */
hwnd_t clock_app_create(void);

/* Clock icon accessors (clock_icon.c). */
const uint8_t *clock_icon16_get(void);
const uint8_t *clock_icon32_get(void);

#endif /* CLOCK_APP_H */
