/*
 * FRANK OS — Desktop shortcut manager
 * Copyright (c) 2026 Mikhail Matveev <xtreme@rh1.tech>
 * https://rh1.tech
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "desktop.h"
#include "file_assoc.h"
#include "filemanager.h"
#include "terminal.h"
#include "window.h"
#include "window_event.h"
#include "window_theme.h"
#include "gfx.h"
#include "font.h"
#include "display.h"
#include "taskbar.h"
#include "menu.h"
#include "dialog.h"
#include "app.h"
#include "ff.h"
#include "sdcard_init.h"
#include "FreeRTOS.h"
#include "task.h"
#include <string.h>
#include <stdio.h>
#include <ctype.h>

/*==========================================================================
 * Layout constants — column-first grid, top-left origin
 *=========================================================================*/

#define DT_CELL_W      76    /* icon cell width (same as Navigator large) */
#define DT_CELL_H      56    /* icon cell height */
#define DT_ICON_SIZE   32    /* 32x32 icons on desktop */
#define DT_MARGIN_X     4    /* left margin */
#define DT_MARGIN_Y     4    /* top margin */
#define DT_TEXT_LINES    2   /* max text lines under icon */

/*==========================================================================
 * Shortcut entry
 *=========================================================================*/

typedef struct {
    char    path[DESKTOP_PATH_MAX];       /* full path to file or app */
    char    name[DESKTOP_NAME_MAX];       /* display name */
    uint8_t icon[DESKTOP_ICON_SIZE];      /* 16x16 icon data */
    bool    has_icon;
    bool    is_app;                       /* true if it's an ELF app */
    bool    used;                         /* slot is in use */
} desktop_shortcut_t;

/*==========================================================================
 * State
 *=========================================================================*/

static desktop_shortcut_t dt_shortcuts[DESKTOP_MAX_SHORTCUTS];
static int dt_count = 0;
static bool dt_dirty = true;
static int dt_selected = -1;  /* index of selected icon, -1 = none */

/* For context menu */
static int dt_ctx_index = -1; /* which shortcut the context menu is for */
static hwnd_t dt_popup_owner = HWND_NULL;

/*==========================================================================
 * Helpers
 *=========================================================================*/

/* Extract filename from path */
static const char *path_basename(const char *path) {
    const char *slash = strrchr(path, '/');
    return slash ? slash + 1 : path;
}

/* Get extension without dot */
static const char *path_ext(const char *path) {
    const char *base = path_basename(path);
    const char *dot = strrchr(base, '.');
    if (!dot) return "";
    return dot + 1;
}

/* Check if path is an ELF app (has .inf companion) */
static bool is_app_path(const char *path) {
    if (path[0] == ':') return true;   /* built-in apps */
    char inf[DESKTOP_PATH_MAX + 4];
    snprintf(inf, sizeof(inf), "%s.inf", path);
    FILINFO fi;
    return f_stat(inf, &fi) == FR_OK;
}

/* Load icon from .inf file or use default */
static void load_app_icon(desktop_shortcut_t *sc) {
    char inf[DESKTOP_PATH_MAX + 4];
    snprintf(inf, sizeof(inf), "%s.inf", sc->path);

    FIL f;
    if (f_open(&f, inf, FA_READ) != FR_OK) return;

    /* Skip name line */
    UINT br;
    char ch;
    while (f_read(&f, &ch, 1, &br) == FR_OK && br == 1) {
        if (ch == '\n') break;
    }
    if (f_read(&f, sc->icon, DESKTOP_ICON_SIZE, &br) == FR_OK
        && br == DESKTOP_ICON_SIZE) {
        sc->has_icon = true;
    }
    f_close(&f);
}

/* Load display name from .inf file */
static void load_app_name(desktop_shortcut_t *sc) {
    char inf[DESKTOP_PATH_MAX + 4];
    snprintf(inf, sizeof(inf), "%s.inf", sc->path);

    FIL f;
    if (f_open(&f, inf, FA_READ) != FR_OK) return;

    UINT br;
    char buf[DESKTOP_NAME_MAX];
    if (f_read(&f, buf, DESKTOP_NAME_MAX - 1, &br) == FR_OK && br > 0) {
        buf[br] = '\0';
        char *nl = strchr(buf, '\n');
        if (nl) *nl = '\0';
        nl = strchr(buf, '\r');
        if (nl) *nl = '\0';
        if (buf[0]) strncpy(sc->name, buf, DESKTOP_NAME_MAX - 1);
    }
    f_close(&f);
}

