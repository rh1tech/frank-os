/*
 * FRANK OS
 * Copyright (c) 2025 Mikhail Matveev <xtreme@rh1.tech>
 * https://rh1.tech
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "filemanager.h"
#include "window.h"
#include "window_event.h"
#include "window_draw.h"
#include "window_theme.h"
#include "menu.h"
#include "dialog.h"
#include "gfx.h"
#include "font.h"
#include "display.h"
#include "taskbar.h"
#include "app.h"
#include "ff.h"
#include "sdcard_init.h"
#include "FreeRTOS.h"
#include "task.h"
#include <string.h>
#include <stdio.h>

/*==========================================================================
 * Layout constants
 *=========================================================================*/

#define LARGE_CELL_W    76
#define LARGE_CELL_H    56
#define SMALL_CELL_W   160
#define SMALL_CELL_H    20
#define LIST_ROW_H      18
#define LIST_HDR_H      20

#define DBLCLICK_MS    400
#define DRAG_THRESHOLD   4

/* Toolbar button geometry */
#define TB_BTN_W        24
#define TB_BTN_H        22
#define TB_BTN_PAD       2
#define TB_SEP_W         6  /* separator width */

/* Toolbar button indices */
#define TB_BACK     0
#define TB_UP       1
#define TB_CUT      2
#define TB_COPY     3
#define TB_PASTE    4
#define TB_DELETE   5
#define TB_BTN_COUNT 6

/*==========================================================================
 * Clipboard (global — shared between file manager instances)
 *=========================================================================*/

static struct {
    char    paths[16][FN_PATH_MAX];
    uint8_t count;
    bool    is_cut;
} fn_clipboard;

/*==========================================================================
 * Helper: get state pointer from hwnd
 *=========================================================================*/

static filemanager_t *fm_from_hwnd(hwnd_t hwnd) {
    window_t *win = wm_get_window(hwnd);
    return win ? (filemanager_t *)win->user_data : NULL;
}

/*==========================================================================
 * App icon cache — 16x16 icons read from .inf files during refresh
 *=========================================================================*/

#define FN_MAX_APP_ICONS 16
static uint8_t  fn_app_icons[FN_MAX_APP_ICONS][256];
static uint8_t  fn_app_icon_count;

/*==========================================================================
 * Icon selection by file type
 *=========================================================================*/

static const uint8_t *fn_get_icon_32(const fn_entry_t *e) {
    if (e->attrib & AM_DIR) return fn_icon32_folder;
    if (e->is_executable) return fn_icon32_application;
    const char *dot = strrchr(e->name, '.');
    if (dot) {
        if (strcmp(dot, ".txt") == 0 || strcmp(dot, ".inf") == 0)
            return fn_icon32_text_doc;
    }
    return fn_icon32_document;
}

static const uint8_t *fn_get_icon_16(const fn_entry_t *e) {
    if (e->attrib & AM_DIR) return fn_icon16_folder;
    if (e->is_executable) {
        if (e->icon_idx >= 0 && e->icon_idx < FN_MAX_APP_ICONS)
            return fn_app_icons[e->icon_idx];
        return fn_icon16_application;
    }
    const char *dot = strrchr(e->name, '.');
    if (dot) {
        if (strcmp(dot, ".txt") == 0 || strcmp(dot, ".inf") == 0)
            return fn_icon16_text_doc;
    }
    return fn_icon16_document;
}

/*==========================================================================
 * Directory reading
 *=========================================================================*/

/* Sort state — set before sort, used by comparators */
static int8_t sort_col;
static int8_t sort_asc;

static int entry_cmp(const void *a, const void *b) {
    const fn_entry_t *ea = (const fn_entry_t *)a;
    const fn_entry_t *eb = (const fn_entry_t *)b;
    /* Directories always first */
    int da = (ea->attrib & AM_DIR) ? 0 : 1;
    int db = (eb->attrib & AM_DIR) ? 0 : 1;
    if (da != db) return da - db;

    int result = 0;
    switch (sort_col) {
    case 0: /* Name */
        result = strncmp(ea->name, eb->name, FN_NAME_MAX);
        break;
    case 1: /* Size */
        if (ea->size < eb->size) result = -1;
        else if (ea->size > eb->size) result = 1;
        else result = strncmp(ea->name, eb->name, FN_NAME_MAX);
        break;
    case 2: { /* Type (extension) */
        const char *ext_a = strrchr(ea->name, '.');
        const char *ext_b = strrchr(eb->name, '.');
        if (!ext_a) ext_a = "";
        if (!ext_b) ext_b = "";
        result = strcmp(ext_a, ext_b);
        if (result == 0)
            result = strncmp(ea->name, eb->name, FN_NAME_MAX);
        break;
    }
    default:
        result = strncmp(ea->name, eb->name, FN_NAME_MAX);
        break;
    }
    return sort_asc ? result : -result;
}

/* Check if a file exists at the given path.
 * Uses caller-provided FILINFO to avoid extra stack usage. */
static bool fm_file_exists(const char *path, FILINFO *tmp) {
    return f_stat(path, tmp) == FR_OK;
}

static void fm_refresh(filemanager_t *fm) {
    fm->entry_count = 0;
    fm->scroll_y = 0;
    fm->focus_index = -1;
    fm->anchor_index = -1;
    fm->selection_count = 0;
    fn_app_icon_count = 0;

    DIR dir;
    FILINFO fno;
    if (f_opendir(&dir, fm->path) != FR_OK) return;

    /* Reuse a single path buffer for .inf checks (avoids stack bloat) */
    char chk_path[FN_PATH_MAX];

    while (fm->entry_count < FN_MAX_ENTRIES) {
        if (f_readdir(&dir, &fno) != FR_OK || fno.fname[0] == 0)
            break;
        /* Skip hidden files */
        if (fno.fattrib & AM_HID) continue;
        if (fno.fname[0] == '.') continue;

        /* Skip .inf companion files when the base executable exists */
        if (!(fno.fattrib & AM_DIR)) {
            const char *dot = strrchr(fno.fname, '.');
            if (dot && strcmp(dot, ".inf") == 0) {
                /* Build base name (strip .inf extension) */
                int blen = (int)(dot - fno.fname);
                if (blen > 0 && blen < FN_NAME_MAX) {
                    if (fm->path[1] == '\0')
                        snprintf(chk_path, sizeof(chk_path), "/%.63s",
                                 fno.fname);
                    else
                        snprintf(chk_path, sizeof(chk_path), "%s/%.63s",
                                 fm->path, fno.fname);
                    /* Trim the .inf from the check path */
                    char *ext = strrchr(chk_path, '.');
                    if (ext) *ext = '\0';
                    /* If base file exists, skip this .inf */
                    FILINFO tmp;
                    if (fm_file_exists(chk_path, &tmp))
                        continue;
                }
            }
        }

        fn_entry_t *e = &fm->entries[fm->entry_count];
        strncpy(e->name, fno.fname, FN_NAME_MAX - 1);
        e->name[FN_NAME_MAX - 1] = '\0';
        e->size = (uint32_t)fno.fsize;
        e->attrib = fno.fattrib;
        e->sel_flags = 0;
        e->is_executable = 0;
        e->icon_idx = -1;
        e->custom_order = -1;

        /* Detect executables: non-directory files with a companion .inf */
        if (!(fno.fattrib & AM_DIR)) {
            if (fm->path[1] == '\0')
                snprintf(chk_path, sizeof(chk_path), "/%s.inf", fno.fname);
            else
                snprintf(chk_path, sizeof(chk_path), "%s/%s.inf",
                         fm->path, fno.fname);
            FILINFO tmp;
            if (fm_file_exists(chk_path, &tmp)) {
                e->is_executable = 1;
                /* Read 16x16 icon from .inf into cache */
                if (fn_app_icon_count < FN_MAX_APP_ICONS) {
                    FIL inf;
                    if (f_open(&inf, chk_path, FA_READ) == FR_OK) {
                        UINT br;
                        char ch;
                        /* Skip name line */
                        while (f_read(&inf, &ch, 1, &br) == FR_OK && br == 1)
                            if (ch == '\n') break;
                        /* Read 256-byte icon */
                        if (f_read(&inf, fn_app_icons[fn_app_icon_count],
                                   256, &br) == FR_OK && br == 256) {
                            e->icon_idx = fn_app_icon_count;
                            fn_app_icon_count++;
                        }
                        f_close(&inf);
                    }
                }
            }
        }

        fm->entry_count++;
    }
    f_closedir(&dir);

    /* Sort: directories first, then by current sort column/order */
    sort_col = fm->sort_column;
    sort_asc = fm->sort_ascending;
    if (fm->entry_count > 1) {
        /* Simple insertion sort — avoids qsort linking issues */
        for (int i = 1; i < (int)fm->entry_count; i++) {
            fn_entry_t tmp = fm->entries[i];
            int j = i - 1;
            while (j >= 0 && entry_cmp(&tmp, &fm->entries[j]) < 0) {
                fm->entries[j + 1] = fm->entries[j];
                j--;
            }
            fm->entries[j + 1] = tmp;
        }
    }

    /* Update window title — truncate with "..." if path too long */
    {
        int path_len = (int)strlen(fm->path);
        if (path_len <= 23) {
            wm_set_title(fm->hwnd, fm->path);
        } else {
            char trunc[24];
            trunc[0] = '.'; trunc[1] = '.'; trunc[2] = '.';
            strncpy(trunc + 3, fm->path + path_len - 20, 20);
            trunc[23] = '\0';
            wm_set_title(fm->hwnd, trunc);
        }
    }
}

/*==========================================================================
 * Navigation
 *=========================================================================*/

