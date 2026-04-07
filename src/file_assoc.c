/*
 * FRANK OS — File Association Registry
 * Copyright (c) 2026 Mikhail Matveev <xtreme@rh1.tech>
 * https://rh1.tech
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "file_assoc.h"
#include "ico.h"
#include "window_event.h"
#include "app.h"
#include "ff.h"
#include "sdcard_init.h"
#include <string.h>
#include <stdio.h>
#include <ctype.h>

/*==========================================================================
 * Internal state
 *=========================================================================*/

static fa_app_t fa_apps[FA_MAX_APPS];
static int      fa_app_count = 0;

/*==========================================================================
 * Helper: case-insensitive extension match
 *=========================================================================*/

static int strcasecmp_short(const char *a, const char *b) {
    while (*a && *b) {
        char ca = (char)tolower((unsigned char)*a);
        char cb = (char)tolower((unsigned char)*b);
        if (ca != cb) return ca - cb;
        a++; b++;
    }
    return (unsigned char)*a - (unsigned char)*b;
}

/*==========================================================================
 * Parse the "ext:" line from an .inf file.
 *
 * Format: ext:txt,bas,log\n   (or EOF)
 * Reads from the current file position (after icon data).
 *=========================================================================*/

static void parse_ext_line(FIL *f, fa_app_t *app) {
    app->ext_count = 0;

    char line[80];
    UINT br;
    if (f_read(f, line, sizeof(line) - 1, &br) != FR_OK || br == 0)
        return;
    line[br] = '\0';

    /* Skip leading whitespace / newlines (icon data may end with
     * a byte that happens to be '\n', or the .inf author may have
     * inserted one) */
    char *lp = line;
    while (*lp == '\n' || *lp == '\r' || *lp == ' ') lp++;

    /* Must start with "ext:" */
    if (strncmp(lp, "ext:", 4) != 0)
        return;

    char *p = lp + 4;
    /* Trim trailing newline/CR */
    char *nl = strchr(p, '\n');
    if (nl) *nl = '\0';
    nl = strchr(p, '\r');
    if (nl) *nl = '\0';

    /* Split on comma/semicolon */
    while (*p && app->ext_count < FA_MAX_EXTS) {
        /* Skip separators */
        while (*p == ',' || *p == ';' || *p == ' ') p++;
        if (!*p) break;

        char *start = p;
        while (*p && *p != ',' && *p != ';' && *p != ' ' && *p != '\n')
            p++;

        int len = (int)(p - start);
        if (len > 0 && len < FA_EXT_LEN) {
            memcpy(app->exts[app->ext_count], start, len);
            app->exts[app->ext_count][len] = '\0';
            /* Lowercase for fast matching */
            for (int i = 0; i < len; i++)
                app->exts[app->ext_count][i] =
                    (char)tolower((unsigned char)app->exts[app->ext_count][i]);
            app->ext_count++;
        }
    }
}

/*==========================================================================
 * Load icons from .ico file (preferred over raw .inf icon data)
 *=========================================================================*/

static bool load_ico_icons(const char *base_path, fa_app_t *app) {
    char ico_path[FA_PATH_LEN + 4];
    snprintf(ico_path, sizeof(ico_path), "%s.ico", base_path);

    FIL f;
    if (f_open(&f, ico_path, FA_READ) != FR_OK) return false;

    FSIZE_t fsize = f_size(&f);
    if (fsize < 22 || fsize > 4096) {   /* sanity: min ICO = ~22 bytes */
        f_close(&f);
        return false;
    }

    uint8_t buf[4096];
    UINT br;
    if (f_read(&f, buf, (UINT)fsize, &br) != FR_OK || br != (UINT)fsize) {
        f_close(&f);
        return false;
    }
    f_close(&f);

    if (ico_parse_16(buf, br, app->icon))
        app->has_icon = true;
    if (ico_parse_32(buf, br, app->icon32))
        app->has_icon32 = true;

    return app->has_icon || app->has_icon32;
}

/*==========================================================================
 * Scan /fos/*.inf
 *=========================================================================*/

