/*
 * FRANK OS — Start > Run dialog
 * Copyright (c) 2025 Mikhail Matveev <xtreme@rh1.tech>
 * https://rh1.tech
 *
 * Win95-style Run dialog: bottom-left of screen, icon, text field,
 * OK / Cancel / Browse buttons, 10-command history dropdown.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "run_dialog.h"
#include "window.h"
#include "window_event.h"
#include "window_draw.h"
#include "window_theme.h"
#include "display.h"
#include "gfx.h"
#include "font.h"
#include "taskbar.h"
#include "dialog.h"
#include "file_dialog.h"
#include "app.h"
#include "file_assoc.h"
#include "cursor.h"
#include "sdcard_init.h"
#include "ff.h"
#include "terminal.h"
#include "filemanager.h"
#include "FreeRTOS.h"
#include "timers.h"
#include <string.h>
#include <stdio.h>
#include <ctype.h>

/* 32x32 dialog icons from dialog_icons.c */
extern const uint8_t icon_info_32x32[1024];

/*==========================================================================
 * Layout constants
 *
 *  +--[ Run ]--------------------------------------------------[X]--+
 *  |  [icon]  Type the name of a program, folder,                   |
 *  |  32x32   document, or FRANK OS will open it for you.           |
 *  |                                                                 |
 *  |  Open: [/fos/game                                   ][v]       |
 *  +--------------------------------------------------------------- -+
 *  |    [   OK   ]      [ Cancel ]      [ Browse...  ]              |
 *  +-----------------------------------------------------------------+
 *=========================================================================*/

#define RD_CLIENT_W     310
#define RD_CLIENT_H     104

/* Icon (32 × 32) */
#define RD_ICON_X        8
#define RD_ICON_Y        6

/* Description text (right of icon, 4 px gap) */
#define RD_TEXT_X       48
#define RD_TEXT_Y        6

/* "Open:" label and text field */
#define RD_LABEL_X       8
#define RD_LABEL_Y      42          /* vertically centred with field */
#define RD_FIELD_X      46
#define RD_FIELD_Y      38
#define RD_FIELD_W     240
#define RD_FIELD_H      20

/* Dropdown toggle button (immediately right of field) */
#define RD_DROP_BTN_X   (RD_FIELD_X + RD_FIELD_W + 2)
#define RD_DROP_BTN_W   14

/* Separator above buttons */
#define RD_SEP_Y        67

/* Bottom buttons */
#define RD_BTN_W        75
#define RD_BTN_H        23
#define RD_BTN_Y        (RD_CLIENT_H - RD_BTN_H - 8)   /* == 73 */
#define RD_BTN_OK_X      8
#define RD_BTN_CANCEL_X (RD_BTN_OK_X     + RD_BTN_W + 6)   /* == 89  */
#define RD_BTN_BROWSE_X (RD_BTN_CANCEL_X + RD_BTN_W + 6)   /* == 170 */

/* Dropdown list (drawn below field, covers buttons while open) */
#define RD_DROP_ITEM_H  14
#define RD_DROP_MAX_VIS  3          /* visible items at once */

/* Focus IDs */
#define RD_FOCUS_FIELD   0
#define RD_FOCUS_OK      1
#define RD_FOCUS_CANCEL  2
#define RD_FOCUS_BROWSE  3

/* History */
#define RD_HISTORY_MAX   5
#define RD_CMD_MAX      64

/*==========================================================================
 * Static state
 *=========================================================================*/

static hwnd_t   rd_hwnd         = HWND_NULL;
static int8_t   rd_focus        = RD_FOCUS_FIELD;
static int8_t   rd_btn_pressed  = -1;   /* -1=none 0=OK 1=Cancel 2=Browse */

/* Text field */
static char     rd_buf[RD_CMD_MAX];
static int8_t   rd_len;
static int8_t   rd_cursor;

/* Cursor blink */
static bool           rd_cursor_vis  = true;
static TimerHandle_t  rd_blink_timer = NULL;

/* History */
static char     rd_history[RD_HISTORY_MAX][RD_CMD_MAX];
static int8_t   rd_history_count = 0;

/* Dropdown */
static bool     rd_drop_open    = false;
static int8_t   rd_drop_hover   = -1;
static int8_t   rd_drop_scroll  = 0;    /* first visible item index */

/* Pending browse result */
static bool     rd_waiting_browse = false;

/* Error dialog pending (parentless — we poll) */
static bool     rd_err_pending  = false;

/* Deferred app launch — set by rd_execute(), consumed by run_dialog_check_pending()
 * so that the actual launch runs at the top of the compositor task loop rather
 * than deep inside wm_dispatch_events(), preventing stack overflow. */
#define RD_LAUNCH_NONE      0
#define RD_LAUNCH_TERMINAL  1
#define RD_LAUNCH_NAVIGATOR 2
#define RD_LAUNCH_FILE      3
static uint8_t  rd_pending_launch      = RD_LAUNCH_NONE;
static char     rd_pending_launch_path[RD_CMD_MAX];