static void fm_history_push(filemanager_t *fm) {
    if (fm->history_count < FN_HISTORY_DEPTH) {
        strncpy(fm->history[fm->history_count], fm->path, FN_PATH_MAX - 1);
        fm->history[fm->history_count][FN_PATH_MAX - 1] = '\0';
        fm->history_pos = fm->history_count;
        fm->history_count++;
    } else {
        /* Shift history down */
        for (int i = 0; i < FN_HISTORY_DEPTH - 1; i++)
            strncpy(fm->history[i], fm->history[i + 1], FN_PATH_MAX);
        strncpy(fm->history[FN_HISTORY_DEPTH - 1], fm->path, FN_PATH_MAX - 1);
        fm->history_pos = FN_HISTORY_DEPTH - 1;
    }
}

static void fm_navigate(filemanager_t *fm, const char *new_path) {
    fm_history_push(fm);
    strncpy(fm->path, new_path, FN_PATH_MAX - 1);
    fm->path[FN_PATH_MAX - 1] = '\0';
    fm_refresh(fm);
    wm_invalidate(fm->hwnd);
}

static void fm_go_back(filemanager_t *fm) {
    if (fm->history_pos <= 0 || fm->history_count == 0) return;
    fm->history_pos--;
    strncpy(fm->path, fm->history[fm->history_pos], FN_PATH_MAX - 1);
    fm->path[FN_PATH_MAX - 1] = '\0';
    fm_refresh(fm);
    wm_invalidate(fm->hwnd);
}

static void fm_go_up(filemanager_t *fm) {
    /* Strip last component */
    char *last = strrchr(fm->path, '/');
    if (!last || last == fm->path) {
        /* Already at root */
        fm_navigate(fm, "/");
        return;
    }
    char new_path[FN_PATH_MAX];
    int len = (int)(last - fm->path);
    if (len == 0) len = 1; /* keep root "/" */
    strncpy(new_path, fm->path, len);
    new_path[len] = '\0';
    fm_navigate(fm, new_path);
}

/*==========================================================================
 * Hit-test: which entry index is at client (mx, my)?
 *=========================================================================*/

static int16_t fm_hit_test(filemanager_t *fm, int16_t mx, int16_t my,
                            int16_t client_w) {
    /* Adjust for file area start */
    int16_t fy = my - FN_HEADER_HEIGHT + fm->scroll_y;
    if (fy < 0 || my < FN_HEADER_HEIGHT) return -1;

    int16_t file_w = client_w - FN_SCROLLBAR_W;
    if (mx >= file_w || mx < 0) return -1;

    int cols, cell_w, cell_h;
    switch (fm->view_mode) {
    case FN_VIEW_LARGE_ICONS:
        cell_w = LARGE_CELL_W;
        cell_h = LARGE_CELL_H;
        cols = file_w / cell_w;
        if (cols < 1) cols = 1;
        break;
    case FN_VIEW_SMALL_ICONS:
        cell_w = SMALL_CELL_W;
        cell_h = SMALL_CELL_H;
        cols = file_w / cell_w;
        if (cols < 1) cols = 1;
        break;
    case FN_VIEW_LIST:
        /* List has header row */
        fy -= LIST_HDR_H;
        if (fy < 0) return -1;
        cell_w = file_w;
        cell_h = LIST_ROW_H;
        cols = 1;
        break;
    default:
        return -1;
    }

    int col = mx / cell_w;
    int row = fy / cell_h;
    int idx = row * cols + col;
    if (idx < 0 || idx >= (int)fm->entry_count) return -1;
    return (int16_t)idx;
}

/*==========================================================================
 * Content height calculation
 *=========================================================================*/

static int16_t fm_calc_content_height(filemanager_t *fm, int16_t client_w) {
    int16_t file_w = client_w - FN_SCROLLBAR_W;
    int cols, cell_h;
    int extra = 0;
    switch (fm->view_mode) {
    case FN_VIEW_LARGE_ICONS:
        cols = file_w / LARGE_CELL_W;
        if (cols < 1) cols = 1;
        cell_h = LARGE_CELL_H;
        break;
    case FN_VIEW_SMALL_ICONS:
        cols = file_w / SMALL_CELL_W;
        if (cols < 1) cols = 1;
        cell_h = SMALL_CELL_H;
        break;
    case FN_VIEW_LIST:
        cols = 1;
        cell_h = LIST_ROW_H;
        extra = LIST_HDR_H;
        break;
    default:
        return 0;
    }
    int rows = (fm->entry_count + cols - 1) / cols;
    return (int16_t)(rows * cell_h + extra);
}

/*==========================================================================
 * Selection helpers
 *=========================================================================*/

static void fm_clear_selection(filemanager_t *fm) {
    for (int i = 0; i < (int)fm->entry_count; i++)
        fm->entries[i].sel_flags &= ~FN_SEL_SELECTED;
    fm->selection_count = 0;
}

static void fm_select_all(filemanager_t *fm) {
    for (int i = 0; i < (int)fm->entry_count; i++)
        fm->entries[i].sel_flags |= FN_SEL_SELECTED;
    fm->selection_count = fm->entry_count;
}

static void fm_update_selection_count(filemanager_t *fm) {
    fm->selection_count = 0;
    for (int i = 0; i < (int)fm->entry_count; i++)
        if (fm->entries[i].sel_flags & FN_SEL_SELECTED)
            fm->selection_count++;
}

/*==========================================================================
 * Scrollbar helpers
 *=========================================================================*/

static int16_t fm_file_area_height(int16_t client_h) {
    return client_h - FN_HEADER_HEIGHT - FN_STATUSBAR_H;
}

static void fm_clamp_scroll(filemanager_t *fm, int16_t client_h) {
    int16_t fah = fm_file_area_height(client_h);
    int16_t max_scroll = fm->content_height - fah;
    if (max_scroll < 0) max_scroll = 0;
    if (fm->scroll_y > max_scroll) fm->scroll_y = max_scroll;
    if (fm->scroll_y < 0) fm->scroll_y = 0;
}

/*==========================================================================
 * Painting: toolbar
 *=========================================================================*/

/* Toolbar icon data for each button */
static const uint8_t * const tb_icons[TB_BTN_COUNT] = {
    NULL, NULL, NULL, NULL, NULL, NULL  /* set at first use via accessor */
};

static const uint8_t *tb_get_icon(int index) {
    switch (index) {
    case TB_BACK:   return fn_icon16_back;
    case TB_UP:     return fn_icon16_up;
    case TB_CUT:    return fn_icon16_cut;
    case TB_COPY:   return fn_icon16_copy;
    case TB_PASTE:  return fn_icon16_paste;
    case TB_DELETE: return fn_icon16_delete;
    default:        return NULL;
    }
}

static const char *tb_get_label(int index) {
    switch (index) {
    case TB_BACK:   return "Back";
    case TB_UP:     return "Up";
    case TB_CUT:    return "Cut";
    case TB_COPY:   return "Copy";
    case TB_PASTE:  return "Paste";
    case TB_DELETE: return "Delete";
    default:        return NULL;
    }
}

/* Width of a toolbar button — all are icon buttons now */
static int tb_btn_w(int index) {
    (void)index;
    return TB_BTN_W;
}

/* Compute x position for toolbar button by index */
static int tb_btn_x(int index) {
    int x = TB_BTN_PAD;
    for (int i = 0; i < index; i++) {
        x += tb_btn_w(i) + TB_BTN_PAD;
        /* Separator after Up */
        if (i == TB_UP) x += TB_SEP_W;
    }
    return x;
}

static void fm_paint_toolbar(filemanager_t *fm, int16_t cw) {
    /* Background */
    wd_fill_rect(0, 0, cw, FN_TOOLBAR_HEIGHT, THEME_BUTTON_FACE);
    /* Bottom separator */
    wd_hline(0, FN_TOOLBAR_HEIGHT - 2, cw, COLOR_DARK_GRAY);
    wd_hline(0, FN_TOOLBAR_HEIGHT - 1, cw, COLOR_WHITE);

    int by = 2;

    /* All toolbar buttons (icon-based) */
    for (int i = 0; i < TB_BTN_COUNT; i++) {
        int bx = tb_btn_x(i);
        bool pressed = (fm->toolbar_pressed && fm->toolbar_hover == i);
        wd_bevel_rect(bx, by, TB_BTN_W, TB_BTN_H,
                       pressed ? COLOR_DARK_GRAY : COLOR_WHITE,
                       pressed ? COLOR_WHITE : COLOR_DARK_GRAY,
                       THEME_BUTTON_FACE);
        const uint8_t *icon = tb_get_icon(i);
        if (icon) wd_icon_16(bx + 4, by + 3, icon);

        /* Separator after Up button */
        if (i == TB_UP) {
            int sx = bx + TB_BTN_W + TB_BTN_PAD + 1;
            wd_vline(sx, 2, TB_BTN_H, COLOR_DARK_GRAY);
            wd_vline(sx + 1, 2, TB_BTN_H, COLOR_WHITE);
        }
    }

    /* Path field: sunken white field after the last button */
    int addr_x = tb_btn_x(TB_DELETE) + TB_BTN_W + TB_BTN_PAD + 4;
    int addr_w = cw - addr_x - 4;
    if (addr_w > 0) {
        wd_hline(addr_x, by, addr_w, COLOR_DARK_GRAY);
        wd_vline(addr_x, by, TB_BTN_H, COLOR_DARK_GRAY);
        wd_hline(addr_x, by + TB_BTN_H - 1, addr_w, COLOR_WHITE);
        wd_vline(addr_x + addr_w - 1, by, TB_BTN_H, COLOR_WHITE);
        wd_fill_rect(addr_x + 1, by + 1, addr_w - 2, TB_BTN_H - 2, COLOR_WHITE);
        wd_text_ui(addr_x + 3, by + (TB_BTN_H - FONT_UI_HEIGHT) / 2,
                   fm->path, COLOR_BLACK, COLOR_WHITE);
    }
}

/*==========================================================================
 * Painting: scrollbar
 *=========================================================================*/

