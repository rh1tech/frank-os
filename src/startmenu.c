/*
 * FRANK OS
 * Copyright (c) 2025 Mikhail Matveev <xtreme@rh1.tech>
 * https://rh1.tech
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "startmenu.h"
#include "taskbar.h"
#include "window.h"
#include "window_event.h"
#include "window_theme.h"
#include "terminal.h"
#include "filemanager.h"
#include "app.h"
#include "gfx.h"
#include "font.h"
#include "display.h"
#include "sdcard_init.h"
#include "ff.h"
#include <string.h>
#include <ctype.h>

/*==========================================================================
 * Start menu item definitions
 *=========================================================================*/

#define SM_ID_TERMINAL      1
#define SM_ID_REBOOT        2
#define SM_ID_FIRMWARE      3

#define SM_ITEM_HEIGHT  24
#define SM_SEPARATOR_H   8
#define SM_PAD_LEFT     28   /* room for icon column */
#define SM_MENU_WIDTH  160
#define SM_SHADOW         2

typedef struct {
    const char *text;
    uint8_t     id;
    bool        separator;  /* draw separator line above this item */
    bool        has_submenu;
} sm_item_t;

static const sm_item_t sm_items[] = {
    { "Programs",  0,               false, true  },
    { "Firmware",  SM_ID_FIRMWARE,  true,  false },
    { "Reboot",    SM_ID_REBOOT,    true,  false },
};
#define SM_ITEM_COUNT  (sizeof(sm_items) / sizeof(sm_items[0]))

/*==========================================================================
 * Dynamic /fos/ app scanning
 *=========================================================================*/

#define FOS_MAX_APPS 16
#define FOS_NAME_LEN 20
#define FOS_PATH_LEN 32
#define ICON16_SIZE  256

static struct {
    char    name[FOS_NAME_LEN];  /* display name from .inf */
    char    path[FOS_PATH_LEN];  /* e.g. "/fos/minesweeper" */
    uint8_t icon[ICON16_SIZE];   /* 16x16 icon data from .inf */
    bool    has_icon;             /* true if icon was loaded */
} fos_apps[FOS_MAX_APPS];
static int fos_app_count = 0;

static void fos_scan(void) {
    fos_app_count = 0;
    if (!sdcard_is_mounted()) return;

    DIR dir;
    FILINFO fno;
    if (f_opendir(&dir, "/fos") != FR_OK) return;

    while (fos_app_count < FOS_MAX_APPS) {
        if (f_readdir(&dir, &fno) != FR_OK || fno.fname[0] == 0)
            break;
        /* Skip directories */
        if (fno.fattrib & AM_DIR) continue;
        /* Skip .inf metadata files */
        const char *dot = strrchr(fno.fname, '.');
        if (dot && strcmp(dot, ".inf") == 0) continue;

        /* Build path */
        snprintf(fos_apps[fos_app_count].path,
                 FOS_PATH_LEN, "/fos/%s", fno.fname);

        /* Try to read display name and icon from companion .inf file */
        char inf_path[FOS_PATH_LEN + 4];
        snprintf(inf_path, sizeof(inf_path), "/fos/%s.inf", fno.fname);
        FIL f;
        bool got_name = false;
        fos_apps[fos_app_count].has_icon = false;
        if (f_open(&f, inf_path, FA_READ) == FR_OK) {
            UINT br;
            char buf[FOS_NAME_LEN];
            if (f_read(&f, buf, FOS_NAME_LEN - 1, &br) == FR_OK && br > 0) {
                buf[br] = '\0';
                /* Trim at first newline */
                char *nl = strchr(buf, '\n');
                if (nl) *nl = '\0';
                nl = strchr(buf, '\r');
                if (nl) *nl = '\0';
                if (buf[0]) {
                    strncpy(fos_apps[fos_app_count].name, buf, FOS_NAME_LEN - 1);
                    fos_apps[fos_app_count].name[FOS_NAME_LEN - 1] = '\0';
                    got_name = true;
                }
            }
            /* Try to read 256-byte icon after the name line.
             * The .inf format is: <name>\n<256 raw bytes> */
            {
                /* Find the end of the name line in the file */
                f_lseek(&f, 0);
                char ch;
                FSIZE_t pos = 0;
                while (f_read(&f, &ch, 1, &br) == FR_OK && br == 1) {
                    pos++;
                    if (ch == '\n') break;
                }
                /* Read icon data at current position */
                if (f_read(&f, fos_apps[fos_app_count].icon,
                           ICON16_SIZE, &br) == FR_OK && br == ICON16_SIZE) {
                    fos_apps[fos_app_count].has_icon = true;
                }
            }
            f_close(&f);
        }

        /* Fallback: capitalize filename */
        if (!got_name) {
            strncpy(fos_apps[fos_app_count].name, fno.fname, FOS_NAME_LEN - 1);
            fos_apps[fos_app_count].name[FOS_NAME_LEN - 1] = '\0';
            fos_apps[fos_app_count].name[0] =
                toupper((unsigned char)fos_apps[fos_app_count].name[0]);
        }

        fos_app_count++;
    }
    f_closedir(&dir);
}

