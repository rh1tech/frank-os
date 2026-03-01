/*
 * FRANK OS — File Association Registry
 * Copyright (c) 2026 Mikhail Matveev <xtreme@rh1.tech>
 * https://rh1.tech
 *
 * Maps file extensions to applications.  INF files declare supported
 * extensions in a third line after the display name and icon data:
 *
 *   <display name>\n
 *   <256 bytes of 16x16 icon>\n
 *   ext:<comma-separated extensions>\n
 *
 * Example:  ext:txt,bas,log
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef FILE_ASSOC_H
#define FILE_ASSOC_H

#include <stdint.h>
#include <stdbool.h>
#include "window.h"

/*==========================================================================
 * Constants
 *=========================================================================*/

#define FA_MAX_APPS        16   /* max registered apps                  */
#define FA_MAX_EXTS        8    /* max extensions per app               */
#define FA_EXT_LEN         8    /* max chars per extension (no dot)     */
#define FA_NAME_LEN        20   /* display name                         */
#define FA_PATH_LEN        32   /* /fos/<binary>                        */
#define FA_ICON_SIZE       256  /* 16x16 paletted icon                  */

/*==========================================================================
 * Data types
 *=========================================================================*/

typedef struct {
    char     name[FA_NAME_LEN];         /* display name from .inf            */
    char     path[FA_PATH_LEN];         /* e.g. "/fos/notepad"               */
    uint8_t  icon[FA_ICON_SIZE];        /* 16x16 icon data                   */
    bool     has_icon;
    char     exts[FA_MAX_EXTS][FA_EXT_LEN]; /* e.g. {"txt","bas","log"}      */
    uint8_t  ext_count;                 /* number of registered extensions   */
} fa_app_t;

/*==========================================================================
 * API
 *=========================================================================*/

/* Scan /fos/*.inf and build the file association registry.
 * Called once at boot or when the SD card is inserted. */
void file_assoc_scan(void);

/* Find the default (first registered) app for a file extension.
 * ext should NOT include the dot (e.g. "txt", not ".txt").
 * Returns NULL if no app is registered for this extension. */
const fa_app_t *file_assoc_find(const char *ext);

/* Find ALL apps that can handle a given extension.
 * Fills 'out' with up to 'max' pointers.
 * Returns the number of matching apps. */
int file_assoc_find_all(const char *ext, const fa_app_t **out, int max);

/* Open a file with its associated application.
 * If the app is already running, posts WM_DROPFILES to it.
 * Otherwise launches the app with the file as argv[1].
 * Returns true if the file was opened or dispatched. */
bool file_assoc_open(const char *file_path);

/* Open a file with a specific app (by path, e.g. "/fos/notepad").
 * Same launch-or-dispatch logic as file_assoc_open. */
bool file_assoc_open_with(const char *file_path, const char *app_path);

/* Get the full registry (for "Open with" menus, etc.) */
const fa_app_t *file_assoc_get_apps(int *count);

#endif /* FILE_ASSOC_H */