/* Load icon for a file shortcut via its associated app */
static void load_file_icon(desktop_shortcut_t *sc) {
    const char *ext = path_ext(sc->path);
    const fa_app_t *app = file_assoc_find(ext);
    if (app && app->has_icon) {
        memcpy(sc->icon, app->icon, DESKTOP_ICON_SIZE);
        sc->has_icon = true;
    }
}

/*==========================================================================
 * Grid layout helpers
 *=========================================================================*/

static int dt_cols(void) {
    /* Number of icon columns in the work area */
    return (DISPLAY_WIDTH - DT_MARGIN_X * 2) / DT_CELL_W;
}

static int dt_rows(void) {
    int work_h = taskbar_work_area_height() - DT_MARGIN_Y * 2;
    return work_h / DT_CELL_H;
}

/* Get the screen rect for a shortcut at index idx (column-first layout). */
static void dt_get_cell_rect(int idx, int16_t *cx, int16_t *cy) {
    int cols = dt_cols();
    int rows = dt_rows();
    if (rows <= 0) rows = 1;
    int col = idx / rows;
    int row = idx % rows;
    *cx = DT_MARGIN_X + col * DT_CELL_W;
    *cy = DT_MARGIN_Y + row * DT_CELL_H;
}

/* Hit-test: find the shortcut index at screen position (x,y).
 * Returns -1 if none. */
static int dt_hit_test(int16_t x, int16_t y) {
    for (int i = 0; i < dt_count; i++) {
        if (!dt_shortcuts[i].used) continue;
        int16_t cx, cy;
        dt_get_cell_rect(i, &cx, &cy);
        if (x >= cx && x < cx + DT_CELL_W &&
            y >= cy && y < cy + DT_CELL_H)
            return i;
    }
    return -1;
}

/*==========================================================================
 * Persistence — simple binary format
 *
 * File: magic(4) + count(4) + entries[count]
 * Entry: path[DESKTOP_PATH_MAX]
 *=========================================================================*/

#define DT_MAGIC 0x44544F53  /* "DTOS" */

static void dt_save(void) {
    if (!sdcard_is_mounted()) return;

    FIL f;
    if (f_open(&f, DESKTOP_DAT_PATH, FA_WRITE | FA_CREATE_ALWAYS) != FR_OK)
        return;

    UINT bw;
    uint32_t magic = DT_MAGIC;
    f_write(&f, &magic, 4, &bw);
    uint32_t cnt = (uint32_t)dt_count;
    f_write(&f, &cnt, 4, &bw);

    for (int i = 0; i < dt_count; i++) {
        f_write(&f, dt_shortcuts[i].path, DESKTOP_PATH_MAX, &bw);
    }
    f_close(&f);
}

