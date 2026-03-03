/*
 * FRANK OS
 * Copyright (c) 2025 Mikhail Matveev <xtreme@rh1.tech>
 * https://rh1.tech
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "cursor.h"
#include "display.h"
#include "window.h" /* COLOR_BLACK, COLOR_WHITE */

/* 0 = transparent, 1 = black (outline), 2 = white (fill)
 * Bitmaps extracted from Win95 .cur files in assets/cursors/ */

/*--- Arrow cursor (11x19, hotspot 0,0) — arrow.cur ---*/
#define ARROW_W   11
#define ARROW_H   19
static const uint8_t arrow_bitmap[ARROW_H][ARROW_W] = {
    { 1,0,0,0,0,0,0,0,0,0,0 },
    { 1,1,0,0,0,0,0,0,0,0,0 },
    { 1,2,1,0,0,0,0,0,0,0,0 },
    { 1,2,2,1,0,0,0,0,0,0,0 },
    { 1,2,2,2,1,0,0,0,0,0,0 },
    { 1,2,2,2,2,1,0,0,0,0,0 },
    { 1,2,2,2,2,2,1,0,0,0,0 },
    { 1,2,2,2,2,2,2,1,0,0,0 },
    { 1,2,2,2,2,2,2,2,1,0,0 },
    { 1,2,2,2,2,2,2,2,2,1,0 },
    { 1,2,2,2,2,2,1,1,1,1,1 },
    { 1,2,2,1,2,2,1,0,0,0,0 },
    { 1,2,1,0,1,2,2,1,0,0,0 },
    { 1,1,0,0,1,2,2,1,0,0,0 },
    { 1,0,0,0,0,1,2,2,1,0,0 },
    { 0,0,0,0,0,1,2,2,1,0,0 },
    { 0,0,0,0,0,0,1,2,2,1,0 },
    { 0,0,0,0,0,0,1,2,2,1,0 },
    { 0,0,0,0,0,0,0,1,1,0,0 },
};

/*--- Resize N-S cursor (9x21, hotspot 4,10) — Cursor_9.cur ---*/
#define NS_W  9
#define NS_H  21
static const uint8_t ns_bitmap[NS_H][NS_W] = {
    { 0,0,0,0,2,0,0,0,0 },
    { 0,0,0,2,1,2,0,0,0 },
    { 0,0,2,1,1,1,2,0,0 },
    { 0,2,1,1,1,1,1,2,0 },
    { 2,1,1,1,1,1,1,1,2 },
    { 2,2,2,2,1,2,2,2,2 },
    { 0,0,0,2,1,2,0,0,0 },
    { 0,0,0,2,1,2,0,0,0 },
    { 0,0,0,2,1,2,0,0,0 },
    { 0,0,0,2,1,2,0,0,0 },
    { 0,0,0,2,1,2,0,0,0 },
    { 0,0,0,2,1,2,0,0,0 },
    { 0,0,0,2,1,2,0,0,0 },
    { 0,0,0,2,1,2,0,0,0 },
    { 0,0,0,2,1,2,0,0,0 },
    { 2,2,2,2,1,2,2,2,2 },
    { 2,1,1,1,1,1,1,1,2 },
    { 0,2,1,1,1,1,1,2,0 },
    { 0,0,2,1,1,1,2,0,0 },
    { 0,0,0,2,1,2,0,0,0 },
    { 0,0,0,0,2,0,0,0,0 },
};

/*--- Resize E-W cursor (21x9, hotspot 10,4) — Cursor_8.cur ---*/
#define EW_W  21
#define EW_H  9
static const uint8_t ew_bitmap[EW_H][EW_W] = {
    { 0,0,0,0,2,2,0,0,0,0,0,0,0,0,0,2,2,0,0,0,0 },
    { 0,0,0,2,1,2,0,0,0,0,0,0,0,0,0,2,1,2,0,0,0 },
    { 0,0,2,1,1,2,0,0,0,0,0,0,0,0,0,2,1,1,2,0,0 },
    { 0,2,1,1,1,2,2,2,2,2,2,2,2,2,2,2,1,1,1,2,0 },
    { 2,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,2 },
    { 0,2,1,1,1,2,2,2,2,2,2,2,2,2,2,2,1,1,1,2,0 },
    { 0,0,2,1,1,2,0,0,0,0,0,0,0,0,0,2,1,1,2,0,0 },
    { 0,0,0,2,1,2,0,0,0,0,0,0,0,0,0,2,1,2,0,0,0 },
    { 0,0,0,0,2,2,0,0,0,0,0,0,0,0,0,2,2,0,0,0,0 },
};

