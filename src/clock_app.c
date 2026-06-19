/*
 * FRANK OS — Clock Settings Internal App
 * Copyright (c) 2026 Mikhail Matveev <xtreme@rh1.tech>
 * https://rh1.tech
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "clock_app.h"
#include "lang.h"
#include "rtc.h"
#include "settings.h"
#include "window.h"
#include "window_event.h"
#include "window_theme.h"
#include "window_draw.h"
#include "controls.h"
#include "font.h"
#include "dialog.h"
#include "taskbar.h"
#include "FreeRTOS.h"
#include "timers.h"
#include <string.h>
#include <stdio.h>

extern void *pvPortMalloc(unsigned int);
extern void  vPortFree(void *);

/*==========================================================================
 * Layout
 *=========================================================================*/

#define CA_LABEL_X     12
#define CA_FIELD_X     96
#define CA_PIN_FIELD_W 56
#define CA_FIELD_H     20

#define CA_SCL_Y       58
#define CA_SDA_Y       (CA_SCL_Y + 28)
#define CA_TEST_X      164
#define CA_TEST_W      80

#define CA_STATUS_Y    (CA_SDA_Y + 30)
#define CA_SEP_Y       (CA_STATUS_Y + 18)
#define CA_SETHDR_Y    (CA_SEP_Y + 8)
#define CA_DATE_Y      (CA_SETHDR_Y + 22)
#define CA_TIME_Y      (CA_DATE_Y + 28)

/* Date fields */
#define CA_Y_X         CA_FIELD_X
#define CA_Y_W         44
#define CA_MO_X        (CA_Y_X + CA_Y_W + 8)
#define CA_MO_W        30
#define CA_D_X         (CA_MO_X + CA_MO_W + 8)
#define CA_D_W         30
/* Time fields */
#define CA_H_X         CA_FIELD_X
#define CA_H_W         30
#define CA_MI_X        (CA_H_X + CA_H_W + 8)
#define CA_MI_W        30
#define CA_S_X         (CA_MI_X + CA_MI_W + 8)
#define CA_S_W         30

#define CA_BTN_W       70
#define CA_BTN_H       22
#define CA_BTN_Y       (CA_TIME_Y + 34)

/* Bottom buttons */
enum { CBTN_SET = 0, CBTN_CLOSE, CBTN_COUNT };

/*==========================================================================
 * State
 *=========================================================================*/

typedef struct {
    hwnd_t  hwnd;
    bool    rtc_found;
    TimerHandle_t blink_timer;
    int8_t  test_pressed;       /* 0 = Test pressed, else -1 */
    int8_t  btn_pressed;        /* CBTN_* or -1 */

    textfield_t tf_scl, tf_sda;
    char    scl_buf[4], sda_buf[4];

    textfield_t tf_y, tf_mo, tf_d, tf_h, tf_mi, tf_s;
    char    y_buf[6], mo_buf[4], d_buf[4], h_buf[4], mi_buf[4], s_buf[4];
} ca_state_t;

static ca_state_t *cap;
#define ca (*cap)

/* All editable fields, in tab order. Date/time entries start at index 2. */
static textfield_t *ca_fields[8];
#define CA_FIELD_COUNT 8
#define CA_DT_FIRST    2   /* first date/time field index */

static void ca_build_field_list(void) {
    ca_fields[0] = &ca.tf_scl;
    ca_fields[1] = &ca.tf_sda;
    ca_fields[2] = &ca.tf_y;
    ca_fields[3] = &ca.tf_mo;
    ca_fields[4] = &ca.tf_d;
    ca_fields[5] = &ca.tf_h;
    ca_fields[6] = &ca.tf_mi;
    ca_fields[7] = &ca.tf_s;
}

/*==========================================================================
 * Timers
 *=========================================================================*/

static void ca_blink_timer_stop(void) {
    if (ca.blink_timer) {
        xTimerStop(ca.blink_timer, 0);
        xTimerDelete(ca.blink_timer, 0);
        ca.blink_timer = NULL;
    }
}

static void ca_blink_timer_cb(TimerHandle_t xTimer) {
    (void)xTimer;
    if (!cap) return;
    for (int i = 0; i < CA_FIELD_COUNT; i++)
        if (ca_fields[i]->focused) {
            textfield_blink(ca_fields[i]);
            return;
        }
}

/*==========================================================================
 * Field helpers
 *=========================================================================*/

static int16_t ca_field_maxlen(int idx) {
    if (idx == 2) return 4;     /* year */
    return 2;                   /* everything else */
}

