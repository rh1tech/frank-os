/*
 * FRANK OS — Paintbrush (shared header)
 * Copyright (c) 2026 Mikhail Matveev <xtreme@rh1.tech>
 * Windows 95 MS Paint clone for FRANK OS
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef PB_H
#define PB_H

#include "m-os-api.h"
#include "frankos-app.h"
#include "lang.h"
#include "m-os-api-ff.h"

#define dbg_printf(...) ((int(*)(const char*, ...))_sys_table_ptrs[438])(__VA_ARGS__)

/* App-local translations */
enum { AL_ABOUT, AL_TOOLS, AL_IMAGE, AL_SAVE_CHANGES, AL_NEW_MENU, AL_OPEN_MENU, AL_SAVE_MENU, AL_COUNT };
static const char *al_en[] = {
    [AL_ABOUT]        = "About Paintbrush",
    [AL_TOOLS]        = "Tools",
    [AL_IMAGE]        = "Image",
    [AL_SAVE_CHANGES] = "The image has been changed.\nDo you want to save the changes?",
    [AL_NEW_MENU]     = "New    Ctrl+N",
    [AL_OPEN_MENU]    = "Open.. Ctrl+O",
    [AL_SAVE_MENU]    = "Save   Ctrl+S",
};
static const char *al_ru[] = {
    [AL_ABOUT]        = "\xD0\x9E Paintbrush",
    [AL_TOOLS]        = "\xD0\x98\xD0\xBD\xD1\x81\xD1\x82\xD1\x80\xD1\x83\xD0\xBC\xD0\xB5\xD0\xBD\xD1\x82\xD1\x8B",
    [AL_IMAGE]        = "\xD0\x98\xD0\xB7\xD0\xBE\xD0\xB1\xD1\x80\xD0\xB0\xD0\xB6\xD0\xB5\xD0\xBD\xD0\xB8\xD0\xB5",
    [AL_SAVE_CHANGES] = "\xD0\x98\xD0\xB7\xD0\xBE\xD0\xB1\xD1\x80\xD0\xB0\xD0\xB6\xD0\xB5\xD0\xBD\xD0\xB8\xD0\xB5 \xD0\xB8\xD0\xB7\xD0\xBC\xD0\xB5\xD0\xBD\xD0\xB5\xD0\xBD\xD0\xBE.\n\xD0\xA1\xD0\xBE\xD1\x85\xD1\x80\xD0\xB0\xD0\xBD\xD0\xB8\xD1\x82\xD1\x8C \xD0\xB8\xD0\xB7\xD0\xBC\xD0\xB5\xD0\xBD\xD0\xB5\xD0\xBD\xD0\xB8\xD1\x8F?",
    [AL_NEW_MENU]     = "\xD0\x9D\xD0\xBE\xD0\xB2\xD1\x8B\xD0\xB9  Ctrl+N",
    [AL_OPEN_MENU]    = "\xD0\x9E\xD1\x82\xD0\xBA\xD1\x80\xD1\x8B\xD1\x82\xD1\x8C  Ctrl+O",
    [AL_SAVE_MENU]    = "\xD0\xA1\xD0\xBE\xD1\x85\xD1\x80\xD0\xB0\xD0\xBD\xD0\xB8\xD1\x82\xD1\x8C Ctrl+S",
};
static inline const char *AL(int id) { return lang_get() == LANG_RU ? al_ru[id] : al_en[id]; }

/*==========================================================================
 * Canvas defaults
 *=========================================================================*/

#define CANVAS_W_DEFAULT   320
#define CANVAS_H_DEFAULT   240
#define PB_PATH_MAX        256

/*==========================================================================
 * Layout constants
 *=========================================================================*/

#define TOOL_BTN_SIZE   20      /* 16px icon + 2px padding each side */
#define TOOL_COLS        2
#define TOOL_ROWS        6      /* 2x6 grid = 12 tools */
#define TOOL_PANEL_W    (TOOL_COLS * TOOL_BTN_SIZE + 4)  /* 44px with 2px gap */
#define PALETTE_H       28
#define FGBG_BOX        22
#define SWATCH_SIZE     14
#define ICON_PAD         2      /* padding inside tool button */

/* Sub-options area below tools */
#define SUBOPTS_Y       (TOOL_ROWS * TOOL_BTN_SIZE + 2)
#define SUBOPTS_H       52      /* height of sub-options panel */

/*==========================================================================
 * Tool IDs — MS Paint 95 layout (2 columns, 8 rows)
 *=========================================================================*/

