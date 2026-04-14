/*
 * FRANK OS — Reusable UI Controls
 * Copyright (c) 2026 Mikhail Matveev <xtreme@rh1.tech>
 * https://rh1.tech
 *
 * Scrollbar and Textarea controls — struct-based, drawn within parent
 * window's client area using the wd_* drawing API.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "controls.h"
#include "window.h"
#include "window_event.h"
#include "window_draw.h"
#include "window_theme.h"
#include "clipboard.h"
#include "display.h"
#include "gfx.h"
#include "font.h"
#include <string.h>
#include <stdio.h>

/*==========================================================================
 * Scrollbar constants
 *=========================================================================*/

#define SB_W          SCROLLBAR_WIDTH
#define SB_MIN_THUMB  16

/*==========================================================================
 * Scrollbar — init
 *=========================================================================*/

void scrollbar_init(scrollbar_t *sb, bool horizontal) {
    memset(sb, 0, sizeof(*sb));
    sb->horizontal = horizontal;
    sb->visible = false;
    sb->dragging = false;
}

/*==========================================================================
 * Scrollbar — set range
 *=========================================================================*/

void scrollbar_set_range(scrollbar_t *sb, int32_t range, int32_t page) {
    sb->range = range;
    sb->page = page;
    sb->visible = (range > page);
    if (sb->pos > range - page) {
        sb->pos = range - page;
        if (sb->pos < 0) sb->pos = 0;
    }
}

/*==========================================================================
 * Scrollbar — set position
 *=========================================================================*/

void scrollbar_set_pos(scrollbar_t *sb, int32_t pos) {
    int32_t max_pos = sb->range - sb->page;
    if (max_pos < 0) max_pos = 0;
    if (pos < 0) pos = 0;
    if (pos > max_pos) pos = max_pos;
    sb->pos = pos;
}

/*==========================================================================
 * Scrollbar — internal helpers
 *=========================================================================*/

static void sb_get_track(const scrollbar_t *sb,
                          int16_t *track_start, int16_t *track_len) {
    if (sb->horizontal) {
        *track_start = sb->x + SB_W;
        *track_len = sb->w - 2 * SB_W;
    } else {
        *track_start = sb->y + SB_W;
        *track_len = sb->h - 2 * SB_W;
    }
    if (*track_len < 0) *track_len = 0;
}

static void sb_get_thumb(const scrollbar_t *sb,
                          int16_t track_start, int16_t track_len,
                          int16_t *thumb_pos, int16_t *thumb_len) {
    if (sb->range <= sb->page || track_len <= 0) {
        *thumb_pos = track_start;
        *thumb_len = track_len;
        return;
    }
    int32_t th = (int32_t)track_len * sb->page / sb->range;
    if (th < SB_MIN_THUMB) th = SB_MIN_THUMB;
    if (th > track_len) th = track_len;
    *thumb_len = (int16_t)th;

    int32_t max_pos = sb->range - sb->page;
    if (max_pos > 0) {
        int32_t tp = (int32_t)(track_len - th) * sb->pos / max_pos;
        *thumb_pos = track_start + (int16_t)tp;
    } else {
        *thumb_pos = track_start;
    }
}

/*==========================================================================
 * Scrollbar — arrow drawing
 *=========================================================================*/

static void sb_draw_arrow_up(int16_t bx, int16_t by) {
    int cx = bx + 7, cy = by + 5;
    wd_pixel(cx, cy, COLOR_BLACK);
    wd_hline(cx - 1, cy + 1, 3, COLOR_BLACK);
    wd_hline(cx - 2, cy + 2, 5, COLOR_BLACK);
    wd_hline(cx - 3, cy + 3, 7, COLOR_BLACK);
}

static void sb_draw_arrow_down(int16_t bx, int16_t by) {
    int cx = bx + 7, cy = by + 10;
    wd_pixel(cx, cy, COLOR_BLACK);
    wd_hline(cx - 1, cy - 1, 3, COLOR_BLACK);
    wd_hline(cx - 2, cy - 2, 5, COLOR_BLACK);
    wd_hline(cx - 3, cy - 3, 7, COLOR_BLACK);
}

static void sb_draw_arrow_left(int16_t bx, int16_t by) {
    int cx = bx + 5, cy = by + 7;
    wd_pixel(cx, cy, COLOR_BLACK);
    wd_vline(cx + 1, cy - 1, 3, COLOR_BLACK);
    wd_vline(cx + 2, cy - 2, 5, COLOR_BLACK);
    wd_vline(cx + 3, cy - 3, 7, COLOR_BLACK);
}

static void sb_draw_arrow_right(int16_t bx, int16_t by) {
    int cx = bx + 10, cy = by + 7;
    wd_pixel(cx, cy, COLOR_BLACK);
    wd_vline(cx - 1, cy - 1, 3, COLOR_BLACK);
    wd_vline(cx - 2, cy - 2, 5, COLOR_BLACK);
    wd_vline(cx - 3, cy - 3, 7, COLOR_BLACK);
}

static void sb_draw_button(int16_t x, int16_t y, int16_t w, int16_t h) {
    wd_fill_rect(x, y, w, h, THEME_BUTTON_FACE);
    wd_hline(x, y, w, COLOR_WHITE);
    wd_vline(x, y, h, COLOR_WHITE);
    wd_hline(x, y + h - 1, w, COLOR_BLACK);
    wd_vline(x + w - 1, y, h, COLOR_BLACK);
    wd_hline(x + 1, y + h - 2, w - 2, COLOR_DARK_GRAY);
    wd_vline(x + w - 2, y + 1, h - 2, COLOR_DARK_GRAY);
}

/*==========================================================================
 * Scrollbar — paint
 *=========================================================================*/

void scrollbar_paint(scrollbar_t *sb) {
    if (!sb->visible) return;

    int16_t track_start, track_len;
    sb_get_track(sb, &track_start, &track_len);

    /* Compute thumb position once so we can draw the track around it,
     * avoiding the flicker caused by overdrawing the thumb area. */
    int16_t thumb_pos = 0, thumb_len = 0;
    bool has_thumb = (sb->range > sb->page);
    if (has_thumb)
        sb_get_thumb(sb, track_start, track_len, &thumb_pos, &thumb_len);

    if (sb->horizontal) {
        /* Left arrow button */
        sb_draw_button(sb->x, sb->y, SB_W, SB_W);
        sb_draw_arrow_left(sb->x, sb->y);

        /* Right arrow button */
        sb_draw_button(sb->x + sb->w - SB_W, sb->y, SB_W, SB_W);
        sb_draw_arrow_right(sb->x + sb->w - SB_W, sb->y);

        /* Track — dithered (skip thumb region to avoid flicker) */
        int16_t t_rel = has_thumb ? thumb_pos - track_start : track_len;
        int16_t t_end = has_thumb ? t_rel + thumb_len : track_len;
        for (int16_t x = 0; x < track_len; x++) {
            if (has_thumb && x >= t_rel && x < t_end) continue;
            for (int16_t y = 0; y < SB_W; y++) {
                uint8_t c = ((x + y) & 1) ? COLOR_WHITE : COLOR_LIGHT_GRAY;
                wd_pixel(track_start + x, sb->y + y, c);
            }
        }

        /* Thumb */
        if (has_thumb)
            sb_draw_button(thumb_pos, sb->y, thumb_len, SB_W);
    } else {
        /* Up arrow button */
        sb_draw_button(sb->x, sb->y, SB_W, SB_W);
        sb_draw_arrow_up(sb->x, sb->y);

        /* Down arrow button */
        sb_draw_button(sb->x, sb->y + sb->h - SB_W, SB_W, SB_W);
        sb_draw_arrow_down(sb->x, sb->y + sb->h - SB_W);

        /* Track — dithered (skip thumb region to avoid flicker) */
        int16_t t_rel = has_thumb ? thumb_pos - track_start : track_len;
        int16_t t_end = has_thumb ? t_rel + thumb_len : track_len;
        for (int16_t y = 0; y < track_len; y++) {
            if (has_thumb && y >= t_rel && y < t_end) continue;
            for (int16_t x = 0; x < SB_W; x++) {
                uint8_t c = ((x + y) & 1) ? COLOR_WHITE : COLOR_LIGHT_GRAY;
                wd_pixel(sb->x + x, track_start + y, c);
            }
        }

        /* Thumb */
        if (has_thumb)
            sb_draw_button(sb->x, thumb_pos, SB_W, thumb_len);
    }
}

/*==========================================================================
 * Scrollbar — event (returns true if handled, sets *new_pos)
 *=========================================================================*/

