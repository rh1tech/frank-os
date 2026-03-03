/*
 * FRANK OS — System File Dialog
 * Copyright (c) 2025 Mikhail Matveev <xtreme@rh1.tech>
 * https://rh1.tech
 *
 * Modal file browser dialog following the dialog.c pattern.
 * Single dialog at a time, static state, Win95-style UI.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "file_dialog.h"
#include "window.h"
#include "window_event.h"
#include "window_draw.h"
#include "window_theme.h"
#include "display.h"
#include "gfx.h"
#include "font.h"
#include "taskbar.h"
#include "dialog.h"          /* DLG_RESULT_CANCEL */
#include "ff.h"
#include <string.h>
#include <stdio.h>
#include "FreeRTOS.h"
#include "task.h"
#include "timers.h"

/* Up-directory icon from Navigator (fn_icons.c) */
extern const uint8_t fn_icon16_up[];

/*==========================================================================
 * Constants
 *=========================================================================*/

#define FD_MAX_ENTRIES  48
#define FD_MAX_VISIBLE  10
#define FD_PATH_MAX    256
#define FD_NAME_LEN     32

/* Layout — Win95 "Open" style
 *
 * Client area: 320 x 250
 * +------------------------------------------------------+
 * | Look in: [/fos/games                        ] [ Up ] |
 * | +----------------------------------------------+-+-+ |
 * | | [folder] subdir                               |^| | |
 * | | [file]   game1.tap                            | | | |
 * | | [file]   game2.tap                            |v| | |
 * | +----------------------------------------------+-+-+ |
 * | File name:    [game1.tap              ] [ Open  ]    |
 * | Files of type:[TAP Files (*.tap)      ] [ Cancel]    |
 * +------------------------------------------------------+
 */

#define FD_CLIENT_W    320
#define FD_CLIENT_H    250
#define FD_PAD           8

/* "Look in:" row */
#define FD_LOOKIN_Y      6
#define FD_PATH_FIELD_X  62
#define FD_PATH_FIELD_Y  4
#define FD_PATH_FIELD_H  22
#define FD_UP_BTN_W      24
#define FD_UP_BTN_H      22
#define FD_UP_BTN_X      (FD_CLIENT_W - FD_PAD - FD_UP_BTN_W)
#define FD_PATH_FIELD_W  (FD_UP_BTN_X - 4 - FD_PATH_FIELD_X)

/* File list */
#define FD_LIST_X        FD_PAD
#define FD_LIST_Y        30
#define FD_ROW_H         16
#define FD_LIST_INNER_H  (FD_MAX_VISIBLE * FD_ROW_H)
#define FD_LIST_H        (FD_LIST_INNER_H + 4)
#define FD_LIST_W        (FD_CLIENT_W - 2 * FD_PAD)

/* Scrollbar (inside list bevel, right side) */
#define FD_SB_W          16
#define FD_SB_X          (FD_LIST_X + FD_LIST_W - 2 - FD_SB_W)
#define FD_SB_Y          (FD_LIST_Y + 2)
#define FD_SB_H          FD_LIST_INNER_H

/* Content area (inside list, left of scrollbar) */
#define FD_CT_X          (FD_LIST_X + 2)
#define FD_CT_Y          (FD_LIST_Y + 2)
#define FD_CT_W          (FD_SB_X - FD_CT_X)
#define FD_CT_H          FD_LIST_INNER_H

/* Icons */
#define FD_ICON_W        16

/* Bottom section: fields left, buttons right */
#define FD_BTN_W         75
#define FD_BTN_H         23
#define FD_BTN_X         (FD_CLIENT_W - FD_PAD - FD_BTN_W)
#define FD_FIELD_X       96
#define FD_FIELD_W       (FD_BTN_X - 6 - FD_FIELD_X)
#define FD_FIELD_H       20
#define FD_FNAME_Y       (FD_LIST_Y + FD_LIST_H + 6)
#define FD_FTYPE_Y       (FD_FNAME_Y + FD_BTN_H + 3)

/* Double-click threshold */
#define FD_DBLCLICK_MS   400

/*==========================================================================
 * Static state (single dialog at a time)
 *=========================================================================*/

static hwnd_t   fd_hwnd   = HWND_NULL;
static hwnd_t   fd_parent = HWND_NULL;
static char     fd_path[FD_PATH_MAX];
static char     fd_result[FD_PATH_MAX];
static char     fd_filter[24];
static char     fd_names[FD_MAX_ENTRIES][FD_NAME_LEN];
static uint8_t  fd_is_dir[FD_MAX_ENTRIES];
static int16_t  fd_count;
static int16_t  fd_selected;
static int16_t  fd_scroll;
static int8_t   fd_btn_focus;   /* 0 = list, 1 = Open, 2 = Cancel */
static int8_t   fd_btn_pressed; /* -1 = none, 0 = Open, 1 = Cancel */
static bool     fd_up_pressed;  /* Up button press animation */

/* Double-click state */
static uint32_t fd_last_click_tick;
static int16_t  fd_last_click_idx;

/* Save mode state */
static bool     fd_save_mode;
static char     fd_filename[FD_NAME_LEN];   /* editable filename */
static uint8_t  fd_fname_len;
static uint8_t  fd_fname_cursor;
static bool     fd_fname_focus;             /* true = filename field focused */
static bool     fd_cursor_visible;
static TimerHandle_t fd_blink_timer = NULL;

/* Overwrite confirmation pending state */
static bool     fd_overwrite_pending;

/* Scrollbar thumb drag state */
static bool     fd_sb_dragging;
static int16_t  fd_sb_drag_offset;   /* mouse Y relative to thumb top */

/*==========================================================================
 * Extension matching (case-insensitive)
 *=========================================================================*/

static bool fd_match_ext(const char *name) {
    if (fd_filter[0] == '\0') return true;
    const char *dot = strrchr(name, '.');
    if (!dot) return false;
    /* Check each semicolon-separated extension in fd_filter */
    const char *f = fd_filter;
    while (*f) {
        const char *end = f;
        while (*end && *end != ';') end++;
        /* Case-insensitive compare of dot against f..end */
        const char *a = dot;
        const char *b = f;
        bool match = true;
        while (b < end && *a) {
            char ca = *a, cb = *b;
            if (ca >= 'A' && ca <= 'Z') ca += 32;
            if (cb >= 'A' && cb <= 'Z') cb += 32;
            if (ca != cb) { match = false; break; }
            a++; b++;
        }
        if (match && b == end && *a == '\0') return true;
        f = (*end == ';') ? end + 1 : end;
    }
    return false;
}

