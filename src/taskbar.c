/*
 * FRANK OS
 * Copyright (c) 2026 Mikhail Matveev <xtreme@rh1.tech>
 * https://rh1.tech
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "taskbar.h"
#include "window.h"
#include "window_event.h"
#include "window_theme.h"
#include "startmenu.h"
#include "swap.h"
#include "snd.h"
#include "netcard.h"
#include "lang.h"
#include "settings.h"
#include "keyboard.h"
#include "gfx.h"
#include "font.h"
#include "display.h"
#include "FreeRTOS.h"
#include "task.h"
#include <string.h>

extern const uint8_t default_icon_16x16[256];

/* Network tray icons (net_icons.c) */
extern const uint8_t *net_icon16_noact_get(void);
extern const uint8_t *net_icon16_tx_get(void);
extern const uint8_t *net_icon16_rx_get(void);
extern const uint8_t *net_icon16_tx_rx_get(void);

/* Spawn Network Settings (main.c) */
extern void spawn_network_settings(void);

/*==========================================================================
 * Constants
 *=========================================================================*/

#define BUTTON_HEIGHT     22   /* height of taskbar buttons */
#define BUTTON_Y          (TASKBAR_Y + 3) /* y of taskbar buttons */
#define TASK_BUTTON_W    120   /* max width of per-window buttons */
#define TASK_BUTTON_PAD    2   /* padding between task buttons */
#define TASK_AREA_X       (TASKBAR_START_W + 6) /* where task buttons start */
#define TASK_AREA_W       (tray_x() - TASK_AREA_X - 4)

/* Notification area (system tray) — right side of taskbar.
 * Layout: [lang 16px?][4px][net_icon 16px?][4px][vol_icon 16px][4px][clock 40px]
 * Language indicator shown only when language != English. */
#define TRAY_LANG_W     16      /* width of language indicator box */

static inline bool tray_has_lang(void) { return settings_get()->language != LANG_EN; }
static inline bool tray_has_net(void)  { return netcard_wifi_connected(); }

static inline int tray_w(void) {
    int w = 4 + 16 + 4 + 40;   /* margin + vol + gap + clock = 64 base */
    if (tray_has_net())  w += 20; /* net icon + gap */
    if (tray_has_lang()) w += 20; /* lang box + gap */
    return w;
}
static inline int tray_x(void) { return (int)display_width - tray_w() - 2; }

/* Tray element positions (left to right) */
static inline int lang_icon_x(void) { return tray_x() + 4; }
static inline int net_icon_x(void)  { return lang_icon_x() + (tray_has_lang() ? 20 : 0); }
static inline int vol_icon_x(void)  { return net_icon_x() + (tray_has_net() ? 20 : 0); }
static inline int clock_x(void)     { return vol_icon_x() + 20; }
#define ICON_Y            (BUTTON_Y + 3)
#define CLOCK_Y           (BUTTON_Y + (BUTTON_HEIGHT - FONT_UI_HEIGHT) / 2)

/*==========================================================================
 * Internal state
 *=========================================================================*/

static bool taskbar_ready = false;
static bool taskbar_dirty = true;  /* starts dirty for first frame */

/* Last drawn clock minute — used by taskbar_tick() to detect changes */
static uint32_t last_clock_minute = 0xFFFFFFFF;

/* Volume popup state (defined here so taskbar_mouse_click can use them) */
static bool vol_popup_open_flag = false;
static bool vol_popup_dragging = false;

/* Forward declaration for network tray popup */
static void net_popup_open_at(int16_t x);

/* Volume icon from ICO (parsed on first use in fn_icons.c) */
extern const uint8_t *fn_icon16_volume_get(void);

/* Compute the button width based on how many task buttons are visible */
static int taskbar_btn_width(void) {
    int count = 0;
    for (uint8_t i = 1; i <= WM_MAX_WINDOWS; i++) {
        window_t *w = wm_get_window(i);
        if (w && (w->flags & WF_ALIVE) && (w->flags & WF_BORDER)) count++;
    }
    if (count == 0) return TASK_BUTTON_W;
    int avail = TASK_AREA_W - (count - 1) * TASK_BUTTON_PAD;
    int w = avail / count;
    if (w > TASK_BUTTON_W) w = TASK_BUTTON_W;
    if (w < 30) w = 30;
    return w;
}

/*==========================================================================
 * Helpers
 *=========================================================================*/

/* Draw a Win95-style raised button */
static void draw_raised_button(int x, int y, int w, int h) {
    gfx_fill_rect(x, y, w, h, THEME_BUTTON_FACE);
    /* Outer highlight */
    gfx_hline(x, y, w, COLOR_WHITE);
    gfx_vline(x, y, h, COLOR_WHITE);
    gfx_hline(x, y + h - 1, w, COLOR_BLACK);
    gfx_vline(x + w - 1, y, h, COLOR_BLACK);
    /* Inner highlight */
    gfx_hline(x + 1, y + h - 2, w - 2, COLOR_DARK_GRAY);
    gfx_vline(x + w - 2, y + 1, h - 2, COLOR_DARK_GRAY);
}

