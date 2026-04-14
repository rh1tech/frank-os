/*
 * FRANK OS — Desktop shortcut manager
 * Copyright (c) 2026 Mikhail Matveev <xtreme@rh1.tech>
 * https://rh1.tech
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "desktop.h"
#include "lang.h"
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
#include "ico.h"
#include "ff.h"
#include "sdcard_init.h"
#include "settings.h"
#include "FreeRTOS.h"
#include "task.h"
#include <string.h>
#include <stdio.h>
#include <ctype.h>

static uint8_t dt_bg_color = COLOR_CYAN; /* updated from settings */

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
static bool dt_kb_focus = false;  /* true when desktop has keyboard focus */

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

/* Nearest-neighbour upscale a 16x16 icon to 32x32 */
static void nn_upscale_16_to_32(const uint8_t *src, uint8_t *dst) {
    for (int y = 0; y < 32; y++) {
        int sy = y * 16 / 32;
        for (int x = 0; x < 32; x++) {
            int sx = x * 16 / 32;
            dst[y * 32 + x] = src[sy * 16 + sx];
        }
    }
}

/* Nearest-neighbour downscale a 32x32 icon to 16x16 */
static void nn_downscale_32_to_16(const uint8_t *src, uint8_t *dst) {
    for (int y = 0; y < 16; y++) {
        int sy = y * 32 / 16;
        for (int x = 0; x < 16; x++) {
            int sx = x * 32 / 16;
            dst[y * 16 + x] = src[sy * 32 + sx];
        }
    }
}

/* Check if path is an app (has .inf or .xa1 companion) */
static bool is_app_path(const char *path) {
    if (path[0] == ':') return true;   /* built-in apps */
    char chk[DESKTOP_PATH_MAX + 4];
    FILINFO fi;
    snprintf(chk, sizeof(chk), "%s.inf", path);
    if (f_stat(chk, &fi) == FR_OK) return true;
    snprintf(chk, sizeof(chk), "%s.xa1", path);
    return f_stat(chk, &fi) == FR_OK;
}

/* Check if path is a cc-compiled executable (.xa1 companion, no .inf) */
static bool is_cc_executable(const char *path) {
    char chk[DESKTOP_PATH_MAX + 4];
    FILINFO fi;
    snprintf(chk, sizeof(chk), "%s.inf", path);
    if (f_stat(chk, &fi) == FR_OK) return false;  /* ELF app */
    snprintf(chk, sizeof(chk), "%s.xa1", path);
    return f_stat(chk, &fi) == FR_OK;
}

/* Try to load 32x32 icon from .ico file.
 * If explicit_path is non-NULL, use it; otherwise derive from sc->path. */
static bool load_ico_from(const char *ico_path, desktop_shortcut_t *sc) {

    FIL f;
    if (f_open(&f, ico_path, FA_READ) != FR_OK) return false;

    FSIZE_t fsize = f_size(&f);
    if (fsize < 22 || fsize > 2048) {
        f_close(&f);
        return false;
    }

    uint8_t buf[2048];
    UINT br;
    if (f_read(&f, buf, (UINT)fsize, &br) != FR_OK || br != (UINT)fsize) {
        f_close(&f);
        return false;
    }
    f_close(&f);

    if (ico_parse_32(buf, br, sc->icon)) {
        sc->has_icon = true;
        return true;
    }
    return false;
}

static bool load_ico_icon(desktop_shortcut_t *sc) {
    char ico_path[DESKTOP_PATH_MAX + 4];
    snprintf(ico_path, sizeof(ico_path), "%s.ico", sc->path);
    return load_ico_from(ico_path, sc);
}

