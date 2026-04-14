/*
 * NetTools — Network Diagnostic Utility for FRANK OS
 * Copyright (c) 2026 Mikhail Matveev <xtreme@rh1.tech>
 * https://rh1.tech
 *
 * Two tabs: Ping (ICMP via AT+PING) and DNS (resolve hostname).
 * Uses netcard API via sys_table.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "m-os-api.h"
#include "frankos-app.h"
#include "lang.h"
#include <string.h>

/* App-local translations */
enum { AL_PING, AL_DNS, AL_HOST, AL_COUNT };
static const char *al_en[] = { [AL_PING] = "Ping", [AL_DNS] = "DNS", [AL_HOST] = "Host:" };
static const char *al_ru[] = {
    [AL_PING] = "\xD0\x9F\xD0\xB8\xD0\xBD\xD0\xB3",
    [AL_DNS]  = "DNS",
    [AL_HOST] = "\xD0\xA5\xD0\xBE\xD1\x81\xD1\x82:",
};
static const char *AL(int id) { return lang_get() == LANG_RU ? al_ru[id] : al_en[id]; }

/* ========================================================================
 * sys_table wrappers for network + debug API
 * ======================================================================== */

static inline bool netcard_wifi_connected(void) {
    typedef bool (*fn_t)(void);
    return ((fn_t)_sys_table_ptrs[538])();
}

static inline bool netcard_resolve(const char *hostname, char *ip_out, int ip_out_size) {
    typedef bool (*fn_t)(const char *, char *, int);
    return ((fn_t)_sys_table_ptrs[549])(hostname, ip_out, ip_out_size);
}

static inline int netcard_ping(const char *host, uint16_t port) {
    typedef int (*fn_t)(const char *, uint16_t);
    return ((fn_t)_sys_table_ptrs[550])(host, port);
}

#define dbg_printf(...) ((int(*)(const char*,...))_sys_table_ptrs[438])(__VA_ARGS__)

/* ========================================================================
 * Constants
 * ======================================================================== */

#define TAB_PING   0
#define TAB_DNS    1

#define CLIENT_W   310
#define CLIENT_H   236

#define TAB_TOP     8           /* same padding as bottom margin */
#define TAB_H       22
#define TAB_W       60
#define LABEL_Y     (TAB_TOP + TAB_H + 8)
#define INPUT_Y     (LABEL_Y + FONT_UI_HEIGHT + 2)
#define INPUT_H     18
#define INPUT_X     8
#define INPUT_W     (CLIENT_W - 16)

#define BTN_W       70
#define BTN_H       22
#define BTN_Y       (INPUT_Y + INPUT_H + 6)

#define RESULT_Y    (BTN_Y + BTN_H + 6)
#define RESULT_H    (CLIENT_H - RESULT_Y - 8)
#define RESULT_X    8
#define RESULT_W    (CLIENT_W - 16)

#define INPUT_BUF_SIZE   128
#define RESULT_BUF_SIZE  512

/* ========================================================================
 * Application state
 * ======================================================================== */

typedef struct {
    hwnd_t      hwnd;
    uint8_t     tab;
    textarea_t  input_ta;
    char        input_buf[INPUT_BUF_SIZE];
    textarea_t  result_ta;
    char        result_buf[RESULT_BUF_SIZE];
    bool        busy;
    bool        closing;
    int8_t      btn_pressed;     /* -1=none, 0=pressed */
    uint8_t     wait_dots;       /* 0-3 for animation */
    TimerHandle_t blink_timer;
} app_t;

static app_t app;
static TaskHandle_t app_task;

/* ========================================================================
 * Blink timer callback — cursor blink + wait dots animation
 * ======================================================================== */

static void blink_cb(TimerHandle_t t) {
    (void)t;
    app.input_ta.cursor_visible = !app.input_ta.cursor_visible;
    app.result_ta.cursor_visible = false;  /* always hidden */
    if (app.busy)
        app.wait_dots = (app.wait_dots + 1) % 4;
    wm_invalidate(app.hwnd);
}

