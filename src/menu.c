/*
 * FRANK OS
 * Copyright (c) 2025 Mikhail Matveev <xtreme@rh1.tech>
 * https://rh1.tech
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "menu.h"
#include "window.h"
#include "window_event.h"
#include "window_theme.h"
#include "gfx.h"
#include "font.h"
#include "display.h"
#include <string.h>

/*==========================================================================
 * Internal state
 *=========================================================================*/

static menu_bar_t menu_bars[MENU_MAX_BARS];
static bool       menu_bars_valid[MENU_MAX_BARS]; /* true if slot has data */

/* Dropdown state */
static hwnd_t  menu_open_hwnd  = HWND_NULL;   /* which window's menu is open */
static int8_t  menu_open_index = -1;           /* which top-level menu is open (-1=none) */
static int8_t  menu_hover_item = -1;           /* hovered item in dropdown (-1=none) */

/* Cached dropdown position */
static int16_t dropdown_x, dropdown_y, dropdown_w, dropdown_h;

#define MENU_ITEM_HEIGHT  20
#define MENU_SEPARATOR_H   8
#define MENU_PAD_LEFT      6
#define MENU_PAD_RIGHT    20
#define MENU_MIN_WIDTH    80
#define MENU_SHADOW         1

/*==========================================================================
 * Menu bar data management
 *=========================================================================*/

void menu_set(hwnd_t hwnd, const menu_bar_t *bar) {
    if (hwnd < 1 || hwnd > MENU_MAX_BARS || !bar) return;
    menu_bars[hwnd - 1] = *bar;
    menu_bars_valid[hwnd - 1] = true;
}

menu_bar_t *menu_get(hwnd_t hwnd) {
    if (hwnd < 1 || hwnd > MENU_MAX_BARS) return NULL;
    if (!menu_bars_valid[hwnd - 1]) return NULL;
    return &menu_bars[hwnd - 1];
}

/*==========================================================================
 * Geometry helpers
 *=========================================================================*/

/* Compute the screen x position and pixel width of each top-level menu title */
static int menu_title_x(hwnd_t hwnd, int index) {
    window_t *win = wm_get_window(hwnd);
    if (!win) return 0;
    int x = win->frame.x + THEME_BORDER_WIDTH + MENU_PAD_LEFT;
    menu_bar_t *bar = menu_get(hwnd);
    if (!bar) return x;
    for (int i = 0; i < index && i < bar->menu_count; i++) {
        x += (int)strlen(bar->menus[i].title) * FONT_UI_WIDTH + MENU_PAD_LEFT * 2;
    }
    return x;
}

static int menu_title_width(const char *title) {
    return (int)strlen(title) * FONT_UI_WIDTH + MENU_PAD_LEFT * 2;
}

static void compute_dropdown_rect(void) {
    menu_bar_t *bar = menu_get(menu_open_hwnd);
    if (!bar || menu_open_index < 0) return;
    menu_def_t *md = &bar->menus[menu_open_index];

    dropdown_x = menu_title_x(menu_open_hwnd, menu_open_index) - 6;

    window_t *win = wm_get_window(menu_open_hwnd);
    if (!win) return;
    dropdown_y = win->frame.y + THEME_BORDER_WIDTH + THEME_TITLE_HEIGHT +
                 THEME_MENU_HEIGHT;

    /* Calculate width from longest item */
    int max_w = MENU_MIN_WIDTH;
    for (int i = 0; i < md->item_count; i++) {
        int tw = (int)strlen(md->items[i].text) * FONT_UI_WIDTH + MENU_PAD_LEFT + MENU_PAD_RIGHT;
        if (tw > max_w) max_w = tw;
    }
    dropdown_w = max_w;

    /* Calculate height */
    dropdown_h = 4; /* 2px border top + 2px border bottom */
    for (int i = 0; i < md->item_count; i++) {
        dropdown_h += (md->items[i].flags & MIF_SEPARATOR) ?
                      MENU_SEPARATOR_H : MENU_ITEM_HEIGHT;
    }
}

/*==========================================================================
 * Menu bar rendering
 *=========================================================================*/