/* Load 32x32 icon: try .ico first, then fall back to .inf data */
static void load_app_icon(desktop_shortcut_t *sc) {
    /* Prefer .ico file */
    if (load_ico_icon(sc)) return;

    /* Fall back to .inf file */
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

    /* Skip 16x16 icon (256 bytes) */
    uint8_t tmp16[DESKTOP_ICON16_SIZE];
    if (f_read(&f, tmp16, DESKTOP_ICON16_SIZE, &br) != FR_OK
            || br != DESKTOP_ICON16_SIZE) {
        f_close(&f);
        return;
    }

    /* Read 32x32 icon (1024 bytes) */
    if (f_read(&f, sc->icon, DESKTOP_ICON32_SIZE, &br) == FR_OK
            && br == DESKTOP_ICON32_SIZE) {
        sc->has_icon = true;
    } else {
        /* v1 inf: no 32x32 data — upscale the 16x16 we already read */
        nn_upscale_16_to_32(tmp16, sc->icon);
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

/* Load 32x32 icon for a file shortcut via its associated app */
static void load_file_icon(desktop_shortcut_t *sc) {
    const char *ext = path_ext(sc->path);
    const fa_app_t *app = file_assoc_find(ext);
    if (!app) return;
    if (app->has_icon32) {
        memcpy(sc->icon, app->icon32, DESKTOP_ICON32_SIZE);
        sc->has_icon = true;
    } else if (app->has_icon) {
        /* Upscale 16x16 to 32x32 */
        nn_upscale_16_to_32(app->icon, sc->icon);
        sc->has_icon = true;
    }
}

/*==========================================================================
 * Grid layout helpers
 *=========================================================================*/

static int dt_cols(void) {
    /* Number of icon columns in the work area */
    return (display_width - DT_MARGIN_X * 2) / DT_CELL_W;
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
                    strncpy(sc->name, L(STR_NAVIGATOR), DESKTOP_NAME_MAX - 1);
                    extern const uint8_t *fn_icon32_navigator_get(void);
                    memcpy(sc->icon, fn_icon32_navigator_get(), DESKTOP_ICON32_SIZE);
                    sc->has_icon = true;
                } else if (strcmp(path, DESKTOP_BUILTIN_TERMINAL) == 0) {
                    strncpy(sc->name, L(STR_TERMINAL), DESKTOP_NAME_MAX - 1);
                    extern const uint8_t *fn_icon32_terminal_get(void);
                    memcpy(sc->icon, fn_icon32_terminal_get(), DESKTOP_ICON32_SIZE);
                    sc->has_icon = true;
                }
            } else if (is_cc_executable(path)) {
                /* CC-compiled .xa1 app — no .inf, use built-in cc_app icon */
                strncpy(sc->name, path_basename(path), DESKTOP_NAME_MAX - 1);
                extern const uint8_t fn_icon32_cc_app[];
                memcpy(sc->icon, fn_icon32_cc_app, DESKTOP_ICON32_SIZE);
                sc->has_icon = true;
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
    dt_bg_color = settings_get()->desktop_color;
    dt_load();
    dt_dirty = true;
}

void desktop_set_bg_color(uint8_t color) {
    if (color > 15) color = COLOR_CYAN;
    dt_bg_color = color;
    dt_dirty = true;
}

uint8_t desktop_get_bg_color(void) {
    return dt_bg_color;
}

/* Cached default 32x32 icon (upscaled from default_icon_16x16 on first use) */
static uint8_t dt_default_icon32[DESKTOP_ICON32_SIZE];
static bool    dt_default_icon32_ready = false;

void desktop_paint(void) {
    extern const uint8_t default_icon_16x16[256];

    if (!dt_default_icon32_ready) {
        nn_upscale_16_to_32(default_icon_16x16, dt_default_icon32);
        dt_default_icon32_ready = true;
    }

    for (int i = 0; i < dt_count; i++) {
        desktop_shortcut_t *sc = &dt_shortcuts[i];
        if (!sc->used) continue;

        int16_t cx, cy;
        dt_get_cell_rect(i, &cx, &cy);

        /* Draw 32x32 icon centered horizontally at top of cell */
        int icon_x = cx + (DT_CELL_W - DT_ICON_SIZE) / 2;
        int icon_y = cy + 2;
        const uint8_t *icon_data = sc->has_icon ? sc->icon : dt_default_icon32;
        gfx_draw_icon_32(icon_x, icon_y, icon_data);

        /* Draw name centered below icon (truncated) */
        int text_y = icon_y + DT_ICON_SIZE + 2;
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
        int tw = gfx_utf8_charcount(display_name) * FONT_UI_WIDTH;
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
                     selected ? COLOR_BLUE : dt_bg_color);
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
                strncpy(sc->name, L(STR_NAVIGATOR), DESKTOP_NAME_MAX - 1);
                extern const uint8_t *fn_icon32_navigator_get(void);
                memcpy(sc->icon, fn_icon32_navigator_get(), DESKTOP_ICON32_SIZE);
                sc->has_icon = true;
            } else if (strcmp(path, DESKTOP_BUILTIN_TERMINAL) == 0) {
                strncpy(sc->name, L(STR_TERMINAL), DESKTOP_NAME_MAX - 1);
                extern const uint8_t *fn_icon32_terminal_get(void);
                memcpy(sc->icon, fn_icon32_terminal_get(), DESKTOP_ICON32_SIZE);
                sc->has_icon = true;
            }
        } else if (is_cc_executable(path)) {
            /* CC-compiled .xa1 app — no .inf, use built-in cc_app icon */
            strncpy(sc->name, path_basename(path), DESKTOP_NAME_MAX - 1);
            extern const uint8_t fn_icon32_cc_app[];
            memcpy(sc->icon, fn_icon32_cc_app, DESKTOP_ICON32_SIZE);
            sc->has_icon = true;
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

/* Show context menu at screen position (sx, sy).
 * If on_shortcut is true, shows Open/Remove; otherwise Sort/Refresh. */
static void dt_show_context_menu(int16_t sx, int16_t sy, bool on_shortcut) {
    menu_item_t items[4];
    uint8_t count = 0;

    if (on_shortcut) {
        strncpy(items[count].text, L(STR_OPEN), sizeof(items[0].text) - 1);
        items[count].command_id = DT_CMD_OPEN;
        items[count].flags = 0;
        items[count].accel_key = 0;
        count++;

        items[count] = (menu_item_t){ "", 0, MIF_SEPARATOR, 0 };
        count++;

        strncpy(items[count].text, L(STR_DT_REMOVE), sizeof(items[0].text) - 1);
        items[count].command_id = DT_CMD_REMOVE;
        items[count].flags = 0;
        items[count].accel_key = 0;
        count++;
    } else {
        strncpy(items[count].text, L(STR_DT_SORT_BY_NAME), sizeof(items[0].text) - 1);
        items[count].command_id = DT_CMD_SORT_NAME;
        items[count].flags = 0;
        items[count].accel_key = 0;
        count++;

        strncpy(items[count].text, L(STR_DT_REFRESH), sizeof(items[0].text) - 1);
        items[count].command_id = DT_CMD_REFRESH;
        items[count].flags = 0;
        items[count].accel_key = 0;
        count++;
    }

    menu_popup_show(HWND_NULL, sx, sy, items, count);
}

/* Launch a desktop shortcut: built-in, ELF app, cc executable, or file */
static void dt_launch_shortcut(desktop_shortcut_t *sc) {
    if (sc->is_app) {
        if (strcmp(sc->path, DESKTOP_BUILTIN_NAVIGATOR) == 0) {
            spawn_filemanager_window();
        } else if (strcmp(sc->path, DESKTOP_BUILTIN_TERMINAL) == 0) {
            extern void spawn_terminal_window(void);
            spawn_terminal_window();
        } else if (is_cc_executable(sc->path)) {
            launch_elf_app_with_file("/fos/pshell", sc->path);
        } else {
            if (sc->has_icon) {
                /* sc->icon is 32x32; downscale for 16x16 title bar icon */
                static uint8_t tmp16[256];
                nn_downscale_32_to_16(sc->icon, tmp16);
                wm_set_pending_icon(tmp16);
                wm_set_pending_icon32(sc->icon);
            }
            launch_elf_app(sc->path);
        }
    } else {
        file_assoc_open(sc->path);
    }
}

/*==========================================================================
 * Mouse handling
 *=========================================================================*/

bool desktop_mouse(uint8_t type, int16_t x, int16_t y) {
    /* Mouse interaction clears keyboard focus on the desktop */
    dt_kb_focus = false;

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
            dt_launch_shortcut(sc);
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
        dt_show_context_menu(x, y, idx >= 0);
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
            dt_launch_shortcut(sc);
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

bool desktop_has_shortcuts(void) {
    return dt_count > 0;
}

const uint8_t *desktop_get_icon(void) {
    extern const uint8_t *fn_icon16_desktop_get(void);
    return fn_icon16_desktop_get();
}

const uint8_t *desktop_get_icon32(void) {
    extern const uint8_t *fn_icon32_desktop_get(void);
    return fn_icon32_desktop_get();
}

/* ---------- Desktop keyboard focus ---------- */

void desktop_focus(void) {
    dt_kb_focus = true;
    /* Select first shortcut */
    if (dt_count > 0 && dt_selected < 0)
        dt_selected = 0;
    dt_dirty = true;
    wm_force_full_repaint();
}

bool desktop_has_focus(void) {
    return dt_kb_focus;
}

/* Arrow key navigation: column-first layout (same as dt_get_cell_rect).
 * idx → (col, row) where col = idx / rows, row = idx % rows. */
bool desktop_key(uint8_t scancode, uint8_t modifiers) {
    (void)modifiers;
    if (!dt_kb_focus || dt_count == 0) return false;

    int rows = dt_rows();
    if (rows <= 0) rows = 1;

    /* HID scancodes: Right=0x4F Left=0x50 Down=0x51 Up=0x52 Enter=0x28 */
    switch (scancode) {
    case 0x52: /* Up */
        if (dt_selected <= 0)
            dt_selected = dt_count - 1;
        else
            dt_selected--;
        dt_dirty = true;
        wm_force_full_repaint();
        return true;

    case 0x51: /* Down */
        if (dt_selected >= dt_count - 1)
            dt_selected = 0;
        else
            dt_selected++;
        dt_dirty = true;
        wm_force_full_repaint();
        return true;

    case 0x4F: /* Right */ {
        int col = dt_selected / rows;
        int row = dt_selected % rows;
        col++;
        int new_idx = col * rows + row;
        if (new_idx >= dt_count) {
            /* Wrap to first column, same row */
            new_idx = row;
            if (new_idx >= dt_count) new_idx = 0;
        }
        dt_selected = new_idx;
        dt_dirty = true;
        wm_force_full_repaint();
        return true;
    }

    case 0x50: /* Left */ {
        int col = dt_selected / rows;
        int row = dt_selected % rows;
        col--;
        if (col < 0) {
            /* Wrap to last column that has this row */
            int max_col = (dt_count - 1) / rows;
            int new_idx = max_col * rows + row;
            if (new_idx >= dt_count) new_idx = (max_col - 1) * rows + row;
            if (new_idx < 0) new_idx = dt_count - 1;
            dt_selected = new_idx;
        } else {
            dt_selected = col * rows + row;
        }
        dt_dirty = true;
        wm_force_full_repaint();
        return true;
    }

    case 0x28: /* Enter */
    case 0x58: /* KP Enter */
        if (dt_selected >= 0 && dt_selected < dt_count) {
            desktop_shortcut_t *sc = &dt_shortcuts[dt_selected];
            dt_launch_shortcut(sc);
            dt_kb_focus = false;
        }
        return true;

    case 0x65: /* Menu/Application key */
    open_desktop_context_menu: {
        /* Open context menu at selected shortcut position */
        int16_t cx, cy;
        if (dt_selected >= 0 && dt_selected < dt_count) {
            dt_ctx_index = dt_selected;
            dt_get_cell_rect(dt_selected, &cx, &cy);
            /* Position menu just below the icon (icon is 32px tall at cy+2) */
            dt_show_context_menu(cx + DT_CELL_W / 2, cy + 36, true);
        } else {
            dt_ctx_index = -1;
            dt_show_context_menu(DT_MARGIN_X + 10, DT_MARGIN_Y + 10, false);
        }
        return true;
    }

    default:
        /* Ctrl+Space: also open context menu */
        if (scancode == 0x2C /* Space */ && (modifiers & KMOD_CTRL))
            goto open_desktop_context_menu;
        break;
    }
    return false;
}