/* Draw a small arrow glyph for scrollbar buttons */
static void fm_draw_sb_arrow(int16_t cx, int16_t cy, bool up) {
    if (up) {
        wd_pixel(cx, cy + 3, COLOR_BLACK);
        wd_hline(cx - 1, cy + 4, 3, COLOR_BLACK);
        wd_hline(cx - 2, cy + 5, 5, COLOR_BLACK);
        wd_hline(cx - 3, cy + 6, 7, COLOR_BLACK);
    } else {
        wd_hline(cx - 3, cy + 3, 7, COLOR_BLACK);
        wd_hline(cx - 2, cy + 4, 5, COLOR_BLACK);
        wd_hline(cx - 1, cy + 5, 3, COLOR_BLACK);
        wd_pixel(cx, cy + 6, COLOR_BLACK);
    }
}

static void fm_paint_scrollbar(filemanager_t *fm, int16_t cw, int16_t ch) {
    int16_t fah = fm_file_area_height(ch);
    int16_t sx = cw - FN_SCROLLBAR_W;
    int16_t sy = FN_HEADER_HEIGHT;
    int16_t sb_w = FN_SCROLLBAR_W;
    int16_t btn_h = sb_w; /* square arrow buttons */

    if (fah < btn_h * 2 + 8) return; /* not enough room */

    /* Up arrow button */
    wd_bevel_rect(sx, sy, sb_w, btn_h,
                   COLOR_WHITE, COLOR_DARK_GRAY, THEME_BUTTON_FACE);
    fm_draw_sb_arrow(sx + sb_w / 2, sy, true);

    /* Down arrow button */
    wd_bevel_rect(sx, sy + fah - btn_h, sb_w, btn_h,
                   COLOR_WHITE, COLOR_DARK_GRAY, THEME_BUTTON_FACE);
    fm_draw_sb_arrow(sx + sb_w / 2, sy + fah - btn_h, false);

    /* Track area between arrow buttons */
    int16_t track_y = sy + btn_h;
    int16_t track_h = fah - btn_h * 2;

    /* Fill track with dithered pattern */
    for (int16_t y = 0; y < track_h; y++) {
        for (int16_t x = 0; x < sb_w; x++) {
            uint8_t c = ((x + y) & 1) ? COLOR_WHITE : COLOR_LIGHT_GRAY;
            wd_pixel(sx + x, track_y + y, c);
        }
    }

    if (fm->content_height <= fah || track_h < 8) return;

    /* Thumb proportional to visible area */
    int thumb_h = (int)track_h * fah / fm->content_height;
    if (thumb_h < 16) thumb_h = 16;
    if (thumb_h > track_h) thumb_h = track_h;
    int max_scroll = fm->content_height - fah;
    int thumb_y = 0;
    if (max_scroll > 0)
        thumb_y = (int)fm->scroll_y * (track_h - thumb_h) / max_scroll;

    wd_bevel_rect(sx, track_y + thumb_y, sb_w, thumb_h,
                   COLOR_WHITE, COLOR_DARK_GRAY, THEME_BUTTON_FACE);
}

/*==========================================================================
 * Painting: status bar
 *=========================================================================*/

static void fm_paint_statusbar(filemanager_t *fm, int16_t cw, int16_t ch) {
    int16_t sy = ch - FN_STATUSBAR_H;

    /* Background and sunken bevel */
    wd_fill_rect(0, sy, cw, FN_STATUSBAR_H, THEME_BUTTON_FACE);
    /* Top edge: sunken look */
    wd_hline(0, sy, cw, COLOR_DARK_GRAY);
    wd_hline(0, sy + 1, cw, COLOR_WHITE);

    /* File count and total size */
    uint32_t total_size = 0;
    uint16_t file_count = 0;
    for (int i = 0; i < (int)fm->entry_count; i++) {
        if (!(fm->entries[i].attrib & AM_DIR)) {
            file_count++;
            total_size += fm->entries[i].size;
        }
    }

    /* Format: "XX file(s)  XX.X KB" */
    char left[48];
    if (total_size < 1024)
        snprintf(left, sizeof(left), "%u file(s)  %lu B",
                 file_count, (unsigned long)total_size);
    else if (total_size < 1024UL * 1024)
        snprintf(left, sizeof(left), "%u file(s)  %lu KB",
                 file_count, (unsigned long)(total_size / 1024));
    else
        snprintf(left, sizeof(left), "%u file(s)  %lu MB",
                 file_count, (unsigned long)(total_size / (1024UL * 1024)));

    /* Sunken panel for file info */
    int panel_w = (int)strlen(left) * FONT_UI_WIDTH + 8;
    if (panel_w > cw / 2) panel_w = cw / 2;
    wd_hline(4, sy + 3, panel_w, COLOR_DARK_GRAY);
    wd_vline(4, sy + 3, FN_STATUSBAR_H - 6, COLOR_DARK_GRAY);
    wd_hline(4, sy + FN_STATUSBAR_H - 4, panel_w, COLOR_WHITE);
    wd_vline(4 + panel_w, sy + 3, FN_STATUSBAR_H - 6, COLOR_WHITE);
    wd_text_ui(8, sy + (FN_STATUSBAR_H - FONT_UI_HEIGHT) / 2,
               left, COLOR_BLACK, THEME_BUTTON_FACE);

    /* Free space on SD card */
    DWORD free_clust = 0;
    FATFS *fs = NULL;
    char right[32];
    right[0] = '\0';
    if (f_getfree("/", &free_clust, &fs) == FR_OK && fs) {
        uint32_t free_bytes = (uint32_t)(free_clust * fs->csize) * 512;
        uint32_t free_kb = free_bytes / 1024;
        if (free_kb < 1024)
            snprintf(right, sizeof(right), "%lu KB free", (unsigned long)free_kb);
        else
            snprintf(right, sizeof(right), "%lu MB free",
                     (unsigned long)(free_kb / 1024));
    }

    if (right[0]) {
        int rw = (int)strlen(right) * FONT_UI_WIDTH + 8;
        int rx = cw - rw - 4;
        if (rx < panel_w + 12) rx = panel_w + 12;
        wd_hline(rx, sy + 3, rw, COLOR_DARK_GRAY);
        wd_vline(rx, sy + 3, FN_STATUSBAR_H - 6, COLOR_DARK_GRAY);
        wd_hline(rx, sy + FN_STATUSBAR_H - 4, rw, COLOR_WHITE);
        wd_vline(rx + rw, sy + 3, FN_STATUSBAR_H - 6, COLOR_WHITE);
        wd_text_ui(rx + 4, sy + (FN_STATUSBAR_H - FONT_UI_HEIGHT) / 2,
                   right, COLOR_BLACK, THEME_BUTTON_FACE);
    }
}

/*==========================================================================
 * Painting: large icon view
 *=========================================================================*/

static void fm_paint_large_icons(filemanager_t *fm, int16_t cw, int16_t ch) {
    int16_t file_w = cw - FN_SCROLLBAR_W;
    int cols = file_w / LARGE_CELL_W;
    if (cols < 1) cols = 1;

    for (int i = 0; i < (int)fm->entry_count; i++) {
        fn_entry_t *e = &fm->entries[i];
        int col = i % cols;
        int row = i / cols;
        int16_t cx = col * LARGE_CELL_W;
        int16_t cy = FN_HEADER_HEIGHT + row * LARGE_CELL_H - fm->scroll_y;

        /* Clip check: between toolbar and status bar */
        if (cy + LARGE_CELL_H < FN_HEADER_HEIGHT ||
            cy >= ch - FN_STATUSBAR_H) continue;

        bool selected = (e->sel_flags & FN_SEL_SELECTED) != 0;
        bool dimmed = (e->sel_flags & FN_SEL_CUT) != 0;

        /* Selection highlight */
        if (selected) {
            wd_fill_rect(cx + (LARGE_CELL_W - 32) / 2 - 2,
                          cy + 1, 36, 34, COLOR_BLUE);
        }

        /* 32x32 icon centered — use cached .inf icon scaled 2x if available */
        if (e->icon_idx >= 0 && e->icon_idx < (int8_t)fn_app_icon_count) {
            /* Scale 16x16 icon to 32x32 (each pixel becomes 2x2) */
            const uint8_t *src = fn_app_icons[e->icon_idx];
            int16_t ix = cx + (LARGE_CELL_W - 32) / 2;
            int16_t iy = cy + 2;
            for (int r = 0; r < 16; r++) {
                for (int c = 0; c < 16; c++) {
                    uint8_t px = src[r * 16 + c];
                    if (px != 0xFF) {
                        wd_pixel(ix + c * 2,     iy + r * 2,     px);
                        wd_pixel(ix + c * 2 + 1, iy + r * 2,     px);
                        wd_pixel(ix + c * 2,     iy + r * 2 + 1, px);
                        wd_pixel(ix + c * 2 + 1, iy + r * 2 + 1, px);
                    }
                }
            }
        } else {
            const uint8_t *icon = fn_get_icon_32(e);
            wd_icon_32(cx + (LARGE_CELL_W - 32) / 2, cy + 2, icon);
        }

        /* Filename (truncated) */
        char label[14];
        strncpy(label, e->name, 13);
        label[13] = '\0';
        int tw = (int)strlen(label) * FONT_UI_WIDTH;
        int tx = cx + (LARGE_CELL_W - tw) / 2;

        uint8_t fg = selected ? COLOR_WHITE : (dimmed ? COLOR_DARK_GRAY : COLOR_BLACK);
        uint8_t bg = selected ? COLOR_BLUE : COLOR_WHITE;
        if (selected) {
            wd_fill_rect(tx - 1, cy + 36, tw + 2, FONT_UI_HEIGHT + 2, COLOR_BLUE);
        }
        wd_text_ui(tx, cy + 37, label, fg, bg);
    }
}

/*==========================================================================
 * Painting: small icon view
 *=========================================================================*/

