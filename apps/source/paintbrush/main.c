/*
 * FRANK OS — Paintbrush (Windows 95 MS Paint clone)
 * Copyright (c) 2026 Mikhail Matveev <xtreme@rh1.tech>
 * https://rh1.tech
 *
 * Freehand drawing uses the solitaire timer pattern:
 *   - Event handler writes to canvas buffer only (lightweight)
 *   - 16ms timer calls wm_invalidate() to refresh display at ~60fps
 *   - Paint handler blits canvas to screen via wd_fb_ptr
 *   - Timer slows to 1s when not drawing
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "pb.h"

paintbrush_t pb;
void *app_task;
volatile bool app_closing;
static TimerHandle_t pb_timer;

static void pb_timer_cb(TimerHandle_t xTimer) {
    (void)xTimer;
    if (pb.tool == TOOL_TEXT && pb.drawing)
        wm_invalidate(pb.hwnd);  /* text cursor blink only */
}
/*==========================================================================
 * Helpers
 *=========================================================================*/

static bool is_freehand_tool(uint8_t tool) {
    return tool == TOOL_PENCIL || tool == TOOL_BRUSH ||
           tool == TOOL_ERASER || tool == TOOL_AIRBRUSH;
}

static bool is_shape_tool(uint8_t tool) {
    return tool == TOOL_LINE || tool == TOOL_RECT ||
           tool == TOOL_ELLIPSE;
}

static uint8_t get_stroke_width(void) {
    return line_widths[pb.line_width_idx];
}

/*==========================================================================
 * Direct screen pixel write — bypasses compositor, no flicker.
 * Writes to cached FB pointer from last full paint.
 *=========================================================================*/

/*==========================================================================
 * Direct canvas-to-screen blit using cached FB pointer.
 * Bypasses compositor entirely — zero flicker.
 * Same pixel packing as paint_canvas() but uses cached show_fb.
 *=========================================================================*/

/* Blit a dirty rectangle from canvas to screen FB (canvas coords) */
static void blit_rect_to_screen(int16_t cx0, int16_t cy0, int16_t cx1, int16_t cy1) {
    if (!pb.show_fb) return;
    /* Clamp to canvas */
    if (cx0 < 0) cx0 = 0;
    if (cy0 < 0) cy0 = 0;
    if (cx1 >= pb.canvas_w) cx1 = pb.canvas_w - 1;
    if (cy1 >= pb.canvas_h) cy1 = pb.canvas_h - 1;
    if (cx0 > cx1 || cy0 > cy1) return;
    /* Clamp to visible scroll region */
    if (cx0 < pb.scroll_x) cx0 = pb.scroll_x;
    if (cy0 < pb.scroll_y) cy0 = pb.scroll_y;
    if (cx1 >= pb.scroll_x + pb.view_w) cx1 = pb.scroll_x + pb.view_w - 1;
    if (cy1 >= pb.scroll_y + pb.view_h) cy1 = pb.scroll_y + pb.view_h - 1;
    if (cx0 > cx1 || cy0 > cy1) return;

    int16_t sy;
    for (sy = cy0; sy <= cy1; sy++) {
        int16_t scr_y = sy - pb.scroll_y + pb.view_y;
        uint8_t *fb_base = pb.show_fb + (int32_t)scr_y * pb.fb_stride;
        const uint8_t *src_row = &pb.canvas[(int32_t)sy * pb.canvas_w];
        int16_t sx;
        for (sx = cx0; sx <= cx1; sx++) {
            int16_t scr_x = sx - pb.scroll_x + pb.view_x;
            uint8_t c = src_row[sx];
            if (scr_x & 1)
                fb_base[scr_x >> 1] = (fb_base[scr_x >> 1] & 0xF0) | (c & 0x0F);
            else
                fb_base[scr_x >> 1] = (c << 4) | (fb_base[scr_x >> 1] & 0x0F);
        }
    }
}

/* Draw a single pixel on the screen FB (for shape previews) */
static void screen_put(int16_t cx, int16_t cy, uint8_t color) {
    if (!pb.show_fb) return;
    int16_t sx = cx - pb.scroll_x + pb.view_x;
    int16_t sy = cy - pb.scroll_y + pb.view_y;
    if (sx < pb.view_x || sx >= pb.view_x + pb.view_w) return;
    if (sy < pb.view_y || sy >= pb.view_y + pb.view_h) return;
    uint8_t *row = pb.show_fb + (int32_t)sy * pb.fb_stride;
    if (sx & 1)
        row[sx >> 1] = (row[sx >> 1] & 0xF0) | (color & 0x0F);
    else
        row[sx >> 1] = (color << 4) | (row[sx >> 1] & 0x0F);
}

/* Draw shape preview line on screen (for line/rect/ellipse preview) */
static void screen_line(int16_t x0, int16_t y0, int16_t x1, int16_t y1, uint8_t c) {
    int16_t adx = x1 - x0, ady = y1 - y0;
    int16_t asx = 1, asy = 1;
    if (adx < 0) { adx = -adx; asx = -1; }
    if (ady < 0) { ady = -ady; asy = -1; }
    int16_t er = adx - ady;
    int lim = adx + ady + 1;
    if (lim > 800) lim = 800;
    while (lim-- > 0) {
        screen_put(x0, y0, c);
        if (x0 == x1 && y0 == y1) break;
        int16_t e2 = 2 * er;
        if (e2 > -ady) { er -= ady; x0 += asx; }
        if (e2 < adx)  { er += adx; y0 += asy; }
    }
}