/* Draw a Win95-style sunken button (for focused window's task button) */
static void draw_sunken_button(int x, int y, int w, int h) {
    gfx_fill_rect(x, y, w, h, THEME_BUTTON_FACE);
    gfx_hline(x, y, w, COLOR_DARK_GRAY);
    gfx_vline(x, y, h, COLOR_DARK_GRAY);
    gfx_hline(x, y + h - 1, w, COLOR_WHITE);
    gfx_vline(x + w - 1, y, h, COLOR_WHITE);
}

/*==========================================================================
 * Public API
 *=========================================================================*/

void taskbar_init(void) {
    taskbar_ready = true;
}

int16_t taskbar_work_area_height(void) {
    return display_height - TASKBAR_HEIGHT;
}

void taskbar_invalidate(void) {
    taskbar_dirty = true;
    wm_mark_dirty();
}

void taskbar_force_dirty(void) {
    taskbar_dirty = true;
}

bool taskbar_needs_redraw(void) {
    return taskbar_dirty;
}

void taskbar_tick(void) {
    if (!taskbar_ready) return;
    uint32_t now_min = (xTaskGetTickCount() / configTICK_RATE_HZ) / 60;
    if (now_min != last_clock_minute)
        taskbar_invalidate();

    /* Force one more repaint after network activity expires so the
     * tray icon transitions from active back to idle. */
    static bool prev_net_active = false;
    bool net_active = netcard_wifi_connected() &&
                      netcard_get_icon_state() != NET_ICON_NOACT;
    if (prev_net_active && !net_active)
        taskbar_invalidate();
    prev_net_active = net_active;
}