/*==========================================================================
 * Internal state
 *=========================================================================*/

static bool  sm_open = false;
static int8_t sm_hover = -1;
static int16_t sm_x, sm_y, sm_w, sm_h;

/* Programs submenu state */
static bool   sub_open = false;
static int8_t sub_hover = -1;
static int16_t sub_x, sub_y, sub_w, sub_h;

/*==========================================================================
 * Geometry
 *=========================================================================*/

static void compute_menu_rect(void) {
    sm_w = SM_MENU_WIDTH;
    sm_h = 4; /* borders */
    for (int i = 0; i < (int)SM_ITEM_COUNT; i++) {
        if (sm_items[i].separator) sm_h += SM_SEPARATOR_H;
        sm_h += SM_ITEM_HEIGHT;
    }
    sm_x = 0;
    sm_y = TASKBAR_Y - sm_h;
}

static void compute_sub_rect(void) {
    sub_w = 148;
    sub_h = 4 + (fos_app_count + 2) * SM_ITEM_HEIGHT; /* apps + Navigator + Terminal */
    sub_x = sm_x + sm_w;
    /* Align with the Programs item */
    sub_y = sm_y + 1;
}

/*==========================================================================
 * Public API
 *=========================================================================*/

void startmenu_toggle(void) {
    if (sm_open) {
        startmenu_close();
    } else {
        fos_scan();  /* re-scan /fos/ each time menu opens */
        sm_open = true;
        sm_hover = -1;
        sub_open = false;
        sub_hover = -1;
        compute_menu_rect();
        wm_mark_dirty();
    }
}

void startmenu_close(void) {
    sm_open = false;
    sub_open = false;
    sm_hover = -1;
    sub_hover = -1;
    wm_mark_dirty();
}

bool startmenu_is_open(void) {
    return sm_open;
}

/*==========================================================================
 * Action handler
 *=========================================================================*/

/* Declared in main.c */
extern void spawn_terminal_window(void);

static void execute_sub_item(int index) {
    startmenu_close();
    extern const uint8_t default_icon_16x16[256];
    if (index < fos_app_count) {
        wm_set_pending_icon(fos_apps[index].has_icon
                            ? fos_apps[index].icon : default_icon_16x16);
        launch_elf_app(fos_apps[index].path);
    } else if (index == fos_app_count) {
        /* FRANK Navigator */
        spawn_filemanager_window();
    } else {
        /* Terminal (last item) */
        wm_set_pending_icon(default_icon_16x16);
        spawn_terminal_window();
    }
}

static void execute_item(uint8_t id) {
    startmenu_close();
    switch (id) {
    case SM_ID_TERMINAL:
        spawn_terminal_window();
        break;
    case SM_ID_FIRMWARE:
        /* Stub — will have submenu in the future */
        break;
    case SM_ID_REBOOT:
        /* Stub — could call watchdog_reboot() */
        break;
    }
}

/*==========================================================================
 * Rendering
 *=========================================================================*/