static void screen_rect(int16_t x0, int16_t y0, int16_t x1, int16_t y1, uint8_t c) {
    int16_t t;
    if (x0 > x1) { t = x0; x0 = x1; x1 = t; }
    if (y0 > y1) { t = y0; y0 = y1; y1 = t; }
    int16_t x, y;
    for (x = x0; x <= x1; x++) { screen_put(x, y0, c); screen_put(x, y1, c); }
    for (y = y0+1; y < y1; y++) { screen_put(x0, y, c); screen_put(x1, y, c); }
}

/*==========================================================================
 * Shape commit (on mouse-up)
 *=========================================================================*/

static void commit_shape(int16_t x0, int16_t y0, int16_t x1, int16_t y1,
                          uint8_t fg, uint8_t bg) {
    uint8_t w = get_stroke_width();
    if (pb.tool == TOOL_LINE) {
        draw_line_bresenham(x0, y0, x1, y1, fg, w);
    } else if (pb.tool == TOOL_RECT) {
        if (pb.fill_mode == FILL_OUTLINE) draw_rect_outline(x0, y0, x1, y1, fg, w);
        else if (pb.fill_mode == FILL_OUTLINE_FILL) { draw_rect_filled(x0, y0, x1, y1, bg); draw_rect_outline(x0, y0, x1, y1, fg, w); }
        else draw_rect_filled(x0, y0, x1, y1, fg);
    } else if (pb.tool == TOOL_ELLIPSE) {
        if (pb.fill_mode == FILL_OUTLINE) draw_ellipse(x0, y0, x1, y1, fg, false);
        else if (pb.fill_mode == FILL_OUTLINE_FILL) { draw_ellipse(x0, y0, x1, y1, bg, true); draw_ellipse(x0, y0, x1, y1, fg, false); }
        else draw_ellipse(x0, y0, x1, y1, fg, true);
    }
}

/*==========================================================================
 * Title / file / actions / menus
 *=========================================================================*/

static void pb_update_title(void) {
    window_t *win = wm_get_window(pb.hwnd);
    if (!win) return;
    const char *name = "Untitled";
    if (pb.filepath[0]) {
        const char *p = pb.filepath, *slash = p;
        while (*p) { if (*p == '/') slash = p + 1; p++; }
        name = slash;
    }
    char title[24]; int n = 0;
    if (pb.modified) title[n++] = '*';
    const char *s = name;
    while (*s && n < 13) title[n++] = *s++;
    const char *suf = " - Paintbrush";
    while (*suf && n < 23) title[n++] = *suf++;
    title[n] = '\0';
    memcpy(win->title, title, n + 1);
}

static void pb_do_save_as(void);
static void pb_setup_menu(void);

static void pb_do_new(void) {
    memset(pb.canvas, COLOR_WHITE, (int32_t)pb.canvas_w * pb.canvas_h);
    pb.filepath[0] = '\0'; pb.modified = false; pb.has_undo = false;
    pb.scroll_x = 0; pb.scroll_y = 0; pb.drawing = false;
    pb_update_title(); pb_setup_menu();
    wm_invalidate(pb.hwnd); taskbar_invalidate();
}
static void pb_do_open(void) {
    char dir[PB_PATH_MAX];
    if (pb.filepath[0]) { strncpy(dir, pb.filepath, PB_PATH_MAX - 1); dir[PB_PATH_MAX - 1] = '\0';
        char *sl = dir, *p = dir; while (*p) { if (*p == '/') sl = p; p++; }
        if (sl == dir) { dir[0] = '/'; dir[1] = '\0'; } else *sl = '\0';
    } else { dir[0] = '/'; dir[1] = '\0'; }
    file_dialog_open(pb.hwnd, L(STR_APP_OPEN), dir, NULL);  /* show all files */
}
static void pb_do_save(void) {
    if (pb.filepath[0]) {
        if (pb_save_bmp(pb.filepath)) { pb.modified = false; pb_update_title(); }
        else dialog_show(pb.hwnd, "Paintbrush", "Failed to save file.", DLG_ICON_ERROR, DLG_BTN_OK);
    } else pb_do_save_as();
}
static void pb_do_save_as(void) {
    char dir[PB_PATH_MAX];
    if (pb.filepath[0]) { strncpy(dir, pb.filepath, PB_PATH_MAX - 1); dir[PB_PATH_MAX - 1] = '\0';
        char *sl = dir, *p = dir; while (*p) { if (*p == '/') sl = p; p++; }
        if (sl == dir) { dir[0] = '/'; dir[1] = '\0'; } else *sl = '\0';
    } else { dir[0] = '/'; dir[1] = '\0'; }
    const char *fn = NULL;
    if (pb.filepath[0]) { const char *p = pb.filepath, *s = p; while (*p) { if (*p == '/') s = p + 1; p++; } fn = s; }
    file_dialog_save(pb.hwnd, L(STR_APP_SAVE_AS), dir, "bmp", fn ? fn : "untitled.bmp");
}
static void pb_do_exit(void) { app_closing = true; wm_destroy_window(pb.hwnd); xTaskNotifyGive(app_task); }
static void pb_prompt_save(uint8_t pending) {
    pb.pending_action = pending;
    dialog_show(pb.hwnd, "Paintbrush", AL(AL_SAVE_CHANGES),
                DLG_ICON_WARNING, DLG_BTN_YES | DLG_BTN_NO | DLG_BTN_CANCEL);
}
static void pb_resume_pending(void) {
    uint8_t a = pb.pending_action; pb.pending_action = PENDING_NONE;
    if (a == PENDING_NEW) pb_do_new(); else if (a == PENDING_OPEN) pb_do_open(); else if (a == PENDING_EXIT) pb_do_exit();
}