/*==========================================================================
 * Sorting helpers
 *=========================================================================*/

/* Case-insensitive strcmp for sorting */
static int fd_stricmp(const char *a, const char *b) {
    while (*a && *b) {
        char ca = *a, cb = *b;
        if (ca >= 'A' && ca <= 'Z') ca += 32;
        if (cb >= 'A' && cb <= 'Z') cb += 32;
        if (ca != cb) return ca - cb;
        a++; b++;
    }
    return (unsigned char)*a - (unsigned char)*b;
}

/* Insertion sort a range [start, end) of fd_names/fd_is_dir */
static void fd_sort_range(int start, int end) {
    char tmp_name[FD_NAME_LEN];
    for (int i = start + 1; i < end; i++) {
        memcpy(tmp_name, fd_names[i], FD_NAME_LEN);
        uint8_t tmp_dir = fd_is_dir[i];
        int j = i - 1;
        while (j >= start && fd_stricmp(fd_names[j], tmp_name) > 0) {
            memcpy(fd_names[j + 1], fd_names[j], FD_NAME_LEN);
            fd_is_dir[j + 1] = fd_is_dir[j];
            j--;
        }
        memcpy(fd_names[j + 1], tmp_name, FD_NAME_LEN);
        fd_is_dir[j + 1] = tmp_dir;
    }
}

/*==========================================================================
 * Read directory with FatFS
 *=========================================================================*/

static void fd_read_dir(void) {
    fd_count = 0;
    fd_selected = 0;
    fd_scroll = 0;

    /* ".." entry unless at root */
    if (fd_path[0] != '\0' && !(fd_path[0] == '/' && fd_path[1] == '\0')) {
        strncpy(fd_names[0], "..", FD_NAME_LEN - 1);
        fd_names[0][FD_NAME_LEN - 1] = '\0';
        fd_is_dir[0] = 1;
        fd_count = 1;
    }

    DIR dir;
    FILINFO fno;
    if (f_opendir(&dir, fd_path) != FR_OK) return;

    /* First pass: directories */
    while (fd_count < FD_MAX_ENTRIES) {
        if (f_readdir(&dir, &fno) != FR_OK || fno.fname[0] == 0)
            break;
        if (fno.fname[0] == '.') continue;
        if (!(fno.fattrib & AM_DIR)) continue;
        strncpy(fd_names[fd_count], fno.fname, FD_NAME_LEN - 1);
        fd_names[fd_count][FD_NAME_LEN - 1] = '\0';
        fd_is_dir[fd_count] = 1;
        fd_count++;
    }
    f_closedir(&dir);

    /* Second pass: files matching filter */
    if (f_opendir(&dir, fd_path) != FR_OK) return;
    while (fd_count < FD_MAX_ENTRIES) {
        if (f_readdir(&dir, &fno) != FR_OK || fno.fname[0] == 0)
            break;
        if (fno.fname[0] == '.') continue;
        if (fno.fattrib & AM_DIR) continue;
        if (!fd_match_ext(fno.fname)) continue;
        strncpy(fd_names[fd_count], fno.fname, FD_NAME_LEN - 1);
        fd_names[fd_count][FD_NAME_LEN - 1] = '\0';
        fd_is_dir[fd_count] = 0;
        fd_count++;
    }
    f_closedir(&dir);

    /* Sort directories and files alphabetically within their groups.
     * The ".." entry (if present at index 0) stays first. */
    int dir_start = 0;
    if (fd_count > 0 && fd_is_dir[0] && fd_names[0][0] == '.' && fd_names[0][1] == '.')
        dir_start = 1;  /* skip ".." entry */
    /* Find where directories end and files begin */
    int file_start = dir_start;
    while (file_start < fd_count && fd_is_dir[file_start]) file_start++;
    /* Sort directories */
    fd_sort_range(dir_start, file_start);
    /* Sort files */
    fd_sort_range(file_start, fd_count);
}

/*==========================================================================
 * Navigate into directory / go up
 *=========================================================================*/

static void fd_navigate(const char *name) {
    if (strcmp(name, "..") == 0) {
        char *slash = strrchr(fd_path, '/');
        if (slash && slash != fd_path)
            *slash = '\0';
        else {
            fd_path[0] = '/';
            fd_path[1] = '\0';
        }
    } else {
        int len = (int)strlen(fd_path);
        if (len == 1 && fd_path[0] == '/')
            snprintf(fd_path + 1, FD_PATH_MAX - 1, "%s", name);
        else
            snprintf(fd_path + len, FD_PATH_MAX - len, "/%s", name);
    }
    fd_read_dir();
    wm_invalidate(fd_hwnd);
}

/*==========================================================================
 * Save mode cursor blink
 *=========================================================================*/

static void fd_blink_callback(TimerHandle_t xTimer) {
    (void)xTimer;
    fd_cursor_visible = !fd_cursor_visible;
    wm_invalidate(fd_hwnd);
}

static void fd_blink_reset(void) {
    fd_cursor_visible = true;
    if (fd_blink_timer) xTimerReset(fd_blink_timer, 0);
    wm_invalidate(fd_hwnd);
}

/*==========================================================================
 * Sunken field drawing
 *=========================================================================*/

static void fd_draw_sunken(int16_t x, int16_t y, int16_t w, int16_t h) {
    wd_hline(x, y, w, COLOR_DARK_GRAY);
    wd_vline(x, y, h, COLOR_DARK_GRAY);
    wd_hline(x + 1, y + 1, w - 2, COLOR_BLACK);
    wd_vline(x + 1, y + 1, h - 2, COLOR_BLACK);
    wd_hline(x, y + h - 1, w, COLOR_WHITE);
    wd_vline(x + w - 1, y, h, COLOR_WHITE);
    wd_hline(x + 1, y + h - 2, w - 2, THEME_BUTTON_FACE);
    wd_vline(x + w - 2, y + 1, h - 2, THEME_BUTTON_FACE);
    wd_fill_rect(x + 2, y + 2, w - 4, h - 4, COLOR_WHITE);
}

/*==========================================================================
 * Icon drawing (inline, small 12x10 icons)
 *=========================================================================*/

