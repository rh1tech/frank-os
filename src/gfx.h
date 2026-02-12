/*
 * FRANK OS
 * Copyright (c) 2025 Mikhail Matveev <xtreme@rh1.tech>
 * https://rh1.tech
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef GFX_H
#define GFX_H

#include <stdint.h>
#include <stdbool.h>

/* Low-level screen-coordinate drawing primitives.
 * These operate directly on the display draw buffer via display_set_pixel.
 * No clipping to window boundaries — caller is responsible. */

/* Horizontal line: y fixed, x from x0 to x0+w-1 */
void gfx_hline(int x, int y, int w, uint8_t color);

/* Vertical line: x fixed, y from y0 to y0+h-1 */
void gfx_vline(int x, int y, int h, uint8_t color);

/* Filled rectangle */
void gfx_fill_rect(int x, int y, int w, int h, uint8_t color);

/* Outline rectangle (1px border) */
void gfx_rect(int x, int y, int w, int h, uint8_t color);

/* Single character at (x,y) using 8x16 font, fg on bg */
void gfx_char(int x, int y, char c, uint8_t fg, uint8_t bg);

/* Null-terminated string, left-to-right, 8px per char */
void gfx_text(int x, int y, const char *str, uint8_t fg, uint8_t bg);

/* Filled rectangle with screen-coordinate clipping to a clip rect.
 * Only pixels within (cx, cy, cw, ch) are drawn. */
void gfx_fill_rect_clipped(int x, int y, int w, int h, uint8_t color,
                            int cx, int cy, int cw, int ch);

/* Single character with clipping — only pixels inside clip rect are drawn */
void gfx_char_clipped(int x, int y, char c, uint8_t fg, uint8_t bg,
                       int cx, int cy, int cw, int ch);

/* Text string with clipping */
void gfx_text_clipped(int x, int y, const char *str, uint8_t fg, uint8_t bg,
                       int cx, int cy, int cw, int ch);

/* UI font (8x12) drawing functions — regular weight */
void gfx_char_ui(int x, int y, char c, uint8_t fg, uint8_t bg);
void gfx_text_ui(int x, int y, const char *str, uint8_t fg, uint8_t bg);
void gfx_text_ui_clipped(int x, int y, const char *str, uint8_t fg, uint8_t bg,
                          int cx, int cy, int cw, int ch);

/* UI font (8x12) drawing functions — bold weight
 * Uses a separate bold font array (W95font Bold variant).
 * Used for window title bars and the Start button. */
void gfx_char_ui_bold(int x, int y, char c, uint8_t fg, uint8_t bg);
void gfx_text_ui_bold(int x, int y, const char *str, uint8_t fg, uint8_t bg);
void gfx_text_ui_bold_clipped(int x, int y, const char *str,
                               uint8_t fg, uint8_t bg,
                               int cx, int cy, int cw, int ch);

/* Draw a 16x16 icon from raw palette-index data.
 * icon_data = 256 bytes (one byte per pixel, row-major).
 * 0xFF pixels are transparent (skipped). */
void gfx_draw_icon_16(int sx, int sy, const uint8_t *icon_data);

/* 16x16 icon with clip rectangle — only pixels inside (cx,cy,cw,ch) are drawn */
void gfx_draw_icon_16_clipped(int sx, int sy, const uint8_t *icon_data,
                               int cx, int cy, int cw, int ch);

/* Draw a 32x32 icon from raw palette-index data.
 * icon_data = 1024 bytes (one byte per pixel, row-major).
 * 0xFF pixels are transparent (skipped). */
void gfx_draw_icon_32(int sx, int sy, const uint8_t *icon_data);

/* 32x32 icon with clip rectangle */
void gfx_draw_icon_32_clipped(int sx, int sy, const uint8_t *icon_data,
                               int cx, int cy, int cw, int ch);

#endif /* GFX_H */