void menu_draw_bar(hwnd_t hwnd, int x, int y, int w) {
    menu_bar_t *bar = menu_get(hwnd);

    /* Draw menu bar background */
    gfx_fill_rect(x, y, w, THEME_MENU_HEIGHT, THEME_BUTTON_FACE);

    if (!bar) return;

    int tx = x + MENU_PAD_LEFT;
    int ty = y + (THEME_MENU_HEIGHT - FONT_UI_HEIGHT) / 2;

    for (int i = 0; i < bar->menu_count; i++) {
        int tw = menu_title_width(bar->menus[i].title);
        bool is_open = (menu_open_hwnd == hwnd && menu_open_index == i);

        if (is_open) {
            /* Sunken appearance for open menu */
            gfx_fill_rect(tx - MENU_PAD_LEFT, y, tw, THEME_MENU_HEIGHT,
                          THEME_BUTTON_FACE);
            gfx_hline(tx - MENU_PAD_LEFT, y, tw, COLOR_DARK_GRAY);
            gfx_vline(tx - MENU_PAD_LEFT, y, THEME_MENU_HEIGHT, COLOR_DARK_GRAY);
            gfx_hline(tx - MENU_PAD_LEFT, y + THEME_MENU_HEIGHT - 1, tw, COLOR_WHITE);
            gfx_vline(tx - MENU_PAD_LEFT + tw - 1, y, THEME_MENU_HEIGHT, COLOR_WHITE);
        }

        gfx_text_ui(tx, ty, bar->menus[i].title, COLOR_BLACK, THEME_BUTTON_FACE);

        /* Underline the accelerator character (Alt+key shortcut) */
        if (bar->menus[i].accel_key >= 0x04 && bar->menus[i].accel_key <= 0x1D) {
            char accel_lower = 'a' + (bar->menus[i].accel_key - 0x04);
            char accel_upper = 'A' + (bar->menus[i].accel_key - 0x04);
            const char *title = bar->menus[i].title;
            for (int j = 0; title[j]; j++) {
                if (title[j] == accel_lower || title[j] == accel_upper) {
                    int ux = tx + j * FONT_UI_WIDTH;
                    int uy = ty + FONT_UI_HEIGHT - 1;
                    gfx_hline(ux, uy, FONT_UI_WIDTH, COLOR_BLACK);
                    break;
                }
            }
        }

        tx += tw;
    }
}

/*==========================================================================
 * Dropdown rendering (compositor overlay)
 *=========================================================================*/

void menu_draw_dropdown(void) {
    if (menu_open_hwnd == HWND_NULL || menu_open_index < 0) return;
    menu_bar_t *bar = menu_get(menu_open_hwnd);
    if (!bar) return;
    menu_def_t *md = &bar->menus[menu_open_index];

    /* Menu background */
    gfx_fill_rect(dropdown_x, dropdown_y, dropdown_w, dropdown_h,
                  THEME_BUTTON_FACE);

    /* Raised border */
    gfx_hline(dropdown_x, dropdown_y, dropdown_w, COLOR_WHITE);
    gfx_vline(dropdown_x, dropdown_y, dropdown_h, COLOR_WHITE);
    gfx_hline(dropdown_x, dropdown_y + dropdown_h - 1, dropdown_w, COLOR_BLACK);
    gfx_vline(dropdown_x + dropdown_w - 1, dropdown_y, dropdown_h, COLOR_BLACK);
    gfx_hline(dropdown_x + 1, dropdown_y + dropdown_h - 2, dropdown_w - 2, COLOR_DARK_GRAY);
    gfx_vline(dropdown_x + dropdown_w - 2, dropdown_y + 1, dropdown_h - 2, COLOR_DARK_GRAY);

    /* Draw items */
    int iy = dropdown_y + 2;
    for (int i = 0; i < md->item_count; i++) {
        menu_item_t *item = &md->items[i];

        if (item->flags & MIF_SEPARATOR) {
            /* Separator line */
            int sep_y = iy + MENU_SEPARATOR_H / 2;
            gfx_hline(dropdown_x + 2, sep_y - 1, dropdown_w - 4, COLOR_DARK_GRAY);
            gfx_hline(dropdown_x + 2, sep_y, dropdown_w - 4, COLOR_WHITE);
            iy += MENU_SEPARATOR_H;
            continue;
        }

        bool hovered = (i == menu_hover_item);
        uint8_t item_bg = hovered ? COLOR_BLUE : THEME_BUTTON_FACE;
        uint8_t item_fg = hovered ? COLOR_WHITE : COLOR_BLACK;

        if (item->flags & MIF_DISABLED) {
            item_fg = COLOR_DARK_GRAY;
            if (hovered) item_bg = THEME_BUTTON_FACE;
        }

        /* Item background */
        gfx_fill_rect(dropdown_x + 2, iy, dropdown_w - 4, MENU_ITEM_HEIGHT, item_bg);

        /* Item text */
        gfx_text_ui(dropdown_x + MENU_PAD_LEFT, iy + (MENU_ITEM_HEIGHT - FONT_UI_HEIGHT) / 2,
                    item->text, item_fg, item_bg);

        iy += MENU_ITEM_HEIGHT;
    }
}

