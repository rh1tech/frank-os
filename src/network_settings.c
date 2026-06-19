/*
 * FRANK OS — Network Settings Internal App
 * Copyright (c) 2026 Mikhail Matveev <xtreme@rh1.tech>
 * https://rh1.tech
 *
 * WiFi settings window: scan for networks, connect, disconnect.
 * Accessible from Start -> Settings -> Network.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "network_settings.h"
#include "lang.h"
#include "netcard.h"
#include "wifi_config.h"
#include "window.h"
#include "window_event.h"
#include "window_theme.h"
#include "window_draw.h"
#include "controls.h"
#include "font.h"
#include "dialog.h"
#include "taskbar.h"
#include "FreeRTOS.h"
#include "task.h"
#include "timers.h"
#include <string.h>
#include <stdio.h>

/* Icons */
extern const uint8_t *net_icon16_connect_get(void);
extern const uint8_t *net_icon32_connect_get(void);

/*==========================================================================
 * Constants
 *=========================================================================*/

#define NS_MAX_NETWORKS    20
#define NS_TABBAR_Y        4       /* top of tab strip */
#define NS_TABBAR_H        20      /* tab strip height */
#define NS_DY              (NS_TABBAR_Y + NS_TABBAR_H + 2)  /* content top */
#define NS_LIST_Y          (NS_DY + 28)  /* top of network list in client coords */
#define NS_LIST_ROW_H      16      /* height of each list row */
#define NS_VISIBLE_ROWS    10      /* max visible rows */
#define NS_BTN_W           70
#define NS_BTN_H           22
#define NS_BTN_Y           (NS_LIST_Y + NS_VISIBLE_ROWS * NS_LIST_ROW_H + 8)

/* Tabs */
enum { TAB_ADAPTER = 0, TAB_WIFI = 1 };

/* Adapter (pins) tab field geometry */
#define NS_PIN_LABEL_X     16
#define NS_PIN_FIELD_X     96
#define NS_PIN_FIELD_W     56
#define NS_PIN_FIELD_H     20
#define NS_PIN_RX_Y        (NS_DY + 46)
#define NS_PIN_TX_Y        (NS_PIN_RX_Y + 30)

/*==========================================================================
 * State
 *=========================================================================*/

typedef struct {
    char ssid[33];
    int  rssi;
    int  enc;
    int  ch;
} ns_network_t;

typedef struct {
    hwnd_t  hwnd;
    int8_t  active_tab;         /* TAB_ADAPTER or TAB_WIFI */
    ns_network_t networks[NS_MAX_NETWORKS];
    int     net_count;
    int8_t  selected;
    int8_t  scroll_top;
    bool    scanning;
    bool    connecting;
    bool    probing;            /* reconfiguring/probing adapter pins */
    uint8_t conn_dots;          /* animation: 0-3 dots */
    TimerHandle_t conn_timer;   /* 500ms timer for dot animation */
    TimerHandle_t blink_timer;  /* 500ms cursor blink for pin fields */
    scrollbar_t vscroll;        /* vertical scrollbar for network list */
    int8_t  btn_pressed;        /* -1=none, 0..2=which button pressed */
    int8_t  pin_btn_pressed;    /* adapter tab: -1=none, 0=Connect, 1=Close */
    int8_t  tab_pressed;        /* -1=none, else tab index being pressed */
    /* Adapter tab pin entry */
    textfield_t tf_rx;
    textfield_t tf_tx;
    char    rx_buf[4];
    char    tx_buf[4];
} ns_state_t;

/* Heap-allocated to save BSS — allocated on create, freed on close */
static ns_state_t *nsp;
#define ns (*nsp)

/*==========================================================================
 * Scrollbar helpers
 *=========================================================================*/

static void ns_update_scrollbar(void) {
    int list_h = NS_VISIBLE_ROWS * NS_LIST_ROW_H;
    int content_h = ns.net_count * NS_LIST_ROW_H;
    rect_t cr = wm_get_client_rect(ns.hwnd);
    int cw = cr.w;

    ns.vscroll.x = cw - 5 - SCROLLBAR_WIDTH;
    ns.vscroll.y = NS_LIST_Y;
    ns.vscroll.w = SCROLLBAR_WIDTH;
    ns.vscroll.h = list_h;
    scrollbar_set_range(&ns.vscroll, content_h, list_h);
    scrollbar_set_pos(&ns.vscroll, ns.scroll_top * NS_LIST_ROW_H);
}

/*==========================================================================
 * Scan callback (called on netcard task, fills state)
 *=========================================================================*/