static void dt_load(void) {
    dt_count = 0;

    if (!sdcard_is_mounted()) return;

    FIL f;
    if (f_open(&f, DESKTOP_DAT_PATH, FA_READ) != FR_OK) return;

    UINT br;
    uint32_t magic;
    if (f_read(&f, &magic, 4, &br) != FR_OK || br != 4 || magic != DT_MAGIC) {
        f_close(&f);
        return;
    }

    uint32_t cnt;
    if (f_read(&f, &cnt, 4, &br) != FR_OK || br != 4) {
        f_close(&f);
        return;
    }
    if (cnt > DESKTOP_MAX_SHORTCUTS) cnt = DESKTOP_MAX_SHORTCUTS;

    for (uint32_t i = 0; i < cnt; i++) {
        char path[DESKTOP_PATH_MAX];
        if (f_read(&f, path, DESKTOP_PATH_MAX, &br) != FR_OK
            || br != DESKTOP_PATH_MAX)
            break;
        path[DESKTOP_PATH_MAX - 1] = '\0';

        /* Validate that file still exists (skip built-in paths) */
        if (path[0] != ':') {
            FILINFO fi;
            if (f_stat(path, &fi) != FR_OK) continue;
        }

        desktop_shortcut_t *sc = &dt_shortcuts[dt_count];
        memset(sc, 0, sizeof(*sc));
        strncpy(sc->path, path, DESKTOP_PATH_MAX - 1);
        sc->used = true;

        /* Determine type and load icon/name */
        sc->is_app = is_app_path(path);
        if (sc->is_app) {
            if (path[0] == ':') {
                if (strcmp(path, DESKTOP_BUILTIN_NAVIGATOR) == 0) {
                    strncpy(sc->name, "Navigator", DESKTOP_NAME_MAX - 1);
                    extern const uint8_t fn_icon16_open_folder[];
                    memcpy(sc->icon, fn_icon16_open_folder, DESKTOP_ICON_SIZE);
                    sc->has_icon = true;
                } else if (strcmp(path, DESKTOP_BUILTIN_TERMINAL) == 0) {
                    strncpy(sc->name, "Terminal", DESKTOP_NAME_MAX - 1);
                    extern const uint8_t default_icon_16x16[];
                    memcpy(sc->icon, default_icon_16x16, DESKTOP_ICON_SIZE);
                    sc->has_icon = true;
                }
            } else {
                load_app_name(sc);
                load_app_icon(sc);
            }
        } else {
            strncpy(sc->name, path_basename(path), DESKTOP_NAME_MAX - 1);
            load_file_icon(sc);
        }

        dt_count++;
    }
    f_close(&f);
}

/*==========================================================================
 * Public API
 *=========================================================================*/

void desktop_init(void) {
    memset(dt_shortcuts, 0, sizeof(dt_shortcuts));
    dt_count = 0;
    dt_selected = -1;
    dt_load();
    dt_dirty = true;
}

void desktop_paint(void) {
    extern const uint8_t default_icon_16x16[256];

    for (int i = 0; i < dt_count; i++) {
        desktop_shortcut_t *sc = &dt_shortcuts[i];
        if (!sc->used) continue;

        int16_t cx, cy;
        dt_get_cell_rect(i, &cx, &cy);

        /* Draw 16x16 icon centered horizontally at top of cell */
        int icon_x = cx + (DT_CELL_W - 16) / 2;
        int icon_y = cy + 4;
        const uint8_t *icon_data = sc->has_icon ? sc->icon : default_icon_16x16;
        gfx_draw_icon_16(icon_x, icon_y, icon_data);

        /* Draw name centered below icon (truncated) */
        int text_y = icon_y + 16 + 2;
        const char *name = sc->name;
        int name_len = (int)strlen(name);
        int max_chars = DT_CELL_W / FONT_UI_WIDTH;
        char display_name[20];
        if (name_len > max_chars) {
            strncpy(display_name, name, max_chars - 1);
            display_name[max_chars - 1] = '\0';
        } else {
            strncpy(display_name, name, sizeof(display_name) - 1);
            display_name[sizeof(display_name) - 1] = '\0';
        }
        int tw = (int)strlen(display_name) * FONT_UI_WIDTH;
        int text_x = cx + (DT_CELL_W - tw) / 2;
        if (text_x < cx) text_x = cx;

        /* Highlight: blue background behind name text only */
        bool selected = (i == dt_selected);
        if (selected) {
            gfx_fill_rect(text_x - 1, text_y - 1,
                          tw + 2, FONT_UI_HEIGHT + 2, COLOR_BLUE);
        }
        gfx_text_ui(text_x, text_y, display_name,
                     COLOR_WHITE,
                     selected ? COLOR_BLUE : THEME_DESKTOP_COLOR);
    }

    dt_dirty = false;
}