static void pb_setup_menu(void) {
    menu_bar_t bar; memset(&bar, 0, sizeof(bar)); bar.menu_count = 4;
    menu_def_t *m;
    m = &bar.menus[0]; strncpy(m->title, L(STR_FILE), sizeof(m->title) - 1); m->accel_key = 0x09; m->item_count = 6;
    strncpy(m->items[0].text, AL(AL_NEW_MENU), sizeof(m->items[0].text) - 1); m->items[0].command_id = CMD_NEW; m->items[0].accel_key = 0x11;
    strncpy(m->items[1].text, AL(AL_OPEN_MENU), sizeof(m->items[1].text) - 1); m->items[1].command_id = CMD_OPEN; m->items[1].accel_key = 0x12;
    strncpy(m->items[2].text, AL(AL_SAVE_MENU), sizeof(m->items[2].text) - 1); m->items[2].command_id = CMD_SAVE; m->items[2].accel_key = 0x16;
    strncpy(m->items[3].text, L(STR_APP_SAVE_AS), sizeof(m->items[3].text) - 1); m->items[3].command_id = CMD_SAVE_AS;
    m->items[4].flags = MIF_SEPARATOR;
    strncpy(m->items[5].text, L(STR_FM_EXIT), sizeof(m->items[5].text) - 1); m->items[5].command_id = CMD_EXIT;
    m = &bar.menus[1]; strncpy(m->title, L(STR_EDIT), sizeof(m->title) - 1); m->accel_key = 0x08; m->item_count = 8;
    strncpy(m->items[0].text, "Undo   Ctrl+Z", 19); m->items[0].command_id = CMD_UNDO; m->items[0].accel_key = 0x1D;
    if (!pb.has_undo) m->items[0].flags = MIF_DISABLED;
    m->items[1].flags = MIF_SEPARATOR;
    strncpy(m->items[2].text, L(STR_FM_CUT_MENU), sizeof(m->items[2].text) - 1); m->items[2].command_id = CMD_CUT; m->items[2].accel_key = 0x1B;
    if (!pb.has_selection) m->items[2].flags = MIF_DISABLED;
    strncpy(m->items[3].text, L(STR_FM_COPY_MENU), sizeof(m->items[3].text) - 1); m->items[3].command_id = CMD_COPY; m->items[3].accel_key = 0x06;
    if (!pb.has_selection) m->items[3].flags = MIF_DISABLED;
    strncpy(m->items[4].text, L(STR_FM_PASTE_MENU), sizeof(m->items[4].text) - 1); m->items[4].command_id = CMD_PASTE; m->items[4].accel_key = 0x19;
    if (!pb.sel_buf) m->items[4].flags = MIF_DISABLED;
    strncpy(m->items[5].text, L(STR_FM_SELALL_MENU), sizeof(m->items[5].text) - 1); m->items[5].command_id = CMD_SELECT_ALL; m->items[5].accel_key = 0x04;
    m->items[6].flags = MIF_SEPARATOR;
    strncpy(m->items[7].text, "Clear Image", 19); m->items[7].command_id = CMD_CLEAR;
    m = &bar.menus[2]; strncpy(m->title, AL(AL_IMAGE), sizeof(m->title) - 1); m->accel_key = 0x0C; m->item_count = 3;
    strncpy(m->items[0].text, "Flip Horiz.", 19); m->items[0].command_id = CMD_FLIP_H;
    strncpy(m->items[1].text, "Flip Vert.", 19); m->items[1].command_id = CMD_FLIP_V;
    strncpy(m->items[2].text, "Invert Colors", 19); m->items[2].command_id = CMD_INVERT;
    m = &bar.menus[3]; strncpy(m->title, L(STR_HELP), sizeof(m->title) - 1); m->accel_key = 0x0B; m->item_count = 1;
    strncpy(m->items[0].text, L(STR_FM_ABOUT_MENU), sizeof(m->items[0].text) - 1); m->items[0].command_id = CMD_ABOUT; m->items[0].accel_key = 0x3A;
    menu_set(pb.hwnd, &bar);
}

static void mark_modified(void) {
    if (!pb.modified) { pb.modified = true; pb_update_title(); pb_setup_menu(); }
}

/*==========================================================================
 * Selection / clipboard helpers
 *=========================================================================*/

static void normalize_rect(int16_t *x, int16_t *y, int16_t *w, int16_t *h,
                            int16_t x0, int16_t y0, int16_t x1, int16_t y1) {
    if (x0 > x1) { *x = x1; *w = x0 - x1 + 1; } else { *x = x0; *w = x1 - x0 + 1; }
    if (y0 > y1) { *y = y1; *h = y0 - y1 + 1; } else { *y = y0; *h = y1 - y0 + 1; }
}