static void ns_scan_cb(const char *ssid, int rssi, int enc, int ch) {
    if (!nsp || ns.net_count >= NS_MAX_NETWORKS) return;
    ns_network_t *n = &ns.networks[ns.net_count];
    strncpy(n->ssid, ssid, 32);
    n->ssid[32] = '\0';
    n->rssi = rssi;
    n->enc = enc;
    n->ch = ch;
    ns.net_count++;
}

static void ns_scan_done(bool success) {
    ns.scanning = false;
    if (nsp && ns.hwnd != HWND_NULL) {
        ns_update_scrollbar();
        wm_invalidate(ns.hwnd);
    }
    (void)success;
}

static void ns_conn_timer_stop(void) {
    if (ns.conn_timer) {
        xTimerStop(ns.conn_timer, 0);
        xTimerDelete(ns.conn_timer, 0);
        ns.conn_timer = NULL;
    }
}

static void ns_join_done(bool success) {
    if (!nsp) return;
    ns.connecting = false;
    ns_conn_timer_stop();
    if (ns.hwnd != HWND_NULL)
        wm_invalidate(ns.hwnd);
    if (success) {
        taskbar_invalidate();
    } else {
        /* Show error dialog */
        dialog_show(ns.hwnd, L(STR_NETWORK),
                    L(STR_NET_CONN_FAILED),
                    DLG_ICON_ERROR, DLG_BTN_OK);
    }
}

static void ns_quit_done(bool success) {
    if (nsp && ns.hwnd != HWND_NULL)
        wm_invalidate(ns.hwnd);
    taskbar_invalidate();
    (void)success;
}

/*==========================================================================
 * Helpers
 *=========================================================================*/

static const char *enc_str(int enc) {
    switch (enc) {
    case 2: return "WPA";
    case 4: return "WPA2";
    case 5: return "WEP";
    case 7: return "Open";
    case 8: return "Auto";
    default: return "?";
    }
}

/* Signal bars: 4=strong, 1=weak */
static int signal_bars(int rssi) {
    if (rssi >= -50) return 4;
    if (rssi >= -60) return 3;
    if (rssi >= -70) return 2;
    return 1;
}

static void ns_start_scan(void) {
    ns.scanning = true;
    ns.net_count = 0;
    ns.selected = 0;
    ns.scroll_top = 0;
    netcard_request_scan(ns_scan_cb, ns_scan_done);
    if (ns.hwnd != HWND_NULL)
        wm_invalidate(ns.hwnd);
}

/* Timer callback: animate dots in status line (connect or probe) */
static void ns_conn_timer_cb(TimerHandle_t xTimer) {
    (void)xTimer;
    if (!nsp || (!ns.connecting && !ns.probing)) return;
    ns.conn_dots = (ns.conn_dots + 1) % 4;
    wm_invalidate(ns.hwnd);
}

static void ns_anim_timer_start(void) {
    ns.conn_dots = 0;
    if (!ns.conn_timer) {
        ns.conn_timer = xTimerCreate("nconn", pdMS_TO_TICKS(500),
                                     pdTRUE, NULL, ns_conn_timer_cb);
    }
    if (ns.conn_timer)
        xTimerStart(ns.conn_timer, 0);
}

static void ns_start_connect(const char *ssid, const char *pass) {
    ns.connecting = true;
    ns_anim_timer_start();
    netcard_request_join(ssid, pass, ns_join_done);
    wm_invalidate(ns.hwnd);
}

/* Button geometry helpers */
static int ns_client_w(void) {
    rect_t r = wm_get_client_rect(ns.hwnd);
    return r.w;
}

/*==========================================================================
 * Tab strip
 *=========================================================================*/

static void ns_tab_rect(int tab, int cw, int *ox, int *oy, int *ow, int *oh) {
    int avail = cw - 8;
    int tw = avail / 2;
    *ox = 4 + tab * tw;
    *oy = NS_TABBAR_Y;
    *ow = (tab == 1) ? (avail - tw) : tw;  /* 2nd tab takes remainder */
    *oh = NS_TABBAR_H;
}

static int ns_tab_hit(int16_t x, int16_t y) {
    int cw = ns_client_w();
    for (int i = 0; i < 2; i++) {
        int tx, ty, tw, th;
        ns_tab_rect(i, cw, &tx, &ty, &tw, &th);
        if (x >= tx && x < tx + tw && y >= ty && y < ty + th)
            return i;
    }
    return -1;
}

/*==========================================================================
 * Adapter (pins) tab — cursor blink + Connect handling
 *=========================================================================*/