void taskbar_draw(void) {
    if (!taskbar_ready || !taskbar_dirty) return;

    /* Suppress taskbar when focused window covers entire screen (fullscreen) */
    hwnd_t fs_focus = wm_get_focus();
    if (fs_focus != HWND_NULL) {
        window_t *fw = wm_get_window(fs_focus);
        if (fw && fw->frame.x == 0 && fw->frame.y == 0 &&
            fw->frame.w == display_width && fw->frame.h == display_height &&
            !(fw->flags & WF_BORDER))
            return;
    }

    taskbar_dirty = false;

    int y = TASKBAR_Y;

    /* Main bar background */
    gfx_fill_rect(0, y, display_width, TASKBAR_HEIGHT, THEME_BUTTON_FACE);

    /* Top raised edge */
    gfx_hline(0, y, display_width, COLOR_WHITE);
    gfx_hline(0, y + 1, display_width, THEME_BUTTON_FACE);

    /* Start button */
    bool start_open = startmenu_is_open();
    int start_x = 2;
    if (start_open) {
        draw_sunken_button(start_x, BUTTON_Y, TASKBAR_START_W, BUTTON_HEIGHT);
        gfx_text_ui_bold(start_x + 12 + 1, BUTTON_Y + (BUTTON_HEIGHT - FONT_UI_HEIGHT) / 2 + 1,
                         L(STR_START), COLOR_BLACK, THEME_BUTTON_FACE);
    } else {
        draw_raised_button(start_x, BUTTON_Y, TASKBAR_START_W, BUTTON_HEIGHT);
        gfx_text_ui_bold(start_x + 12, BUTTON_Y + (BUTTON_HEIGHT - FONT_UI_HEIGHT) / 2,
                         L(STR_START), COLOR_BLACK, THEME_BUTTON_FACE);
    }

    /* Per-window task buttons */
    hwnd_t focus = wm_get_focus();
    int btn_x = TASK_AREA_X;
    int btn_w = taskbar_btn_width();

    for (uint8_t i = 1; i <= WM_MAX_WINDOWS; i++) {
        window_t *win = wm_get_window(i);
        if (!win || !(win->flags & WF_ALIVE)) continue;
        if (!(win->flags & WF_BORDER)) continue; /* skip borderless popups */

        if (btn_x + btn_w > tray_x() - 4) break; /* no room for more buttons */

        bool is_focused = (i == focus) && (win->flags & WF_VISIBLE);

        if (is_focused) {
            draw_sunken_button(btn_x, BUTTON_Y, btn_w, BUTTON_HEIGHT);
        } else {
            draw_raised_button(btn_x, BUTTON_Y, btn_w, BUTTON_HEIGHT);
        }

        /* Draw 16x16 icon in button */
        int offset = is_focused ? 1 : 0;
        const uint8_t *icon = win->icon ? win->icon : default_icon_16x16;
        gfx_draw_icon_16(btn_x + 4 + offset, BUTTON_Y + 3 + offset, icon);

        /* Truncated title text (shifted right for icon).
         * Suspended apps show title in dark gray to indicate they're inactive. */
        int text_x = btn_x + 22 + offset;
        int text_y = BUTTON_Y + (BUTTON_HEIGHT - FONT_UI_HEIGHT) / 2 + offset;
        int text_max_w = btn_w - 26;
        if (text_max_w > 0) {
            uint8_t text_color = (win->flags & WF_SUSPENDED) ?
                                  COLOR_DARK_GRAY : COLOR_BLACK;
            gfx_text_ui_clipped(text_x, text_y, win->title,
                                text_color, THEME_BUTTON_FACE,
                                btn_x + 2, BUTTON_Y, btn_w - 4, BUTTON_HEIGHT);
        }

        btn_x += btn_w + TASK_BUTTON_PAD;
    }

    /* ---- Notification area (sunken well with icons + clock) ---- */
    {
        int tx = tray_x();
        int ty = BUTTON_Y;
        int tw = tray_w();
        int th = BUTTON_HEIGHT;

        /* Sunken well border */
        gfx_fill_rect(tx, ty, tw, th, THEME_BUTTON_FACE);
        gfx_hline(tx, ty, tw, COLOR_DARK_GRAY);
        gfx_vline(tx, ty, th, COLOR_DARK_GRAY);
        gfx_hline(tx + 1, ty + th - 1, tw - 1, COLOR_WHITE);
        gfx_vline(tx + tw - 1, ty + 1, th - 1, COLOR_WHITE);

        /* Language indicator (only shown when language != English) */
        if (tray_has_lang()) {
            int lx = lang_icon_x();
            int ly = ICON_Y;
            /* Blue box with white text: EN or RU */
            gfx_fill_rect(lx, ly, TRAY_LANG_W, 16, COLOR_BLUE);
            const char *lt = keyboard_is_russian() ? "RU" : "EN";
            int tw2 = 2 * FONT_UI_WIDTH;
            gfx_text_ui(lx + (TRAY_LANG_W - tw2) / 2,
                        ly + (16 - FONT_UI_HEIGHT) / 2,
                        lt, COLOR_WHITE, COLOR_BLUE);
        }

        /* Network icon (only shown when WiFi is connected) */
        if (netcard_wifi_connected()) {
            const uint8_t *net_icon;
            switch (netcard_get_icon_state()) {
            case NET_ICON_TX:    net_icon = net_icon16_tx_get();    break;
            case NET_ICON_RX:    net_icon = net_icon16_rx_get();    break;
            case NET_ICON_TX_RX: net_icon = net_icon16_tx_rx_get(); break;
            default:             net_icon = net_icon16_noact_get(); break;
            }
            gfx_draw_icon_16(net_icon_x(), ICON_Y, net_icon);
        }

        /* Speaker icon */
        gfx_draw_icon_16(vol_icon_x(), ICON_Y, fn_icon16_volume_get());

        /* Uptime clock: HH:MM */
        uint32_t ticks = xTaskGetTickCount();
        uint32_t total_sec = ticks / configTICK_RATE_HZ;
        uint32_t hours = total_sec / 3600;
        uint32_t minutes = (total_sec % 3600) / 60;
        last_clock_minute = total_sec / 60;

        char clk[6];
        clk[0] = '0' + (hours / 10) % 10;
        clk[1] = '0' + hours % 10;
        clk[2] = ':';
        clk[3] = '0' + minutes / 10;
        clk[4] = '0' + minutes % 10;
        clk[5] = '\0';

        gfx_text_ui(clock_x(), CLOCK_Y, clk, COLOR_BLACK, THEME_BUTTON_FACE);
    }
}