static void sel_copy(void) {
    if (!pb.has_selection) return;
    int16_t sx = pb.sel_x, sy = pb.sel_y, sw = pb.sel_w, sh = pb.sel_h;
    if (pb.sel_buf) free(pb.sel_buf);
    pb.sel_buf = (uint8_t *)malloc((int32_t)sw * sh);
    if (!pb.sel_buf) return;
    pb.sel_buf_w = sw; pb.sel_buf_h = sh;
    int16_t y, x;
    for (y = 0; y < sh; y++)
        for (x = 0; x < sw; x++)
            pb.sel_buf[(int32_t)y * sw + x] = canvas_get(sx + x, sy + y);
}

static void sel_cut(void) {
    sel_copy();
    if (!pb.sel_buf) return;
    int16_t sx = pb.sel_x, sy = pb.sel_y, sw = pb.sel_w, sh = pb.sel_h;
    int16_t y, x;
    for (y = 0; y < sh; y++)
        for (x = 0; x < sw; x++)
            canvas_set(sx + x, sy + y, pb.bg_color);
    pb.has_selection = false;
    mark_modified();
}

static void sel_paste_commit(void) {
    /* Paste sel_buf at float_x/float_y */
    if (!pb.sel_buf) return;
    save_undo();
    int16_t y, x;
    for (y = 0; y < pb.sel_buf_h; y++)
        for (x = 0; x < pb.sel_buf_w; x++) {
            uint8_t c = pb.sel_buf[(int32_t)y * pb.sel_buf_w + x];
            canvas_set(pb.float_x + x, pb.float_y + y, c);
        }
    mark_modified();
}

static void sel_start_floating(void) {
    if (!pb.sel_buf) return;
    pb.floating = true;
    pb.float_x = 0; pb.float_y = 0;
}

/*==========================================================================
 * Paint handler — full repaint, called by compositor on wm_invalidate()
 *=========================================================================*/

static bool paint_initialized = false;

static void pb_paint(hwnd_t hwnd) {
    wd_begin(hwnd);
    compute_layout();
    paint_tool_panel();
    paint_sub_options();
    paint_canvas();
    paint_shape_preview();
    if (pb.hscroll.visible) scrollbar_paint(&pb.hscroll);
    if (pb.vscroll.visible) scrollbar_paint(&pb.vscroll);
    if (pb.hscroll.visible && pb.vscroll.visible) {
        rect_t cr = wm_get_client_rect(pb.hwnd);
        wd_fill_rect(cr.w - SCROLLBAR_WIDTH, cr.h - PALETTE_H - SCROLLBAR_WIDTH,
                     SCROLLBAR_WIDTH, SCROLLBAR_WIDTH, THEME_BUTTON_FACE);
    }
    paint_palette();
    paint_initialized = true;
    wd_end();
}

/*==========================================================================
 * Event handler — lightweight. Canvas writes only, NO wm_invalidate
 * during freehand. Timer drives display refresh at 60fps.
 *=========================================================================*/

