#ifndef FONT_H
#define FONT_H

#include <stdint.h>

/* 8x8 fixed-width bitmap font, CP866 (256 glyphs).
 * Each glyph is 8 bytes — one byte per row, MSB = leftmost pixel.
 * Total: 256 * 8 = 2048 bytes, stored in flash (.rodata). */
#define FONT8x8_WIDTH   8
#define FONT8x8_HEIGHT  8

extern const uint8_t font_8x8[2048];

static inline const uint8_t *font8x8_get_glyph(uint8_t c) {
    return &font_8x8[c * 8];
}

/* 8x16 fixed-width bitmap font, CP866 (256 glyphs).
 * Each glyph is 16 bytes — one byte per row, MSB = leftmost pixel.
 * Total: 256 * 16 = 4096 bytes, stored in flash (.rodata). */
#define FONT8x16_WIDTH  8
#define FONT8x16_HEIGHT 16

extern const uint8_t font_8x16[4096];

static inline const uint8_t *font8x16_get_glyph(uint8_t c) {
    return &font_8x16[c * 16];
}

/* Default font (8x8) — used by gfx.c and window title/text drawing */
#define FONT_WIDTH   8
#define FONT_HEIGHT  8

static inline const uint8_t *font_get_glyph(char c) {
    uint8_t u = (uint8_t)c;
    if (u >= 'a' && u <= 'z') u -= 32;
    return &font_8x8[u * 8];
}

#endif /* FONT_H */
