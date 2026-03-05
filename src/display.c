/*
 * FRANK OS
 * Copyright (c) 2025 Mikhail Matveev <xtreme@rh1.tech>
 * https://rh1.tech
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "display.h"
#include "disphstx.h"
#include <string.h>
#include <stdio.h>

// Windows 95 16-color palette (RGB888)
static const uint32_t default_palette_rgb888[16] = {
    0x000000, // 0  Black
    0x000080, // 1  Blue (navy)
    0x008000, // 2  Green
    0x008080, // 3  Cyan (teal)
    0x800000, // 4  Red (maroon)
    0x800080, // 5  Magenta (purple)
    0x808000, // 6  Brown (olive)
    0xC0C0C0, // 7  Light Gray (silver)
    0x808080, // 8  Dark Gray
    0x0000FF, // 9  Light Blue
    0x00FF00, // 10 Light Green (lime)
    0x00FFFF, // 11 Light Cyan (aqua)
    0xFF0000, // 12 Light Red
    0xFF00FF, // 13 Light Magenta (fuchsia)
    0xFFFF00, // 14 Yellow
    0xFFFFFF, // 15 White
};

// CGA palette in RGB565 format for DispHSTX
static u16 cga_palette_rgb565[16];

// Single framebuffer (320 x 480, pair-encoded 4-bit)
#include <stdalign.h>
static alignas(4) uint8_t framebuffer_a[FB_STRIDE * FB_HEIGHT];

static uint8_t *draw_buffer = framebuffer_a;
uint8_t *display_show_buffer_ptr = framebuffer_a;

/* Public pointer for inline fast-path access (display.h) */
uint8_t *display_draw_buffer_ptr = framebuffer_a;

// Convert RGB888 to RGB565
static inline u16 rgb888_to_rgb565(uint32_t rgb888) {
    uint8_t r = (rgb888 >> 16) & 0xFF;
    uint8_t g = (rgb888 >> 8) & 0xFF;
    uint8_t b = rgb888 & 0xFF;
    return (u16)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | ((b & 0xF8) >> 3));
}

void display_init(void) {
    memset(framebuffer_a, 0, sizeof(framebuffer_a));
    display_draw_buffer_ptr = draw_buffer;

    // Convert CGA palette to RGB565
    for (int i = 0; i < 16; i++) {
        cga_palette_rgb565[i] = rgb888_to_rgb565(default_palette_rgb888[i]);
    }

    // Initialize DispHSTX in 640x480 4-bit paletted mode (252 MHz sys_clock).
    // We pass our own framebuffer (display_show_buffer_ptr) so the library doesn't malloc one.
    // The library launches Core 1 internally for DVI rendering.
    sDispHstxVModeState *vmode = &DispHstxVMode;
    DispHstxVModeInitTime(vmode, &DispHstxVModeTimeList[vmodetime_640x480_fast]);

    DispHstxVModeAddStrip(vmode, -1);

    int err = DispHstxVModeAddSlot(vmode,
        1,                          // hdbl: 1 = full resolution
        1,                          // vdbl: 1 = no vertical doubling
        -1,                         // w: -1 = full width (640 pixels)
        DISPHSTX_FORMAT_4_PAL,     // 4-bit paletted
        display_show_buffer_ptr,                // our own framebuffer
        -1,                         // pitch: -1 = auto (320 bytes)
        cga_palette_rgb565,         // our CGA palette
        NULL,                       // palvga: not used (DVI only)
        NULL,                       // font: not used
        -1,                         // fonth: auto
        0,                          // gap_col: no separator
        0);                         // gap_len: no separator

    if (err != DISPHSTX_ERR_OK) {
        printf("DispHSTX slot error: %d\n", err);
    }

    // Start DVI output (launches Core 1 internally)
    DispHstxSelDispMode(DISPHSTX_DISPMODE_DVI, vmode);

}

// Set pixel in the draw buffer (pair-encoded: 2 pixels per byte)
void display_set_pixel(int x, int y, uint8_t color) {
    if ((unsigned)x >= DISPLAY_WIDTH || (unsigned)y >= FB_HEIGHT) return;
    color &= 0x0F;
    uint8_t *p = &draw_buffer[y * FB_STRIDE + (x >> 1)];
    if (x & 1)
        *p = (*p & 0xF0) | color;         // right pixel = low nibble
    else
        *p = (*p & 0x0F) | (color << 4);  // left pixel = high nibble
}