static void ca_clear_focus(void) {
    for (int i = 0; i < CA_FIELD_COUNT; i++)
        ca_fields[i]->focused = false;
}

static void ca_focus(int idx) {
    ca_clear_focus();
    if (idx >= 0 && idx < CA_FIELD_COUNT)
        ca_fields[idx]->focused = true;
}

/* Cycle focus to the next selectable field (skip date/time if not detected). */
static void ca_focus_next(void) {
    int cur = -1;
    for (int i = 0; i < CA_FIELD_COUNT; i++)
        if (ca_fields[i]->focused) { cur = i; break; }
    int limit = ca.rtc_found ? CA_FIELD_COUNT : CA_DT_FIRST;
    int next = (cur + 1) % limit;
    ca_focus(next);
}

static int ca_parse_uint(const char *s) {
    if (!s || !s[0]) return -1;
    int v = 0;
    for (const char *p = s; *p; p++) {
        if (*p < '0' || *p > '9') return -1;
        v = v * 10 + (*p - '0');
    }
    return v;
}

static void ca_set_field_int(textfield_t *tf, int v, int width) {
    char tmp[8];
    snprintf(tmp, sizeof(tmp), "%0*d", width, v);
    textfield_set_text(tf, tmp);
}

/* Fill the date/time fields from the chip's current time. */
static void ca_prefill_datetime(void) {
    rtc_time_t t;
    if (!rtc_get_time(&t)) return;
    if (t.year < 2000 || t.year > 2099) t.year = 2026;
    if (t.month < 1 || t.month > 12) t.month = 1;
    if (t.day < 1 || t.day > 31) t.day = 1;
    ca_set_field_int(&ca.tf_y, t.year, 4);
    ca_set_field_int(&ca.tf_mo, t.month, 2);
    ca_set_field_int(&ca.tf_d, t.day, 2);
    ca_set_field_int(&ca.tf_h, t.hour, 2);
    ca_set_field_int(&ca.tf_mi, t.minute, 2);
    ca_set_field_int(&ca.tf_s, t.second, 2);
}

/*==========================================================================
 * Actions
 *=========================================================================*/

static void ca_do_test(void) {
    int scl = ca_parse_uint(textfield_get_text(&ca.tf_scl));
    int sda = ca_parse_uint(textfield_get_text(&ca.tf_sda));
    if (scl < 16 || scl > 47 || sda < 16 || sda > 47 || scl == sda) {
        dialog_show(ca.hwnd, L(STR_CLOCK), L(STR_RTC_BAD_PINS),
                    DLG_ICON_ERROR, DLG_BTN_OK);
        return;
    }

    rtc_set_pins((uint8_t)scl, (uint8_t)sda);
    bool found = rtc_detect();
    ca.rtc_found = found;

    if (found) {
        /* Persist the working pins. */
        settings_t *cfg = settings_get();
        cfg->rtc_scl_pin = (uint8_t)scl;
        cfg->rtc_sda_pin = (uint8_t)sda;
        settings_save();
        ca_prefill_datetime();
        taskbar_invalidate();
    } else {
        dialog_show(ca.hwnd, L(STR_CLOCK), L(STR_RTC_NOT_FOUND),
                    DLG_ICON_ERROR, DLG_BTN_OK);
    }
    wm_invalidate(ca.hwnd);
}

static bool ca_days_in_month_ok(int y, int mo, int d) {
    static const uint8_t dim[] = {31,28,31,30,31,30,31,31,30,31,30,31};
    if (mo < 1 || mo > 12 || d < 1) return false;
    int max = dim[mo - 1];
    if (mo == 2 && ((y % 4 == 0 && y % 100 != 0) || y % 400 == 0)) max = 29;
    return d <= max;
}