/*==========================================================================
 * Blink timer
 *=========================================================================*/

static void rd_blink_cb(TimerHandle_t xTimer) {
    (void)xTimer;
    rd_cursor_vis = !rd_cursor_vis;
    if (rd_hwnd != HWND_NULL) wm_invalidate(rd_hwnd);
}

static void rd_blink_reset(void) {
    rd_cursor_vis = true;
    if (rd_blink_timer) xTimerReset(rd_blink_timer, 0);
    if (rd_hwnd != HWND_NULL) wm_invalidate(rd_hwnd);
}

/*==========================================================================
 * History management
 *=========================================================================*/

#define RD_HISTORY_FILE  "/fos/.run_history"

static bool rd_history_loaded = false;  /* load at most once per boot */

static void rd_history_save(void) {
    if (!sdcard_is_mounted()) return;
    FIL f;
    if (f_open(&f, RD_HISTORY_FILE, FA_WRITE | FA_CREATE_ALWAYS) != FR_OK)
        return;
    for (int i = 0; i < rd_history_count; i++) {
        UINT bw;
        f_write(&f, rd_history[i], strlen(rd_history[i]), &bw);
        f_write(&f, "\n", 1, &bw);
    }
    f_close(&f);
}

static void rd_history_load(void) {
    if (rd_history_loaded) return;
    rd_history_loaded = true;         /* only try once */
    if (!sdcard_is_mounted()) return;
    FIL f;
    if (f_open(&f, RD_HISTORY_FILE, FA_READ) != FR_OK) return;
    rd_history_count = 0;
    char line[RD_CMD_MAX + 2];
    while (rd_history_count < RD_HISTORY_MAX) {
        UINT br;
        /* Read byte-by-byte to find newlines (tiny file, no buffering needed) */
        int len = 0;
        bool eof = false;
        while (len < RD_CMD_MAX) {
            UINT r;
            char ch;
            if (f_read(&f, &ch, 1, &r) != FR_OK || r == 0) { eof = true; break; }
            if (ch == '\r') continue;
            if (ch == '\n') break;
            line[len++] = ch;
        }
        if (len > 0) {
            line[len] = '\0';
            memcpy(rd_history[rd_history_count], line, len + 1);
            rd_history_count++;
        }
        if (eof) break;
    }
    f_close(&f);
}

static void history_push(const char *cmd) {
    if (!cmd || !*cmd) return;

    /* Remove duplicate if it already exists anywhere */
    for (int i = 0; i < rd_history_count; i++) {
        if (strcmp(rd_history[i], cmd) == 0) {
            /* Shift earlier entries down, bring this one to front */
            char tmp[RD_CMD_MAX];
            strncpy(tmp, rd_history[i], RD_CMD_MAX - 1);
            tmp[RD_CMD_MAX - 1] = '\0';
            for (int j = i; j > 0; j--)
                memcpy(rd_history[j], rd_history[j - 1], RD_CMD_MAX);
            memcpy(rd_history[0], tmp, RD_CMD_MAX);
            rd_history_save();
            return;
        }
    }

    /* Shift all entries down, insert at front */
    int n = (rd_history_count < RD_HISTORY_MAX) ?
             rd_history_count : RD_HISTORY_MAX - 1;
    for (int j = n; j > 0; j--)
        memcpy(rd_history[j], rd_history[j - 1], RD_CMD_MAX);

    strncpy(rd_history[0], cmd, RD_CMD_MAX - 1);
    rd_history[0][RD_CMD_MAX - 1] = '\0';

    if (rd_history_count < RD_HISTORY_MAX) rd_history_count++;
    rd_history_save();
}

/*==========================================================================
 * Close helpers
 *=========================================================================*/

static void rd_stop_timer(void) {
    if (rd_blink_timer) {
        xTimerStop(rd_blink_timer, 0);
        xTimerDelete(rd_blink_timer, 0);
        rd_blink_timer = NULL;
    }
}

static void rd_close(void) {
    rd_drop_open   = false;
    rd_err_pending = false;
    wm_clear_modal();
    wm_destroy_window(rd_hwnd);
    rd_hwnd = HWND_NULL;
    rd_stop_timer();
    wm_force_full_repaint();
}

/*==========================================================================
 * Resolve path: returns resolved absolute path in out_path (RD_CMD_MAX).
 * Returns true if the file was found.
 *=========================================================================*/

static bool rd_resolve_path(const char *cmd, char *out_path) {
    FILINFO fno;

    /* Absolute path — use as-is */
    if (cmd[0] == '/') {
        strncpy(out_path, cmd, RD_CMD_MAX - 1);
        out_path[RD_CMD_MAX - 1] = '\0';
        return (f_stat(out_path, &fno) == FR_OK &&
                !(fno.fattrib & AM_DIR));
    }

    /* Try /fos/<cmd> */
    snprintf(out_path, RD_CMD_MAX, "/fos/%s", cmd);
    if (f_stat(out_path, &fno) == FR_OK && !(fno.fattrib & AM_DIR))
        return true;

    /* Try current path as-is (relative) */
    strncpy(out_path, cmd, RD_CMD_MAX - 1);
    out_path[RD_CMD_MAX - 1] = '\0';
    return (f_stat(out_path, &fno) == FR_OK && !(fno.fattrib & AM_DIR));
}

