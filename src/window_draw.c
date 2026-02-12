/*
 * FRANK OS
 * Copyright (c) 2025 Mikhail Matveev <xtreme@rh1.tech>
 * https://rh1.tech
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "window_draw.h"
#include "window_theme.h"
#include "gfx.h"
#include "display.h"
#include "font.h"

/*==========================================================================
 * Drawing context — tracks current window's clip rect and origin
 *=========================================================================*/

static struct {
    int16_t ox, oy;     /* origin offset (screen coords of client 0,0) */
    int16_t cw, ch;     /* client area size */
    bool    active;      /* true between wd_begin/wd_end */
} draw_ctx;

/*==========================================================================
 * Context management
 *=========================================================================*/

void wd_begin(hwnd_t hwnd) {
    window_t *win = wm_get_window(hwnd);
    if (!win) return;

    if (win->flags & WF_BORDER) {
        point_t origin = theme_client_origin(&win->frame, win->flags);
        rect_t client = theme_client_rect(&win->frame, win->flags);
        draw_ctx.ox = origin.x;
        draw_ctx.oy = origin.y;
        draw_ctx.cw = client.w;
        draw_ctx.ch = client.h;
    } else {
        draw_ctx.ox = win->frame.x;
        draw_ctx.oy = win->frame.y;
        draw_ctx.cw = win->frame.w;
        draw_ctx.ch = win->frame.h;
    }
    draw_ctx.active = true;
}

void wd_end(void) {
    draw_ctx.active = false;
}

/*==========================================================================
 * Primitives — clipped to client area
 *=========================================================================*/

void wd_pixel(int16_t x, int16_t y, uint8_t color) {
    if (!draw_ctx.active) return;
    if (x < 0 || x >= draw_ctx.cw || y < 0 || y >= draw_ctx.ch) return;
    display_set_pixel(draw_ctx.ox + x, draw_ctx.oy + y, color);
}

void wd_hline(int16_t x, int16_t y, int16_t w, uint8_t color) {
    if (!draw_ctx.active) return;
    if (y < 0 || y >= draw_ctx.ch) return;
    int16_t x0 = x < 0 ? 0 : x;
    int16_t x1 = (x + w) > draw_ctx.cw ? draw_ctx.cw : (x + w);
    if (x0 >= x1) return;
    display_hline_safe(draw_ctx.ox + x0, draw_ctx.oy + y, x1 - x0, color);
}

void wd_vline(int16_t x, int16_t y, int16_t h, uint8_t color) {
    if (!draw_ctx.active) return;
    if (x < 0 || x >= draw_ctx.cw) return;
    int16_t y0 = y < 0 ? 0 : y;
    int16_t y1 = (y + h) > draw_ctx.ch ? draw_ctx.ch : (y + h);
    for (int16_t py = y0; py < y1; py++)
        display_set_pixel(draw_ctx.ox + x, draw_ctx.oy + py, color);
}

void wd_rect(int16_t x, int16_t y, int16_t w, int16_t h, uint8_t color) {
    wd_hline(x, y, w, color);
    wd_hline(x, y + h - 1, w, color);
    wd_vline(x, y, h, color);
    wd_vline(x + w - 1, y, h, color);
}

void wd_fill_rect(int16_t x, int16_t y, int16_t w, int16_t h, uint8_t color) {
    if (!draw_ctx.active) return;
    int16_t x0 = x < 0 ? 0 : x;
    int16_t y0 = y < 0 ? 0 : y;
    int16_t x1 = (x + w) > draw_ctx.cw ? draw_ctx.cw : (x + w);
    int16_t y1 = (y + h) > draw_ctx.ch ? draw_ctx.ch : (y + h);
    if (x0 >= x1 || y0 >= y1) return;
    int sx = draw_ctx.ox + x0;
    int sy = draw_ctx.oy + y0;
    int span = x1 - x0;
    int rows = y1 - y0;
    if (sx < 0) { span += sx; sx = 0; }
    if (sx + span > DISPLAY_WIDTH) span = DISPLAY_WIDTH - sx;
    if (sy < 0) { rows += sy; sy = 0; }
    if (sy + rows > FB_HEIGHT) rows = FB_HEIGHT - sy;
    if (span <= 0 || rows <= 0) return;
    color &= 0x0F;
    for (int py = 0; py < rows; py++)
        display_hline_fast(sx, sy + py, span, color);
}

void wd_clear(uint8_t color) {
    wd_fill_rect(0, 0, draw_ctx.cw, draw_ctx.ch, color);
}