bool scrollbar_event(scrollbar_t *sb, const window_event_t *event,
                     int32_t *new_pos) {
    if (!sb->visible) return false;

    int16_t mx, my;
    if (event->type == WM_LBUTTONDOWN || event->type == WM_LBUTTONUP ||
        event->type == WM_MOUSEMOVE) {
        mx = event->mouse.x;
        my = event->mouse.y;
    } else {
        return false;
    }

    /* Check if mouse is within scrollbar bounds */
    bool in_sb = (mx >= sb->x && mx < sb->x + sb->w &&
                  my >= sb->y && my < sb->y + sb->h);

    if (event->type == WM_LBUTTONDOWN && in_sb) {
        int16_t track_start, track_len;
        sb_get_track(sb, &track_start, &track_len);

        int16_t coord = sb->horizontal ? mx : my;
        int16_t sb_start = sb->horizontal ? sb->x : sb->y;
        int16_t sb_end = sb->horizontal ? (sb->x + sb->w) : (sb->y + sb->h);

        /* Arrow button 1 (up/left) */
        if (coord < sb_start + SB_W) {
            int32_t step = sb->horizontal ? FONT_UI_WIDTH : FONT_UI_HEIGHT;
            scrollbar_set_pos(sb, sb->pos - step);
            *new_pos = sb->pos;
            return true;
        }

        /* Arrow button 2 (down/right) */
        if (coord >= sb_end - SB_W) {
            int32_t step = sb->horizontal ? FONT_UI_WIDTH : FONT_UI_HEIGHT;
            scrollbar_set_pos(sb, sb->pos + step);
            *new_pos = sb->pos;
            return true;
        }

        /* Track or thumb area */
        int16_t thumb_pos, thumb_len;
        sb_get_thumb(sb, track_start, track_len, &thumb_pos, &thumb_len);

        if (coord >= thumb_pos && coord < thumb_pos + thumb_len) {
            /* Start thumb drag — position unchanged */
            sb->dragging = true;
            sb->drag_offset = coord - thumb_pos;
            *new_pos = sb->pos;
            return true;
        }

        /* Track click — page scroll */
        if (coord < thumb_pos) {
            scrollbar_set_pos(sb, sb->pos - sb->page);
        } else {
            scrollbar_set_pos(sb, sb->pos + sb->page);
        }
        *new_pos = sb->pos;
        return true;
    }

    if (event->type == WM_MOUSEMOVE && sb->dragging) {
        int16_t track_start, track_len;
        sb_get_track(sb, &track_start, &track_len);

        int16_t thumb_pos, thumb_len;
        sb_get_thumb(sb, track_start, track_len, &thumb_pos, &thumb_len);

        int16_t coord = sb->horizontal ? mx : my;
        int16_t new_thumb_top = coord - sb->drag_offset;
        int16_t usable = track_len - thumb_len;
        if (usable > 0) {
            int32_t max_pos = sb->range - sb->page;
            int32_t ratio_pos = (int32_t)(new_thumb_top - track_start) * max_pos / usable;
            scrollbar_set_pos(sb, ratio_pos);
        }
        *new_pos = sb->pos;
        return true;
    }

    if (event->type == WM_LBUTTONUP && sb->dragging) {
        sb->dragging = false;
        *new_pos = sb->pos;
        return true;
    }

    return false;
}

/*==========================================================================
 * Textarea — internal helpers
 *=========================================================================*/

/* Count total lines and find max line width in the buffer */
static void ta_scan_lines(textarea_t *ta) {
    int32_t lines = 1;
    int32_t max_w = 0;
    int32_t cur_w = 0;

    for (int32_t i = 0; i < ta->len; i++) {
        if (ta->buf[i] == '\n') {
            if (cur_w > max_w) max_w = cur_w;
            cur_w = 0;
            lines++;
        } else {
            cur_w++;
        }
    }
    if (cur_w > max_w) max_w = cur_w;

    ta->total_lines = lines;
    ta->max_line_width = max_w;
}

/* Get the visible text area (excluding scrollbars) */
static void ta_get_text_rect(const textarea_t *ta,
                               int16_t *tx, int16_t *ty,
                               int16_t *tw, int16_t *th) {
    *tx = ta->rect_x;
    *ty = ta->rect_y;
    *tw = ta->rect_w;
    *th = ta->rect_h;

    if (ta->vscroll.visible)
        *tw -= SB_W;
    if (ta->hscroll.visible)
        *th -= SB_W;
}

/* Get line number and column from byte offset */
/* UTF-8 helpers for cursor movement */
static inline bool ta_is_utf8_cont(char c) {
    return ((uint8_t)c & 0xC0) == 0x80;
}

/* Move cursor forward by one UTF-8 character */
static inline int32_t ta_next_char(const textarea_t *ta, int32_t pos) {
    if (pos >= ta->len) return pos;
    pos++;
    while (pos < ta->len && ta_is_utf8_cont(ta->buf[pos])) pos++;
    return pos;
}

/* Move cursor backward by one UTF-8 character */
static inline int32_t ta_prev_char(const textarea_t *ta, int32_t pos) {
    if (pos <= 0) return 0;
    pos--;
    while (pos > 0 && ta_is_utf8_cont(ta->buf[pos])) pos--;
    return pos;
}

static void ta_offset_to_lc(const textarea_t *ta, int32_t offset,
                              int32_t *line, int32_t *col) {
    int32_t l = 0, c = 0;
    for (int32_t i = 0; i < offset && i < ta->len; i++) {
        if (ta->buf[i] == '\n') {
            l++;
            c = 0;
        } else if (!ta_is_utf8_cont(ta->buf[i])) {
            c++;
        }
    }
    *line = l;
    *col = c;
}

/* Get byte offset from line number and column */
static int32_t ta_lc_to_offset(const textarea_t *ta,
                                 int32_t line, int32_t col) {
    int32_t l = 0;
    int32_t i = 0;

    /* Find start of target line */
    while (i < ta->len && l < line) {
        if (ta->buf[i] == '\n') l++;
        i++;
    }

    /* Advance by col, but clamp to end of line */
    int32_t start = i;
    while (i < ta->len && ta->buf[i] != '\n' && (i - start) < col) {
        i++;
    }
    return i;
}

/* Get the length of the line (in chars) at a given byte offset */
static int32_t ta_line_len_at(const textarea_t *ta, int32_t line_start) {
    int32_t len = 0;
    int32_t i = line_start;
    while (i < ta->len && ta->buf[i] != '\n') {
        len++;
        i++;
    }
    return len;
}

/* Get byte offset of line start from line number */
static int32_t ta_line_start(const textarea_t *ta, int32_t line) {
    int32_t l = 0;
    int32_t i = 0;
    while (i < ta->len && l < line) {
        if (ta->buf[i] == '\n') l++;
        i++;
    }
    return i;
}

/* Get selection range (ordered: start <= end) */
static void ta_get_sel(const textarea_t *ta, int32_t *start, int32_t *end) {
    if (ta->sel_anchor < 0) {
        *start = ta->cursor;
        *end = ta->cursor;
    } else if (ta->sel_anchor < ta->cursor) {
        *start = ta->sel_anchor;
        *end = ta->cursor;
    } else {
        *start = ta->cursor;
        *end = ta->sel_anchor;
    }
}

/* Delete selected text, returns true if there was a selection */
static bool ta_delete_sel(textarea_t *ta) {
    int32_t s, e;
    ta_get_sel(ta, &s, &e);
    if (s == e) return false;

    int32_t del = e - s;
    memmove(ta->buf + s, ta->buf + e, ta->len - e);
    ta->len -= del;
    ta->buf[ta->len] = '\0';
    ta->cursor = s;
    ta->sel_anchor = -1;
    return true;
}

/* Insert text at cursor */
static bool ta_insert(textarea_t *ta, const char *text, int32_t text_len) {
    if (ta->len + text_len >= ta->buf_size) return false;
    memmove(ta->buf + ta->cursor + text_len,
            ta->buf + ta->cursor,
            ta->len - ta->cursor);
    memcpy(ta->buf + ta->cursor, text, text_len);
    ta->len += text_len;
    ta->cursor += text_len;
    ta->buf[ta->len] = '\0';
    return true;
}

/* Update scrollbar ranges based on content */
static void ta_update_scrollbars(textarea_t *ta) {
    int16_t tx, ty, tw, th;

    /* First pass: compute text rect assuming no scrollbars */
    tw = ta->rect_w;
    th = ta->rect_h;

    int32_t content_h = ta->total_lines * FONT_UI_HEIGHT;
    int32_t content_w = ta->max_line_width * FONT_UI_WIDTH;

    /* Determine which scrollbars are needed */
    bool need_v = content_h > th;
    bool need_h = content_w > tw;

    /* Second pass: if one scrollbar is needed, check if the other is too */
    if (need_v && !need_h) {
        need_h = content_w > (tw - SB_W);
    }
    if (need_h && !need_v) {
        need_v = content_h > (th - SB_W);
    }

    /* Get actual text rect */
    int16_t final_tw = ta->rect_w - (need_v ? SB_W : 0);
    int16_t final_th = ta->rect_h - (need_h ? SB_W : 0);

    /* Vertical scrollbar */
    ta->vscroll.x = ta->rect_x + ta->rect_w - SB_W;
    ta->vscroll.y = ta->rect_y;
    ta->vscroll.w = SB_W;
    ta->vscroll.h = final_th;
    scrollbar_set_range(&ta->vscroll, content_h, final_th);
    ta->vscroll.visible = need_v;

    /* Horizontal scrollbar */
    ta->hscroll.x = ta->rect_x;
    ta->hscroll.y = ta->rect_y + ta->rect_h - SB_W;
    ta->hscroll.w = final_tw;
    ta->hscroll.h = SB_W;
    scrollbar_set_range(&ta->hscroll, content_w, final_tw);
    ta->hscroll.visible = need_h;
}