bool taskbar_mouse_click(int16_t x, int16_t y) {
    if (y < TASKBAR_Y) return false;

    /* Start button */
    if (x >= 2 && x < 2 + TASKBAR_START_W &&
        y >= BUTTON_Y && y < BUTTON_Y + BUTTON_HEIGHT) {
        startmenu_toggle();
        return true;
    }

    /* Network icon click — open Network Settings */
    if (netcard_wifi_connected() &&
        x >= net_icon_x() && x < net_icon_x() + 16 &&
        y >= BUTTON_Y && y < BUTTON_Y + BUTTON_HEIGHT) {
        spawn_network_settings();
        return true;
    }

    /* Volume icon in notification area — toggle popup */
    if (x >= vol_icon_x() && x < vol_icon_x() + 20 &&
        y >= BUTTON_Y && y < BUTTON_Y + BUTTON_HEIGHT) {
        if (vol_popup_is_open())
            vol_popup_close();
        else {
            vol_popup_open_flag = true;
            vol_popup_dragging = false;
            wm_mark_dirty();
        }
        return true;
    }

    /* Clock area — consume but ignore */
    if (x >= tray_x() && x < tray_x() + tray_w() &&
        y >= BUTTON_Y && y < BUTTON_Y + BUTTON_HEIGHT) {
        return true;
    }

    /* Per-window task buttons */
    int btn_x = TASK_AREA_X;
    int btn_w = taskbar_btn_width();

    for (uint8_t i = 1; i <= WM_MAX_WINDOWS; i++) {
        window_t *win = wm_get_window(i);
        if (!win || !(win->flags & WF_ALIVE)) continue;
        if (!(win->flags & WF_BORDER)) continue;

        if (btn_x + btn_w > display_width) break;

        if (x >= btn_x && x < btn_x + btn_w &&
            y >= BUTTON_Y && y < BUTTON_Y + BUTTON_HEIGHT) {
            hwnd_t hwnd = (hwnd_t)i;
            if (win->state == WS_MINIMIZED) {
                wm_restore_window(hwnd);
            }
            swap_switch_to(hwnd);
            wm_set_focus(hwnd);
            wm_mark_dirty();
            return true;
        }

        btn_x += btn_w + TASK_BUTTON_PAD;
    }

    return true; /* consumed the click (was in taskbar area) */
}

/*==========================================================================
 * Taskbar right-click context menu (standalone popup, like sysmenu)
 *=========================================================================*/

#define TB_ITEM_HEIGHT  20
#define TB_MENU_WIDTH  100
#define TB_MAX_ITEMS     3

#define TB_ID_RESTORE    1
#define TB_ID_MINIMIZE   2
#define TB_ID_CLOSE      3

typedef struct {
    const char *text;
    uint8_t     id;
} tb_popup_item_t;

static bool   tb_popup_open_flag = false;
static hwnd_t tb_popup_hwnd = HWND_NULL;
static int8_t tb_popup_hover = -1;
static int16_t tb_popup_x, tb_popup_y;
static tb_popup_item_t tb_items[TB_MAX_ITEMS];
static int tb_item_count = 0;

bool taskbar_popup_is_open(void) {
    return tb_popup_open_flag;
}

void taskbar_popup_close(void) {
    tb_popup_open_flag = false;
    tb_popup_hwnd = HWND_NULL;
    tb_popup_hover = -1;
    wm_mark_dirty();
}

bool taskbar_mouse_rclick(int16_t x, int16_t y) {
    if (y < TASKBAR_Y) return false;

    /* Right-click on network icon: show network context menu */
    if (netcard_wifi_connected() &&
        x >= net_icon_x() && x < net_icon_x() + 16 &&
        y >= BUTTON_Y && y < BUTTON_Y + BUTTON_HEIGHT) {
        net_popup_open_at(x);
        return true;
    }

    int btn_x = TASK_AREA_X;
    int btn_w = taskbar_btn_width();

    for (uint8_t i = 1; i <= WM_MAX_WINDOWS; i++) {
        window_t *win = wm_get_window(i);
        if (!win || !(win->flags & WF_ALIVE) || !(win->flags & WF_BORDER))
            continue;

        if (x >= btn_x && x < btn_x + btn_w &&
            y >= BUTTON_Y && y < BUTTON_Y + BUTTON_HEIGHT) {
            /* Build popup items for this window */
            tb_popup_hwnd = (hwnd_t)i;
            tb_item_count = 0;

            if (win->state == WS_MINIMIZED) {
                tb_items[tb_item_count++] = (tb_popup_item_t){ L(STR_RESTORE), TB_ID_RESTORE };
            } else {
                tb_items[tb_item_count++] = (tb_popup_item_t){ L(STR_MINIMIZE), TB_ID_MINIMIZE };
            }
            tb_items[tb_item_count++] = (tb_popup_item_t){ L(STR_CLOSE), TB_ID_CLOSE };

            /* Position popup above the clicked button */
            int menu_h = 4 + tb_item_count * TB_ITEM_HEIGHT;
            tb_popup_x = btn_x;
            tb_popup_y = TASKBAR_Y - menu_h;
            if (tb_popup_x + TB_MENU_WIDTH > display_width)
                tb_popup_x = display_width - TB_MENU_WIDTH;

            tb_popup_hover = 0;
            tb_popup_open_flag = true;
            wm_mark_dirty();
            return true;
        }

        btn_x += btn_w + TASK_BUTTON_PAD;
    }

    return true; /* consumed (was in taskbar area) */
}