static bool pb_event(hwnd_t hwnd, const window_event_t *ev) {

    if (ev->type == WM_CLOSE) {
        if (pb.modified) pb_prompt_save(PENDING_EXIT); else pb_do_exit();
        return true;
    }
    if (ev->type == WM_SIZE) { compute_layout(); pb.drawing = false; wm_invalidate(pb.hwnd); return true; }

    /* ---- Mouse down ---- */
    if (ev->type == WM_LBUTTONDOWN || ev->type == WM_RBUTTONDOWN) {
        int16_t mx = ev->mouse.x, my = ev->mouse.y;
        bool is_right = (ev->type == WM_RBUTTONDOWN);

        if (!is_right) {
            int t = hit_tool(mx, my);
            if (t >= 0) {
                pb.drawing = false;  /* cancel any active drawing/text mode */
                xTimerStop(pb_timer, 0);
                pb.tool = (uint8_t)t;
                wm_invalidate(hwnd);
                return true;
            }
            int so = hit_sub_option(mx, my);
            if (so >= 0) {
                if (pb.tool == TOOL_LINE) pb.line_width_idx = (uint8_t)so;
                else if (pb.tool == TOOL_ERASER) pb.eraser_size_idx = (uint8_t)so;
                else if (pb.tool == TOOL_BRUSH) pb.brush_size_idx = (uint8_t)so;
                else if (pb.tool == TOOL_AIRBRUSH) pb.airbrush_size_idx = (uint8_t)so;
                else if (pb.tool == TOOL_RECT || pb.tool == TOOL_ELLIPSE) pb.fill_mode = (uint8_t)so;
                wm_invalidate(hwnd); return true;
            }
        }
        /* FG/BG swap: click on the FG/BG indicator box */
        { rect_t cr = wm_get_client_rect(hwnd);
          int16_t py = cr.h - PALETTE_H;
          if (mx >= 3 && mx < 3 + FGBG_BOX && my >= py + 3 && my < py + 3 + FGBG_BOX) {
              uint8_t tmp = pb.fg_color; pb.fg_color = pb.bg_color; pb.bg_color = tmp;
              wm_invalidate(hwnd); return true;
          }
        }
        { int ci = hit_palette_color(mx, my);
          if (ci >= 0) { if (is_right) pb.bg_color = (uint8_t)ci; else pb.fg_color = (uint8_t)ci; wm_invalidate(hwnd); return true; } }
        if (pb.hscroll.visible) { int32_t np; if (scrollbar_event(&pb.hscroll, ev, &np)) { pb.scroll_x = (int16_t)np; wm_invalidate(hwnd); return true; } }
        if (pb.vscroll.visible) { int32_t np; if (scrollbar_event(&pb.vscroll, ev, &np)) { pb.scroll_y = (int16_t)np; wm_invalidate(hwnd); return true; } }

        int16_t cx, cy;
        if (mouse_to_canvas(mx, my, &cx, &cy)) {
            pb.draw_color = is_right ? pb.bg_color : pb.fg_color;

            if (pb.tool == TOOL_FILL) {
                pb.input_cx = cx; pb.input_cy = cy;
                pb.deferred_cmd = CMD_FILL_DEFERRED; xTaskNotifyGive(app_task);
                return true;
            }
            if (pb.tool == TOOL_PICKER) {
                uint8_t picked = canvas_get(cx, cy);
                if (is_right) pb.bg_color = picked; else pb.fg_color = picked;
                wm_invalidate(hwnd); return true;
            }
            /* Floating paste: click to commit at current position */
            if (pb.floating) {
                pb.deferred_cmd = CMD_PASTE; xTaskNotifyGive(app_task);
                pb.floating = false;
                pb.has_selection = false;
                return true;
            }

            if (pb.tool == TOOL_SELECT) {
                /* Start selection rectangle drag */
                pb.drawing = true;
                pb.has_selection = false;
                pb.start_x = cx; pb.start_y = cy;
                pb.last_x = cx; pb.last_y = cy;
                xTimerChangePeriod(pb_timer, pdMS_TO_TICKS(50), 0); xTimerStart(pb_timer, 0);
                return true;
            }

            /* Text tool */
            if (pb.tool == TOOL_TEXT) {
                if (pb.drawing) {
                    /* Already typing — commit current text and start new */
                    pb.drawing = false;
                    mark_modified();
                }
                pb.deferred_cmd = CMD_SAVE_UNDO; xTaskNotifyGive(app_task);
                pb.text_x = cx; pb.text_y = cy;
                pb.text_buf[0] = '\0';
                pb.drawing = true;
                /* Start blink timer */
                xTimerChangePeriod(pb_timer, pdMS_TO_TICKS(500), 0); xTimerStart(pb_timer, 0);
                wm_invalidate(hwnd);
                return true;
            }

            /* Begin drawing */
            pb.drawing = true;
            pb.start_x = cx; pb.start_y = cy;
            pb.last_x = cx; pb.last_y = cy;

            /* Undo — defer to main loop */
            pb.deferred_cmd = CMD_SAVE_UNDO; xTaskNotifyGive(app_task);

            /* Stamp first pixel/area */
            if (is_freehand_tool(pb.tool)) {
                uint8_t dc = (pb.tool == TOOL_ERASER) ? pb.bg_color : pb.draw_color;
                canvas_set(cx, cy, dc);
            }
            wm_invalidate(hwnd);
            return true;
        }
        return false;
    }

    /* ---- Mouse move ---- */
    if (ev->type == WM_MOUSEMOVE) {
        int16_t mx = ev->mouse.x, my = ev->mouse.y;

        if (pb.hscroll.visible && pb.hscroll.dragging) { int32_t np; if (scrollbar_event(&pb.hscroll, ev, &np)) { pb.scroll_x = (int16_t)np; wm_invalidate(hwnd); return true; } }
        if (pb.vscroll.visible && pb.vscroll.dragging) { int32_t np; if (scrollbar_event(&pb.vscroll, ev, &np)) { pb.scroll_y = (int16_t)np; wm_invalidate(hwnd); return true; } }

        /* Floating paste: track mouse to position paste */
        if (pb.floating) {
            int16_t cx, cy;
            if (mouse_to_canvas(mx, my, &cx, &cy)) {
                pb.float_x = cx; pb.float_y = cy;
                wm_invalidate(hwnd);
            }
            return true;
        }

        if (!pb.drawing) return false;

        int16_t cx, cy;
        if (mouse_to_canvas(mx, my, &cx, &cy)) {
            if (is_freehand_tool(pb.tool)) {
                uint8_t dc = (pb.tool == TOOL_ERASER) ? pb.bg_color : pb.draw_color;
                /* Safe inline Bresenham — write to canvas buffer */
                int16_t lx = pb.last_x, ly = pb.last_y;
                int16_t adx = cx - lx, ady = cy - ly;
                int16_t asx = 1, asy = 1;
                if (adx < 0) { adx = -adx; asx = -1; }
                if (ady < 0) { ady = -ady; asy = -1; }
                int16_t er = adx - ady;
                int lim = adx + ady + 1;
                if (lim > 600) lim = 600;
                while (lim-- > 0) {
                    /* Write to canvas AND screen simultaneously */
                    if (pb.tool == TOOL_PENCIL) {
                        canvas_set(lx, ly, dc);
                        screen_put(lx, ly, dc);
                    } else if (pb.tool == TOOL_ERASER) {
                        uint8_t esz = eraser_sizes[pb.eraser_size_idx];
                        int16_t eh = esz / 2, ey, ex;
                        for (ey = 0; ey < esz; ey++)
                            for (ex = 0; ex < esz; ex++) {
                                canvas_set(lx - eh + ex, ly - eh + ey, dc);
                                screen_put(lx - eh + ex, ly - eh + ey, dc);
                            }
                    } else if (pb.tool == TOOL_AIRBRUSH) {
                        airbrush_spray(lx, ly, dc, airbrush_sizes[pb.airbrush_size_idx]);
                        /* airbrush: screen updates on mouseup */
                    } else if (pb.tool == TOOL_BRUSH) {
                        uint8_t sz = brush_sizes[pb.brush_size_idx];
                        int16_t r = sz / 2, by, bx;
                        if (pb.brush_shape == BRUSH_SQUARE) {
                            for (by = -r; by <= r; by++)
                                for (bx = -r; bx <= r; bx++) {
                                    canvas_set(lx+bx, ly+by, dc);
                                    screen_put(lx+bx, ly+by, dc);
                                }
                        } else {
                            for (by = -r; by <= r; by++)
                                for (bx = -r; bx <= r; bx++)
                                    if (bx*bx + by*by <= r*r) {
                                        canvas_set(lx+bx, ly+by, dc);
                                        screen_put(lx+bx, ly+by, dc);
                                    }
                        }
                    }
                    if (lx == cx && ly == cy) break;
                    int16_t e2 = 2 * er;
                    if (e2 > -ady) { er -= ady; lx += asx; }
                    if (e2 < adx)  { er += adx; ly += asy; }
                }
            }
            /* wm_invalidate triggers fast-path paint (canvas blit only, no flicker) */
            wm_invalidate(hwnd);
            pb.last_x = cx; pb.last_y = cy;
        }
        return true;
    }

    /* ---- Mouse up ---- */
    if (ev->type == WM_LBUTTONUP || ev->type == WM_RBUTTONUP) {
        if (pb.hscroll.visible) { int32_t np; if (scrollbar_event(&pb.hscroll, ev, &np)) pb.scroll_x = (int16_t)np; }
        if (pb.vscroll.visible) { int32_t np; if (scrollbar_event(&pb.vscroll, ev, &np)) pb.scroll_y = (int16_t)np; }

        if (!pb.drawing) return true;

        /* Text tool: keep drawing mode active (typing continues after mouseup) */
        if (pb.tool == TOOL_TEXT) return true;

        /* Stop fast timer */
        xTimerStop(pb_timer, 0);

        pb.drawing = false;

        /* Finalize selection rectangle */
        if (pb.tool == TOOL_SELECT) {
            int16_t cx, cy;
            if (mouse_to_canvas(ev->mouse.x, ev->mouse.y, &cx, &cy)) {
                normalize_rect(&pb.sel_x, &pb.sel_y, &pb.sel_w, &pb.sel_h,
                                pb.start_x, pb.start_y, cx, cy);
                if (pb.sel_w > 1 && pb.sel_h > 1)
                    pb.has_selection = true;
            }
            pb_setup_menu();
            wm_invalidate(hwnd);
            return true;
        }

        if (is_shape_tool(pb.tool)) {
            int16_t cx, cy;
            if (mouse_to_canvas(ev->mouse.x, ev->mouse.y, &cx, &cy))
                commit_shape(pb.start_x, pb.start_y, cx, cy, pb.draw_color, pb.bg_color);
        }
        mark_modified();
        wm_invalidate(hwnd);
        return true;
    }

    /* ---- Commands ---- */
    if (ev->type == WM_COMMAND) {
        uint16_t cmd = ev->command.id;
        if (cmd == CMD_NEW) { if (pb.modified) pb_prompt_save(PENDING_NEW); else pb_do_new(); return true; }
        if (cmd == CMD_OPEN) { if (pb.modified) pb_prompt_save(PENDING_OPEN); else pb_do_open(); return true; }
        if (cmd == CMD_SAVE) { pb_do_save(); return true; }
        if (cmd == CMD_SAVE_AS) { pb_do_save_as(); return true; }
        if (cmd == CMD_EXIT) { if (pb.modified) pb_prompt_save(PENDING_EXIT); else pb_do_exit(); return true; }
        if (cmd == CMD_ABOUT) { dialog_show(hwnd, AL(AL_ABOUT), "Paintbrush\n\nFRANK OS v" FRANK_VERSION_STR "\n(c) 2026 Mikhail Matveev\n<xtreme@rh1.tech>\ngithub.com/rh1tech/frank-os", DLG_ICON_INFO, DLG_BTN_OK); return true; }
        if (cmd == CMD_CUT) { sel_cut(); pb_setup_menu(); wm_invalidate(hwnd); return true; }
        if (cmd == CMD_COPY) { sel_copy(); pb_setup_menu(); wm_invalidate(hwnd); return true; }
        if (cmd == CMD_PASTE) { sel_start_floating(); wm_invalidate(hwnd); return true; }
        if (cmd == CMD_SELECT_ALL) {
            pb.sel_x = 0; pb.sel_y = 0; pb.sel_w = pb.canvas_w; pb.sel_h = pb.canvas_h;
            pb.has_selection = true; pb_setup_menu(); wm_invalidate(hwnd); return true;
        }
        if (cmd == CMD_UNDO || cmd == CMD_CLEAR || cmd == CMD_FLIP_H || cmd == CMD_FLIP_V || cmd == CMD_INVERT) {
            pb.deferred_cmd = cmd; xTaskNotifyGive(app_task); return true;
        }
        if (cmd == DLG_RESULT_FILE) {
            const char *path = file_dialog_get_path();
            if (path && path[0] && pb_load_bmp(path)) {
                strncpy(pb.filepath, path, PB_PATH_MAX - 1); pb.filepath[PB_PATH_MAX - 1] = '\0';
                pb.modified = false; pb.has_undo = false; pb.scroll_x = 0; pb.scroll_y = 0;
                pb_update_title(); pb_setup_menu();
            }
            pb.drawing = false;  /* ensure full repaint with wd_begin */
            wm_invalidate(hwnd); return true;
        }
        if (cmd == DLG_RESULT_FILE_SAVE) {
            const char *path = file_dialog_get_path();
            if (path && path[0] && pb_save_bmp(path)) {
                strncpy(pb.filepath, path, PB_PATH_MAX - 1); pb.filepath[PB_PATH_MAX - 1] = '\0';
                pb.modified = false; pb_update_title();
            }
            if (pb.pending_action != PENDING_NONE) pb_resume_pending();
            wm_invalidate(hwnd); return true;
        }
        if (cmd == DLG_RESULT_YES) { if (pb.filepath[0]) { pb_do_save(); pb_resume_pending(); } else pb_do_save_as(); return true; }
        if (cmd == DLG_RESULT_NO) { pb.modified = false; pb_resume_pending(); wm_invalidate(hwnd); return true; }
        if (cmd == DLG_RESULT_CANCEL) { pb.pending_action = PENDING_NONE; wm_invalidate(hwnd); return true; }
        if (cmd == DLG_RESULT_OK) { wm_invalidate(hwnd); return true; }
        return false;
    }

    /* ---- Text input (WM_CHAR) ---- */
    if (ev->type == WM_CHAR && pb.drawing && pb.tool == TOOL_TEXT) {
        char ch = ev->charev.ch;
        int len = 0;
        while (pb.text_buf[len]) len++;
        if (ch == '\r' || ch == '\n' || ch == 27) {
            /* Enter or Escape — commit text */
            pb.drawing = false;
            xTimerStop(pb_timer, 0);
            mark_modified();
            wm_invalidate(hwnd);
        } else if (ch == '\b') {
            if (len > 0) {
                pb.text_buf[len - 1] = '\0';
                restore_undo();
                canvas_text(pb.text_x, pb.text_y, pb.text_buf, pb.fg_color);
                wm_invalidate(hwnd);
            }
        } else if (ch >= 32 && len < 126) {
            pb.text_buf[len] = ch;
            pb.text_buf[len + 1] = '\0';
            restore_undo();
            canvas_text(pb.text_x, pb.text_y, pb.text_buf, pb.fg_color);
            wm_invalidate(hwnd);
        }
        return true;
    }

    /* ---- Keyboard ---- */
    if (ev->type == WM_KEYDOWN) {
        uint8_t sc = ev->key.scancode, mod = ev->key.modifiers;
        /* Escape/Enter commits text or cancels floating paste */
        if (sc == 0x29 || sc == 0x28 || sc == 0x58) { /* ESC, Enter, Keypad Enter */
            if (pb.drawing && pb.tool == TOOL_TEXT) {
                pb.drawing = false; xTimerStop(pb_timer, 0);
                mark_modified(); wm_invalidate(hwnd); return true;
            }
            if (sc == 0x29 && pb.floating) {
                pb.floating = false; wm_invalidate(hwnd); return true;
            }
        }
        if ((mod & KMOD_CTRL) && sc == 0x1D) { pb.deferred_cmd = CMD_UNDO; xTaskNotifyGive(app_task); return true; } /* Ctrl+Z */
        if ((mod & KMOD_CTRL) && sc == 0x1B) { sel_cut(); pb_setup_menu(); wm_invalidate(hwnd); return true; } /* Ctrl+X */
        if ((mod & KMOD_CTRL) && sc == 0x06) { sel_copy(); pb_setup_menu(); wm_invalidate(hwnd); return true; } /* Ctrl+C */
        if ((mod & KMOD_CTRL) && sc == 0x19) { sel_start_floating(); wm_invalidate(hwnd); return true; } /* Ctrl+V */
        if ((mod & KMOD_CTRL) && sc == 0x04) { /* Ctrl+A */
            pb.sel_x = 0; pb.sel_y = 0; pb.sel_w = pb.canvas_w; pb.sel_h = pb.canvas_h;
            pb.has_selection = true; pb_setup_menu(); wm_invalidate(hwnd); return true;
        }
        if ((mod & KMOD_CTRL) && sc == 0x11) { if (pb.modified) pb_prompt_save(PENDING_NEW); else pb_do_new(); return true; } /* Ctrl+N */
        if ((mod & KMOD_CTRL) && sc == 0x12) { if (pb.modified) pb_prompt_save(PENDING_OPEN); else pb_do_open(); return true; } /* Ctrl+O */
        if ((mod & KMOD_CTRL) && sc == 0x16) { pb_do_save(); return true; } /* Ctrl+S */
        if (sc == 0x3A) { /* F1 */
            window_event_t ce = {0}; ce.type = WM_COMMAND; ce.command.id = CMD_ABOUT;
            wm_post_event(hwnd, &ce); return true;
        }
        return false;
    }

    return false;
}