/*==========================================================================
 * Execute command
 *=========================================================================*/

static void rd_execute(void) {
    if (rd_len == 0) return;

    /* Check built-in commands first (case-insensitive) */
    {
        char lc[RD_CMD_MAX];
        for (int i = 0; i <= rd_len; i++)
            lc[i] = (char)tolower((unsigned char)rd_buf[i]);

        if (strcmp(lc, "terminal") == 0) {
            history_push(rd_buf);
            rd_pending_launch = RD_LAUNCH_TERMINAL;
            rd_close();
            return;
        }
        if (strcmp(lc, "navigator") == 0) {
            history_push(rd_buf);
            rd_pending_launch = RD_LAUNCH_NAVIGATOR;
            rd_close();
            return;
        }
    }

    char path[RD_CMD_MAX];

    if (!sdcard_is_mounted()) {
        dialog_show(HWND_NULL, "Run",
                    "No SD card mounted.\n"
                    "Please insert an SD card\n"
                    "and try again.",
                    DLG_ICON_ERROR, DLG_BTN_OK);
        rd_err_pending = true;
        return;
    }

    if (!rd_resolve_path(rd_buf, path)) {
        char msg[128];
        snprintf(msg, sizeof(msg),
                 "Cannot find '%s'.\n\n"
                 "Make sure the name is correct,\n"
                 "then try again.",
                 rd_buf);
        dialog_show(HWND_NULL, "Run", msg, DLG_ICON_ERROR, DLG_BTN_OK);
        rd_err_pending = true;
        return;
    }

    /* Success — save path for deferred launch, close dialog.
     * The actual file_assoc_open()/launch_elf_app() call happens from
     * run_dialog_check_pending() at the top of the compositor loop,
     * not from inside wm_dispatch_events(), to avoid stack overflow. */
    history_push(rd_buf);
    rd_drop_open = false;
    strncpy(rd_pending_launch_path, path, RD_CMD_MAX - 1);
    rd_pending_launch_path[RD_CMD_MAX - 1] = '\0';
    rd_pending_launch = RD_LAUNCH_FILE;
    rd_close();
}

/*==========================================================================
 * Paint handler
 *=========================================================================*/

/* Helper: draw one sunken field border + white interior in client coords */
static void rd_draw_field_border(int fx, int fy, int fw, int fh) {
    wd_hline((int16_t)fx, (int16_t)fy, (int16_t)fw, COLOR_DARK_GRAY);
    wd_vline((int16_t)fx, (int16_t)fy, (int16_t)fh, COLOR_DARK_GRAY);
    wd_hline((int16_t)(fx + 1), (int16_t)(fy + 1),
             (int16_t)(fw - 2), COLOR_BLACK);
    wd_vline((int16_t)(fx + 1), (int16_t)(fy + 1),
             (int16_t)(fh - 2), COLOR_BLACK);
    wd_hline((int16_t)fx, (int16_t)(fy + fh - 1), (int16_t)fw, COLOR_WHITE);
    wd_vline((int16_t)(fx + fw - 1), (int16_t)fy, (int16_t)fh, COLOR_WHITE);
    wd_hline((int16_t)(fx + 1), (int16_t)(fy + fh - 2),
             (int16_t)(fw - 2), THEME_BUTTON_FACE);
    wd_vline((int16_t)(fx + fw - 2), (int16_t)(fy + 1),
             (int16_t)(fh - 2), THEME_BUTTON_FACE);
    wd_fill_rect((int16_t)(fx + 2), (int16_t)(fy + 2),
                 (int16_t)(fw - 4), (int16_t)(fh - 4), COLOR_WHITE);
}