/* Ensure cursor is visible by adjusting scroll */
static void ta_ensure_visible(textarea_t *ta) {
    int32_t line, col;
    ta_offset_to_lc(ta, ta->cursor, &line, &col);

    int16_t tx, ty, tw, th;
    ta_get_text_rect(ta, &tx, &ty, &tw, &th);

    int32_t cy = line * FONT_UI_HEIGHT;
    int32_t cx = col * FONT_UI_WIDTH;

    /* Vertical */
    if (cy < ta->scroll_y) {
        ta->scroll_y = cy;
    } else if (cy + FONT_UI_HEIGHT > ta->scroll_y + th) {
        ta->scroll_y = cy + FONT_UI_HEIGHT - th;
    }
    if (ta->scroll_y < 0) ta->scroll_y = 0;

    /* Horizontal */
    if (cx < ta->scroll_x) {
        ta->scroll_x = cx;
    } else if (cx + FONT_UI_WIDTH > ta->scroll_x + tw) {
        ta->scroll_x = cx + FONT_UI_WIDTH - tw;
    }
    if (ta->scroll_x < 0) ta->scroll_x = 0;

    scrollbar_set_pos(&ta->vscroll, ta->scroll_y);
    scrollbar_set_pos(&ta->hscroll, ta->scroll_x);
}

/* Case-insensitive character comparison */
static char ta_lower(char c) {
    if (c >= 'A' && c <= 'Z') return c + 32;
    return c;
}

/*==========================================================================
 * Textarea — init
 *=========================================================================*/

void textarea_init(textarea_t *ta, char *buf, int32_t buf_size, hwnd_t hwnd) {
    memset(ta, 0, sizeof(*ta));
    ta->buf = buf;
    ta->buf_size = buf_size;
    ta->hwnd = hwnd;
    ta->sel_anchor = -1;
    ta->cursor_visible = true;
    ta->buf[0] = '\0';

    scrollbar_init(&ta->vscroll, false);
    scrollbar_init(&ta->hscroll, true);
}

/*==========================================================================
 * Textarea — set text
 *=========================================================================*/

void textarea_set_text(textarea_t *ta, const char *text, int32_t len) {
    if (len >= ta->buf_size) len = ta->buf_size - 1;
    memcpy(ta->buf, text, len);
    ta->buf[len] = '\0';
    ta->len = len;
    ta->cursor = 0;
    ta->sel_anchor = -1;
    ta->scroll_x = 0;
    ta->scroll_y = 0;

    ta_scan_lines(ta);
    ta_update_scrollbars(ta);
}

/*==========================================================================
 * Textarea — get text / length
 *=========================================================================*/

const char *textarea_get_text(textarea_t *ta) {
    return ta->buf;
}

int32_t textarea_get_length(textarea_t *ta) {
    return ta->len;
}

/*==========================================================================
 * Textarea — set rect
 *=========================================================================*/

void textarea_set_rect(textarea_t *ta, int16_t x, int16_t y,
                        int16_t w, int16_t h) {
    ta->rect_x = x;
    ta->rect_y = y;
    ta->rect_w = w;
    ta->rect_h = h;

    ta_update_scrollbars(ta);
}

/*==========================================================================
 * Textarea — paint
 *=========================================================================*/

void textarea_paint(textarea_t *ta) {
    int16_t tx, ty, tw, th;
    ta_get_text_rect(ta, &tx, &ty, &tw, &th);

    /* White background */
    wd_fill_rect(tx, ty, tw, th, COLOR_WHITE);

    /* Selection range */
    int32_t sel_s, sel_e;
    ta_get_sel(ta, &sel_s, &sel_e);

    /* Visible line range */
    int32_t first_line = ta->scroll_y / FONT_UI_HEIGHT;
    int32_t visible_lines = th / FONT_UI_HEIGHT + 2;

    /* Walk to first visible line */
    int32_t offset = 0;
    int32_t cur_line = 0;
    while (offset < ta->len && cur_line < first_line) {
        if (ta->buf[offset] == '\n') cur_line++;
        offset++;
    }

    /* Draw visible lines */
    for (int32_t vl = 0; vl < visible_lines && offset <= ta->len; vl++) {
        int32_t line_num = first_line + vl;
        int32_t py = ty + line_num * FONT_UI_HEIGHT - ta->scroll_y;

        if (py >= ty + th) break;
        if (py < ty) {
            /* Line starts above text rect — skip to avoid drawing
             * into the uncleared margin area above the textarea. */
            while (offset < ta->len && ta->buf[offset] != '\n') offset++;
            if (offset < ta->len) offset++; /* skip \n */
            continue;
        }

        /* Find end of line */
        int32_t line_start = offset;
        int32_t line_end = line_start;
        while (line_end < ta->len && ta->buf[line_end] != '\n') line_end++;

        /* Draw characters of this line (UTF-8 aware) */
        int32_t col = 0;
        int32_t i = line_start;
        while (i < line_end) {
            int32_t px = tx + col * FONT_UI_WIDTH - ta->scroll_x;

            /* Determine UTF-8 character byte length */
            uint8_t b0 = (uint8_t)ta->buf[i];
            int32_t clen = 1;
            if (b0 >= 0xF0 && i + 3 <= line_end) clen = 4;
            else if (b0 >= 0xE0 && i + 2 <= line_end) clen = 3;
            else if (b0 >= 0xC0 && i + 1 <= line_end) clen = 2;

            if (px + FONT_UI_WIDTH <= tx) { i += clen; col++; continue; }
            if (px >= tx + tw) break;

            bool in_sel = (sel_s != sel_e && i >= sel_s && i < sel_e);
            uint8_t fg = in_sel ? COLOR_WHITE : COLOR_BLACK;
            uint8_t bg = in_sel ? COLOR_BLUE : COLOR_WHITE;

            if (clen == 1) {
                wd_char_ui(px, py, ta->buf[i], fg, bg);
            } else {
                char tmp[5];
                int32_t j;
                for (j = 0; j < clen && j < 4; j++) tmp[j] = ta->buf[i + j];
                tmp[j] = '\0';
                wd_fill_rect(px, py, FONT_UI_WIDTH, FONT_UI_HEIGHT, bg);
                wd_text_ui(px, py, tmp, fg, bg);
            }
            i += clen;
            col++;
        }

        /* Draw selection highlight for the remainder of a selected line */
        if (sel_s != sel_e) {
            /* If newline at line_end is within selection, highlight to end */
            if (line_end >= sel_s && line_end < sel_e && line_end < ta->len) {
                int32_t end_px = tx + col * FONT_UI_WIDTH - ta->scroll_x;
                if (end_px < tx + tw) {
                    wd_fill_rect(end_px, py,
                                  FONT_UI_WIDTH, FONT_UI_HEIGHT, COLOR_BLUE);
                }
            }
        }

        /* Advance past newline */
        offset = line_end;
        if (offset < ta->len) offset++; /* skip \n */
    }

    /* Draw cursor */
    if (ta->cursor_visible) {
        int32_t cline, ccol;
        ta_offset_to_lc(ta, ta->cursor, &cline, &ccol);
        int32_t cx = tx + ccol * FONT_UI_WIDTH - ta->scroll_x;
        int32_t cy = ty + cline * FONT_UI_HEIGHT - ta->scroll_y;

        if (cx >= tx && cx < tx + tw && cy >= ty && cy + FONT_UI_HEIGHT <= ty + th) {
            for (int r = 0; r < FONT_UI_HEIGHT; r++) {
                wd_pixel(cx, cy + r, COLOR_BLACK);
            }
        }
    }

    /* Draw scrollbars */
    scrollbar_paint(&ta->vscroll);
    scrollbar_paint(&ta->hscroll);

    /* Corner box if both scrollbars visible */
    if (ta->vscroll.visible && ta->hscroll.visible) {
        wd_fill_rect(ta->rect_x + ta->rect_w - SB_W,
                      ta->rect_y + ta->rect_h - SB_W,
                      SB_W, SB_W, THEME_BUTTON_FACE);
    }
}

/*==========================================================================
 * Textarea — keyboard event handling
 *=========================================================================*/