void startmenu_draw(void) {
    if (!sm_open) return;

    /* Menu background */
    gfx_fill_rect(sm_x, sm_y, sm_w, sm_h, THEME_BUTTON_FACE);

    /* Raised border (single pixel: white top/left, black bottom/right) */
    gfx_hline(sm_x, sm_y, sm_w, COLOR_WHITE);
    gfx_vline(sm_x, sm_y, sm_h, COLOR_WHITE);
    gfx_hline(sm_x, sm_y + sm_h - 1, sm_w, COLOR_DARK_GRAY);
    gfx_vline(sm_x + sm_w - 1, sm_y, sm_h, COLOR_DARK_GRAY);

    /* Blue side bar (Win95 branding strip) */
    gfx_fill_rect(sm_x + 2, sm_y + 2, 22, sm_h - 4, COLOR_BLUE);

    /* Draw items */
    int iy = sm_y + 2;
    for (int i = 0; i < (int)SM_ITEM_COUNT; i++) {
        if (sm_items[i].separator) {
            int sep_y = iy + SM_SEPARATOR_H / 2;
            gfx_hline(sm_x + SM_PAD_LEFT - 2, sep_y - 1,
                      sm_w - SM_PAD_LEFT, COLOR_DARK_GRAY);
            gfx_hline(sm_x + SM_PAD_LEFT - 2, sep_y,
                      sm_w - SM_PAD_LEFT, COLOR_WHITE);
            iy += SM_SEPARATOR_H;
        }

        bool hovered = (i == sm_hover);
        uint8_t bg = hovered ? COLOR_BLUE : THEME_BUTTON_FACE;
        uint8_t fg = hovered ? COLOR_WHITE : COLOR_BLACK;

        gfx_fill_rect(sm_x + SM_PAD_LEFT - 2, iy,
                      sm_w - SM_PAD_LEFT, SM_ITEM_HEIGHT, bg);
        gfx_text_ui(sm_x + SM_PAD_LEFT + 2,
                    iy + (SM_ITEM_HEIGHT - FONT_UI_HEIGHT) / 2,
                    sm_items[i].text, fg, bg);

        /* Submenu arrow */
        if (sm_items[i].has_submenu) {
            gfx_char_ui(sm_x + sm_w - 14,
                        iy + (SM_ITEM_HEIGHT - FONT_UI_HEIGHT) / 2,
                        '\x17', fg, bg);
        }

        iy += SM_ITEM_HEIGHT;
    }

    /* Draw Programs submenu if open */
    if (sub_open) {
        int sub_count = fos_app_count + 2; /* apps + Terminal */
        gfx_fill_rect(sub_x, sub_y, sub_w, sub_h, THEME_BUTTON_FACE);
        gfx_hline(sub_x, sub_y, sub_w, COLOR_WHITE);
        gfx_vline(sub_x, sub_y, sub_h, COLOR_WHITE);
        gfx_hline(sub_x, sub_y + sub_h - 1, sub_w, COLOR_DARK_GRAY);
        gfx_vline(sub_x + sub_w - 1, sub_y, sub_h, COLOR_DARK_GRAY);

        extern const uint8_t default_icon_16x16[256];
        int sy = sub_y + 2;
        for (int i = 0; i < sub_count; i++) {
            bool hovered = (i == sub_hover);
            uint8_t bg = hovered ? COLOR_BLUE : THEME_BUTTON_FACE;
            uint8_t fg = hovered ? COLOR_WHITE : COLOR_BLACK;
            gfx_fill_rect(sub_x + 2, sy, sub_w - 4, SM_ITEM_HEIGHT, bg);

            /* Draw 16x16 icon */
            const uint8_t *icon = default_icon_16x16;
            const char *label;
            if (i < fos_app_count) {
                if (fos_apps[i].has_icon) icon = fos_apps[i].icon;
                label = fos_apps[i].name;
            } else if (i == fos_app_count) {
                icon = fn_icon16_open_folder;
                label = "Navigator";
            } else {
                label = "Terminal";
            }
            gfx_draw_icon_16(sub_x + 4, sy + 4, icon);
            gfx_text_ui(sub_x + 24, sy + (SM_ITEM_HEIGHT - FONT_UI_HEIGHT) / 2,
                        label, fg, bg);
            sy += SM_ITEM_HEIGHT;
        }
    }
}

/*==========================================================================
 * Mouse handling
 *=========================================================================*/