static void ns_blink_timer_stop(void) {
    if (ns.blink_timer) {
        xTimerStop(ns.blink_timer, 0);
        xTimerDelete(ns.blink_timer, 0);
        ns.blink_timer = NULL;
    }
}

static void ns_blink_timer_cb(TimerHandle_t xTimer) {
    (void)xTimer;
    if (!nsp || ns.active_tab != TAB_ADAPTER) return;
    if (ns.tf_rx.focused) textfield_blink(&ns.tf_rx);
    else if (ns.tf_tx.focused) textfield_blink(&ns.tf_tx);
}

/* Adapter tab button layout: Connect + Close, centered. */
enum { PBTN_CONNECT = 0, PBTN_CLOSE, PBTN_COUNT };

static void ns_pin_btn_rect(int btn, int cw, int *ox, int *oy, int *ow, int *oh) {
    int spacing = 8;
    int total_w = PBTN_COUNT * NS_BTN_W + (PBTN_COUNT - 1) * spacing;
    int start_x = (cw - total_w) / 2;
    *ox = start_x + btn * (NS_BTN_W + spacing);
    *oy = NS_BTN_Y;
    *ow = NS_BTN_W;
    *oh = NS_BTN_H;
}

static int ns_pin_btn_hit(int16_t x, int16_t y) {
    int cw = ns_client_w();
    for (int i = 0; i < PBTN_COUNT; i++) {
        int bx, by, bw, bh;
        ns_pin_btn_rect(i, cw, &bx, &by, &bw, &bh);
        if (x >= bx && x < bx + bw && y >= by && y < by + bh)
            return i;
    }
    return -1;
}

/* Parse a 1-2 digit string into an int; -1 if empty/invalid. */
static int ns_parse_pin(const char *s) {
    if (!s || !s[0]) return -1;
    int v = 0;
    for (const char *p = s; *p; p++) {
        if (*p < '0' || *p > '9') return -1;
        v = v * 10 + (*p - '0');
    }
    return v;
}

/* Result of the netcard pin reconfigure/probe request. */
static void ns_setpins_done(bool success) {
    if (!nsp) return;
    ns.probing = false;
    ns_conn_timer_stop();
    if (success) {
        /* Adapter answered — switch to the Wi-Fi tab and scan. */
        ns.active_tab = TAB_WIFI;
        ns.tf_rx.focused = false;
        ns.tf_tx.focused = false;
        taskbar_invalidate();
        if (ns.hwnd != HWND_NULL)
            wm_invalidate(ns.hwnd);
        ns_start_scan();
    } else {
        if (ns.hwnd != HWND_NULL)
            wm_invalidate(ns.hwnd);
        dialog_show(ns.hwnd, L(STR_NETWORK),
                    L(STR_NET_NO_ADAPTER_PINS),
                    DLG_ICON_ERROR, DLG_BTN_OK);
    }
}

static void ns_pin_connect(void) {
    int rx = ns_parse_pin(textfield_get_text(&ns.tf_rx));
    int tx = ns_parse_pin(textfield_get_text(&ns.tf_tx));
    if (rx < 16 || rx > 47 || tx < 16 || tx > 47 || rx == tx) {
        dialog_show(ns.hwnd, L(STR_NETWORK),
                    L(STR_NET_INVALID_PINS),
                    DLG_ICON_ERROR, DLG_BTN_OK);
        return;
    }
    ns.probing = true;
    ns_anim_timer_start();
    netcard_request_setpins((uint8_t)rx, (uint8_t)tx, ns_setpins_done);
    wm_invalidate(ns.hwnd);
}

/*==========================================================================
 * Button hit-test
 *=========================================================================*/

enum { BTN_SCAN = 0, BTN_CONNECT, BTN_DISCONNECT, BTN_CLOSE, BTN_COUNT };

static void ns_btn_rect(int btn, int cw, int *ox, int *oy, int *ow, int *oh) {
    int spacing = 8;
    int total_w = BTN_COUNT * NS_BTN_W + (BTN_COUNT - 1) * spacing;
    int start_x = (cw - total_w) / 2;
    *ox = start_x + btn * (NS_BTN_W + spacing);
    *oy = NS_BTN_Y;
    *ow = NS_BTN_W;
    *oh = NS_BTN_H;
}

static int ns_btn_hit(int16_t x, int16_t y) {
    int cw = ns_client_w();
    for (int i = 0; i < BTN_COUNT; i++) {
        int bx, by, bw, bh;
        ns_btn_rect(i, cw, &bx, &by, &bw, &bh);
        if (x >= bx && x < bx + bw && y >= by && y < by + bh)
            return i;
    }
    return -1;
}