static void fm_paint_small_icons(filemanager_t *fm, int16_t cw, int16_t ch) {
    int16_t file_w = cw - FN_SCROLLBAR_W;
    int cols = file_w / SMALL_CELL_W;
    if (cols < 1) cols = 1;

    for (int i = 0; i < (int)fm->entry_count; i++) {
        fn_entry_t *e = &fm->entries[i];
        int col = i % cols;
        int row = i / cols;
        int16_t cx = col * SMALL_CELL_W;
        int16_t cy = FN_HEADER_HEIGHT + row * SMALL_CELL_H - fm->scroll_y;

        if (cy + SMALL_CELL_H < FN_HEADER_HEIGHT ||
            cy >= ch - FN_STATUSBAR_H) continue;

        bool selected = (e->sel_flags & FN_SEL_SELECTED) != 0;
        bool dimmed = (e->sel_flags & FN_SEL_CUT) != 0;

        uint8_t fg = selected ? COLOR_WHITE : (dimmed ? COLOR_DARK_GRAY : COLOR_BLACK);
        uint8_t bg = selected ? COLOR_BLUE : COLOR_WHITE;

        if (selected) {
            wd_fill_rect(cx, cy, SMALL_CELL_W, SMALL_CELL_H, COLOR_BLUE);
        }

        wd_icon_16(cx + 2, cy + 2, fn_get_icon_16(e));
        wd_text_ui(cx + 20, cy + (SMALL_CELL_H - FONT_UI_HEIGHT) / 2,
                   e->name, fg, bg);
    }
}

/*==========================================================================
 * Painting: list view
 *=========================================================================*/

/* Draw a sort indicator arrow (small triangle) in the column header */
static void fm_draw_sort_arrow(int16_t x, int16_t y, bool ascending) {
    if (ascending) {
        /* Up arrow: ▲ */
        wd_pixel(x + 3, y, COLOR_BLACK);
        wd_hline(x + 2, y + 1, 3, COLOR_BLACK);
        wd_hline(x + 1, y + 2, 5, COLOR_BLACK);
        wd_hline(x,     y + 3, 7, COLOR_BLACK);
    } else {
        /* Down arrow: ▼ */
        wd_hline(x,     y, 7, COLOR_BLACK);
        wd_hline(x + 1, y + 1, 5, COLOR_BLACK);
        wd_hline(x + 2, y + 2, 3, COLOR_BLACK);
        wd_pixel(x + 3, y + 3, COLOR_BLACK);
    }
}

static void fm_paint_list(filemanager_t *fm, int16_t cw, int16_t ch) {
    int16_t file_w = cw - FN_SCROLLBAR_W;

    /* Column widths from state */
    int name_w = fm->col_name_w;
    int size_w = fm->col_size_w;
    int type_w = file_w - name_w - size_w;
    if (type_w < 40) type_w = 40;

    /* Header row */
    int16_t hy = FN_HEADER_HEIGHT;
    wd_bevel_rect(0, hy, name_w, LIST_HDR_H,
                   COLOR_WHITE, COLOR_DARK_GRAY, THEME_BUTTON_FACE);
    wd_text_ui(4, hy + (LIST_HDR_H - FONT_UI_HEIGHT) / 2,
               "Name", COLOR_BLACK, THEME_BUTTON_FACE);
    if (fm->sort_column == 0)
        fm_draw_sort_arrow(name_w - 14, hy + (LIST_HDR_H - 4) / 2,
                           fm->sort_ascending);

    wd_bevel_rect(name_w, hy, size_w, LIST_HDR_H,
                   COLOR_WHITE, COLOR_DARK_GRAY, THEME_BUTTON_FACE);
    wd_text_ui(name_w + 4, hy + (LIST_HDR_H - FONT_UI_HEIGHT) / 2,
               "Size", COLOR_BLACK, THEME_BUTTON_FACE);
    if (fm->sort_column == 1)
        fm_draw_sort_arrow(name_w + size_w - 14, hy + (LIST_HDR_H - 4) / 2,
                           fm->sort_ascending);

    wd_bevel_rect(name_w + size_w, hy, type_w, LIST_HDR_H,
                   COLOR_WHITE, COLOR_DARK_GRAY, THEME_BUTTON_FACE);
    wd_text_ui(name_w + size_w + 4, hy + (LIST_HDR_H - FONT_UI_HEIGHT) / 2,
               "Type", COLOR_BLACK, THEME_BUTTON_FACE);
    if (fm->sort_column == 2)
        fm_draw_sort_arrow(name_w + size_w + type_w - 14,
                           hy + (LIST_HDR_H - 4) / 2, fm->sort_ascending);

    /* Rows */
    for (int i = 0; i < (int)fm->entry_count; i++) {
        fn_entry_t *e = &fm->entries[i];
        int16_t ry = FN_HEADER_HEIGHT + LIST_HDR_H + i * LIST_ROW_H - fm->scroll_y;

        if (ry + LIST_ROW_H < FN_HEADER_HEIGHT + LIST_HDR_H ||
            ry >= ch - FN_STATUSBAR_H) continue;

        bool selected = (e->sel_flags & FN_SEL_SELECTED) != 0;
        bool dimmed = (e->sel_flags & FN_SEL_CUT) != 0;

        uint8_t fg = selected ? COLOR_WHITE : (dimmed ? COLOR_DARK_GRAY : COLOR_BLACK);
        uint8_t bg = selected ? COLOR_BLUE : COLOR_WHITE;

        if (selected) {
            wd_fill_rect(0, ry, file_w, LIST_ROW_H, COLOR_BLUE);
        }

        /* Icon + name */
        wd_icon_16(2, ry + 1, fn_get_icon_16(e));
        wd_text_ui(20, ry + (LIST_ROW_H - FONT_UI_HEIGHT) / 2,
                   e->name, fg, bg);

        /* Size */
        if (!(e->attrib & AM_DIR)) {
            char sbuf[16];
            if (e->size < 1024)
                snprintf(sbuf, sizeof(sbuf), "%lu B", (unsigned long)e->size);
            else if (e->size < 1024UL * 1024)
                snprintf(sbuf, sizeof(sbuf), "%lu KB",
                         (unsigned long)(e->size / 1024));
            else
                snprintf(sbuf, sizeof(sbuf), "%lu MB",
                         (unsigned long)(e->size / (1024UL * 1024)));
            wd_text_ui(name_w + 4, ry + (LIST_ROW_H - FONT_UI_HEIGHT) / 2,
                       sbuf, fg, bg);
        }

        /* Type */
        const char *type_str = (e->attrib & AM_DIR) ? "Folder" :
                               e->is_executable ? "Application" : "File";
        wd_text_ui(name_w + size_w + 4, ry + (LIST_ROW_H - FONT_UI_HEIGHT) / 2,
                   type_str, fg, bg);
    }
}

/*==========================================================================
 * Paint handler
 *=========================================================================*/

static void fm_paint(hwnd_t hwnd) {
    filemanager_t *fm = fm_from_hwnd(hwnd);
    if (!fm) return;

    rect_t cr = wm_get_client_rect(hwnd);
    int16_t cw = cr.w;
    int16_t ch = cr.h;

    fm->content_height = fm_calc_content_height(fm, cw);
    fm_clamp_scroll(fm, ch);

    /* Determine whether scrollbar is needed */
    int16_t fah = fm_file_area_height(ch);
    bool need_scrollbar = (fm->content_height > fah && fah > 0);

    /* Clear file area background (full width — between toolbar and status bar) */
    wd_fill_rect(0, FN_HEADER_HEIGHT, cw,
                  ch - FN_HEADER_HEIGHT - FN_STATUSBAR_H, COLOR_WHITE);

    /* File view — painted before toolbar so toolbar covers any
     * items that scroll into the toolbar region */
    switch (fm->view_mode) {
    case FN_VIEW_LARGE_ICONS:
        fm_paint_large_icons(fm, cw, ch);
        break;
    case FN_VIEW_SMALL_ICONS:
        fm_paint_small_icons(fm, cw, ch);
        break;
    case FN_VIEW_LIST:
        fm_paint_list(fm, cw, ch);
        break;
    }

    /* Toolbar (painted after file view to cover scrolled items) */
    fm_paint_toolbar(fm, cw);

    /* Scrollbar — only when content overflows */
    if (need_scrollbar)
        fm_paint_scrollbar(fm, cw, ch);

    /* Status bar */
    fm_paint_statusbar(fm, cw, ch);

    /* Rubber band */
    if (fm->rubber_active) {
        int16_t rx0 = fm->rubber_x0 < fm->rubber_x1 ? fm->rubber_x0 : fm->rubber_x1;
        int16_t ry0 = fm->rubber_y0 < fm->rubber_y1 ? fm->rubber_y0 : fm->rubber_y1;
        int16_t rx1 = fm->rubber_x0 > fm->rubber_x1 ? fm->rubber_x0 : fm->rubber_x1;
        int16_t ry1 = fm->rubber_y0 > fm->rubber_y1 ? fm->rubber_y0 : fm->rubber_y1;
        wd_rect(rx0, ry0, rx1 - rx0 + 1, ry1 - ry0 + 1, COLOR_BLACK);
    }

    /* Tooltip — drawn above toolbar button in screen coordinates */
    if (fm->tooltip_btn >= 0 && !fm->toolbar_pressed) {
        if ((xTaskGetTickCount() - fm->tooltip_hover_tick) >= pdMS_TO_TICKS(1000)) {
            const char *tip = tb_get_label(fm->tooltip_btn);
            if (tip) {
                window_t *win = wm_get_window(hwnd);
                if (win) {
                    point_t co = theme_client_origin(&win->frame, win->flags);
                    int tw = (int)strlen(tip) * FONT_UI_WIDTH;
                    int ph = FONT_UI_HEIGHT + 4;
                    int pw = tw + 6;
                    int sx = co.x + tb_btn_x(fm->tooltip_btn);
                    int sy = co.y - ph + 1;
                    if (sy < 0) sy = co.y + 2 + TB_BTN_H + 4;
                    gfx_fill_rect(sx, sy, pw, ph, COLOR_WHITE);
                    gfx_rect(sx, sy, pw, ph, COLOR_BLACK);
                    gfx_text_ui(sx + 3, sy + 2, tip, COLOR_BLACK, COLOR_WHITE);
                }
            }
        } else {
            /* Delay not elapsed yet — schedule repaint so tooltip appears */
            wm_mark_dirty();
        }
    }
}

