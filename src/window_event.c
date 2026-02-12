/*
 * FRANK OS
 * Copyright (c) 2025 Mikhail Matveev <xtreme@rh1.tech>
 * https://rh1.tech
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "window_event.h"
#include "window_theme.h"
#include "cursor.h"
#include "display.h"
#include "menu.h"
#include "taskbar.h"
#include "startmenu.h"
#include "sysmenu.h"
#include <string.h>
#include "hardware/sync.h"

/*==========================================================================
 * Internal state
 *=========================================================================*/

/* Hardware spinlock for the event queue — safe across both cores,
 * takes ~2 cycles, and does NOT raise BASEPRI (unlike FreeRTOS
 * critical sections) so it can never mask or delay the HDMI DMA IRQ. */
static spin_lock_t *eq_spinlock;

/* Circular event queue */
static window_event_t event_queue[WM_EVENT_QUEUE_SIZE];
static hwnd_t         event_target[WM_EVENT_QUEUE_SIZE]; /* target window */
static uint8_t        eq_head;
static uint8_t        eq_tail;
static uint8_t        eq_count;

/* Key state bitmap — 128 bits for scancodes 0x00-0x7F */
static uint8_t key_state[16];

/* Mouse state */
static point_t  mouse_pos;
static uint8_t  modifier_state;

/* Atomic cursor position: upper 16 bits = x, lower 16 bits = y.
 * Packed into one word for atomic load/store on Cortex-M33.
 * Initialized to screen center. */
static volatile uint32_t cursor_pos_packed =
    ((uint32_t)(uint16_t)(DISPLAY_WIDTH / 2) << 16) |
     (uint32_t)(uint16_t)(DISPLAY_HEIGHT / 2);

/* Live mouse button state (written by input_task, read by compositor) */
static volatile uint8_t mouse_buttons_live;

/* Compositor dirty flag — avoids recompositing idle frames.
 * Starts dirty so the first frame is always drawn. */
static volatile uint8_t compositor_dirty = 1;

/* Drag/resize state — managed by wm_handle_mouse_input() */
#define DRAG_NONE    0
#define DRAG_MOVE    1
#define DRAG_RESIZE  2

/* Modal dialog — when set, only this window receives input */
static hwnd_t modal_hwnd = HWND_NULL;

static uint8_t drag_mode  = DRAG_NONE;
static hwnd_t  drag_hwnd  = HWND_NULL;
static int16_t drag_anchor_x;            /* mouse position at drag start */
static int16_t drag_anchor_y;
static rect_t  drag_orig;                /* window rect at drag start */
static rect_t  drag_rect;                /* proposed rect during drag */
static uint8_t drag_edge;                /* HT_BORDER_* for resize */

void wm_event_init(void) {
    int lock_num = spin_lock_claim_unused(true);
    eq_spinlock = spin_lock_init(lock_num);
    eq_head = 0;
    eq_tail = 0;
    eq_count = 0;
}

/*==========================================================================
 * Event dispatch — stub implementations
 *=========================================================================*/

bool wm_post_event(hwnd_t hwnd, const window_event_t *event) {
    if (!event) return false;

    uint32_t save = spin_lock_blocking(eq_spinlock);
    if (eq_count >= WM_EVENT_QUEUE_SIZE) {
        spin_unlock(eq_spinlock, save);
        return false;
    }
    event_queue[eq_head] = *event;
    event_target[eq_head] = hwnd;
    eq_head = (eq_head + 1) % WM_EVENT_QUEUE_SIZE;
    eq_count++;
    spin_unlock(eq_spinlock, save);
    compositor_dirty = 1;
    return true;
}

bool wm_post_event_focused(const window_event_t *event) {
    hwnd_t focus = wm_get_focus();
    if (focus == HWND_NULL) return false;
    return wm_post_event(focus, event);
}