static int ns_list_hit(int16_t x, int16_t y) {
    if (y < NS_LIST_Y || y >= NS_LIST_Y + NS_VISIBLE_ROWS * NS_LIST_ROW_H)
        return -1;
    int row = (y - NS_LIST_Y) / NS_LIST_ROW_H + ns.scroll_top;
    if (row >= ns.net_count)
        return -1;
    (void)x;
    return row;
}

/*==========================================================================
 * Event handler
 *=========================================================================*/

/* Adapter (pins) tab event handling */
static bool ns_event_pins(hwnd_t hwnd, const window_event_t *ev) {
    switch (ev->type) {
    case WM_LBUTTONDOWN: {
        /* Re-evaluate field focus from the click location. */
        ns.tf_rx.focused = false;
        ns.tf_tx.focused = false;
        textfield_event(&ns.tf_rx, ev);
        textfield_event(&ns.tf_tx, ev);
        int btn_down = ns_pin_btn_hit(ev->mouse.x, ev->mouse.y);
        if (btn_down >= 0)
            ns.pin_btn_pressed = (int8_t)btn_down;
        wm_invalidate(hwnd);
        return true;
    }
    case WM_LBUTTONUP: {
        ns.pin_btn_pressed = -1;
        int btn = ns_pin_btn_hit(ev->mouse.x, ev->mouse.y);
        if (btn == PBTN_CONNECT && !ns.probing) {
            ns_pin_connect();
        } else if (btn == PBTN_CLOSE) {
            window_event_t close_ev;
            memset(&close_ev, 0, sizeof(close_ev));
            close_ev.type = WM_CLOSE;
            wm_post_event(hwnd, &close_ev);
        }
        wm_invalidate(hwnd);
        return true;
    }
    case WM_CHAR: {
        if (ns.probing) return true;
        char ch = ev->charev.ch;
        /* Pins are 2-digit GPIO numbers — accept digits only. */
        if (ch < '0' || ch > '9') return true;
        if (ns.tf_rx.focused && ns.tf_rx.len < 2)
            textfield_event(&ns.tf_rx, ev);
        else if (ns.tf_tx.focused && ns.tf_tx.len < 2)
            textfield_event(&ns.tf_tx, ev);
        return true;
    }
    case WM_KEYDOWN: {
        if (ns.probing) return true;
        /* Tab key toggles focus between the two pin fields. */
        if (ev->key.scancode == 0x2B /* Tab */) {
            bool rx = ns.tf_rx.focused;
            ns.tf_rx.focused = !rx;
            ns.tf_tx.focused = rx;
            wm_invalidate(hwnd);
            return true;
        }
        if (ns.tf_rx.focused) return textfield_event(&ns.tf_rx, ev);
        if (ns.tf_tx.focused) return textfield_event(&ns.tf_tx, ev);
        return false;
    }
    default:
        return false;
    }
}

