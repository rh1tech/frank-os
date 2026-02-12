/*
 * FRANK OS
 * Copyright (c) 2025 Mikhail Matveev <xtreme@rh1.tech>
 * https://rh1.tech
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "sysmenu.h"
#include "window.h"
#include "window_event.h"
#include "window_theme.h"
#include "gfx.h"
#include "font.h"
#include <string.h>

/*==========================================================================
 * System menu items
 *=========================================================================*/

#define SYS_ID_MINIMIZE   1
#define SYS_ID_MAXIMIZE   2
#define SYS_ID_RESTORE    3
#define SYS_ID_CLOSE      4

#define SYS_ITEM_HEIGHT  20
#define SYS_SEPARATOR_H   8
#define SYS_MENU_WIDTH   120
#define SYS_SHADOW         1

typedef struct {
    const char *text;
    uint8_t     id;
    bool        separator_above;
} sys_item_t;

/* Items change based on window state */
static sys_item_t sys_items[4];
static int sys_item_count;

/*==========================================================================
 * Internal state
 *=========================================================================*/

static bool    sys_open = false;
static hwnd_t  sys_hwnd = HWND_NULL;
static int8_t  sys_hover = -1;
static int16_t sys_x, sys_y, sys_w, sys_h;

static void build_items(void) {
    window_t *win = wm_get_window(sys_hwnd);
    if (!win) return;

    sys_item_count = 0;

    if (win->state == WS_MAXIMIZED && (win->flags & WF_RESIZABLE)) {
        sys_items[sys_item_count++] = (sys_item_t){ "Restore", SYS_ID_RESTORE, false };
    }
    if ((win->flags & WF_RESIZABLE) && win->state != WS_MAXIMIZED) {
        sys_items[sys_item_count++] = (sys_item_t){ "Maximize", SYS_ID_MAXIMIZE, false };
    }
    /* Minimize available for all closable windows */
    if (win->flags & WF_CLOSABLE) {
        sys_items[sys_item_count++] = (sys_item_t){ "Minimize", SYS_ID_MINIMIZE, false };
    }
    if (win->flags & WF_CLOSABLE) {
        sys_items[sys_item_count++] = (sys_item_t){ "Close", SYS_ID_CLOSE, true };
    }
}

static void compute_rect(void) {
    window_t *win = wm_get_window(sys_hwnd);
    if (!win) return;

    sys_w = SYS_MENU_WIDTH;
    sys_h = 4;
    for (int i = 0; i < sys_item_count; i++) {
        if (sys_items[i].separator_above) sys_h += SYS_SEPARATOR_H;
        sys_h += SYS_ITEM_HEIGHT;
    }
    sys_x = win->frame.x + THEME_BORDER_WIDTH;
    sys_y = win->frame.y + THEME_BORDER_WIDTH + THEME_TITLE_HEIGHT;
}

/*==========================================================================
 * Public API
 *=========================================================================*/

void sysmenu_open(hwnd_t hwnd) {
    if (sys_open && sys_hwnd == hwnd) {
        sysmenu_close();
        return;
    }
    sys_hwnd = hwnd;
    sys_open = true;
    build_items();
    sys_hover = (sys_item_count > 0) ? 0 : -1;
    compute_rect();
    wm_mark_dirty();
}

void sysmenu_close(void) {
    sys_open = false;
    sys_hwnd = HWND_NULL;
    sys_hover = -1;
    wm_mark_dirty();
}

bool sysmenu_is_open(void) {
    return sys_open;
}

/*==========================================================================
 * Action handler
 *=========================================================================*/

static void execute(uint8_t id) {
    hwnd_t hwnd = sys_hwnd;
    sysmenu_close();

    switch (id) {
    case SYS_ID_MINIMIZE:
        wm_minimize_window(hwnd);
        break;
    case SYS_ID_MAXIMIZE:
        wm_maximize_window(hwnd);
        break;
    case SYS_ID_RESTORE:
        wm_restore_window(hwnd);
        break;
    case SYS_ID_CLOSE: {
        window_event_t ev;
        memset(&ev, 0, sizeof(ev));
        ev.type = WM_CLOSE;
        wm_post_event(hwnd, &ev);
        break;
    }
    }
    wm_mark_dirty();
}

