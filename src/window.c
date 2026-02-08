#include "window.h"
#include "window_theme.h"
#include "window_event.h"
#include "window_draw.h"
#include "cursor.h"
#include "display.h"
#include "gfx.h"
#include "font.h"
#include <string.h>

/*==========================================================================
 * Internal state
 *=========================================================================*/

static window_t  windows[WM_MAX_WINDOWS];     /* window table (slots 0-15) */
static hwnd_t    z_stack[WM_MAX_WINDOWS];      /* z-order: bottom to top */
static uint8_t   z_count;                       /* number of entries in z_stack */
static hwnd_t    focus_hwnd;                    /* currently focused window */

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
                         const char *title, uint8_t style,
                         event_handler_t event_cb,
                         paint_handler_t paint_cb) {
    /* Find a free slot */
    for (uint8_t i = 0; i < WM_MAX_WINDOWS; i++) {
        if (!(windows[i].flags & WF_ALIVE)) {
            window_t *win = &windows[i];
            memset(win, 0, sizeof(*win));
            win->flags = WF_ALIVE | WF_VISIBLE | WF_DIRTY | (style & 0x78);
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

            hwnd_t hwnd = (hwnd_t)(i + 1);

            /* Add to top of z-stack */
            win->z_order = z_count;
            z_stack[z_count++] = hwnd;

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

    /* Clear focus if this was focused */
    if (focus_hwnd == hwnd) {
        focus_hwnd = (z_count > 0) ? z_stack[z_count - 1] : HWND_NULL;
    }

    memset(win, 0, sizeof(*win));
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
}

void wm_maximize_window(hwnd_t hwnd) {
    if (!valid_hwnd(hwnd)) return;
    window_t *win = &windows[hwnd - 1];
    if (win->state == WS_NORMAL) {
        win->restore_rect = win->frame;
    }
    win->state = WS_MAXIMIZED;
    win->flags |= WF_DIRTY;
    /* TODO: set frame to full screen minus decorations */
}

void wm_restore_window(hwnd_t hwnd) {
    if (!valid_hwnd(hwnd)) return;
    window_t *win = &windows[hwnd - 1];
    if (win->state == WS_MAXIMIZED) {
        win->frame = win->restore_rect;
    }
    win->state = WS_NORMAL;
    win->flags |= WF_VISIBLE | WF_DIRTY;
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
        return theme_client_rect(&win->frame);
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
 * Decoration drawing
 *=========================================================================*/

static void draw_bevel_rect(int x, int y, int w, int h,
                             uint8_t light, uint8_t dark) {
    /* Top and left edges = light */
    gfx_hline(x, y, w, light);
    gfx_vline(x, y, h, light);
    /* Bottom and right edges = dark */
    gfx_hline(x, y + h - 1, w, dark);
    gfx_vline(x + w - 1, y, h, dark);
}

static void draw_button(int x, int y, int w, int h) {
    gfx_fill_rect(x, y, w, h, THEME_BUTTON_FACE);
    draw_bevel_rect(x, y, w, h, THEME_BEVEL_LIGHT, THEME_BEVEL_DARK);
}

static void draw_close_glyph(const rect_t *btn) {
    /* Small X centered in button */
    int cx = btn->x + btn->w / 2;
    int cy = btn->y + btn->h / 2;
    for (int d = -3; d <= 3; d++) {
        display_set_pixel(cx + d, cy + d, COLOR_BLACK);
        display_set_pixel(cx + d, cy - d, COLOR_BLACK);
    }
}

static void draw_maximize_glyph(const rect_t *btn) {
    /* Small rectangle centered in button */
    int bx = btn->x + 3;
    int by = btn->y + 3;
    int bw = btn->w - 6;
    int bh = btn->h - 6;
    gfx_rect(bx, by, bw, bh, COLOR_BLACK);
    gfx_hline(bx, by + 1, bw, COLOR_BLACK); /* thick top edge */
}

static void draw_minimize_glyph(const rect_t *btn) {
    /* Underscore bar at bottom-center of button */
    int bx = btn->x + 3;
    int by = btn->y + btn->h - 5;
    gfx_hline(bx, by, btn->w - 6, COLOR_BLACK);
}

static void draw_window_decorations(hwnd_t hwnd, window_t *win) {
    rect_t f = win->frame;
    bool focused = (win->flags & WF_FOCUSED) != 0;

    if (!(win->flags & WF_BORDER)) {
        /* No border — just fill with bg color */
        gfx_fill_rect(f.x, f.y, f.w, f.h, win->bg_color);
        return;
    }

    uint8_t border_color = focused ? THEME_ACTIVE_BORDER : THEME_INACTIVE_BORDER;
    uint8_t title_bg = focused ? THEME_ACTIVE_TITLE_BG : THEME_INACTIVE_TITLE_BG;
    uint8_t title_fg = focused ? THEME_ACTIVE_TITLE_FG : THEME_INACTIVE_TITLE_FG;

    /* Outer border */
    gfx_fill_rect(f.x, f.y, f.w, THEME_BORDER_WIDTH, border_color);                           /* top */
    gfx_fill_rect(f.x, f.y + f.h - THEME_BORDER_WIDTH, f.w, THEME_BORDER_WIDTH, border_color); /* bottom */
    gfx_fill_rect(f.x, f.y, THEME_BORDER_WIDTH, f.h, border_color);                            /* left */
    gfx_fill_rect(f.x + f.w - THEME_BORDER_WIDTH, f.y, THEME_BORDER_WIDTH, f.h, border_color); /* right */

    /* Title bar background */
    int tb_x = f.x + THEME_BORDER_WIDTH;
    int tb_y = f.y + THEME_BORDER_WIDTH;
    int tb_w = f.w - 2 * THEME_BORDER_WIDTH;
    gfx_fill_rect(tb_x, tb_y, tb_w, THEME_TITLE_HEIGHT, title_bg);

    /* Title text — vertically centered in title bar, clipped to title area */
    int text_y = tb_y + (THEME_TITLE_HEIGHT - FONT_HEIGHT) / 2;
    int text_x = tb_x + 4;
    /* Clip title text to leave room for buttons */
    int max_title_w = tb_w - 4;
    if (win->flags & WF_CLOSABLE) max_title_w -= THEME_BUTTON_W + THEME_BUTTON_PAD;
    if (win->flags & WF_RESIZABLE) max_title_w -= 2 * (THEME_BUTTON_W + THEME_BUTTON_PAD);
    if (max_title_w > 0) {
        gfx_text_clipped(text_x, text_y, win->title, title_fg, title_bg,
                          tb_x, tb_y, max_title_w, THEME_TITLE_HEIGHT);
    }

    /* Title bar buttons */
    if (win->flags & WF_CLOSABLE) {
        rect_t cb = theme_close_btn_rect(&f);
        draw_button(cb.x, cb.y, cb.w, cb.h);
        draw_close_glyph(&cb);
    }
    if (win->flags & WF_RESIZABLE) {
        rect_t mb = theme_max_btn_rect(&f);
        draw_button(mb.x, mb.y, mb.w, mb.h);
        draw_maximize_glyph(&mb);

        rect_t nb = theme_min_btn_rect(&f);
        draw_button(nb.x, nb.y, nb.w, nb.h);
        draw_minimize_glyph(&nb);
    }

    /* Client area background */
    point_t co = theme_client_origin(&f);
    rect_t cr = theme_client_rect(&f);
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

    /* Draw drag outline if a window is being dragged */
    {
        hwnd_t dh;
        int16_t dx, dy;
        if (wm_get_drag_outline(&dh, &dx, &dy)) {
            window_t *dw = wm_get_window(dh);
            if (dw) {
                gfx_rect(dx, dy, dw->frame.w, dw->frame.h, COLOR_BLACK);
                gfx_rect(dx + 1, dy + 1,
                          dw->frame.w - 2, dw->frame.h - 2, COLOR_WHITE);
            }
        }
    }

    /* Draw mouse cursor as final layer */
    int16_t mx, my;
    wm_get_cursor_pos(&mx, &my);
    cursor_draw(mx, my);

    display_swap_buffers();
}
