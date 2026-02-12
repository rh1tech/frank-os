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

void cursor_set_type(cursor_type_t type) {
    if (type < CURSOR_COUNT)
        current_cursor = type;
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
