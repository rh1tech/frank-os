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
#include "swap.h"
#include "alttab.h"
#include "FreeRTOS.h"
#include "task.h"
#include <string.h>


/*==========================================================================
 * Internal state
 *=========================================================================*/

static window_t  windows[WM_MAX_WINDOWS];     /* window table (slots 0-15) */
static hwnd_t    z_stack[WM_MAX_WINDOWS];      /* z-order: bottom to top */
static uint8_t   z_count;                       /* number of entries in z_stack */
static hwnd_t    focus_hwnd;                    /* currently focused window */

/* Full-repaint flag: when true, the compositor clears the entire framebuffer
 * before painting.  Set by structural changes (window create/destroy/move/etc).
 * Content-only invalidations (app redraws) leave it false so the screen is
 * repainted in-place without the full clear — eliminating the "flash" that
 * the beam would otherwise catch on the single visible buffer. */
static bool needs_full_repaint = true;

/* Cascade counter for new window positioning */
static uint8_t cascade_counter = 0;
#define CASCADE_STEP_X  20
#define CASCADE_STEP_Y  20
#define CASCADE_MAX      8  /* wrap after 8 steps */

/* Expose rect queue — desktop areas revealed by structural changes */
#define EXPOSE_MAX 8
static rect_t  expose_rects[EXPOSE_MAX];
static uint8_t expose_count = 0;


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

/* Queue a desktop area for clearing (structural change revealed it).
 * Clips to screen bounds to prevent framebuffer wrap-around when
 * windows are moved partially off screen. */
static void wm_add_expose_rect(const rect_t *r) {
    int16_t x0 = r->x < 0 ? 0 : r->x;
    int16_t y0 = r->y < 0 ? 0 : r->y;
    int16_t x1 = r->x + r->w;
    int16_t y1 = r->y + r->h;
    if (x1 > DISPLAY_WIDTH)  x1 = DISPLAY_WIDTH;
    if (y1 > DISPLAY_HEIGHT) y1 = DISPLAY_HEIGHT;
    int16_t cw = x1 - x0;
    int16_t ch = y1 - y0;
    if (cw <= 0 || ch <= 0) return;

    rect_t clipped = { x0, y0, cw, ch };
    if (expose_count < EXPOSE_MAX)
        expose_rects[expose_count++] = clipped;
    else
        needs_full_repaint = true;  /* overflow fallback */
}

/* Rectangle overlap test */
static inline bool rect_overlaps(const rect_t *a, const rect_t *b) {
    return a->x < b->x + b->w && a->x + a->w > b->x &&
           a->y < b->y + b->h && a->y + a->h > b->y;
}

/* Bounding box union of two rects */
static inline rect_t rect_union(const rect_t *a, const rect_t *b) {
    int16_t x0 = a->x < b->x ? a->x : b->x;
    int16_t y0 = a->y < b->y ? a->y : b->y;
    int16_t x1 = (a->x + a->w) > (b->x + b->w) ? (a->x + a->w) : (b->x + b->w);
    int16_t y1 = (a->y + a->h) > (b->y + b->h) ? (a->y + a->h) : (b->y + b->h);
    return (rect_t){ x0, y0, x1 - x0, y1 - y0 };
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
            win->flags = WF_ALIVE | WF_VISIBLE | WF_DIRTY | WF_FRAME_DIRTY | (style & 0x178);
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

            /* Cascade bordered windows so they don't stack on top of each other */
            if (style & WF_BORDER) {
                int16_t offset = (cascade_counter % CASCADE_MAX) * CASCADE_STEP_X;
                win->frame.x += offset;
                win->frame.y += offset;
                /* Clamp to keep on screen */
                int16_t max_x = DISPLAY_WIDTH - win->frame.w;
                int16_t max_y = taskbar_work_area_height() - win->frame.h;
                if (win->frame.x > max_x) win->frame.x = max_x > 0 ? max_x : 0;
                if (win->frame.y > max_y) win->frame.y = max_y > 0 ? max_y : 0;
                win->restore_rect = win->frame;
                cascade_counter++;
            }

            hwnd_t hwnd = (hwnd_t)(i + 1);

            /* Add to top of z-stack */
            win->z_order = z_count;
            z_stack[z_count++] = hwnd;

            /* Auto-register app windows for swap management.
             * App tasks run at APP_TASK_PRIORITY (1) and have WF_BORDER.
             * Only register the FIRST window a task creates — child
             * windows (dialogs, file dialogs) share the same task and
             * must NOT be registered separately or they would suspend
             * their own parent. */
            {
                TaskHandle_t caller = xTaskGetCurrentTaskHandle();
                if ((style & WF_BORDER) &&
                    uxTaskPriorityGet(caller) == 1 &&
                    !swap_find_by_task(caller)) {
                    swap_register(hwnd, caller);
                    /* This new app becomes the foreground */
                    swap_switch_to(hwnd);
                }
            }

            /* New window is WF_DIRTY — it will paint itself */
            taskbar_invalidate();
            return hwnd;
        }
    }
    return HWND_NULL;
}

