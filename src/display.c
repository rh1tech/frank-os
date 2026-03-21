/*
 * FRANK OS
 * Copyright (c) 2025 Mikhail Matveev <xtreme@rh1.tech>
 * https://rh1.tech
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "display.h"
#include "disphstx.h"
#include "FreeRTOS.h"
#include "portable.h"
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

// CGA palette in RGB565 format for DispHSTX (16-color mode)
static u16 cga_palette_rgb565[16];

// 256-color palette in RGB565 format for DispHSTX (320x240x256 mode)
static u16 palette_256_rgb565[256];

// Framebuffer — large enough for the biggest mode:
// 640x480x4bpp = 153,600 bytes; 320x240x8bpp = 76,800 bytes
#include <stdalign.h>
static alignas(4) uint8_t framebuffer_a[FB_STRIDE * FB_HEIGHT];

/* Pre-initialized vmode descriptor for 320x240x256 hot-swap.
 * Heap-allocated (4.7KB) at display_init time so we can swap
 * pDispHstxVMode without stopping/restarting DVI. */
static sDispHstxVModeState *vmode_320x240 = NULL;

static uint8_t *draw_buffer = framebuffer_a;
uint8_t *display_show_buffer_ptr = framebuffer_a;

/* Public pointer for inline fast-path access (display.h) */
uint8_t *display_draw_buffer_ptr = framebuffer_a;

/* Runtime video mode state */
uint16_t display_width      = DISPLAY_WIDTH;
uint16_t display_height     = DISPLAY_HEIGHT;
uint16_t display_fb_stride  = FB_STRIDE;
uint8_t  display_bpp        = 4;
uint8_t  display_video_mode = VIDEO_MODE_640x480x16;
volatile uint8_t display_compositor_idle = 0;

// Convert RGB888 to RGB565
static inline u16 rgb888_to_rgb565(uint32_t rgb888) {
    uint8_t r = (rgb888 >> 16) & 0xFF;
    uint8_t g = (rgb888 >> 8) & 0xFF;
    uint8_t b = rgb888 & 0xFF;
    return (u16)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | ((b & 0xF8) >> 3));
}

/* Initialize the default 256-color VGA palette.
 * Indices 0-15: CGA/EGA colors (same as 16-color mode).
 * Indices 16-231: 6x6x6 color cube.
 * Indices 232-255: grayscale ramp. */
static void init_default_256_palette(void) {
    // 0-15: standard CGA colors
    for (int i = 0; i < 16; i++)
        palette_256_rgb565[i] = rgb888_to_rgb565(default_palette_rgb888[i]);

    // 16-231: 6x6x6 color cube
    int idx = 16;
    for (int r = 0; r < 6; r++)
        for (int g = 0; g < 6; g++)
            for (int b = 0; b < 6; b++) {
                uint32_t rgb = ((r * 51) << 16) | ((g * 51) << 8) | (b * 51);
                palette_256_rgb565[idx++] = rgb888_to_rgb565(rgb);
            }

    // 232-255: grayscale ramp (8..238 in steps of 10)
    for (int i = 0; i < 24; i++) {
        uint8_t v = (uint8_t)(8 + i * 10);
        uint32_t rgb = ((uint32_t)v << 16) | ((uint32_t)v << 8) | v;
        palette_256_rgb565[idx++] = rgb888_to_rgb565(rgb);
    }
}

/* ======================================================================
 * Internal: configure and start a video mode via DispHSTX
 * ====================================================================== */

static void start_mode_640x480x16(void) {
    sDispHstxVModeState *vmode = &DispHstxVMode;
    DispHstxVModeInitTime(vmode, &DispHstxVModeTimeList[vmodetime_640x480_fast]);

    DispHstxVModeAddStrip(vmode, -1);

    int err = DispHstxVModeAddSlot(vmode,
        1,                          // hdbl: 1 = full resolution
        1,                          // vdbl: 1 = no vertical doubling
        -1,                         // w: -1 = full width (640 pixels)
        DISPHSTX_FORMAT_4_PAL,     // 4-bit paletted
        display_show_buffer_ptr,    // our framebuffer
        -1,                         // pitch: -1 = auto (320 bytes)
        cga_palette_rgb565,         // CGA palette
        NULL,                       // palvga: not used (DVI only)
        NULL,                       // font: not used
        -1,                         // fonth: auto
        0,                          // gap_col: no separator
        0);                         // gap_len: no separator

    if (err != DISPHSTX_ERR_OK) {
        printf("DispHSTX 640x480 slot error: %d\n", err);
    }

    DispHstxSelDispMode(DISPHSTX_DISPMODE_DVI, vmode);
}

