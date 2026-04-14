/*
 * FRANK OS — Control Panel
 * Copyright (c) 2026 Mikhail Matveev <xtreme@rh1.tech>
 * https://rh1.tech
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "control_panel.h"
#include "controls.h"
#include "settings.h"
#include "window.h"
#include "window_event.h"
#include "window_theme.h"
#include "window_draw.h"
#include "gfx.h"
#include "font.h"
#include "display.h"
#include "dialog.h"
#include "taskbar.h"
#include "lang.h"
#include "board_config.h"
#include "FreeRTOS.h"
#include "task.h"
#include "hardware/clocks.h"
#include <string.h>
#include <stdio.h>

/* Forward declarations for psram */
extern uint32_t psram_detected_bytes(void);

/*==========================================================================
 * Icon data (defined in cp_icons.c)
 *=========================================================================*/

extern const uint8_t cp_icon16[];
extern const uint8_t cp_icon32[];
extern const uint8_t cp_disp_icon32[];
extern const uint8_t cp_sys_icon32[];
extern const uint8_t cp_mouse_icon32[];
extern const uint8_t cp_freq_icon32[];
extern const uint8_t cp_lang_icon32[];

/* Folder icon for mouse double-click test */
extern const uint8_t fn_icon32_folder[];

/*==========================================================================
 * Configurable double-click speed (defined in window_event.c)
 *=========================================================================*/

extern void wm_set_dblclick_speed(uint16_t ms);

/*==========================================================================
 * Desktop color update
 *=========================================================================*/

extern void desktop_set_bg_color(uint8_t color);

/*==========================================================================
 * Control Panel main window
 *=========================================================================*/

#define CP_ITEMS      5
#define CP_CELL_W    76
#define CP_CELL_H    56
#define CP_ICON_SIZE 32
#define CP_MARGIN_X   8
#define CP_MARGIN_Y   8

typedef struct {
    hwnd_t  hwnd;
    int8_t  selected;
    int8_t  hover;
    /* Double-click tracking */
    int8_t  last_click_idx;
    uint32_t last_click_tick;
} cp_state_t;

static cp_state_t cp_state;

/* Label string IDs for each CP item */
static const int cp_str_ids[CP_ITEMS] = {
    STR_DESKTOP, STR_SYSTEM, STR_MOUSE, STR_FREQUENCIES, STR_LANGUAGE
};

static const uint8_t *cp_icons[CP_ITEMS];

/* Forward declarations for applet creation */
static void cp_open_display(void);
static void cp_open_system(void);
static void cp_open_mouse(void);
static void cp_open_freq(void);
static void cp_open_lang(void);
static void cp_open_applet(int idx) {
    switch (idx) {
    case 0: cp_open_display(); break;
    case 1: cp_open_system();  break;
    case 2: cp_open_mouse();   break;
    case 3: cp_open_freq();    break;
    case 4: cp_open_lang();    break;
    }
}

/* Grid hit-test: returns item index or -1 */
static int cp_hit(int16_t x, int16_t y) {
    /* Items laid out left-to-right */
    for (int i = 0; i < CP_ITEMS; i++) {
        int cx = CP_MARGIN_X + i * CP_CELL_W;
        int cy = CP_MARGIN_Y;
        if (x >= cx && x < cx + CP_CELL_W &&
            y >= cy && y < cy + CP_CELL_H)
            return i;
    }
    return -1;
}

static bool cp_event(hwnd_t hwnd, const window_event_t *ev) {
    cp_state_t *st = &cp_state;

    switch (ev->type) {
    case WM_CLOSE:
        wm_destroy_window(hwnd);
        memset(st, 0, sizeof(*st));
        return true;

    case WM_LBUTTONDOWN: {
        int idx = cp_hit(ev->mouse.x, ev->mouse.y);
        if (idx >= 0) {
            st->selected = idx;
            wm_invalidate(hwnd);
        }
        return true;
    }

    case WM_LBUTTONUP: {
        int idx = cp_hit(ev->mouse.x, ev->mouse.y);
        uint32_t now = xTaskGetTickCount();
        if (idx >= 0 && idx == st->last_click_idx &&
            (now - st->last_click_tick) < pdMS_TO_TICKS(400)) {
            /* Double-click: open applet */
            cp_open_applet(idx);
            st->last_click_idx = -1;
            st->last_click_tick = 0;
        } else {
            st->last_click_idx = idx;
            st->last_click_tick = now;
        }
        return true;
    }

    case WM_KEYDOWN:
        if (ev->key.scancode == 0x28 /* Enter */ && st->selected >= 0) {
            cp_open_applet(st->selected);
            return true;
        }
        /* Arrow keys */
        if (ev->key.scancode == 0x4F /* Right */) {
            st->selected = (st->selected + 1) % CP_ITEMS;
            wm_invalidate(hwnd);
            return true;
        }
        if (ev->key.scancode == 0x50 /* Left */) {
            st->selected = (st->selected + CP_ITEMS - 1) % CP_ITEMS;
            wm_invalidate(hwnd);
            return true;
        }
        break;

    default:
        break;
    }
    return false;
}

static void cp_paint(hwnd_t hwnd) {
    cp_state_t *st = &cp_state;
    wd_begin(hwnd);
    wd_clear(COLOR_WHITE);

    for (int i = 0; i < CP_ITEMS; i++) {
        int cx = CP_MARGIN_X + i * CP_CELL_W;
        int cy = CP_MARGIN_Y;

        /* Draw 32x32 icon centered in cell */
        int icon_x = cx + (CP_CELL_W - CP_ICON_SIZE) / 2;
        int icon_y = cy + 2;
        if (cp_icons[i])
            wd_icon_32(icon_x, icon_y, cp_icons[i]);

        /* Draw label centered below icon */
        const char *label = L(cp_str_ids[i]);
        int tw = gfx_utf8_charcount(label) * FONT_UI_WIDTH;
        int text_x = cx + (CP_CELL_W - tw) / 2;
        int text_y = icon_y + CP_ICON_SIZE + 2;

        bool selected = (i == st->selected);
        if (selected) {
            wd_fill_rect(text_x - 1, text_y - 1,
                         tw + 2, FONT_UI_HEIGHT + 2, COLOR_BLUE);
        }
        wd_text_ui(text_x, text_y, label,
                   selected ? COLOR_WHITE : COLOR_BLACK,
                   selected ? COLOR_BLUE : COLOR_WHITE);
    }

    wd_end();
}