/*==========================================================================
 * Progress state for copy/delete operations
 *=========================================================================*/

static uint32_t copy_total_bytes;
static uint32_t copy_done_bytes;
static uint32_t copy_last_update;  /* tick of last progress redraw */
static int      copy_total_files;
static int      copy_done_files;

/*==========================================================================
 * File operations
 *=========================================================================*/

static void fm_delete_selected(filemanager_t *fm) {
    if (fm->selection_count == 0) return;
    dialog_show(fm->hwnd, "Confirm Delete",
                "Delete selected item(s)?",
                DLG_ICON_WARNING, DLG_BTN_YES | DLG_BTN_NO);
}

/* Forward declaration */
static void fm_show_progress(const char *label, int percent);

/* Recursively delete a file or directory */
static void fm_delete_recursive(const char *path) {
    FILINFO fno;
    if (f_stat(path, &fno) != FR_OK) return;

    if (fno.fattrib & AM_DIR) {
        DIR dir;
        if (f_opendir(&dir, path) == FR_OK) {
            while (f_readdir(&dir, &fno) == FR_OK && fno.fname[0] != 0) {
                char child[FN_PATH_MAX];
                snprintf(child, sizeof(child), "%s/%s", path, fno.fname);
                fm_delete_recursive(child);
            }
            f_closedir(&dir);
        }
    }
    f_unlink(path);
}

static void fm_do_delete(filemanager_t *fm) {
    char full[FN_PATH_MAX];
    int total = 0, done = 0;
    /* Count selected items */
    for (int i = 0; i < (int)fm->entry_count; i++)
        if (fm->entries[i].sel_flags & FN_SEL_SELECTED) total++;
    copy_total_files = 0;  /* clear so progress doesn't show file counts */
    copy_done_files = 0;
    if (total > 1)
        fm_show_progress("Deleting", 0);

    for (int i = 0; i < (int)fm->entry_count; i++) {
        if (!(fm->entries[i].sel_flags & FN_SEL_SELECTED)) continue;
        if (fm->path[1] == '\0')
            snprintf(full, sizeof(full), "/%s", fm->entries[i].name);
        else
            snprintf(full, sizeof(full), "%s/%s", fm->path, fm->entries[i].name);
        fm_delete_recursive(full);
        /* Also delete companion .inf for executables */
        if (fm->entries[i].is_executable) {
            char inf[FN_PATH_MAX];
            snprintf(inf, sizeof(inf), "%s.inf", full);
            f_unlink(inf);
        }
        done++;
        if (total > 1)
            fm_show_progress("Deleting", done * 100 / total);
    }
    fm_refresh(fm);
    wm_invalidate(fm->hwnd);
}

static void fm_new_folder(filemanager_t *fm) {
    fm->pending_rename = 0;
    dialog_input_show(fm->hwnd, "New Folder", "Name:", NULL, 60);
}

static void fm_rename_selected(filemanager_t *fm) {
    if (fm->focus_index < 0 || fm->focus_index >= (int)fm->entry_count) return;
    fm->pending_rename = 1;
    dialog_input_show(fm->hwnd, "Rename",
                      "New name:", fm->entries[fm->focus_index].name, 60);
}

static void fm_open_item(filemanager_t *fm, int idx) {
    if (idx < 0 || idx >= (int)fm->entry_count) return;
    fn_entry_t *e = &fm->entries[idx];

    char full[FN_PATH_MAX];
    if (fm->path[1] == '\0')
        snprintf(full, sizeof(full), "/%s", e->name);
    else
        snprintf(full, sizeof(full), "%s/%s", fm->path, e->name);

    if (e->attrib & AM_DIR) {
        fm_navigate(fm, full);
    } else if (e->is_executable) {
        /* Load icon from companion .inf and launch */
        char inf_path[FN_PATH_MAX];
        snprintf(inf_path, sizeof(inf_path), "%s.inf", full);

        extern const uint8_t default_icon_16x16[256];
        static uint8_t app_icon[256];
        const uint8_t *icon = default_icon_16x16;

        /* Try to read 16x16 icon from .inf (after name line) */
        FIL f;
        if (f_open(&f, inf_path, FA_READ) == FR_OK) {
            UINT br;
            char ch;
            /* Skip past name line */
            while (f_read(&f, &ch, 1, &br) == FR_OK && br == 1) {
                if (ch == '\n') break;
            }
            if (f_read(&f, app_icon, 256, &br) == FR_OK && br == 256)
                icon = app_icon;
            f_close(&f);
        }
        wm_set_pending_icon(icon);
        launch_elf_app(full);
    }
}

/*==========================================================================
 * Clipboard operations
 *=========================================================================*/

static void fm_clip_cut(filemanager_t *fm) {
    fn_clipboard.count = 0;
    fn_clipboard.is_cut = true;
    for (int i = 0; i < (int)fm->entry_count && fn_clipboard.count < 16; i++) {
        if (!(fm->entries[i].sel_flags & FN_SEL_SELECTED)) continue;
        char *p = fn_clipboard.paths[fn_clipboard.count];
        if (fm->path[1] == '\0')
            snprintf(p, FN_PATH_MAX, "/%s", fm->entries[i].name);
        else
            snprintf(p, FN_PATH_MAX, "%s/%s", fm->path, fm->entries[i].name);
        fm->entries[i].sel_flags |= FN_SEL_CUT;
        fn_clipboard.count++;
        /* Also add companion .inf for executables */
        if (fm->entries[i].is_executable && fn_clipboard.count < 16) {
            char *ip = fn_clipboard.paths[fn_clipboard.count];
            snprintf(ip, FN_PATH_MAX, "%s.inf", p);
            fn_clipboard.count++;
        }
    }
    wm_invalidate(fm->hwnd);
}

static void fm_clip_copy(filemanager_t *fm) {
    fn_clipboard.count = 0;
    fn_clipboard.is_cut = false;
    for (int i = 0; i < (int)fm->entry_count && fn_clipboard.count < 16; i++) {
        if (!(fm->entries[i].sel_flags & FN_SEL_SELECTED)) continue;
        char *p = fn_clipboard.paths[fn_clipboard.count];
        if (fm->path[1] == '\0')
            snprintf(p, FN_PATH_MAX, "/%s", fm->entries[i].name);
        else
            snprintf(p, FN_PATH_MAX, "%s/%s", fm->path, fm->entries[i].name);
        fn_clipboard.count++;
        /* Also add companion .inf for executables */
        if (fm->entries[i].is_executable && fn_clipboard.count < 16) {
            char *ip = fn_clipboard.paths[fn_clipboard.count];
            snprintf(ip, FN_PATH_MAX, "%s.inf", p);
            fn_clipboard.count++;
        }
    }
}

/*==========================================================================
 * Progress overlay — drawn directly to framebuffer during blocking copy
 *=========================================================================*/

/* Draw a progress overlay with percentage and file count.
 * Reads current mouse position and redraws cursor so mouse stays responsive. */
static void fm_show_progress(const char *label, int percent) {
    char msg[64];
    if (copy_total_files > 0)
        snprintf(msg, sizeof(msg), "%s: %d%% (%d of %d)...",
                 label, percent, copy_done_files, copy_total_files);
    else if (percent >= 0)
        snprintf(msg, sizeof(msg), "%s: %d%%...", label, percent);
    else
        snprintf(msg, sizeof(msg), "%s...", label);

    /* Redraw overlay directly to framebuffer */
    int bw = 220, bh = 50;
    int bx = (DISPLAY_WIDTH - bw) / 2;
    int by = (DISPLAY_HEIGHT - bh) / 2;

    gfx_fill_rect(bx, by, bw, bh, THEME_BUTTON_FACE);
    /* Raised border */
    gfx_hline(bx, by, bw, COLOR_WHITE);
    gfx_vline(bx, by, bh, COLOR_WHITE);
    gfx_hline(bx, by + bh - 1, bw, COLOR_DARK_GRAY);
    gfx_vline(bx + bw - 1, by, bh, COLOR_DARK_GRAY);

    /* Centered message */
    int tw = (int)strlen(msg) * FONT_UI_WIDTH;
    gfx_text_ui(bx + (bw - tw) / 2, by + (bh - FONT_UI_HEIGHT) / 2,
                msg, COLOR_BLACK, THEME_BUTTON_FACE);

    display_swap_buffers();

    /* Yield to let other tasks run */
    vTaskDelay(1);
}

/*==========================================================================
 * Recursive file/folder copy via FatFS (with progress)
 *=========================================================================*/

/* Pre-scan to calculate total size of a source path */
static uint32_t fm_calc_copy_size(const char *src) {
    FILINFO fno;
    if (f_stat(src, &fno) != FR_OK) return 0;
    if (fno.fattrib & AM_DIR) {
        uint32_t total = 0;
        DIR dir;
        if (f_opendir(&dir, src) != FR_OK) return 0;
        while (1) {
            if (f_readdir(&dir, &fno) != FR_OK || fno.fname[0] == 0) break;
            if (fno.fname[0] == '.') continue;
            char child[FN_PATH_MAX];
            snprintf(child, sizeof(child), "%s/%s", src, fno.fname);
            total += fm_calc_copy_size(child);
        }
        f_closedir(&dir);
        return total;
    }
    return (uint32_t)fno.fsize;
}

/* Count files (not dirs) recursively for progress "N of Y" display */
static int fm_count_files(const char *src) {
    FILINFO fno;
    if (f_stat(src, &fno) != FR_OK) return 0;
    if (fno.fattrib & AM_DIR) {
        int count = 0;
        DIR dir;
        if (f_opendir(&dir, src) != FR_OK) return 0;
        while (1) {
            if (f_readdir(&dir, &fno) != FR_OK || fno.fname[0] == 0) break;
            if (fno.fname[0] == '.') continue;
            char child[FN_PATH_MAX];
            snprintf(child, sizeof(child), "%s/%s", src, fno.fname);
            count += fm_count_files(child);
        }
        f_closedir(&dir);
        return count;
    }
    return 1;
}