/* Wi-Fi (scan/connect) tab event handling */
static bool ns_event_wifi(hwnd_t hwnd, const window_event_t *ev) {
    /* Let scrollbar handle mouse events first */
    if (ev->type == WM_LBUTTONDOWN || ev->type == WM_LBUTTONUP ||
        ev->type == WM_MOUSEMOVE) {
        int32_t new_pos;
        if (scrollbar_event(&ns.vscroll, ev, &new_pos)) {
            ns.scroll_top = new_pos / NS_LIST_ROW_H;
            if (ns.scroll_top < 0) ns.scroll_top = 0;
            int max_top = ns.net_count - NS_VISIBLE_ROWS;
            if (max_top < 0) max_top = 0;
            if (ns.scroll_top > max_top) ns.scroll_top = max_top;
            scrollbar_set_pos(&ns.vscroll, ns.scroll_top * NS_LIST_ROW_H);
            wm_invalidate(hwnd);
            return true;
        }
    }

    switch (ev->type) {
    case WM_LBUTTONDOWN: {
        int row = ns_list_hit(ev->mouse.x, ev->mouse.y);
        if (row >= 0) {
            ns.selected = row;
            wm_invalidate(hwnd);
        }
        /* Button press animation */
        int btn_down = ns_btn_hit(ev->mouse.x, ev->mouse.y);
        if (btn_down >= 0) {
            nsp->btn_pressed = (int8_t)btn_down;
            wm_invalidate(hwnd);
        }
        return true;
    }

    case WM_LBUTTONUP: {
        nsp->btn_pressed = -1;
        wm_invalidate(hwnd);
        int btn = ns_btn_hit(ev->mouse.x, ev->mouse.y);
        if (btn == BTN_SCAN && !ns.scanning && !ns.connecting) {
            ns_start_scan();
        } else if (btn == BTN_CONNECT && ns.connecting) {
            /* Cancel ongoing connection */
            ns.connecting = false;
            ns_conn_timer_stop();
            netcard_request_quit(NULL);
            wm_invalidate(hwnd);
        } else if (btn == BTN_CONNECT && ns.selected >= 0 && ns.selected < ns.net_count &&
                   !ns.scanning && !ns.connecting) {
            if (ns.networks[ns.selected].enc == 7) {
                /* Open network */
                ns_start_connect(ns.networks[ns.selected].ssid, "");
            } else {
                /* Need password */
                dialog_input_set_mask(true);
                dialog_input_show(hwnd, L(STR_WIFI_PASSWORD),
                                  L(STR_ENTER_PASSWORD), NULL, 64);
            }
        } else if (btn == BTN_DISCONNECT && netcard_wifi_connected()) {
            netcard_request_quit(ns_quit_done);
        } else if (btn == BTN_CLOSE) {
            window_event_t close_ev;
            memset(&close_ev, 0, sizeof(close_ev));
            close_ev.type = WM_CLOSE;
            wm_post_event(hwnd, &close_ev);
        }
        return true;
    }

    case WM_COMMAND:
        /* Password dialog result */
        if (ev->command.id == DLG_RESULT_INPUT && ns.selected >= 0) {
            const char *pass = dialog_input_get_text();
            if (pass)
                ns_start_connect(ns.networks[ns.selected].ssid, pass);
        }
        return true;

    case WM_KEYDOWN:
        if (ev->key.scancode == 0x52 /* UP */ && ns.selected > 0) {
            ns.selected--;
            if (ns.selected < ns.scroll_top) {
                ns.scroll_top = ns.selected;
                ns_update_scrollbar();
            }
            wm_invalidate(hwnd);
            return true;
        }
        if (ev->key.scancode == 0x51 /* DOWN */ && ns.selected < ns.net_count - 1) {
            ns.selected++;
            if (ns.selected >= ns.scroll_top + NS_VISIBLE_ROWS) {
                ns.scroll_top = ns.selected - NS_VISIBLE_ROWS + 1;
                ns_update_scrollbar();
            }
            wm_invalidate(hwnd);
            return true;
        }
        break;

    default:
        break;
    }
    return false;
}

static bool ns_event(hwnd_t hwnd, const window_event_t *ev) {
    /* Window close — tear down regardless of active tab. */
    if (ev->type == WM_CLOSE) {
        extern void vPortFree(void *);
        ns_conn_timer_stop();
        ns_blink_timer_stop();
        wm_destroy_window(hwnd);
        vPortFree(nsp);
        nsp = NULL;
        return true;
    }

    /* Tab strip clicks switch tabs (only when not busy). */
    if (ev->type == WM_LBUTTONDOWN) {
        int t = ns_tab_hit(ev->mouse.x, ev->mouse.y);
        if (t >= 0) {
            ns.tab_pressed = (int8_t)t;
            wm_invalidate(hwnd);
            return true;
        }
    } else if (ev->type == WM_LBUTTONUP && ns.tab_pressed >= 0) {
        int t = ns_tab_hit(ev->mouse.x, ev->mouse.y);
        int pressed = ns.tab_pressed;
        ns.tab_pressed = -1;
        if (t == pressed && !ns.probing && !ns.scanning && !ns.connecting) {
            if (t == TAB_WIFI) {
                /* Wi-Fi tab only usable once an adapter is present. */
                if (netcard_is_available()) {
                    ns.active_tab = TAB_WIFI;
                    if (ns.net_count == 0)
                        ns_start_scan();
                }
            } else {
                ns.active_tab = TAB_ADAPTER;
            }
        }
        wm_invalidate(hwnd);
        return true;
    }

    if (ns.active_tab == TAB_ADAPTER)
        return ns_event_pins(hwnd, ev);
    return ns_event_wifi(hwnd, ev);
}

/*==========================================================================
 * Paint handler
 *=========================================================================*/

static void ns_draw_button(int btn, int cw, const char *label, bool enabled) {
    int bx, by, bw, bh;
    ns_btn_rect(btn, cw, &bx, &by, &bw, &bh);
    bool pressed = (nsp->btn_pressed == btn);
    wd_button(bx, by, bw, bh, label, false, pressed);
    if (!enabled && !pressed) {
        int tw = gfx_utf8_charcount(label) * FONT_UI_WIDTH;
        int tx = bx + (bw - tw) / 2;
        int ty = by + (bh - FONT_UI_HEIGHT) / 2;
        wd_text_ui(tx, ty, label, COLOR_DARK_GRAY, THEME_BUTTON_FACE);
    }
}