#define TOOL_SELECT        0
#define TOOL_ERASER        1
#define TOOL_FILL          2
#define TOOL_PICKER        3
#define TOOL_PENCIL        4
#define TOOL_BRUSH         5
#define TOOL_AIRBRUSH      6
#define TOOL_TEXT          7
#define TOOL_LINE          8
#define TOOL_RECT          9
#define TOOL_ELLIPSE      10
#define TOOL_COUNT        11

/*==========================================================================
 * Shape fill modes (sub-option for rect/ellipse/polygon/rounded rect)
 *=========================================================================*/

#define FILL_OUTLINE       0    /* outline only */
#define FILL_OUTLINE_FILL  1    /* outline + fill */
#define FILL_SOLID         2    /* fill only (no outline) */
#define FILL_MODE_COUNT    3

/*==========================================================================
 * Brush shapes (sub-option for brush tool)
 *=========================================================================*/

#define BRUSH_CIRCLE       0
#define BRUSH_SQUARE       1
#define BRUSH_FWD_SLASH    2
#define BRUSH_BACK_SLASH   3
#define BRUSH_SHAPE_COUNT  4

/*==========================================================================
 * Size options
 *=========================================================================*/

/* Line/curve stroke widths */
#define LINE_WIDTH_COUNT   5
static const uint8_t line_widths[LINE_WIDTH_COUNT] = { 1, 2, 3, 4, 5 };

/* Eraser sizes (square block) */
#define ERASER_SIZE_COUNT  4
static const uint8_t eraser_sizes[ERASER_SIZE_COUNT] = { 4, 6, 8, 10 };

/* Brush sizes per shape (3 options each) */
#define BRUSH_SIZE_COUNT   3
static const uint8_t brush_sizes[BRUSH_SIZE_COUNT] = { 7, 4, 1 };

/* Airbrush sizes */
#define AIRBRUSH_SIZE_COUNT 3
static const uint8_t airbrush_sizes[AIRBRUSH_SIZE_COUNT] = { 9, 16, 24 };

/*==========================================================================
 * Menu commands
 *=========================================================================*/

#define CMD_NEW       100
#define CMD_OPEN      101
#define CMD_SAVE      102
#define CMD_SAVE_AS   103
#define CMD_EXIT      104
#define CMD_UNDO      200
#define CMD_CLEAR     201
#define CMD_CUT       202
#define CMD_COPY      203
#define CMD_PASTE     204
#define CMD_SELECT_ALL 205
#define CMD_ABOUT     300
#define CMD_FILL_DEFERRED 900  /* internal: flood fill deferred to main loop */
#define CMD_SAVE_UNDO     901  /* internal: save undo buffer (76KB memcpy) */
/* Image menu */
#define CMD_FLIP_H    400
#define CMD_FLIP_V    401
#define CMD_INVERT    402

/* Pending actions (for save-changes dialog) */
#define PENDING_NONE  0
#define PENDING_NEW   1
#define PENDING_OPEN  2
#define PENDING_EXIT  3

/*==========================================================================
 * Application state
 *=========================================================================*/