/*--- Resize NW-SE cursor (15x15, hotspot 7,7) — Cursor_6.cur ---*/
#define NWSE_W  15
#define NWSE_H  15
static const uint8_t nwse_bitmap[NWSE_H][NWSE_W] = {
    { 2,2,2,2,2,2,2,0,0,0,0,0,0,0,0 },
    { 2,1,1,1,1,1,2,0,0,0,0,0,0,0,0 },
    { 2,1,1,1,1,2,0,0,0,0,0,0,0,0,0 },
    { 2,1,1,1,2,0,0,0,0,0,0,0,0,0,0 },
    { 2,1,1,2,1,2,0,0,0,0,0,0,0,0,0 },
    { 2,1,2,0,2,1,2,0,0,0,0,0,0,0,0 },
    { 2,2,0,0,0,2,1,2,0,0,0,0,0,0,0 },
    { 0,0,0,0,0,0,2,1,2,0,0,0,0,0,0 },
    { 0,0,0,0,0,0,0,2,1,2,0,0,0,2,2 },
    { 0,0,0,0,0,0,0,0,2,1,2,0,2,1,2 },
    { 0,0,0,0,0,0,0,0,0,2,1,2,1,1,2 },
    { 0,0,0,0,0,0,0,0,0,0,2,1,1,1,2 },
    { 0,0,0,0,0,0,0,0,0,2,1,1,1,1,2 },
    { 0,0,0,0,0,0,0,0,2,1,1,1,1,1,2 },
    { 0,0,0,0,0,0,0,0,2,2,2,2,2,2,2 },
};

/*--- Resize NE-SW cursor (15x15, hotspot 7,7) — Cursor_7.cur ---*/
#define NESW_W  15
#define NESW_H  15
static const uint8_t nesw_bitmap[NESW_H][NESW_W] = {
    { 0,0,0,0,0,0,0,0,2,2,2,2,2,2,2 },
    { 0,0,0,0,0,0,0,0,2,1,1,1,1,1,2 },
    { 0,0,0,0,0,0,0,0,0,2,1,1,1,1,2 },
    { 0,0,0,0,0,0,0,0,0,0,2,1,1,1,2 },
    { 0,0,0,0,0,0,0,0,0,2,1,2,1,1,2 },
    { 0,0,0,0,0,0,0,0,2,1,2,0,2,1,2 },
    { 0,0,0,0,0,0,0,2,1,2,0,0,0,2,2 },
    { 0,0,0,0,0,0,2,1,2,0,0,0,0,0,0 },
    { 2,2,0,0,0,2,1,2,0,0,0,0,0,0,0 },
    { 2,1,2,0,2,1,2,0,0,0,0,0,0,0,0 },
    { 2,1,1,2,1,2,0,0,0,0,0,0,0,0,0 },
    { 2,1,1,1,2,0,0,0,0,0,0,0,0,0,0 },
    { 2,1,1,1,1,2,0,0,0,0,0,0,0,0,0 },
    { 2,1,1,1,1,1,2,0,0,0,0,0,0,0,0 },
    { 2,2,2,2,2,2,2,0,0,0,0,0,0,0,0 },
};

/*--- Wait/hourglass cursor (13x22, hotspot 6,10) — Cursor_3.cur ---*/
#define WAIT_W  13
#define WAIT_H  22
static const uint8_t wait_bitmap[WAIT_H][WAIT_W] = {
    { 1,1,1,1,1,1,1,1,1,1,1,1,1 },
    { 1,1,2,2,2,2,2,2,2,2,2,1,1 },
    { 1,1,1,1,1,1,1,1,1,1,1,1,1 },
    { 0,1,2,2,2,2,2,2,2,2,2,1,0 },
    { 0,1,2,2,2,2,2,2,2,2,2,1,0 },
    { 0,1,2,2,1,2,1,2,1,2,2,1,0 },
    { 0,1,2,2,2,1,2,1,2,2,2,1,0 },
    { 0,1,1,2,2,2,1,2,2,2,1,1,0 },
    { 0,0,1,1,2,2,2,2,2,1,1,0,0 },
    { 0,0,0,1,1,2,1,2,1,1,0,0,0 },
    { 0,0,0,0,1,1,2,1,1,0,0,0,0 },
    { 0,0,0,0,1,1,2,1,1,0,0,0,0 },
    { 0,0,0,1,1,2,2,2,1,1,0,0,0 },
    { 0,0,1,1,2,2,1,2,2,1,1,0,0 },
    { 0,1,1,2,2,2,2,2,2,2,1,1,0 },
    { 0,1,2,2,2,2,1,2,2,2,2,1,0 },
    { 0,1,2,2,2,1,2,1,2,2,2,1,0 },
    { 0,1,2,2,1,2,1,2,1,2,2,1,0 },
    { 0,1,2,1,2,1,2,1,2,1,2,1,0 },
    { 1,1,1,1,1,1,1,1,1,1,1,1,1 },
    { 1,1,2,2,2,2,2,2,2,2,2,1,1 },
    { 1,1,1,1,1,1,1,1,1,1,1,1,1 },
};