void wm_dispatch_events(void) {
    while (eq_count > 0) {
        uint32_t save = spin_lock_blocking(eq_spinlock);
        if (eq_count == 0) {
            spin_unlock(eq_spinlock, save);
            break;
        }
        hwnd_t hwnd = event_target[eq_tail];
        window_event_t ev_copy = event_queue[eq_tail];
        eq_tail = (eq_tail + 1) % WM_EVENT_QUEUE_SIZE;
        eq_count--;
        spin_unlock(eq_spinlock, save);

        window_event_t *ev = &ev_copy;

        /* Update internal key state tracking */
        if (ev->type == WM_KEYDOWN) {
            uint8_t sc = ev->key.scancode;
            key_state[sc >> 3] |= (1u << (sc & 7));
            modifier_state = ev->key.modifiers;
        } else if (ev->type == WM_KEYUP) {
            uint8_t sc = ev->key.scancode;
            key_state[sc >> 3] &= ~(1u << (sc & 7));
            modifier_state = ev->key.modifiers;
        } else if (ev->type == WM_MOUSEMOVE) {
            mouse_pos.x = ev->mouse.x;
            mouse_pos.y = ev->mouse.y;
        }

        /* Deliver to window's event handler */
        window_t *win = wm_get_window(hwnd);
        if (win && win->event_handler) {
            win->event_handler(hwnd, ev);
        }
    }
}

/*==========================================================================
 * Input state queries
 *=========================================================================*/

point_t wm_get_mouse_pos(void) {
    uint32_t packed = cursor_pos_packed;
    point_t p;
    p.x = (int16_t)(packed >> 16);
    p.y = (int16_t)(packed & 0xFFFF);
    return p;
}

bool wm_is_key_down(uint8_t scancode) {
    if (scancode >= 128) return false;
    return (key_state[scancode >> 3] & (1u << (scancode & 7))) != 0;
}

uint8_t wm_get_modifiers(void) {
    return modifier_state;
}

/*==========================================================================
 * Cursor position (atomic packed word)
 *=========================================================================*/

void wm_set_cursor_pos(int16_t x, int16_t y) {
    cursor_pos_packed = ((uint32_t)(uint16_t)x << 16) | (uint32_t)(uint16_t)y;
    compositor_dirty = 1;
}

void wm_get_cursor_pos(int16_t *x, int16_t *y) {
    uint32_t packed = cursor_pos_packed;
    *x = (int16_t)(packed >> 16);
    *y = (int16_t)(packed & 0xFFFF);
}

void wm_set_mouse_buttons(uint8_t buttons) {
    mouse_buttons_live = buttons;
}

uint8_t wm_get_mouse_buttons(void) {
    return mouse_buttons_live;
}

void wm_mark_dirty(void) {
    compositor_dirty = 1;
}

bool wm_needs_composite(void) {
    if (!compositor_dirty) return false;
    compositor_dirty = 0;
    return true;
}

/*==========================================================================
 * Mouse input handler — move, resize, focus, hit-test
 *=========================================================================*/

static inline void forward_mouse_event(uint8_t type, int16_t x, int16_t y,
                                        uint8_t buttons, hwnd_t target) {
    window_t *win = wm_get_window(target);
    if (!win) return;

    window_event_t ev;
    memset(&ev, 0, sizeof(ev));
    ev.type = type;
    point_t co = theme_client_origin(&win->frame, win->flags);
    ev.mouse.x = x - co.x;
    ev.mouse.y = y - co.y;
    ev.mouse.buttons = buttons;
    ev.mouse.modifiers = modifier_state;
    wm_post_event(target, &ev);
}

static void begin_drag(hwnd_t hwnd, uint8_t mode, uint8_t edge,
                        int16_t mx, int16_t my) {
    window_t *win = wm_get_window(hwnd);
    if (!win) return;
    drag_mode     = mode;
    drag_hwnd     = hwnd;
    drag_anchor_x = mx;
    drag_anchor_y = my;
    drag_orig     = win->frame;
    drag_rect     = win->frame;
    drag_edge     = edge;
}

