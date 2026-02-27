/*
 * FRANK OS
 * Copyright (c) 2025 Mikhail Matveev <xtreme@rh1.tech>
 * https://rh1.tech
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef STARTMENU_H
#define STARTMENU_H

#include <stdint.h>
#include <stdbool.h>

/* Pre-scan /fos/ apps at boot (call once from compositor task) */
void startmenu_init(void);

/* Toggle start menu visibility */
void startmenu_toggle(void);

/* Close the start menu */
void startmenu_close(void);

/* Is the start menu currently open? */
bool startmenu_is_open(void);

/* Draw the start menu as a compositor overlay */
void startmenu_draw(void);

/* Handle mouse click. Returns true if consumed. */
bool startmenu_mouse(uint8_t type, int16_t x, int16_t y);

/* Handle keyboard navigation. Returns true if consumed. */
bool startmenu_handle_key(uint8_t hid_code, uint8_t modifiers);

/* Poll for deferred dialog results (call from compositor loop) */
void startmenu_check_pending(void);

#endif /* STARTMENU_H */