hwnd_t control_panel_create(void) {
    /* Initialize icon pointers */
    cp_icons[0] = cp_disp_icon32;
    cp_icons[1] = cp_sys_icon32;
    cp_icons[2] = cp_mouse_icon32;
    cp_icons[3] = cp_freq_icon32;
    cp_icons[4] = cp_lang_icon32;

    memset(&cp_state, 0, sizeof(cp_state));
    cp_state.selected = 0;
    cp_state.last_click_idx = -1;

    int w = CP_MARGIN_X * 2 + CP_ITEMS * CP_CELL_W + THEME_BORDER_WIDTH * 2;
    int h = CP_MARGIN_Y * 2 + CP_CELL_H + THEME_TITLE_HEIGHT + THEME_BORDER_WIDTH * 2;

    wm_set_pending_icon(cp_icon16);
    wm_set_pending_icon32(cp_icon32);

    hwnd_t hwnd = wm_create_window(
        60, 60, w, h,
        L(STR_CONTROL_PANEL), WSTYLE_DEFAULT,
        cp_event, cp_paint);

    if (hwnd == HWND_NULL) return HWND_NULL;

    window_t *win = wm_get_window(hwnd);
    if (win) win->bg_color = COLOR_WHITE;

    cp_state.hwnd = hwnd;
    wm_set_focus(hwnd);
    taskbar_invalidate();
    return hwnd;
}

const uint8_t *cp_get_icon16(void) { return cp_icon16; }
const uint8_t *cp_get_icon32(void) { return cp_icon32; }

/*==========================================================================
 * Common applet helpers
 *=========================================================================*/

#define APPLET_BTN_W    65
#define APPLET_BTN_H    22
#define APPLET_BTN_GAP   8

/* Simple button hit-test */
static bool btn_hit(int16_t mx, int16_t my,
                    int16_t bx, int16_t by, int16_t bw, int16_t bh) {
    return mx >= bx && mx < bx + bw && my >= by && my < by + bh;
}

/*==========================================================================
 * Desktop Properties
 *=========================================================================*/

typedef struct {
    hwnd_t       hwnd;
    uint8_t      color;      /* selected palette index */
    uint8_t      focus;      /* 0=colors, 1=theme, 2=OK, 3=Cancel */
    radiogroup_t theme_rg;
} disp_applet_t;

static disp_applet_t disp_app;

#define DISP_W       250
#define DISP_H       250
#define DISP_SWATCH_X  20
#define DISP_SWATCH_Y  30
#define DISP_SWATCH_SZ 20
#define DISP_PREVIEW_X  20
#define DISP_PREVIEW_Y 100
#define DISP_PREVIEW_W 200
#define DISP_PREVIEW_H  30
#define DISP_THEME_X    30
#define DISP_THEME_Y   155
#define DISP_THEME_H    18

static void disp_apply_ok(disp_applet_t *d) {
    settings_t *set = settings_get();
    set->desktop_color = d->color;
    set->theme_id = d->theme_rg.selected;
    desktop_set_bg_color(d->color);
    theme_set(d->theme_rg.selected);
    settings_save();
    wm_force_full_repaint();
}