/*==========================================================================
 * Main loop — deferred heavy ops only
 *=========================================================================*/

int main(int argc, char **argv) {
    app_task = xTaskGetCurrentTaskHandle();
    app_closing = false;
    memset(&pb, 0, sizeof(pb));
    pb.canvas_w = CANVAS_W_DEFAULT; pb.canvas_h = CANVAS_H_DEFAULT;
    pb.fg_color = COLOR_BLACK; pb.bg_color = COLOR_WHITE; pb.tool = TOOL_PENCIL;

    int32_t canvas_sz = (int32_t)pb.canvas_w * pb.canvas_h;
    pb.canvas = (uint8_t *)malloc(canvas_sz);
    pb.undo_buf = (uint8_t *)malloc(canvas_sz);
    if (!pb.canvas || !pb.undo_buf) { free(pb.canvas); free(pb.undo_buf); return 1; }
    memset(pb.canvas, COLOR_WHITE, canvas_sz);
    memset(pb.undo_buf, COLOR_WHITE, canvas_sz);

    int16_t cw = TOOL_PANEL_W + pb.canvas_w + SCROLLBAR_WIDTH;
    int16_t ch = pb.canvas_h + SCROLLBAR_WIDTH + PALETTE_H;
    int16_t fw = cw + 2 * THEME_BORDER_WIDTH;
    int16_t fh = ch + THEME_TITLE_HEIGHT + THEME_MENU_HEIGHT + 2 * THEME_BORDER_WIDTH;
    if (fw > DISPLAY_WIDTH) fw = DISPLAY_WIDTH;
    if (fh > DISPLAY_HEIGHT - TASKBAR_HEIGHT) fh = DISPLAY_HEIGHT - TASKBAR_HEIGHT;
    int16_t wx = (DISPLAY_WIDTH - fw) / 2, wy = (DISPLAY_HEIGHT - TASKBAR_HEIGHT - fh) / 2;
    if (wy < 0) wy = 0;

    pb.hwnd = wm_create_window(wx, wy, fw, fh, "Untitled - Paintbrush",
                                WSTYLE_DEFAULT | WF_MENUBAR, pb_event, pb_paint);
    if (pb.hwnd == HWND_NULL) { free(pb.canvas); free(pb.undo_buf); return 1; }

    window_t *win = wm_get_window(pb.hwnd);
    if (win) win->bg_color = THEME_BUTTON_FACE;
    scrollbar_init(&pb.hscroll, true); scrollbar_init(&pb.vscroll, false);
    pb_setup_menu(); compute_layout();

    pb_timer = xTimerCreate("pb_tmr", pdMS_TO_TICKS(50), pdTRUE, NULL, pb_timer_cb);
    /* Timer not started — only runs during drawing */

    wm_show_window(pb.hwnd); wm_set_focus(pb.hwnd); taskbar_invalidate();

    if (argc > 1 && argv[1] && argv[1][0]) {
        if (pb_load_bmp(argv[1])) {
            strncpy(pb.filepath, argv[1], PB_PATH_MAX - 1); pb.filepath[PB_PATH_MAX - 1] = '\0';
            pb.modified = false; pb_update_title(); compute_layout();
            wm_invalidate(pb.hwnd);
        }
    }

    while (!app_closing) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        uint16_t cmd = pb.deferred_cmd; pb.deferred_cmd = 0;

        /* Ensure full repaint for any deferred visual update */
        if (cmd && cmd != CMD_SAVE_UNDO)
            pb.drawing = false;

        if (cmd == CMD_SAVE_UNDO) {
            save_undo();
        } else if (cmd == CMD_PASTE) {
            sel_paste_commit(); pb_setup_menu(); wm_invalidate(pb.hwnd);
        } else if (cmd == CMD_FILL_DEFERRED) {
            save_undo(); flood_fill(pb.input_cx, pb.input_cy, pb.draw_color);
            mark_modified(); wm_invalidate(pb.hwnd);
        } else if (cmd == CMD_UNDO && pb.has_undo) {
            restore_undo(); pb.has_undo = false; pb.modified = true;
            pb_update_title(); pb_setup_menu(); wm_invalidate(pb.hwnd);
        } else if (cmd == CMD_CLEAR) {
            save_undo(); memset(pb.canvas, COLOR_WHITE, (int32_t)pb.canvas_w * pb.canvas_h);
            mark_modified(); wm_invalidate(pb.hwnd);
        } else if (cmd == CMD_FLIP_H) {
            save_undo(); int16_t w = pb.canvas_w, h = pb.canvas_h, x, y;
            for (y = 0; y < h; y++) for (x = 0; x < w / 2; x++) {
                int32_t a = (int32_t)y * w + x, b = (int32_t)y * w + (w - 1 - x);
                uint8_t t = pb.canvas[a]; pb.canvas[a] = pb.canvas[b]; pb.canvas[b] = t;
            }
            mark_modified(); wm_invalidate(pb.hwnd);
        } else if (cmd == CMD_FLIP_V) {
            save_undo(); int16_t w = pb.canvas_w, h = pb.canvas_h, x, y;
            for (y = 0; y < h / 2; y++) for (x = 0; x < w; x++) {
                int32_t a = (int32_t)y * w + x, b = (int32_t)(h - 1 - y) * w + x;
                uint8_t t = pb.canvas[a]; pb.canvas[a] = pb.canvas[b]; pb.canvas[b] = t;
            }
            mark_modified(); wm_invalidate(pb.hwnd);
        } else if (cmd == CMD_INVERT) {
            save_undo(); int32_t sz = (int32_t)pb.canvas_w * pb.canvas_h, i;
            for (i = 0; i < sz; i++) pb.canvas[i] = 15 - pb.canvas[i];
            mark_modified(); wm_invalidate(pb.hwnd);
        }
    }

    xTimerStop(pb_timer, 0); xTimerDelete(pb_timer, 0);
    free(pb.sel_buf);
    free(pb.canvas); free(pb.undo_buf);
    return 0;
}