static void start_mode_320x240x256(void) {
    sDispHstxVModeState *vmode = &DispHstxVMode;
    DispHstxVModeInitTime(vmode, &DispHstxVModeTimeList[vmodetime_640x480_fast]);

    DispHstxVModeAddStrip(vmode, -1);

    int err = DispHstxVModeAddSlot(vmode,
        2,                          // hdbl: 2 = double pixels (640/2 = 320)
        2,                          // vdbl: 2 = double lines  (480/2 = 240)
        -1,                         // w: -1 = full width (320 logical pixels)
        DISPHSTX_FORMAT_8_PAL,     // 8-bit paletted
        display_show_buffer_ptr,    // same framebuffer
        -1,                         // pitch: -1 = auto (320 bytes)
        palette_256_rgb565,         // 256-color palette
        NULL,                       // palvga: not used
        NULL,                       // font: not used
        -1,                         // fonth: auto
        0,                          // gap_col: no separator
        0);                         // gap_len: no separator

    if (err != DISPHSTX_ERR_OK) {
        printf("DispHSTX 320x240 slot error: %d\n", err);
    }

    DispHstxSelDispMode(DISPHSTX_DISPMODE_DVI, vmode);
}

/* ======================================================================
 * Public API
 * ====================================================================== */

void display_init(void) {
    memset(framebuffer_a, 0, sizeof(framebuffer_a));
    display_draw_buffer_ptr = draw_buffer;

    // Convert CGA palette to RGB565
    for (int i = 0; i < 16; i++) {
        cga_palette_rgb565[i] = rgb888_to_rgb565(default_palette_rgb888[i]);
    }

    // Initialize default 256-color palette
    init_default_256_palette();

    // Set runtime state for default mode
    display_width      = DISPLAY_WIDTH;
    display_height     = DISPLAY_HEIGHT;
    display_fb_stride  = FB_STRIDE;
    display_bpp        = 4;
    display_video_mode = VIDEO_MODE_640x480x16;

    start_mode_640x480x16();

    /* Pre-initialize the 320x240x256 vmode descriptor so that
     * display_set_video_mode can hot-swap without DVI restart. */
    {
        vmode_320x240 = (sDispHstxVModeState *)pvPortCalloc(1, sizeof(sDispHstxVModeState));
        if (vmode_320x240) {
            sDispHstxVModeState *vm = vmode_320x240;
            DispHstxVModeInitTime(vm, &DispHstxVModeTimeList[vmodetime_640x480_fast]);
            DispHstxVModeAddStrip(vm, -1);
            DispHstxVModeAddSlot(vm, 2, 2, -1,
                DISPHSTX_FORMAT_8_PAL,
                display_show_buffer_ptr,
                -1,
                palette_256_rgb565,
                NULL, NULL, -1, 0, 0);
            DispHstxDviPrepare(vm);
        }
    }
}

/* Set the video_mode flag without actually switching hardware.
 * Used to make the compositor enter bypass BEFORE the real switch. */
void display_request_mode(uint8_t mode) {
    display_video_mode = mode;
}

/* Reconfigure the ACTIVE vmode descriptor in-place during vblank.
 * DVI keeps running — no DispHstxAllTerm, no restart, HDMI link stays up. */
static void reconfigure_vmode_inplace(int hdbl, int vdbl, int format,
                                       const u16 *pal) {
    sDispHstxVModeState *v = &DispHstxVMode;

    /* Clear framebuffer first (while old mode still displays) */
    memset(framebuffer_a, 0, sizeof(framebuffer_a));
    display_draw_buffer_ptr = draw_buffer;
    display_show_buffer_ptr = framebuffer_a;

    /* Wait for vblank start, then quickly reconfigure with IRQs disabled
     * so the DVI ISR doesn't read a half-updated descriptor. */
    DispHstxWaitVSync();
    uint32_t saved = save_and_disable_interrupts();

    DispHstxVModeInitTime(v, &DispHstxVModeTimeList[vmodetime_640x480_fast]);
    DispHstxVModeAddStrip(v, -1);
    DispHstxVModeAddSlot(v, hdbl, vdbl, -1, format,
                          display_show_buffer_ptr, -1,
                          pal, NULL, NULL, -1, 0, 0);
    DispHstxDviPrepare(v);

    restore_interrupts(saved);
}

int display_set_video_mode(uint8_t mode) {
    if (mode == display_video_mode) {
        /* If mode was pre-set by display_request_mode, force the switch */
        if (mode == VIDEO_MODE_320x240x256 && display_bpp != 8)
            goto do_switch;
        if (mode == VIDEO_MODE_640x480x16 && display_bpp != 4)
            goto do_switch;
        return 0;
    }
do_switch:

    switch (mode) {
    case VIDEO_MODE_640x480x16:
        display_width      = 640;
        display_height     = 480;
        display_fb_stride  = 320;
        display_bpp        = 4;
        display_video_mode = VIDEO_MODE_640x480x16;
        reconfigure_vmode_inplace(1, 1, DISPHSTX_FORMAT_4_PAL,
                                   cga_palette_rgb565);
        return 0;

    case VIDEO_MODE_320x240x256:
        display_width      = 320;
        display_height     = 240;
        display_fb_stride  = 320;
        display_bpp        = 8;
        display_video_mode = VIDEO_MODE_320x240x256;
        reconfigure_vmode_inplace(2, 2, DISPHSTX_FORMAT_8_PAL,
                                   palette_256_rgb565);
        return 0;

    default:
        return -1;
    }
}