static bool disp_event(hwnd_t hwnd, const window_event_t *ev) {
    disp_applet_t *d = &disp_app;
    int client_w = DISP_W - THEME_BORDER_WIDTH * 2;
    int client_h = DISP_H - THEME_TITLE_HEIGHT - THEME_BORDER_WIDTH * 2;
    int btn_y = client_h - APPLET_BTN_H - 8;
    int ok_x = client_w - APPLET_BTN_W * 2 - APPLET_BTN_GAP - 10;
    int cancel_x = ok_x + APPLET_BTN_W + APPLET_BTN_GAP;

    switch (ev->type) {
    case WM_CLOSE:
        wm_destroy_window(hwnd);
        memset(d, 0, sizeof(*d));
        return true;

    case WM_LBUTTONDOWN: {
        int16_t mx = ev->mouse.x, my = ev->mouse.y;
        /* Color swatch grid: 8x2 */
        if (my >= DISP_SWATCH_Y && my < DISP_SWATCH_Y + 2 * DISP_SWATCH_SZ &&
            mx >= DISP_SWATCH_X && mx < DISP_SWATCH_X + 8 * DISP_SWATCH_SZ) {
            int col = (mx - DISP_SWATCH_X) / DISP_SWATCH_SZ;
            int row = (my - DISP_SWATCH_Y) / DISP_SWATCH_SZ;
            d->color = row * 8 + col;
            if (d->color > 15) d->color = 15;
            wm_invalidate(hwnd);
        }
        /* Theme radio group */
        uint8_t new_sel;
        if (radiogroup_event(&d->theme_rg, ev, &new_sel)) {
            wm_invalidate(hwnd);
        }
        return true;
    }

    case WM_LBUTTONUP: {
        int16_t mx = ev->mouse.x, my = ev->mouse.y;
        if (btn_hit(mx, my, ok_x, btn_y, APPLET_BTN_W, APPLET_BTN_H)) {
            disp_apply_ok(d);
            wm_destroy_window(hwnd);
            memset(d, 0, sizeof(*d));
        } else if (btn_hit(mx, my, cancel_x, btn_y, APPLET_BTN_W, APPLET_BTN_H)) {
            wm_destroy_window(hwnd);
            memset(d, 0, sizeof(*d));
        }
        return true;
    }

    case WM_KEYDOWN:
        if (ev->key.scancode == 0x29) { /* Esc — cancel */
            wm_destroy_window(hwnd);
            memset(d, 0, sizeof(*d));
            return true;
        }
        if (ev->key.scancode == 0x2B) { /* Tab — cycle focus */
            d->focus = (d->focus + 1) % 4;
            wm_invalidate(hwnd);
            return true;
        }
        if (ev->key.scancode == 0x28) { /* Enter */
            if (d->focus == 3) { /* Cancel */
                wm_destroy_window(hwnd);
                memset(d, 0, sizeof(*d));
            } else { /* OK */
                disp_apply_ok(d);
                wm_destroy_window(hwnd);
                memset(d, 0, sizeof(*d));
            }
            return true;
        }
        /* Arrow keys — navigate color grid when focus=0 */
        if (d->focus == 0) {
            if (ev->key.scancode == 0x4F) { /* Right */
                d->color = (d->color + 1) & 0x0F;
                wm_invalidate(hwnd);
                return true;
            }
            if (ev->key.scancode == 0x50) { /* Left */
                d->color = (d->color + 15) & 0x0F;
                wm_invalidate(hwnd);
                return true;
            }
            if (ev->key.scancode == 0x51) { /* Down */
                d->color = (d->color + 8) & 0x0F;
                wm_invalidate(hwnd);
                return true;
            }
            if (ev->key.scancode == 0x52) { /* Up */
                d->color = (d->color + 8) & 0x0F;
                wm_invalidate(hwnd);
                return true;
            }
        }
        /* Arrow keys — navigate theme radios when focus=1 */
        if (d->focus == 1) {
            if (ev->key.scancode == 0x52 /* Up */) {
                if (d->theme_rg.selected > 0) d->theme_rg.selected--;
                wm_invalidate(hwnd);
                return true;
            }
            if (ev->key.scancode == 0x51 /* Down */) {
                if (d->theme_rg.selected < THEME_COUNT - 1)
                    d->theme_rg.selected++;
                wm_invalidate(hwnd);
                return true;
            }
        }
        break;

    default:
        break;
    }
    return false;
}

static void disp_paint(hwnd_t hwnd) {
    disp_applet_t *d = &disp_app;
    int client_w = DISP_W - THEME_BORDER_WIDTH * 2;
    int client_h = DISP_H - THEME_TITLE_HEIGHT - THEME_BORDER_WIDTH * 2;

    wd_begin(hwnd);
    wd_clear(THEME_BUTTON_FACE);

    wd_text_ui(20, 10, L(STR_BG_COLOR),
               COLOR_BLACK, THEME_BUTTON_FACE);

    /* 8x2 color swatch grid */
    for (int i = 0; i < 16; i++) {
        int col = i % 8;
        int row = i / 8;
        int sx = DISP_SWATCH_X + col * DISP_SWATCH_SZ;
        int sy = DISP_SWATCH_Y + row * DISP_SWATCH_SZ;
        wd_fill_rect(sx + 1, sy + 1, DISP_SWATCH_SZ - 2,
                     DISP_SWATCH_SZ - 2, (uint8_t)i);
        /* Highlight selected */
        if ((uint8_t)i == d->color) {
            wd_rect(sx, sy, DISP_SWATCH_SZ, DISP_SWATCH_SZ, COLOR_BLACK);
            wd_rect(sx + 1, sy + 1, DISP_SWATCH_SZ - 2,
                    DISP_SWATCH_SZ - 2, COLOR_WHITE);
        } else {
            wd_rect(sx, sy, DISP_SWATCH_SZ, DISP_SWATCH_SZ,
                    THEME_BUTTON_FACE);
        }
    }

    /* Dotted focus rect around color grid when focused */
    if (d->focus == 0) {
        wd_rect(DISP_SWATCH_X - 2, DISP_SWATCH_Y - 2,
                8 * DISP_SWATCH_SZ + 4, 2 * DISP_SWATCH_SZ + 4,
                COLOR_BLACK);
    }

    /* Preview */
    wd_text_ui(20, DISP_PREVIEW_Y - 14, L(STR_PREVIEW),
               COLOR_BLACK, THEME_BUTTON_FACE);
    wd_bevel_rect(DISP_PREVIEW_X, DISP_PREVIEW_Y,
                  DISP_PREVIEW_W, DISP_PREVIEW_H,
                  COLOR_DARK_GRAY, COLOR_WHITE, d->color);

    /* Theme selector */
    wd_text_ui(20, DISP_THEME_Y - 14, L(STR_WINDOW_THEME),
               COLOR_BLACK, THEME_BUTTON_FACE);
    radiogroup_paint(&d->theme_rg);
    if (d->focus == 1) {
        wd_rect(DISP_THEME_X - 4, DISP_THEME_Y - 2,
                160, THEME_COUNT * DISP_THEME_H + 4, COLOR_BLACK);
    }

    /* Buttons */
    int btn_y = client_h - APPLET_BTN_H - 8;
    int ok_x = client_w - APPLET_BTN_W * 2 - APPLET_BTN_GAP - 10;
    int cancel_x = ok_x + APPLET_BTN_W + APPLET_BTN_GAP;
    wd_button(ok_x, btn_y, APPLET_BTN_W, APPLET_BTN_H,
              L(STR_OK), d->focus == 2, false);
    wd_button(cancel_x, btn_y, APPLET_BTN_W, APPLET_BTN_H,
              L(STR_CANCEL), d->focus == 3, false);

    wd_end();
}

