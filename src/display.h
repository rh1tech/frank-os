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

/* ======================================================================
 * Video modes
 * ====================================================================== */

#define VIDEO_MODE_640x480x16   0   /* Desktop: 640x480, 4bpp, 16 colors  */
#define VIDEO_MODE_320x240x256  1   /* Fullscreen: 320x240, 8bpp, 256 colors */

/* ======================================================================
 * Compile-time constants for the default desktop mode (640x480x16)
 * ====================================================================== */

#define DISPLAY_WIDTH  640
#define DISPLAY_HEIGHT 480
#define FB_WIDTH       320
#define FB_HEIGHT      480
#define FB_STRIDE      320
#define NUM_COLORS     16

/* ======================================================================
 * Runtime state — reflects the currently active video mode
 * ====================================================================== */

extern uint16_t display_width;      /* logical pixel width  (640 or 320) */
extern uint16_t display_height;     /* logical pixel height (480 or 240) */
extern uint16_t display_fb_stride;  /* bytes per framebuffer row         */
extern uint8_t  display_bpp;        /* bits per pixel (4 or 8)           */
extern uint8_t  display_video_mode; /* current VIDEO_MODE_*              */
extern volatile uint8_t display_compositor_idle; /* 1 = compositor in bypass */

/* ======================================================================
 * Core API
 * ====================================================================== */

void display_init(void);

/* Request a video mode change — sets the mode flag so the compositor
 * enters its bypass loop, but does NOT reconfigure DVI hardware.
 * Call this, wait ~50ms for compositor to yield, then call
 * display_set_video_mode() for the actual switch. */
void display_request_mode(uint8_t mode);

/* Switch video mode at runtime.  Stops DVI output, reconfigures DispHSTX,
 * clears the framebuffer, and restarts DVI.
 * Returns 0 on success, negative on error. */
int  display_set_video_mode(uint8_t mode);

/* Returns the current VIDEO_MODE_* value. */
uint8_t display_get_video_mode(void);

/* Set one entry in the 256-color palette (only meaningful in 320x240x256).
 * index 0..255, rgb888 is 0xRRGGBB. */
void display_set_palette_entry(uint8_t index, uint32_t rgb888);

void display_set_pixel(int x, int y, uint8_t color);
void display_clear(uint8_t color);
void display_swap_buffers(void);
void display_wait_vsync(void);
uint16_t display_get_scanline(void);
void display_wait_scanline(int16_t y);
void display_draw_test_pattern(void);

/* Direct draw-buffer pointer — updated by display_init / display_swap_buffers */
extern uint8_t *display_draw_buffer_ptr;

/* Direct show-buffer pointer — updated by display_init / display_swap_buffers */
extern uint8_t *display_show_buffer_ptr;

/* ======================================================================
 * Fast inline pixel set — 4bpp mode only (640x480x16)
 *
 * No bounds check — caller must guarantee valid coords.
 * x is in 640-wide screen coords, y in 480-high coords.
 * DO NOT use this in 320x240x256 mode; use display_set_pixel_8bpp_fast.
 * ====================================================================== */

static inline void display_set_pixel_fast(int x, int y, uint8_t color) {
    if (display_bpp == 8) {
        display_draw_buffer_ptr[y * display_fb_stride + x] = color;
    } else {
        uint8_t *p = &display_draw_buffer_ptr[y * FB_STRIDE + (x >> 1)];
        if (x & 1)
            *p = (*p & 0xF0) | color;
        else
            *p = (*p & 0x0F) | (color << 4);
    }
}

/* ======================================================================
 * Fast inline pixel set — 8bpp mode only (320x240x256)
 *
 * No bounds check — caller must guarantee valid coords.
 * x is in 320-wide coords, y in 240-high coords.
 * ====================================================================== */

static inline void display_set_pixel_8bpp_fast(int x, int y, uint8_t color) {
    display_draw_buffer_ptr[y * 320 + x] = color;
}

/* Fast horizontal span fill — no bounds check.
 * x0, y in screen coords; w = pixel count; color = 4-bit palette index.
 * 4bpp (640x480x16) mode only. */
void display_hline_fast(int x0, int y, int w, uint8_t color);

/* Bounds-checked horizontal span — clips to screen, then calls hline_fast.
 * Mode-aware: works in both 4bpp and 8bpp modes. */
void display_hline_safe(int x0, int y, int w, uint8_t color);

/* Fast 8-wide glyph blitter.
 * x must be even (always true at 8px grid). Writes directly to framebuffer.
 * glyph = pointer to font row data (h bytes, 1 byte/row, bit0=leftmost).
 * fg, bg = 4-bit color indices.
 * 4bpp (640x480x16) mode only. */
void display_blit_glyph_8wide(int x, int y, const uint8_t *glyph,
                               int h, uint8_t fg, uint8_t bg);

#endif