/* ========================================================================
 * Drawing helpers
 * ======================================================================== */

static void draw_tab(int16_t x, int16_t y, int16_t w, int16_t h,
                     const char *label, bool active) {
    if (active) {
        wd_fill_rect(x, y, w, h + 2, THEME_BUTTON_FACE);
        wd_hline(x, y, w, COLOR_WHITE);
        wd_vline(x, y, h + 2, COLOR_WHITE);
        wd_vline(x + w - 1, y, h + 1, COLOR_DARK_GRAY);
        wd_vline(x + w - 2, y + 1, h, COLOR_BLACK);
    } else {
        wd_fill_rect(x, y + 2, w, h - 2, THEME_BUTTON_FACE);
        wd_hline(x, y + 2, w, COLOR_WHITE);
        wd_vline(x, y + 2, h - 2, COLOR_WHITE);
        wd_vline(x + w - 1, y + 2, h - 2, COLOR_DARK_GRAY);
    }
    int tw = (int)strlen(label) * FONT_UI_WIDTH;
    int tx = x + (w - tw) / 2;
    int ty = y + (active ? (h - FONT_UI_HEIGHT) / 2 : (h - FONT_UI_HEIGHT) / 2 + 2);
    wd_text_ui(tx, ty, label, COLOR_BLACK, THEME_BUTTON_FACE);
}

/* ========================================================================
 * Paint handler
 * ======================================================================== */

static void app_paint(hwnd_t hwnd) {
    (void)hwnd;
    wd_begin(hwnd);
    wd_clear(THEME_BUTTON_FACE);

    /* Tabs */
    draw_tab(4, TAB_TOP, TAB_W, TAB_H, AL(AL_PING), app.tab == TAB_PING);
    draw_tab(4 + TAB_W + 2, TAB_TOP, TAB_W, TAB_H, AL(AL_DNS), app.tab == TAB_DNS);

    /* Tab content border */
    wd_hline(0, TAB_TOP + TAB_H, CLIENT_W, COLOR_WHITE);

    /* Label */
    wd_text_ui(INPUT_X, LABEL_Y, AL(AL_HOST), COLOR_BLACK, THEME_BUTTON_FACE);

    /* Input field — sunken border + textarea */
    wd_bevel_rect(INPUT_X - 2, INPUT_Y - 2, INPUT_W + 4, INPUT_H + 4,
                  COLOR_DARK_GRAY, COLOR_WHITE, COLOR_WHITE);
    textarea_paint(&app.input_ta);

    /* Button */
    {
        const char *btn_label = app.tab == TAB_PING ? AL(AL_PING) : AL(AL_DNS);
        bool enabled = !app.busy && textarea_get_length(&app.input_ta) > 0;
        bool pressed = app.btn_pressed == 0;
        wd_button(INPUT_X, BTN_Y, BTN_W, BTN_H, btn_label, false, pressed);
        if (!enabled && !pressed) {
            int tw = (int)strlen(btn_label) * FONT_UI_WIDTH;
            int tx = INPUT_X + (BTN_W - tw) / 2;
            int ty = BTN_Y + (BTN_H - FONT_UI_HEIGHT) / 2;
            wd_text_ui(tx, ty, btn_label, COLOR_DARK_GRAY, THEME_BUTTON_FACE);
        }
    }

    /* Status label with animated dots */
    if (app.busy) {
        const char *dots = &"..."[3 - app.wait_dots];
        char wait_text[24];
        snprintf(wait_text, sizeof(wait_text), "Please wait%s", dots);
        wd_text_ui(INPUT_X + BTN_W + 8, BTN_Y + (BTN_H - FONT_UI_HEIGHT) / 2,
                   wait_text, COLOR_DARK_GRAY, THEME_BUTTON_FACE);
    }

    /* Result area — sunken border + textarea */
    wd_bevel_rect(RESULT_X - 2, RESULT_Y - 2, RESULT_W + 4, RESULT_H + 4,
                  COLOR_DARK_GRAY, COLOR_WHITE, COLOR_WHITE);
    textarea_paint(&app.result_ta);

    wd_end();
}