static void cp_open_display(void) {
    if (disp_app.hwnd) {
        wm_set_focus(disp_app.hwnd);
        return;
    }
    memset(&disp_app, 0, sizeof(disp_app));
    disp_app.color = settings_get()->desktop_color;

    /* Theme radio group */
    static const char *theme_labels[THEME_COUNT];
    for (int i = 0; i < THEME_COUNT; i++)
        theme_labels[i] = builtin_themes[i].name;
    radiogroup_init(&disp_app.theme_rg, DISP_THEME_X, DISP_THEME_Y,
                    THEME_COUNT, DISP_THEME_H);
    radiogroup_set_labels(&disp_app.theme_rg, theme_labels);
    disp_app.theme_rg.selected = theme_get_id();

    wm_set_pending_icon(cp_icon16);
    hwnd_t hwnd = wm_create_window(
        110, 70, DISP_W, DISP_H,
        L(STR_DESKTOP_PROPS), WSTYLE_DIALOG,
        disp_event, disp_paint);
    if (hwnd == HWND_NULL) return;
    window_t *win = wm_get_window(hwnd);
    if (win) win->bg_color = THEME_BUTTON_FACE;
    disp_app.hwnd = hwnd;
    wm_set_focus(hwnd);
    taskbar_invalidate();
}

/*==========================================================================
 * System Properties
 *=========================================================================*/

typedef struct {
    hwnd_t hwnd;
} sys_applet_t;

static sys_applet_t sys_app;

#define SYS_W  260
#define SYS_H  270

static bool sys_event(hwnd_t hwnd, const window_event_t *ev) {
    sys_applet_t *s = &sys_app;
    int client_w = SYS_W - THEME_BORDER_WIDTH * 2;
    int client_h = SYS_H - THEME_TITLE_HEIGHT - THEME_BORDER_WIDTH * 2;
    int btn_y = client_h - APPLET_BTN_H - 8;
    int ok_x = client_w - APPLET_BTN_W - 10;

    switch (ev->type) {
    case WM_CLOSE:
        wm_destroy_window(hwnd);
        memset(s, 0, sizeof(*s));
        return true;

    case WM_LBUTTONUP: {
        int16_t mx = ev->mouse.x, my = ev->mouse.y;
        if (btn_hit(mx, my, ok_x, btn_y, APPLET_BTN_W, APPLET_BTN_H)) {
            wm_destroy_window(hwnd);
            memset(s, 0, sizeof(*s));
        }
        return true;
    }

    case WM_KEYDOWN:
        if (ev->key.scancode == 0x29 || ev->key.scancode == 0x28) {
            wm_destroy_window(hwnd);
            memset(s, 0, sizeof(*s));
            return true;
        }
        break;

    default:
        break;
    }
    return false;
}

static void sys_paint(hwnd_t hwnd) {
    int client_w = SYS_W - THEME_BORDER_WIDTH * 2;
    int client_h = SYS_H - THEME_TITLE_HEIGHT - THEME_BORDER_WIDTH * 2;

    wd_begin(hwnd);
    wd_clear(THEME_BUTTON_FACE);

    char buf[64];
    int y = 12;
    int x = 16;

    wd_text_ui(x, y, "FRANK OS v" FRANK_VERSION_STR, COLOR_BLACK, THEME_BUTTON_FACE);
    y += FONT_UI_HEIGHT + 2;
    wd_text_ui(x, y, "(c) 2026 Mikhail Matveev", COLOR_DARK_GRAY, THEME_BUTTON_FACE);
    y += FONT_UI_HEIGHT + 2;
    wd_text_ui(x, y, "github.com/rh1tech/frank-os", COLOR_DARK_GRAY, THEME_BUTTON_FACE);
    y += FONT_UI_HEIGHT + 10;

    /* Separator line */
    wd_hline(x, y, client_w - x * 2, COLOR_DARK_GRAY);
    y += 8;

    uint32_t cpu_mhz = clock_get_hz(clk_sys) / 1000000;
    snprintf(buf, sizeof(buf), "CPU: %lu MHz", (unsigned long)cpu_mhz);
    wd_text_ui(x, y, buf, COLOR_BLACK, THEME_BUTTON_FACE);
    y += FONT_UI_HEIGHT + 4;

    uint32_t psram_kb = psram_detected_bytes() / 1024;
    if (psram_kb >= 1024)
        snprintf(buf, sizeof(buf), "PSRAM: %lu MB",
                 (unsigned long)(psram_kb / 1024));
    else
        snprintf(buf, sizeof(buf), "PSRAM: %lu KB",
                 (unsigned long)psram_kb);
    wd_text_ui(x, y, buf, COLOR_BLACK, THEME_BUTTON_FACE);
    y += FONT_UI_HEIGHT + 4;

    uint32_t ticks = xTaskGetTickCount();
    uint32_t secs = ticks / configTICK_RATE_HZ;
    uint32_t mins = secs / 60; secs %= 60;
    uint32_t hrs = mins / 60; mins %= 60;
    snprintf(buf, sizeof(buf), "Uptime: %luh %lum %lus",
             (unsigned long)hrs, (unsigned long)mins, (unsigned long)secs);
    wd_text_ui(x, y, buf, COLOR_BLACK, THEME_BUTTON_FACE);
    y += FONT_UI_HEIGHT + 4;

    snprintf(buf, sizeof(buf), "Kernel: FreeRTOS %s",
             tskKERNEL_VERSION_NUMBER);
    wd_text_ui(x, y, buf, COLOR_BLACK, THEME_BUTTON_FACE);

    /* OK button */
    int btn_y = client_h - APPLET_BTN_H - 8;
    int ok_x = client_w - APPLET_BTN_W - 10;
    wd_button(ok_x, btn_y, APPLET_BTN_W, APPLET_BTN_H, L(STR_OK), true, false);

    wd_end();
}