static void update_resize_rect(int16_t mx, int16_t my) {
    int16_t dx = mx - drag_anchor_x;
    int16_t dy = my - drag_anchor_y;

    drag_rect = drag_orig;

    /* Adjust edges based on which border is being dragged */
    switch (drag_edge) {
        case HT_BORDER_R:
            drag_rect.w += dx;
            break;
        case HT_BORDER_B:
            drag_rect.h += dy;
            break;
        case HT_BORDER_L:
            drag_rect.x += dx;
            drag_rect.w -= dx;
            break;
        case HT_BORDER_T:
            drag_rect.y += dy;
            drag_rect.h -= dy;
            break;
        case HT_BORDER_BR:
            drag_rect.w += dx;
            drag_rect.h += dy;
            break;
        case HT_BORDER_BL:
            drag_rect.x += dx;
            drag_rect.w -= dx;
            drag_rect.h += dy;
            break;
        case HT_BORDER_TR:
            drag_rect.w += dx;
            drag_rect.y += dy;
            drag_rect.h -= dy;
            break;
        case HT_BORDER_TL:
            drag_rect.x += dx;
            drag_rect.w -= dx;
            drag_rect.y += dy;
            drag_rect.h -= dy;
            break;
    }

    /* Enforce minimum size — clamp and fix position for left/top edges */
    if (drag_rect.w < THEME_MIN_W) {
        if (drag_edge == HT_BORDER_L || drag_edge == HT_BORDER_TL ||
            drag_edge == HT_BORDER_BL) {
            drag_rect.x = drag_orig.x + drag_orig.w - THEME_MIN_W;
        }
        drag_rect.w = THEME_MIN_W;
    }
    if (drag_rect.h < THEME_MIN_H) {
        if (drag_edge == HT_BORDER_T || drag_edge == HT_BORDER_TL ||
            drag_edge == HT_BORDER_TR) {
            drag_rect.y = drag_orig.y + drag_orig.h - THEME_MIN_H;
        }
        drag_rect.h = THEME_MIN_H;
    }
}

static cursor_type_t cursor_for_edge(uint8_t zone) {
    switch (zone) {
        case HT_BORDER_T:  case HT_BORDER_B:  return CURSOR_RESIZE_NS;
        case HT_BORDER_L:  case HT_BORDER_R:  return CURSOR_RESIZE_EW;
        case HT_BORDER_TL: case HT_BORDER_BR: return CURSOR_RESIZE_NWSE;
        case HT_BORDER_TR: case HT_BORDER_BL: return CURSOR_RESIZE_NESW;
        default: return CURSOR_ARROW;
    }
}

