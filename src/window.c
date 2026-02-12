/*
 * FRANK OS
 * Copyright (c) 2025 Mikhail Matveev <xtreme@rh1.tech>
 * https://rh1.tech
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "window.h"
#include "window_theme.h"
#include "window_event.h"
#include "window_draw.h"
#include "cursor.h"
#include "display.h"
#include "gfx.h"
#include "font.h"
#include "menu.h"
#include "taskbar.h"
#include "startmenu.h"
#include "sysmenu.h"
#include <string.h>

/*==========================================================================
 * Internal state
 *=========================================================================*/

static window_t  windows[WM_MAX_WINDOWS];     /* window table (slots 0-15) */
static hwnd_t    z_stack[WM_MAX_WINDOWS];      /* z-order: bottom to top */
static uint8_t   z_count;                       /* number of entries in z_stack */
static hwnd_t    focus_hwnd;                    /* currently focused window */

/* Per-window icon storage — copied here so icons survive fos_apps[] rescan */
#define ICON16_SIZE 256
static uint8_t   icon_pool[WM_MAX_WINDOWS][ICON16_SIZE];

/* Pending icon — set before wm_create_window(), consumed by it */
static const uint8_t *pending_icon = NULL;

/*==========================================================================
 * Internal helpers
 *=========================================================================*/

static inline bool valid_hwnd(hwnd_t h) {
    return h >= 1 && h <= WM_MAX_WINDOWS &&
           (windows[h - 1].flags & WF_ALIVE);
}

/*==========================================================================
 * Window Manager API — stub implementations
 *=========================================================================*/

void wm_init(void) {
    wm_event_init();
    memset(windows, 0, sizeof(windows));
    memset(z_stack, 0, sizeof(z_stack));
    z_count = 0;
    focus_hwnd = HWND_NULL;
}

hwnd_t wm_create_window(int16_t x, int16_t y, int16_t w, int16_t h,
                         const char *title, uint16_t style,
                         event_handler_t event_cb,
                         paint_handler_t paint_cb) {
    /* Find a free slot */
    for (uint8_t i = 0; i < WM_MAX_WINDOWS; i++) {
        if (!(windows[i].flags & WF_ALIVE)) {
            window_t *win = &windows[i];
            memset(win, 0, sizeof(*win));
            win->flags = WF_ALIVE | WF_VISIBLE | WF_DIRTY | (style & 0x178);
            win->state = WS_NORMAL;
            win->frame = (rect_t){ x, y, w, h };
            win->restore_rect = win->frame;
            win->bg_color = COLOR_WHITE;
            win->event_handler = event_cb;
            win->paint_handler = paint_cb;

            if (title) {
                strncpy(win->title, title, sizeof(win->title) - 1);
                win->title[sizeof(win->title) - 1] = '\0';
            }

            /* Assign pending icon (if any) */
            if (pending_icon) {
                memcpy(icon_pool[i], pending_icon, ICON16_SIZE);
                win->icon = icon_pool[i];
                pending_icon = NULL;
            } else {
                win->icon = NULL;
            }

            hwnd_t hwnd = (hwnd_t)(i + 1);

            /* Add to top of z-stack */
            win->z_order = z_count;
            z_stack[z_count++] = hwnd;

            taskbar_invalidate();
            return hwnd;
        }
    }
    return HWND_NULL;
}

void wm_destroy_window(hwnd_t hwnd) {
    if (!valid_hwnd(hwnd)) return;
    window_t *win = &windows[hwnd - 1];

    /* Remove from z-stack */
    for (uint8_t i = 0; i < z_count; i++) {
        if (z_stack[i] == hwnd) {
            for (uint8_t j = i; j < z_count - 1; j++) {
                z_stack[j] = z_stack[j + 1];
            }
            z_count--;
            break;
        }
    }

    /* Update z_order for remaining windows */
    for (uint8_t i = 0; i < z_count; i++) {
        windows[z_stack[i] - 1].z_order = i;
    }

    /* Transfer focus to next top window if this was focused */
    if (focus_hwnd == hwnd) {
        focus_hwnd = HWND_NULL;
        if (z_count > 0) {
            hwnd_t next = z_stack[z_count - 1];
            focus_hwnd = next;
            windows[next - 1].flags |= WF_FOCUSED | WF_DIRTY;
        }
    }

    memset(win, 0, sizeof(*win));
    taskbar_invalidate();
}