static void fd_draw_folder_icon(int16_t x, int16_t y) {
    /* Tab */
    wd_fill_rect(x, y, 5, 2, COLOR_YELLOW);
    wd_hline(x, y, 5, COLOR_BROWN);
    wd_vline(x, y, 2, COLOR_BROWN);
    /* Body */
    wd_fill_rect(x, y + 2, 12, 8, COLOR_YELLOW);
    wd_hline(x, y + 2, 12, COLOR_BROWN);
    wd_hline(x, y + 9, 12, COLOR_BROWN);
    wd_vline(x, y + 2, 8, COLOR_BROWN);
    wd_vline(x + 11, y + 2, 8, COLOR_BROWN);
}

static void fd_draw_file_icon(int16_t x, int16_t y) {
    /* Body */
    wd_fill_rect(x + 1, y, 10, 12, COLOR_WHITE);
    wd_hline(x + 1, y, 8, COLOR_DARK_GRAY);
    wd_hline(x + 1, y + 11, 10, COLOR_DARK_GRAY);
    wd_vline(x + 1, y, 12, COLOR_DARK_GRAY);
    wd_vline(x + 10, y + 2, 10, COLOR_DARK_GRAY);
    /* Folded corner */
    wd_hline(x + 9, y, 2, COLOR_DARK_GRAY);
    wd_vline(x + 10, y, 3, COLOR_DARK_GRAY);
    wd_pixel(x + 9, y + 1, COLOR_DARK_GRAY);
    /* Text lines */
    wd_hline(x + 3, y + 4, 5, COLOR_LIGHT_GRAY);
    wd_hline(x + 3, y + 6, 5, COLOR_LIGHT_GRAY);
    wd_hline(x + 3, y + 8, 4, COLOR_LIGHT_GRAY);
}

/*==========================================================================
 * Scrollbar drawing helpers
 *=========================================================================*/

static void fd_draw_sb_arrow_up(int16_t bx, int16_t by, bool pressed) {
    /* Small raised/sunken button */
    wd_fill_rect(bx, by, FD_SB_W, FD_SB_W, THEME_BUTTON_FACE);
    if (pressed) {
        wd_hline(bx, by, FD_SB_W, COLOR_DARK_GRAY);
        wd_vline(bx, by, FD_SB_W, COLOR_DARK_GRAY);
        wd_hline(bx, by + FD_SB_W - 1, FD_SB_W, COLOR_WHITE);
        wd_vline(bx + FD_SB_W - 1, by, FD_SB_W, COLOR_WHITE);
    } else {
        wd_hline(bx, by, FD_SB_W, COLOR_WHITE);
        wd_vline(bx, by, FD_SB_W, COLOR_WHITE);
        wd_hline(bx, by + FD_SB_W - 1, FD_SB_W, COLOR_BLACK);
        wd_vline(bx + FD_SB_W - 1, by, FD_SB_W, COLOR_BLACK);
        wd_hline(bx + 1, by + FD_SB_W - 2, FD_SB_W - 2, COLOR_DARK_GRAY);
        wd_vline(bx + FD_SB_W - 2, by + 1, FD_SB_W - 2, COLOR_DARK_GRAY);
    }
    /* Up triangle */
    int off = pressed ? 1 : 0;
    int cx = bx + 7 + off, cy = by + 5 + off;
    wd_pixel(cx, cy, COLOR_BLACK);
    wd_hline(cx - 1, cy + 1, 3, COLOR_BLACK);
    wd_hline(cx - 2, cy + 2, 5, COLOR_BLACK);
    wd_hline(cx - 3, cy + 3, 7, COLOR_BLACK);
}

static void fd_draw_sb_arrow_dn(int16_t bx, int16_t by, bool pressed) {
    wd_fill_rect(bx, by, FD_SB_W, FD_SB_W, THEME_BUTTON_FACE);
    if (pressed) {
        wd_hline(bx, by, FD_SB_W, COLOR_DARK_GRAY);
        wd_vline(bx, by, FD_SB_W, COLOR_DARK_GRAY);
        wd_hline(bx, by + FD_SB_W - 1, FD_SB_W, COLOR_WHITE);
        wd_vline(bx + FD_SB_W - 1, by, FD_SB_W, COLOR_WHITE);
    } else {
        wd_hline(bx, by, FD_SB_W, COLOR_WHITE);
        wd_vline(bx, by, FD_SB_W, COLOR_WHITE);
        wd_hline(bx, by + FD_SB_W - 1, FD_SB_W, COLOR_BLACK);
        wd_vline(bx + FD_SB_W - 1, by, FD_SB_W, COLOR_BLACK);
        wd_hline(bx + 1, by + FD_SB_W - 2, FD_SB_W - 2, COLOR_DARK_GRAY);
        wd_vline(bx + FD_SB_W - 2, by + 1, FD_SB_W - 2, COLOR_DARK_GRAY);
    }
    /* Down triangle */
    int off = pressed ? 1 : 0;
    int cx = bx + 7 + off, cy = by + 10 + off;
    wd_pixel(cx, cy, COLOR_BLACK);
    wd_hline(cx - 1, cy - 1, 3, COLOR_BLACK);
    wd_hline(cx - 2, cy - 2, 5, COLOR_BLACK);
    wd_hline(cx - 3, cy - 3, 7, COLOR_BLACK);
}

/*==========================================================================
 * Paint handler
 *=========================================================================*/

