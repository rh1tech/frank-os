/*
 * FRANK OS
 * Copyright (c) 2025 Mikhail Matveev <xtreme@rh1.tech>
 * https://rh1.tech
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef WINDOW_DRAW_H
#define WINDOW_DRAW_H

#include <stdint.h>
#include "window.h"

/*==========================================================================
 * Client-area drawing API
 *
 * All wd_* functions draw in client coordinates (0,0 = top-left of client
 * area). Drawing is automatically clipped to the client area.
 *
 * Usage:
 *   wd_begin(hwnd);       // set up clip rect and origin
 *   wd_fill_rect(...);    // draw
 *   wd_text(...);
 *   wd_end();             // restore state
 *=========================================================================*/

/*==========================================================================
 * Drawing context management
 *=========================================================================*/

/* Begin drawing to a window's client area.
 * Sets up origin offset and clip rectangle.
 * Must be paired with wd_end(). */
void wd_begin(hwnd_t hwnd);

/* End drawing to the current window */
void wd_end(void);

/*==========================================================================
 * Primitives (client coordinates, auto-clipped)
 *=========================================================================*/

/* Single pixel */
void wd_pixel(int16_t x, int16_t y, uint8_t color);

/* Horizontal line */
void wd_hline(int16_t x, int16_t y, int16_t w, uint8_t color);

/* Vertical line */
void wd_vline(int16_t x, int16_t y, int16_t h, uint8_t color);

/* Outline rectangle (1px border) */
void wd_rect(int16_t x, int16_t y, int16_t w, int16_t h, uint8_t color);

/* Filled rectangle */
void wd_fill_rect(int16_t x, int16_t y, int16_t w, int16_t h, uint8_t color);

/* Clear entire client area with color */
void wd_clear(uint8_t color);

/* Single character at (x,y) with foreground on background */
void wd_char(int16_t x, int16_t y, char c, uint8_t fg, uint8_t bg);

/* Text string with foreground on background */
void wd_text(int16_t x, int16_t y, const char *str, uint8_t fg, uint8_t bg);

/* Text with transparent background (only fg pixels drawn) */
void wd_text_transparent(int16_t x, int16_t y, const char *str, uint8_t fg);

/* Arbitrary line (Bresenham) */
void wd_line(int16_t x0, int16_t y0, int16_t x1, int16_t y1, uint8_t color);

/* 3D beveled rectangle (raised or sunken) */
void wd_bevel_rect(int16_t x, int16_t y, int16_t w, int16_t h,
                    uint8_t light, uint8_t dark, uint8_t face);

/* Single character using the 6×12 UI font */
void wd_char_ui(int16_t x, int16_t y, char c, uint8_t fg, uint8_t bg);

/* Text string using the 6×12 UI font */
void wd_text_ui(int16_t x, int16_t y, const char *str, uint8_t fg, uint8_t bg);

/* 16x16 icon (auto-clipped to client area) */
void wd_icon_16(int16_t x, int16_t y, const uint8_t *icon_data);

/* 32x32 icon (auto-clipped to client area) */
void wd_icon_32(int16_t x, int16_t y, const uint8_t *icon_data);

#endif /* WINDOW_DRAW_H */