/* Cursor descriptor */
typedef struct {
    const uint8_t *bitmap;
    int16_t w, h;
    int16_t hotspot_x, hotspot_y;
} cursor_def_t;

static const cursor_def_t cursors[CURSOR_COUNT] = {
    [CURSOR_ARROW]       = { (const uint8_t *)arrow_bitmap, ARROW_W, ARROW_H, 0,  0  },
    [CURSOR_RESIZE_NS]   = { (const uint8_t *)ns_bitmap,    NS_W,    NS_H,    4,  10 },
    [CURSOR_RESIZE_EW]   = { (const uint8_t *)ew_bitmap,    EW_W,    EW_H,    10, 4  },
    [CURSOR_RESIZE_NWSE] = { (const uint8_t *)nwse_bitmap,  NWSE_W,  NWSE_H,  7,  7  },
    [CURSOR_RESIZE_NESW] = { (const uint8_t *)nesw_bitmap,  NESW_W,  NESW_H,  7,  7  },
    [CURSOR_WAIT]        = { (const uint8_t *)wait_bitmap,  WAIT_W,  WAIT_H,  6,  10 },
};

static cursor_type_t current_cursor = CURSOR_ARROW;
static bool cursor_visible = true;

void cursor_set_type(cursor_type_t type) {
    if (type < CURSOR_COUNT)
        current_cursor = type;
}

void cursor_set_visible(bool visible) {
    cursor_visible = visible;
}

bool cursor_is_visible(void) {
    return cursor_visible;
}

cursor_type_t cursor_get_type(void) {
    return current_cursor;
}

void cursor_draw(int16_t x, int16_t y) {
    const cursor_def_t *c = &cursors[current_cursor];
    int16_t ox = x - c->hotspot_x;
    int16_t oy = y - c->hotspot_y;

    for (int row = 0; row < c->h; row++) {
        int py = oy + row;
        if (py < 0 || py >= DISPLAY_HEIGHT) continue;
        for (int col = 0; col < c->w; col++) {
            uint8_t px_val = c->bitmap[row * c->w + col];
            if (px_val == 0) continue;
            int px = ox + col;
            if (px < 0 || px >= DISPLAY_WIDTH) continue;
            display_set_pixel(px, py,
                              (px_val == 1) ? COLOR_BLACK : COLOR_WHITE);
        }
    }
}

void cursor_get_bounds(int16_t x, int16_t y,
                       int16_t *x0, int16_t *y0,
                       int16_t *x1, int16_t *y1) {
    const cursor_def_t *c = &cursors[current_cursor];
    int16_t lx = x - c->hotspot_x;
    int16_t ly = y - c->hotspot_y;
    if (lx < 0) lx = 0;
    if (ly < 0) ly = 0;
    int16_t rx = lx + c->w - 1;
    int16_t ry = ly + c->h - 1;
    if (rx >= DISPLAY_WIDTH)  rx = DISPLAY_WIDTH  - 1;
    if (ry >= DISPLAY_HEIGHT) ry = DISPLAY_HEIGHT - 1;
    *x0 = lx; *y0 = ly; *x1 = rx; *y1 = ry;
}

/*==========================================================================
 * Show-buffer cursor overlay with save-under
 *
 * The compositor renders a "clean" frame (no cursor) to the draw buffer.
 * After display_swap_buffers(), the cursor is stamped onto the show buffer
 * using a save-under technique.  When only the cursor moves (no content
 * change), we restore the saved pixels and redraw the cursor at the new
 * position directly on the show buffer — no compositor involvement needed.
 *=========================================================================*/

#define OVERLAY_MAX_BPR   12   /* max bytes per row: ceil(21/2) + 1 for alignment */
#define OVERLAY_MAX_ROWS  22   /* tallest cursor: wait (22 rows) */
#define OVERLAY_SAVE_SIZE (OVERLAY_MAX_BPR * OVERLAY_MAX_ROWS)