static void fd_paint(hwnd_t hwnd) {
    (void)hwnd;

    /* === "Look in:" row === */
    wd_text_ui(FD_PAD, FD_LOOKIN_Y, "Look in:",
               COLOR_BLACK, THEME_BUTTON_FACE);

    /* Sunken path field */
    fd_draw_sunken(FD_PATH_FIELD_X, FD_PATH_FIELD_Y,
                   FD_PATH_FIELD_W, FD_PATH_FIELD_H);
    {
        char dp[48];
        int max_c = (FD_PATH_FIELD_W - 8) / FONT_UI_WIDTH;
        if (max_c > (int)sizeof(dp) - 1) max_c = (int)sizeof(dp) - 1;
        int plen = (int)strlen(fd_path);
        if (plen <= max_c) {
            memcpy(dp, fd_path, plen + 1);
        } else {
            dp[0] = '.'; dp[1] = '.'; dp[2] = '.';
            memcpy(dp + 3, fd_path + plen - (max_c - 3), max_c - 3 + 1);
        }
        wd_text_ui(FD_PATH_FIELD_X + 4,
                   FD_PATH_FIELD_Y + (FD_PATH_FIELD_H - FONT_UI_HEIGHT) / 2,
                   dp, COLOR_BLACK, COLOR_WHITE);
    }

    /* Up button — same style as Navigator toolbar */
    wd_bevel_rect(FD_UP_BTN_X, FD_PATH_FIELD_Y, FD_UP_BTN_W, FD_UP_BTN_H,
                   fd_up_pressed ? COLOR_DARK_GRAY : COLOR_WHITE,
                   fd_up_pressed ? COLOR_WHITE : COLOR_DARK_GRAY,
                   THEME_BUTTON_FACE);
    wd_icon_16(FD_UP_BTN_X + 4, FD_PATH_FIELD_Y + 3, fn_icon16_up);

    /* === File list sunken bevel === */
    fd_draw_sunken(FD_LIST_X, FD_LIST_Y, FD_LIST_W, FD_LIST_H);

    /* === File list entries === */
    {
        int max_chars = (FD_CT_W - FD_ICON_W - 6) / FONT_UI_WIDTH;
        if (max_chars > FD_NAME_LEN) max_chars = FD_NAME_LEN;

        for (int i = 0; i < FD_MAX_VISIBLE; i++) {
            int idx = fd_scroll + i;
            int ey = FD_CT_Y + i * FD_ROW_H;

            if (idx >= fd_count) {
                /* Fill empty rows with white */
                wd_fill_rect(FD_CT_X, ey, FD_CT_W, FD_ROW_H, COLOR_WHITE);
                continue;
            }

            bool sel = (idx == fd_selected);
            uint8_t bg = sel ? COLOR_BLUE : COLOR_WHITE;
            uint8_t fg = sel ? COLOR_WHITE : COLOR_BLACK;

            /* Highlight bar */
            wd_fill_rect(FD_CT_X, ey, FD_CT_W, FD_ROW_H, bg);

            /* Icon */
            int icon_x = FD_CT_X + 2;
            int icon_y = ey + (FD_ROW_H - 10) / 2;
            if (fd_is_dir[idx])
                fd_draw_folder_icon(icon_x, icon_y);
            else
                fd_draw_file_icon(icon_x, icon_y);

            /* Text */
            char display[FD_NAME_LEN + 1];
            strncpy(display, fd_names[idx], FD_NAME_LEN);
            display[FD_NAME_LEN] = '\0';
            if ((int)strlen(display) > max_chars)
                display[max_chars] = '\0';

            wd_text_ui(FD_CT_X + FD_ICON_W + 2,
                       ey + (FD_ROW_H - FONT_UI_HEIGHT) / 2,
                       display, fg, bg);
        }
    }

    /* === Scrollbar === */
    {
        /* Track background */
        wd_fill_rect(FD_SB_X, FD_SB_Y, FD_SB_W, FD_SB_H, COLOR_LIGHT_GRAY);

        /* Up/down arrow buttons */
        bool can_up = fd_scroll > 0;
        bool can_dn = fd_count > FD_MAX_VISIBLE &&
                      fd_scroll < fd_count - FD_MAX_VISIBLE;
        fd_draw_sb_arrow_up(FD_SB_X, FD_SB_Y, false);
        fd_draw_sb_arrow_dn(FD_SB_X, FD_SB_Y + FD_SB_H - FD_SB_W, false);
        (void)can_up; (void)can_dn;

        /* Thumb */
        if (fd_count > FD_MAX_VISIBLE) {
            int track_y = FD_SB_Y + FD_SB_W;
            int track_h = FD_SB_H - 2 * FD_SB_W;
            int max_scroll = fd_count - FD_MAX_VISIBLE;
            int thumb_h = track_h * FD_MAX_VISIBLE / fd_count;
            if (thumb_h < 16) thumb_h = 16;
            if (thumb_h > track_h) thumb_h = track_h;
            int thumb_y = track_y;
            if (max_scroll > 0)
                thumb_y = track_y + (track_h - thumb_h) * fd_scroll / max_scroll;

            /* Raised thumb */
            wd_fill_rect(FD_SB_X, thumb_y, FD_SB_W, thumb_h,
                         THEME_BUTTON_FACE);
            wd_hline(FD_SB_X, thumb_y, FD_SB_W, COLOR_WHITE);
            wd_vline(FD_SB_X, thumb_y, thumb_h, COLOR_WHITE);
            wd_hline(FD_SB_X, thumb_y + thumb_h - 1, FD_SB_W, COLOR_BLACK);
            wd_vline(FD_SB_X + FD_SB_W - 1, thumb_y, thumb_h, COLOR_BLACK);
            wd_hline(FD_SB_X + 1, thumb_y + thumb_h - 2, FD_SB_W - 2,
                     COLOR_DARK_GRAY);
            wd_vline(FD_SB_X + FD_SB_W - 2, thumb_y + 1, thumb_h - 2,
                     COLOR_DARK_GRAY);
        }
    }

    /* === Separator line === */
    {
        int sep_y = FD_FNAME_Y - 6;
        wd_hline(0, sep_y, FD_CLIENT_W, COLOR_DARK_GRAY);
        wd_hline(0, sep_y + 1, FD_CLIENT_W, COLOR_WHITE);
    }

    /* === "File name:" row === */
    wd_text_ui(FD_PAD, FD_FNAME_Y + (FD_FIELD_H - FONT_UI_HEIGHT) / 2,
               "File name:", COLOR_BLACK, THEME_BUTTON_FACE);
    fd_draw_sunken(FD_FIELD_X, FD_FNAME_Y, FD_FIELD_W, FD_FIELD_H);

    if (fd_save_mode) {
        /* Editable filename field */
        int fc = (FD_FIELD_W - 8) / FONT_UI_WIDTH;
        char fn[FD_NAME_LEN + 1];
        strncpy(fn, fd_filename, FD_NAME_LEN);
        fn[FD_NAME_LEN] = '\0';
        if ((int)strlen(fn) > fc) fn[fc] = '\0';

        /* Get screen coords for cursor drawing */
        window_t *win = wm_get_window(fd_hwnd);
        int txt_sx = 0, txt_sy = 0;
        if (win) {
            point_t co = theme_client_origin(&win->frame, win->flags);
            txt_sx = co.x + FD_FIELD_X + 4;
            txt_sy = co.y + FD_FNAME_Y + (FD_FIELD_H - FONT_UI_HEIGHT) / 2;
        }
        gfx_text_ui(txt_sx, txt_sy, fn, COLOR_BLACK, COLOR_WHITE);

        /* Cursor */
        if (fd_fname_focus && fd_cursor_visible && win) {
            int cx = txt_sx + fd_fname_cursor * FONT_UI_WIDTH;
            for (int row = 0; row < FONT_UI_HEIGHT; row++)
                display_set_pixel(cx, txt_sy + row, COLOR_BLACK);
        }
    } else {
        /* Read-only: show selected file name */
        if (fd_count > 0 && fd_selected >= 0 && fd_selected < fd_count &&
            !fd_is_dir[fd_selected]) {
            int fc = (FD_FIELD_W - 8) / FONT_UI_WIDTH;
            char fn[FD_NAME_LEN + 1];
            strncpy(fn, fd_names[fd_selected], FD_NAME_LEN);
            fn[FD_NAME_LEN] = '\0';
            if ((int)strlen(fn) > fc) fn[fc] = '\0';
            wd_text_ui(FD_FIELD_X + 4,
                       FD_FNAME_Y + (FD_FIELD_H - FONT_UI_HEIGHT) / 2,
                       fn, COLOR_BLACK, COLOR_WHITE);
        }
    }

    wd_button(FD_BTN_X, FD_FNAME_Y, FD_BTN_W, FD_BTN_H,
              fd_save_mode ? "Save" : "Open",
              fd_btn_focus == 1, fd_btn_pressed == 0);

    /* === "Files of type:" row === */
    wd_text_ui(FD_PAD, FD_FTYPE_Y + (FD_FIELD_H - FONT_UI_HEIGHT) / 2,
               "Files of type:", COLOR_BLACK, THEME_BUTTON_FACE);
    fd_draw_sunken(FD_FIELD_X, FD_FTYPE_Y, FD_FIELD_W, FD_FIELD_H);
    {
        char ft[48];
        if (fd_filter[0]) {
            /* Build "*.mp3; *.mod" from ".mp3;.mod" */
            char *o = ft;
            const char *f = fd_filter;
            while (*f && o < ft + sizeof(ft) - 4) {
                if (f != fd_filter) { *o++ = ';'; *o++ = ' '; }
                *o++ = '*';
                while (*f && *f != ';' && o < ft + sizeof(ft) - 1)
                    *o++ = *f++;
                if (*f == ';') f++;
            }
            *o = '\0';
        } else {
            snprintf(ft, sizeof(ft), "All Files (*.*)");
        }
        wd_text_ui(FD_FIELD_X + 4,
                   FD_FTYPE_Y + (FD_FIELD_H - FONT_UI_HEIGHT) / 2,
                   ft, COLOR_BLACK, COLOR_WHITE);
    }
    wd_button(FD_BTN_X, FD_FTYPE_Y, FD_BTN_W, FD_BTN_H, "Cancel",
              fd_btn_focus == 2, fd_btn_pressed == 1);
}