void wm_show_window(hwnd_t hwnd) {
    if (!valid_hwnd(hwnd)) return;
    windows[hwnd - 1].flags |= WF_VISIBLE | WF_DIRTY;
}

void wm_hide_window(hwnd_t hwnd) {
    if (!valid_hwnd(hwnd)) return;
    windows[hwnd - 1].flags &= ~WF_VISIBLE;
}

void wm_minimize_window(hwnd_t hwnd) {
    if (!valid_hwnd(hwnd)) return;
    windows[hwnd - 1].state = WS_MINIMIZED;
    windows[hwnd - 1].flags &= ~WF_VISIBLE;
    taskbar_invalidate();
}

void wm_maximize_window(hwnd_t hwnd) {
    if (!valid_hwnd(hwnd)) return;
    window_t *win = &windows[hwnd - 1];
    if (win->state == WS_NORMAL) {
        win->restore_rect = win->frame;
    }
    win->state = WS_MAXIMIZED;
    win->frame = (rect_t){ 0, 0, DISPLAY_WIDTH, taskbar_work_area_height() };
    win->flags |= WF_DIRTY;
}

void wm_restore_window(hwnd_t hwnd) {
    if (!valid_hwnd(hwnd)) return;
    window_t *win = &windows[hwnd - 1];
    if (win->state == WS_MAXIMIZED) {
        win->frame = win->restore_rect;
    }
    win->state = WS_NORMAL;
    win->flags |= WF_VISIBLE | WF_DIRTY;
    taskbar_invalidate();
}

void wm_move_window(hwnd_t hwnd, int16_t x, int16_t y) {
    if (!valid_hwnd(hwnd)) return;
    windows[hwnd - 1].frame.x = x;
    windows[hwnd - 1].frame.y = y;
    windows[hwnd - 1].flags |= WF_DIRTY;
}

void wm_resize_window(hwnd_t hwnd, int16_t w, int16_t h) {
    if (!valid_hwnd(hwnd)) return;
    windows[hwnd - 1].frame.w = w;
    windows[hwnd - 1].frame.h = h;
    windows[hwnd - 1].flags |= WF_DIRTY;
}

void wm_set_window_rect(hwnd_t hwnd, int16_t x, int16_t y,
                         int16_t w, int16_t h) {
    if (!valid_hwnd(hwnd)) return;
    windows[hwnd - 1].frame = (rect_t){ x, y, w, h };
    windows[hwnd - 1].flags |= WF_DIRTY;
}

void wm_set_focus(hwnd_t hwnd) {
    if (focus_hwnd == hwnd) return;

    /* Modal blocking: refuse focus change away from modal dialog */
    hwnd_t modal = wm_get_modal();
    if (modal != HWND_NULL && hwnd != modal) return;

    /* Remove focus from old window */
    if (valid_hwnd(focus_hwnd)) {
        windows[focus_hwnd - 1].flags &= ~WF_FOCUSED;
        windows[focus_hwnd - 1].flags |= WF_DIRTY;
    }

    focus_hwnd = hwnd;

    /* Set focus on new window and raise to top of z-stack */
    if (valid_hwnd(hwnd)) {
        windows[hwnd - 1].flags |= WF_FOCUSED | WF_DIRTY;

        /* Move to top of z-stack */
        for (uint8_t i = 0; i < z_count; i++) {
            if (z_stack[i] == hwnd) {
                for (uint8_t j = i; j < z_count - 1; j++) {
                    z_stack[j] = z_stack[j + 1];
                }
                z_stack[z_count - 1] = hwnd;
                break;
            }
        }

        /* Update z_order for all windows */
        for (uint8_t i = 0; i < z_count; i++) {
            windows[z_stack[i] - 1].z_order = i;
        }
    }

    taskbar_invalidate();
}