static bool ta_key_event(textarea_t *ta, const window_event_t *event) {
    uint8_t sc = event->key.scancode;
    uint8_t mod = event->key.modifiers;
    bool shift = (mod & KMOD_SHIFT) != 0;
    bool ctrl = (mod & KMOD_CTRL) != 0;

    /* Ctrl+A — select all */
    if (ctrl && sc == 0x04) { /* HID 'a' */
        textarea_select_all(ta);
        return true;
    }

    /* Ctrl+C — copy */
    if (ctrl && sc == 0x06) { /* HID 'c' */
        textarea_copy(ta);
        return true;
    }

    /* Ctrl+X — cut */
    if (ctrl && sc == 0x1B) { /* HID 'x' */
        textarea_cut(ta);
        return true;
    }

    /* Ctrl+V — paste */
    if (ctrl && sc == 0x19) { /* HID 'v' */
        textarea_paste(ta);
        return true;
    }

    /* Arrow keys */
    if (sc == 0x50) { /* Left */
        if (!shift && ta->sel_anchor >= 0) {
            int32_t s, e;
            ta_get_sel(ta, &s, &e);
            ta->cursor = s;
            ta->sel_anchor = -1;
        } else {
            if (shift && ta->sel_anchor < 0)
                ta->sel_anchor = ta->cursor;
            if (ta->cursor > 0) {
                if (ctrl) {
                    /* Word left */
                    ta->cursor = ta_prev_char(ta, ta->cursor);
                    while (ta->cursor > 0 && ta->buf[ta->cursor - 1] != ' ' &&
                           ta->buf[ta->cursor - 1] != '\n')
                        ta->cursor = ta_prev_char(ta, ta->cursor);
                } else {
                    ta->cursor = ta_prev_char(ta, ta->cursor);
                }
            }
            if (!shift) ta->sel_anchor = -1;
        }
        ta_ensure_visible(ta);
        wm_invalidate(ta->hwnd);
        return true;
    }

    if (sc == 0x4F) { /* Right */
        if (!shift && ta->sel_anchor >= 0) {
            int32_t s, e;
            ta_get_sel(ta, &s, &e);
            ta->cursor = e;
            ta->sel_anchor = -1;
        } else {
            if (shift && ta->sel_anchor < 0)
                ta->sel_anchor = ta->cursor;
            if (ta->cursor < ta->len) {
                if (ctrl) {
                    /* Word right */
                    ta->cursor = ta_next_char(ta, ta->cursor);
                    while (ta->cursor < ta->len &&
                           ta->buf[ta->cursor] != ' ' &&
                           ta->buf[ta->cursor] != '\n')
                        ta->cursor = ta_next_char(ta, ta->cursor);
                } else {
                    ta->cursor = ta_next_char(ta, ta->cursor);
                }
            }
            if (!shift) ta->sel_anchor = -1;
        }
        ta_ensure_visible(ta);
        wm_invalidate(ta->hwnd);
        return true;
    }

    if (sc == 0x52) { /* Up */
        if (shift && ta->sel_anchor < 0)
            ta->sel_anchor = ta->cursor;
        int32_t line, col;
        ta_offset_to_lc(ta, ta->cursor, &line, &col);
        if (line > 0)
            ta->cursor = ta_lc_to_offset(ta, line - 1, col);
        if (!shift) ta->sel_anchor = -1;
        ta_ensure_visible(ta);
        wm_invalidate(ta->hwnd);
        return true;
    }

    if (sc == 0x51) { /* Down */
        if (shift && ta->sel_anchor < 0)
            ta->sel_anchor = ta->cursor;
        int32_t line, col;
        ta_offset_to_lc(ta, ta->cursor, &line, &col);
        if (line < ta->total_lines - 1)
            ta->cursor = ta_lc_to_offset(ta, line + 1, col);
        if (!shift) ta->sel_anchor = -1;
        ta_ensure_visible(ta);
        wm_invalidate(ta->hwnd);
        return true;
    }

    /* Home */
    if (sc == 0x4A) {
        if (shift && ta->sel_anchor < 0)
            ta->sel_anchor = ta->cursor;
        if (ctrl) {
            ta->cursor = 0;
        } else {
            int32_t line, col;
            ta_offset_to_lc(ta, ta->cursor, &line, &col);
            ta->cursor = ta_line_start(ta, line);
        }
        if (!shift) ta->sel_anchor = -1;
        ta_ensure_visible(ta);
        wm_invalidate(ta->hwnd);
        return true;
    }

    /* End */
    if (sc == 0x4D) {
        if (shift && ta->sel_anchor < 0)
            ta->sel_anchor = ta->cursor;
        if (ctrl) {
            ta->cursor = ta->len;
        } else {
            int32_t line, col;
            ta_offset_to_lc(ta, ta->cursor, &line, &col);
            int32_t ls = ta_line_start(ta, line);
            ta->cursor = ls + ta_line_len_at(ta, ls);
        }
        if (!shift) ta->sel_anchor = -1;
        ta_ensure_visible(ta);
        wm_invalidate(ta->hwnd);
        return true;
    }

    /* Page Up */
    if (sc == 0x4B) {
        if (shift && ta->sel_anchor < 0)
            ta->sel_anchor = ta->cursor;
        int16_t tx, ty, tw, th;
        ta_get_text_rect(ta, &tx, &ty, &tw, &th);
        int32_t page_lines = th / FONT_UI_HEIGHT;
        int32_t line, col;
        ta_offset_to_lc(ta, ta->cursor, &line, &col);
        line -= page_lines;
        if (line < 0) line = 0;
        ta->cursor = ta_lc_to_offset(ta, line, col);
        if (!shift) ta->sel_anchor = -1;
        ta_ensure_visible(ta);
        wm_invalidate(ta->hwnd);
        return true;
    }

    /* Page Down */
    if (sc == 0x4E) {
        if (shift && ta->sel_anchor < 0)
            ta->sel_anchor = ta->cursor;
        int16_t tx, ty, tw, th;
        ta_get_text_rect(ta, &tx, &ty, &tw, &th);
        int32_t page_lines = th / FONT_UI_HEIGHT;
        int32_t line, col;
        ta_offset_to_lc(ta, ta->cursor, &line, &col);
        line += page_lines;
        if (line >= ta->total_lines) line = ta->total_lines - 1;
        ta->cursor = ta_lc_to_offset(ta, line, col);
        if (!shift) ta->sel_anchor = -1;
        ta_ensure_visible(ta);
        wm_invalidate(ta->hwnd);
        return true;
    }

    /* Backspace */
    if (sc == 0x2A) {
        if (ta_delete_sel(ta)) {
            ta_scan_lines(ta);
            ta_update_scrollbars(ta);
            ta_ensure_visible(ta);
            wm_invalidate(ta->hwnd);
            return true;
        }
        if (ta->cursor > 0) {
            /* Delete one UTF-8 character backward */
            int32_t prev = ta_prev_char(ta, ta->cursor);
            int32_t del_len = ta->cursor - prev;
            memmove(ta->buf + prev,
                    ta->buf + ta->cursor,
                    ta->len - ta->cursor);
            ta->len -= del_len;
            ta->cursor = prev;
            ta->buf[ta->len] = '\0';
            ta_scan_lines(ta);
            ta_update_scrollbars(ta);
            ta_ensure_visible(ta);
            wm_invalidate(ta->hwnd);
        }
        return true;
    }

    /* Delete */
    if (sc == 0x4C) {
        if (ta_delete_sel(ta)) {
            ta_scan_lines(ta);
            ta_update_scrollbars(ta);
            ta_ensure_visible(ta);
            wm_invalidate(ta->hwnd);
            return true;
        }
        if (ta->cursor < ta->len) {
            /* Delete one UTF-8 character forward */
            int32_t next = ta_next_char(ta, ta->cursor);
            int32_t del_len = next - ta->cursor;
            memmove(ta->buf + ta->cursor,
                    ta->buf + next,
                    ta->len - next);
            ta->len -= del_len;
            ta->buf[ta->len] = '\0';
            ta_scan_lines(ta);
            ta_update_scrollbars(ta);
            wm_invalidate(ta->hwnd);
        }
        return true;
    }

    /* Enter — insert newline */
    if (sc == 0x28) {
        ta_delete_sel(ta);
        if (ta_insert(ta, "\n", 1)) {
            ta_scan_lines(ta);
            ta_update_scrollbars(ta);
            ta_ensure_visible(ta);
            wm_invalidate(ta->hwnd);
        }
        return true;
    }

    /* Tab — insert tab (4 spaces) */
    if (sc == 0x2B) {
        ta_delete_sel(ta);
        if (ta_insert(ta, "    ", 4)) {
            ta_scan_lines(ta);
            ta_update_scrollbars(ta);
            ta_ensure_visible(ta);
            wm_invalidate(ta->hwnd);
        }
        return true;
    }

    return false;
}

/*==========================================================================
 * Textarea — char event (printable characters)
 *=========================================================================*/

static bool ta_char_event(textarea_t *ta, const window_event_t *event) {
    char ch = event->charev.ch;

    /* Ignore control characters (handled by WM_KEYDOWN) */
    if (ch < 0x20 || ch == 0x7F) return false;

    /* Ctrl held — already handled by ta_key_event */
    if (event->charev.modifiers & KMOD_CTRL) return false;

    ta_delete_sel(ta);
    if (ta_insert(ta, &ch, 1)) {
        ta_scan_lines(ta);
        ta_update_scrollbars(ta);
        ta_ensure_visible(ta);
        wm_invalidate(ta->hwnd);
    }
    return true;
}

/*==========================================================================
 * Textarea — mouse event handling
 *=========================================================================*/