/* ========================================================================
 * Operations (run on app task, blocking is OK)
 * ======================================================================== */

static void do_ping(void) {
    const char *host = textarea_get_text(&app.input_ta);
    app.busy = true;
    app.wait_dots = 0;
    textarea_set_text(&app.result_ta, "", 0);
    wm_invalidate(app.hwnd);

    char buf[RESULT_BUF_SIZE];
    buf[0] = '\0';
    int offset = 0;
    int i;

    for (i = 0; i < 4; i++) {
        int ms = netcard_ping(host, 80);
        if (ms >= 0) {
            offset += snprintf(buf + offset, sizeof(buf) - offset,
                               "Reply from %s: time=%dms\n", host, ms);
        } else {
            offset += snprintf(buf + offset, sizeof(buf) - offset,
                               "Request to %s timed out\n", host);
        }
        textarea_set_text(&app.result_ta, buf, offset);
        wm_invalidate(app.hwnd);

        if (i < 3)
            vTaskDelay(pdMS_TO_TICKS(500));
    }

    app.busy = false;
    wm_invalidate(app.hwnd);
}

static void do_resolve(void) {
    const char *host = textarea_get_text(&app.input_ta);
    app.busy = true;
    app.wait_dots = 0;
    textarea_set_text(&app.result_ta, "", 0);
    wm_invalidate(app.hwnd);

    char ip[16];
    char result[128];
    if (netcard_resolve(host, ip, sizeof(ip))) {
        snprintf(result, sizeof(result), "%s -> %s", host, ip);
    } else {
        snprintf(result, sizeof(result), "DNS lookup failed for %s", host);
    }
    textarea_set_text(&app.result_ta, result, strlen(result));

    app.busy = false;
    wm_invalidate(app.hwnd);
}

/* ========================================================================
 * Event handler
 * ======================================================================== */

static bool app_event(hwnd_t hwnd, const window_event_t *ev) {
    if (ev->type == WM_CLOSE) {
        app.closing = true;
        xTaskNotifyGive(app_task);
        return true;
    }

    if (ev->type == WM_SETFOCUS) {
        if (app.blink_timer) xTimerStart(app.blink_timer, 0);
        return true;
    }
    if (ev->type == WM_KILLFOCUS) {
        if (app.blink_timer) xTimerStop(app.blink_timer, 0);
        app.input_ta.cursor_visible = false;
        wm_invalidate(hwnd);
        return true;
    }

    if (ev->type == WM_LBUTTONDOWN) {
        int mx = ev->mouse.x, my = ev->mouse.y;

        /* Button press animation */
        if (!app.busy && textarea_get_length(&app.input_ta) > 0 &&
            mx >= INPUT_X && mx < INPUT_X + BTN_W &&
            my >= BTN_Y && my < BTN_Y + BTN_H) {
            app.btn_pressed = 0;
            wm_invalidate(hwnd);
            return true;
        }

        /* Tab clicks (disabled when busy) */
        if (!app.busy && my >= TAB_TOP && my < TAB_TOP + TAB_H) {
            if (mx >= 4 && mx < 4 + TAB_W && app.tab != TAB_PING) {
                app.tab = TAB_PING;
                textarea_set_text(&app.input_ta, "", 0);
                textarea_set_text(&app.result_ta, "", 0);
                wm_invalidate(hwnd);
                return true;
            }
            if (mx >= 4 + TAB_W + 2 && mx < 4 + 2 * TAB_W + 2 && app.tab != TAB_DNS) {
                app.tab = TAB_DNS;
                textarea_set_text(&app.input_ta, "", 0);
                textarea_set_text(&app.result_ta, "", 0);
                wm_invalidate(hwnd);
                return true;
            }
        }

        /* Let input textarea handle clicks (only when not busy) */
        if (!app.busy && textarea_event(&app.input_ta, ev)) {
            wm_invalidate(hwnd);
            return true;
        }
        return true;
    }

    if (ev->type == WM_LBUTTONUP) {
        int mx = ev->mouse.x, my = ev->mouse.y;
        bool was_pressed = (app.btn_pressed == 0);
        app.btn_pressed = -1;

        if (was_pressed && !app.busy &&
            mx >= INPUT_X && mx < INPUT_X + BTN_W &&
            my >= BTN_Y && my < BTN_Y + BTN_H &&
            textarea_get_length(&app.input_ta) > 0) {
            xTaskNotifyGive(app_task);
        }
        wm_invalidate(hwnd);

        if (!app.busy && textarea_event(&app.input_ta, ev))
            wm_invalidate(hwnd);
        return true;
    }

    if (ev->type == WM_MOUSEMOVE) {
        if (!app.busy && textarea_event(&app.input_ta, ev))
            wm_invalidate(hwnd);
        return true;
    }

    if (ev->type == WM_KEYDOWN) {
        uint8_t sc = ev->key.scancode;

        /* Enter — trigger operation */
        if (sc == 0x28 && !app.busy && textarea_get_length(&app.input_ta) > 0) {
            xTaskNotifyGive(app_task);
            return true;
        }

        /* Forward to input textarea when not busy */
        if (!app.busy && textarea_event(&app.input_ta, ev)) {
            wm_invalidate(hwnd);
            return true;
        }
        return false;
    }

    if (ev->type == WM_CHAR) {
        if (!app.busy && textarea_event(&app.input_ta, ev)) {
            wm_invalidate(hwnd);
            return true;
        }
        return true;
    }

    return false;
}