bool desktop_add_shortcut(const char *path) {
    if (!path || !*path) return false;

    /* Check for duplicate */
    for (int i = 0; i < dt_count; i++) {
        if (dt_shortcuts[i].used && strcmp(dt_shortcuts[i].path, path) == 0)
            return false; /* already on desktop */
    }

    if (dt_count >= DESKTOP_MAX_SHORTCUTS) return false;

    desktop_shortcut_t *sc = &dt_shortcuts[dt_count];
    memset(sc, 0, sizeof(*sc));
    strncpy(sc->path, path, DESKTOP_PATH_MAX - 1);
    sc->used = true;

    sc->is_app = is_app_path(path);
    if (sc->is_app) {
        if (path[0] == ':') {
            /* Built-in app — set name and icon manually */
            if (strcmp(path, DESKTOP_BUILTIN_NAVIGATOR) == 0) {
                strncpy(sc->name, "Navigator", DESKTOP_NAME_MAX - 1);
                extern const uint8_t fn_icon16_open_folder[];
                memcpy(sc->icon, fn_icon16_open_folder, DESKTOP_ICON_SIZE);
                sc->has_icon = true;
            } else if (strcmp(path, DESKTOP_BUILTIN_TERMINAL) == 0) {
                strncpy(sc->name, "Terminal", DESKTOP_NAME_MAX - 1);
                extern const uint8_t default_icon_16x16[];
                memcpy(sc->icon, default_icon_16x16, DESKTOP_ICON_SIZE);
                sc->has_icon = true;
            }
        } else {
            load_app_name(sc);
            load_app_icon(sc);
        }
    } else {
        strncpy(sc->name, path_basename(path), DESKTOP_NAME_MAX - 1);
        load_file_icon(sc);
    }
    if (!sc->name[0])
        strncpy(sc->name, path_basename(path), DESKTOP_NAME_MAX - 1);

    dt_count++;
    dt_save();
    dt_dirty = true;
    wm_force_full_repaint();
    return true;
}

void desktop_remove_shortcut(int idx) {
    if (idx < 0 || idx >= dt_count) return;

    /* Shift remaining entries */
    for (int i = idx; i < dt_count - 1; i++)
        dt_shortcuts[i] = dt_shortcuts[i + 1];
    memset(&dt_shortcuts[dt_count - 1], 0, sizeof(desktop_shortcut_t));
    dt_count--;
    dt_selected = -1;
    dt_save();
    dt_dirty = true;
    wm_force_full_repaint();
}

static int name_cmp(const void *a, const void *b) {
    const desktop_shortcut_t *sa = (const desktop_shortcut_t *)a;
    const desktop_shortcut_t *sb = (const desktop_shortcut_t *)b;
    /* Sort apps before files, then by name */
    if (sa->is_app != sb->is_app)
        return sb->is_app - sa->is_app;
    for (int i = 0; i < DESKTOP_NAME_MAX; i++) {
        char ca = (char)tolower((unsigned char)sa->name[i]);
        char cb = (char)tolower((unsigned char)sb->name[i]);
        if (ca != cb) return ca - cb;
        if (!ca) break;
    }
    return 0;
}

void desktop_sort(void) {
    if (dt_count <= 1) return;
    /* Simple insertion sort — dt_count is small */
    for (int i = 1; i < dt_count; i++) {
        desktop_shortcut_t tmp = dt_shortcuts[i];
        int j = i - 1;
        while (j >= 0 && name_cmp(&tmp, &dt_shortcuts[j]) < 0) {
            dt_shortcuts[j + 1] = dt_shortcuts[j];
            j--;
        }
        dt_shortcuts[j + 1] = tmp;
    }
    dt_save();
    dt_dirty = true;
    wm_force_full_repaint();
}

/*==========================================================================
 * Mouse handling
 *=========================================================================*/