static bool ta_mouse_event(textarea_t *ta, const window_event_t *event) {
    int16_t mx = event->mouse.x;
    int16_t my = event->mouse.y;

    /* Forward to scrollbars first */
    int32_t new_pos;
    if (scrollbar_event(&ta->vscroll, event, &new_pos)) {
        ta->scroll_y = new_pos;
        wm_invalidate(ta->hwnd);
        return true;
    }
    if (scrollbar_event(&ta->hscroll, event, &new_pos)) {
        ta->scroll_x = new_pos;
        wm_invalidate(ta->hwnd);
        return true;
    }

    /* Text area hit test */
    int16_t tx, ty, tw, th;
    ta_get_text_rect(ta, &tx, &ty, &tw, &th);

    if (mx < tx || mx >= tx + tw || my < ty || my >= ty + th)
        return false;

    if (event->type == WM_LBUTTONDOWN) {
        bool shift = (event->mouse.modifiers & KMOD_SHIFT) != 0;

        /* Calculate line and column from mouse position */
        int32_t click_line = (my - ty + ta->scroll_y) / FONT_UI_HEIGHT;
        int32_t click_col = (mx - tx + ta->scroll_x) / FONT_UI_WIDTH;
        if (click_line < 0) click_line = 0;
        if (click_col < 0) click_col = 0;
        if (click_line >= ta->total_lines) click_line = ta->total_lines - 1;

        /* Clamp column to line length */
        int32_t ls = ta_line_start(ta, click_line);
        int32_t ll = ta_line_len_at(ta, ls);
        if (click_col > ll) click_col = ll;

        int32_t new_cursor = ls + click_col;

        if (shift) {
            if (ta->sel_anchor < 0)
                ta->sel_anchor = ta->cursor;
            ta->cursor = new_cursor;
        } else {
            ta->cursor = new_cursor;
            ta->sel_anchor = new_cursor; /* Will become selection start on drag */
        }

        ta->cursor_visible = true;
        wm_invalidate(ta->hwnd);
        return true;
    }

    if (event->type == WM_MOUSEMOVE && (event->mouse.buttons & 1)) {
        /* Drag selection — WM_MOUSEMOVE is always sent before
         * WM_LBUTTONDOWN in the same PS/2 poll (main.c line 489),
         * so the first drag MOUSEMOVE may arrive before LBUTTONDOWN
         * has set sel_anchor.  Seed it from the current cursor. */
        if (ta->sel_anchor < 0)
            ta->sel_anchor = ta->cursor;

        int32_t click_line = (my - ty + ta->scroll_y) / FONT_UI_HEIGHT;
        int32_t click_col = (mx - tx + ta->scroll_x) / FONT_UI_WIDTH;
        if (click_line < 0) click_line = 0;
        if (click_col < 0) click_col = 0;
        if (click_line >= ta->total_lines) click_line = ta->total_lines - 1;

        int32_t ls = ta_line_start(ta, click_line);
        int32_t ll = ta_line_len_at(ta, ls);
        if (click_col > ll) click_col = ll;

        ta->cursor = ls + click_col;

        ta->cursor_visible = true;
        wm_invalidate(ta->hwnd);
        return true;
    }

    if (event->type == WM_LBUTTONUP) {
        /* If no drag happened, clear selection */
        if (ta->sel_anchor >= 0 && ta->sel_anchor == ta->cursor)
            ta->sel_anchor = -1;
        return true;
    }

    return false;
}

/*==========================================================================
 * Textarea — event (main dispatch)
 *=========================================================================*/

bool textarea_event(textarea_t *ta, const window_event_t *event) {
    switch (event->type) {
    case WM_KEYDOWN:
        return ta_key_event(ta, event);
    case WM_CHAR:
        return ta_char_event(ta, event);
    case WM_LBUTTONDOWN:
    case WM_LBUTTONUP:
    case WM_MOUSEMOVE:
        return ta_mouse_event(ta, event);
    default:
        return false;
    }
}

/*==========================================================================
 * Textarea — clipboard operations
 *=========================================================================*/

void textarea_copy(textarea_t *ta) {
    int32_t s, e;
    ta_get_sel(ta, &s, &e);
    if (s == e) return;
    clipboard_set_text(ta->buf + s, (uint16_t)(e - s));
}

void textarea_cut(textarea_t *ta) {
    textarea_copy(ta);
    ta_delete_sel(ta);
    ta_scan_lines(ta);
    ta_update_scrollbars(ta);
    ta_ensure_visible(ta);
    wm_invalidate(ta->hwnd);
}

void textarea_paste(textarea_t *ta) {
    uint16_t clip_len = clipboard_get_length();
    if (clip_len == 0) return;

    ta_delete_sel(ta);
    const char *text = clipboard_get_text();
    if (ta_insert(ta, text, clip_len)) {
        ta_scan_lines(ta);
        ta_update_scrollbars(ta);
        ta_ensure_visible(ta);
        wm_invalidate(ta->hwnd);
    }
}

/*==========================================================================
 * Textarea — select all
 *=========================================================================*/

void textarea_select_all(textarea_t *ta) {
    ta->sel_anchor = 0;
    ta->cursor = ta->len;
    wm_invalidate(ta->hwnd);
}

/*==========================================================================
 * Textarea — find
 *=========================================================================*/

bool textarea_find(textarea_t *ta, const char *needle, bool case_sensitive,
                    bool forward) {
    int32_t needle_len = (int32_t)strlen(needle);
    if (needle_len == 0) return false;

    if (forward) {
        /* Search from cursor+1 to end, then wrap to beginning */
        int32_t start = ta->cursor + 1;
        if (start > ta->len) start = 0;

        for (int32_t pass = 0; pass < 2; pass++) {
            int32_t from = (pass == 0) ? start : 0;
            int32_t to = (pass == 0) ? ta->len : start;

            for (int32_t i = from; i <= to - needle_len; i++) {
                bool match = true;
                for (int32_t j = 0; j < needle_len; j++) {
                    char a = ta->buf[i + j];
                    char b = needle[j];
                    if (!case_sensitive) { a = ta_lower(a); b = ta_lower(b); }
                    if (a != b) { match = false; break; }
                }
                if (match) {
                    ta->sel_anchor = i;
                    ta->cursor = i + needle_len;
                    ta_ensure_visible(ta);
                    wm_invalidate(ta->hwnd);
                    return true;
                }
            }
        }
    } else {
        /* Search backwards from cursor-1 */
        int32_t start = ta->cursor - 1;
        if (start < 0) start = ta->len - needle_len;

        for (int32_t pass = 0; pass < 2; pass++) {
            int32_t from = (pass == 0) ? start : ta->len - needle_len;
            int32_t to = (pass == 0) ? 0 : start;

            for (int32_t i = from; i >= to; i--) {
                if (i + needle_len > ta->len) continue;
                bool match = true;
                for (int32_t j = 0; j < needle_len; j++) {
                    char a = ta->buf[i + j];
                    char b = needle[j];
                    if (!case_sensitive) { a = ta_lower(a); b = ta_lower(b); }
                    if (a != b) { match = false; break; }
                }
                if (match) {
                    ta->sel_anchor = i;
                    ta->cursor = i + needle_len;
                    ta_ensure_visible(ta);
                    wm_invalidate(ta->hwnd);
                    return true;
                }
            }
        }
    }

    return false;
}

/*==========================================================================
 * Textarea — replace
 *=========================================================================*/

bool textarea_replace(textarea_t *ta, const char *needle,
                       const char *replacement, bool case_sensitive) {
    /* Replace current selection if it matches needle */
    int32_t s, e;
    ta_get_sel(ta, &s, &e);
    int32_t needle_len = (int32_t)strlen(needle);
    int32_t repl_len = (int32_t)strlen(replacement);

    if (e - s == needle_len && s >= 0) {
        bool match = true;
        for (int32_t j = 0; j < needle_len; j++) {
            char a = ta->buf[s + j];
            char b = needle[j];
            if (!case_sensitive) { a = ta_lower(a); b = ta_lower(b); }
            if (a != b) { match = false; break; }
        }
        if (match) {
            ta_delete_sel(ta);
            ta_insert(ta, replacement, repl_len);
            ta_scan_lines(ta);
            ta_update_scrollbars(ta);
            ta_ensure_visible(ta);
            wm_invalidate(ta->hwnd);
            return true;
        }
    }

    /* Otherwise, find next occurrence */
    return textarea_find(ta, needle, case_sensitive, true);
}

/*==========================================================================
 * Textarea — replace all
 *=========================================================================*/

int textarea_replace_all(textarea_t *ta, const char *needle,
                          const char *replacement, bool case_sensitive) {
    int32_t needle_len = (int32_t)strlen(needle);
    int32_t repl_len = (int32_t)strlen(replacement);
    if (needle_len == 0) return 0;

    int count = 0;
    int32_t i = 0;

    while (i <= ta->len - needle_len) {
        bool match = true;
        for (int32_t j = 0; j < needle_len; j++) {
            char a = ta->buf[i + j];
            char b = needle[j];
            if (!case_sensitive) { a = ta_lower(a); b = ta_lower(b); }
            if (a != b) { match = false; break; }
        }
        if (match) {
            /* Check buffer space */
            if (ta->len + (repl_len - needle_len) >= ta->buf_size) break;

            memmove(ta->buf + i + repl_len,
                    ta->buf + i + needle_len,
                    ta->len - i - needle_len);
            memcpy(ta->buf + i, replacement, repl_len);
            ta->len += (repl_len - needle_len);
            ta->buf[ta->len] = '\0';
            i += repl_len;
            count++;
        } else {
            i++;
        }
    }

    if (count > 0) {
        ta->sel_anchor = -1;
        ta_scan_lines(ta);
        ta_update_scrollbars(ta);
        ta_ensure_visible(ta);
        wm_invalidate(ta->hwnd);
    }
    return count;
}

