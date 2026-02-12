/*
 * FRANK OS
 * Copyright (c) 2025 Mikhail Matveev <xtreme@rh1.tech>
 * https://rh1.tech
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "gfx.h"
#include "display.h"
#include "font.h"

void gfx_hline(int x, int y, int w, uint8_t color) {
    display_hline_safe(x, y, w, color);
}

void gfx_vline(int x, int y, int h, uint8_t color) {
    for (int i = 0; i < h; i++)
        display_set_pixel(x, y + i, color);
}

void gfx_fill_rect(int x, int y, int w, int h, uint8_t color) {
    /* Clip to screen */
    int x1 = x + w;
    int y1 = y + h;
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    if (x1 > DISPLAY_WIDTH) x1 = DISPLAY_WIDTH;
    if (y1 > FB_HEIGHT) y1 = FB_HEIGHT;
    if (x >= x1 || y >= y1) return;
    int cw = x1 - x;
    color &= 0x0F;
    for (int row = y; row < y1; row++)
        display_hline_fast(x, row, cw, color);
}

void gfx_fill_rect_dithered(int x, int y, int w, int h, uint8_t color) {
    int x1 = x + w;
    int y1 = y + h;
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    if (x1 > DISPLAY_WIDTH) x1 = DISPLAY_WIDTH;
    if (y1 > FB_HEIGHT) y1 = FB_HEIGHT;
    if (x >= x1 || y >= y1) return;
    for (int row = y; row < y1; row++)
        for (int col = x; col < x1; col++)
            if ((row + col) & 1)
                display_set_pixel(col, row, color);
}

void gfx_rect(int x, int y, int w, int h, uint8_t color) {
    gfx_hline(x, y, w, color);
    gfx_hline(x, y + h - 1, w, color);
    gfx_vline(x, y, h, color);
    gfx_vline(x + w - 1, y, h, color);
}

void gfx_char(int x, int y, char c, uint8_t fg, uint8_t bg) {
    /* Fast path: even x and fully on-screen */
    if (!(x & 1) &&
        x >= 0 && (x + FONT_WIDTH) <= DISPLAY_WIDTH &&
        y >= 0 && (y + FONT_HEIGHT) <= FB_HEIGHT) {
        display_blit_glyph_8wide(x, y, font_get_glyph(c),
                                  FONT_HEIGHT, fg & 0x0F, bg & 0x0F);
        return;
    }
    /* Fallback: per-pixel */
    const uint8_t *glyph = font_get_glyph(c);
    for (int row = 0; row < FONT_HEIGHT; row++) {
        uint8_t bits = glyph[row];
        for (int col = 0; col < FONT_WIDTH; col++) {
            display_set_pixel(x + col, y + row,
                              (bits & (1 << col)) ? fg : bg);
        }
    }
}

void gfx_text(int x, int y, const char *str, uint8_t fg, uint8_t bg) {
    while (*str) {
        gfx_char(x, y, *str, fg, bg);
        x += FONT_WIDTH;
        str++;
    }
}

void gfx_fill_rect_clipped(int x, int y, int w, int h, uint8_t color,
                            int cx, int cy, int cw, int ch) {
    /* Intersect (x,y,w,h) with clip rect (cx,cy,cw,ch) */
    int x0 = x < cx ? cx : x;
    int y0 = y < cy ? cy : y;
    int x1 = (x + w) < (cx + cw) ? (x + w) : (cx + cw);
    int y1 = (y + h) < (cy + ch) ? (y + h) : (cy + ch);

    if (x0 >= x1 || y0 >= y1) return;

    /* Also clip to screen */
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > DISPLAY_WIDTH) x1 = DISPLAY_WIDTH;
    if (y1 > FB_HEIGHT) y1 = FB_HEIGHT;
    if (x0 >= x1 || y0 >= y1) return;

    int span = x1 - x0;
    color &= 0x0F;
    for (int row = y0; row < y1; row++)
        display_hline_fast(x0, row, span, color);
}