void display_clear(uint8_t color) {
    uint8_t fill = (color << 4) | (color & 0x0F);
    memset(draw_buffer, fill, FB_STRIDE * FB_HEIGHT);
}

void display_swap_buffers(void) {
    /* No-op: single-buffer mode — kept for sys_table backward compat */
}

void display_wait_vsync(void) {
    DispHstxWaitVSync();
}

uint16_t display_get_scanline(void) {
    return pDispHstxVMode->line;
}

void display_wait_scanline(int16_t y) {
    while ((int16_t)pDispHstxVMode->line <= y)
        __dmb();
}

void display_draw_test_pattern(void) {
    // 16 vertical color bars, each 20 bytes wide (40 pixels in 640 mode)
    for (int y = 0; y < FB_HEIGHT; y++) {
        uint8_t *row = &draw_buffer[y * FB_STRIDE];
        for (int bar = 0; bar < 16; bar++) {
            uint8_t fill = (bar << 4) | bar;
            memset(&row[bar * 20], fill, 20);
        }
    }
}

/*==========================================================================
 * Fast horizontal span fill (no bounds check — caller must clip)
 *=========================================================================*/
void display_hline_fast(int x0, int y, int w, uint8_t color) {
    if (w <= 0) return;
    uint8_t *row = &draw_buffer[y * FB_STRIDE];
    int x_end = x0 + w;
    uint8_t fill = (color << 4) | color;

    if (x0 & 1) {
        uint8_t *p = &row[x0 >> 1];
        *p = (*p & 0xF0) | color;
        x0++;
    }

    if (x_end & 1) {
        x_end--;
        uint8_t *p = &row[x_end >> 1];
        *p = (*p & 0x0F) | (color << 4);
    }

    int byte0 = x0 >> 1;
    int byte1 = x_end >> 1;
    if (byte1 > byte0)
        memset(&row[byte0], fill, byte1 - byte0);
}

/*==========================================================================
 * Bounds-checked horizontal span — clips to screen then calls hline_fast
 *=========================================================================*/
void display_hline_safe(int x0, int y, int w, uint8_t color) {
    if (y < 0 || y >= FB_HEIGHT || w <= 0) return;
    int x1 = x0 + w;
    if (x0 < 0) x0 = 0;
    if (x1 > DISPLAY_WIDTH) x1 = DISPLAY_WIDTH;
    if (x0 >= x1) return;
    display_hline_fast(x0, y, x1 - x0, color & 0x0F);
}

/*==========================================================================
 * Fast 8-wide glyph blitter
 *
 * Requires x to be even (true for any 8px font grid).  Writes 4 bytes
 * per font row using a 4-entry lookup table that maps each pair of font
 * bits to a framebuffer byte.
 *
 * Font bit ordering: bit 0 = leftmost pixel (matches gfx_char's
 * "bits & (1 << col)" convention).
 *
 * Pixel packing: high nibble = even-x (left), low nibble = odd-x (right).
 * So for a pair of pixels at positions (2k, 2k+1):
 *   byte = (left_color << 4) | right_color
 *
 * Bit pair (bit 2k, bit 2k+1) from the font byte:
 *   bit 2k   → left pixel  (even x) → high nibble
 *   bit 2k+1 → right pixel (odd x)  → low nibble
 *=========================================================================*/
void display_blit_glyph_8wide(int x, int y, const uint8_t *glyph,
                               int h, uint8_t fg, uint8_t bg) {
    uint8_t lut[4];
    lut[0] = (bg << 4) | bg;
    lut[1] = (fg << 4) | bg;
    lut[2] = (bg << 4) | fg;
    lut[3] = (fg << 4) | fg;

    int byte_x = x >> 1;

    for (int r = 0; r < h; r++) {
        int py = y + r;
        if ((unsigned)py >= (unsigned)FB_HEIGHT) continue;
        uint8_t bits = glyph[r];
        uint8_t *dst = &draw_buffer[py * FB_STRIDE + byte_x];
        dst[0] = lut[(bits >> 0) & 3];
        dst[1] = lut[(bits >> 2) & 3];
        dst[2] = lut[(bits >> 4) & 3];
        dst[3] = lut[(bits >> 6) & 3];
    }
}