/*==========================================================================
 * Close dialog and notify parent
 *=========================================================================*/

static void fd_close(uint16_t result) {
    if (fd_blink_timer) {
        xTimerStop(fd_blink_timer, 0);
        xTimerDelete(fd_blink_timer, 0);
        fd_blink_timer = NULL;
    }
    wm_clear_modal();
    wm_destroy_window(fd_hwnd);
    fd_hwnd = HWND_NULL;

    if (fd_parent != HWND_NULL) {
        wm_set_focus(fd_parent);
        window_event_t ev;
        memset(&ev, 0, sizeof(ev));
        ev.type = WM_COMMAND;
        ev.command.id = result;
        wm_post_event(fd_parent, &ev);
    }
}

/*==========================================================================
 * Select current entry (Open action)
 *=========================================================================*/

static void fd_select_current(void) {
    if (fd_save_mode) {
        /* Save mode: use the filename field */
        if (fd_filename[0] == '\0') return;

        /* If list has a selected directory, enter it */
        if (fd_count > 0 && fd_is_dir[fd_selected] &&
            fd_btn_focus == 0) {
            fd_navigate(fd_names[fd_selected]);
            return;
        }

        /* Build full path */
        if (fd_path[0] == '/' && fd_path[1] == '\0')
            snprintf(fd_result, FD_PATH_MAX, "/%s", fd_filename);
        else
            snprintf(fd_result, FD_PATH_MAX, "%s/%s", fd_path, fd_filename);

        /* Check if file exists — show overwrite confirmation */
        FILINFO fno;
        if (f_stat(fd_result, &fno) == FR_OK) {
            fd_overwrite_pending = true;
            dialog_show(fd_hwnd, "Confirm Save As",
                        "File already exists.\nDo you want to replace it?",
                        DLG_ICON_WARNING,
                        DLG_BTN_YES | DLG_BTN_NO);
            return;
        }

        fd_close(DLG_RESULT_FILE_SAVE);
    } else {
        /* Open mode: existing behavior */
        if (fd_count == 0) return;

        if (fd_is_dir[fd_selected]) {
            fd_navigate(fd_names[fd_selected]);
        } else {
            if (fd_path[0] == '/' && fd_path[1] == '\0')
                snprintf(fd_result, FD_PATH_MAX, "/%s", fd_names[fd_selected]);
            else
                snprintf(fd_result, FD_PATH_MAX, "%s/%s",
                         fd_path, fd_names[fd_selected]);
            fd_close(DLG_RESULT_FILE);
        }
    }
}

/*==========================================================================
 * Scroll helpers
 *=========================================================================*/

static void fd_ensure_visible(void) {
    if (fd_selected < fd_scroll)
        fd_scroll = fd_selected;
    else if (fd_selected >= fd_scroll + FD_MAX_VISIBLE)
        fd_scroll = fd_selected - FD_MAX_VISIBLE + 1;
}

static void fd_scroll_up(void) {
    if (fd_scroll > 0) {
        fd_scroll--;
        wm_invalidate(fd_hwnd);
    }
}

static void fd_scroll_down(void) {
    if (fd_count > FD_MAX_VISIBLE && fd_scroll < fd_count - FD_MAX_VISIBLE) {
        fd_scroll++;
        wm_invalidate(fd_hwnd);
    }
}

