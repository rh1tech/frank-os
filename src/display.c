#include "display.h"
#include "disphstx.h"
#include <string.h>
#include <stdio.h>

// CGA/EGA 16-color palette (RGB888)
static const uint32_t default_palette_rgb888[16] = {
    0x000000, // 0  Black
    0x0000AA, // 1  Blue
    0x00AA00, // 2  Green
    0x00AAAA, // 3  Cyan
    0xAA0000, // 4  Red
    0xAA00AA, // 5  Magenta
    0xAA5500, // 6  Brown
    0xAAAAAA, // 7  Light Gray
    0x555555, // 8  Dark Gray
    0x5555FF, // 9  Light Blue
    0x55FF55, // 10 Light Green
    0x55FFFF, // 11 Light Cyan
    0xFF5555, // 12 Light Red
    0xFF55FF, // 13 Light Magenta
    0xFFFF55, // 14 Yellow
    0xFFFFFF, // 15 White
};

// CGA palette in RGB565 format for DispHSTX
static u16 cga_palette_rgb565[16];

// Double-buffered framebuffers (320 x 480, pair-encoded 4-bit)
#include <stdalign.h>
static alignas(4) uint8_t framebuffer_a[FB_STRIDE * FB_HEIGHT];
static alignas(4) uint8_t framebuffer_b[FB_STRIDE * FB_HEIGHT];

static uint8_t *draw_buffer = framebuffer_b;
static uint8_t *show_buffer = framebuffer_a;

// Convert RGB888 to RGB565
static inline u16 rgb888_to_rgb565(uint32_t rgb888) {
    uint8_t r = (rgb888 >> 16) & 0xFF;
    uint8_t g = (rgb888 >> 8) & 0xFF;
    uint8_t b = rgb888 & 0xFF;
    return (u16)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | ((b & 0xF8) >> 3));
}

void display_init(void) {
    memset(framebuffer_a, 0, sizeof(framebuffer_a));
    memset(framebuffer_b, 0, sizeof(framebuffer_b));

    // Convert CGA palette to RGB565
    for (int i = 0; i < 16; i++) {
        cga_palette_rgb565[i] = rgb888_to_rgb565(default_palette_rgb888[i]);
    }

    // Initialize DispHSTX in 640x480 4-bit paletted mode (252 MHz sys_clock).
    // We pass our own framebuffer (show_buffer) so the library doesn't malloc one.
    // The library launches Core 1 internally for DVI rendering.
    sDispHstxVModeState *vmode = &DispHstxVMode;
    DispHstxVModeInitTime(vmode, &DispHstxVModeTimeList[vmodetime_640x480_fast]);

    DispHstxVModeAddStrip(vmode, -1);

    int err = DispHstxVModeAddSlot(vmode,
        1,                          // hdbl: 1 = full resolution
        1,                          // vdbl: 1 = no vertical doubling
        -1,                         // w: -1 = full width (640 pixels)
        DISPHSTX_FORMAT_4_PAL,     // 4-bit paletted
        show_buffer,                // our own framebuffer
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

    printf("DispHSTX: 640x480x4 DVI initialized\n");
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
    uint8_t *tmp = draw_buffer;
    draw_buffer = show_buffer;
    show_buffer = tmp;

    // Update the DispHSTX slot's framebuffer pointer to the new show buffer.
    // The library reads this pointer during scanline rendering on Core 1.
    pDispHstxVMode->strip[0].slot[0].buf = show_buffer;
}

void display_wait_vsync(void) {
    DispHstxWaitVSync();
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
    printf("Test pattern: 16 color bars (640x480, pair-encoded)\n");
}