static void ca_do_set(void) {
    int y  = ca_parse_uint(textfield_get_text(&ca.tf_y));
    int mo = ca_parse_uint(textfield_get_text(&ca.tf_mo));
    int d  = ca_parse_uint(textfield_get_text(&ca.tf_d));
    int h  = ca_parse_uint(textfield_get_text(&ca.tf_h));
    int mi = ca_parse_uint(textfield_get_text(&ca.tf_mi));
    int s  = ca_parse_uint(textfield_get_text(&ca.tf_s));

    if (y < 2000 || y > 2099 || h < 0 || h > 23 || mi < 0 || mi > 59 ||
        s < 0 || s > 59 || !ca_days_in_month_ok(y, mo, d)) {
        dialog_show(ca.hwnd, L(STR_CLOCK), L(STR_RTC_BAD_TIME),
                    DLG_ICON_ERROR, DLG_BTN_OK);
        return;
    }

    rtc_time_t t = {
        .year = (uint16_t)y, .month = (uint8_t)mo, .day = (uint8_t)d,
        .hour = (uint8_t)h, .minute = (uint8_t)mi, .second = (uint8_t)s
    };
    if (rtc_set_time(&t)) {
        taskbar_invalidate();
        dialog_show(ca.hwnd, L(STR_CLOCK), L(STR_RTC_TIME_SET),
                    DLG_ICON_INFO, DLG_BTN_OK);
    } else {
        dialog_show(ca.hwnd, L(STR_CLOCK), L(STR_RTC_NOT_FOUND),
                    DLG_ICON_ERROR, DLG_BTN_OK);
    }
    wm_invalidate(ca.hwnd);
}

/*==========================================================================
 * Hit-testing
 *=========================================================================*/

static int ca_client_w(void) {
    rect_t r = wm_get_client_rect(ca.hwnd);
    return r.w;
}

static void ca_btn_rect(int btn, int cw, int *ox, int *oy, int *ow, int *oh) {
    int spacing = 8;
    int total_w = CBTN_COUNT * CA_BTN_W + (CBTN_COUNT - 1) * spacing;
    int start_x = (cw - total_w) / 2;
    *ox = start_x + btn * (CA_BTN_W + spacing);
    *oy = CA_BTN_Y;
    *ow = CA_BTN_W;
    *oh = CA_BTN_H;
}

static int ca_btn_hit(int16_t x, int16_t y) {
    int cw = ca_client_w();
    for (int i = 0; i < CBTN_COUNT; i++) {
        int bx, by, bw, bh;
        ca_btn_rect(i, cw, &bx, &by, &bw, &bh);
        if (x >= bx && x < bx + bw && y >= by && y < by + bh)
            return i;
    }
    return -1;
}

static bool ca_test_hit(int16_t x, int16_t y) {
    return x >= CA_TEST_X && x < CA_TEST_X + CA_TEST_W &&
           y >= CA_SCL_Y && y < CA_SCL_Y + CA_BTN_H;
}

/*==========================================================================
 * Event handler
 *=========================================================================*/

static bool ca_event(hwnd_t hwnd, const window_event_t *ev) {
    switch (ev->type) {
    case WM_CLOSE:
        ca_blink_timer_stop();
        wm_destroy_window(hwnd);
        vPortFree(cap);
        cap = NULL;
        return true;

    case WM_LBUTTONDOWN: {
        ca_clear_focus();
        textfield_event(&ca.tf_scl, ev);
        textfield_event(&ca.tf_sda, ev);
        if (ca.rtc_found) {
            textfield_event(&ca.tf_y, ev);
            textfield_event(&ca.tf_mo, ev);
            textfield_event(&ca.tf_d, ev);
            textfield_event(&ca.tf_h, ev);
            textfield_event(&ca.tf_mi, ev);
            textfield_event(&ca.tf_s, ev);
        }
        if (ca_test_hit(ev->mouse.x, ev->mouse.y))
            ca.test_pressed = 0;
        int b = ca_btn_hit(ev->mouse.x, ev->mouse.y);
        if (b >= 0)
            ca.btn_pressed = (int8_t)b;
        wm_invalidate(hwnd);
        return true;
    }

    case WM_LBUTTONUP: {
        bool test_was = (ca.test_pressed == 0);
        int  btn_was = ca.btn_pressed;
        ca.test_pressed = -1;
        ca.btn_pressed = -1;

        if (test_was && ca_test_hit(ev->mouse.x, ev->mouse.y)) {
            ca_do_test();
        } else if (btn_was == CBTN_SET && ca_btn_hit(ev->mouse.x, ev->mouse.y) == CBTN_SET) {
            if (ca.rtc_found) ca_do_set();
        } else if (btn_was == CBTN_CLOSE && ca_btn_hit(ev->mouse.x, ev->mouse.y) == CBTN_CLOSE) {
            window_event_t close_ev;
            memset(&close_ev, 0, sizeof(close_ev));
            close_ev.type = WM_CLOSE;
            wm_post_event(hwnd, &close_ev);
        }
        wm_invalidate(hwnd);
        return true;
    }

    case WM_CHAR: {
        char ch = ev->charev.ch;
        if (ch < '0' || ch > '9') return true;
        for (int i = 0; i < CA_FIELD_COUNT; i++) {
            if (ca_fields[i]->focused) {
                if (ca_fields[i]->len < ca_field_maxlen(i))
                    textfield_event(ca_fields[i], ev);
                break;
            }
        }
        return true;
    }

    case WM_KEYDOWN: {
        if (ev->key.scancode == 0x2B /* Tab */) {
            ca_focus_next();
            wm_invalidate(hwnd);
            return true;
        }
        if (ev->key.scancode == 0x28 /* Enter */) {
            if (ca.rtc_found) ca_do_set();
            else ca_do_test();
            return true;
        }
        for (int i = 0; i < CA_FIELD_COUNT; i++)
            if (ca_fields[i]->focused)
                return textfield_event(ca_fields[i], ev);
        return false;
    }

    default:
        return false;
    }
}