static void ns_draw_tabs(int cw) {
    const char *labels[2] = { L(STR_NET_TAB_ADAPTER), L(STR_NET_TAB_WIFI) };
    bool enabled[2] = { true, netcard_is_available() };
    for (int i = 0; i < 2; i++) {
        int tx, ty, tw, th;
        ns_tab_rect(i, cw, &tx, &ty, &tw, &th);
        bool active = (ns.active_tab == i);
        bool pressed = (ns.tab_pressed == i);
        /* Active tab raised/forward; inactive sunken. */
        wd_button(tx, ty, tw, th, "", false, pressed ? true : !active);
        const char *label = labels[i];
        uint8_t fg = enabled[i] ? COLOR_BLACK : COLOR_DARK_GRAY;
        int lw = gfx_utf8_charcount(label) * FONT_UI_WIDTH;
        int lx = tx + (tw - lw) / 2;
        int ly = ty + (th - FONT_UI_HEIGHT) / 2;
        wd_text_ui(lx, ly, label, fg, THEME_BUTTON_FACE);
    }
}

static void ns_pin_draw_button(int btn, int cw, const char *label, bool enabled) {
    int bx, by, bw, bh;
    ns_pin_btn_rect(btn, cw, &bx, &by, &bw, &bh);
    bool pressed = (ns.pin_btn_pressed == btn);
    wd_button(bx, by, bw, bh, label, false, pressed);
    if (!enabled && !pressed) {
        int tw = gfx_utf8_charcount(label) * FONT_UI_WIDTH;
        int tx = bx + (bw - tw) / 2;
        int ty = by + (bh - FONT_UI_HEIGHT) / 2;
        wd_text_ui(tx, ty, label, COLOR_DARK_GRAY, THEME_BUTTON_FACE);
    }
}

/* Draw a multi-line ('\n'-separated) hint string starting at (x, y). */
static void ns_draw_multiline(int x, int y, const char *s, uint8_t fg) {
    char line[64];
    int li = 0;
    for (const char *p = s; ; p++) {
        if (*p == '\n' || *p == '\0') {
            line[li] = '\0';
            wd_text_ui(x, y, line, fg, THEME_BUTTON_FACE);
            y += FONT_UI_HEIGHT + 2;
            li = 0;
            if (*p == '\0') break;
        } else if (li < (int)sizeof(line) - 1) {
            line[li++] = *p;
        }
    }
}

static void ns_paint_pins(int cw) {
    /* Hint */
    ns_draw_multiline(NS_PIN_LABEL_X, NS_DY + 2, L(STR_NET_ADAPTER_HINT),
                      COLOR_DARK_GRAY);

    /* RX field */
    wd_text_ui(NS_PIN_LABEL_X, NS_PIN_RX_Y + (NS_PIN_FIELD_H - FONT_UI_HEIGHT) / 2,
               L(STR_NET_RX_PIN), COLOR_BLACK, THEME_BUTTON_FACE);
    textfield_paint(&ns.tf_rx);

    /* TX field */
    wd_text_ui(NS_PIN_LABEL_X, NS_PIN_TX_Y + (NS_PIN_FIELD_H - FONT_UI_HEIGHT) / 2,
               L(STR_NET_TX_PIN), COLOR_BLACK, THEME_BUTTON_FACE);
    textfield_paint(&ns.tf_tx);

    /* Status line */
    int status_y = NS_PIN_TX_Y + NS_PIN_FIELD_H + 12;
    if (ns.probing) {
        char status[48];
        const char *dots = &"..."[3 - ns.conn_dots];
        snprintf(status, sizeof(status), "%s%s", L(STR_NET_PROBING), dots);
        wd_text_ui(NS_PIN_LABEL_X, status_y, status, COLOR_BLACK, THEME_BUTTON_FACE);
    } else if (netcard_is_available()) {
        wd_text_ui(NS_PIN_LABEL_X, status_y, L(STR_NOT_CONNECTED),
                   COLOR_BLACK, THEME_BUTTON_FACE);
    } else {
        wd_text_ui(NS_PIN_LABEL_X, status_y, L(STR_NO_ADAPTER),
                   COLOR_DARK_GRAY, THEME_BUTTON_FACE);
    }

    /* Buttons */
    ns_pin_draw_button(PBTN_CONNECT, cw, L(STR_CONNECT), !ns.probing);
    ns_pin_draw_button(PBTN_CLOSE, cw, L(STR_CLOSE), true);
}