/*==========================================================================
 * Event handler
 *=========================================================================*/

static bool fd_event(hwnd_t hwnd, const window_event_t *event) {
    (void)hwnd;

    switch (event->type) {
    case WM_CLOSE:
        fd_close(DLG_RESULT_CANCEL);
        return true;

    case WM_COMMAND:
        /* Handle overwrite confirmation dialog result */
        if (fd_overwrite_pending) {
            fd_overwrite_pending = false;
            if (event->command.id == DLG_RESULT_YES) {
                fd_close(DLG_RESULT_FILE_SAVE);
            }
            /* DLG_RESULT_NO just returns to the save dialog */
            return true;
        }
        return false;

    case WM_CHAR:
        /* Character input for save mode filename field */
        if (fd_save_mode && fd_fname_focus) {
            char ch = event->charev.ch;
            if (ch >= 0x20 && ch < 0x7F && fd_fname_len < FD_NAME_LEN - 1) {
                for (int i = fd_fname_len; i > fd_fname_cursor; i--)
                    fd_filename[i] = fd_filename[i - 1];
                fd_filename[fd_fname_cursor] = ch;
                fd_fname_len++;
                fd_fname_cursor++;
                fd_filename[fd_fname_len] = '\0';
                fd_blink_reset();
            }
            return true;
        }
        return false;

    case WM_KEYDOWN:
        /* Save mode: filename field key handling */
        if (fd_save_mode && fd_fname_focus) {
            switch (event->key.scancode) {
            case 0x29: /* Escape */
                fd_close(DLG_RESULT_CANCEL);
                return true;
            case 0x28: /* Enter */
                fd_select_current();
                return true;
            case 0x2A: /* Backspace */
                if (fd_fname_cursor > 0) {
                    for (int i = fd_fname_cursor - 1; i < fd_fname_len - 1; i++)
                        fd_filename[i] = fd_filename[i + 1];
                    fd_fname_len--;
                    fd_fname_cursor--;
                    fd_filename[fd_fname_len] = '\0';
                    fd_blink_reset();
                }
                return true;
            case 0x4C: /* Delete */
                if (fd_fname_cursor < fd_fname_len) {
                    for (int i = fd_fname_cursor; i < fd_fname_len - 1; i++)
                        fd_filename[i] = fd_filename[i + 1];
                    fd_fname_len--;
                    fd_filename[fd_fname_len] = '\0';
                    fd_blink_reset();
                }
                return true;
            case 0x50: /* Left */
                if (fd_fname_cursor > 0) {
                    fd_fname_cursor--;
                    fd_blink_reset();
                }
                return true;
            case 0x4F: /* Right */
                if (fd_fname_cursor < fd_fname_len) {
                    fd_fname_cursor++;
                    fd_blink_reset();
                }
                return true;
            case 0x4A: /* Home */
                fd_fname_cursor = 0;
                fd_blink_reset();
                return true;
            case 0x4D: /* End */
                fd_fname_cursor = fd_fname_len;
                fd_blink_reset();
                return true;
            case 0x2B: /* Tab — cycle to list/buttons */
                fd_fname_focus = false;
                fd_btn_focus = 0;
                wm_invalidate(fd_hwnd);
                return true;
            }
            return false;
        }

        /* Standard key handling (list/button focused) */
        switch (event->key.scancode) {
        case 0x29: /* Escape */
            fd_close(DLG_RESULT_CANCEL);
            return true;

        case 0x28: /* Enter */
            if (fd_btn_focus == 2)
                fd_close(DLG_RESULT_CANCEL);
            else
                fd_select_current();
            return true;

        case 0x2A: /* Backspace — go up */
            if (fd_btn_focus == 0)
                fd_navigate("..");
            return true;

        case 0x52: /* Up Arrow */
            if (fd_btn_focus != 0) {
                fd_btn_focus = 0;
            } else if (fd_selected > 0) {
                fd_selected--;
                fd_ensure_visible();
            }
            wm_invalidate(fd_hwnd);
            return true;

        case 0x51: /* Down Arrow */
            if (fd_btn_focus != 0) {
                fd_btn_focus = 0;
            } else if (fd_selected < fd_count - 1) {
                fd_selected++;
                fd_ensure_visible();
            }
            wm_invalidate(fd_hwnd);
            return true;

        case 0x2B: /* Tab */
            if (fd_save_mode) {
                /* cycle: list(0) -> Save(1) -> Cancel(2) -> filename -> list */
                if (fd_btn_focus == 2) {
                    fd_fname_focus = true;
                    fd_btn_focus = 0;
                } else {
                    fd_btn_focus = (fd_btn_focus + 1) % 3;
                }
            } else {
                fd_btn_focus = (fd_btn_focus + 1) % 3;
            }
            wm_invalidate(fd_hwnd);
            return true;

        case 0x4B: /* Page Up */
            if (fd_btn_focus == 0) {
                fd_selected -= FD_MAX_VISIBLE;
                if (fd_selected < 0) fd_selected = 0;
                fd_ensure_visible();
                wm_invalidate(fd_hwnd);
            }
            return true;

        case 0x4E: /* Page Down */
            if (fd_btn_focus == 0) {
                fd_selected += FD_MAX_VISIBLE;
                if (fd_selected >= fd_count) fd_selected = fd_count - 1;
                if (fd_selected < 0) fd_selected = 0;
                fd_ensure_visible();
                wm_invalidate(fd_hwnd);
            }
            return true;

        case 0x4A: /* Home */
            if (fd_btn_focus == 0) {
                fd_selected = 0;
                fd_ensure_visible();
                wm_invalidate(fd_hwnd);
            }
            return true;

        case 0x4D: /* End */
            if (fd_btn_focus == 0 && fd_count > 0) {
                fd_selected = fd_count - 1;
                fd_ensure_visible();
                wm_invalidate(fd_hwnd);
            }
            return true;
        }
        return false;

    case WM_LBUTTONDOWN: {
        int mx = event->mouse.x;
        int my = event->mouse.y;

        /* Hit-test: Up button — track press for animation */
        if (mx >= FD_UP_BTN_X && mx < FD_UP_BTN_X + FD_UP_BTN_W &&
            my >= FD_PATH_FIELD_Y && my < FD_PATH_FIELD_Y + FD_UP_BTN_H) {
            fd_up_pressed = true;
            wm_invalidate(fd_hwnd);
            return true;
        }

        /* Hit-test: Scrollbar up arrow */
        if (mx >= FD_SB_X && mx < FD_SB_X + FD_SB_W &&
            my >= FD_SB_Y && my < FD_SB_Y + FD_SB_W) {
            fd_scroll_up();
            return true;
        }

        /* Hit-test: Scrollbar down arrow */
        {
            int dn_y = FD_SB_Y + FD_SB_H - FD_SB_W;
            if (mx >= FD_SB_X && mx < FD_SB_X + FD_SB_W &&
                my >= dn_y && my < dn_y + FD_SB_W) {
                fd_scroll_down();
                return true;
            }
        }

        /* Hit-test: Scrollbar track (thumb drag / page up/down) */
        if (mx >= FD_SB_X && mx < FD_SB_X + FD_SB_W &&
            my >= FD_SB_Y + FD_SB_W &&
            my < FD_SB_Y + FD_SB_H - FD_SB_W &&
            fd_count > FD_MAX_VISIBLE) {
            int track_y = FD_SB_Y + FD_SB_W;
            int track_h = FD_SB_H - 2 * FD_SB_W;
            int max_scroll = fd_count - FD_MAX_VISIBLE;
            int thumb_h = track_h * FD_MAX_VISIBLE / fd_count;
            if (thumb_h < 16) thumb_h = 16;
            int thumb_y = track_y;
            if (max_scroll > 0)
                thumb_y = track_y + (track_h - thumb_h) * fd_scroll / max_scroll;

            if (my >= thumb_y && my < thumb_y + thumb_h) {
                /* Thumb drag start */
                fd_sb_dragging = true;
                fd_sb_drag_offset = (int16_t)(my - thumb_y);
            } else if (my < thumb_y) {
                /* Page up */
                fd_scroll -= FD_MAX_VISIBLE;
                if (fd_scroll < 0) fd_scroll = 0;
            } else {
                /* Page down */
                fd_scroll += FD_MAX_VISIBLE;
                if (fd_scroll > max_scroll) fd_scroll = max_scroll;
            }
            wm_invalidate(fd_hwnd);
            return true;
        }

        /* Hit-test: File list content area */
        if (mx >= FD_CT_X && mx < FD_CT_X + FD_CT_W &&
            my >= FD_CT_Y && my < FD_CT_Y + FD_CT_H) {
            int clicked = fd_scroll + (my - FD_CT_Y) / FD_ROW_H;
            if (clicked >= 0 && clicked < fd_count) {
                /* Double-click detection */
                uint32_t now = xTaskGetTickCount();
                if (clicked == fd_last_click_idx &&
                    (now - fd_last_click_tick) < pdMS_TO_TICKS(FD_DBLCLICK_MS)) {
                    /* Double-click: open/enter */
                    fd_selected = clicked;
                    fd_select_current();
                    fd_last_click_idx = -1;
                } else {
                    /* Single click: select */
                    fd_selected = clicked;
                    fd_last_click_tick = now;
                    fd_last_click_idx = clicked;
                    fd_btn_focus = 0;
                    fd_fname_focus = false;
                    /* Save mode: populate filename from selected file */
                    if (fd_save_mode && !fd_is_dir[clicked]) {
                        strncpy(fd_filename, fd_names[clicked], FD_NAME_LEN - 1);
                        fd_filename[FD_NAME_LEN - 1] = '\0';
                        fd_fname_len = (uint8_t)strlen(fd_filename);
                        fd_fname_cursor = fd_fname_len;
                    }
                    wm_invalidate(fd_hwnd);
                }
            }
            return true;
        }

        /* Hit-test: Filename field (save mode) */
        if (fd_save_mode &&
            mx >= FD_FIELD_X + 2 && mx < FD_FIELD_X + FD_FIELD_W - 2 &&
            my >= FD_FNAME_Y && my < FD_FNAME_Y + FD_FIELD_H) {
            fd_fname_focus = true;
            fd_btn_focus = 0;
            int click_x = mx - FD_FIELD_X - 4;
            int new_pos = click_x / FONT_UI_WIDTH;
            if (new_pos < 0) new_pos = 0;
            if (new_pos > fd_fname_len) new_pos = fd_fname_len;
            fd_fname_cursor = new_pos;
            fd_blink_reset();
            return true;
        }

        /* Hit-test: Open/Save button */
        fd_btn_pressed = -1;
        if (mx >= FD_BTN_X && mx < FD_BTN_X + FD_BTN_W &&
            my >= FD_FNAME_Y && my < FD_FNAME_Y + FD_BTN_H) {
            fd_btn_pressed = 0;
            fd_btn_focus = 1;
            fd_fname_focus = false;
            wm_invalidate(fd_hwnd);
            return true;
        }

        /* Hit-test: Cancel button */
        if (mx >= FD_BTN_X && mx < FD_BTN_X + FD_BTN_W &&
            my >= FD_FTYPE_Y && my < FD_FTYPE_Y + FD_BTN_H) {
            fd_btn_pressed = 1;
            fd_btn_focus = 2;
            wm_invalidate(fd_hwnd);
            return true;
        }
        return true;
    }

    case WM_MOUSEMOVE: {
        if (fd_sb_dragging && fd_count > FD_MAX_VISIBLE) {
            int my = event->mouse.y;
            int track_y = FD_SB_Y + FD_SB_W;
            int track_h = FD_SB_H - 2 * FD_SB_W;
            int max_scroll = fd_count - FD_MAX_VISIBLE;
            int thumb_h = track_h * FD_MAX_VISIBLE / fd_count;
            if (thumb_h < 16) thumb_h = 16;
            int avail = track_h - thumb_h;
            if (avail > 0) {
                int thumb_top = my - fd_sb_drag_offset;
                int pos = (thumb_top - track_y) * max_scroll / avail;
                if (pos < 0) pos = 0;
                if (pos > max_scroll) pos = max_scroll;
                fd_scroll = (int16_t)pos;
                wm_invalidate(fd_hwnd);
            }
        }
        return true;
    }

    case WM_LBUTTONUP: {
        int mx = event->mouse.x;
        int my = event->mouse.y;

        /* Scrollbar thumb drag release */
        if (fd_sb_dragging) {
            fd_sb_dragging = false;
            return true;
        }

        /* Up button release */
        if (fd_up_pressed) {
            fd_up_pressed = false;
            if (mx >= FD_UP_BTN_X && mx < FD_UP_BTN_X + FD_UP_BTN_W &&
                my >= FD_PATH_FIELD_Y && my < FD_PATH_FIELD_Y + FD_UP_BTN_H) {
                fd_navigate("..");
            } else {
                wm_invalidate(fd_hwnd);
            }
            return true;
        }

        /* Open/Cancel button release */
        if (fd_btn_pressed < 0) return true;
        int pressed = fd_btn_pressed;
        fd_btn_pressed = -1;

        int btn_y = (pressed == 0) ? FD_FNAME_Y : FD_FTYPE_Y;

        if (mx >= FD_BTN_X && mx < FD_BTN_X + FD_BTN_W &&
            my >= btn_y && my < btn_y + FD_BTN_H) {
            if (pressed == 0)
                fd_select_current();
            else
                fd_close(DLG_RESULT_CANCEL);
        } else {
            wm_invalidate(fd_hwnd);
        }
        return true;
    }

    default:
        return false;
    }
}