hwnd_t wm_get_focus(void) {
    return focus_hwnd;
}

void wm_cycle_focus(void) {
    if (z_count == 0) return;

    /* Find next visible window below current top */
    for (uint8_t i = z_count; i > 0; i--) {
        hwnd_t h = z_stack[i - 1];
        if (h != focus_hwnd && valid_hwnd(h) &&
            (windows[h - 1].flags & WF_VISIBLE)) {
            wm_set_focus(h);
            return;
        }
    }
}

rect_t wm_get_client_rect(hwnd_t hwnd) {
    rect_t r = { 0, 0, 0, 0 };
    if (!valid_hwnd(hwnd)) return r;

    window_t *win = &windows[hwnd - 1];
    if (win->flags & WF_BORDER) {
        return theme_client_rect(&win->frame, win->flags);
    }

    /* No border: client area = full frame, but in client coordinates */
    r.x = 0;
    r.y = 0;
    r.w = win->frame.w;
    r.h = win->frame.h;
    return r;
}

window_t *wm_get_window(hwnd_t hwnd) {
    if (!valid_hwnd(hwnd)) return (window_t *)0;
    return &windows[hwnd - 1];
}

void wm_invalidate(hwnd_t hwnd) {
    if (!valid_hwnd(hwnd)) return;
    windows[hwnd - 1].flags |= WF_DIRTY;
    wm_mark_dirty();
}

void wm_set_title(hwnd_t hwnd, const char *title) {
    if (!valid_hwnd(hwnd) || !title) return;
    strncpy(windows[hwnd - 1].title, title,
            sizeof(windows[hwnd - 1].title) - 1);
    windows[hwnd - 1].title[sizeof(windows[hwnd - 1].title) - 1] = '\0';
    windows[hwnd - 1].flags |= WF_DIRTY;
}

void wm_set_pending_icon(const uint8_t *icon_data) {
    pending_icon = icon_data;
}

/*==========================================================================
 * Hit-test: find topmost window at a screen point
 *=========================================================================*/

hwnd_t wm_window_at_point(int16_t x, int16_t y) {
    /* Walk z-stack top to bottom */
    for (int i = (int)z_count - 1; i >= 0; i--) {
        hwnd_t hwnd = z_stack[i];
        window_t *win = &windows[hwnd - 1];
        if (!(win->flags & WF_VISIBLE)) continue;

        rect_t *f = &win->frame;
        if (x >= f->x && x < f->x + f->w &&
            y >= f->y && y < f->y + f->h) {
            return hwnd;
        }
    }
    return HWND_NULL;
}

/*==========================================================================
 * Decoration drawing — Win95 style
 *=========================================================================*/

/* Win95 raised bevel: 2px border with 3D highlight/shadow.
 * Outer edge: light_gray (blends with button face), black.
 * Inner edge: white (bright highlight), dark_gray (shadow). */
static void draw_bevel_raised(int x, int y, int w, int h) {
    /* Outer edge: top/left = light_gray, bottom/right = black */
    gfx_hline(x, y, w, COLOR_LIGHT_GRAY);
    gfx_vline(x, y, h, COLOR_LIGHT_GRAY);
    gfx_hline(x, y + h - 1, w, COLOR_BLACK);
    gfx_vline(x + w - 1, y, h, COLOR_BLACK);
    /* Inner edge: top/left = white, bottom/right = dark_gray */
    gfx_hline(x + 1, y + 1, w - 2, COLOR_WHITE);
    gfx_vline(x + 1, y + 1, h - 2, COLOR_WHITE);
    gfx_hline(x + 1, y + h - 2, w - 2, COLOR_DARK_GRAY);
    gfx_vline(x + w - 2, y + 1, h - 2, COLOR_DARK_GRAY);
}