static void tb_popup_execute(uint8_t id) {
    hwnd_t hwnd = tb_popup_hwnd;
    taskbar_popup_close();

    switch (id) {
    case TB_ID_RESTORE:
        wm_restore_window(hwnd);
        wm_set_focus(hwnd);
        break;
    case TB_ID_MINIMIZE:
        wm_minimize_window(hwnd);
        break;
    case TB_ID_CLOSE:
        if (swap_is_suspended(hwnd)) {
            /* Suspended app can't process WM_CLOSE — force-close it */
            swap_force_close(hwnd);
        } else {
            window_event_t ev;
            memset(&ev, 0, sizeof(ev));
            ev.type = WM_CLOSE;
            wm_post_event(hwnd, &ev);
        }
        break;
    }
    wm_mark_dirty();
}

void taskbar_popup_draw(void) {
    if (!tb_popup_open_flag) return;

    int menu_h = 4 + tb_item_count * TB_ITEM_HEIGHT;

    /* Shadow */
    gfx_fill_rect_dithered(tb_popup_x + 1, tb_popup_y + 1,
                           TB_MENU_WIDTH, menu_h, COLOR_BLACK);

    /* Background */
    gfx_fill_rect(tb_popup_x, tb_popup_y, TB_MENU_WIDTH, menu_h, THEME_BUTTON_FACE);

    /* Border */
    gfx_hline(tb_popup_x, tb_popup_y, TB_MENU_WIDTH, COLOR_WHITE);
    gfx_vline(tb_popup_x, tb_popup_y, menu_h, COLOR_WHITE);
    gfx_hline(tb_popup_x, tb_popup_y + menu_h - 1, TB_MENU_WIDTH, COLOR_BLACK);
    gfx_vline(tb_popup_x + TB_MENU_WIDTH - 1, tb_popup_y, menu_h, COLOR_BLACK);
    gfx_hline(tb_popup_x + 1, tb_popup_y + menu_h - 2, TB_MENU_WIDTH - 2, COLOR_DARK_GRAY);
    gfx_vline(tb_popup_x + TB_MENU_WIDTH - 2, tb_popup_y + 1, menu_h - 2, COLOR_DARK_GRAY);

    /* Items */
    int iy = tb_popup_y + 2;
    for (int i = 0; i < tb_item_count; i++) {
        bool hovered = (i == tb_popup_hover);
        uint8_t bg = hovered ? COLOR_BLUE : THEME_BUTTON_FACE;
        uint8_t fg = hovered ? COLOR_WHITE : COLOR_BLACK;

        gfx_fill_rect(tb_popup_x + 2, iy, TB_MENU_WIDTH - 4, TB_ITEM_HEIGHT, bg);
        gfx_text_ui(tb_popup_x + 6, iy + (TB_ITEM_HEIGHT - FONT_UI_HEIGHT) / 2,
                    tb_items[i].text, fg, bg);
        iy += TB_ITEM_HEIGHT;
    }
}

bool taskbar_popup_mouse(uint8_t type, int16_t x, int16_t y) {
    if (!tb_popup_open_flag) return false;

    int menu_h = 4 + tb_item_count * TB_ITEM_HEIGHT;

    if (x >= tb_popup_x && x < tb_popup_x + TB_MENU_WIDTH &&
        y >= tb_popup_y && y < tb_popup_y + menu_h) {
        if (type == WM_MOUSEMOVE || type == WM_LBUTTONDOWN) {
            int iy = tb_popup_y + 2;
            int new_hover = -1;
            for (int i = 0; i < tb_item_count; i++) {
                if (y >= iy && y < iy + TB_ITEM_HEIGHT) {
                    new_hover = i;
                    break;
                }
                iy += TB_ITEM_HEIGHT;
            }
            if (new_hover != tb_popup_hover) {
                tb_popup_hover = new_hover;
                wm_mark_dirty();
            }
        }
        if (type == WM_LBUTTONUP && tb_popup_hover >= 0) {
            tb_popup_execute(tb_items[tb_popup_hover].id);
        }
        return true;
    }

    /* Click outside — close */
    if (type == WM_LBUTTONDOWN || type == WM_RBUTTONDOWN) {
        taskbar_popup_close();
        return false;
    }
    return false;
}

bool taskbar_popup_handle_key(uint8_t hid_code, uint8_t modifiers) {
    if (!tb_popup_open_flag) return false;
    (void)modifiers;

    switch (hid_code) {
    case 0x52: /* UP */
        tb_popup_hover--;
        if (tb_popup_hover < 0) tb_popup_hover = tb_item_count - 1;
        wm_mark_dirty();
        return true;
    case 0x51: /* DOWN */
        tb_popup_hover++;
        if (tb_popup_hover >= tb_item_count) tb_popup_hover = 0;
        wm_mark_dirty();
        return true;
    case 0x28: /* ENTER */
        if (tb_popup_hover >= 0) tb_popup_execute(tb_items[tb_popup_hover].id);
        return true;
    case 0x29: /* ESC */
        taskbar_popup_close();
        return true;
    }
    return false;
}