static void rd_paint(hwnd_t hwnd) {
    window_t *win = wm_get_window(hwnd);
    if (!win) return;

    /* Cache screen origin for direct-pixel drawing (icon) */
    point_t co = theme_client_origin(&win->frame, win->flags);

    /* ------------------------------------------------------------------ */
    /* Icon — draw directly using display_set_pixel (32×32, transparent)  */
    /* ------------------------------------------------------------------ */
    for (int row = 0; row < 32; row++) {
        for (int col = 0; col < 32; col++) {
            uint8_t c = icon_info_32x32[row * 32 + col];
            if (c == 0xFF) continue;
            int sx = co.x + RD_ICON_X + col;
            int sy = co.y + RD_ICON_Y + row;
            if ((unsigned)sx < (unsigned)DISPLAY_WIDTH &&
                (unsigned)sy < (unsigned)FB_HEIGHT)
                display_set_pixel(sx, sy, c);
        }
    }

    /* ------------------------------------------------------------------ */
    /* Description text (two lines, right of icon)                        */
    /* ------------------------------------------------------------------ */
    wd_text_ui(RD_TEXT_X, RD_TEXT_Y,
               "Type the name of a program, folder,",
               COLOR_BLACK, THEME_BUTTON_FACE);
    wd_text_ui(RD_TEXT_X, RD_TEXT_Y + FONT_UI_HEIGHT,
               "document, and FRANK OS will open it.",
               COLOR_BLACK, THEME_BUTTON_FACE);

    /* ------------------------------------------------------------------ */
    /* "Open:" label                                                       */
    /* ------------------------------------------------------------------ */
    wd_text_ui(RD_LABEL_X, RD_LABEL_Y,
               "Open:", COLOR_BLACK, THEME_BUTTON_FACE);

    /* ------------------------------------------------------------------ */
    /* Combined combobox: single sunken border, text area left, arrow btn */
    /* right — both share the same border (Win95 combo style)             */
    /* ------------------------------------------------------------------ */
    rd_draw_field_border(RD_FIELD_X, RD_FIELD_Y,
                         RD_FIELD_W + 2 + RD_DROP_BTN_W, RD_FIELD_H);
    {
        /* Arrow button area — gray background inside the shared border */
        int bx = RD_DROP_BTN_X;         /* right of text zone + 2px gap  */
        int bw = RD_DROP_BTN_W - 2;     /* minus right border pixels     */
        int by = RD_FIELD_Y + 2;
        int bh = RD_FIELD_H - 4;
        wd_fill_rect((int16_t)bx, (int16_t)by,
                     (int16_t)bw, (int16_t)bh, THEME_BUTTON_FACE);
        /* Vertical separator between text area and arrow */
        wd_vline((int16_t)(bx - 1), (int16_t)by,
                 (int16_t)bh, COLOR_DARK_GRAY);
    }

    /* Text content — draw at screen coords so we can place cursor */
    {
        int ttx = co.x + RD_FIELD_X + 4;
        int tty = co.y + RD_FIELD_Y + (RD_FIELD_H - FONT_UI_HEIGHT) / 2;
        gfx_text_ui(ttx, tty, rd_buf, COLOR_BLACK, COLOR_WHITE);

        /* Blinking cursor when text field has focus and dropdown is closed */
        if (rd_focus == RD_FOCUS_FIELD && rd_cursor_vis && !rd_drop_open) {
            int cx = ttx + rd_cursor * FONT_UI_WIDTH;
            int cy = tty;
            for (int row = 0; row < FONT_UI_HEIGHT; row++)
                if ((unsigned)(cy + row) < (unsigned)FB_HEIGHT)
                    display_set_pixel(cx, cy + row, COLOR_BLACK);
        }
    }

    /* Down-arrow triangle centred in the button area (drawn pixel-directly
     * because the \x1F glyph in the UI font is nearly invisible).
     * Shape (3-row triangle, apex at bottom):
     *   #####    row cy-1  (5 px)
     *    ###     row cy    (3 px)
     *     #      row cy+1  (1 px)           */
    {
        int bx = RD_DROP_BTN_X;
        int bw = RD_DROP_BTN_W - 2;
        int by = RD_FIELD_Y + 2;
        int bh = RD_FIELD_H - 4;
        int cx = bx + bw / 2;
        int cy = by + bh / 2;
        wd_hline((int16_t)(cx - 2), (int16_t)(cy - 1), 5, COLOR_BLACK);
        wd_hline((int16_t)(cx - 1), (int16_t)(cy),     3, COLOR_BLACK);
        wd_pixel((int16_t)(cx),     (int16_t)(cy + 1),    COLOR_BLACK);
    }

    /* ------------------------------------------------------------------ */
    /* When dropdown is open: draw list (covers separator + buttons)       */
    /* When closed: draw separator + buttons                               */
    /* ------------------------------------------------------------------ */
    if (rd_drop_open && rd_history_count > 0) {
        int vis  = rd_history_count < RD_DROP_MAX_VIS ?
                   rd_history_count : RD_DROP_MAX_VIS;
        int dl_x = RD_FIELD_X;
        int dl_y = RD_FIELD_Y + RD_FIELD_H;        /* just below field */
        int dl_w = RD_FIELD_W + RD_DROP_BTN_W + 2;
        int dl_h = vis * RD_DROP_ITEM_H + 2;

        /* Border + white background */
        wd_fill_rect((int16_t)dl_x, (int16_t)dl_y,
                     (int16_t)dl_w, (int16_t)dl_h, COLOR_WHITE);
        wd_hline((int16_t)dl_x, (int16_t)dl_y, (int16_t)dl_w, COLOR_DARK_GRAY);
        wd_hline((int16_t)dl_x, (int16_t)(dl_y + dl_h - 1),
                 (int16_t)dl_w, COLOR_DARK_GRAY);
        wd_vline((int16_t)dl_x, (int16_t)dl_y, (int16_t)dl_h, COLOR_DARK_GRAY);
        wd_vline((int16_t)(dl_x + dl_w - 1), (int16_t)dl_y,
                 (int16_t)dl_h, COLOR_DARK_GRAY);

        /* Items */
        for (int i = 0; i < vis; i++) {
            int idx    = rd_drop_scroll + i;
            if (idx >= rd_history_count) break;
            bool hov   = (rd_drop_hover == idx);
            uint8_t bg = hov ? COLOR_BLUE  : COLOR_WHITE;
            uint8_t fg = hov ? COLOR_WHITE : COLOR_BLACK;
            int iy = dl_y + 1 + i * RD_DROP_ITEM_H;

            wd_fill_rect((int16_t)(dl_x + 1), (int16_t)iy,
                         (int16_t)(dl_w - 2), RD_DROP_ITEM_H, bg);
            wd_text_ui((int16_t)(dl_x + 4),
                       (int16_t)(iy + (RD_DROP_ITEM_H - FONT_UI_HEIGHT) / 2),
                       rd_history[idx], fg, bg);
        }

    } else {
        /* Separator */
        wd_hline(0, RD_SEP_Y, RD_CLIENT_W, COLOR_DARK_GRAY);
        wd_hline(0, RD_SEP_Y + 1, RD_CLIENT_W, COLOR_WHITE);

        /* Buttons */
        wd_button(RD_BTN_OK_X,     RD_BTN_Y, RD_BTN_W, RD_BTN_H,
                  "OK",
                  rd_focus == RD_FOCUS_OK,     rd_btn_pressed == 0);
        wd_button(RD_BTN_CANCEL_X, RD_BTN_Y, RD_BTN_W, RD_BTN_H,
                  "Cancel",
                  rd_focus == RD_FOCUS_CANCEL, rd_btn_pressed == 1);
        wd_button(RD_BTN_BROWSE_X, RD_BTN_Y, RD_BTN_W, RD_BTN_H,
                  "Browse...",
                  rd_focus == RD_FOCUS_BROWSE, rd_btn_pressed == 2);
    }
}