static void ns_paint_wifi(int cw) {
    bool has_sb = ns.vscroll.visible;
    int list_content_w = cw - 10 - (has_sb ? SCROLLBAR_WIDTH : 0);

    /* Status line */
    if (ns.connecting) {
        char status[64];
        const char *dots = &"..."[3 - ns.conn_dots]; /* "", ".", "..", "..." */
        if (ns.selected >= 0 && ns.selected < ns.net_count)
            snprintf(status, sizeof(status), L(STR_CONNECTING_TO),
                     ns.networks[ns.selected].ssid, dots);
        else
            snprintf(status, sizeof(status), "%s%s", L(STR_CONNECTING), dots);
        wd_text_ui(8, NS_DY, status, COLOR_BLACK, THEME_BUTTON_FACE);
    } else if (netcard_wifi_connected()) {
        char status[80];
        snprintf(status, sizeof(status), L(STR_CONNECTED_TO),
                 wifi_config_get()->ssid, netcard_wifi_ip());
        wd_text_ui(8, NS_DY, status, COLOR_BLACK, THEME_BUTTON_FACE);
    } else if (!netcard_is_available()) {
        wd_text_ui(8, NS_DY, L(STR_NO_ADAPTER), COLOR_DARK_GRAY, THEME_BUTTON_FACE);
    } else {
        wd_text_ui(8, NS_DY, L(STR_NOT_CONNECTED), COLOR_BLACK, THEME_BUTTON_FACE);
    }

    /* Separator */
    wd_hline(4, NS_DY + 12, cw - 8, COLOR_DARK_GRAY);
    wd_hline(4, NS_DY + 13, cw - 8, COLOR_WHITE);

    /* Column headers */
    wd_text_ui(8, NS_DY + 16, L(STR_HDR_NETWORK), COLOR_BLACK, THEME_BUTTON_FACE);
    wd_text_ui(5 + list_content_w - 90, NS_DY + 16, L(STR_HDR_SIGNAL), COLOR_BLACK, THEME_BUTTON_FACE);
    wd_text_ui(5 + list_content_w - 40, NS_DY + 16, L(STR_HDR_TYPE), COLOR_BLACK, THEME_BUTTON_FACE);

    /* Network list (sunken well) */
    int list_h = NS_VISIBLE_ROWS * NS_LIST_ROW_H;
    int well_w = cw - 8;
    wd_fill_rect(4, NS_LIST_Y - 1, well_w, list_h + 2, COLOR_WHITE);
    wd_hline(4, NS_LIST_Y - 1, well_w, COLOR_DARK_GRAY);
    wd_vline(4, NS_LIST_Y - 1, list_h + 2, COLOR_DARK_GRAY);
    wd_hline(5, NS_LIST_Y + list_h, well_w - 1, COLOR_WHITE);
    wd_vline(4 + well_w - 1, NS_LIST_Y, list_h, COLOR_WHITE);

    if (ns.scanning) {
        wd_text_ui(8, NS_LIST_Y + list_h / 2 - FONT_UI_HEIGHT / 2,
                   L(STR_SCANNING), COLOR_DARK_GRAY, COLOR_WHITE);
    } else if (ns.net_count == 0) {
        wd_text_ui(8, NS_LIST_Y + list_h / 2 - FONT_UI_HEIGHT / 2,
                   L(STR_NO_NETWORKS), COLOR_DARK_GRAY, COLOR_WHITE);
    } else {
        for (int i = 0; i < NS_VISIBLE_ROWS && (i + ns.scroll_top) < ns.net_count; i++) {
            int ni = i + ns.scroll_top;
            bool sel = (ni == ns.selected);
            uint8_t fg = sel ? COLOR_WHITE : COLOR_BLACK;
            uint8_t bg = sel ? COLOR_BLUE : COLOR_WHITE;
            int ry = NS_LIST_Y + i * NS_LIST_ROW_H;

            wd_fill_rect(5, ry, list_content_w, NS_LIST_ROW_H, bg);

            /* SSID */
            wd_text_ui(8, ry + (NS_LIST_ROW_H - FONT_UI_HEIGHT) / 2,
                       ns.networks[ni].ssid, fg, bg);

            /* Signal bars */
            int bars = signal_bars(ns.networks[ni].rssi);
            char sig[8];
            for (int b = 0; b < 4; b++)
                sig[b] = (b < bars) ? '|' : ' ';
            sig[4] = '\0';
            wd_text_ui(5 + list_content_w - 90, ry + (NS_LIST_ROW_H - FONT_UI_HEIGHT) / 2,
                       sig, fg, bg);

            /* Encryption type */
            wd_text_ui(5 + list_content_w - 40, ry + (NS_LIST_ROW_H - FONT_UI_HEIGHT) / 2,
                       enc_str(ns.networks[ni].enc), fg, bg);
        }
    }

    /* Scrollbar */
    ns_update_scrollbar();
    if (ns.vscroll.visible)
        scrollbar_paint(&ns.vscroll);

    /* Buttons */
    bool can_act = netcard_is_available() && !ns.scanning && !ns.connecting;
    ns_draw_button(BTN_SCAN, cw, L(STR_SCAN), can_act);
    if (ns.connecting) {
        ns_draw_button(BTN_CONNECT, cw, L(STR_CANCEL), true);
    } else {
        ns_draw_button(BTN_CONNECT, cw, L(STR_CONNECT),
                       can_act && ns.selected >= 0 && ns.selected < ns.net_count);
    }
    ns_draw_button(BTN_DISCONNECT, cw, L(STR_DISCONNECT),
                   can_act && netcard_wifi_connected());
    ns_draw_button(BTN_CLOSE, cw, L(STR_CLOSE), true);
}

