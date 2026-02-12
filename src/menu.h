/*
 * FRANK OS
 * Copyright (c) 2025 Mikhail Matveev <xtreme@rh1.tech>
 * https://rh1.tech
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef MENU_H
#define MENU_H

#include <stdint.h>
#include <stdbool.h>
#include "window.h"

/*==========================================================================
 * Menu bar data structures
 *=========================================================================*/

#define MENU_MAX_ITEMS    8    /* max items per dropdown */
#define MENU_MAX_MENUS    4    /* max top-level menus per window */
#define MENU_MAX_BARS    16    /* one per window slot */

/* Item flags */
#define MIF_SEPARATOR    (1u << 0)
#define MIF_DISABLED     (1u << 1)

typedef struct {
    char     text[20];       /* display text */
    uint16_t command_id;     /* WM_COMMAND id sent to window */
    uint8_t  flags;          /* MIF_* flags */
    uint8_t  accel_key;      /* HID code for Alt+key shortcut (0=none) */
} menu_item_t;

typedef struct {
    char        title[12];   /* top-level menu title (e.g. "File") */
    uint8_t     accel_key;   /* HID code for Alt+letter (0=none) */
    uint8_t     item_count;
    menu_item_t items[MENU_MAX_ITEMS];
} menu_def_t;

typedef struct {
    uint8_t    menu_count;
    menu_def_t menus[MENU_MAX_MENUS];
} menu_bar_t;

/*==========================================================================
 * Menu bar API
 *=========================================================================*/

/* Attach a menu bar to a window. Copies the data. */
void menu_set(hwnd_t hwnd, const menu_bar_t *bar);

/* Retrieve menu bar for a window (NULL if none) */
menu_bar_t *menu_get(hwnd_t hwnd);

/* Draw the menu bar strip for a window (called from draw_window_decorations
 * when WF_MENUBAR is set). x,y,w = menu bar area in screen coords. */
void menu_draw_bar(hwnd_t hwnd, int x, int y, int w);

/* Draw the currently open dropdown as a compositor overlay.
 * Call from wm_composite() after windows, before cursor. */
void menu_draw_dropdown(void);

/* Handle mouse click on the menu bar area.
 * lx = local x within the menu bar (0 = left edge of bar). */
void menu_bar_click(hwnd_t hwnd, int lx);

/* Handle mouse event while dropdown is open.
 * Returns true if the event was consumed. */
bool menu_dropdown_mouse(uint8_t type, int16_t x, int16_t y);

/* Handle keyboard input while menu is open.
 * Returns true if the key was consumed. */
bool menu_handle_key(uint8_t hid_code, uint8_t modifiers);

/* Check if any menu dropdown is currently open */
bool menu_is_open(void);

/* Close any open menu */
void menu_close(void);

/* Check if a window's menu bar has a menu matching an Alt+key press.
 * If found, opens that menu and returns true. */
bool menu_try_alt_key(hwnd_t hwnd, uint8_t hid_code);

/*==========================================================================
 * Popup context menu API
 *=========================================================================*/

/* Show a popup context menu at screen position (sx, sy).
 * owner receives WM_COMMAND on item selection.
 * items/count specify the menu items. */
void menu_popup_show(hwnd_t owner, int16_t sx, int16_t sy,
                     const menu_item_t *items, uint8_t count);

/* Check if a popup menu is currently open */
bool menu_popup_is_open(void);

/* Handle mouse event while popup is open.
 * Returns true if the event was consumed. */
bool menu_popup_mouse(uint8_t type, int16_t x, int16_t y);

/* Handle keyboard input while popup is open.
 * Returns true if the key was consumed. */
bool menu_popup_handle_key(uint8_t hid_code, uint8_t modifiers);

/* Draw the popup menu overlay (call from wm_composite after menu_draw_dropdown) */
void menu_popup_draw(void);

/* Close any open popup menu */
void menu_popup_close(void);

#endif /* MENU_H */