/*==========================================================================
 * Hit-test: client-coord point inside the dropdown list?
 * Returns history index (0..n-1) or -1 if outside.
 *=========================================================================*/

static int rd_drop_hittest(int mx, int my) {
    if (!rd_drop_open || rd_history_count == 0) return -1;

    int vis  = rd_history_count < RD_DROP_MAX_VIS ?
               rd_history_count : RD_DROP_MAX_VIS;
    int dl_x = RD_FIELD_X;
    int dl_y = RD_FIELD_Y + RD_FIELD_H;
    int dl_w = RD_FIELD_W + RD_DROP_BTN_W + 2;
    int dl_h = vis * RD_DROP_ITEM_H + 2;

    if (mx < dl_x || mx >= dl_x + dl_w) return -1;
    if (my < dl_y || my >= dl_y + dl_h) return -1;

    int rel  = my - dl_y - 1;
    if (rel < 0) return -1;
    int idx  = rd_drop_scroll + rel / RD_DROP_ITEM_H;
    if (idx >= rd_history_count) return -1;
    return idx;
}

/*==========================================================================
 * Select a history item: fill text field and close dropdown
 *=========================================================================*/

static void rd_history_select(int idx) {
    if (idx < 0 || idx >= rd_history_count) return;
    strncpy(rd_buf, rd_history[idx], RD_CMD_MAX - 1);
    rd_buf[RD_CMD_MAX - 1] = '\0';
    rd_len    = (int8_t)strlen(rd_buf);
    rd_cursor = rd_len;
    rd_drop_open  = false;
    rd_drop_hover = -1;
    rd_focus      = RD_FOCUS_FIELD;
    wm_force_full_repaint();
}

/*==========================================================================
 * Event handler
 *=========================================================================*/