/*==========================================================================
 * Volume popup — Win95-style vertical slider above the speaker icon
 *=========================================================================*/

#define VP_WIDTH     58
#define VP_HEIGHT   100
#define VP_X        (tray_x() + (tray_w() - VP_WIDTH) / 2)
#define VP_Y        (TASKBAR_Y - VP_HEIGHT - 2)

/* Track geometry inside the popup */
#define VP_TRACK_X   (VP_X + (VP_WIDTH - 10) / 2)
#define VP_TRACK_Y   (VP_Y + 20)
#define VP_TRACK_W   10
#define VP_TRACK_H   50

/* Thumb */
#define VP_THUMB_W   16
#define VP_THUMB_H    8
#define VP_THUMB_X   (VP_TRACK_X + (VP_TRACK_W - VP_THUMB_W) / 2)

/* Mute checkbox */
#define VP_MUTE_X    (VP_X + 8)
#define VP_MUTE_Y    (VP_Y + VP_HEIGHT - 20)
#define VP_CHECK_SZ  10

/* Volume to slider Y position: vol 0 (max) = top, vol 4 (mute) = bottom */
static int vol_thumb_y(void) {
    uint8_t v = snd_get_volume();
    if (v > 4) v = 4;
    int range = VP_TRACK_H - VP_THUMB_H;
    return VP_TRACK_Y + (v * range) / 4;
}

/* Slider Y position to volume level */
static uint8_t vol_from_y(int16_t y) {
    int range = VP_TRACK_H - VP_THUMB_H;
    int pos = y - VP_TRACK_Y;
    if (pos < 0) pos = 0;
    if (pos > range) pos = range;
    return (uint8_t)((pos * 4 + range / 2) / range);
}

bool vol_popup_is_open(void) {
    return vol_popup_open_flag;
}

void vol_popup_close(void) {
    vol_popup_open_flag = false;
    vol_popup_dragging = false;
    wm_mark_dirty();
}

void vol_popup_draw(void) {
    if (!vol_popup_open_flag) return;

    int px = VP_X, py = VP_Y, pw = VP_WIDTH, ph = VP_HEIGHT;

    /* Shadow */
    gfx_fill_rect_dithered(px + 1, py + 1, pw, ph, COLOR_BLACK);

    /* Background */
    gfx_fill_rect(px, py, pw, ph, THEME_BUTTON_FACE);

    /* 3D border */
    gfx_hline(px, py, pw, COLOR_WHITE);
    gfx_vline(px, py, ph, COLOR_WHITE);
    gfx_hline(px, py + ph - 1, pw, COLOR_BLACK);
    gfx_vline(px + pw - 1, py, ph, COLOR_BLACK);
    gfx_hline(px + 1, py + ph - 2, pw - 2, COLOR_DARK_GRAY);
    gfx_vline(px + pw - 2, py + 1, ph - 2, COLOR_DARK_GRAY);

    /* "Volume" label */
    gfx_text_ui(px + (pw - 6 * 6) / 2, py + 4, L(STR_VOLUME),
                COLOR_BLACK, THEME_BUTTON_FACE);

    /* Sunken track */
    int tx = VP_TRACK_X, ty = VP_TRACK_Y;
    gfx_fill_rect(tx, ty, VP_TRACK_W, VP_TRACK_H, THEME_BUTTON_FACE);
    gfx_hline(tx, ty, VP_TRACK_W, COLOR_DARK_GRAY);
    gfx_vline(tx, ty, VP_TRACK_H, COLOR_DARK_GRAY);
    gfx_hline(tx + 1, ty + VP_TRACK_H - 1, VP_TRACK_W - 1, COLOR_WHITE);
    gfx_vline(tx + VP_TRACK_W - 1, ty + 1, VP_TRACK_H - 1, COLOR_WHITE);

    /* Tick marks beside the track */
    for (int i = 0; i <= 4; i++) {
        int range = VP_TRACK_H - VP_THUMB_H;
        int tick_y = VP_TRACK_Y + (i * range) / 4 + VP_THUMB_H / 2;
        gfx_hline(tx - 4, tick_y, 3, COLOR_BLACK);
        gfx_hline(tx + VP_TRACK_W + 1, tick_y, 3, COLOR_BLACK);
    }

    /* Thumb (raised button) */
    int thumb_y = vol_thumb_y();
    draw_raised_button(VP_THUMB_X, thumb_y, VP_THUMB_W, VP_THUMB_H);

    /* Mute checkbox */
    bool muted = (snd_get_volume() >= 4);
    int cx = VP_MUTE_X, cy = VP_MUTE_Y;

    /* Sunken checkbox box */
    gfx_fill_rect(cx, cy, VP_CHECK_SZ, VP_CHECK_SZ, COLOR_WHITE);
    gfx_hline(cx, cy, VP_CHECK_SZ, COLOR_DARK_GRAY);
    gfx_vline(cx, cy, VP_CHECK_SZ, COLOR_DARK_GRAY);
    gfx_hline(cx + 1, cy + VP_CHECK_SZ - 1, VP_CHECK_SZ - 1, COLOR_WHITE);
    gfx_vline(cx + VP_CHECK_SZ - 1, cy + 1, VP_CHECK_SZ - 1, COLOR_WHITE);

    if (muted) {
        /* Draw X check mark */
        for (int i = 2; i < VP_CHECK_SZ - 2; i++) {
            display_set_pixel(cx + i, cy + i, COLOR_BLACK);
            display_set_pixel(cx + VP_CHECK_SZ - 1 - i, cy + i, COLOR_BLACK);
        }
    }

    gfx_text_ui(cx + VP_CHECK_SZ + 3, cy + (VP_CHECK_SZ - FONT_UI_HEIGHT) / 2,
                L(STR_MUTE), COLOR_BLACK, THEME_BUTTON_FACE);
}

