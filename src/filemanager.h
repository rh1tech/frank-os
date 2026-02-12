/*
 * FRANK OS
 * Copyright (c) 2025 Mikhail Matveev <xtreme@rh1.tech>
 * https://rh1.tech
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef FILEMANAGER_H
#define FILEMANAGER_H

#include <stdint.h>
#include "window.h"

/*==========================================================================
 * General constants
 *=========================================================================*/

#define FN_MAX_ENTRIES      128
#define FN_PATH_MAX         256
#define FN_NAME_MAX         64
#define FN_HISTORY_DEPTH    8

/*==========================================================================
 * View modes
 *=========================================================================*/

#define FN_VIEW_LARGE_ICONS 0
#define FN_VIEW_SMALL_ICONS 1
#define FN_VIEW_LIST        2

/*==========================================================================
 * Selection flags
 *=========================================================================*/

#define FN_SEL_SELECTED     0x01
#define FN_SEL_CUT          0x02

/*==========================================================================
 * Layout constants
 *=========================================================================*/

#define FN_TOOLBAR_HEIGHT   26
#define FN_HEADER_HEIGHT    FN_TOOLBAR_HEIGHT
#define FN_STATUSBAR_H      20
#define FN_SCROLLBAR_W      16

/*==========================================================================
 * Menu / command IDs  --  File menu
 *=========================================================================*/

#define FN_CMD_NEW_FOLDER   101
#define FN_CMD_DELETE        102
#define FN_CMD_RENAME        103
#define FN_CMD_CLOSE         105

/*  Edit menu  */
#define FN_CMD_CUT           201
#define FN_CMD_COPY          202
#define FN_CMD_PASTE         203
#define FN_CMD_SELECT_ALL    204

/*  View menu  */
#define FN_CMD_LARGE_ICONS   301
#define FN_CMD_SMALL_ICONS   302
#define FN_CMD_LIST           303
#define FN_CMD_REFRESH        304

/*  Help menu  */
#define FN_CMD_ABOUT         601

/*  Navigation  */
#define FN_CMD_BACK          401
#define FN_CMD_UP            402
#define FN_CMD_OPEN          403

/*==========================================================================
 * Context-menu command IDs
 *=========================================================================*/

#define FN_CMD_CTX_OPEN       501
#define FN_CMD_CTX_CUT        502
#define FN_CMD_CTX_COPY       503
#define FN_CMD_CTX_PASTE      504
#define FN_CMD_CTX_DELETE     505
#define FN_CMD_CTX_RENAME     506
#define FN_CMD_CTX_NEW_FOLDER 507
#define FN_CMD_CTX_REFRESH    508

/*==========================================================================
 * Directory entry
 *=========================================================================*/

typedef struct {
    char     name[FN_NAME_MAX];
    uint32_t size;
    uint8_t  attrib;            /* FatFS AM_DIR, AM_RDO, etc. */
    uint8_t  sel_flags;         /* FN_SEL_* bitmask             */
    uint8_t  is_executable;     /* has companion .inf file      */
    int8_t   icon_idx;          /* index into app icon cache (-1=none) */
    int16_t  custom_order;
} fn_entry_t;

/*==========================================================================
 * File-manager instance state
 *=========================================================================*/

typedef struct {
    /* --- window -------------------------------------------------------- */
    hwnd_t   hwnd;

    /* --- current directory --------------------------------------------- */
    char     path[FN_PATH_MAX];

    /* --- directory listing --------------------------------------------- */
    fn_entry_t entries[FN_MAX_ENTRIES];
    uint16_t   entry_count;

    /* --- view / scroll ------------------------------------------------- */
    uint8_t  view_mode;         /* FN_VIEW_*                    */
    int16_t  scroll_y;
    int16_t  content_height;

    /* --- selection / focus --------------------------------------------- */
    int16_t  focus_index;
    int16_t  anchor_index;
    uint16_t selection_count;

    /* --- rubber-band selection ----------------------------------------- */
    uint8_t  rubber_active;
    int16_t  rubber_x0, rubber_y0;
    int16_t  rubber_x1, rubber_y1;

    /* --- drag-and-drop ------------------------------------------------- */
    uint8_t  drag_active;
    uint8_t  drag_started;
    int16_t  drag_index;
    int16_t  drag_x, drag_y;

    /* --- double-click detection ---------------------------------------- */
    int16_t  last_click_index;
    uint32_t last_click_tick;

    /* --- navigation history -------------------------------------------- */
    char     history[FN_HISTORY_DEPTH][FN_PATH_MAX];
    int8_t   history_pos;
    int8_t   history_count;

    /* --- toolbar state ------------------------------------------------- */
    int8_t   toolbar_hover;
    int8_t   toolbar_pressed;
    int8_t   tooltip_btn;           /* button index for tooltip (-1=none) */
    uint32_t tooltip_hover_tick;    /* tick when hover started            */

    /* --- list view columns --------------------------------------------- */
    int16_t  col_name_w;            /* Name column width (pixels)   */
    int16_t  col_size_w;            /* Size column width (pixels)   */
    int8_t   sort_column;           /* 0=Name, 1=Size, 2=Type      */
    int8_t   sort_ascending;        /* 1=ascending, 0=descending    */
    int8_t   col_resize_active;     /* which column edge is being dragged (-1=none) */
    int16_t  col_resize_start_x;    /* mouse x at start of drag     */
    int16_t  col_resize_start_w;    /* column width at start of drag */

    /* --- misc flags ---------------------------------------------------- */
    uint8_t  has_custom_layout;
    uint8_t  pending_rename;
} filemanager_t;

/*==========================================================================
 * Public API
 *=========================================================================*/

hwnd_t filemanager_create(const char *initial_path);
void   spawn_filemanager_window(void);

/*==========================================================================
 * Icon data (defined in fn_icons.c)
 *  32x32 large icons  --  1024 bytes each (1 byte/pixel, palette index)
 *  16x16 small icons  --   256 bytes each
 *=========================================================================*/

/* 32x32 large icons */
extern const uint8_t fn_icon32_folder[];
extern const uint8_t fn_icon32_open_folder[];
extern const uint8_t fn_icon32_document[];
extern const uint8_t fn_icon32_text_doc[];
extern const uint8_t fn_icon32_application[];
extern const uint8_t fn_icon32_drive[];
extern const uint8_t fn_icon32_unknown[];

/* 16x16 small icons */
extern const uint8_t fn_icon16_folder[];
extern const uint8_t fn_icon16_open_folder[];
extern const uint8_t fn_icon16_document[];
extern const uint8_t fn_icon16_text_doc[];
extern const uint8_t fn_icon16_application[];
extern const uint8_t fn_icon16_drive[];
extern const uint8_t fn_icon16_unknown[];
extern const uint8_t fn_icon16_back[];
extern const uint8_t fn_icon16_up[];
extern const uint8_t fn_icon16_cut[];
extern const uint8_t fn_icon16_copy[];
extern const uint8_t fn_icon16_paste[];
extern const uint8_t fn_icon16_delete[];

#endif /* FILEMANAGER_H */