/*==========================================================================
 * Public API
 *=========================================================================*/

hwnd_t file_dialog_open(hwnd_t parent, const char *title,
                        const char *initial_path, const char *filter_ext) {
    if (fd_hwnd != HWND_NULL) return HWND_NULL;

    fd_parent = parent;
    fd_save_mode = false;
    fd_btn_focus = 0;
    fd_btn_pressed = -1;
    fd_up_pressed = false;
    fd_last_click_idx = -1;
    fd_last_click_tick = 0;
    fd_fname_focus = false;
    fd_overwrite_pending = false;

    if (initial_path && initial_path[0]) {
        strncpy(fd_path, initial_path, FD_PATH_MAX - 1);
        fd_path[FD_PATH_MAX - 1] = '\0';
    } else {
        fd_path[0] = '/';
        fd_path[1] = '\0';
    }

    if (filter_ext && filter_ext[0]) {
        strncpy(fd_filter, filter_ext, sizeof(fd_filter) - 1);
        fd_filter[sizeof(fd_filter) - 1] = '\0';
    } else {
        fd_filter[0] = '\0';
    }

    fd_read_dir();

    int outer_w = FD_CLIENT_W + 2 * THEME_BORDER_WIDTH;
    int outer_h = FD_CLIENT_H + THEME_TITLE_HEIGHT + 2 * THEME_BORDER_WIDTH;

    int work_h = taskbar_work_area_height();
    int x = (DISPLAY_WIDTH - outer_w) / 2;
    int y = (work_h - outer_h) / 2;
    if (x < 0) x = 0;
    if (y < 0) y = 0;

    fd_hwnd = wm_create_window((int16_t)x, (int16_t)y,
                                (int16_t)outer_w, (int16_t)outer_h,
                                title, WSTYLE_DIALOG,
                                fd_event, fd_paint);
    if (fd_hwnd == HWND_NULL) return HWND_NULL;

    window_t *win = wm_get_window(fd_hwnd);
    if (win) win->bg_color = THEME_BUTTON_FACE;

    wm_set_focus(fd_hwnd);
    wm_set_modal(fd_hwnd);

    return fd_hwnd;
}

