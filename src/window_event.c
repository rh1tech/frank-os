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
#include "swap.h"
#include "desktop.h"
#include <string.h>
#include "hardware/sync.h"
#include "FreeRTOS.h"
#include "task.h"

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
static bool    drag_moved;               /* true after first mouse move */

/* Drag outline overlay — XOR drawn on show buffer, zero extra RAM.
 * drag_ol_rect tracks the position where the outline was last stamped
 * so that erase always XORs the exact same pixels, even if drag_rect
 * has been updated by input_task in the meantime. */
static bool   drag_ol_active;
static rect_t drag_ol_rect;

/* Title bar button press tracking — press-on-release behavior */
static hwnd_t  titlebar_btn_hwnd  = HWND_NULL; /* which window's button is captured */
static uint8_t titlebar_btn_zone  = HT_NOWHERE; /* HT_CLOSE / HT_MAXIMIZE / HT_MINIMIZE */
static bool    titlebar_btn_shown = false;       /* visually pressed (false when cursor leaves) */

/* Title bar double-click detection */
#define DBLCLICK_TICKS  pdMS_TO_TICKS(400)
static uint32_t dblclick_tick = 0;
static hwnd_t   dblclick_hwnd = HWND_NULL;

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
    if (event->type != WM_MOUSEMOVE)
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

        /* Deliver to window's event handler.
         * Focus events (WM_KILLFOCUS / WM_SETFOCUS) must be delivered even
         * to suspended windows — the swap subsystem suspends the main task
         * BEFORE the focus event is dispatched, so without this exception
         * the app never learns it lost focus and background tasks (cursor
         * blink, direct-FB writes) keep running. */
        window_t *win = wm_get_window(hwnd);
        bool is_focus_event = (ev->type == WM_KILLFOCUS || ev->type == WM_SETFOCUS);
        if (win && win->event_handler &&
            (is_focus_event || !(win->flags & WF_SUSPENDED))) {
            win->event_handler(hwnd, ev);
        } else if (hwnd == HWND_NULL && ev->type == WM_COMMAND) {
            /* Desktop context menu commands (popup_owner = HWND_NULL) */
            desktop_handle_command(ev->command.id);
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
    /* Suspend the app while dragging — apps that write directly to the
     * framebuffer (e.g. ZX Spectrum) cause artifacts if they keep
     * rendering while the window outline moves. */
    drag_moved = false;
    swap_suspend(hwnd);
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

/*==========================================================================
 * XOR outline drawing — directly on show buffer
 *=========================================================================*/

static void show_xor_hline(int16_t x, int16_t y, int16_t w) {
    if (y < 0 || y >= DISPLAY_HEIGHT || w <= 0) return;
    int16_t x0 = x < 0 ? 0 : x;
    int16_t x1 = x + w - 1;
    if (x1 >= DISPLAY_WIDTH) x1 = DISPLAY_WIDTH - 1;
    if (x0 > x1) return;

    uint8_t *row = &display_show_buffer_ptr[y * FB_STRIDE];
    int16_t px = x0;

    if (px & 1) { row[px >> 1] ^= 0x0F; px++; }
    while (px + 1 <= x1) { row[px >> 1] ^= 0xFF; px += 2; }
    if (px <= x1) { row[px >> 1] ^= 0xF0; }
}

static void show_xor_vline(int16_t x, int16_t y, int16_t h) {
    if (x < 0 || x >= DISPLAY_WIDTH || h <= 0) return;
    int16_t y0 = y < 0 ? 0 : y;
    int16_t y1 = y + h - 1;
    if (y1 >= DISPLAY_HEIGHT) y1 = DISPLAY_HEIGHT - 1;
    if (y0 > y1) return;

    int16_t byte_off = x >> 1;
    uint8_t mask = (x & 1) ? 0x0F : 0xF0;
    uint8_t *p = &display_show_buffer_ptr[y0 * FB_STRIDE + byte_off];

    for (int16_t r = y0; r <= y1; r++) {
        *p ^= mask;
        p += FB_STRIDE;
    }
}

static void show_xor_rect(int16_t x, int16_t y, int16_t w, int16_t h) {
    if (w <= 0 || h <= 0) return;
    if (h == 1) { show_xor_hline(x, y, w); return; }
    if (w == 1) { show_xor_vline(x, y, h); return; }
    show_xor_hline(x, y, w);
    show_xor_hline(x, y + h - 1, w);
    show_xor_vline(x, y + 1, h - 2);
    show_xor_vline(x + w - 1, y + 1, h - 2);
}

static void show_xor_outline(const rect_t *r) {
    show_xor_rect(r->x, r->y, r->w, r->h);
    if (r->w > 2 && r->h > 2)
        show_xor_rect(r->x + 1, r->y + 1, r->w - 2, r->h - 2);
}

/*==========================================================================
 * Drag overlay — stamp/erase XOR outline on show buffer
 *=========================================================================*/

void drag_overlay_stamp(const rect_t *r) {
    if (drag_ol_active) return;
    show_xor_outline(r);
    drag_ol_rect = *r;
    drag_ol_active = true;
}

void drag_overlay_erase(void) {
    if (!drag_ol_active) return;
    /* Erase cursor first — its save-under may contain XOR'd outline
     * pixels.  Restoring the cursor cleans the buffer so the outline
     * XOR-erase undoes correctly.  cursor_overlay_erase is a no-op
     * when the cursor isn't stamped; re-stamp happens later. */
    cursor_overlay_erase();
    show_xor_outline(&drag_ol_rect);
    drag_ol_active = false;
}

void drag_overlay_reset(void) {
    drag_ol_active = false;
}

void wm_handle_mouse_input(uint8_t type, int16_t x, int16_t y, uint8_t buttons) {
    /* ---- Active drag/resize in progress ---- */
    if (drag_mode != DRAG_NONE) {
        if (type == WM_MOUSEMOVE) {
            /* Only update drag_rect — compositor handles all XOR
             * erase/stamp via drag_overlay_erase/stamp.  Doing XOR
             * here from input_task races with compositor_task on the
             * dual-core RP2350, causing outline flicker & garbage. */
            if (drag_mode == DRAG_MOVE) {
                drag_rect.x = drag_orig.x + (x - drag_anchor_x);
                drag_rect.y = drag_orig.y + (y - drag_anchor_y);
            } else {
                update_resize_rect(x, y);
            }
            drag_moved = true;
            compositor_dirty = 1;
            return;
        }
        if (type == WM_LBUTTONUP) {
            /* Clear drag state FIRST.  The compositor reads drag_mode
             * to decide whether to stamp a new XOR outline; clearing
             * it before triggering the compositor prevents a race where
             * the compositor stamps one final outline after we erase
             * on the dual-core RP2350. */
            hwnd_t saved_hwnd = drag_hwnd;
            drag_mode = DRAG_NONE;
            drag_hwnd = HWND_NULL;
            cursor_set_type(CURSOR_ARROW);
            wm_set_window_rect(saved_hwnd, drag_rect.x, drag_rect.y,
                                drag_rect.w, drag_rect.h);
            /* Resume the app after the window has been repositioned */
            swap_resume(saved_hwnd);
            /* Don't manipulate overlays from the input task — the
             * compositor handles XOR outline erase in its synchronized
             * cycle, avoiding the TOCTOU race on cursor_overlay_is_locked.
             * wm_set_window_rect() adds an expose rect for the old frame
             * and marks WF_DIRTY|WF_FRAME_DIRTY for the selective path. */
            compositor_dirty = 1;
            return;
        }
        return;
    }

    /* ---- Overlay mouse routing priority ---- */
    /* Taskbar popup context menu */
    if (taskbar_popup_is_open()) {
        if (taskbar_popup_mouse(type, x, y)) return;
    }

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
        /* A new click always cancels any stale titlebar-button capture.
         * Normally LBUTTONUP clears it, but if an overlay consumed the
         * LBUTTONUP, the capture stays set and blocks all MOUSEMOVE
         * events (the tracker at line ~611 returns early).  Clearing
         * here guarantees the drag path is unblocked for this click. */
        if (titlebar_btn_hwnd != HWND_NULL) {
            titlebar_btn_shown = false;
            wm_invalidate(titlebar_btn_hwnd);
            titlebar_btn_hwnd = HWND_NULL;
            titlebar_btn_zone = HT_NOWHERE;
        }
        if (taskbar_popup_is_open()) taskbar_popup_close();
        if (startmenu_is_open()) startmenu_close();
        if (sysmenu_is_open()) sysmenu_close();
        if (menu_is_open()) menu_close();
        if (menu_popup_is_open()) menu_popup_close();
    }

    /* ---- Left button down: hit-test to decide action ---- */
    if (type == WM_LBUTTONDOWN) {
        hwnd_t target = wm_window_at_point(x, y);
        if (target == HWND_NULL) {
            /* Click on desktop — handle icon selection */
            desktop_mouse(type, x, y);
            return;
        }

        /* Modal blocking: if a modal dialog is open and the click
         * target is not the modal window, ignore the click. */
        if (modal_hwnd != HWND_NULL && target != modal_hwnd) {
            wm_invalidate(modal_hwnd); /* flash the dialog */
            return;
        }

        window_t *win = wm_get_window(target);
        if (!win) return;

        /* Resume suspended app before focusing */
        swap_switch_to(target);
        wm_set_focus(target);

        uint8_t zone = theme_hit_test(&win->frame, win->flags, x, y);

        switch (zone) {
            case HT_TITLEBAR: {
                uint32_t now = xTaskGetTickCount();
                if (target == dblclick_hwnd &&
                    (now - dblclick_tick) <= DBLCLICK_TICKS) {
                    /* Double-click: toggle maximize/restore */
                    dblclick_hwnd = HWND_NULL;
                    if (win->flags & WF_RESIZABLE) {
                        if (win->state == WS_MAXIMIZED)
                            wm_restore_window(target);
                        else
                            wm_maximize_window(target);
                        compositor_dirty = 1;
                    }
                    return;
                }
                dblclick_hwnd = target;
                dblclick_tick = now;
                if ((win->flags & WF_MOVABLE) && win->state != WS_MAXIMIZED) {
                    begin_drag(target, DRAG_MOVE, 0, x, y);
                }
                return;
            }

            case HT_CLOSE:
            case HT_MAXIMIZE:
            case HT_MINIMIZE:
                /* Capture button press — action fires on release.
                 * Set FRAME_DIRTY directly (wm_invalidate skips
                 * non-focused windows and doesn't repaint decorations). */
                titlebar_btn_hwnd  = target;
                titlebar_btn_zone  = zone;
                titlebar_btn_shown = true;
                win->flags |= WF_DIRTY | WF_FRAME_DIRTY;
                wm_mark_dirty();
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
        /* Title bar button release — execute action if still over button */
        if (titlebar_btn_hwnd != HWND_NULL) {
            hwnd_t btn_hwnd = titlebar_btn_hwnd;
            uint8_t btn_zone = titlebar_btn_zone;
            titlebar_btn_hwnd  = HWND_NULL;
            titlebar_btn_zone  = HT_NOWHERE;
            titlebar_btn_shown = false;

            window_t *bwin = wm_get_window(btn_hwnd);
            if (bwin) {
                uint8_t zone = theme_hit_test(&bwin->frame, bwin->flags, x, y);
                if (zone == btn_zone) {
                    switch (btn_zone) {
                    case HT_CLOSE:
                        if (bwin->flags & WF_CLOSABLE) {
                            window_event_t ev;
                            memset(&ev, 0, sizeof(ev));
                            ev.type = WM_CLOSE;
                            wm_post_event(btn_hwnd, &ev);
                        }
                        break;
                    case HT_MAXIMIZE:
                        if (bwin->flags & WF_RESIZABLE) {
                            if (bwin->state == WS_MAXIMIZED)
                                wm_restore_window(btn_hwnd);
                            else
                                wm_maximize_window(btn_hwnd);
                        }
                        break;
                    case HT_MINIMIZE:
                        wm_minimize_window(btn_hwnd);
                        break;
                    }
                }
            }
            if (bwin)
                bwin->flags |= WF_DIRTY | WF_FRAME_DIRTY;
            compositor_dirty = 1;
            return;
        }

        /* Route to overlays first */
        if (startmenu_is_open() && startmenu_mouse(type, x, y)) return;
        if (sysmenu_is_open() && sysmenu_mouse(type, x, y)) return;
        if (menu_is_open() && menu_dropdown_mouse(type, x, y)) return;

        /* If the button-up lands on the desktop (no window under cursor),
         * route to desktop_mouse() for double-click icon handling —
         * even when another window has focus. */
        if (wm_window_at_point(x, y) == HWND_NULL) {
            desktop_mouse(type, x, y);
            return;
        }

        hwnd_t focus = wm_get_focus();
        if (focus != HWND_NULL)
            forward_mouse_event(WM_LBUTTONUP, x, y, buttons, focus);
        else
            desktop_mouse(type, x, y);
        return;
    }

    /* ---- Mouse move (not dragging) ---- */
    if (type == WM_MOUSEMOVE) {
        /* Track titlebar button capture — toggle visual on/off */
        if (titlebar_btn_hwnd != HWND_NULL) {
            window_t *bwin = wm_get_window(titlebar_btn_hwnd);
            if (bwin) {
                uint8_t zone = theme_hit_test(&bwin->frame, bwin->flags, x, y);
                bool over = (zone == titlebar_btn_zone);
                if (over != titlebar_btn_shown) {
                    titlebar_btn_shown = over;
                    bwin->flags |= WF_DIRTY | WF_FRAME_DIRTY;
                    wm_mark_dirty();
                }
            }
            return;
        }

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

    /* ---- Right button events ---- */
    if (type == WM_RBUTTONDOWN) {
        /* Close taskbar popup if open and click is outside */
        if (taskbar_popup_is_open()) {
            if (taskbar_popup_mouse(type, x, y)) return;
        }
        /* Right-click on taskbar: show context menu */
        if (taskbar_mouse_rclick(x, y)) return;
        /* Forward to window under cursor */
        hwnd_t target = wm_window_at_point(x, y);
        if (target != HWND_NULL)
            forward_mouse_event(type, x, y, buttons, target);
        else
            desktop_mouse(type, x, y);
        return;
    }
    if (type == WM_RBUTTONUP) {
        hwnd_t target = wm_window_at_point(x, y);
        if (target != HWND_NULL)
            forward_mouse_event(type, x, y, buttons, target);
        else
            desktop_mouse(type, x, y);
        return;
    }
}

bool wm_get_drag_outline(rect_t *outline) {
    if (drag_mode == DRAG_NONE) return false;
    /* Suppress outline until the mouse has actually moved.  Without
     * this, the first outline sits exactly on the window border and
     * the XOR corrupts the border pixels on the single-buffer display. */
    if (!drag_moved) return false;
    if (outline) *outline = drag_rect;
    return true;
}

/*==========================================================================
 * Title bar button press query
 *=========================================================================*/

uint8_t wm_get_pressed_titlebar_btn(hwnd_t hwnd) {
    if (titlebar_btn_hwnd == hwnd && titlebar_btn_shown)
        return titlebar_btn_zone;
    return HT_NOWHERE;
}

/*==========================================================================
 * Keyboard window move mode
 *=========================================================================*/

static bool    kb_move_active = false;
static hwnd_t  kb_move_hwnd   = HWND_NULL;
#define KB_MOVE_STEP 8

void wm_begin_keyboard_move(hwnd_t hwnd) {
    window_t *win = wm_get_window(hwnd);
    if (!win || !(win->flags & WF_MOVABLE) || win->state == WS_MAXIMIZED) return;
    kb_move_active = true;
    kb_move_hwnd = hwnd;
    cursor_set_type(CURSOR_ARROW);
}

bool wm_keyboard_move_key(uint8_t hid_code) {
    if (!kb_move_active) return false;
    window_t *win = wm_get_window(kb_move_hwnd);
    if (!win) { kb_move_active = false; return false; }

    switch (hid_code) {
    case 0x50: /* LEFT */
        wm_move_window(kb_move_hwnd, win->frame.x - KB_MOVE_STEP, win->frame.y);
        break;
    case 0x4F: /* RIGHT */
        wm_move_window(kb_move_hwnd, win->frame.x + KB_MOVE_STEP, win->frame.y);
        break;
    case 0x52: /* UP */
        wm_move_window(kb_move_hwnd, win->frame.x, win->frame.y - KB_MOVE_STEP);
        break;
    case 0x51: /* DOWN */
        wm_move_window(kb_move_hwnd, win->frame.x, win->frame.y + KB_MOVE_STEP);
        break;
    case 0x28: /* Enter */
    case 0x29: /* Esc */
        kb_move_active = false;
        kb_move_hwnd = HWND_NULL;
        break;
    default:
        return false;
    }
    compositor_dirty = 1;
    return true;
}

bool wm_is_keyboard_move_active(void) {
    return kb_move_active;
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