static void ns_paint(hwnd_t hwnd) {
    wd_begin(hwnd);
    wd_clear(THEME_BUTTON_FACE);

    rect_t cr = wm_get_client_rect(hwnd);
    int cw = cr.w;

    ns_draw_tabs(cw);

    if (ns.active_tab == TAB_ADAPTER)
        ns_paint_pins(cw);
    else
        ns_paint_wifi(cw);

    wd_end();
}

/*==========================================================================
 * Create
 *=========================================================================*/

hwnd_t network_settings_create(void) {
    extern void *pvPortMalloc(unsigned int);

    if (nsp) return nsp->hwnd;  /* already open */
    nsp = (ns_state_t *)pvPortMalloc(sizeof(ns_state_t));
    if (!nsp) return HWND_NULL;
    memset(&ns, 0, sizeof(ns));
    ns.selected = -1;
    nsp->btn_pressed = -1;
    ns.pin_btn_pressed = -1;
    ns.tab_pressed = -1;

    scrollbar_init(&ns.vscroll, false);

    int w = 340 + THEME_BORDER_WIDTH * 2;
    int h = NS_BTN_Y + NS_BTN_H + 12 + THEME_TITLE_HEIGHT + THEME_BORDER_WIDTH * 2;

    hwnd_t hwnd = wm_create_window(
        80, 40, w, h,
        L(STR_NETWORK), WSTYLE_DIALOG,
        ns_event, ns_paint);

    if (hwnd == HWND_NULL) {
        extern void vPortFree(void *);
        vPortFree(nsp);
        nsp = NULL;
        return HWND_NULL;
    }

    ns.hwnd = hwnd;

    /* Adapter (pins) tab fields — prefill from saved/default config. */
    wifi_config_t *cfg = wifi_config_get();
    uint8_t rx = (cfg && cfg->rx_pin) ? cfg->rx_pin : 0;
    uint8_t tx = (cfg && cfg->tx_pin) ? cfg->tx_pin : 0;
    ns.rx_buf[0] = ns.tx_buf[0] = '\0';
    if (rx) snprintf(ns.rx_buf, sizeof(ns.rx_buf), "%u", rx);
    if (tx) snprintf(ns.tx_buf, sizeof(ns.tx_buf), "%u", tx);
    textfield_init(&ns.tf_rx, ns.rx_buf, sizeof(ns.rx_buf), hwnd);
    textfield_init(&ns.tf_tx, ns.tx_buf, sizeof(ns.tx_buf), hwnd);
    textfield_set_rect(&ns.tf_rx, NS_PIN_FIELD_X, NS_PIN_RX_Y,
                       NS_PIN_FIELD_W, NS_PIN_FIELD_H);
    textfield_set_rect(&ns.tf_tx, NS_PIN_FIELD_X, NS_PIN_TX_Y,
                       NS_PIN_FIELD_W, NS_PIN_FIELD_H);

    /* Start on the Wi-Fi tab if an adapter is already up, else Adapter tab. */
    if (netcard_is_available()) {
        ns.active_tab = TAB_WIFI;
        ns_start_scan();
    } else {
        ns.active_tab = TAB_ADAPTER;
        ns.tf_rx.focused = true;
    }

    /* Cursor blink timer for the pin fields. */
    ns.blink_timer = xTimerCreate("nsblink", pdMS_TO_TICKS(500),
                                  pdTRUE, NULL, ns_blink_timer_cb);
    if (ns.blink_timer)
        xTimerStart(ns.blink_timer, 0);

    wm_set_focus(hwnd);
    taskbar_invalidate();

    return hwnd;
}