static bool rd_event(hwnd_t hwnd, const window_event_t *ev) {
    (void)hwnd;

    switch (ev->type) {

    /* ------------------------------------------------------------------ */
    case WM_CLOSE:
        rd_close();
        return true;

    /* ------------------------------------------------------------------ */
    /* File dialog posts WM_COMMAND to us when the user picks a file      */
    case WM_COMMAND:
        if (ev->command.id == DLG_RESULT_FILE && rd_waiting_browse) {
            rd_waiting_browse = false;
            const char *sel = file_dialog_get_path();
            if (sel && *sel) {
                strncpy(rd_buf, sel, RD_CMD_MAX - 1);
                rd_buf[RD_CMD_MAX - 1] = '\0';
                rd_len    = (int8_t)strlen(rd_buf);
                rd_cursor = rd_len;
            }
            /* Re-claim modal + focus */
            wm_set_modal(rd_hwnd);
            wm_set_focus(rd_hwnd);
            rd_focus = RD_FOCUS_FIELD;
            wm_invalidate(rd_hwnd);
            return true;
        }
        return false;

    /* ------------------------------------------------------------------ */
    case WM_CHAR: {
        if (rd_focus != RD_FOCUS_FIELD || rd_drop_open) return true;
        char ch = ev->charev.ch;
        if (ch >= 0x20 && ch < 0x7F && rd_len < RD_CMD_MAX - 1) {
            /* Insert at cursor */
            for (int i = rd_len; i > rd_cursor; i--)
                rd_buf[i] = rd_buf[i - 1];
            rd_buf[rd_cursor++] = ch;
            rd_buf[++rd_len]    = '\0';   /* overwritten above then '\0' */
            /* Correct double-increment: undo one */
            rd_len--;
            rd_len = (int8_t)strlen(rd_buf); /* recount */
            rd_cursor = (int8_t)(rd_cursor); /* already incremented */
            rd_blink_reset();
        }
        return true;
    }

    /* ------------------------------------------------------------------ */
    case WM_KEYDOWN: {
        uint8_t sc = ev->key.scancode;

        /* Escape: close dropdown if open, otherwise close dialog */
        if (sc == 0x29) {
            if (rd_drop_open) {
                rd_drop_open  = false;
                rd_drop_hover = -1;
                wm_force_full_repaint();
            } else {
                rd_close();
            }
            return true;
        }

        /* Enter: accept highlighted dropdown item or activate focused ctrl */
        if (sc == 0x28) {
            if (rd_drop_open) {
                if (rd_drop_hover >= 0) {
                    rd_history_select(rd_drop_hover);
                } else {
                    /* Dropdown open but nothing highlighted — just close it */
                    rd_drop_open  = false;
                    rd_drop_hover = -1;
                    wm_force_full_repaint();
                }
            } else {
                rd_drop_open = false;
                switch (rd_focus) {
                case RD_FOCUS_FIELD:
                case RD_FOCUS_OK:
                    rd_execute();
                    break;
                case RD_FOCUS_CANCEL:
                    rd_close();
                    break;
                case RD_FOCUS_BROWSE:
                    goto do_browse;
                }
            }
            return true;
        }

        /* Tab: cycle focus (close dropdown first) */
        if (sc == 0x2B) {
            if (rd_drop_open) {
                rd_drop_open  = false;
                rd_drop_hover = -1;
                wm_force_full_repaint();
            }
            switch (rd_focus) {
            case RD_FOCUS_FIELD:  rd_focus = RD_FOCUS_OK;     break;
            case RD_FOCUS_OK:     rd_focus = RD_FOCUS_CANCEL; break;
            case RD_FOCUS_CANCEL: rd_focus = RD_FOCUS_BROWSE; break;
            case RD_FOCUS_BROWSE: rd_focus = RD_FOCUS_FIELD;  break;
            }
            wm_invalidate(rd_hwnd);
            return true;
        }

        /* Up / Down: navigate dropdown history
         *
         * Down from closed field → open the list, no item pre-selected.
         *   Each subsequent Down moves the highlight: -1 → 0 → 1 → 2 ...
         *
         * Up from open list:
         *   hover > 0   → move highlight up.
         *   hover == 0  → deselect (hover = -1); field text unchanged.
         *   hover == -1 → close dropdown without selecting anything.
         *
         * wm_force_full_repaint is used so the button row refreshes when
         * the dropdown appears/disappears over it.
         */
        if (sc == 0x52) { /* Up */
            if (rd_drop_open) {
                if (rd_drop_hover > 0) {
                    rd_drop_hover--;
                    if (rd_drop_hover < rd_drop_scroll)
                        rd_drop_scroll = rd_drop_hover;
                } else if (rd_drop_hover == 0) {
                    rd_drop_hover = -1;   /* deselect; next Up closes list */
                } else {
                    /* hover == -1: close dropdown, keep typed text */
                    rd_drop_open  = false;
                    rd_drop_hover = -1;
                }
                wm_force_full_repaint();
            }
            /* Up when list is closed does nothing */
            return true;
        }
        if (sc == 0x51) { /* Down */
            if (rd_drop_open) {
                /* Advance highlight, but don't go past last item */
                if (rd_drop_hover < rd_history_count - 1) {
                    rd_drop_hover++;
                    int vis = rd_history_count < RD_DROP_MAX_VIS ?
                              rd_history_count : RD_DROP_MAX_VIS;
                    if (rd_drop_hover >= rd_drop_scroll + vis)
                        rd_drop_scroll = rd_drop_hover - vis + 1;
                }
                wm_force_full_repaint();
            } else if (rd_focus == RD_FOCUS_FIELD && rd_history_count > 0) {
                /* Open list with no item pre-selected */
                rd_drop_open   = true;
                rd_drop_hover  = -1;
                rd_drop_scroll = 0;
                wm_force_full_repaint();
            }
            return true;
        }

        /* Text-field editing keys (only when field focused and drop closed) */
        if (rd_focus == RD_FOCUS_FIELD && !rd_drop_open) {
            switch (sc) {
            case 0x2A: /* Backspace */
                if (rd_cursor > 0) {
                    for (int i = rd_cursor - 1; i < rd_len - 1; i++)
                        rd_buf[i] = rd_buf[i + 1];
                    rd_buf[--rd_len] = '\0';
                    rd_cursor--;
                    rd_blink_reset();
                }
                return true;
            case 0x4C: /* Delete */
                if (rd_cursor < rd_len) {
                    for (int i = rd_cursor; i < rd_len - 1; i++)
                        rd_buf[i] = rd_buf[i + 1];
                    rd_buf[--rd_len] = '\0';
                    rd_blink_reset();
                }
                return true;
            case 0x50: /* Left */
                if (rd_cursor > 0) { rd_cursor--; rd_blink_reset(); }
                return true;
            case 0x4F: /* Right */
                if (rd_cursor < rd_len) { rd_cursor++; rd_blink_reset(); }
                return true;
            case 0x4A: /* Home */
                rd_cursor = 0; rd_blink_reset();
                return true;
            case 0x4D: /* End */
                rd_cursor = rd_len; rd_blink_reset();
                return true;
            }
        }

        return false;
    }

    /* ------------------------------------------------------------------ */
    case WM_LBUTTONDOWN: {
        int mx = ev->mouse.x;
        int my = ev->mouse.y;
        rd_btn_pressed = -1;

        /* Dropdown list has priority when open */
        {
            int hit = rd_drop_hittest(mx, my);
            if (hit >= 0) {
                rd_drop_hover = (int8_t)hit;
                wm_invalidate(rd_hwnd);
                return true;
            }
        }

        /* Dropdown toggle button */
        if (mx >= RD_DROP_BTN_X && mx < RD_DROP_BTN_X + RD_DROP_BTN_W &&
            my >= RD_FIELD_Y    && my < RD_FIELD_Y + RD_FIELD_H) {
            rd_drop_open  = !rd_drop_open;
            rd_drop_hover = -1;
            rd_drop_scroll = 0;
            wm_force_full_repaint();
            return true;
        }

        /* Text field click: move cursor */
        if (mx >= RD_FIELD_X + 2 && mx < RD_FIELD_X + RD_FIELD_W - 2 &&
            my >= RD_FIELD_Y     && my < RD_FIELD_Y + RD_FIELD_H) {
            rd_focus = RD_FOCUS_FIELD;
            int pos  = (mx - RD_FIELD_X - 4) / FONT_UI_WIDTH;
            if (pos < 0)       pos = 0;
            if (pos > rd_len)  pos = rd_len;
            rd_cursor = (int8_t)pos;
            if (rd_drop_open) {
                rd_drop_open = false;
                wm_force_full_repaint();
            } else {
                rd_blink_reset();
            }
            return true;
        }

        /* Button hit-test (only when dropdown closed) */
        if (!rd_drop_open &&
            my >= RD_BTN_Y && my < RD_BTN_Y + RD_BTN_H) {
            if (mx >= RD_BTN_OK_X && mx < RD_BTN_OK_X + RD_BTN_W) {
                rd_btn_pressed = 0; rd_focus = RD_FOCUS_OK;
                wm_invalidate(rd_hwnd); return true;
            }
            if (mx >= RD_BTN_CANCEL_X && mx < RD_BTN_CANCEL_X + RD_BTN_W) {
                rd_btn_pressed = 1; rd_focus = RD_FOCUS_CANCEL;
                wm_invalidate(rd_hwnd); return true;
            }
            if (mx >= RD_BTN_BROWSE_X && mx < RD_BTN_BROWSE_X + RD_BTN_W) {
                rd_btn_pressed = 2; rd_focus = RD_FOCUS_BROWSE;
                wm_invalidate(rd_hwnd); return true;
            }
        }

        /* Click anywhere else — close dropdown if open */
        if (rd_drop_open) {
            rd_drop_open  = false;
            rd_drop_hover = -1;
            wm_force_full_repaint();
        }
        return true;
    }

    /* ------------------------------------------------------------------ */
    case WM_LBUTTONUP: {
        if (rd_btn_pressed < 0) {
            /* Released without prior press — check dropdown selection */
            if (rd_drop_open) {
                int hit = rd_drop_hittest(ev->mouse.x, ev->mouse.y);
                if (hit >= 0) rd_history_select(hit);
            }
            return true;
        }

        int pressed        = rd_btn_pressed;
        rd_btn_pressed     = -1;
        int mx             = ev->mouse.x;
        int my             = ev->mouse.y;
        bool in_row        = (my >= RD_BTN_Y && my < RD_BTN_Y + RD_BTN_H);

        switch (pressed) {
        case 0: /* OK */
            if (in_row && mx >= RD_BTN_OK_X &&
                mx < RD_BTN_OK_X + RD_BTN_W)
                rd_execute();
            else
                wm_invalidate(rd_hwnd);
            break;
        case 1: /* Cancel */
            if (in_row && mx >= RD_BTN_CANCEL_X &&
                mx < RD_BTN_CANCEL_X + RD_BTN_W)
                rd_close();
            else
                wm_invalidate(rd_hwnd);
            break;
        case 2: /* Browse */
            if (in_row && mx >= RD_BTN_BROWSE_X &&
                mx < RD_BTN_BROWSE_X + RD_BTN_W) {
do_browse:
                /* Release modal so the file dialog can take over */
                wm_clear_modal();
                rd_waiting_browse = true;
                file_dialog_open(rd_hwnd, "Browse",
                                 "/fos", NULL);
            } else {
                wm_invalidate(rd_hwnd);
            }
            break;
        }
        return true;
    }

    /* ------------------------------------------------------------------ */
    case WM_MOUSEMOVE: {
        if (!rd_drop_open) return false;
        int hit = rd_drop_hittest(ev->mouse.x, ev->mouse.y);
        if (hit != rd_drop_hover) {
            rd_drop_hover = (int8_t)hit;
            wm_invalidate(rd_hwnd);
        }
        return false;
    }

    default:
        return false;
    }
}