static void fm_copy_recursive(const char *src, const char *dst) {
    FILINFO fno;
    if (f_stat(src, &fno) != FR_OK) return;

    if (fno.fattrib & AM_DIR) {
        f_mkdir(dst);
        DIR dir;
        if (f_opendir(&dir, src) != FR_OK) return;
        while (1) {
            if (f_readdir(&dir, &fno) != FR_OK || fno.fname[0] == 0) break;
            if (fno.fname[0] == '.') continue;
            char src_child[FN_PATH_MAX], dst_child[FN_PATH_MAX];
            snprintf(src_child, sizeof(src_child), "%s/%s", src, fno.fname);
            snprintf(dst_child, sizeof(dst_child), "%s/%s", dst, fno.fname);
            fm_copy_recursive(src_child, dst_child);
        }
        f_closedir(&dir);
    } else {
        FIL sf, df;
        if (f_open(&sf, src, FA_READ) == FR_OK) {
            if (f_open(&df, dst, FA_WRITE | FA_CREATE_NEW) == FR_OK) {
                uint8_t buf[512];
                UINT br, bw;
                while (f_read(&sf, buf, sizeof(buf), &br) == FR_OK && br > 0) {
                    f_write(&df, buf, br, &bw);
                    copy_done_bytes += br;
                    /* Update progress every ~100ms */
                    uint32_t now = xTaskGetTickCount();
                    if (now - copy_last_update >= pdMS_TO_TICKS(100)) {
                        copy_last_update = now;
                        int pct = copy_total_bytes > 0
                            ? (int)((uint64_t)copy_done_bytes * 100 / copy_total_bytes)
                            : -1;
                        fm_show_progress("Copying", pct);
                    }
                }
                f_close(&df);
            }
            f_close(&sf);
        }
        copy_done_files++;
    }
}

static void fm_clip_paste(filemanager_t *fm) {
    if (fn_clipboard.count == 0) return;

    if (!fn_clipboard.is_cut) {
        /* Pre-scan total size and file count for progress display */
        copy_total_bytes = 0;
        copy_done_bytes = 0;
        copy_last_update = 0;
        copy_total_files = 0;
        copy_done_files = 0;
        for (int i = 0; i < fn_clipboard.count; i++) {
            copy_total_bytes += fm_calc_copy_size(fn_clipboard.paths[i]);
            copy_total_files += fm_count_files(fn_clipboard.paths[i]);
        }
        fm_show_progress("Copying", 0);
    }

    for (int i = 0; i < fn_clipboard.count; i++) {
        const char *name = strrchr(fn_clipboard.paths[i], '/');
        if (!name) continue;
        name++;

        char dest[FN_PATH_MAX];
        if (fm->path[1] == '\0')
            snprintf(dest, sizeof(dest), "/%s", name);
        else
            snprintf(dest, sizeof(dest), "%s/%s", fm->path, name);

        if (fn_clipboard.is_cut) {
            f_rename(fn_clipboard.paths[i], dest);
        } else {
            fm_copy_recursive(fn_clipboard.paths[i], dest);
        }
    }

    if (fn_clipboard.is_cut)
        fn_clipboard.count = 0;

    fm_refresh(fm);
    wm_invalidate(fm->hwnd);
}

/*==========================================================================
 * Context menu
 *=========================================================================*/

static void fm_show_context_menu(filemanager_t *fm, int16_t sx, int16_t sy,
                                  bool on_item) {
    menu_item_t items[8];
    uint8_t count = 0;

    if (on_item) {
        strncpy(items[count].text, "Open", sizeof(items[0].text));
        items[count].command_id = FN_CMD_CTX_OPEN;
        items[count].flags = 0;
        items[count].accel_key = 0;
        count++;

        items[count] = (menu_item_t){ "", 0, MIF_SEPARATOR, 0 };
        count++;

        strncpy(items[count].text, "Cut", sizeof(items[0].text));
        items[count].command_id = FN_CMD_CTX_CUT;
        items[count].flags = 0;
        items[count].accel_key = 0;
        count++;

        strncpy(items[count].text, "Copy", sizeof(items[0].text));
        items[count].command_id = FN_CMD_CTX_COPY;
        items[count].flags = 0;
        items[count].accel_key = 0;
        count++;

        items[count] = (menu_item_t){ "", 0, MIF_SEPARATOR, 0 };
        count++;

        strncpy(items[count].text, "Delete", sizeof(items[0].text));
        items[count].command_id = FN_CMD_CTX_DELETE;
        items[count].flags = 0;
        items[count].accel_key = 0;
        count++;

        strncpy(items[count].text, "Rename", sizeof(items[0].text));
        items[count].command_id = FN_CMD_CTX_RENAME;
        items[count].flags = 0;
        items[count].accel_key = 0;
        count++;
    } else {
        strncpy(items[count].text, "Paste", sizeof(items[0].text));
        items[count].command_id = FN_CMD_CTX_PASTE;
        items[count].flags = (fn_clipboard.count == 0) ? MIF_DISABLED : 0;
        items[count].accel_key = 0;
        count++;

        items[count] = (menu_item_t){ "", 0, MIF_SEPARATOR, 0 };
        count++;

        strncpy(items[count].text, "New Folder", sizeof(items[0].text));
        items[count].command_id = FN_CMD_CTX_NEW_FOLDER;
        items[count].flags = 0;
        items[count].accel_key = 0;
        count++;

        items[count] = (menu_item_t){ "", 0, MIF_SEPARATOR, 0 };
        count++;

        strncpy(items[count].text, "Refresh", sizeof(items[0].text));
        items[count].command_id = FN_CMD_CTX_REFRESH;
        items[count].flags = 0;
        items[count].accel_key = 0;
        count++;
    }

    menu_popup_show(fm->hwnd, sx, sy, items, count);
}

/*==========================================================================
 * Scrollbar mouse handling
 *=========================================================================*/

static bool fm_scrollbar_click(filemanager_t *fm, int16_t mx, int16_t my,
                                int16_t cw, int16_t ch) {
    int16_t sx = cw - FN_SCROLLBAR_W;
    if (mx < sx) return false;

    int16_t fah = fm_file_area_height(ch);
    if (fm->content_height <= fah) return false;

    int16_t ry = my - FN_HEADER_HEIGHT;
    if (ry < 0 || ry >= fah) return false;

    int16_t btn_h = FN_SCROLLBAR_W; /* square buttons */
    int scroll_step = 32; /* pixels to scroll per arrow click */

    /* Up arrow button */
    if (ry < btn_h) {
        fm->scroll_y -= scroll_step;
        fm_clamp_scroll(fm, ch);
        wm_invalidate(fm->hwnd);
        return true;
    }

    /* Down arrow button */
    if (ry >= fah - btn_h) {
        fm->scroll_y += scroll_step;
        fm_clamp_scroll(fm, ch);
        wm_invalidate(fm->hwnd);
        return true;
    }

    /* Track area: proportional jump */
    int16_t track_y = btn_h;
    int16_t track_h = fah - btn_h * 2;
    int max_scroll = fm->content_height - fah;
    if (track_h > 0 && max_scroll > 0)
        fm->scroll_y = (int16_t)((ry - track_y) * max_scroll / track_h);
    fm_clamp_scroll(fm, ch);
    wm_invalidate(fm->hwnd);
    return true;
}

/*==========================================================================
 * Event handler
 *=========================================================================*/

