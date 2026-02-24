/*
 * FRANK OS
 * Copyright (c) 2025 Mikhail Matveev <xtreme@rh1.tech>
 * https://rh1.tech
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef DISPLAY_H
#define DISPLAY_H

#include <stdint.h>
#include <string.h>

#define DISPLAY_WIDTH  640
#define DISPLAY_HEIGHT 480
#define FB_WIDTH       320
#define FB_HEIGHT      480
#define FB_STRIDE      320
#define NUM_COLORS     16

void display_init(void);
void display_set_pixel(int x, int y, uint8_t color);
void display_clear(uint8_t color);
void display_swap_buffers(void);
void display_wait_vsync(void);
void display_draw_test_pattern(void);

/* Direct draw-buffer pointer — updated by display_init / display_swap_buffers */
extern uint8_t *display_draw_buffer_ptr;

/* Direct show-buffer pointer — updated by display_init / display_swap_buffers */
extern uint8_t *display_show_buffer_ptr;

/* Fast pixel set — no bounds check, caller must guarantee valid coords.
 * x is in 640-wide screen coords, y in 480-high coords. */
static inline void display_set_pixel_fast(int x, int y, uint8_t color) {
    uint8_t *p = &display_draw_buffer_ptr[y * FB_STRIDE + (x >> 1)];
    if (x & 1)
        *p = (*p & 0xF0) | color;
    else
        *p = (*p & 0x0F) | (color << 4);
}

/* Fast horizontal span fill — no bounds check.
 * x0, y in screen coords; w = pixel count; color = 4-bit palette index. */
void display_hline_fast(int x0, int y, int w, uint8_t color);

/* Bounds-checked horizontal span — clips to screen, then calls hline_fast. */
void display_hline_safe(int x0, int y, int w, uint8_t color);

/* Fast 8-wide glyph blitter.
 * x must be even (always true at 8px grid). Writes directly to framebuffer.
 * glyph = pointer to font row data (h bytes, 1 byte/row, bit0=leftmost).
 * fg, bg = 4-bit color indices. */
void display_blit_glyph_8wide(int x, int y, const uint8_t *glyph,
                               int h, uint8_t fg, uint8_t bg);

#endif