/* Win95 sunken bevel (2px: outer dark_gray/white, inner black/light_gray) */
static void draw_bevel_sunken(int x, int y, int w, int h) {
    /* Outer edge */
    gfx_hline(x, y, w, COLOR_DARK_GRAY);
    gfx_vline(x, y, h, COLOR_DARK_GRAY);
    gfx_hline(x, y + h - 1, w, COLOR_WHITE);
    gfx_vline(x + w - 1, y, h, COLOR_WHITE);
    /* Inner edge */
    gfx_hline(x + 1, y + 1, w - 2, COLOR_BLACK);
    gfx_vline(x + 1, y + 1, h - 2, COLOR_BLACK);
    gfx_hline(x + 1, y + h - 2, w - 2, COLOR_LIGHT_GRAY);
    gfx_vline(x + w - 2, y + 1, h - 2, COLOR_LIGHT_GRAY);
}

static void draw_button(int x, int y, int w, int h, bool pressed) {
    gfx_fill_rect(x, y, w, h, THEME_BUTTON_FACE);
    if (pressed) {
        /* 1px sunken for small buttons */
        gfx_hline(x, y, w, COLOR_DARK_GRAY);
        gfx_vline(x, y, h, COLOR_DARK_GRAY);
        gfx_hline(x, y + h - 1, w, COLOR_WHITE);
        gfx_vline(x + w - 1, y, h, COLOR_WHITE);
    } else {
        draw_bevel_raised(x, y, w, h);
    }
}

static void draw_close_glyph(const rect_t *btn, bool pressed) {
    /* 2px-thick X centered in button */
    int ox = pressed ? 1 : 0;
    int oy = pressed ? 1 : 0;
    int cx = btn->x + btn->w / 2 - 1 + ox;
    int cy = btn->y + (btn->h - 1) / 2 + oy;
    for (int d = -3; d <= 3; d++) {
        display_set_pixel(cx + d, cy + d, COLOR_BLACK);
        display_set_pixel(cx + d, cy - d, COLOR_BLACK);
        /* Second pixel for thickness */
        display_set_pixel(cx + d + 1, cy + d, COLOR_BLACK);
        display_set_pixel(cx + d + 1, cy - d, COLOR_BLACK);
    }
}

static void draw_maximize_glyph(const rect_t *btn, bool pressed, uint8_t color) {
    int ox = pressed ? 1 : 0;
    int oy = pressed ? 1 : 0;
    int bx = btn->x + 3 + ox;
    int by = btn->y + 3 + oy;
    int bw = btn->w - 6;
    int bh = btn->h - 6;
    gfx_rect(bx, by, bw, bh, color);
    gfx_hline(bx, by + 1, bw, color); /* thick top edge */
}

static void draw_restore_glyph(const rect_t *btn, bool pressed) {
    int ox = pressed ? 1 : 0;
    int oy = pressed ? 1 : 0;
    int bx = btn->x + 3 + ox;
    int by = btn->y + 2 + oy;
    int bw = btn->w - 8;
    int bh = btn->h - 7;
    /* Back (upper-right) rectangle */
    gfx_rect(bx + 2, by, bw, bh, COLOR_BLACK);
    gfx_hline(bx + 2, by + 1, bw, COLOR_BLACK);
    /* Front (lower-left) rectangle */
    gfx_fill_rect(bx, by + 2, bw, bh, THEME_BUTTON_FACE);
    gfx_rect(bx, by + 2, bw, bh, COLOR_BLACK);
    gfx_hline(bx, by + 3, bw, COLOR_BLACK);
}