static void cp_open_system(void) {
    if (sys_app.hwnd) {
        wm_set_focus(sys_app.hwnd);
        return;
    }
    memset(&sys_app, 0, sizeof(sys_app));

    wm_set_pending_icon(cp_icon16);
    hwnd_t hwnd = wm_create_window(
        120, 60, SYS_W, SYS_H,
        L(STR_SYSTEM_PROPS), WSTYLE_DIALOG,
        sys_event, sys_paint);
    if (hwnd == HWND_NULL) return;
    window_t *win = wm_get_window(hwnd);
    if (win) win->bg_color = THEME_BUTTON_FACE;
    sys_app.hwnd = hwnd;
    wm_set_focus(hwnd);
    taskbar_invalidate();
}

/*==========================================================================
 * Mouse Properties
 *=========================================================================*/

typedef struct {
    hwnd_t   hwnd;
    slider_t slider;
    /* Test area */
    int8_t   test_clicks;
    uint32_t test_tick;
    uint8_t  focus;         /* 0=OK, 1=Cancel */
} mouse_applet_t;

static mouse_applet_t mouse_app;

#define MOUSE_W       260
#define MOUSE_H       225
#define MOUSE_TRACK_X  20
#define MOUSE_TRACK_Y  45
#define MOUSE_TRACK_W 210
#define MOUSE_TRACK_H  16
#define MOUSE_TEST_X   20
#define MOUSE_TEST_Y   85
#define MOUSE_TEST_W   80
#define MOUSE_TEST_H   50

/* Convert slider value (0-600) to ms (800-200).
 * Slider value 0 = 800ms (slow, left), 600 = 200ms (fast, right). */
static uint16_t mouse_slider_to_ms(int32_t val) {
    int ms = 800 - (int)val;
    ms = ((ms + 50) / 100) * 100;
    if (ms < 200) ms = 200;
    if (ms > 800) ms = 800;
    return (uint16_t)ms;
}

static int32_t mouse_ms_to_slider(uint16_t ms) {
    return (int32_t)(800 - ms);
}

static bool mouse_event(hwnd_t hwnd, const window_event_t *ev) {
    mouse_applet_t *m = &mouse_app;
    int client_w = MOUSE_W - THEME_BORDER_WIDTH * 2;
    int client_h = MOUSE_H - THEME_TITLE_HEIGHT - THEME_BORDER_WIDTH * 2;
    int btn_y = client_h - APPLET_BTN_H - 8;
    int ok_x = client_w - APPLET_BTN_W * 2 - APPLET_BTN_GAP - 10;
    int cancel_x = ok_x + APPLET_BTN_W + APPLET_BTN_GAP;

    switch (ev->type) {
    case WM_CLOSE:
        wm_destroy_window(hwnd);
        memset(m, 0, sizeof(*m));
        return true;

    case WM_LBUTTONDOWN:
    case WM_MOUSEMOVE: {
        int32_t new_val;
        if (slider_event(&m->slider, ev, &new_val)) {
            wm_invalidate(hwnd);
        }
        if (ev->type == WM_LBUTTONDOWN) return true;
        return true;
    }

    case WM_LBUTTONUP: {
        slider_event(&m->slider, ev, NULL);
        int16_t mx = ev->mouse.x, my = ev->mouse.y;

        /* Test area double-click */
        if (mx >= MOUSE_TEST_X && mx < MOUSE_TEST_X + MOUSE_TEST_W &&
            my >= MOUSE_TEST_Y && my < MOUSE_TEST_Y + MOUSE_TEST_H) {
            uint32_t now = xTaskGetTickCount();
            if (m->test_clicks > 0 &&
                (now - m->test_tick) < pdMS_TO_TICKS(mouse_slider_to_ms(m->slider.value))) {
                m->test_clicks = 2;
                wm_invalidate(hwnd);
            } else {
                m->test_clicks = 1;
            }
            m->test_tick = now;
            return true;
        }

        /* Buttons */
        if (btn_hit(mx, my, ok_x, btn_y, APPLET_BTN_W, APPLET_BTN_H)) {
            settings_t *set = settings_get();
            set->dblclick_ms = mouse_slider_to_ms(m->slider.value);
            wm_set_dblclick_speed(mouse_slider_to_ms(m->slider.value));
            settings_save();
            wm_destroy_window(hwnd);
            memset(m, 0, sizeof(*m));
        } else if (btn_hit(mx, my, cancel_x, btn_y,
                           APPLET_BTN_W, APPLET_BTN_H)) {
            wm_destroy_window(hwnd);
            memset(m, 0, sizeof(*m));
        }
        return true;
    }

    case WM_KEYDOWN:
        if (ev->key.scancode == 0x29) { /* Esc — cancel */
            wm_destroy_window(hwnd);
            memset(m, 0, sizeof(*m));
            return true;
        }
        if (ev->key.scancode == 0x2B) { /* Tab — toggle OK/Cancel */
            m->focus = 1 - m->focus;
            wm_invalidate(hwnd);
            return true;
        }
        if (ev->key.scancode == 0x28) { /* Enter */
            if (m->focus == 1) { /* Cancel */
                wm_destroy_window(hwnd);
                memset(m, 0, sizeof(*m));
            } else { /* OK */
                settings_t *set = settings_get();
                set->dblclick_ms = mouse_slider_to_ms(m->slider.value);
                wm_set_dblclick_speed(mouse_slider_to_ms(m->slider.value));
                settings_save();
                wm_destroy_window(hwnd);
                memset(m, 0, sizeof(*m));
            }
            return true;
        }
        break;

    default:
        break;
    }
    return false;
}