const char *file_dialog_get_path(void) {
    return fd_result;
}

hwnd_t file_dialog_save(hwnd_t parent, const char *title,
                        const char *initial_path, const char *filter_ext,
                        const char *initial_name) {
    if (fd_hwnd != HWND_NULL) return HWND_NULL;

    fd_parent = parent;
    fd_save_mode = true;
    fd_btn_focus = 0;
    fd_btn_pressed = -1;
    fd_up_pressed = false;
    fd_last_click_idx = -1;
    fd_last_click_tick = 0;
    fd_overwrite_pending = false;

    /* Initialize filename field */
    fd_fname_focus = true;  /* Start with filename field focused */
    if (initial_name && initial_name[0]) {
        strncpy(fd_filename, initial_name, FD_NAME_LEN - 1);
        fd_filename[FD_NAME_LEN - 1] = '\0';
        fd_fname_len = (uint8_t)strlen(fd_filename);
        fd_fname_cursor = fd_fname_len;
    } else {
        fd_filename[0] = '\0';
        fd_fname_len = 0;
        fd_fname_cursor = 0;
    }

    if (initial_path && initial_path[0]) {
        strncpy(fd_path, initial_path, FD_PATH_MAX - 1);
        fd_path[FD_PATH_MAX - 1] = '\0';
    } else {
        fd_path[0] = '/';
        fd_path[1] = '\0';
    }

    if (filter_ext && filter_ext[0]) {
        strncpy(fd_filter, filter_ext, sizeof(fd_filter) - 1);
        fd_filter[sizeof(fd_filter) - 1] = '\0';
    } else {
        fd_filter[0] = '\0';
    }

    fd_read_dir();

    int outer_w = FD_CLIENT_W + 2 * THEME_BORDER_WIDTH;
    int outer_h = FD_CLIENT_H + THEME_TITLE_HEIGHT + 2 * THEME_BORDER_WIDTH;

    int work_h = taskbar_work_area_height();
    int x = (DISPLAY_WIDTH - outer_w) / 2;
    int y = (work_h - outer_h) / 2;
    if (x < 0) x = 0;
    if (y < 0) y = 0;

    fd_hwnd = wm_create_window((int16_t)x, (int16_t)y,
                                (int16_t)outer_w, (int16_t)outer_h,
                                title, WSTYLE_DIALOG,
                                fd_event, fd_paint);
    if (fd_hwnd == HWND_NULL) {
        fd_save_mode = false;
        return HWND_NULL;
    }

    window_t *win = wm_get_window(fd_hwnd);
    if (win) win->bg_color = THEME_BUTTON_FACE;

    wm_set_focus(fd_hwnd);
    wm_set_modal(fd_hwnd);

    /* Start cursor blink timer for filename field */
    fd_cursor_visible = true;
    fd_blink_timer = xTimerCreate("fdblink", pdMS_TO_TICKS(500),
                                    pdTRUE, NULL, fd_blink_callback);
    if (fd_blink_timer) xTimerStart(fd_blink_timer, 0);

    return fd_hwnd;
}