static void draw_minimize_glyph(const rect_t *btn, bool pressed) {
    int ox = pressed ? 1 : 0;
    int oy = pressed ? 1 : 0;
    int bx = btn->x + 3 + ox;
    int by = btn->y + btn->h - 5 + oy;
    gfx_hline(bx, by, btn->w - 6, COLOR_BLACK);
    gfx_hline(bx, by + 1, btn->w - 6, COLOR_BLACK);
}

static void draw_window_decorations(hwnd_t hwnd, window_t *win) {
    rect_t f = win->frame;
    bool focused = (win->flags & WF_FOCUSED) != 0;

    if (!(win->flags & WF_BORDER)) {
        /* No border — just fill with bg color */
        gfx_fill_rect(f.x, f.y, f.w, f.h, win->bg_color);
        return;
    }

    uint8_t title_bg = focused ? THEME_ACTIVE_TITLE_BG : THEME_INACTIVE_TITLE_BG;
    uint8_t title_fg = focused ? THEME_ACTIVE_TITLE_FG : THEME_INACTIVE_TITLE_FG;

    /* Win95 outer frame (outside-in):
     * 1. 1px outer: top/left = light_gray, bottom/right = black
     * 2. 1px highlight: top/left = white, bottom/right = dark_gray
     * 3. Interior fill = light_gray (button face)
     * 4. Sunken edge around client area */

    /* Fill entire frame with button face first */
    gfx_fill_rect(f.x, f.y, f.w, f.h, THEME_BUTTON_FACE);

    /* Outer raised bevel (2px total) */
    draw_bevel_raised(f.x, f.y, f.w, f.h);

    /* Hide inner left white highlight line — it's too close to the
     * sunken client edge and looks wrong in 4-bit color. */
    gfx_vline(f.x + 1, f.y + 1, f.h - 2, THEME_BUTTON_FACE);

    /* Sunken edge around client area — in real Win95 this sits BELOW the
     * menu bar; the menu bar is part of the non-client chrome above it. */
    {
        int sx = f.x + THEME_BORDER_WIDTH - 2;
        int sy = f.y + THEME_BORDER_WIDTH + THEME_TITLE_HEIGHT;
        int sw = f.w - 2 * (THEME_BORDER_WIDTH - 2);
        int sh = f.h - THEME_TITLE_HEIGHT - THEME_BORDER_WIDTH - (THEME_BORDER_WIDTH - 2);
        if (win->flags & WF_MENUBAR) {
            sy += THEME_MENU_HEIGHT;
            sh -= THEME_MENU_HEIGHT;
        }
        draw_bevel_sunken(sx, sy, sw, sh);
    }

    /* Title bar background */
    int tb_x = f.x + THEME_BORDER_WIDTH;
    int tb_y = f.y + THEME_BORDER_WIDTH;
    int tb_w = f.w - 2 * THEME_BORDER_WIDTH;
    gfx_fill_rect(tb_x, tb_y, tb_w, THEME_TITLE_HEIGHT, title_bg);

    /* Title bar icon — draw 16x16 icon if available, use default otherwise */
    extern const uint8_t default_icon_16x16[256];
    const uint8_t *icon = win->icon ? win->icon : default_icon_16x16;
    gfx_draw_icon_16(tb_x + 2, tb_y + 2, icon);

    /* Title text — bold UI font, vertically centered in title bar */
    int text_y = tb_y + (THEME_TITLE_HEIGHT - FONT_UI_HEIGHT) / 2;
    int text_x = tb_x + 20;
    int max_title_w = tb_w - 20;
    if (win->flags & WF_CLOSABLE) {
        /* Close + maximize + minimize buttons */
        max_title_w -= 3 * (THEME_BUTTON_W + THEME_BUTTON_PAD);
    }
    if (max_title_w > 0) {
        gfx_text_ui_bold_clipped(text_x, text_y, win->title, title_fg, title_bg,
                                  tb_x, tb_y, max_title_w, THEME_TITLE_HEIGHT);
    }

    /* Title bar buttons — all closable windows get close + maximize + minimize */
    if (win->flags & WF_CLOSABLE) {
        uint8_t pressed_btn = wm_get_pressed_titlebar_btn(hwnd);

        rect_t cb = theme_close_btn_rect(&f);
        bool close_pressed = (pressed_btn == HT_CLOSE);
        draw_button(cb.x, cb.y, cb.w, cb.h, close_pressed);
        draw_close_glyph(&cb, close_pressed);

        rect_t mb = theme_max_btn_rect(&f);
        bool max_pressed = (pressed_btn == HT_MAXIMIZE);
        draw_button(mb.x, mb.y, mb.w, mb.h, max_pressed);
        if (win->state == WS_MAXIMIZED) {
            draw_restore_glyph(&mb, max_pressed);
        } else {
            uint8_t glyph_color = (win->flags & WF_RESIZABLE) ?
                                   COLOR_BLACK : COLOR_DARK_GRAY;
            draw_maximize_glyph(&mb, max_pressed, glyph_color);
        }

        rect_t nb = theme_min_btn_rect(&f);
        bool min_pressed = (pressed_btn == HT_MINIMIZE);
        draw_button(nb.x, nb.y, nb.w, nb.h, min_pressed);
        draw_minimize_glyph(&nb, min_pressed);
    }

    /* Menu bar (drawn by menu system if WF_MENUBAR is set) */
    if (win->flags & WF_MENUBAR) {
        int mb_x = f.x + THEME_BORDER_WIDTH;
        int mb_y = f.y + THEME_BORDER_WIDTH + THEME_TITLE_HEIGHT;
        int mb_w = f.w - 2 * THEME_BORDER_WIDTH;
        menu_draw_bar(hwnd, mb_x, mb_y, mb_w);

        /* Raised bottom edge of menu bar — Win95 draws a highlight line
         * here that separates the menu bar from the client area below. */
        int sep_x = f.x + THEME_BORDER_WIDTH - 2;
        int sep_w = f.w - 2 * (THEME_BORDER_WIDTH - 2);
        gfx_hline(sep_x, mb_y + THEME_MENU_HEIGHT - 1, sep_w, COLOR_WHITE);
    }

    /* Client area background */
    point_t co = theme_client_origin(&f, win->flags);
    rect_t cr = theme_client_rect(&f, win->flags);
    gfx_fill_rect(co.x, co.y, cr.w, cr.h, win->bg_color);
}