void wm_destroy_window(hwnd_t hwnd) {
    if (!valid_hwnd(hwnd)) return;
    window_t *win = &windows[hwnd - 1];

    /* Expose the area this window occupied */
    wm_add_expose_rect(&win->frame);

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
            windows[next - 1].flags |= WF_FOCUSED | WF_DIRTY | WF_FRAME_DIRTY;
        }
    }

    memset(win, 0, sizeof(*win));
    taskbar_invalidate();
}

void wm_show_window(hwnd_t hwnd) {
    if (!valid_hwnd(hwnd)) return;
    windows[hwnd - 1].flags |= WF_VISIBLE | WF_DIRTY | WF_FRAME_DIRTY;
}

void wm_hide_window(hwnd_t hwnd) {
    if (!valid_hwnd(hwnd)) return;
    rect_t frame = windows[hwnd - 1].frame;
    windows[hwnd - 1].flags &= ~WF_VISIBLE;
    wm_add_expose_rect(&frame);
}

void wm_minimize_window(hwnd_t hwnd) {
    if (!valid_hwnd(hwnd)) return;
    rect_t frame = windows[hwnd - 1].frame;
    windows[hwnd - 1].state = WS_MINIMIZED;
    windows[hwnd - 1].flags &= ~WF_VISIBLE;
    wm_add_expose_rect(&frame);
    taskbar_invalidate();
}

void wm_maximize_window(hwnd_t hwnd) {
    if (!valid_hwnd(hwnd)) return;
    window_t *win = &windows[hwnd - 1];
    rect_t old_frame = win->frame;
    if (win->state == WS_NORMAL) {
        win->restore_rect = win->frame;
    }
    win->state = WS_MAXIMIZED;
    win->frame = (rect_t){ 0, 0, DISPLAY_WIDTH, taskbar_work_area_height() };
    win->flags |= WF_DIRTY | WF_FRAME_DIRTY;
    wm_add_expose_rect(&old_frame);
}

void wm_restore_window(hwnd_t hwnd) {
    if (!valid_hwnd(hwnd)) return;
    window_t *win = &windows[hwnd - 1];
    rect_t old_frame = win->frame;
    if (win->state == WS_MAXIMIZED) {
        win->frame = win->restore_rect;
    }
    win->state = WS_NORMAL;
    win->flags |= WF_VISIBLE | WF_DIRTY | WF_FRAME_DIRTY;
    wm_add_expose_rect(&old_frame);
    taskbar_invalidate();
}

void wm_move_window(hwnd_t hwnd, int16_t x, int16_t y) {
    if (!valid_hwnd(hwnd)) return;
    window_t *win = &windows[hwnd - 1];

    /* Clamp position: x >= 0 to prevent wd_fb_ptr wrap-around (apps
     * using direct buffer writes don't handle negative origins).
     * Right edge allows dragging partially off-screen but keeps 60px
     * visible.  Title bar stays reachable vertically. */
    if (x < 0) x = 0;
    if (x > DISPLAY_WIDTH - 60)   x = DISPLAY_WIDTH - 60;
    if (y < 0) y = 0;
    int16_t max_y = taskbar_work_area_height() - THEME_TITLE_HEIGHT;
    if (y > max_y) y = max_y;

    rect_t old_frame = win->frame;
    win->frame.x = x;
    win->frame.y = y;
    win->flags |= WF_DIRTY | WF_FRAME_DIRTY;
    wm_add_expose_rect(&old_frame);
}