static void mouse_paint(hwnd_t hwnd) {
    mouse_applet_t *m = &mouse_app;
    int client_w = MOUSE_W - THEME_BORDER_WIDTH * 2;
    int client_h = MOUSE_H - THEME_TITLE_HEIGHT - THEME_BORDER_WIDTH * 2;

    wd_begin(hwnd);
    wd_clear(THEME_BUTTON_FACE);

    /* Speed label */
    char buf[48];
    snprintf(buf, sizeof(buf), "%s %u ms",
             L(STR_DBLCLICK_SPEED), mouse_slider_to_ms(m->slider.value));
    wd_text_ui(20, 10, buf, COLOR_BLACK, THEME_BUTTON_FACE);

    /* Labels */
    wd_text_ui(MOUSE_TRACK_X, MOUSE_TRACK_Y - 14, L(STR_SLOW),
               COLOR_BLACK, THEME_BUTTON_FACE);
    wd_text_ui(MOUSE_TRACK_X + MOUSE_TRACK_W - gfx_utf8_charcount(L(STR_FAST)) * FONT_UI_WIDTH,
               MOUSE_TRACK_Y - 14, L(STR_FAST),
               COLOR_BLACK, THEME_BUTTON_FACE);

    /* Slider (track + thumb) */
    slider_paint(&m->slider);

    /* Test area */
    wd_text_ui(MOUSE_TEST_X, MOUSE_TEST_Y - 14, L(STR_TEST_AREA),
               COLOR_BLACK, THEME_BUTTON_FACE);
    wd_bevel_rect(MOUSE_TEST_X, MOUSE_TEST_Y,
                  MOUSE_TEST_W, MOUSE_TEST_H,
                  COLOR_DARK_GRAY, COLOR_WHITE, COLOR_WHITE);

    /* Draw folder icon in test area */
    wd_icon_32(MOUSE_TEST_X + (MOUSE_TEST_W - 32) / 2,
               MOUSE_TEST_Y + 2, fn_icon32_folder);

    /* Show "OK!" on successful double-click */
    if (m->test_clicks >= 2) {
        wd_text_ui(MOUSE_TEST_X + MOUSE_TEST_W + 8,
                   MOUSE_TEST_Y + (MOUSE_TEST_H - FONT_UI_HEIGHT) / 2,
                   "OK!", COLOR_GREEN, THEME_BUTTON_FACE);
    }

    /* Buttons */
    int btn_y = client_h - APPLET_BTN_H - 8;
    int ok_x = client_w - APPLET_BTN_W * 2 - APPLET_BTN_GAP - 10;
    int cancel_x = ok_x + APPLET_BTN_W + APPLET_BTN_GAP;
    wd_button(ok_x, btn_y, APPLET_BTN_W, APPLET_BTN_H,
              L(STR_OK), m->focus == 0, false);
    wd_button(cancel_x, btn_y, APPLET_BTN_W, APPLET_BTN_H,
              L(STR_CANCEL), m->focus == 1, false);

    wd_end();
}

static void cp_open_mouse(void) {
    if (mouse_app.hwnd) {
        wm_set_focus(mouse_app.hwnd);
        return;
    }
    memset(&mouse_app, 0, sizeof(mouse_app));
    /* Initialize slider: 0=slow(800ms) on left, 600=fast(200ms) on right */
    slider_init(&mouse_app.slider, true);
    slider_set_range(&mouse_app.slider, 0, 600, 100);
    slider_set_rect(&mouse_app.slider, MOUSE_TRACK_X, MOUSE_TRACK_Y,
                    MOUSE_TRACK_W, MOUSE_TRACK_H);
    mouse_app.slider.value = mouse_ms_to_slider(settings_get()->dblclick_ms);

    wm_set_pending_icon(cp_icon16);
    hwnd_t hwnd = wm_create_window(
        90, 70, MOUSE_W, MOUSE_H,
        L(STR_MOUSE_PROPS), WSTYLE_DIALOG,
        mouse_event, mouse_paint);
    if (hwnd == HWND_NULL) return;
    window_t *win = wm_get_window(hwnd);
    if (win) win->bg_color = THEME_BUTTON_FACE;
    mouse_app.hwnd = hwnd;
    wm_set_focus(hwnd);
    taskbar_invalidate();
}

/*==========================================================================
 * Frequencies
 *=========================================================================*/

typedef struct {
    hwnd_t       hwnd;
    radiogroup_t cpu_rg;
    radiogroup_t psram_rg;
    uint8_t      focus;      /* 0=CPU, 1=PSRAM, 2=OK, 3=Cancel */
} freq_applet_t;

static freq_applet_t freq_app;

#define FREQ_W  260
#define FREQ_H  270

static const uint16_t cpu_freqs[] = { 252, 378, 504 };
static const uint16_t psram_freqs[] = { 0, 133, 166 };
static const char *cpu_labels[] = { "252 MHz", "378 MHz", "504 MHz" };
static const char *psram_labels[] = { "Default", "133 MHz", "166 MHz" };

#define FREQ_RADIO_X  30
#define FREQ_CPU_Y    30
#define FREQ_PSRAM_Y 115
#define FREQ_RADIO_H  18
#define FREQ_RADIO_R   6

static uint8_t freq_cpu_idx(uint16_t mhz) {
    for (int i = 0; i < 3; i++)
        if (cpu_freqs[i] == mhz) return i;
    return 2; /* default to 504 */
}

static uint8_t freq_psram_idx(uint16_t mhz) {
    for (int i = 0; i < 3; i++)
        if (psram_freqs[i] == mhz) return i;
    return 0;
}