/*==========================================================================
 * Compositor
 *=========================================================================*/

void wm_composite(void) {
    /* Clear to desktop color */
    display_clear(THEME_DESKTOP_COLOR);

    /* Paint visible windows back-to-front */
    for (uint8_t i = 0; i < z_count; i++) {
        hwnd_t hwnd = z_stack[i];
        window_t *win = &windows[hwnd - 1];
        if (!(win->flags & WF_VISIBLE)) continue;

        draw_window_decorations(hwnd, win);

        /* Call application paint handler */
        if (win->paint_handler) {
            wd_begin(hwnd);
            win->paint_handler(hwnd);
            wd_end();
        }

        win->flags &= ~WF_DIRTY;
    }

    /* Taskbar (compositor overlay, not a window) */
    taskbar_draw();

    /* Overlay menus — drawn after all windows and taskbar */
    startmenu_draw();
    menu_draw_dropdown();
    menu_popup_draw();
    sysmenu_draw();

    /* Draw drag/resize outline */
    {
        rect_t outline;
        if (wm_get_drag_outline(&outline)) {
            gfx_rect(outline.x, outline.y, outline.w, outline.h, COLOR_BLACK);
            gfx_rect(outline.x + 1, outline.y + 1,
                      outline.w - 2, outline.h - 2, COLOR_WHITE);
        }
    }

    /* Draw mouse cursor as final layer */
    int16_t mx, my;
    wm_get_cursor_pos(&mx, &my);
    cursor_draw(mx, my);

    display_swap_buffers();
}