static struct {
    bool           valid;           /* overlay is currently stamped */
    int16_t        stamp_x, stamp_y; /* cursor position when stamped */
    cursor_type_t  stamp_type;      /* cursor type when stamped */
    int16_t        save_byte_x;     /* first FB byte column of saved region */
    int16_t        save_bpr;        /* bytes per row in saved region */
    int16_t        save_y0, save_y1; /* first/last row (inclusive) */
    uint8_t        save[OVERLAY_SAVE_SIZE];
    volatile bool  locked;
} overlay;

void cursor_overlay_stamp(int16_t x, int16_t y) {
    const cursor_def_t *c = &cursors[current_cursor];
    int16_t ox = x - c->hotspot_x;
    int16_t oy = y - c->hotspot_y;

    /* Clipped bounding box */
    int16_t x0 = ox < 0 ? 0 : ox;
    int16_t y0 = oy < 0 ? 0 : oy;
    int16_t x1 = ox + c->w - 1;
    int16_t y1 = oy + c->h - 1;
    if (x1 >= DISPLAY_WIDTH)  x1 = DISPLAY_WIDTH  - 1;
    if (y1 >= DISPLAY_HEIGHT) y1 = DISPLAY_HEIGHT - 1;

    if (x0 > x1 || y0 > y1) {
        overlay.valid = false;
        return;
    }

    /* Byte range in framebuffer */
    int16_t byte_x0 = x0 >> 1;
    int16_t byte_x1 = x1 >> 1;
    int16_t bpr = byte_x1 - byte_x0 + 1;

    overlay.save_byte_x = byte_x0;
    overlay.save_bpr    = bpr;
    overlay.save_y0     = y0;
    overlay.save_y1     = y1;
    overlay.stamp_x     = x;
    overlay.stamp_y     = y;
    overlay.stamp_type  = current_cursor;

    /* Save show-buffer pixels under cursor */
    uint8_t *show = display_show_buffer_ptr;
    uint8_t *dst  = overlay.save;
    for (int16_t row = y0; row <= y1; row++) {
        memcpy(dst, &show[row * FB_STRIDE + byte_x0], bpr);
        dst += bpr;
    }

    /* Draw cursor onto show buffer */
    for (int row = 0; row < c->h; row++) {
        int py = oy + row;
        if (py < 0 || py >= DISPLAY_HEIGHT) continue;
        for (int col = 0; col < c->w; col++) {
            uint8_t px_val = c->bitmap[row * c->w + col];
            if (px_val == 0) continue;
            int px = ox + col;
            if (px < 0 || px >= DISPLAY_WIDTH) continue;
            uint8_t color = (px_val == 1) ? COLOR_BLACK : COLOR_WHITE;
            uint8_t *p = &show[py * FB_STRIDE + (px >> 1)];
            if (px & 1)
                *p = (*p & 0xF0) | color;
            else
                *p = (*p & 0x0F) | (color << 4);
        }
    }

    overlay.valid = true;
}

void cursor_overlay_erase(void) {
    if (!overlay.valid) return;

    uint8_t *show = display_show_buffer_ptr;
    const uint8_t *src = overlay.save;
    int16_t bpr = overlay.save_bpr;

    for (int16_t row = overlay.save_y0; row <= overlay.save_y1; row++) {
        memcpy(&show[row * FB_STRIDE + overlay.save_byte_x], src, bpr);
        src += bpr;
    }

    overlay.valid = false;
}

void cursor_overlay_move(int16_t new_x, int16_t new_y) {
    if (overlay.locked) return;
    /* No-op if position and cursor type are unchanged */
    if (overlay.valid &&
        new_x == overlay.stamp_x && new_y == overlay.stamp_y &&
        current_cursor == overlay.stamp_type)
        return;

    cursor_overlay_erase();
    cursor_overlay_stamp(new_x, new_y);
}

bool cursor_overlay_get_stamp(int16_t *x, int16_t *y) {
    *x = overlay.stamp_x;
    *y = overlay.stamp_y;
    return overlay.valid;
}

void cursor_overlay_reset(void) {
    overlay.valid = false;
}

void cursor_overlay_lock(void) {
    overlay.locked = true;
}

void cursor_overlay_unlock(void) {
    overlay.locked = false;
}

bool cursor_overlay_is_locked(void) {
    return overlay.locked;
}

bool cursor_overlay_type_changed(void) {
    return overlay.valid && current_cursor != overlay.stamp_type;
}