void wd_char(int16_t x, int16_t y, char c, uint8_t fg, uint8_t bg) {
    if (!draw_ctx.active) return;
    gfx_char_clipped(draw_ctx.ox + x, draw_ctx.oy + y, c, fg, bg,
                      draw_ctx.ox, draw_ctx.oy,
                      draw_ctx.cw, draw_ctx.ch);
}

void wd_text(int16_t x, int16_t y, const char *str, uint8_t fg, uint8_t bg) {
    while (*str) {
        if (x + FONT_WIDTH > 0 && x < draw_ctx.cw)
            wd_char(x, y, *str, fg, bg);
        x += FONT_WIDTH;
        str++;
    }
}

void wd_text_transparent(int16_t x, int16_t y, const char *str, uint8_t fg) {
    if (!draw_ctx.active) return;
    const uint8_t *glyph;
    int16_t cx1 = draw_ctx.cw;
    int16_t cy1 = draw_ctx.ch;
    while (*str) {
        if (x + FONT_WIDTH > 0 && x < cx1) {
            glyph = font_get_glyph(*str);
            for (int row = 0; row < FONT_HEIGHT; row++) {
                int16_t py = y + row;
                if (py < 0 || py >= cy1) continue;
                uint8_t bits = glyph[row];
                for (int col = 0; col < FONT_WIDTH; col++) {
                    int16_t px = x + col;
                    if (px < 0 || px >= cx1) continue;
                    if (bits & (1 << col))
                        display_set_pixel(draw_ctx.ox + px,
                                          draw_ctx.oy + py, fg);
                }
            }
        }
        x += FONT_WIDTH;
        str++;
    }
}

void wd_line(int16_t x0, int16_t y0, int16_t x1, int16_t y1, uint8_t color) {
    if (!draw_ctx.active) return;
    int16_t dx = x1 > x0 ? x1 - x0 : x0 - x1;
    int16_t dy = y1 > y0 ? y1 - y0 : y0 - y1;
    int16_t sx = x0 < x1 ? 1 : -1;
    int16_t sy = y0 < y1 ? 1 : -1;
    int16_t err = dx - dy;

    for (;;) {
        wd_pixel(x0, y0, color);
        if (x0 == x1 && y0 == y1) break;
        int16_t e2 = 2 * err;
        if (e2 > -dy) { err -= dy; x0 += sx; }
        if (e2 <  dx) { err += dx; y0 += sy; }
    }
}

void wd_bevel_rect(int16_t x, int16_t y, int16_t w, int16_t h,
                    uint8_t light, uint8_t dark, uint8_t face) {
    wd_fill_rect(x + 1, y + 1, w - 2, h - 2, face);
    wd_hline(x, y, w, light);
    wd_vline(x, y, h, light);
    wd_hline(x, y + h - 1, w, dark);
    wd_vline(x + w - 1, y, h, dark);
}

void wd_char_ui(int16_t x, int16_t y, char c, uint8_t fg, uint8_t bg) {
    if (!draw_ctx.active) return;
    char s[2] = { c, '\0' };
    gfx_text_ui_clipped(draw_ctx.ox + x, draw_ctx.oy + y, s, fg, bg,
                         draw_ctx.ox, draw_ctx.oy, draw_ctx.cw, draw_ctx.ch);
}

void wd_text_ui(int16_t x, int16_t y, const char *str, uint8_t fg, uint8_t bg) {
    if (!draw_ctx.active) return;
    gfx_text_ui_clipped(draw_ctx.ox + x, draw_ctx.oy + y, str, fg, bg,
                         draw_ctx.ox, draw_ctx.oy, draw_ctx.cw, draw_ctx.ch);
}

void wd_icon_16(int16_t x, int16_t y, const uint8_t *icon_data) {
    if (!draw_ctx.active || !icon_data) return;
    gfx_draw_icon_16_clipped(draw_ctx.ox + x, draw_ctx.oy + y, icon_data,
                              draw_ctx.ox, draw_ctx.oy, draw_ctx.cw, draw_ctx.ch);
}

void wd_icon_32(int16_t x, int16_t y, const uint8_t *icon_data) {
    if (!draw_ctx.active || !icon_data) return;
    gfx_draw_icon_32_clipped(draw_ctx.ox + x, draw_ctx.oy + y, icon_data,
                              draw_ctx.ox, draw_ctx.oy, draw_ctx.cw, draw_ctx.ch);
}