static bool fm_event(hwnd_t hwnd, const window_event_t *event) {
    filemanager_t *fm = fm_from_hwnd(hwnd);
    if (!fm) return false;

    rect_t cr = wm_get_client_rect(hwnd);
    int16_t cw = cr.w;
    int16_t ch = cr.h;

    switch (event->type) {
    case WM_LBUTTONDOWN: {
        int16_t mx = event->mouse.x;
        int16_t my = event->mouse.y;

        /* Toolbar buttons */
        if (my < FN_TOOLBAR_HEIGHT && my >= 2 && my < 2 + TB_BTN_H) {
            for (int i = 0; i < TB_BTN_COUNT; i++) {
                int bx = tb_btn_x(i);
                int bw = tb_btn_w(i);
                if (mx >= bx && mx < bx + bw) {
                    fm->toolbar_hover = i;
                    fm->toolbar_pressed = 1;
                    fm->tooltip_btn = -1;
                    wm_invalidate(hwnd);
                    return true;
                }
            }
            return true;
        }

        /* List view column header: resize or sort */
        if (fm->view_mode == FN_VIEW_LIST &&
            my >= FN_HEADER_HEIGHT && my < FN_HEADER_HEIGHT + LIST_HDR_H) {
            int name_w = fm->col_name_w;
            int size_w = fm->col_size_w;
            /* Check for resize grab zones (4px around column edges) */
            #define COL_GRAB 4
            if (mx >= name_w - COL_GRAB && mx <= name_w + COL_GRAB) {
                fm->col_resize_active = 0; /* resizing Name column */
                fm->col_resize_start_x = mx;
                fm->col_resize_start_w = name_w;
                return true;
            }
            if (mx >= name_w + size_w - COL_GRAB &&
                mx <= name_w + size_w + COL_GRAB) {
                fm->col_resize_active = 1; /* resizing Size column */
                fm->col_resize_start_x = mx;
                fm->col_resize_start_w = size_w;
                return true;
            }
            /* Column header click — sort */
            if (mx < name_w) {
                /* Name column */
                if (fm->sort_column == 0)
                    fm->sort_ascending = !fm->sort_ascending;
                else { fm->sort_column = 0; fm->sort_ascending = 1; }
            } else if (mx < name_w + size_w) {
                /* Size column */
                if (fm->sort_column == 1)
                    fm->sort_ascending = !fm->sort_ascending;
                else { fm->sort_column = 1; fm->sort_ascending = 1; }
            } else {
                /* Type column */
                if (fm->sort_column == 2)
                    fm->sort_ascending = !fm->sort_ascending;
                else { fm->sort_column = 2; fm->sort_ascending = 1; }
            }
            fm_refresh(fm);
            wm_invalidate(hwnd);
            return true;
        }

        /* Scrollbar */
        if (fm_scrollbar_click(fm, mx, my, cw, ch))
            return true;

        /* File area hit-test */
        int16_t idx = fm_hit_test(fm, mx, my, cw);

        if (idx >= 0) {
            /* Check double-click */
            uint32_t now = xTaskGetTickCount();
            if (idx == fm->last_click_index &&
                (now - fm->last_click_tick) < pdMS_TO_TICKS(DBLCLICK_MS)) {
                fm_open_item(fm, idx);
                fm->last_click_index = -1;
                return true;
            }
            fm->last_click_index = idx;
            fm->last_click_tick = now;

            /* Selection logic */
            uint8_t mods = event->mouse.modifiers;
            if (mods & KMOD_CTRL) {
                fm->entries[idx].sel_flags ^= FN_SEL_SELECTED;
                fm->focus_index = idx;
            } else if (mods & KMOD_SHIFT) {
                if (fm->anchor_index >= 0) {
                    fm_clear_selection(fm);
                    int lo = fm->anchor_index < idx ? fm->anchor_index : idx;
                    int hi = fm->anchor_index > idx ? fm->anchor_index : idx;
                    for (int i = lo; i <= hi; i++)
                        fm->entries[i].sel_flags |= FN_SEL_SELECTED;
                }
                fm->focus_index = idx;
            } else {
                fm_clear_selection(fm);
                fm->entries[idx].sel_flags |= FN_SEL_SELECTED;
                fm->focus_index = idx;
                fm->anchor_index = idx;
            }
            fm_update_selection_count(fm);
        } else {
            /* Click on blank area — start rubber band */
            if (!(event->mouse.modifiers & KMOD_CTRL)) {
                fm_clear_selection(fm);
            }
            fm->rubber_active = 1;
            fm->rubber_x0 = fm->rubber_x1 = mx;
            fm->rubber_y0 = fm->rubber_y1 = my;
            fm->last_click_index = -1;
        }
        wm_invalidate(hwnd);
        return true;
    }

    case WM_LBUTTONUP: {
        /* End column resize */
        if (fm->col_resize_active >= 0) {
            fm->col_resize_active = -1;
            return true;
        }

        /* Toolbar button release — only fire action if still hovering
         * the same button that was originally pressed */
        if (fm->toolbar_pressed) {
            int8_t btn = fm->toolbar_hover;
            fm->toolbar_pressed = 0;
            fm->toolbar_hover = -1;
            wm_invalidate(hwnd);
            if (btn >= 0) {
                switch (btn) {
                case TB_BACK:   fm_go_back(fm); break;
                case TB_UP:     fm_go_up(fm); break;
                case TB_CUT:    fm_clip_cut(fm); break;
                case TB_COPY:   fm_clip_copy(fm); break;
                case TB_PASTE:  fm_clip_paste(fm); break;
                case TB_DELETE: fm_delete_selected(fm); break;
                }
            }
            return true;
        }

        /* End rubber band */
        if (fm->rubber_active) {
            fm->rubber_active = 0;
            wm_invalidate(hwnd);
        }
        return true;
    }

    case WM_MOUSEMOVE: {
        int16_t mx = event->mouse.x;
        int16_t my = event->mouse.y;

        /* Column resize drag */
        if (fm->col_resize_active >= 0) {
            int16_t delta = mx - fm->col_resize_start_x;
            int16_t new_w = fm->col_resize_start_w + delta;
            if (new_w < 40) new_w = 40;
            if (new_w > cw - FN_SCROLLBAR_W - 80) new_w = cw - FN_SCROLLBAR_W - 80;
            if (fm->col_resize_active == 0)
                fm->col_name_w = new_w;
            else
                fm->col_size_w = new_w;
            wm_invalidate(hwnd);
            return true;
        }

        /* Toolbar hover tracking while pressed */
        if (fm->toolbar_pressed) {
            int8_t new_hover = -1;
            if (my >= 2 && my < 2 + TB_BTN_H) {
                for (int i = 0; i < TB_BTN_COUNT; i++) {
                    int bx = tb_btn_x(i);
                    int bw = tb_btn_w(i);
                    if (mx >= bx && mx < bx + bw) {
                        new_hover = i;
                        break;
                    }
                }
            }
            if (new_hover != fm->toolbar_hover) {
                fm->toolbar_hover = new_hover;
                wm_invalidate(hwnd);
            }
            return true;
        }

        /* Rubber band update */
        if (fm->rubber_active) {
            fm->rubber_x1 = mx;
            fm->rubber_y1 = my;

            /* Select items intersecting the rubber band */
            int16_t rx0 = fm->rubber_x0 < fm->rubber_x1 ? fm->rubber_x0 : fm->rubber_x1;
            int16_t ry0 = fm->rubber_y0 < fm->rubber_y1 ? fm->rubber_y0 : fm->rubber_y1;
            int16_t rx1 = fm->rubber_x0 > fm->rubber_x1 ? fm->rubber_x0 : fm->rubber_x1;
            int16_t ry1 = fm->rubber_y0 > fm->rubber_y1 ? fm->rubber_y0 : fm->rubber_y1;

            int16_t file_w = cw - FN_SCROLLBAR_W;
            int cols, cell_w, cell_h;
            int y_off = FN_HEADER_HEIGHT;
            switch (fm->view_mode) {
            case FN_VIEW_LARGE_ICONS:
                cell_w = LARGE_CELL_W; cell_h = LARGE_CELL_H;
                cols = file_w / cell_w; if (cols < 1) cols = 1;
                break;
            case FN_VIEW_SMALL_ICONS:
                cell_w = SMALL_CELL_W; cell_h = SMALL_CELL_H;
                cols = file_w / cell_w; if (cols < 1) cols = 1;
                break;
            case FN_VIEW_LIST:
                cell_w = file_w; cell_h = LIST_ROW_H;
                cols = 1; y_off += LIST_HDR_H;
                break;
            default:
                cols = 1; cell_w = 1; cell_h = 1;
            }

            for (int i = 0; i < (int)fm->entry_count; i++) {
                int col = i % cols;
                int row = i / cols;
                int16_t ex = col * cell_w;
                int16_t ey = y_off + row * cell_h - fm->scroll_y;
                int16_t ex2 = ex + cell_w;
                int16_t ey2 = ey + cell_h;

                bool intersects = !(ex2 <= rx0 || ex >= rx1 ||
                                    ey2 <= ry0 || ey >= ry1);
                if (intersects)
                    fm->entries[i].sel_flags |= FN_SEL_SELECTED;
                else if (!(event->mouse.modifiers & KMOD_CTRL))
                    fm->entries[i].sel_flags &= ~FN_SEL_SELECTED;
            }
            fm_update_selection_count(fm);
            wm_invalidate(hwnd);
            return true;
        }

        /* Scrollbar drag */
        if (event->mouse.buttons & 0x01) {
            if (mx >= cw - FN_SCROLLBAR_W) {
                fm_scrollbar_click(fm, mx, my, cw, ch);
                return true;
            }
        }

        /* Tooltip hover tracking (non-pressed) */
        {
            int8_t new_tip = -1;
            if (!fm->toolbar_pressed && my >= 2 && my < 2 + TB_BTN_H) {
                for (int i = 0; i < TB_BTN_COUNT; i++) {
                    int bx = tb_btn_x(i);
                    if (mx >= bx && mx < bx + TB_BTN_W) {
                        new_tip = i;
                        break;
                    }
                }
            }
            if (new_tip != fm->tooltip_btn) {
                fm->tooltip_btn = new_tip;
                fm->tooltip_hover_tick = xTaskGetTickCount();
                wm_invalidate(hwnd);
            }
        }
        return true;
    }

    case WM_RBUTTONDOWN: {
        int16_t mx = event->mouse.x;
        int16_t my = event->mouse.y;
        int16_t idx = fm_hit_test(fm, mx, my, cw);

        if (idx >= 0 && !(fm->entries[idx].sel_flags & FN_SEL_SELECTED)) {
            fm_clear_selection(fm);
            fm->entries[idx].sel_flags |= FN_SEL_SELECTED;
            fm->focus_index = idx;
            fm_update_selection_count(fm);
            wm_invalidate(hwnd);
        }

        /* Convert to screen coordinates for popup */
        window_t *win = wm_get_window(hwnd);
        if (!win) return true;
        point_t origin = theme_client_origin(&win->frame, win->flags);
        fm_show_context_menu(fm, origin.x + mx, origin.y + my, idx >= 0);
        return true;
    }

    case WM_KEYDOWN: {
        uint8_t sc = event->key.scancode;
        uint8_t mods = event->key.modifiers;

        /* Ctrl+A: select all */
        if ((mods & KMOD_CTRL) && sc == 0x04 /* HID A */) {
            fm_select_all(fm);
            wm_invalidate(hwnd);
            return true;
        }
        /* Ctrl+C: copy */
        if ((mods & KMOD_CTRL) && sc == 0x06 /* HID C */) {
            fm_clip_copy(fm);
            return true;
        }
        /* Ctrl+X: cut */
        if ((mods & KMOD_CTRL) && sc == 0x1B /* HID X */) {
            fm_clip_cut(fm);
            return true;
        }
        /* Ctrl+V: paste */
        if ((mods & KMOD_CTRL) && sc == 0x19 /* HID V */) {
            fm_clip_paste(fm);
            return true;
        }
        /* Delete key */
        if (sc == 0x4C /* HID DELETE */) {
            fm_delete_selected(fm);
            return true;
        }
        /* Enter: open selected */
        if (sc == 0x28 /* HID ENTER */) {
            if (fm->focus_index >= 0)
                fm_open_item(fm, fm->focus_index);
            return true;
        }
        /* Backspace: go up */
        if (sc == 0x2A /* HID BACKSPACE */) {
            fm_go_up(fm);
            return true;
        }
        /* F5: refresh */
        if (sc == 0x3E /* HID F5 */) {
            fm_refresh(fm);
            wm_invalidate(hwnd);
            return true;
        }
        /* F2: rename */
        if (sc == 0x3B /* HID F2 */) {
            fm_rename_selected(fm);
            return true;
        }
        return false;
    }

    case WM_COMMAND: {
        uint16_t id = event->command.id;
        switch (id) {
        /* Dialog results */
        case DLG_RESULT_YES:
            fm_do_delete(fm);
            return true;
        case DLG_RESULT_NO:
        case DLG_RESULT_CANCEL:
            return true;
        case DLG_RESULT_INPUT: {
            const char *text = dialog_input_get_text();
            if (!text || text[0] == '\0') return true;
            char full[FN_PATH_MAX];
            if (fm->pending_rename) {
                /* Rename */
                if (fm->focus_index >= 0 && fm->focus_index < (int)fm->entry_count) {
                    char old[FN_PATH_MAX];
                    if (fm->path[1] == '\0') {
                        snprintf(old, sizeof(old), "/%s",
                                 fm->entries[fm->focus_index].name);
                        snprintf(full, sizeof(full), "/%s", text);
                    } else {
                        snprintf(old, sizeof(old), "%s/%s",
                                 fm->path, fm->entries[fm->focus_index].name);
                        snprintf(full, sizeof(full), "%s/%s", fm->path, text);
                    }
                    f_rename(old, full);
                    /* Also rename companion .inf for executables */
                    if (fm->entries[fm->focus_index].is_executable) {
                        char old_inf[FN_PATH_MAX], new_inf[FN_PATH_MAX];
                        snprintf(old_inf, sizeof(old_inf), "%s.inf", old);
                        snprintf(new_inf, sizeof(new_inf), "%s.inf", full);
                        f_rename(old_inf, new_inf);
                    }
                }
            } else {
                /* New folder */
                if (fm->path[1] == '\0')
                    snprintf(full, sizeof(full), "/%s", text);
                else
                    snprintf(full, sizeof(full), "%s/%s", fm->path, text);
                f_mkdir(full);
            }
            fm->pending_rename = 0;
            fm_refresh(fm);
            wm_invalidate(hwnd);
            return true;
        }

        /* File menu */
        case FN_CMD_NEW_FOLDER:
        case FN_CMD_CTX_NEW_FOLDER:
            fm_new_folder(fm);
            return true;
        case FN_CMD_DELETE:
        case FN_CMD_CTX_DELETE:
            fm_delete_selected(fm);
            return true;
        case FN_CMD_RENAME:
        case FN_CMD_CTX_RENAME:
            fm_rename_selected(fm);
            return true;
        case FN_CMD_CLOSE:
            wm_destroy_window(hwnd);
            vPortFree(fm);
            return true;

        /* Edit menu */
        case FN_CMD_CUT:
        case FN_CMD_CTX_CUT:
            fm_clip_cut(fm);
            return true;
        case FN_CMD_COPY:
        case FN_CMD_CTX_COPY:
            fm_clip_copy(fm);
            return true;
        case FN_CMD_PASTE:
        case FN_CMD_CTX_PASTE:
            fm_clip_paste(fm);
            return true;
        case FN_CMD_SELECT_ALL:
            fm_select_all(fm);
            wm_invalidate(hwnd);
            return true;

        /* View menu */
        case FN_CMD_LARGE_ICONS:
            fm->view_mode = FN_VIEW_LARGE_ICONS;
            fm->scroll_y = 0;
            wm_invalidate(hwnd);
            return true;
        case FN_CMD_SMALL_ICONS:
            fm->view_mode = FN_VIEW_SMALL_ICONS;
            fm->scroll_y = 0;
            wm_invalidate(hwnd);
            return true;
        case FN_CMD_LIST:
            fm->view_mode = FN_VIEW_LIST;
            fm->scroll_y = 0;
            wm_invalidate(hwnd);
            return true;
        case FN_CMD_REFRESH:
        case FN_CMD_CTX_REFRESH:
            fm_refresh(fm);
            wm_invalidate(hwnd);
            return true;

        /* Navigation */
        case FN_CMD_BACK:
            fm_go_back(fm);
            return true;
        case FN_CMD_UP:
            fm_go_up(fm);
            return true;
        case FN_CMD_CTX_OPEN:
        case FN_CMD_OPEN:
            if (fm->focus_index >= 0)
                fm_open_item(fm, fm->focus_index);
            return true;
        }
        return false;
    }

    case WM_CLOSE:
        wm_destroy_window(hwnd);
        vPortFree(fm);
        return true;

    default:
        return false;
    }
}