/*==========================================================================
 * Textarea — blink
 *=========================================================================*/

void textarea_blink(textarea_t *ta) {
    ta->cursor_visible = !ta->cursor_visible;
    wm_invalidate(ta->hwnd);
}

/*==========================================================================
 * Checkbox
 *=========================================================================*/

void checkbox_init(checkbox_t *cb, int16_t x, int16_t y, const char *label) {
    memset(cb, 0, sizeof(*cb));
    cb->x = x;
    cb->y = y;
    if (label) {
        strncpy(cb->label, label, sizeof(cb->label) - 1);
        cb->label[sizeof(cb->label) - 1] = '\0';
    }
}

void checkbox_paint(checkbox_t *cb) {
    int16_t x = cb->x, y = cb->y;

    /* Sunken box */
    wd_hline(x, y, CHECKBOX_SIZE, COLOR_DARK_GRAY);
    wd_vline(x, y, CHECKBOX_SIZE, COLOR_DARK_GRAY);
    wd_hline(x, y + CHECKBOX_SIZE - 1, CHECKBOX_SIZE, COLOR_WHITE);
    wd_vline(x + CHECKBOX_SIZE - 1, y, CHECKBOX_SIZE, COLOR_WHITE);
    wd_fill_rect(x + 1, y + 1, CHECKBOX_SIZE - 2, CHECKBOX_SIZE - 2,
                 COLOR_WHITE);

    if (cb->checked) {
        /* Draw checkmark */
        int cx = x + 3, cy = y + 5;
        wd_pixel(cx, cy, COLOR_BLACK);
        wd_pixel(cx + 1, cy + 1, COLOR_BLACK);
        wd_pixel(cx + 2, cy + 2, COLOR_BLACK);
        wd_pixel(cx + 3, cy + 1, COLOR_BLACK);
        wd_pixel(cx + 4, cy, COLOR_BLACK);
        wd_pixel(cx + 5, cy - 1, COLOR_BLACK);
        wd_pixel(cx + 6, cy - 2, COLOR_BLACK);
        /* Thicken */
        wd_pixel(cx, cy - 1, COLOR_BLACK);
        wd_pixel(cx + 1, cy, COLOR_BLACK);
        wd_pixel(cx + 2, cy + 1, COLOR_BLACK);
        wd_pixel(cx + 3, cy, COLOR_BLACK);
        wd_pixel(cx + 4, cy - 1, COLOR_BLACK);
        wd_pixel(cx + 5, cy - 2, COLOR_BLACK);
        wd_pixel(cx + 6, cy - 3, COLOR_BLACK);
    }

    /* Label */
    wd_text_ui(x + CHECKBOX_SIZE + 4,
               y + (CHECKBOX_SIZE - FONT_UI_HEIGHT) / 2,
               cb->label, COLOR_BLACK, THEME_BUTTON_FACE);
}

bool checkbox_event(checkbox_t *cb, const window_event_t *event,
                    bool *changed) {
    if (event->type != WM_LBUTTONDOWN) return false;
    int16_t mx = event->mouse.x;
    int16_t my = event->mouse.y;
    int16_t label_w = gfx_utf8_charcount(cb->label) * FONT_UI_WIDTH;
    int16_t hit_w = CHECKBOX_SIZE + 4 + label_w;
    if (mx >= cb->x && mx < cb->x + hit_w &&
        my >= cb->y && my < cb->y + CHECKBOX_SIZE) {
        cb->checked = !cb->checked;
        if (changed) *changed = true;
        return true;
    }
    return false;
}

/*==========================================================================
 * Radio Button Group
 *=========================================================================*/

void radiogroup_init(radiogroup_t *rg, int16_t x, int16_t y,
                     uint8_t count, int16_t spacing) {
    memset(rg, 0, sizeof(*rg));
    rg->x = x;
    rg->y = y;
    rg->count = count > RADIO_MAX_OPTIONS ? RADIO_MAX_OPTIONS : count;
    rg->spacing = spacing;
}

void radiogroup_set_labels(radiogroup_t *rg, const char **labels) {
    for (uint8_t i = 0; i < rg->count; i++) {
        strncpy(rg->labels[i], labels[i], sizeof(rg->labels[0]) - 1);
        rg->labels[i][sizeof(rg->labels[0]) - 1] = '\0';
    }
}

void radiogroup_paint(radiogroup_t *rg) {
    for (uint8_t i = 0; i < rg->count; i++) {
        int16_t ry = rg->y + i * rg->spacing;
        int16_t rx = rg->x;
        bool sel = (rg->selected == i);

        /* Sunken square approximating a radio circle */
        wd_bevel_rect(rx, ry + 2, 12, 12,
                      COLOR_DARK_GRAY, COLOR_WHITE, COLOR_WHITE);
        if (sel) {
            /* Inner filled dot */
            wd_fill_rect(rx + 4, ry + 6, 4, 4, COLOR_BLACK);
        }
        wd_text_ui(rx + 16, ry + 2, rg->labels[i],
                   COLOR_BLACK, THEME_BUTTON_FACE);
    }
}

bool radiogroup_event(radiogroup_t *rg, const window_event_t *event,
                      uint8_t *new_sel) {
    if (event->type != WM_LBUTTONDOWN) return false;
    int16_t mx = event->mouse.x;
    int16_t my = event->mouse.y;
    for (uint8_t i = 0; i < rg->count; i++) {
        int16_t ry = rg->y + i * rg->spacing;
        if (mx >= rg->x && mx < rg->x + 120 &&
            my >= ry && my < ry + rg->spacing) {
            if (rg->selected != i) {
                rg->selected = i;
                if (new_sel) *new_sel = i;
                return true;
            }
            return false;
        }
    }
    return false;
}

/*==========================================================================
 * Single-line Text Field
 *=========================================================================*/

/* Draw sunken field border */
static void tf_draw_border(int16_t x, int16_t y, int16_t w, int16_t h) {
    wd_hline(x, y, w, COLOR_DARK_GRAY);
    wd_vline(x, y, h, COLOR_DARK_GRAY);
    wd_hline(x + 1, y + 1, w - 2, COLOR_BLACK);
    wd_vline(x + 1, y + 1, h - 2, COLOR_BLACK);
    wd_hline(x, y + h - 1, w, COLOR_WHITE);
    wd_vline(x + w - 1, y, h, COLOR_WHITE);
    wd_hline(x + 1, y + h - 2, w - 2, THEME_BUTTON_FACE);
    wd_vline(x + w - 2, y + 1, h - 2, THEME_BUTTON_FACE);
    wd_fill_rect(x + 2, y + 2, w - 4, h - 4, COLOR_WHITE);
}

void textfield_init(textfield_t *tf, char *buf, int16_t buf_size,
                    hwnd_t hwnd) {
    memset(tf, 0, sizeof(*tf));
    tf->buf = buf;
    tf->buf_size = buf_size;
    tf->hwnd = hwnd;
    tf->len = (int16_t)strlen(buf);
    tf->cursor = tf->len;
    tf->cursor_visible = true;
    tf->focused = false;
}

void textfield_set_rect(textfield_t *tf, int16_t x, int16_t y,
                        int16_t w, int16_t h) {
    tf->x = x;
    tf->y = y;
    tf->w = w;
    tf->h = h;
}

void textfield_paint(textfield_t *tf) {
    tf_draw_border(tf->x, tf->y, tf->w, tf->h);

    /* Text content — needs screen coords for pixel-level cursor */
    window_t *win = wm_get_window(tf->hwnd);
    if (!win) return;
    point_t co = theme_client_origin(&win->frame, win->flags);

    int tx = co.x + tf->x + 4;
    int ty = co.y + tf->y + (tf->h - FONT_UI_HEIGHT) / 2;
    gfx_text_ui(tx, ty, tf->buf, COLOR_BLACK, COLOR_WHITE);

    /* Blinking cursor */
    if (tf->focused && tf->cursor_visible) {
        int cx = tx + tf->cursor * FONT_UI_WIDTH;
        for (int r = 0; r < FONT_UI_HEIGHT; r++)
            if ((unsigned)(ty + r) < (unsigned)display_height)
                display_set_pixel(cx, ty + r, COLOR_BLACK);
    }
}