bool vol_popup_mouse(uint8_t type, int16_t x, int16_t y) {
    if (!vol_popup_open_flag) return false;

    /* Dragging the thumb */
    if (vol_popup_dragging) {
        if (type == WM_MOUSEMOVE || type == WM_LBUTTONDOWN) {
            int thumb_center_y = y - VP_THUMB_H / 2;
            uint8_t new_vol = vol_from_y(thumb_center_y);
            if (new_vol != snd_get_volume()) {
                snd_set_volume(new_vol);
                wm_mark_dirty();
            }
            return true;
        }
        if (type == WM_LBUTTONUP) {
            vol_popup_dragging = false;
            return true;
        }
        return true;
    }

    /* Check if click is inside popup bounds */
    if (x >= VP_X && x < VP_X + VP_WIDTH &&
        y >= VP_Y && y < VP_Y + VP_HEIGHT) {

        if (type == WM_LBUTTONDOWN) {
            /* Check mute checkbox area */
            if (x >= VP_MUTE_X && x < VP_MUTE_X + VP_CHECK_SZ + 30 &&
                y >= VP_MUTE_Y && y < VP_MUTE_Y + VP_CHECK_SZ) {
                if (snd_get_volume() >= 4)
                    snd_set_volume(0);
                else
                    snd_set_volume(4);
                wm_mark_dirty();
                return true;
            }

            /* Check thumb area — start drag */
            int ty = vol_thumb_y();
            if (x >= VP_THUMB_X && x < VP_THUMB_X + VP_THUMB_W &&
                y >= ty && y < ty + VP_THUMB_H) {
                vol_popup_dragging = true;
                return true;
            }

            /* Click on track — jump to position */
            if (x >= VP_TRACK_X && x < VP_TRACK_X + VP_TRACK_W &&
                y >= VP_TRACK_Y && y < VP_TRACK_Y + VP_TRACK_H) {
                uint8_t new_vol = vol_from_y(y - VP_THUMB_H / 2);
                snd_set_volume(new_vol);
                vol_popup_dragging = true;
                wm_mark_dirty();
                return true;
            }
        }

        return true; /* consume all events inside popup */
    }

    /* Click outside — close */
    if (type == WM_LBUTTONDOWN || type == WM_RBUTTONDOWN) {
        vol_popup_close();
        return false;
    }
    return false;
}

/*==========================================================================
 * Network tray right-click context menu
 *=========================================================================*/

#define NET_POPUP_W       110
#define NET_ITEM_HEIGHT    20
#define NET_POPUP_ITEMS     2

#define NET_ID_SETTINGS     1
#define NET_ID_DISCONNECT   2

static bool   net_popup_open_flag = false;
static int8_t net_popup_hover = -1;
static int16_t net_popup_x, net_popup_y;

static void net_popup_open_at(int16_t x) {
    int menu_h = 4 + NET_POPUP_ITEMS * NET_ITEM_HEIGHT;
    net_popup_x = x - NET_POPUP_W / 2;
    if (net_popup_x < 0) net_popup_x = 0;
    if (net_popup_x + NET_POPUP_W > (int)display_width)
        net_popup_x = display_width - NET_POPUP_W;
    net_popup_y = TASKBAR_Y - menu_h;
    net_popup_hover = 0;
    net_popup_open_flag = true;
    wm_mark_dirty();
}

bool net_popup_is_open(void) {
    return net_popup_open_flag;
}

void net_popup_close(void) {
    net_popup_open_flag = false;
    net_popup_hover = -1;
    wm_mark_dirty();
}