/*==========================================================================
 * Mouse interaction
 *=========================================================================*/

void menu_bar_click(hwnd_t hwnd, int lx) {
    menu_bar_t *bar = menu_get(hwnd);
    if (!bar) return;

    /* Find which title was clicked */
    int tx = MENU_PAD_LEFT;
    for (int i = 0; i < bar->menu_count; i++) {
        int tw = menu_title_width(bar->menus[i].title);
        if (lx >= tx - MENU_PAD_LEFT && lx < tx - MENU_PAD_LEFT + tw) {
            if (menu_open_hwnd == hwnd && menu_open_index == i) {
                /* Toggle off */
                menu_close();
            } else {
                menu_open_hwnd = hwnd;
                menu_open_index = i;
                menu_hover_item = -1;
                compute_dropdown_rect();
            }
            wm_mark_dirty();
            return;
        }
        tx += tw;
    }
}

bool menu_dropdown_mouse(uint8_t type, int16_t x, int16_t y) {
    if (menu_open_hwnd == HWND_NULL || menu_open_index < 0) return false;

    menu_bar_t *bar = menu_get(menu_open_hwnd);
    if (!bar) return false;
    menu_def_t *md = &bar->menus[menu_open_index];

    /* Check if mouse is in dropdown area */
    if (x >= dropdown_x && x < dropdown_x + dropdown_w &&
        y >= dropdown_y && y < dropdown_y + dropdown_h) {

        if (type == WM_MOUSEMOVE || type == WM_LBUTTONDOWN) {
            /* Find which item is hovered */
            int iy = dropdown_y + 2;
            int new_hover = -1;
            for (int i = 0; i < md->item_count; i++) {
                int ih = (md->items[i].flags & MIF_SEPARATOR) ?
                         MENU_SEPARATOR_H : MENU_ITEM_HEIGHT;
                if (y >= iy && y < iy + ih) {
                    if (!(md->items[i].flags & (MIF_SEPARATOR | MIF_DISABLED)))
                        new_hover = i;
                    break;
                }
                iy += ih;
            }
            if (new_hover != menu_hover_item) {
                menu_hover_item = new_hover;
                wm_mark_dirty();
            }
        }

        if (type == WM_LBUTTONDOWN || type == WM_LBUTTONUP) {
            /* Select the hovered item on click */
            if (menu_hover_item >= 0 && type == WM_LBUTTONUP) {
                uint16_t cmd = md->items[menu_hover_item].command_id;
                hwnd_t target = menu_open_hwnd;
                menu_close();
                /* Send WM_COMMAND to the window */
                window_event_t ev;
                memset(&ev, 0, sizeof(ev));
                ev.type = WM_COMMAND;
                ev.command.id = cmd;
                wm_post_event(target, &ev);
                wm_mark_dirty();
            }
        }
        return true;
    }

    /* Check if mouse is in the menu bar area — allow switching menus */
    window_t *win = wm_get_window(menu_open_hwnd);
    if (win) {
        int bar_x = win->frame.x + THEME_BORDER_WIDTH;
        int bar_y = win->frame.y + THEME_BORDER_WIDTH + THEME_TITLE_HEIGHT;
        int bar_w = win->frame.w - 2 * THEME_BORDER_WIDTH;
        if (x >= bar_x && x < bar_x + bar_w &&
            y >= bar_y && y < bar_y + THEME_MENU_HEIGHT) {
            if (type == WM_LBUTTONDOWN) {
                /* Click toggles the menu (open/close) */
                menu_bar_click(menu_open_hwnd, x - bar_x);
            } else if (type == WM_MOUSEMOVE) {
                /* Hover switches to a different menu title (no toggle) */
                int lx_val = x - bar_x;
                int tx = MENU_PAD_LEFT;
                for (int i = 0; i < bar->menu_count; i++) {
                    int tw = menu_title_width(bar->menus[i].title);
                    if (lx_val >= tx - MENU_PAD_LEFT && lx_val < tx - MENU_PAD_LEFT + tw) {
                        if (i != menu_open_index) {
                            menu_open_index = i;
                            menu_hover_item = -1;
                            compute_dropdown_rect();
                            wm_mark_dirty();
                        }
                        break;
                    }
                    tx += tw;
                }
            }
            return true;
        }
    }

    /* Click outside — close menu */
    if (type == WM_LBUTTONDOWN) {
        menu_close();
        return false; /* let the click propagate */
    }

    return false;
}