bool textfield_event(textfield_t *tf, const window_event_t *event) {
    switch (event->type) {
    case WM_CHAR: {
        if (!tf->focused) return false;
        char ch = event->charev.ch;
        if (ch < 0x20 || ch >= 0x7F) return false;
        if (event->charev.modifiers & KMOD_CTRL) return false;
        if (tf->len < tf->buf_size - 1) {
            for (int i = tf->len; i > tf->cursor; i--)
                tf->buf[i] = tf->buf[i - 1];
            tf->buf[tf->cursor] = ch;
            tf->len++;
            tf->cursor++;
            tf->buf[tf->len] = '\0';
            tf->cursor_visible = true;
            wm_invalidate(tf->hwnd);
        }
        return true;
    }

    case WM_KEYDOWN: {
        if (!tf->focused) return false;
        uint8_t sc = event->key.scancode;
        switch (sc) {
        case 0x2A: /* Backspace */
            if (tf->cursor > 0) {
                for (int i = tf->cursor - 1; i < tf->len - 1; i++)
                    tf->buf[i] = tf->buf[i + 1];
                tf->len--;
                tf->cursor--;
                tf->buf[tf->len] = '\0';
                tf->cursor_visible = true;
                wm_invalidate(tf->hwnd);
            }
            return true;
        case 0x4C: /* Delete */
            if (tf->cursor < tf->len) {
                for (int i = tf->cursor; i < tf->len - 1; i++)
                    tf->buf[i] = tf->buf[i + 1];
                tf->len--;
                tf->buf[tf->len] = '\0';
                tf->cursor_visible = true;
                wm_invalidate(tf->hwnd);
            }
            return true;
        case 0x50: /* Left */
            if (tf->cursor > 0) {
                tf->cursor--;
                tf->cursor_visible = true;
                wm_invalidate(tf->hwnd);
            }
            return true;
        case 0x4F: /* Right */
            if (tf->cursor < tf->len) {
                tf->cursor++;
                tf->cursor_visible = true;
                wm_invalidate(tf->hwnd);
            }
            return true;
        case 0x4A: /* Home */
            tf->cursor = 0;
            tf->cursor_visible = true;
            wm_invalidate(tf->hwnd);
            return true;
        case 0x4D: /* End */
            tf->cursor = tf->len;
            tf->cursor_visible = true;
            wm_invalidate(tf->hwnd);
            return true;
        }
        return false;
    }

    case WM_LBUTTONDOWN: {
        int16_t mx = event->mouse.x;
        int16_t my = event->mouse.y;
        if (mx >= tf->x + 2 && mx < tf->x + tf->w - 2 &&
            my >= tf->y && my < tf->y + tf->h) {
            tf->focused = true;
            int pos = (mx - tf->x - 4) / FONT_UI_WIDTH;
            if (pos < 0) pos = 0;
            if (pos > tf->len) pos = tf->len;
            tf->cursor = (int16_t)pos;
            tf->cursor_visible = true;
            wm_invalidate(tf->hwnd);
            return true;
        }
        return false;
    }

    default:
        return false;
    }
}

void textfield_blink(textfield_t *tf) {
    tf->cursor_visible = !tf->cursor_visible;
    wm_invalidate(tf->hwnd);
}

void textfield_set_text(textfield_t *tf, const char *text) {
    strncpy(tf->buf, text, tf->buf_size - 1);
    tf->buf[tf->buf_size - 1] = '\0';
    tf->len = (int16_t)strlen(tf->buf);
    tf->cursor = tf->len;
}

const char *textfield_get_text(textfield_t *tf) {
    return tf->buf;
}

int16_t textfield_get_length(textfield_t *tf) {
    return tf->len;
}

/*==========================================================================
 * Slider
 *=========================================================================*/

#define SLIDER_THUMB_W  12
#define SLIDER_THUMB_H  16

void slider_init(slider_t *sl, bool horizontal) {
    memset(sl, 0, sizeof(*sl));
    sl->horizontal = horizontal;
}

void slider_set_range(slider_t *sl, int32_t min_val, int32_t max_val,
                      int32_t step) {
    sl->min_val = min_val;
    sl->max_val = max_val;
    sl->step = step;
}

void slider_set_rect(slider_t *sl, int16_t x, int16_t y,
                     int16_t w, int16_t h) {
    sl->x = x;
    sl->y = y;
    sl->w = w;
    sl->h = h;
}

static int32_t slider_clamp(slider_t *sl, int32_t val) {
    if (val < sl->min_val) val = sl->min_val;
    if (val > sl->max_val) val = sl->max_val;
    if (sl->step > 0) {
        int32_t offset = val - sl->min_val;
        offset = ((offset + sl->step / 2) / sl->step) * sl->step;
        val = sl->min_val + offset;
        if (val > sl->max_val) val = sl->max_val;
    }
    return val;
}

/* Pixel position of thumb for current value */
static int16_t slider_thumb_pos(slider_t *sl) {
    int32_t range = sl->max_val - sl->min_val;
    if (range <= 0) return sl->x;
    if (sl->horizontal) {
        int16_t track = sl->w - SLIDER_THUMB_W;
        return sl->x + (int16_t)((sl->value - sl->min_val) * track / range);
    } else {
        int16_t track = sl->h - SLIDER_THUMB_H;
        return sl->y + (int16_t)((sl->value - sl->min_val) * track / range);
    }
}

/* Value from pixel position */
static int32_t slider_val_from_pos(slider_t *sl, int16_t pos) {
    int32_t range = sl->max_val - sl->min_val;
    if (range <= 0) return sl->min_val;
    if (sl->horizontal) {
        int16_t track = sl->w - SLIDER_THUMB_W;
        if (track <= 0) return sl->min_val;
        int rel = pos - sl->x;
        if (rel < 0) rel = 0;
        if (rel > track) rel = track;
        return slider_clamp(sl, sl->min_val + (int32_t)rel * range / track);
    } else {
        int16_t track = sl->h - SLIDER_THUMB_H;
        if (track <= 0) return sl->min_val;
        int rel = pos - sl->y;
        if (rel < 0) rel = 0;
        if (rel > track) rel = track;
        return slider_clamp(sl, sl->min_val + (int32_t)rel * range / track);
    }
}

void slider_paint(slider_t *sl) {
    /* Sunken track */
    wd_bevel_rect(sl->x, sl->y, sl->w, sl->h,
                  COLOR_DARK_GRAY, COLOR_WHITE, THEME_BUTTON_FACE);

    /* Raised thumb */
    if (sl->horizontal) {
        int16_t tx = slider_thumb_pos(sl);
        wd_bevel_rect(tx, sl->y, SLIDER_THUMB_W, sl->h,
                      COLOR_WHITE, COLOR_DARK_GRAY, THEME_BUTTON_FACE);
    } else {
        int16_t ty = slider_thumb_pos(sl);
        wd_bevel_rect(sl->x, ty, sl->w, SLIDER_THUMB_H,
                      COLOR_WHITE, COLOR_DARK_GRAY, THEME_BUTTON_FACE);
    }
}

bool slider_event(slider_t *sl, const window_event_t *event,
                  int32_t *new_val) {
    switch (event->type) {
    case WM_LBUTTONDOWN: {
        int16_t mx = event->mouse.x;
        int16_t my = event->mouse.y;
        if (mx >= sl->x && mx < sl->x + sl->w &&
            my >= sl->y && my < sl->y + sl->h) {
            sl->dragging = true;
            int32_t val;
            if (sl->horizontal)
                val = slider_val_from_pos(sl, mx);
            else
                val = slider_val_from_pos(sl, my);
            if (val != sl->value) {
                sl->value = val;
                if (new_val) *new_val = val;
                return true;
            }
        }
        return false;
    }

    case WM_MOUSEMOVE: {
        if (!sl->dragging) return false;
        int32_t val;
        if (sl->horizontal)
            val = slider_val_from_pos(sl, event->mouse.x);
        else
            val = slider_val_from_pos(sl, event->mouse.y);
        if (val != sl->value) {
            sl->value = val;
            if (new_val) *new_val = val;
            return true;
        }
        return false;
    }

    case WM_LBUTTONUP:
        sl->dragging = false;
        return false;

    default:
        return false;
    }
}

/*==========================================================================
 * Combobox
 *=========================================================================*/

void combobox_init(combobox_t *cb, char *buf, int16_t buf_size, hwnd_t hwnd) {
    memset(cb, 0, sizeof(*cb));
    textfield_init(&cb->field, buf, buf_size, hwnd);
    cb->drop_btn_w = 14;
    cb->drop_hover = -1;
    cb->max_visible = 3;
    cb->drop_item_h = 14;
    cb->hwnd = hwnd;
}

void combobox_set_rect(combobox_t *cb, int16_t x, int16_t y,
                       int16_t w, int16_t h) {
    /* Text field gets the left portion */
    textfield_set_rect(&cb->field, x, y, w - cb->drop_btn_w - 2, h);
    /* Store full rect for dropdown positioning */
    cb->field.x = x;
    cb->field.w = w - cb->drop_btn_w - 2;
}

void combobox_set_items(combobox_t *cb, char (*items)[64], int8_t count) {
    cb->items = items;
    cb->item_count = count > COMBOBOX_MAX_ITEMS ?
                     COMBOBOX_MAX_ITEMS : count;
}