void wm_resize_window(hwnd_t hwnd, int16_t w, int16_t h) {
    if (!valid_hwnd(hwnd)) return;
    rect_t old_frame = windows[hwnd - 1].frame;
    windows[hwnd - 1].frame.w = w;
    windows[hwnd - 1].frame.h = h;
    windows[hwnd - 1].flags |= WF_DIRTY | WF_FRAME_DIRTY;
    wm_add_expose_rect(&old_frame);
}

void wm_set_window_rect(hwnd_t hwnd, int16_t x, int16_t y,
                         int16_t w, int16_t h) {
    if (!valid_hwnd(hwnd)) return;

    /* Clamp position: x >= 0 to prevent wd_fb_ptr wrap-around.
     * Keep at least 60px visible on right edge, title bar reachable. */
    if (x < 0) x = 0;
    if (x > DISPLAY_WIDTH - 60)   x = DISPLAY_WIDTH - 60;
    if (y < 0) y = 0;
    int16_t max_y = taskbar_work_area_height() - THEME_TITLE_HEIGHT;
    if (y > max_y) y = max_y;

    rect_t old_frame = windows[hwnd - 1].frame;
    windows[hwnd - 1].frame = (rect_t){ x, y, w, h };
    windows[hwnd - 1].flags |= WF_DIRTY | WF_FRAME_DIRTY;
    wm_add_expose_rect(&old_frame);
}