/*==========================================================================
 * Keyboard navigation
 *=========================================================================*/

bool menu_handle_key(uint8_t hid_code, uint8_t modifiers) {
    if (menu_open_hwnd == HWND_NULL || menu_open_index < 0) return false;

    menu_bar_t *bar = menu_get(menu_open_hwnd);
    if (!bar) return false;
    menu_def_t *md = &bar->menus[menu_open_index];

    (void)modifiers;

    switch (hid_code) {
    case 0x52: /* HID_KEY_UP */
        /* Move hover up, skipping separators and disabled */
        for (int tries = 0; tries < md->item_count; tries++) {
            menu_hover_item--;
            if (menu_hover_item < 0) menu_hover_item = md->item_count - 1;
            if (!(md->items[menu_hover_item].flags & (MIF_SEPARATOR | MIF_DISABLED)))
                break;
        }
        wm_mark_dirty();
        return true;

    case 0x51: /* HID_KEY_DOWN */
        for (int tries = 0; tries < md->item_count; tries++) {
            menu_hover_item++;
            if (menu_hover_item >= md->item_count) menu_hover_item = 0;
            if (!(md->items[menu_hover_item].flags & (MIF_SEPARATOR | MIF_DISABLED)))
                break;
        }
        wm_mark_dirty();
        return true;

    case 0x50: /* HID_KEY_LEFT */
        if (menu_open_index > 0) {
            menu_open_index--;
            menu_hover_item = -1;
            compute_dropdown_rect();
            wm_mark_dirty();
        }
        return true;

    case 0x4F: /* HID_KEY_RIGHT */
        if (menu_open_index < bar->menu_count - 1) {
            menu_open_index++;
            menu_hover_item = -1;
            compute_dropdown_rect();
            wm_mark_dirty();
        }
        return true;

    case 0x28: /* HID_KEY_ENTER */
        if (menu_hover_item >= 0) {
            uint16_t cmd = md->items[menu_hover_item].command_id;
            hwnd_t target = menu_open_hwnd;
            menu_close();
            window_event_t ev;
            memset(&ev, 0, sizeof(ev));
            ev.type = WM_COMMAND;
            ev.command.id = cmd;
            wm_post_event(target, &ev);
            wm_mark_dirty();
        }
        return true;

    case 0x29: /* HID_KEY_ESCAPE */
        menu_close();
        return true;
    }

    return false;
}

/*==========================================================================
 * State queries
 *=========================================================================*/

bool menu_is_open(void) {
    return menu_open_hwnd != HWND_NULL && menu_open_index >= 0;
}

void menu_close(void) {
    menu_open_hwnd = HWND_NULL;
    menu_open_index = -1;
    menu_hover_item = -1;
    wm_mark_dirty();
}