/*==========================================================================
 * Public API
 *=========================================================================*/

void run_dialog_open(void) {
    /* Load history from SD card on first ever open */
    rd_history_load();

    if (rd_hwnd != HWND_NULL) {
        wm_set_focus(rd_hwnd);  /* already open — bring to front */
        return;
    }

    /* Reset UI state (preserve history and last typed text) */
    rd_focus        = RD_FOCUS_FIELD;
    rd_btn_pressed  = -1;
    rd_drop_open    = false;
    rd_drop_hover   = -1;
    rd_drop_scroll  = 0;
    rd_waiting_browse = false;
    rd_err_pending  = false;
    rd_cursor_vis   = true;

    /* Cursor at end of last command */
    rd_len    = (int8_t)strlen(rd_buf);
    rd_cursor = rd_len;

    int outer_w = RD_CLIENT_W + 2 * THEME_BORDER_WIDTH;
    int outer_h = RD_CLIENT_H + THEME_TITLE_HEIGHT + 2 * THEME_BORDER_WIDTH;

    /* Position: bottom-left corner, just above the taskbar */
    int16_t px = 4;
    int16_t py = (int16_t)(TASKBAR_Y - outer_h - 4);
    if (py < 0) py = 0;

    rd_hwnd = wm_create_window(px, py,
                                (int16_t)outer_w, (int16_t)outer_h,
                                "Run",
                                WSTYLE_DIALOG,
                                rd_event, rd_paint);
    if (rd_hwnd == HWND_NULL) return;

    window_t *win = wm_get_window(rd_hwnd);
    if (win) win->bg_color = THEME_BUTTON_FACE;

    wm_set_focus(rd_hwnd);
    wm_set_modal(rd_hwnd);

    /* Start cursor blink (500 ms auto-reload) */
    rd_blink_timer = xTimerCreate("rdblink", pdMS_TO_TICKS(500),
                                   pdTRUE, NULL, rd_blink_cb);
    if (rd_blink_timer) xTimerStart(rd_blink_timer, 0);
}