void gfx_char_clipped(int x, int y, char c, uint8_t fg, uint8_t bg,
                       int cx, int cy, int cw, int ch) {
    /* Fast path: even x and fully inside clip rect and on-screen */
    if (!(x & 1) &&
        x >= cx && (x + FONT_WIDTH) <= (cx + cw) &&
        y >= cy && (y + FONT_HEIGHT) <= (cy + ch) &&
        x >= 0 && (x + FONT_WIDTH) <= DISPLAY_WIDTH &&
        y >= 0 && (y + FONT_HEIGHT) <= FB_HEIGHT) {
        display_blit_glyph_8wide(x, y, font_get_glyph(c),
                                  FONT_HEIGHT, fg & 0x0F, bg & 0x0F);
        return;
    }
    /* Fallback: per-pixel with clip */
    const uint8_t *glyph = font_get_glyph(c);
    int cx1 = cx + cw;
    int cy1 = cy + ch;

    for (int row = 0; row < FONT_HEIGHT; row++) {
        int py = y + row;
        if (py < cy || py >= cy1) continue;
        uint8_t bits = glyph[row];
        for (int col = 0; col < FONT_WIDTH; col++) {
            int px = x + col;
            if (px < cx || px >= cx1) continue;
            display_set_pixel(px, py, (bits & (1 << col)) ? fg : bg);
        }
    }
}

void gfx_text_clipped(int x, int y, const char *str, uint8_t fg, uint8_t bg,
                       int cx, int cy, int cw, int ch) {
    while (*str) {
        /* Skip chars entirely outside clip rect */
        if (x + FONT_WIDTH > cx && x < cx + cw)
            gfx_char_clipped(x, y, *str, fg, bg, cx, cy, cw, ch);
        x += FONT_WIDTH;
        str++;
    }
}

/*==========================================================================
 * UI font (8x12) — regular weight
 *
 * The UI font uses MSB=leftmost bit ordering (natural for authoring),
 * while display_blit_glyph_8wide expects LSB=leftmost.  Since the UI
 * font only renders small amounts of chrome text (not a 70x20 grid),
 * we always use per-pixel rendering — the cost is negligible.
 *=========================================================================*/

void gfx_char_ui(int x, int y, char c, uint8_t fg, uint8_t bg) {
    const uint8_t *glyph = font_ui_get_glyph((uint8_t)c);
    for (int row = 0; row < FONT_UI_HEIGHT; row++) {
        uint8_t bits = glyph[row];
        for (int col = 0; col < FONT_UI_WIDTH; col++) {
            display_set_pixel(x + col, y + row,
                              (bits & (0x80 >> col)) ? fg : bg);
        }
    }
}

void gfx_text_ui(int x, int y, const char *str, uint8_t fg, uint8_t bg) {
    while (*str) {
        gfx_char_ui(x, y, *str, fg, bg);
        x += FONT_UI_WIDTH;
        str++;
    }
}

void gfx_text_ui_clipped(int x, int y, const char *str, uint8_t fg, uint8_t bg,
                          int cx, int cy, int cw, int ch) {
    int cx1 = cx + cw;
    int cy1 = cy + ch;
    while (*str) {
        if (x + FONT_UI_WIDTH > cx && x < cx1) {
            const uint8_t *glyph = font_ui_get_glyph((uint8_t)*str);
            for (int row = 0; row < FONT_UI_HEIGHT; row++) {
                int py = y + row;
                if (py < cy || py >= cy1) continue;
                uint8_t bits = glyph[row];
                for (int col = 0; col < FONT_UI_WIDTH; col++) {
                    int px = x + col;
                    if (px < cx || px >= cx1) continue;
                    display_set_pixel(px, py,
                                      (bits & (0x80 >> col)) ? fg : bg);
                }
            }
        }
        x += FONT_UI_WIDTH;
        str++;
    }
}

/*==========================================================================
 * UI font (8x12) — bold weight (separate font data)
 *
 * Uses the dedicated bold font array (font_ui_bold_8x12) rendered from
 * the W95font Bold variant, giving proper typographic bold weight.
 *=========================================================================*/