/* ========================================================================
 * Entry point
 * ======================================================================== */

int main(void) {
    memset(&app, 0, sizeof(app));
    app.btn_pressed = -1;
    app_task = xTaskGetCurrentTaskHandle();

    int fw = CLIENT_W + 2 * THEME_BORDER_WIDTH;
    int fh = CLIENT_H + THEME_TITLE_HEIGHT + 2 * THEME_BORDER_WIDTH;

    app.hwnd = wm_create_window(
        100, 60, fw, fh,
        "NetTools", WSTYLE_DIALOG,
        app_event, app_paint);

    if (app.hwnd == HWND_NULL)
        return 1;

    window_t *win = wm_get_window(app.hwnd);
    if (win) win->bg_color = THEME_BUTTON_FACE;

    /* Input textarea — single line height */
    textarea_init(&app.input_ta, app.input_buf, INPUT_BUF_SIZE, app.hwnd);
    textarea_set_rect(&app.input_ta, INPUT_X, INPUT_Y, INPUT_W, INPUT_H);

    /* Result textarea — multi-line, read-only (cursor always hidden) */
    textarea_init(&app.result_ta, app.result_buf, RESULT_BUF_SIZE, app.hwnd);
    textarea_set_rect(&app.result_ta, RESULT_X, RESULT_Y, RESULT_W, RESULT_H);
    app.result_ta.cursor_visible = false;

    /* Cursor blink timer (500ms) */
    app.blink_timer = xTimerCreate("ntblink", pdMS_TO_TICKS(500),
                                   pdTRUE, 0, blink_cb);
    if (app.blink_timer)
        xTimerStart(app.blink_timer, 0);

    wm_show_window(app.hwnd);
    wm_set_focus(app.hwnd);
    taskbar_invalidate();

    /* Main loop */
    while (!app.closing) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

        if (app.closing)
            break;

        if (!app.busy && textarea_get_length(&app.input_ta) > 0) {
            if (!netcard_wifi_connected()) {
                textarea_set_text(&app.result_ta, "Not connected to WiFi", 21);
                wm_invalidate(app.hwnd);
                continue;
            }
            if (app.tab == TAB_PING)
                do_ping();
            else
                do_resolve();
        }
    }

    if (app.blink_timer) {
        xTimerStop(app.blink_timer, 0);
        xTimerDelete(app.blink_timer, 0);
    }
    wm_destroy_window(app.hwnd);
    taskbar_invalidate();
    return 0;
}