bool desktop_mouse(uint8_t type, int16_t x, int16_t y) {
    /* Only handle events on the desktop area (above the taskbar) */
    if (y >= taskbar_work_area_height()) return false;

    /* Don't handle if the click is on a window */
    if (wm_window_at_point(x, y) != HWND_NULL) return false;

    int idx = dt_hit_test(x, y);

    if (type == WM_LBUTTONDOWN) {
        if (idx >= 0) {
            dt_selected = idx;
            dt_dirty = true;
            wm_force_full_repaint();
        } else {
            if (dt_selected >= 0) {
                dt_selected = -1;
                dt_dirty = true;
                wm_force_full_repaint();
            }
        }
        return idx >= 0;
    }

    if (type == WM_LBUTTONUP) {
        /* Double-click detection — re-use last_click logic */
        static int last_idx = -1;
        static uint32_t last_tick = 0;
        uint32_t now = xTaskGetTickCount();
        bool dbl = (idx >= 0 && idx == last_idx &&
                    (now - last_tick) < pdMS_TO_TICKS(400));
        last_idx = idx;
        last_tick = now;

        if (dbl && idx >= 0) {
            /* Open the shortcut */
            desktop_shortcut_t *sc = &dt_shortcuts[idx];
            if (sc->is_app) {
                /* Launch: built-in or ELF */
                if (strcmp(sc->path, DESKTOP_BUILTIN_NAVIGATOR) == 0) {
                    spawn_filemanager_window();
                } else if (strcmp(sc->path, DESKTOP_BUILTIN_TERMINAL) == 0) {
                    extern void spawn_terminal_window(void);
                    spawn_terminal_window();
                } else {
                    if (sc->has_icon)
                        wm_set_pending_icon(sc->icon);
                    launch_elf_app(sc->path);
                }
            } else {
                file_assoc_open(sc->path);
            }
            return true;
        }
        return idx >= 0;
    }

    if (type == WM_RBUTTONDOWN) {
        return true; /* consume to prevent other handling */
    }

    if (type == WM_RBUTTONUP) {
        /* Show context menu */
        dt_ctx_index = idx;
        menu_item_t items[4];
        uint8_t count = 0;

        if (idx >= 0) {
            strncpy(items[count].text, "Open", sizeof(items[0].text));
            items[count].command_id = DT_CMD_OPEN;
            items[count].flags = 0;
            items[count].accel_key = 0;
            count++;

            items[count] = (menu_item_t){ "", 0, MIF_SEPARATOR, 0 };
            count++;

            strncpy(items[count].text, "Remove", sizeof(items[0].text));
            items[count].command_id = DT_CMD_REMOVE;
            items[count].flags = 0;
            items[count].accel_key = 0;
            count++;
        } else {
            strncpy(items[count].text, "Sort by Name", sizeof(items[0].text));
            items[count].command_id = DT_CMD_SORT_NAME;
            items[count].flags = 0;
            items[count].accel_key = 0;
            count++;

            strncpy(items[count].text, "Refresh", sizeof(items[0].text));
            items[count].command_id = DT_CMD_REFRESH;
            items[count].flags = 0;
            items[count].accel_key = 0;
            count++;
        }

        /* Use HWND_NULL as owner — we'll intercept the result in
         * desktop_handle_command which is called from compositor. */
        menu_popup_show(HWND_NULL, x, y, items, count);
        return true;
    }

    return false;
}

bool desktop_handle_command(uint16_t cmd) {
    switch (cmd) {
    case DT_CMD_REMOVE:
        if (dt_ctx_index >= 0 && dt_ctx_index < dt_count)
            desktop_remove_shortcut(dt_ctx_index);
        dt_ctx_index = -1;
        return true;
    case DT_CMD_SORT_NAME:
        desktop_sort();
        return true;
    case DT_CMD_REFRESH:
        desktop_init();
        wm_force_full_repaint();
        return true;
    case DT_CMD_OPEN:
        if (dt_ctx_index >= 0 && dt_ctx_index < dt_count) {
            desktop_shortcut_t *sc = &dt_shortcuts[dt_ctx_index];
            if (sc->is_app) {
                if (strcmp(sc->path, DESKTOP_BUILTIN_NAVIGATOR) == 0) {
                    spawn_filemanager_window();
                } else if (strcmp(sc->path, DESKTOP_BUILTIN_TERMINAL) == 0) {
                    extern void spawn_terminal_window(void);
                    spawn_terminal_window();
                } else {
                    if (sc->has_icon)
                        wm_set_pending_icon(sc->icon);
                    launch_elf_app(sc->path);
                }
            } else {
                file_assoc_open(sc->path);
            }
        }
        dt_ctx_index = -1;
        return true;
    default:
        return false;
    }
}

bool desktop_is_dirty(void) {
    return dt_dirty;
}

void desktop_mark_dirty(void) {
    dt_dirty = true;
}