void gfx_char_ui_bold(int x, int y, char c, uint8_t fg, uint8_t bg) {
    const uint8_t *glyph = font_ui_bold_get_glyph((uint8_t)c);
    for (int row = 0; row < FONT_UI_HEIGHT; row++) {
        uint8_t bits = glyph[row];
        for (int col = 0; col < FONT_UI_WIDTH; col++) {
            display_set_pixel(x + col, y + row,
                              (bits & (0x80 >> col)) ? fg : bg);
        }
    }
}

void gfx_text_ui_bold(int x, int y, const char *str, uint8_t fg, uint8_t bg) {
    while (*str) {
        gfx_char_ui_bold(x, y, *str, fg, bg);
        x += FONT_UI_WIDTH;
        str++;
    }
}

void gfx_text_ui_bold_clipped(int x, int y, const char *str,
                               uint8_t fg, uint8_t bg,
                               int cx, int cy, int cw, int ch) {
    int cx1 = cx + cw;
    int cy1 = cy + ch;
    while (*str) {
        if (x + FONT_UI_WIDTH > cx && x < cx1) {
            const uint8_t *glyph = font_ui_bold_get_glyph((uint8_t)*str);
            for (int row = 0; row < FONT_UI_HEIGHT; row++) {
                int py = y + row;
                if (py < cy || py >= cy1) continue;
                uint8_t bits = glyph[row];
                for (int col = 0; col < FONT_UI_WIDTH; col++) {
                    int px = x + col;
                    if (px < cx || px >= cx1) continue;
                    display_set_pixel(px, py,
                                      (bits & (0x80 >> col)) ? fg : bg);
                }
            }
        }
        x += FONT_UI_WIDTH;
        str++;
    }
}

/*==========================================================================
 * 16x16 icon blitter
 *=========================================================================*/

void gfx_draw_icon_16(int sx, int sy, const uint8_t *icon_data) {
    for (int row = 0; row < 16; row++) {
        int py = sy + row;
        if (py < 0 || py >= FB_HEIGHT) continue;
        for (int col = 0; col < 16; col++) {
            int px = sx + col;
            if (px < 0 || px >= DISPLAY_WIDTH) continue;
            uint8_t c = icon_data[row * 16 + col];
            if (c != 0xFF)
                display_set_pixel(px, py, c);
        }
    }
}

void gfx_draw_icon_16_clipped(int sx, int sy, const uint8_t *icon_data,
                               int cx, int cy, int cw, int ch) {
    int cx1 = cx + cw;
    int cy1 = cy + ch;
    for (int row = 0; row < 16; row++) {
        int py = sy + row;
        if (py < cy || py >= cy1 || py < 0 || py >= FB_HEIGHT) continue;
        for (int col = 0; col < 16; col++) {
            int px = sx + col;
            if (px < cx || px >= cx1 || px < 0 || px >= DISPLAY_WIDTH) continue;
            uint8_t c = icon_data[row * 16 + col];
            if (c != 0xFF)
                display_set_pixel(px, py, c);
        }
    }
}

/*==========================================================================
 * 32x32 icon blitter
 *=========================================================================*/

void gfx_draw_icon_32(int sx, int sy, const uint8_t *icon_data) {
    for (int row = 0; row < 32; row++) {
        int py = sy + row;
        if (py < 0 || py >= FB_HEIGHT) continue;
        for (int col = 0; col < 32; col++) {
            int px = sx + col;
            if (px < 0 || px >= DISPLAY_WIDTH) continue;
            uint8_t c = icon_data[row * 32 + col];
            if (c != 0xFF)
                display_set_pixel(px, py, c);
        }
    }
}

void gfx_draw_icon_32_clipped(int sx, int sy, const uint8_t *icon_data,
                               int cx, int cy, int cw, int ch) {
    int cx1 = cx + cw;
    int cy1 = cy + ch;
    for (int row = 0; row < 32; row++) {
        int py = sy + row;
        if (py < cy || py >= cy1 || py < 0 || py >= FB_HEIGHT) continue;
        for (int col = 0; col < 32; col++) {
            int px = sx + col;
            if (px < cx || px >= cx1 || px < 0 || px >= DISPLAY_WIDTH) continue;
            uint8_t c = icon_data[row * 32 + col];
            if (c != 0xFF)
                display_set_pixel(px, py, c);
        }
    }
}