/*==========================================================================
 * Rendering
 *=========================================================================*/

void sysmenu_draw(void) {
    if (!sys_open) return;

    /* Shadow (dithered for semi-transparency) */
    gfx_fill_rect_dithered(sys_x + SYS_SHADOW, sys_y + SYS_SHADOW,
                           sys_w, sys_h, COLOR_BLACK);

    /* Background */
    gfx_fill_rect(sys_x, sys_y, sys_w, sys_h, THEME_BUTTON_FACE);

    /* Border */
    gfx_hline(sys_x, sys_y, sys_w, COLOR_WHITE);
    gfx_vline(sys_x, sys_y, sys_h, COLOR_WHITE);
    gfx_hline(sys_x, sys_y + sys_h - 1, sys_w, COLOR_BLACK);
    gfx_vline(sys_x + sys_w - 1, sys_y, sys_h, COLOR_BLACK);
    gfx_hline(sys_x + 1, sys_y + sys_h - 2, sys_w - 2, COLOR_DARK_GRAY);
    gfx_vline(sys_x + sys_w - 2, sys_y + 1, sys_h - 2, COLOR_DARK_GRAY);

    int iy = sys_y + 2;
    for (int i = 0; i < sys_item_count; i++) {
        if (sys_items[i].separator_above) {
            int sep_y = iy + SYS_SEPARATOR_H / 2;
            gfx_hline(sys_x + 2, sep_y - 1, sys_w - 4, COLOR_DARK_GRAY);
            gfx_hline(sys_x + 2, sep_y, sys_w - 4, COLOR_WHITE);
            iy += SYS_SEPARATOR_H;
        }

        bool hovered = (i == sys_hover);
        uint8_t bg = hovered ? COLOR_BLUE : THEME_BUTTON_FACE;
        uint8_t fg = hovered ? COLOR_WHITE : COLOR_BLACK;

        gfx_fill_rect(sys_x + 2, iy, sys_w - 4, SYS_ITEM_HEIGHT, bg);
        gfx_text_ui(sys_x + 6, iy + (SYS_ITEM_HEIGHT - FONT_UI_HEIGHT) / 2,
                    sys_items[i].text, fg, bg);
        iy += SYS_ITEM_HEIGHT;
    }
}

/*==========================================================================
 * Mouse handling
 *=========================================================================*/

bool sysmenu_mouse(uint8_t type, int16_t x, int16_t y) {
    if (!sys_open) return false;

    if (x >= sys_x && x < sys_x + sys_w &&
        y >= sys_y && y < sys_y + sys_h) {
        if (type == WM_MOUSEMOVE || type == WM_LBUTTONDOWN) {
            int iy = sys_y + 2;
            sys_hover = -1;
            for (int i = 0; i < sys_item_count; i++) {
                if (sys_items[i].separator_above) iy += SYS_SEPARATOR_H;
                if (y >= iy && y < iy + SYS_ITEM_HEIGHT) {
                    sys_hover = i;
                    break;
                }
                iy += SYS_ITEM_HEIGHT;
            }
            wm_mark_dirty();
        }
        if (type == WM_LBUTTONUP && sys_hover >= 0) {
            execute(sys_items[sys_hover].id);
        }
        return true;
    }

    if (type == WM_LBUTTONDOWN) {
        sysmenu_close();
        return false;
    }
    return false;
}

/*==========================================================================
 * Keyboard
 *=========================================================================*/

bool sysmenu_handle_key(uint8_t hid_code, uint8_t modifiers) {
    if (!sys_open) return false;
    (void)modifiers;

    switch (hid_code) {
    case 0x52: /* UP */
        sys_hover--;
        if (sys_hover < 0) sys_hover = sys_item_count - 1;
        wm_mark_dirty();
        return true;
    case 0x51: /* DOWN */
        sys_hover++;
        if (sys_hover >= sys_item_count) sys_hover = 0;
        wm_mark_dirty();
        return true;
    case 0x28: /* ENTER */
        if (sys_hover >= 0) execute(sys_items[sys_hover].id);
        return true;
    case 0x29: /* ESC */
        sysmenu_close();
        return true;
    }
    return false;
}