static bool freq_event(hwnd_t hwnd, const window_event_t *ev) {
    freq_applet_t *f = &freq_app;
    int client_w = FREQ_W - THEME_BORDER_WIDTH * 2;
    int client_h = FREQ_H - THEME_TITLE_HEIGHT - THEME_BORDER_WIDTH * 2;
    int btn_y = client_h - APPLET_BTN_H - 8;
    int ok_x = client_w - APPLET_BTN_W * 2 - APPLET_BTN_GAP - 10;
    int cancel_x = ok_x + APPLET_BTN_W + APPLET_BTN_GAP;

    switch (ev->type) {
    case WM_CLOSE:
        wm_destroy_window(hwnd);
        memset(f, 0, sizeof(*f));
        return true;

    case WM_LBUTTONDOWN: {
        uint8_t new_sel;
        if (radiogroup_event(&f->cpu_rg, ev, &new_sel)) {
            wm_invalidate(hwnd);
            return true;
        }
        if (radiogroup_event(&f->psram_rg, ev, &new_sel)) {
            wm_invalidate(hwnd);
            return true;
        }
        return true;
    }

    case WM_LBUTTONUP: {
        int16_t mx = ev->mouse.x, my = ev->mouse.y;
        if (btn_hit(mx, my, ok_x, btn_y, APPLET_BTN_W, APPLET_BTN_H)) {
            settings_t *set = settings_get();
            set->cpu_freq_mhz = cpu_freqs[f->cpu_rg.selected];
            set->psram_freq_mhz = psram_freqs[f->psram_rg.selected];
            settings_save();
            /* Show info dialog */
            dialog_show(hwnd, L(STR_FREQUENCIES),
                L(STR_REBOOT_NOTICE),
                DLG_ICON_INFO, DLG_BTN_OK);
            wm_destroy_window(hwnd);
            memset(f, 0, sizeof(*f));
        } else if (btn_hit(mx, my, cancel_x, btn_y,
                           APPLET_BTN_W, APPLET_BTN_H)) {
            wm_destroy_window(hwnd);
            memset(f, 0, sizeof(*f));
        }
        return true;
    }

    case WM_KEYDOWN:
        if (ev->key.scancode == 0x29) { /* Esc — cancel */
            wm_destroy_window(hwnd);
            memset(f, 0, sizeof(*f));
            return true;
        }
        if (ev->key.scancode == 0x2B) { /* Tab — cycle focus */
            f->focus = (f->focus + 1) % 4;
            wm_invalidate(hwnd);
            return true;
        }
        if (ev->key.scancode == 0x28) { /* Enter */
            if (f->focus == 3) { /* Cancel */
                wm_destroy_window(hwnd);
                memset(f, 0, sizeof(*f));
            } else { /* OK (focus 0, 1, or 2) */
                settings_t *set = settings_get();
                set->cpu_freq_mhz = cpu_freqs[f->cpu_rg.selected];
                set->psram_freq_mhz = psram_freqs[f->psram_rg.selected];
                settings_save();
                dialog_show(HWND_NULL, L(STR_FREQUENCIES),
                    L(STR_REBOOT_NOTICE),
                    DLG_ICON_INFO, DLG_BTN_OK);
                wm_destroy_window(hwnd);
                memset(f, 0, sizeof(*f));
            }
            return true;
        }
        /* Up/Down arrows — change radio selection when focused on a group */
        if (f->focus == 0) { /* CPU group */
            if (ev->key.scancode == 0x52 /* Up */) {
                if (f->cpu_rg.selected > 0) f->cpu_rg.selected--;
                wm_invalidate(hwnd);
                return true;
            }
            if (ev->key.scancode == 0x51 /* Down */) {
                if (f->cpu_rg.selected < 2) f->cpu_rg.selected++;
                wm_invalidate(hwnd);
                return true;
            }
        }
        if (f->focus == 1) { /* PSRAM group */
            if (ev->key.scancode == 0x52 /* Up */) {
                if (f->psram_rg.selected > 0) f->psram_rg.selected--;
                wm_invalidate(hwnd);
                return true;
            }
            if (ev->key.scancode == 0x51 /* Down */) {
                if (f->psram_rg.selected < 2) f->psram_rg.selected++;
                wm_invalidate(hwnd);
                return true;
            }
        }
        break;

    default:
        break;
    }
    return false;
}

/* Radio button drawing now uses controls.h radiogroup_t */

static void freq_paint(hwnd_t hwnd) {
    freq_applet_t *f = &freq_app;
    int client_w = FREQ_W - THEME_BORDER_WIDTH * 2;
    int client_h = FREQ_H - THEME_TITLE_HEIGHT - THEME_BORDER_WIDTH * 2;

    wd_begin(hwnd);
    wd_clear(THEME_BUTTON_FACE);

    /* CPU frequency group */
    wd_text_ui(16, 14, L(STR_CPU_FREQ), COLOR_BLACK, THEME_BUTTON_FACE);
    radiogroup_paint(&f->cpu_rg);
    if (f->focus == 0) {
        wd_rect(FREQ_RADIO_X - 4, FREQ_CPU_Y - 2,
                130, 3 * FREQ_RADIO_H + 4, COLOR_BLACK);
    }

    /* PSRAM frequency group */
    wd_text_ui(16, FREQ_PSRAM_Y - 16, L(STR_PSRAM_FREQ),
               COLOR_BLACK, THEME_BUTTON_FACE);
    radiogroup_paint(&f->psram_rg);
    if (f->focus == 1) {
        wd_rect(FREQ_RADIO_X - 4, FREQ_PSRAM_Y - 2,
                130, 3 * FREQ_RADIO_H + 4, COLOR_BLACK);
    }

    /* Reboot notice */
    wd_text_ui(16, FREQ_PSRAM_Y + 3 * FREQ_RADIO_H + 4,
               L(STR_REBOOT_NOTICE),
               COLOR_DARK_GRAY, THEME_BUTTON_FACE);

    /* Buttons */
    int btn_y = client_h - APPLET_BTN_H - 8;
    int ok_x = client_w - APPLET_BTN_W * 2 - APPLET_BTN_GAP - 10;
    int cancel_x = ok_x + APPLET_BTN_W + APPLET_BTN_GAP;
    wd_button(ok_x, btn_y, APPLET_BTN_W, APPLET_BTN_H,
              L(STR_OK), f->focus == 2, false);
    wd_button(cancel_x, btn_y, APPLET_BTN_W, APPLET_BTN_H,
              L(STR_CANCEL), f->focus == 3, false);

    wd_end();
}