void wm_handle_mouse_input(uint8_t type, int16_t x, int16_t y, uint8_t buttons) {
    /* ---- Active drag/resize in progress ---- */
    if (drag_mode != DRAG_NONE) {
        if (type == WM_MOUSEMOVE) {
            if (drag_mode == DRAG_MOVE) {
                drag_rect.x = drag_orig.x + (x - drag_anchor_x);
                drag_rect.y = drag_orig.y + (y - drag_anchor_y);
            } else {
                update_resize_rect(x, y);
            }
            compositor_dirty = 1;
            return;
        }
        if (type == WM_LBUTTONUP) {
            /* Apply final rect */
            wm_set_window_rect(drag_hwnd, drag_rect.x, drag_rect.y,
                                drag_rect.w, drag_rect.h);
            drag_mode = DRAG_NONE;
            drag_hwnd = HWND_NULL;
            cursor_set_type(CURSOR_ARROW);
            compositor_dirty = 1;
            return;
        }
        return;
    }

    /* ---- Overlay mouse routing priority ---- */
    /* Start menu first */
    if (startmenu_is_open()) {
        if (startmenu_mouse(type, x, y)) return;
    }

    /* System menu */
    if (sysmenu_is_open()) {
        if (sysmenu_mouse(type, x, y)) return;
    }

    /* Popup context menu */
    if (menu_popup_is_open()) {
        if (menu_popup_mouse(type, x, y)) return;
    }

    /* Dropdown menu */
    if (menu_is_open()) {
        if (menu_dropdown_mouse(type, x, y)) return;
    }

    /* Taskbar */
    if (type == WM_LBUTTONDOWN && taskbar_mouse_click(x, y)) return;

    /* Close menus on desktop click */
    if (type == WM_LBUTTONDOWN) {
        if (startmenu_is_open()) startmenu_close();
        if (sysmenu_is_open()) sysmenu_close();
        if (menu_is_open()) menu_close();
        if (menu_popup_is_open()) menu_popup_close();
    }

    /* ---- Left button down: hit-test to decide action ---- */
    if (type == WM_LBUTTONDOWN) {
        hwnd_t target = wm_window_at_point(x, y);
        if (target == HWND_NULL) return;

        /* Modal blocking: if a modal dialog is open and the click
         * target is not the modal window, ignore the click. */
        if (modal_hwnd != HWND_NULL && target != modal_hwnd) {
            wm_invalidate(modal_hwnd); /* flash the dialog */
            return;
        }

        window_t *win = wm_get_window(target);
        if (!win) return;

        wm_set_focus(target);

        uint8_t zone = theme_hit_test(&win->frame, win->flags, x, y);

        switch (zone) {
            case HT_TITLEBAR:
                if ((win->flags & WF_MOVABLE) && win->state != WS_MAXIMIZED) {
                    begin_drag(target, DRAG_MOVE, 0, x, y);
                }
                return;

            case HT_CLOSE:
                if (win->flags & WF_CLOSABLE) {
                    window_event_t ev;
                    memset(&ev, 0, sizeof(ev));
                    ev.type = WM_CLOSE;
                    wm_post_event(target, &ev);
                }
                return;

            case HT_MAXIMIZE:
                if (win->flags & WF_RESIZABLE) {
                    if (win->state == WS_MAXIMIZED)
                        wm_restore_window(target);
                    else
                        wm_maximize_window(target);
                    compositor_dirty = 1;
                }
                /* Non-resizable: button is disabled, do nothing */
                return;

            case HT_MINIMIZE:
                wm_minimize_window(target);
                compositor_dirty = 1;
                return;

            case HT_MENUBAR:
                if (win->flags & WF_MENUBAR) {
                    int bar_x = win->frame.x + THEME_BORDER_WIDTH;
                    menu_bar_click(target, x - bar_x);
                }
                return;

            case HT_BORDER_L:  case HT_BORDER_R:
            case HT_BORDER_T:  case HT_BORDER_B:
            case HT_BORDER_TL: case HT_BORDER_TR:
            case HT_BORDER_BL: case HT_BORDER_BR:
                if ((win->flags & WF_RESIZABLE) && win->state != WS_MAXIMIZED) {
                    begin_drag(target, DRAG_RESIZE, zone, x, y);
                    cursor_set_type(cursor_for_edge(zone));
                }
                return;

            case HT_CLIENT:
                forward_mouse_event(WM_LBUTTONDOWN, x, y, buttons, target);
                return;

            default:
                return;
        }
    }

    /* ---- Left button up (not dragging) ---- */
    if (type == WM_LBUTTONUP) {
        /* Route to overlays first */
        if (startmenu_is_open() && startmenu_mouse(type, x, y)) return;
        if (sysmenu_is_open() && sysmenu_mouse(type, x, y)) return;
        if (menu_is_open() && menu_dropdown_mouse(type, x, y)) return;

        hwnd_t focus = wm_get_focus();
        if (focus != HWND_NULL)
            forward_mouse_event(WM_LBUTTONUP, x, y, buttons, focus);
        return;
    }

    /* ---- Mouse move (not dragging) ---- */
    if (type == WM_MOUSEMOVE) {
        /* Route to overlays first for hover tracking */
        if (startmenu_is_open()) startmenu_mouse(type, x, y);
        if (sysmenu_is_open()) sysmenu_mouse(type, x, y);
        if (menu_is_open()) menu_dropdown_mouse(type, x, y);

        /* Update cursor shape based on what's under the pointer */
        cursor_type_t cur = CURSOR_ARROW;
        hwnd_t hover = wm_window_at_point(x, y);
        if (hover != HWND_NULL) {
            window_t *hwin = wm_get_window(hover);
            if (hwin && (hwin->flags & WF_RESIZABLE) &&
                hwin->state != WS_MAXIMIZED) {
                uint8_t zone = theme_hit_test(&hwin->frame, hwin->flags, x, y);
                cur = cursor_for_edge(zone);
            }
        }
        cursor_set_type(cur);

        hwnd_t focus = wm_get_focus();
        if (focus != HWND_NULL)
            forward_mouse_event(WM_MOUSEMOVE, x, y, buttons, focus);
        return;
    }

    /* ---- Right button events: forward to focused window ---- */
    if (type == WM_RBUTTONDOWN || type == WM_RBUTTONUP) {
        hwnd_t focus = wm_get_focus();
        if (focus != HWND_NULL)
            forward_mouse_event(type, x, y, buttons, focus);
        return;
    }
}

bool wm_get_drag_outline(rect_t *outline) {
    if (drag_mode == DRAG_NONE) return false;
    if (outline) *outline = drag_rect;
    return true;
}

/*==========================================================================
 * Modal dialog support
 *=========================================================================*/

void wm_set_modal(hwnd_t hwnd) {
    modal_hwnd = hwnd;
}

void wm_clear_modal(void) {
    modal_hwnd = HWND_NULL;
}

hwnd_t wm_get_modal(void) {
    return modal_hwnd;
}