/*==========================================================================
 * Window creation
 *=========================================================================*/

hwnd_t filemanager_create(const char *initial_path) {
    filemanager_t *fm = (filemanager_t *)pvPortMalloc(sizeof(filemanager_t));
    if (!fm) return HWND_NULL;
    memset(fm, 0, sizeof(*fm));

    /* Set initial path */
    if (initial_path)
        strncpy(fm->path, initial_path, FN_PATH_MAX - 1);
    else
        strncpy(fm->path, "/", FN_PATH_MAX - 1);
    fm->path[FN_PATH_MAX - 1] = '\0';

    fm->view_mode = FN_VIEW_LARGE_ICONS;
    fm->focus_index = -1;
    fm->anchor_index = -1;
    fm->last_click_index = -1;
    fm->toolbar_hover = -1;
    fm->tooltip_btn = -1;
    fm->col_name_w = 200;
    fm->col_size_w = 80;
    fm->sort_column = 0;   /* sort by name */
    fm->sort_ascending = 1;
    fm->col_resize_active = -1;

    /* Create window */
    fm->hwnd = wm_create_window(
        30, 20, 500, 380,
        fm->path,
        WSTYLE_DEFAULT | WF_MENUBAR,
        fm_event, fm_paint);

    if (fm->hwnd == HWND_NULL) {
        vPortFree(fm);
        return HWND_NULL;
    }

    window_t *win = wm_get_window(fm->hwnd);
    if (win) {
        win->bg_color = COLOR_WHITE;
        win->user_data = fm;
    }

    /* Set up menu bar — heap-allocated to avoid ~800 byte stack frame */
    menu_bar_t *bar = (menu_bar_t *)pvPortMalloc(sizeof(menu_bar_t));
    if (bar) {
        memset(bar, 0, sizeof(*bar));
        bar->menu_count = 3;

        /* File menu */
        strncpy(bar->menus[0].title, "File", sizeof(bar->menus[0].title));
        bar->menus[0].accel_key = 0x09; /* HID F */
        bar->menus[0].item_count = 6;
        strncpy(bar->menus[0].items[0].text, "New Folder", 20);
        bar->menus[0].items[0].command_id = FN_CMD_NEW_FOLDER;
        bar->menus[0].items[1].flags = MIF_SEPARATOR;
        strncpy(bar->menus[0].items[2].text, "Delete", 20);
        bar->menus[0].items[2].command_id = FN_CMD_DELETE;
        strncpy(bar->menus[0].items[3].text, "Rename", 20);
        bar->menus[0].items[3].command_id = FN_CMD_RENAME;
        bar->menus[0].items[4].flags = MIF_SEPARATOR;
        strncpy(bar->menus[0].items[5].text, "Close", 20);
        bar->menus[0].items[5].command_id = FN_CMD_CLOSE;

        /* Edit menu */
        strncpy(bar->menus[1].title, "Edit", sizeof(bar->menus[1].title));
        bar->menus[1].accel_key = 0x08; /* HID E */
        bar->menus[1].item_count = 5;
        strncpy(bar->menus[1].items[0].text, "Cut", 20);
        bar->menus[1].items[0].command_id = FN_CMD_CUT;
        strncpy(bar->menus[1].items[1].text, "Copy", 20);
        bar->menus[1].items[1].command_id = FN_CMD_COPY;
        strncpy(bar->menus[1].items[2].text, "Paste", 20);
        bar->menus[1].items[2].command_id = FN_CMD_PASTE;
        bar->menus[1].items[3].flags = MIF_SEPARATOR;
        strncpy(bar->menus[1].items[4].text, "Select All", 20);
        bar->menus[1].items[4].command_id = FN_CMD_SELECT_ALL;

        /* View menu */
        strncpy(bar->menus[2].title, "View", sizeof(bar->menus[2].title));
        bar->menus[2].accel_key = 0x19; /* HID V */
        bar->menus[2].item_count = 5;
        strncpy(bar->menus[2].items[0].text, "Large Icons", 20);
        bar->menus[2].items[0].command_id = FN_CMD_LARGE_ICONS;
        strncpy(bar->menus[2].items[1].text, "Small Icons", 20);
        bar->menus[2].items[1].command_id = FN_CMD_SMALL_ICONS;
        strncpy(bar->menus[2].items[2].text, "List", 20);
        bar->menus[2].items[2].command_id = FN_CMD_LIST;
        bar->menus[2].items[3].flags = MIF_SEPARATOR;
        strncpy(bar->menus[2].items[4].text, "Refresh", 20);
        bar->menus[2].items[4].command_id = FN_CMD_REFRESH;

        menu_set(fm->hwnd, bar);
        vPortFree(bar);
    }

    /* Read initial directory */
    fm_refresh(fm);

    return fm->hwnd;
}

/*==========================================================================
 * Spawn helper (called from main.c / startmenu.c)
 *=========================================================================*/

void spawn_filemanager_window(void) {
    wm_set_pending_icon(fn_icon16_open_folder);
    hwnd_t hwnd = filemanager_create("/");
    if (hwnd != HWND_NULL) {
        wm_set_focus(hwnd);
        taskbar_invalidate();
    }
}
