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
#include <stdbool.h>

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

/* Show / hide the mouse cursor globally (for fullscreen modes) */
void cursor_set_visible(bool visible);
bool cursor_is_visible(void);

/* Draw the current cursor at screen position (x, y). */
void cursor_draw(int16_t x, int16_t y);

/* Return the screen-space bounding box of the current cursor centred
 * on hotspot position (x, y).  Clipped to [0, DISPLAY_WIDTH/HEIGHT). */
void cursor_get_bounds(int16_t x, int16_t y,
                       int16_t *x0, int16_t *y0,
                       int16_t *x1, int16_t *y1);

/* Return the overlay stamp position and validity.
 * *x, *y are set to the position where the cursor was last drawn.
 * Returns true if the overlay is currently stamped (valid save-under). */
bool cursor_overlay_get_stamp(int16_t *x, int16_t *y);

/* --- Show-buffer cursor overlay (save-under) --- */

/* Save pixels under cursor from show buffer, draw cursor onto show buffer */
void cursor_overlay_stamp(int16_t x, int16_t y);

/* Restore saved pixels to show buffer */
void cursor_overlay_erase(void);

/* Erase + stamp at new position; no-op if position and type unchanged */
void cursor_overlay_move(int16_t new_x, int16_t new_y);

/* Invalidate overlay state without restoring (before compositor clears) */
void cursor_overlay_reset(void);

/* Prevent/allow input_task from modifying show buffer during compositor swap */
void cursor_overlay_lock(void);
void cursor_overlay_unlock(void);
bool cursor_overlay_is_locked(void);

/* True if the current cursor type differs from the stamped overlay */
bool cursor_overlay_type_changed(void);

#endif /* CURSOR_H */