void combobox_paint(combobox_t *cb) {
    textfield_t *tf = &cb->field;
    int16_t full_w = tf->w + 2 + cb->drop_btn_w;

    /* Draw combined sunken border */
    tf_draw_border(tf->x, tf->y, full_w, tf->h);

    /* Text content */
    window_t *win = wm_get_window(cb->hwnd);
    if (!win) return;
    point_t co = theme_client_origin(&win->frame, win->flags);

    int tx = co.x + tf->x + 4;
    int ty = co.y + tf->y + (tf->h - FONT_UI_HEIGHT) / 2;
    gfx_text_ui(tx, ty, tf->buf, COLOR_BLACK, COLOR_WHITE);

    /* Cursor (when focused and dropdown closed) */
    if (tf->focused && tf->cursor_visible && !cb->drop_open) {
        int cx = tx + tf->cursor * FONT_UI_WIDTH;
        for (int r = 0; r < FONT_UI_HEIGHT; r++)
            if ((unsigned)(ty + r) < (unsigned)display_height)
                display_set_pixel(cx, ty + r, COLOR_BLACK);
    }

    /* Arrow button area */
    int16_t bx = tf->x + tf->w + 2;
    int16_t bw = cb->drop_btn_w - 2;
    int16_t by = tf->y + 2;
    int16_t bh = tf->h - 4;
    wd_fill_rect(bx, by, bw, bh, THEME_BUTTON_FACE);
    wd_vline(bx - 1, by, bh, COLOR_DARK_GRAY);

    /* Down-arrow triangle */
    int16_t acx = bx + bw / 2;
    int16_t acy = by + bh / 2;
    wd_hline(acx - 2, acy - 1, 5, COLOR_BLACK);
    wd_hline(acx - 1, acy,     3, COLOR_BLACK);
    wd_pixel(acx,     acy + 1,    COLOR_BLACK);

    /* Dropdown list */
    if (cb->drop_open && cb->item_count > 0) {
        int vis = cb->item_count < cb->max_visible ?
                  cb->item_count : cb->max_visible;
        int16_t dl_x = tf->x;
        int16_t dl_y = tf->y + tf->h;
        int16_t dl_w = full_w;
        int16_t dl_h = (int16_t)(vis * cb->drop_item_h + 2);

        wd_fill_rect(dl_x, dl_y, dl_w, dl_h, COLOR_WHITE);
        wd_hline(dl_x, dl_y, dl_w, COLOR_DARK_GRAY);
        wd_hline(dl_x, dl_y + dl_h - 1, dl_w, COLOR_DARK_GRAY);
        wd_vline(dl_x, dl_y, dl_h, COLOR_DARK_GRAY);
        wd_vline(dl_x + dl_w - 1, dl_y, dl_h, COLOR_DARK_GRAY);

        for (int i = 0; i < vis; i++) {
            int idx = cb->drop_scroll + i;
            if (idx >= cb->item_count) break;
            bool hov = (cb->drop_hover == idx);
            uint8_t bg = hov ? COLOR_BLUE  : COLOR_WHITE;
            uint8_t fg = hov ? COLOR_WHITE : COLOR_BLACK;
            int16_t iy = dl_y + 1 + (int16_t)(i * cb->drop_item_h);

            wd_fill_rect(dl_x + 1, iy, dl_w - 2,
                         cb->drop_item_h, bg);
            wd_text_ui(dl_x + 4,
                       iy + (cb->drop_item_h - FONT_UI_HEIGHT) / 2,
                       cb->items[idx], fg, bg);
        }
    }
}

/* Hit-test: returns item index or -1 */
static int cb_drop_hittest(combobox_t *cb, int16_t mx, int16_t my) {
    if (!cb->drop_open || cb->item_count == 0) return -1;
    textfield_t *tf = &cb->field;
    int16_t full_w = tf->w + 2 + cb->drop_btn_w;
    int vis = cb->item_count < cb->max_visible ?
              cb->item_count : cb->max_visible;
    int16_t dl_x = tf->x;
    int16_t dl_y = tf->y + tf->h;
    int16_t dl_w = full_w;
    int16_t dl_h = (int16_t)(vis * cb->drop_item_h + 2);

    if (mx < dl_x || mx >= dl_x + dl_w) return -1;
    if (my < dl_y || my >= dl_y + dl_h) return -1;

    int rel = my - dl_y - 1;
    if (rel < 0) return -1;
    int idx = cb->drop_scroll + rel / cb->drop_item_h;
    if (idx >= cb->item_count) return -1;
    return idx;
}

bool combobox_event(combobox_t *cb, const window_event_t *event) {
    textfield_t *tf = &cb->field;
    int16_t full_w = tf->w + 2 + cb->drop_btn_w;

    switch (event->type) {
    case WM_LBUTTONDOWN: {
        int16_t mx = event->mouse.x;
        int16_t my = event->mouse.y;

        /* Dropdown list has priority */
        {
            int hit = cb_drop_hittest(cb, mx, my);
            if (hit >= 0) {
                cb->drop_hover = (int8_t)hit;
                wm_invalidate(cb->hwnd);
                return true;
            }
        }

        /* Arrow button */
        int16_t btn_x = tf->x + tf->w + 2;
        if (mx >= btn_x && mx < btn_x + cb->drop_btn_w &&
            my >= tf->y && my < tf->y + tf->h) {
            cb->drop_open = !cb->drop_open;
            cb->drop_hover = -1;
            cb->drop_scroll = 0;
            wm_force_full_repaint();
            return true;
        }

        /* Text field click */
        if (mx >= tf->x + 2 && mx < tf->x + tf->w - 2 &&
            my >= tf->y && my < tf->y + tf->h) {
            tf->focused = true;
            int pos = (mx - tf->x - 4) / FONT_UI_WIDTH;
            if (pos < 0) pos = 0;
            if (pos > tf->len) pos = tf->len;
            tf->cursor = (int16_t)pos;
            if (cb->drop_open) {
                cb->drop_open = false;
                wm_force_full_repaint();
            } else {
                tf->cursor_visible = true;
                wm_invalidate(cb->hwnd);
            }
            return true;
        }

        /* Click elsewhere — close dropdown */
        if (cb->drop_open) {
            cb->drop_open = false;
            cb->drop_hover = -1;
            wm_force_full_repaint();
        }
        return false;
    }

    case WM_LBUTTONUP: {
        if (cb->drop_open) {
            int hit = cb_drop_hittest(cb, event->mouse.x, event->mouse.y);
            if (hit >= 0) {
                combobox_select_item(cb, (int8_t)hit);
                return true;
            }
        }
        return false;
    }

    case WM_MOUSEMOVE: {
        if (!cb->drop_open) return false;
        int hit = cb_drop_hittest(cb, event->mouse.x, event->mouse.y);
        if (hit != cb->drop_hover) {
            cb->drop_hover = (int8_t)hit;
            wm_invalidate(cb->hwnd);
        }
        return false;
    }

    case WM_KEYDOWN: {
        if (!tf->focused) return false;
        uint8_t sc = event->key.scancode;

        /* Escape closes dropdown */
        if (sc == 0x29 && cb->drop_open) {
            cb->drop_open = false;
            cb->drop_hover = -1;
            wm_force_full_repaint();
            return true;
        }

        /* Enter selects dropdown item */
        if (sc == 0x28 && cb->drop_open) {
            if (cb->drop_hover >= 0)
                combobox_select_item(cb, cb->drop_hover);
            else {
                cb->drop_open = false;
                cb->drop_hover = -1;
                wm_force_full_repaint();
            }
            return true;
        }

        /* Down arrow: open/navigate dropdown */
        if (sc == 0x51) {
            if (cb->drop_open) {
                if (cb->drop_hover < cb->item_count - 1) {
                    cb->drop_hover++;
                    int vis = cb->item_count < cb->max_visible ?
                              cb->item_count : cb->max_visible;
                    if (cb->drop_hover >= cb->drop_scroll + vis)
                        cb->drop_scroll = cb->drop_hover - vis + 1;
                }
                wm_force_full_repaint();
            } else if (cb->item_count > 0) {
                cb->drop_open = true;
                cb->drop_hover = -1;
                cb->drop_scroll = 0;
                wm_force_full_repaint();
            }
            return true;
        }

        /* Up arrow: navigate dropdown */
        if (sc == 0x52 && cb->drop_open) {
            if (cb->drop_hover > 0) {
                cb->drop_hover--;
                if (cb->drop_hover < cb->drop_scroll)
                    cb->drop_scroll = cb->drop_hover;
            } else if (cb->drop_hover == 0) {
                cb->drop_hover = -1;
            } else {
                cb->drop_open = false;
                cb->drop_hover = -1;
            }
            wm_force_full_repaint();
            return true;
        }

        /* When dropdown is open, don't pass text editing keys */
        if (cb->drop_open) return true;

        /* Delegate text editing to textfield */
        return textfield_event(tf, event);
    }

    case WM_CHAR:
        if (!tf->focused || cb->drop_open) return cb->drop_open;
        return textfield_event(tf, event);

    default:
        return false;
    }
}

bool combobox_is_open(combobox_t *cb) {
    return cb->drop_open;
}

void combobox_select_item(combobox_t *cb, int8_t index) {
    if (index < 0 || index >= cb->item_count) return;
    textfield_set_text(&cb->field, cb->items[index]);
    cb->drop_open = false;
    cb->drop_hover = -1;
    cb->field.focused = true;
    wm_force_full_repaint();
}