static void cp_open_freq(void) {
    if (freq_app.hwnd) {
        wm_set_focus(freq_app.hwnd);
        return;
    }
    memset(&freq_app, 0, sizeof(freq_app));

    /* CPU radio group */
    radiogroup_init(&freq_app.cpu_rg, FREQ_RADIO_X, FREQ_CPU_Y,
                    3, FREQ_RADIO_H);
    radiogroup_set_labels(&freq_app.cpu_rg, cpu_labels);
    freq_app.cpu_rg.selected = freq_cpu_idx(settings_get()->cpu_freq_mhz);

    /* PSRAM radio group */
    radiogroup_init(&freq_app.psram_rg, FREQ_RADIO_X, FREQ_PSRAM_Y,
                    3, FREQ_RADIO_H);
    radiogroup_set_labels(&freq_app.psram_rg, psram_labels);
    freq_app.psram_rg.selected = freq_psram_idx(settings_get()->psram_freq_mhz);

    wm_set_pending_icon(cp_icon16);
    hwnd_t hwnd = wm_create_window(
        80, 50, FREQ_W, FREQ_H,
        L(STR_FREQUENCIES), WSTYLE_DIALOG,
        freq_event, freq_paint);
    if (hwnd == HWND_NULL) return;
    window_t *win = wm_get_window(hwnd);
    if (win) win->bg_color = THEME_BUTTON_FACE;
    freq_app.hwnd = hwnd;
    wm_set_focus(hwnd);
    taskbar_invalidate();
}

/*==========================================================================
 * Language applet
 *=========================================================================*/

static struct {
    hwnd_t hwnd;
    uint8_t selected;  /* 0=English, 1=Russian */
    uint8_t toggle;    /* 0=Alt+Shift, 1=Ctrl+Shift, 2=Alt+Space */
} lang_app;

#define LA_BTN_Y  148

static void lang_apply(void) {
    lang_set(lang_app.selected);
    settings_get()->input_toggle = lang_app.toggle;
    settings_save();
    wm_destroy_window(lang_app.hwnd);
    taskbar_invalidate();
    wm_force_full_repaint();
}

static bool lang_event(hwnd_t hwnd, const window_event_t *ev) {
    if (ev->type == WM_CLOSE) {
        wm_destroy_window(hwnd);
        taskbar_invalidate();
        return true;
    }
    if (ev->type == WM_COMMAND) {
        if (ev->command.id == DLG_RESULT_OK ||
            ev->command.id == DLG_RESULT_CANCEL) {
            wm_invalidate(hwnd);
            return true;
        }
    }
    if (ev->type == WM_LBUTTONDOWN) {
        int16_t mx = ev->mouse.x;
        int16_t my = ev->mouse.y;

        /* Language radio buttons */
        if (my >= 28 && my < 44) { lang_app.selected = LANG_EN; wm_invalidate(hwnd); return true; }
        if (my >= 46 && my < 62) { lang_app.selected = LANG_RU; wm_invalidate(hwnd); return true; }

        /* Toggle radio buttons */
        if (my >= 84 && my < 100)  { lang_app.toggle = 0; wm_invalidate(hwnd); return true; }
        if (my >= 102 && my < 118) { lang_app.toggle = 1; wm_invalidate(hwnd); return true; }
        if (my >= 120 && my < 136) { lang_app.toggle = 2; wm_invalidate(hwnd); return true; }

        /* OK button */
        if (mx >= 30 && mx < 90 && my >= LA_BTN_Y && my < LA_BTN_Y + 22) {
            lang_apply();
            return true;
        }
        /* Cancel button */
        if (mx >= 100 && mx < 160 && my >= LA_BTN_Y && my < LA_BTN_Y + 22) {
            wm_destroy_window(hwnd);
            taskbar_invalidate();
            return true;
        }
    }
    return false;
}

static void lang_paint(hwnd_t hwnd) {
    (void)hwnd;
    wd_begin(hwnd);
    wd_clear(THEME_BUTTON_FACE);

    /* Language label + radios */
    wd_text_ui(12, 10, L(STR_SELECT_LANGUAGE), COLOR_BLACK, THEME_BUTTON_FACE);
    wd_radio(12, 30, "English", lang_app.selected == LANG_EN);
    wd_radio(12, 48, "\xD0\xF3\xF1\xF1\xEA\xE8\xE9", lang_app.selected == LANG_RU);

    /* Input toggle label + radios */
    wd_text_ui(12, 68, L(STR_INPUT_TOGGLE), COLOR_BLACK, THEME_BUTTON_FACE);
    wd_radio(12, 86, "Alt+Shift", lang_app.toggle == 0);
    wd_radio(12, 104, "Ctrl+Shift", lang_app.toggle == 1);
    wd_radio(12, 122, "Alt+Space", lang_app.toggle == 2);

    /* OK / Cancel buttons */
    wd_button(30, LA_BTN_Y, 60, 22, L(STR_OK), false, false);
    wd_button(100, LA_BTN_Y, 60, 22, L(STR_CANCEL), false, false);

    wd_end();
}

static void cp_open_lang(void) {
    lang_app.selected = lang_get();
    lang_app.toggle = settings_get()->input_toggle;

    int16_t w = 200 + 2 * THEME_BORDER_WIDTH;
    int16_t h = 180 + THEME_TITLE_HEIGHT + 2 * THEME_BORDER_WIDTH;
    hwnd_t hwnd = wm_create_window(
        180, 120, w, h, L(STR_LANGUAGE_PROPS),
        WSTYLE_DIALOG, lang_event, lang_paint);
    if (hwnd == HWND_NULL) return;
    window_t *win = wm_get_window(hwnd);
    if (win) win->bg_color = THEME_BUTTON_FACE;
    lang_app.hwnd = hwnd;
    wm_set_focus(hwnd);
    taskbar_invalidate();
}