uint8_t display_get_video_mode(void) {
    return display_video_mode;
}

void display_set_palette_entry(uint8_t index, uint32_t rgb888) {
    palette_256_rgb565[index] = rgb888_to_rgb565(rgb888);
}

// Set pixel in the draw buffer — mode-aware
void display_set_pixel(int x, int y, uint8_t color) {
    if (display_bpp == 8) {
        // 8bpp: 1 byte per pixel
        if ((unsigned)x >= display_width || (unsigned)y >= display_height) return;
        draw_buffer[y * display_fb_stride + x] = color;
    } else {
        // 4bpp: 2 pixels per byte (pair-encoded)
        if ((unsigned)x >= DISPLAY_WIDTH || (unsigned)y >= FB_HEIGHT) return;
        color &= 0x0F;
        uint8_t *p = &draw_buffer[y * FB_STRIDE + (x >> 1)];
        if (x & 1)
            *p = (*p & 0xF0) | color;         // right pixel = low nibble
        else
            *p = (*p & 0x0F) | (color << 4);  // left pixel = high nibble
    }
}

void display_clear(uint8_t color) {
    if (display_bpp == 8) {
        memset(draw_buffer, color, display_fb_stride * display_height);
    } else {
        uint8_t fill = (color << 4) | (color & 0x0F);
        memset(draw_buffer, fill, FB_STRIDE * FB_HEIGHT);
    }
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
    if (display_bpp == 8) {
        // 256-color bars (32 bars of 10 pixels each)
        for (int y = 0; y < (int)display_height; y++) {
            uint8_t *row = &draw_buffer[y * display_fb_stride];
            for (int x = 0; x < (int)display_width; x++) {
                row[x] = (uint8_t)(x * 256 / display_width);
            }
        }
    } else {
        // 16 vertical color bars, each 20 bytes wide (40 pixels in 640 mode)
        for (int y = 0; y < FB_HEIGHT; y++) {
            uint8_t *row = &draw_buffer[y * FB_STRIDE];
            for (int bar = 0; bar < 16; bar++) {
                uint8_t fill = (bar << 4) | bar;
                memset(&row[bar * 20], fill, 20);
            }
        }
    }
}

/*==========================================================================
 * Fast horizontal span fill (no bounds check — caller must clip)
 *=========================================================================*/
void display_hline_fast(int x0, int y, int w, uint8_t color) {
    if (w <= 0) return;

    if (display_bpp == 8) {
        /* 8bpp: one byte per pixel — simple memset */
        memset(&draw_buffer[y * display_fb_stride + x0], color, w);
        return;
    }

    /* 4bpp: pair-encoded nibbles */
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
 * Bounds-checked horizontal span — clips to screen, mode-aware
 *=========================================================================*/
void display_hline_safe(int x0, int y, int w, uint8_t color) {
    if (w <= 0) return;

    if (display_bpp == 8) {
        if (y < 0 || y >= (int)display_height) return;
        int x1 = x0 + w;
        if (x0 < 0) x0 = 0;
        if (x1 > (int)display_width) x1 = (int)display_width;
        if (x0 >= x1) return;
        memset(&draw_buffer[y * display_fb_stride + x0], color, x1 - x0);
    } else {
        if (y < 0 || y >= FB_HEIGHT) return;
        int x1 = x0 + w;
        if (x0 < 0) x0 = 0;
        if (x1 > DISPLAY_WIDTH) x1 = DISPLAY_WIDTH;
        if (x0 >= x1) return;
        display_hline_fast(x0, y, x1 - x0, color & 0x0F);
    }
}

/*==========================================================================
 * Fast 8-wide glyph blitter (4bpp mode only)
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
    if (display_bpp == 8) {
        /* 8bpp: write 8 bytes per font row, one byte per pixel */
        for (int r = 0; r < h; r++) {
            int py = y + r;
            if ((unsigned)py >= (unsigned)display_height) continue;
            uint8_t bits = glyph[r];
            uint8_t *dst = &draw_buffer[py * display_fb_stride + x];
            for (int col = 0; col < 8; col++)
                dst[col] = (bits & (1 << col)) ? fg : bg;
        }
        return;
    }

    /* 4bpp: pair-encoded LUT path */
    uint8_t lut[4];
    lut[0] = (bg << 4) | bg;
    lut[1] = (fg << 4) | bg;
    lut[2] = (bg << 4) | fg;
    lut[3] = (fg << 4) | fg;

    int byte_x = x >> 1;

    for (int r = 0; r < h; r++) {
        int py = y + r;
        if ((unsigned)py >= (unsigned)display_height) continue;
        uint8_t bits = glyph[r];
        uint8_t *dst = &draw_buffer[py * FB_STRIDE + byte_x];
        dst[0] = lut[(bits >> 0) & 3];
        dst[1] = lut[(bits >> 2) & 3];
        dst[2] = lut[(bits >> 4) & 3];
        dst[3] = lut[(bits >> 6) & 3];
    }
}