void file_assoc_scan(void) {
    fa_app_count = 0;
    if (!sdcard_is_mounted()) return;

    DIR dir;
    FILINFO fno;
    if (f_opendir(&dir, "/fos") != FR_OK) return;

    while (fa_app_count < FA_MAX_APPS) {
        if (f_readdir(&dir, &fno) != FR_OK || fno.fname[0] == 0)
            break;
        if (fno.fattrib & AM_DIR) continue;

        /* Only process .inf files */
        const char *dot = strrchr(fno.fname, '.');
        if (!dot || strcmp(dot, ".inf") != 0) continue;

        /* Check that the base executable exists */
        char base_path[FA_PATH_LEN];
        int base_len = (int)(dot - fno.fname);
        if (base_len <= 0 || base_len >= FA_PATH_LEN - 5) continue;
        snprintf(base_path, sizeof(base_path), "/fos/%.*s", base_len, fno.fname);
        {
            FILINFO tmp;
            if (f_stat(base_path, &tmp) != FR_OK) continue;
        }

        fa_app_t *app = &fa_apps[fa_app_count];
        memset(app, 0, sizeof(*app));
        strncpy(app->path, base_path, FA_PATH_LEN - 1);
        app->has_icon = false;

        /* Open .inf */
        char inf_path[FA_PATH_LEN + 4];
        snprintf(inf_path, sizeof(inf_path), "/fos/%s", fno.fname);
        FIL f;
        if (f_open(&f, inf_path, FA_READ) != FR_OK) continue;

        /* Line 1: display name */
        {
            UINT br;
            char buf[FA_NAME_LEN];
            if (f_read(&f, buf, FA_NAME_LEN - 1, &br) == FR_OK && br > 0) {
                buf[br] = '\0';
                char *nl = strchr(buf, '\n');
                if (nl) *nl = '\0';
                nl = strchr(buf, '\r');
                if (nl) *nl = '\0';
                strncpy(app->name, buf, FA_NAME_LEN - 1);
            }
        }
        if (!app->name[0]) {
            f_close(&f);
            continue;
        }

        /* Extension list (right after display name — .inf is text-only) */
        f_lseek(&f, 0);
        {
            char ch;
            UINT br;
            while (f_read(&f, &ch, 1, &br) == FR_OK && br == 1)
                if (ch == '\n') break;
        }
        parse_ext_line(&f, app);
        f_close(&f);

        /* Load icons from .ico file; fall back to terminal icon */
        if (!load_ico_icons(base_path, app)) {
            extern const uint8_t *fn_icon16_terminal_get(void);
            extern const uint8_t *fn_icon32_terminal_get(void);
            memcpy(app->icon,   fn_icon16_terminal_get(), FA_ICON_SIZE);
            memcpy(app->icon32, fn_icon32_terminal_get(), FA_ICON32_SIZE);
            app->has_icon   = true;
            app->has_icon32 = true;
        }

        /* Ensure 32x32 is always available: upscale from 16x16 if needed */
        if (app->has_icon && !app->has_icon32) {
            for (int r = 31; r >= 0; r--)
                for (int c = 31; c >= 0; c--)
                    app->icon32[r * 32 + c] = app->icon[(r/2) * 16 + (c/2)];
            app->has_icon32 = true;
        }

        fa_app_count++;
    }
    f_closedir(&dir);
}

/* Extensions recognized as plain text — opened with notepad as fallback
 * when no explicit association is registered. */
static bool is_text_ext(const char *ext) {
    static const char *text_exts[] = {
        "xml", "json", "yaml", "yml", "toml", "html", "htm", "css",
        "js", "ts", "java", "rs", "go", "rb", "pl", "lua", "sql",
        "bat", "env", "conf", "cmake", "mk", "asm", "s", "inc",
        "gitignore", "properties", "rc", "def", "map", "ld", "gdb",
        NULL
    };
    for (const char **p = text_exts; *p; p++) {
        if (strcasecmp_short(ext, *p) == 0) return true;
    }
    return false;
}

/*==========================================================================
 * Query API
 *=========================================================================*/

const fa_app_t *file_assoc_find(const char *ext) {
    if (!ext || !*ext) return NULL;
    for (int i = 0; i < fa_app_count; i++) {
        for (int j = 0; j < fa_apps[i].ext_count; j++) {
            if (strcasecmp_short(ext, fa_apps[i].exts[j]) == 0)
                return &fa_apps[i];
        }
    }
    /* Fallback: known text/code/config extensions → notepad */
    if (is_text_ext(ext)) {
        for (int i = 0; i < fa_app_count; i++) {
            if (strcmp(fa_apps[i].path, "/fos/notepad") == 0)
                return &fa_apps[i];
        }
    }
    return NULL;
}

int file_assoc_find_all(const char *ext, const fa_app_t **out, int max) {
    if (!ext || !*ext || !out || max <= 0) return 0;
    int n = 0;
    for (int i = 0; i < fa_app_count && n < max; i++) {
        for (int j = 0; j < fa_apps[i].ext_count; j++) {
            if (strcasecmp_short(ext, fa_apps[i].exts[j]) == 0) {
                out[n++] = &fa_apps[i];
                break;
            }
        }
    }
    return n;
}

const fa_app_t *file_assoc_get_apps(int *count) {
    if (count) *count = fa_app_count;
    return fa_apps;
}

/*==========================================================================
 * Open a file with an app — launch or dispatch
 *=========================================================================*/

/* Extract filename extension (without dot), returns "" if none */
static const char *get_ext(const char *path) {
    const char *dot = strrchr(path, '.');
    const char *slash = strrchr(path, '/');
    if (!dot || (slash && dot < slash)) return "";
    return dot + 1;
}

bool file_assoc_open(const char *file_path) {
    const char *ext = get_ext(file_path);
    const fa_app_t *app = file_assoc_find(ext);
    if (!app) return false;
    return file_assoc_open_with(file_path, app->path);
}

bool file_assoc_open_with(const char *file_path, const char *app_path) {
    if (!file_path || !app_path) return false;

    /* Set the app's icon for the new window */
    for (int i = 0; i < fa_app_count; i++) {
        if (strcmp(fa_apps[i].path, app_path) == 0) {
            if (fa_apps[i].has_icon)
                wm_set_pending_icon(fa_apps[i].icon);
            if (fa_apps[i].has_icon32)
                wm_set_pending_icon32(fa_apps[i].icon32);
            break;
        }
    }

    launch_elf_app_with_file(app_path, file_path);
    return true;
}
