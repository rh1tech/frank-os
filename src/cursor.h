/*
 * FRANK OS
 * Copyright (c) 2025 Mikhail Matveev <xtreme@rh1.tech>
 * https://rh1.tech
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef CURSOR_H
#define CURSOR_H

#include <stdint.h>

/* Cursor types */
typedef enum {
    CURSOR_ARROW,       /* default pointer */
    CURSOR_RESIZE_NS,   /* vertical resize ↕ (top/bottom borders) */
    CURSOR_RESIZE_EW,   /* horizontal resize ↔ (left/right borders) */
    CURSOR_RESIZE_NWSE, /* diagonal resize ↖↘ (TL/BR corners) */
    CURSOR_RESIZE_NESW, /* diagonal resize ↗↙ (TR/BL corners) */
    CURSOR_WAIT,        /* hourglass (busy) */
    CURSOR_COUNT
} cursor_type_t;

/* Set the active cursor shape */
void cursor_set_type(cursor_type_t type);

/* Get the active cursor shape */
cursor_type_t cursor_get_type(void);

/* Draw the current cursor at screen position (x, y). */
void cursor_draw(int16_t x, int16_t y);

#endif /* CURSOR_H */
