/*
 * FRANK OS — Localization / Language Support
 * Copyright (c) 2026 Mikhail Matveev <xtreme@rh1.tech>
 * https://rh1.tech
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef LANG_H
#define LANG_H

#include <stdint.h>

#define LANG_EN  0
#define LANG_RU  1

/* Input toggle modes */
#define INPUT_TOGGLE_ALT_SHIFT   0
#define INPUT_TOGGLE_CTRL_SHIFT  1
#define INPUT_TOGGLE_ALT_SPACE   2

/* String IDs — every user-visible string in the OS */
enum {
    /* Common buttons */
    STR_OK, STR_CANCEL, STR_CLOSE, STR_YES, STR_NO,

    /* Start menu */
    STR_START, STR_PROGRAMS, STR_SETTINGS, STR_FIRMWARE,
    STR_RUN_DOTS, STR_REBOOT,

    /* Programs submenu */
    STR_NAVIGATOR, STR_TERMINAL,

    /* Settings submenu */
    STR_CONTROL_PANEL, STR_NETWORK, STR_LANGUAGE,

    /* System menu */
    STR_RESTORE, STR_MOVE, STR_MAXIMIZE, STR_MINIMIZE,
    STR_ENTER_FULLSCREEN, STR_EXIT_FULLSCREEN,

    /* Taskbar */
    STR_VOLUME, STR_MUTE, STR_DISCONNECT,

    /* Control Panel items */
    STR_DESKTOP, STR_SYSTEM, STR_MOUSE, STR_FREQUENCIES,

    /* Desktop Properties */
    STR_BG_COLOR, STR_PREVIEW, STR_WINDOW_THEME,
    STR_DESKTOP_PROPS,

    /* System Properties */
    STR_SYSTEM_PROPS,

    /* Mouse Properties */
    STR_DBLCLICK_SPEED, STR_SLOW, STR_FAST, STR_TEST_AREA,
    STR_MOUSE_PROPS,

    /* Frequencies */
    STR_CPU_FREQ, STR_PSRAM_FREQ, STR_REBOOT_NOTICE,

    /* Language */
    STR_LANGUAGE_PROPS, STR_SELECT_LANGUAGE, STR_INPUT_TOGGLE,

    /* File dialog */
    STR_LOOK_IN, STR_FILE_NAME, STR_OPEN, STR_SAVE,
    STR_CONFIRM_SAVE_AS,

    /* Find/Replace */
    STR_FIND, STR_REPLACE, STR_FIND_NEXT, STR_REPLACE_ALL,
    STR_FIND_WHAT, STR_REPLACE_WITH, STR_MATCH_CASE,

    /* Run dialog */
    STR_RUN, STR_RUN_DESC,
    STR_ERR_NO_SDCARD, STR_ERR_FILE_NOT_FOUND,

    /* Network */
    STR_NET_CONN_FAILED, STR_WIFI_PASSWORD, STR_ENTER_PASSWORD,
    STR_CONNECTING_TO, STR_CONNECTING, STR_CONNECTED_TO,
    STR_NO_ADAPTER, STR_NOT_CONNECTED, STR_SCANNING,
    STR_NO_NETWORKS, STR_SCAN, STR_CONNECT,
    STR_HDR_NETWORK, STR_HDR_SIGNAL, STR_HDR_TYPE,
    STR_ERR_NO_NET_ADAPTER,

    /* File manager - toolbar */
    STR_FM_BACK, STR_FM_UP, STR_FM_CUT, STR_FM_COPY,
    STR_FM_PASTE, STR_FM_DELETE,

    /* File manager - context menu */
    STR_FM_OPEN_WITH, STR_FM_RENAME, STR_FM_NEW_FOLDER,
    STR_FM_REFRESH,

    /* File manager - menu bar */
    STR_FILE, STR_EDIT, STR_VIEW, STR_HELP,
    STR_FM_NEW_FOLDER_MENU, STR_FM_DELETE_MENU, STR_FM_RENAME_MENU,
    STR_FM_CLOSE_MENU,
    STR_FM_CUT_MENU, STR_FM_COPY_MENU, STR_FM_PASTE_MENU,
    STR_FM_SELALL_MENU,
    STR_FM_LARGE_ICONS, STR_FM_SMALL_ICONS, STR_FM_LIST,
    STR_FM_REFRESH_MENU, STR_FM_ABOUT_MENU,

    /* File manager - columns + types */
    STR_FM_NAME, STR_FM_SIZE, STR_FM_TYPE,
    STR_FM_FOLDER, STR_FM_APPLICATION, STR_FM_FILE_TYPE,
    STR_FM_OBJECTS,

    /* File manager - dialogs */
    STR_CONFIRM_DELETE, STR_FM_DELETE_ITEMS,
    STR_FM_NO_APP_REGISTERED, STR_ABOUT_NAVIGATOR,
    STR_FM_DELETING, STR_FM_COPYING,
    STR_FM_NEW_FOLDER_DLG, STR_FM_RENAME_DLG,
    STR_FM_ENTER_NAME, STR_FM_NEW_NAME,
    STR_FM_ALL_FILES,

    /* Terminal */
    STR_FM_EXIT, STR_ABOUT_TERMINAL,

    /* Desktop context menu */
    STR_SEND_TO_DESKTOP,
    STR_DT_REMOVE, STR_DT_SORT_BY_NAME, STR_DT_REFRESH,

    /* Alt+Tab */
    STR_ALTTAB_DESKTOP,

    /* Reboot/firmware dialogs */
    STR_LAUNCH_FIRMWARE, STR_LAUNCH_FW_MSG,
    STR_REBOOT_CONFIRM,
    STR_FLASHING_FW,

    /* No UF2 */
    STR_NO_UF2,

    /* Run dialog extras */
    STR_BROWSE, STR_OPEN_LABEL, STR_FILES_OF_TYPE,

    /* Dialog buttons */
    STR_DLG_OK, STR_DLG_CANCEL, STR_DLG_YES, STR_DLG_NO,

    /* Shared app menu items (used by external apps via L()) */
    STR_APP_NEW, STR_APP_OPEN, STR_APP_SAVE, STR_APP_SAVE_AS,
    STR_APP_UNDO, STR_APP_CUT, STR_APP_COPY, STR_APP_PASTE,
    STR_APP_SELECT_ALL,

    STR_COUNT
};

/* Get/set current language */
uint8_t lang_get(void);
void    lang_set(uint8_t lang);

/* Get localized string by ID.
 * Russian strings are Win1251-encoded (matches the UI font table). */
const char *L(int str_id);

#endif /* LANG_H */