bool startmenu_mouse(uint8_t type, int16_t x, int16_t y) {
    if (!sm_open) return false;

    /* Check submenu first */
    if (sub_open && x >= sub_x && x < sub_x + sub_w &&
        y >= sub_y && y < sub_y + sub_h) {
        int sub_count = fos_app_count + 2;
        if (type == WM_MOUSEMOVE || type == WM_LBUTTONDOWN) {
            int iy = sub_y + 2;
            sub_hover = -1;
            for (int i = 0; i < sub_count; i++) {
                if (y >= iy && y < iy + SM_ITEM_HEIGHT) {
                    sub_hover = i;
                    break;
                }
                iy += SM_ITEM_HEIGHT;
            }
            wm_mark_dirty();
        }
        if (type == WM_LBUTTONUP && sub_hover >= 0) {
            execute_sub_item(sub_hover);
        }
        return true;
    }

    /* Check main menu */
    if (x >= sm_x && x < sm_x + sm_w &&
        y >= sm_y && y < sm_y + sm_h) {
        if (type == WM_MOUSEMOVE || type == WM_LBUTTONDOWN) {
            int iy = sm_y + 2;
            int new_hover = -1;
            for (int i = 0; i < (int)SM_ITEM_COUNT; i++) {
                if (sm_items[i].separator) iy += SM_SEPARATOR_H;
                if (y >= iy && y < iy + SM_ITEM_HEIGHT) {
                    new_hover = i;
                    break;
                }
                iy += SM_ITEM_HEIGHT;
            }
            if (new_hover != sm_hover) {
                sm_hover = new_hover;
                /* Open/close submenu based on hover (only Programs has content) */
                if (sm_hover == 0 && sm_items[sm_hover].has_submenu) {
                    sub_open = true;
                    sub_hover = -1;
                    compute_sub_rect();
                } else {
                    sub_open = false;
                    sub_hover = -1;
                }
                wm_mark_dirty();
            }
        }
        if (type == WM_LBUTTONUP && sm_hover >= 0) {
            if (!sm_items[sm_hover].has_submenu && sm_items[sm_hover].id > 0) {
                execute_item(sm_items[sm_hover].id);
            }
        }
        return true;
    }

    /* Click outside — close */
    if (type == WM_LBUTTONDOWN) {
        startmenu_close();
        return false; /* let click propagate */
    }

    return false;
}

/*==========================================================================
 * Keyboard navigation
 *=========================================================================*/

bool startmenu_handle_key(uint8_t hid_code, uint8_t modifiers) {
    if (!sm_open) return false;
    (void)modifiers;

    if (sub_open) {
        int sub_count = fos_app_count + 2;
        switch (hid_code) {
        case 0x52: /* UP */
            sub_hover--;
            if (sub_hover < 0) sub_hover = sub_count - 1;
            wm_mark_dirty();
            return true;
        case 0x51: /* DOWN */
            sub_hover++;
            if (sub_hover >= sub_count) sub_hover = 0;
            wm_mark_dirty();
            return true;
        case 0x50: /* LEFT — close submenu */
            sub_open = false;
            sub_hover = -1;
            wm_mark_dirty();
            return true;
        case 0x28: /* ENTER */
            if (sub_hover >= 0) execute_sub_item(sub_hover);
            return true;
        case 0x29: /* ESC */
            sub_open = false;
            sub_hover = -1;
            wm_mark_dirty();
            return true;
        }
        return true;
    }

    switch (hid_code) {
    case 0x52: /* UP */
        sm_hover--;
        if (sm_hover < 0) sm_hover = SM_ITEM_COUNT - 1;
        sub_open = false;
        wm_mark_dirty();
        return true;
    case 0x51: /* DOWN */
        sm_hover++;
        if (sm_hover >= (int)SM_ITEM_COUNT) sm_hover = 0;
        sub_open = false;
        wm_mark_dirty();
        return true;
    case 0x4F: /* RIGHT — open submenu if applicable */
        if (sm_hover == 0 && sm_items[sm_hover].has_submenu) {
            sub_open = true;
            sub_hover = 0;
            compute_sub_rect();
            wm_mark_dirty();
        }
        return true;
    case 0x28: /* ENTER */
        if (sm_hover >= 0) {
            if (sm_hover == 0 && sm_items[sm_hover].has_submenu) {
                sub_open = true;
                sub_hover = 0;
                compute_sub_rect();
                wm_mark_dirty();
            } else if (sm_items[sm_hover].id > 0) {
                execute_item(sm_items[sm_hover].id);
            }
        }
        return true;
    case 0x29: /* ESC */
        startmenu_close();
        return true;
    }

    return false;
}