/*==========================================================================
 * Paint
 *=========================================================================*/

static void ca_draw_multiline(int x, int y, const char *s, uint8_t fg) {
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

static void ca_label(int x, int field_y, const char *s, bool enabled) {
    wd_text_ui(x, field_y + (CA_FIELD_H - FONT_UI_HEIGHT) / 2, s,
               enabled ? COLOR_BLACK : COLOR_DARK_GRAY, THEME_BUTTON_FACE);
}

static void ca_draw_button(int bx, int by, int bw, int bh, const char *label,
                           bool enabled, bool pressed) {
    wd_button(bx, by, bw, bh, label, false, pressed);
    if (!enabled && !pressed) {
        int tw = gfx_utf8_charcount(label) * FONT_UI_WIDTH;
        wd_text_ui(bx + (bw - tw) / 2, by + (bh - FONT_UI_HEIGHT) / 2,
                   label, COLOR_DARK_GRAY, THEME_BUTTON_FACE);
    }
}

static void ca_paint(hwnd_t hwnd) {
    wd_begin(hwnd);
    wd_clear(THEME_BUTTON_FACE);

    rect_t cr = wm_get_client_rect(hwnd);
    int cw = cr.w;
    bool found = ca.rtc_found;

    /* Hint */
    ca_draw_multiline(CA_LABEL_X, 8, L(STR_RTC_HINT), COLOR_DARK_GRAY);

    /* Pin fields */
    ca_label(CA_LABEL_X, CA_SCL_Y, L(STR_RTC_SCL_PIN), true);
    textfield_paint(&ca.tf_scl);
    ca_label(CA_LABEL_X, CA_SDA_Y, L(STR_RTC_SDA_PIN), true);
    textfield_paint(&ca.tf_sda);

    /* Test button */
    ca_draw_button(CA_TEST_X, CA_SCL_Y, CA_TEST_W, CA_BTN_H, L(STR_RTC_TEST),
                   true, ca.test_pressed == 0);

    /* Status line */
    if (found) {
        wd_text_ui(CA_LABEL_X, CA_STATUS_Y, L(STR_RTC_DETECTED),
                   COLOR_BLACK, THEME_BUTTON_FACE);
    } else {
        wd_text_ui(CA_LABEL_X, CA_STATUS_Y, L(STR_RTC_NOT_FOUND),
                   COLOR_DARK_GRAY, THEME_BUTTON_FACE);
    }

    /* Separator */
    wd_hline(4, CA_SEP_Y, cw - 8, COLOR_DARK_GRAY);
    wd_hline(4, CA_SEP_Y + 1, cw - 8, COLOR_WHITE);

    /* Date/time section (enabled only when the chip is present) */
    wd_text_ui(CA_LABEL_X, CA_SETHDR_Y, L(STR_RTC_SET_TIME),
               found ? COLOR_BLACK : COLOR_DARK_GRAY, THEME_BUTTON_FACE);

    ca_label(CA_LABEL_X, CA_DATE_Y, L(STR_RTC_DATE), found);
    ca_label(CA_LABEL_X, CA_TIME_Y, L(STR_RTC_TIME), found);
    if (found) {
        textfield_paint(&ca.tf_y);
        textfield_paint(&ca.tf_mo);
        textfield_paint(&ca.tf_d);
        textfield_paint(&ca.tf_h);
        textfield_paint(&ca.tf_mi);
        textfield_paint(&ca.tf_s);
    }

    /* Bottom buttons */
    int bx, by, bw, bh;
    ca_btn_rect(CBTN_SET, cw, &bx, &by, &bw, &bh);
    ca_draw_button(bx, by, bw, bh, L(STR_RTC_APPLY), found, ca.btn_pressed == CBTN_SET);
    ca_btn_rect(CBTN_CLOSE, cw, &bx, &by, &bw, &bh);
    ca_draw_button(bx, by, bw, bh, L(STR_CLOSE), true, ca.btn_pressed == CBTN_CLOSE);

    wd_end();
}

/*==========================================================================
 * Create
 *=========================================================================*/

hwnd_t clock_app_create(void) {
    if (cap) return cap->hwnd;   /* already open */
    cap = (ca_state_t *)pvPortMalloc(sizeof(ca_state_t));
    if (!cap) return HWND_NULL;
    memset(&ca, 0, sizeof(ca));
    ca.test_pressed = -1;
    ca.btn_pressed = -1;

    int w = 300 + THEME_BORDER_WIDTH * 2;
    int h = CA_BTN_Y + CA_BTN_H + 12 + THEME_TITLE_HEIGHT + THEME_BORDER_WIDTH * 2;

    hwnd_t hwnd = wm_create_window(90, 50, w, h, L(STR_CLOCK), WSTYLE_DIALOG,
                                   ca_event, ca_paint);
    if (hwnd == HWND_NULL) {
        vPortFree(cap);
        cap = NULL;
        return HWND_NULL;
    }
    ca.hwnd = hwnd;

    /* Prefill pin fields from settings. */
    settings_t *cfg = settings_get();
    snprintf(ca.scl_buf, sizeof(ca.scl_buf), "%u", cfg ? cfg->rtc_scl_pin : 29);
    snprintf(ca.sda_buf, sizeof(ca.sda_buf), "%u", cfg ? cfg->rtc_sda_pin : 28);
    ca.y_buf[0] = ca.mo_buf[0] = ca.d_buf[0] = '\0';
    ca.h_buf[0] = ca.mi_buf[0] = ca.s_buf[0] = '\0';

    textfield_init(&ca.tf_scl, ca.scl_buf, sizeof(ca.scl_buf), hwnd);
    textfield_init(&ca.tf_sda, ca.sda_buf, sizeof(ca.sda_buf), hwnd);
    textfield_init(&ca.tf_y, ca.y_buf, sizeof(ca.y_buf), hwnd);
    textfield_init(&ca.tf_mo, ca.mo_buf, sizeof(ca.mo_buf), hwnd);
    textfield_init(&ca.tf_d, ca.d_buf, sizeof(ca.d_buf), hwnd);
    textfield_init(&ca.tf_h, ca.h_buf, sizeof(ca.h_buf), hwnd);
    textfield_init(&ca.tf_mi, ca.mi_buf, sizeof(ca.mi_buf), hwnd);
    textfield_init(&ca.tf_s, ca.s_buf, sizeof(ca.s_buf), hwnd);

    textfield_set_rect(&ca.tf_scl, CA_FIELD_X, CA_SCL_Y, CA_PIN_FIELD_W, CA_FIELD_H);
    textfield_set_rect(&ca.tf_sda, CA_FIELD_X, CA_SDA_Y, CA_PIN_FIELD_W, CA_FIELD_H);
    textfield_set_rect(&ca.tf_y, CA_Y_X, CA_DATE_Y, CA_Y_W, CA_FIELD_H);
    textfield_set_rect(&ca.tf_mo, CA_MO_X, CA_DATE_Y, CA_MO_W, CA_FIELD_H);
    textfield_set_rect(&ca.tf_d, CA_D_X, CA_DATE_Y, CA_D_W, CA_FIELD_H);
    textfield_set_rect(&ca.tf_h, CA_H_X, CA_TIME_Y, CA_H_W, CA_FIELD_H);
    textfield_set_rect(&ca.tf_mi, CA_MI_X, CA_TIME_Y, CA_MI_W, CA_FIELD_H);
    textfield_set_rect(&ca.tf_s, CA_S_X, CA_TIME_Y, CA_S_W, CA_FIELD_H);

    ca_build_field_list();

    /* If the chip was already detected at boot, open ready to set the time. */
    ca.rtc_found = rtc_present();
    if (ca.rtc_found)
        ca_prefill_datetime();
    ca_focus(0);

    ca.blink_timer = xTimerCreate("cablink", pdMS_TO_TICKS(500), pdTRUE, NULL,
                                  ca_blink_timer_cb);
    if (ca.blink_timer) xTimerStart(ca.blink_timer, 0);

    wm_set_focus(hwnd);
    taskbar_invalidate();
    return hwnd;
}