void wm_set_focus(hwnd_t hwnd) {
    if (focus_hwnd == hwnd) return;

    /* Modal blocking: refuse focus change away from modal dialog */
    hwnd_t modal = wm_get_modal();
    if (modal != HWND_NULL && hwnd != modal) return;

    /* Remove focus from old window — title bar changes color */
    if (valid_hwnd(focus_hwnd)) {
        windows[focus_hwnd - 1].flags &= ~WF_FOCUSED;
        windows[focus_hwnd - 1].flags |= WF_DIRTY | WF_FRAME_DIRTY;
    }

    focus_hwnd = hwnd;

    /* Set focus on new window and raise to top of z-stack */
    if (valid_hwnd(hwnd)) {
        windows[hwnd - 1].flags |= WF_FOCUSED | WF_DIRTY | WF_FRAME_DIRTY;

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
    if (z_count < 2) return;

    /* Pick the bottom-most visible, non-minimized window and bring it
     * to the top.  Since wm_set_focus() raises the target to the top
     * of z_stack, repeated Alt-Tab naturally rotates through ALL
     * windows:  [C,B,A] → focus C → [B,A,C] → focus B → [A,C,B] …
     *
     * Suspended windows are included — Alt+Tab resumes them. */
    for (int i = 0; i < z_count; i++) {
        hwnd_t h = z_stack[i];
        if (h != focus_hwnd && valid_hwnd(h) &&
            (windows[h - 1].flags & (WF_VISIBLE | WF_SUSPENDED)) &&
            windows[h - 1].state != WS_MINIMIZED) {
            swap_switch_to(h);
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

    /* Reject suspended windows — timer callbacks may try to invalidate
     * them, but their task is frozen and paint handler must not run. */
    if (windows[hwnd - 1].flags & WF_SUSPENDED) return;

    /* Only the focused window gets real-time updates, UNLESS it is a
     * background app (e.g. FrankAmp) which keeps running and needs its
     * window repainted even when not focused. */
    if (hwnd != focus_hwnd && !swap_is_background(hwnd)) return;

    windows[hwnd - 1].flags |= WF_DIRTY;
    wm_mark_dirty();
}

void wm_force_full_repaint(void) {
    needs_full_repaint = true;
    wm_mark_dirty();
}

void wm_set_title(hwnd_t hwnd, const char *title) {
    if (!valid_hwnd(hwnd) || !title) return;
    strncpy(windows[hwnd - 1].title, title,
            sizeof(windows[hwnd - 1].title) - 1);
    windows[hwnd - 1].title[sizeof(windows[hwnd - 1].title) - 1] = '\0';
    windows[hwnd - 1].flags |= WF_DIRTY | WF_FRAME_DIRTY;
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

/* Check if a point lies inside a region that WILL be repainted this frame.
 * For windows with WF_FRAME_DIRTY the full frame is repainted.
 * For content-only updates (WF_DIRTY alone) only the client area is
 * repainted — the title bar / border area is NOT touched, so the
 * cursor save-under must be preserved there. */
static bool point_in_dirty_window(int16_t px, int16_t py) {
    if (taskbar_needs_redraw() && py >= taskbar_work_area_height())
        return true;
    for (uint8_t i = 0; i < z_count; i++) {
        window_t *w = &windows[z_stack[i] - 1];
        if (!(w->flags & WF_VISIBLE) || !(w->flags & WF_DIRTY)) continue;

        if (w->flags & WF_FRAME_DIRTY) {
            /* Full frame repaint — check entire frame */
            if (px >= w->frame.x && px < w->frame.x + w->frame.w &&
                py >= w->frame.y && py < w->frame.y + w->frame.h)
                return true;
        } else if (w->flags & WF_BORDER) {
            /* Content-only — only client area will be repainted */
            point_t co = theme_client_origin(&w->frame, w->flags);
            rect_t  cr = theme_client_rect(&w->frame, w->flags);
            if (px >= co.x && px < co.x + cr.w &&
                py >= co.y && py < co.y + cr.h)
                return true;
        } else {
            /* No border — frame IS client area */
            if (px >= w->frame.x && px < w->frame.x + w->frame.w &&
                py >= w->frame.y && py < w->frame.y + w->frame.h)
                return true;
        }
    }
    return false;
}

void wm_composite(void) {
    cursor_overlay_lock();

    /* Erase drag outline (fast XOR) before vsync wait */
    drag_overlay_erase();

    /* Detect popup-overlay close: if any overlay was open last frame
     * but is now closed, stale overlay pixels sit on bare desktop and
     * need a full clear to remove them. */
    bool has_popup = startmenu_is_open() || sysmenu_is_open() ||
                     menu_is_open() || menu_popup_is_open() ||
                     taskbar_popup_is_open();
    {
        static bool prev_had_popup = false;
        if (prev_had_popup && !has_popup)
            needs_full_repaint = true;
        prev_had_popup = has_popup;
    }

    /* Wait for vblank — cursor stays visible during this wait,
     * which may block up to one frame period. */
    display_wait_vsync();

    enum { CUR_ERASE_STAMP, CUR_RESET_STAMP, CUR_SKIP } cursor_mode;

    if (needs_full_repaint) {
        /*--- Fallback path: full repaint ---*/
        cursor_overlay_erase();
        display_clear(THEME_DESKTOP_COLOR);
        needs_full_repaint = false;
        expose_count = 0;

        /* Mark ALL visible windows and taskbar dirty */
        for (uint8_t i = 0; i < z_count; i++) {
            window_t *w = &windows[z_stack[i] - 1];
            if (w->flags & WF_VISIBLE)
                w->flags |= WF_DIRTY | WF_FRAME_DIRTY;
        }
        taskbar_force_dirty();
        cursor_mode = CUR_ERASE_STAMP;
    } else if (has_popup) {
        /*--- Popup-freeze path: overlays paint on top, so freeze
         * window painting to prevent dirty windows from overwriting
         * popup pixels mid-scanline on the single buffer.  When the
         * popup closes, needs_full_repaint refreshes everything. ---*/
        expose_count = 0;  /* deferred — full repaint on close */
        cursor_overlay_erase();
        cursor_mode = CUR_ERASE_STAMP;
    } else {
        /*--- Selective path ---*/
        uint8_t saved_expose_count = expose_count;
        expose_count = 0;

        /* Phase 1: Mark overlapping windows dirty from expose rects
         * (no framebuffer writes yet — cursor is still stamped) */
        for (uint8_t e = 0; e < saved_expose_count; e++) {
            rect_t *er = &expose_rects[e];

            for (uint8_t i = 0; i < z_count; i++) {
                window_t *w = &windows[z_stack[i] - 1];
                if (!(w->flags & WF_VISIBLE)) continue;
                if (rect_overlaps(er, &w->frame))
                    w->flags |= WF_DIRTY | WF_FRAME_DIRTY;
            }

            /* Mark taskbar dirty if expose overlaps it */
            rect_t tb = { 0, taskbar_work_area_height(),
                          DISPLAY_WIDTH, TASKBAR_HEIGHT };
            if (rect_overlaps(er, &tb))
                taskbar_force_dirty();
        }

        /* Dirty propagation: if a dirty window (lower z) is painted,
         * it overwrites pixels that a higher clean window should cover.
         * Mark the higher window dirty only if it actually overlaps a
         * dirty window below it — NOT just the bounding box of all
         * dirty windows, which can cause false cascades. */
        for (uint8_t i = 1; i < z_count; i++) {
            window_t *w = &windows[z_stack[i] - 1];
            if (!(w->flags & WF_VISIBLE) || (w->flags & WF_DIRTY)) continue;

            for (uint8_t j = 0; j < i; j++) {
                window_t *d = &windows[z_stack[j] - 1];
                if (!(d->flags & WF_VISIBLE) || !(d->flags & WF_DIRTY)) continue;
                if (rect_overlaps(&w->frame, &d->frame)) {
                    w->flags |= WF_DIRTY | WF_FRAME_DIRTY;
                    break;
                }
            }
        }

        /* Cursor mode selection */
        if (saved_expose_count > 0) {
            /* Structural change — always erase cursor before filling
             * expose rects (same ordering as old full-clear path) */
            cursor_overlay_erase();
            cursor_mode = CUR_ERASE_STAMP;
        } else {
            /* Content-only path — test cursor against dirty windows */
            int16_t sx, sy;
            bool stamped = cursor_overlay_get_stamp(&sx, &sy);
            int16_t mx, my;
            wm_get_cursor_pos(&mx, &my);
            bool cursor_moved = !stamped || mx != sx || my != sy;

            bool any_dirty = false;
            for (uint8_t i = 0; i < z_count && !any_dirty; i++) {
                window_t *w = &windows[z_stack[i] - 1];
                if ((w->flags & WF_VISIBLE) && (w->flags & WF_DIRTY))
                    any_dirty = true;
            }

            if (!any_dirty && !taskbar_needs_redraw()) {
                if (!cursor_moved) {
                    cursor_mode = CUR_SKIP;
                } else {
                    cursor_overlay_erase();
                    cursor_mode = CUR_ERASE_STAMP;
                }
            } else {
                int16_t cx = stamped ? sx : mx;
                int16_t cy = stamped ? sy : my;
                int16_t bx0, by0, bx1, by1;
                cursor_get_bounds(cx, cy, &bx0, &by0, &bx1, &by1);

                bool tl = point_in_dirty_window(bx0, by0);
                bool tr = point_in_dirty_window(bx1, by0);
                bool bl = point_in_dirty_window(bx0, by1);
                bool br = point_in_dirty_window(bx1, by1);

                if (tl && tr && bl && br) {
                    /* Cursor fully inside dirty area — erase it so the
                     * window paint can write clean pixels underneath. */
                    cursor_overlay_erase();
                    cursor_mode = CUR_ERASE_STAMP;
                } else if (!tl && !tr && !bl && !br && !cursor_moved) {
                    cursor_mode = CUR_SKIP;
                } else if (!cursor_moved) {
                    /* Cursor partially overlaps dirty area — erase to
                     * prevent ghost pixels from the old stamp. */
                    cursor_overlay_erase();
                    cursor_mode = CUR_ERASE_STAMP;
                } else {
                    cursor_overlay_erase();
                    cursor_mode = CUR_ERASE_STAMP;
                }
            }
        }

        /* Phase 2: Fill expose rects with desktop color
         * (cursor is now safely erased) */
        for (uint8_t e = 0; e < saved_expose_count; e++) {
            rect_t *er = &expose_rects[e];
            gfx_fill_rect(er->x, er->y, er->w, er->h, THEME_DESKTOP_COLOR);
        }
    }

    /* Paint dirty visible windows back-to-front.
     * Skip when popups are open — their overlays sit on top and
     * window paints would briefly overwrite them mid-scanline. */
    if (!has_popup) {
        for (uint8_t i = 0; i < z_count; i++) {
            hwnd_t hwnd = z_stack[i];
            window_t *win = &windows[hwnd - 1];
            if (!(win->flags & WF_VISIBLE)) continue;
            if (!(win->flags & WF_DIRTY)) continue;

            /* Only repaint decorations (border, title bar, client bg)
             * when the frame actually changed.  Content-only updates
             * (wm_invalidate) skip this — avoids the full-frame fill
             * that causes flicker on the single-buffer display. */
            if (win->flags & WF_FRAME_DIRTY)
                draw_window_decorations(hwnd, win);

            if (win->paint_handler) {
                /* Skip paint if client area extends past framebuffer
                 * bounds — apps using wd_fb_ptr() write directly
                 * with stride and may overflow into the next row. */
                point_t co = theme_client_origin(&win->frame, win->flags);
                rect_t  cr = theme_client_rect(&win->frame, win->flags);
                if (co.y + cr.h <= FB_HEIGHT && co.x >= 0 &&
                    co.x + cr.w <= DISPLAY_WIDTH) {
                    wd_begin(hwnd);
                    win->paint_handler(hwnd);
                    wd_end();
                }
            }

            win->flags &= ~(WF_DIRTY | WF_FRAME_DIRTY);
        }

    }

    /* Taskbar sits below all popups — always safe to draw.
     * Drawing it outside the popup guard fixes Start button
     * animation (sunken state when start menu is open). */
    taskbar_draw();

    /* Overlay menus — drawn after all windows and taskbar (always
     * when open — cheap and prevents overwrite by window paint) */
    startmenu_draw();

    /* If the dropdown switched to a different menu, repaint the parent
     * window to erase stale dropdown pixels left from the previous
     * position.  This runs in the overlay phase (after popup-freeze)
     * so the parent's content is restored, then the new dropdown is
     * drawn on top immediately below. */
    if (menu_dropdown_moved()) {
        hwnd_t mhwnd = menu_get_open_hwnd();
        window_t *mwin = mhwnd != HWND_NULL ? wm_get_window(mhwnd) : NULL;
        if (mwin && (mwin->flags & WF_VISIBLE)) {
            draw_window_decorations(mhwnd, mwin);
            if (mwin->paint_handler) {
                point_t co = theme_client_origin(&mwin->frame, mwin->flags);
                rect_t  cr = theme_client_rect(&mwin->frame, mwin->flags);
                if (co.y + cr.h <= FB_HEIGHT && co.x >= 0 &&
                    co.x + cr.w <= DISPLAY_WIDTH) {
                    wd_begin(mhwnd);
                    mwin->paint_handler(mhwnd);
                    wd_end();
                }
            }
        }
    }
    menu_draw_dropdown();
    menu_popup_draw();
    sysmenu_draw();
    taskbar_popup_draw();
    alttab_draw();

    /* Stamp drag outline on visible buffer (before cursor) */
    {
        rect_t outline;
        if (wm_get_drag_outline(&outline))
            drag_overlay_stamp(&outline);
    }

    /* Stamp cursor — skip if already stamped above or untouched */
    if (cursor_mode != CUR_SKIP) {
        int16_t mx, my;
        wm_get_cursor_pos(&mx, &my);
        cursor_overlay_stamp(mx, my);
    }

    cursor_overlay_unlock();
}