bool menu_try_alt_key(hwnd_t hwnd, uint8_t hid_code) {
    menu_bar_t *bar = menu_get(hwnd);
    if (!bar) return false;

    for (int i = 0; i < bar->menu_count; i++) {
        if (bar->menus[i].accel_key == hid_code) {
            menu_open_hwnd = hwnd;
            menu_open_index = i;
            menu_hover_item = -1;
            compute_dropdown_rect();
            wm_mark_dirty();
            return true;
        }
    }
    return false;
}

/*==========================================================================
 * Popup context menu
 *=========================================================================*/

static hwnd_t      popup_owner   = HWND_NULL;
static menu_item_t popup_items[MENU_MAX_ITEMS];
static uint8_t     popup_count;
static int8_t      popup_hover   = -1;
static int16_t     popup_x, popup_y, popup_w, popup_h;
static bool        popup_visible;

void menu_popup_show(hwnd_t owner, int16_t sx, int16_t sy,
                     const menu_item_t *items, uint8_t count) {
    /* Close any open dropdown first */
    if (menu_is_open()) menu_close();

    popup_owner = owner;
    popup_count = count > MENU_MAX_ITEMS ? MENU_MAX_ITEMS : count;
    for (int i = 0; i < popup_count; i++)
        popup_items[i] = items[i];
    popup_hover = -1;

    /* Calculate popup dimensions */
    int max_w = MENU_MIN_WIDTH;
    for (int i = 0; i < popup_count; i++) {
        int tw = (int)strlen(popup_items[i].text) * FONT_UI_WIDTH +
                 MENU_PAD_LEFT + MENU_PAD_RIGHT;
        if (tw > max_w) max_w = tw;
    }
    popup_w = max_w;

    popup_h = 4; /* border */
    for (int i = 0; i < popup_count; i++) {
        popup_h += (popup_items[i].flags & MIF_SEPARATOR) ?
                   MENU_SEPARATOR_H : MENU_ITEM_HEIGHT;
    }

    /* Position: try to fit on screen */
    popup_x = sx;
    popup_y = sy;
    if (popup_x + popup_w + MENU_SHADOW > DISPLAY_WIDTH)
        popup_x = DISPLAY_WIDTH - popup_w - MENU_SHADOW;
    if (popup_y + popup_h + MENU_SHADOW > FB_HEIGHT)
        popup_y = FB_HEIGHT - popup_h - MENU_SHADOW;
    if (popup_x < 0) popup_x = 0;
    if (popup_y < 0) popup_y = 0;

    popup_visible = true;
    wm_mark_dirty();
}

bool menu_popup_is_open(void) {
    return popup_visible;
}

void menu_popup_close(void) {
    popup_visible = false;
    popup_owner = HWND_NULL;
    popup_hover = -1;
    wm_mark_dirty();
}

void menu_popup_draw(void) {
    if (!popup_visible) return;

    /* Drop shadow (dithered for semi-transparency) */
    gfx_fill_rect_dithered(popup_x + MENU_SHADOW, popup_y + MENU_SHADOW,
                           popup_w, popup_h, COLOR_BLACK);

    /* Background */
    gfx_fill_rect(popup_x, popup_y, popup_w, popup_h, THEME_BUTTON_FACE);

    /* Raised border */
    gfx_hline(popup_x, popup_y, popup_w, COLOR_WHITE);
    gfx_vline(popup_x, popup_y, popup_h, COLOR_WHITE);
    gfx_hline(popup_x, popup_y + popup_h - 1, popup_w, COLOR_BLACK);
    gfx_vline(popup_x + popup_w - 1, popup_y, popup_h, COLOR_BLACK);
    gfx_hline(popup_x + 1, popup_y + popup_h - 2, popup_w - 2, COLOR_DARK_GRAY);
    gfx_vline(popup_x + popup_w - 2, popup_y + 1, popup_h - 2, COLOR_DARK_GRAY);

    /* Draw items */
    int iy = popup_y + 2;
    for (int i = 0; i < popup_count; i++) {
        menu_item_t *item = &popup_items[i];

        if (item->flags & MIF_SEPARATOR) {
            int sep_y = iy + MENU_SEPARATOR_H / 2;
            gfx_hline(popup_x + 2, sep_y - 1, popup_w - 4, COLOR_DARK_GRAY);
            gfx_hline(popup_x + 2, sep_y, popup_w - 4, COLOR_WHITE);
            iy += MENU_SEPARATOR_H;
            continue;
        }

        bool hovered = (i == popup_hover);
        uint8_t item_bg = hovered ? COLOR_BLUE : THEME_BUTTON_FACE;
        uint8_t item_fg = hovered ? COLOR_WHITE : COLOR_BLACK;

        if (item->flags & MIF_DISABLED) {
            item_fg = COLOR_DARK_GRAY;
            if (hovered) item_bg = THEME_BUTTON_FACE;
        }

        gfx_fill_rect(popup_x + 2, iy, popup_w - 4, MENU_ITEM_HEIGHT, item_bg);
        gfx_text_ui(popup_x + MENU_PAD_LEFT,
                    iy + (MENU_ITEM_HEIGHT - FONT_UI_HEIGHT) / 2,
                    item->text, item_fg, item_bg);

        iy += MENU_ITEM_HEIGHT;
    }
}