bool run_dialog_is_open(void) {
    return rd_hwnd != HWND_NULL;
}

/* Overlay draw is no longer needed since dropdown is inside the window,
 * but the symbol is kept for the header contract. */
void run_dialog_draw_dropdown(void) {
    (void)0;
}

void run_dialog_check_pending(void) {
    /* Deferred app launch — executed here (compositor top loop) to avoid
     * stack overflow that occurs when called from inside wm_dispatch_events(). */
    if (rd_pending_launch != RD_LAUNCH_NONE) {
        uint8_t what = rd_pending_launch;
        rd_pending_launch = RD_LAUNCH_NONE;
        if (what == RD_LAUNCH_TERMINAL) {
            spawn_terminal_window();
        } else if (what == RD_LAUNCH_NAVIGATOR) {
            spawn_filemanager_window();
        } else if (what == RD_LAUNCH_FILE) {
            cursor_set_type(CURSOR_WAIT);
            wm_composite();
            if (!file_assoc_open(rd_pending_launch_path))
                launch_elf_app(rd_pending_launch_path);
            cursor_set_type(CURSOR_ARROW);
        }
        return;
    }

    if (!rd_err_pending) return;
    uint16_t r = dialog_poll_result();
    if (r == 0) return;   /* error dialog still visible */

    /* Error dialog dismissed — return focus to Run dialog (if still open) */
    rd_err_pending = false;
    if (rd_hwnd != HWND_NULL) {
        wm_set_modal(rd_hwnd);
        wm_set_focus(rd_hwnd);
        wm_invalidate(rd_hwnd);
    }
}
