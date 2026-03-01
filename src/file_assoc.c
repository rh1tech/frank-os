/*
 * FRANK OS — File Association Registry
 * Copyright (c) 2026 Mikhail Matveev <xtreme@rh1.tech>
 * https://rh1.tech
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "file_assoc.h"
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

        /* Seek back to just past the first newline */
        f_lseek(&f, 0);
        {
            char ch;
            UINT br;
            while (f_read(&f, &ch, 1, &br) == FR_OK && br == 1) {
                if (ch == '\n') break;
            }
        }

        /* Read 256-byte icon */
        {
            UINT br;
            if (f_read(&f, app->icon, FA_ICON_SIZE, &br) == FR_OK
                && br == FA_ICON_SIZE) {
                app->has_icon = true;
            }
        }

        /* Line 3 (optional): extension list after icon data.
         * parse_ext_line skips leading whitespace/newlines. */
        if (app->has_icon) {
            parse_ext_line(&f, app);
        }

        f_close(&f);
        fa_app_count++;
    }
    f_closedir(&dir);
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

    /* TODO: detect if app is already running and post WM_DROPFILES
     * to its window instead of launching a second instance. */
    launch_elf_app_with_file(app_path, file_path);
    return true;
}