static void net_popup_execute(uint8_t id) {
    net_popup_close();
    switch (id) {
    case NET_ID_SETTINGS:
        spawn_network_settings();
        break;
    case NET_ID_DISCONNECT:
        netcard_request_quit(NULL);
        taskbar_invalidate();
        break;
    }
}

void net_popup_draw(void) {
    if (!net_popup_open_flag) return;

    const char *labels[] = { L(STR_SETTINGS), L(STR_DISCONNECT) };
    static const uint8_t ids[] = { NET_ID_SETTINGS, NET_ID_DISCONNECT };
    int menu_h = 4 + NET_POPUP_ITEMS * NET_ITEM_HEIGHT;

    /* Shadow */
    gfx_fill_rect_dithered(net_popup_x + 1, net_popup_y + 1,
                           NET_POPUP_W, menu_h, COLOR_BLACK);

    /* Background */
    gfx_fill_rect(net_popup_x, net_popup_y, NET_POPUP_W, menu_h, THEME_BUTTON_FACE);

    /* Border */
    gfx_hline(net_popup_x, net_popup_y, NET_POPUP_W, COLOR_WHITE);
    gfx_vline(net_popup_x, net_popup_y, menu_h, COLOR_WHITE);
    gfx_hline(net_popup_x, net_popup_y + menu_h - 1, NET_POPUP_W, COLOR_BLACK);
    gfx_vline(net_popup_x + NET_POPUP_W - 1, net_popup_y, menu_h, COLOR_BLACK);
    gfx_hline(net_popup_x + 1, net_popup_y + menu_h - 2, NET_POPUP_W - 2, COLOR_DARK_GRAY);
    gfx_vline(net_popup_x + NET_POPUP_W - 2, net_popup_y + 1, menu_h - 2, COLOR_DARK_GRAY);

    /* Items */
    int iy = net_popup_y + 2;
    for (int i = 0; i < NET_POPUP_ITEMS; i++) {
        bool hovered = (i == net_popup_hover);
        uint8_t bg = hovered ? COLOR_BLUE : THEME_BUTTON_FACE;
        uint8_t fg = hovered ? COLOR_WHITE : COLOR_BLACK;

        gfx_fill_rect(net_popup_x + 2, iy, NET_POPUP_W - 4, NET_ITEM_HEIGHT, bg);
        gfx_text_ui(net_popup_x + 6, iy + (NET_ITEM_HEIGHT - FONT_UI_HEIGHT) / 2,
                    labels[i], fg, bg);
        iy += NET_ITEM_HEIGHT;
    }
    (void)ids;
}

bool net_popup_mouse(uint8_t type, int16_t x, int16_t y) {
    if (!net_popup_open_flag) return false;

    int menu_h = 4 + NET_POPUP_ITEMS * NET_ITEM_HEIGHT;

    if (x >= net_popup_x && x < net_popup_x + NET_POPUP_W &&
        y >= net_popup_y && y < net_popup_y + menu_h) {
        if (type == WM_MOUSEMOVE || type == WM_LBUTTONDOWN) {
            int iy = net_popup_y + 2;
            int new_hover = -1;
            for (int i = 0; i < NET_POPUP_ITEMS; i++) {
                if (y >= iy && y < iy + NET_ITEM_HEIGHT) {
                    new_hover = i;
                    break;
                }
                iy += NET_ITEM_HEIGHT;
            }
            if (new_hover != net_popup_hover) {
                net_popup_hover = new_hover;
                wm_mark_dirty();
            }
        }
        if (type == WM_LBUTTONUP && net_popup_hover >= 0) {
            uint8_t id = (net_popup_hover == 0) ? NET_ID_SETTINGS : NET_ID_DISCONNECT;
            net_popup_execute(id);
        }
        return true;
    }

    /* Click outside — close */
    if (type == WM_LBUTTONDOWN || type == WM_RBUTTONDOWN) {
        net_popup_close();
        return false;
    }
    return false;
}

bool net_popup_handle_key(uint8_t hid_code, uint8_t modifiers) {
    if (!net_popup_open_flag) return false;
    (void)modifiers;

    switch (hid_code) {
    case 0x52: /* UP */
        net_popup_hover--;
        if (net_popup_hover < 0) net_popup_hover = NET_POPUP_ITEMS - 1;
        wm_mark_dirty();
        return true;
    case 0x51: /* DOWN */
        net_popup_hover++;
        if (net_popup_hover >= NET_POPUP_ITEMS) net_popup_hover = 0;
        wm_mark_dirty();
        return true;
    case 0x28: /* ENTER */
        if (net_popup_hover >= 0) {
            uint8_t id = (net_popup_hover == 0) ? NET_ID_SETTINGS : NET_ID_DISCONNECT;
            net_popup_execute(id);
        }
        return true;
    case 0x29: /* ESC */
        net_popup_close();
        return true;
    }
    return false;
}