typedef struct {
    hwnd_t     hwnd;
    uint8_t   *canvas;
    uint8_t   *undo_buf;
    int16_t    canvas_w, canvas_h;

    /* Current tool + settings */
    uint8_t    tool;
    uint8_t    fg_color;
    uint8_t    bg_color;

    /* Sub-options per tool type */
    uint8_t    line_width_idx;     /* for line/curve/shape outlines */
    uint8_t    eraser_size_idx;    /* for eraser */
    uint8_t    brush_shape;        /* BRUSH_CIRCLE etc */
    uint8_t    brush_size_idx;     /* index into brush_sizes */
    uint8_t    airbrush_size_idx;  /* index into airbrush_sizes */
    uint8_t    fill_mode;          /* FILL_OUTLINE etc */

    /* Drawing state */
    bool       drawing;
    uint8_t    draw_color;
    int16_t    start_x, start_y;   /* shape start (canvas coords) */
    int16_t    last_x, last_y;     /* freehand last pos (canvas coords) */

    /* Selection state */
    bool       has_selection;
    int16_t    sel_x, sel_y, sel_w, sel_h;  /* selection rect (canvas coords) */
    uint8_t   *sel_buf;                      /* copied pixel data (NULL = none) */
    int16_t    sel_buf_w, sel_buf_h;         /* dimensions of sel_buf */

    /* Floating paste (movable before commit) */
    bool       floating;                     /* true = paste floating, click to commit */
    int16_t    float_x, float_y;             /* top-left of floating content */

    /* Text input state */
    int16_t    text_x, text_y;               /* text insertion point (canvas) */
    char       text_buf[128];                /* text input buffer */

    /* File state */
    bool       modified;
    bool       has_undo;
    uint8_t    pending_action;
    char       filepath[PB_PATH_MAX];

    /* Scroll */
    scrollbar_t hscroll, vscroll;
    int16_t    scroll_x, scroll_y;

    /* Paint flags */
    bool       dirty_canvas;       /* canvas changed, needs blit */
    bool       dirty_ui;           /* toolbar/palette needs repaint */
    volatile bool fast_repaint;    /* true = canvas-only blit (freehand drawing) */

    /* Deferred canvas input (set by event handler, consumed by main loop) */
    volatile bool    input_down;      /* button pressed on canvas */
    volatile bool    input_up;        /* button released */
    volatile bool    input_moved;     /* mouse moved during drawing */
    volatile int16_t input_cx, input_cy; /* latest canvas coords */
    volatile bool    input_right;     /* right button */

    /* Deferred commands (set by event handler, consumed by main loop) */
    volatile uint16_t deferred_cmd;   /* WM_COMMAND id to process (0=none) */

    /* Cached layout (client coords) */
    int16_t    view_x, view_y, view_w, view_h;

    /* Cached framebuffer for direct writes during freehand drawing */
    uint8_t   *show_fb;
    int16_t    fb_stride;
} paintbrush_t;

extern paintbrush_t pb;
extern void *app_task;
extern volatile bool app_closing;

/*==========================================================================
 * CGA palette — BMP format (BGRA)
 *=========================================================================*/

extern const uint8_t bmp_pal[16][4];
extern const uint8_t cga_rgb[16][3];

/*==========================================================================
 * pb_canvas.c — drawing primitives
 *=========================================================================*/

void    canvas_set(int16_t x, int16_t y, uint8_t c);
uint8_t canvas_get(int16_t x, int16_t y);

void stamp_brush_circle(int16_t cx, int16_t cy, uint8_t color, uint8_t radius);
void stamp_brush_square(int16_t cx, int16_t cy, uint8_t color, uint8_t size);
void stamp_eraser(int16_t cx, int16_t cy, uint8_t color, uint8_t size);
void draw_line_bresenham(int16_t x0, int16_t y0, int16_t x1, int16_t y1,
                         uint8_t color, uint8_t width);
void draw_rect_outline(int16_t x0, int16_t y0, int16_t x1, int16_t y1,
                        uint8_t color, uint8_t width);
void draw_rect_filled(int16_t x0, int16_t y0, int16_t x1, int16_t y1,
                       uint8_t color);
void draw_ellipse(int16_t x0, int16_t y0, int16_t x1, int16_t y1,
                   uint8_t color, bool filled);
void draw_rounded_rect(int16_t x0, int16_t y0, int16_t x1, int16_t y1,
                        uint8_t color, uint8_t width, bool filled);
void flood_fill(int16_t sx, int16_t sy, uint8_t nc);
void airbrush_spray(int16_t cx, int16_t cy, uint8_t color, uint8_t diameter);

/* Undo */
void save_undo(void);
void restore_undo(void);

/* Text rendering to canvas (6x8 embedded font) */
void canvas_text(int16_t x, int16_t y, const char *str, uint8_t color);

/*==========================================================================
 * pb_ui.c — toolbar, palette, sub-options rendering
 *=========================================================================*/

void compute_layout(void);
bool mouse_to_canvas(int16_t mx, int16_t my, int16_t *cx, int16_t *cy);
void paint_tool_panel(void);
void paint_sub_options(void);
void paint_palette(void);
void paint_canvas(void);
void paint_shape_preview(void);

int  hit_tool(int16_t mx, int16_t my);
int  hit_sub_option(int16_t mx, int16_t my);
int  hit_palette_color(int16_t mx, int16_t my);

/*==========================================================================
 * pb_icons.c — tool icon bitmaps
 *=========================================================================*/

extern const uint8_t *tool_icons[TOOL_COUNT];

/*==========================================================================
 * pb_bmp.c — BMP file I/O
 *=========================================================================*/

bool pb_save_bmp(const char *path);
bool pb_load_bmp(const char *path);

#endif /* PB_H */