bool menu_popup_mouse(uint8_t type, int16_t x, int16_t y) {
    if (!popup_visible) return false;

    /* Check if mouse is inside popup */
    if (x >= popup_x && x < popup_x + popup_w &&
        y >= popup_y && y < popup_y + popup_h) {

        if (type == WM_MOUSEMOVE || type == WM_LBUTTONDOWN) {
            int iy = popup_y + 2;
            int new_hover = -1;
            for (int i = 0; i < popup_count; i++) {
                int ih = (popup_items[i].flags & MIF_SEPARATOR) ?
                         MENU_SEPARATOR_H : MENU_ITEM_HEIGHT;
                if (y >= iy && y < iy + ih) {
                    if (!(popup_items[i].flags & (MIF_SEPARATOR | MIF_DISABLED)))
                        new_hover = i;
                    break;
                }
                iy += ih;
            }
            if (new_hover != popup_hover) {
                popup_hover = new_hover;
                wm_mark_dirty();
            }
        }

        if (type == WM_LBUTTONUP && popup_hover >= 0) {
            uint16_t cmd = popup_items[popup_hover].command_id;
            hwnd_t target = popup_owner;
            menu_popup_close();
            window_event_t ev;
            memset(&ev, 0, sizeof(ev));
            ev.type = WM_COMMAND;
            ev.command.id = cmd;
            wm_post_event(target, &ev);
            wm_mark_dirty();
        }
        return true;
    }

    /* Click outside — close popup */
    if (type == WM_LBUTTONDOWN || type == WM_RBUTTONDOWN) {
        menu_popup_close();
        return false;
    }

    return false;
}

bool menu_popup_handle_key(uint8_t hid_code, uint8_t modifiers) {
    if (!popup_visible) return false;
    (void)modifiers;

    switch (hid_code) {
    case 0x52: /* UP */
        for (int tries = 0; tries < popup_count; tries++) {
            popup_hover--;
            if (popup_hover < 0) popup_hover = popup_count - 1;
            if (!(popup_items[popup_hover].flags & (MIF_SEPARATOR | MIF_DISABLED)))
                break;
        }
        wm_mark_dirty();
        return true;

    case 0x51: /* DOWN */
        for (int tries = 0; tries < popup_count; tries++) {
            popup_hover++;
            if (popup_hover >= popup_count) popup_hover = 0;
            if (!(popup_items[popup_hover].flags & (MIF_SEPARATOR | MIF_DISABLED)))
                break;
        }
        wm_mark_dirty();
        return true;

    case 0x28: /* ENTER */
        if (popup_hover >= 0) {
            uint16_t cmd = popup_items[popup_hover].command_id;
            hwnd_t target = popup_owner;
            menu_popup_close();
            window_event_t ev;
            memset(&ev, 0, sizeof(ev));
            ev.type = WM_COMMAND;
            ev.command.id = cmd;
            wm_post_event(target, &ev);
            wm_mark_dirty();
        }
        return true;

    case 0x29: /* ESCAPE */
        menu_popup_close();
        return true;
    }

    return false;
}
