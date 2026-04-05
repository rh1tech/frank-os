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
#include "dialog.h"
#include "cursor.h"
#include "gfx.h"
#include "font.h"
#include "display.h"
#include "sdcard_init.h"
#include "desktop.h"
#include "snd.h"
#include "file_assoc.h"
#include "ico.h"
#include "ff.h"
#include "run_dialog.h"
#include "control_panel.h"
#include "hardware/watchdog.h"
#include "FreeRTOS.h"
#include <string.h>
#include <stdio.h>
#include <ctype.h>

/*==========================================================================
 * Start menu item definitions
 *=========================================================================*/

#define SM_ID_TERMINAL      1
#define SM_ID_REBOOT        2
#define SM_ID_FIRMWARE      3
#define SM_ID_RUN           4
#define SM_ID_SETTINGS      5

/* Right-click context popup state (drawn inside startmenu, not popup menu) */
static bool    sm_ctx_open    = false;
static int8_t  sm_ctx_hover   = -1;
static int     sm_ctx_app_idx = -1;
static int16_t sm_ctx_x, sm_ctx_y, sm_ctx_w, sm_ctx_h;
#define SM_CTX_ITEM_H  20   /* height of one popup item */

#define SM_ITEM_HEIGHT  24
#define SM_SEPARATOR_H   8
#define SM_PAD_LEFT     28   /* room for icon column */
#define SM_MENU_WIDTH  160
#define SM_SHADOW         1

typedef struct {
    const char *text;
    uint8_t     id;
    bool        separator;  /* draw separator line above this item */
    bool        has_submenu;
} sm_item_t;

static const sm_item_t sm_items[] = {
    { "Programs",  0,               false, true  },
    { "Settings",  SM_ID_SETTINGS,  false, true  },
    { "Firmware",  SM_ID_FIRMWARE,  true,  true  },
    { "Run...",    SM_ID_RUN,       true,  false },
    { "Reboot",    SM_ID_REBOOT,    true,  false },
};
#define SM_ITEM_COUNT  (sizeof(sm_items) / sizeof(sm_items[0]))
#define SM_IDX_PROGRAMS  0
#define SM_IDX_SETTINGS  1
#define SM_IDX_FIRMWARE  2

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
            /* Load 16x16 icon from .ico file */
            {
                char ico_path[FOS_PATH_LEN + 4];
                snprintf(ico_path, sizeof(ico_path), "%s.ico",
                         fos_apps[fos_app_count].path);
                FIL ico;
                if (f_open(&ico, ico_path, FA_READ) == FR_OK) {
                    FSIZE_t fsize = f_size(&ico);
                    if (fsize >= 22 && fsize <= 4096) {
                        uint8_t ibuf[4096];
                        UINT ibr;
                        if (f_read(&ico, ibuf, (UINT)fsize, &ibr) == FR_OK
                            && ibr == (UINT)fsize) {
                            if (ico_parse_16(ibuf, ibr,
                                    fos_apps[fos_app_count].icon))
                                fos_apps[fos_app_count].has_icon = true;
                        }
                    }
                    f_close(&ico);
                }
            }
            /* No .ico → use terminal icon as default */
            if (!fos_apps[fos_app_count].has_icon) {
                extern const uint8_t *fn_icon16_terminal_get(void);
                memcpy(fos_apps[fos_app_count].icon,
                       fn_icon16_terminal_get(), ICON16_SIZE);
                fos_apps[fos_app_count].has_icon = true;
            }
            f_close(&f);
        }

        /* Skip files without a companion .inf */
        if (!got_name) continue;

        fos_app_count++;
    }
    f_closedir(&dir);
}

/*==========================================================================
 * Dynamic /uf2/ firmware scanning
 *=========================================================================*/

#define UF2_MAX_FILES 16
#define UF2_NAME_LEN  28
#define UF2_PATH_LEN  40

static struct {
    char name[UF2_NAME_LEN];  /* display name (filename without ext) */
    char path[UF2_PATH_LEN];  /* e.g. "/uf2/game.uf2" */
} uf2_files[UF2_MAX_FILES];
static int uf2_file_count = 0;

static void uf2_scan(void) {
    uf2_file_count = 0;
    if (!sdcard_is_mounted()) return;

    DIR dir;
    FILINFO fno;
    if (f_opendir(&dir, "/uf2") != FR_OK) return;

    while (uf2_file_count < UF2_MAX_FILES) {
        if (f_readdir(&dir, &fno) != FR_OK || fno.fname[0] == 0)
            break;
        if (fno.fattrib & AM_DIR) continue;

        /* Accept .uf2 and .m2p2 extensions */
        const char *dot = strrchr(fno.fname, '.');
        if (!dot) continue;
        if (strcmp(dot, ".uf2") != 0 && strcmp(dot, ".m2p2") != 0) continue;

        snprintf(uf2_files[uf2_file_count].path,
                 UF2_PATH_LEN, "/uf2/%s", fno.fname);

        /* Display name: filename without extension */
        size_t base_len = dot - fno.fname;
        if (base_len >= UF2_NAME_LEN) base_len = UF2_NAME_LEN - 1;
        memcpy(uf2_files[uf2_file_count].name, fno.fname, base_len);
        uf2_files[uf2_file_count].name[base_len] = '\0';

        uf2_file_count++;
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

/* Firmware submenu state */
static bool   fw_open = false;
static int8_t fw_hover = -1;
static int16_t fw_x, fw_y, fw_w, fw_h;

/* Settings submenu state */
static bool   set_open = false;
static int8_t set_hover = -1;
static int16_t set_x, set_y, set_w, set_h;

/* Save-under: saves framebuffer pixels under popup submenus so they
 * can be restored instantly when the submenu closes or moves — no
 * window repainting, no full-screen clear, no flicker.
 * Only one submenu is open at a time; context popup has its own buf.
 * Buffers are heap-allocated when the menu opens to avoid bloating BSS. */
static uint8_t *sm_su_buf = NULL;
static int      sm_su_cap = 0;        /* allocated capacity in bytes */
static int16_t  sm_su_x, sm_su_y, sm_su_w, sm_su_h;
static bool     sm_su_valid = false;
/* Which submenu the save-under belongs to (0=none, 1=prog, 2=fw, 3=set) */
static uint8_t  sm_su_which = 0;

static uint8_t *sm_ctx_su_buf = NULL;
static int      sm_ctx_su_cap = 0;
static int16_t  sm_ctx_su_x, sm_ctx_su_y, sm_ctx_su_w, sm_ctx_su_h;
static bool     sm_ctx_su_valid = false;

/* Ensure a heap buffer has enough room; (re)alloc if needed. */
static bool su_ensure(uint8_t **buf, int *cap, int need) {
    if (*cap >= need) return true;
    if (*buf) vPortFree(*buf);
    *buf = (uint8_t *)pvPortMalloc(need);
    *cap = *buf ? need : 0;
    return *buf != NULL;
}

static bool fb_save(uint8_t **buf, int *cap,
                    int16_t x, int16_t y, int16_t w, int16_t h) {
    int bx = x >> 1;
    int bw = (w + 1) >> 1;
    int need = bw * h;
    if (!su_ensure(buf, cap, need)) return false;
    uint8_t *fb = display_draw_buffer_ptr;
    uint8_t *dst = *buf;
    for (int r = 0; r < h; r++)
        memcpy(dst + r * bw, fb + (y + r) * FB_STRIDE + bx, bw);
    return true;
}

static void fb_restore(const uint8_t *src,
                       int16_t x, int16_t y, int16_t w, int16_t h) {
    if (!src) return;
    int bx = x >> 1;
    int bw = (w + 1) >> 1;
    uint8_t *fb = display_draw_buffer_ptr;
    for (int r = 0; r < h; r++)
        memcpy(fb + (y + r) * FB_STRIDE + bx, src + r * bw, bw);
}

/* Pending action state — deferred until dialog is dismissed */
enum { PENDING_NONE, PENDING_REBOOT, PENDING_FIRMWARE };
static uint8_t pending_action = PENDING_NONE;
static int     pending_fw_index = -1;
static char    pending_fw_text[256];

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
    /* Align with the Programs item, but move up if it won't fit */
    sub_y = sm_y + 1;
    if (sub_y + sub_h > TASKBAR_Y)
        sub_y = TASKBAR_Y - sub_h;
    if (sub_y < 0)
        sub_y = 0;
}

static void compute_fw_rect(void) {
    fw_w = 172;
    fw_h = 4 + (uf2_file_count > 0 ? uf2_file_count : 1) * SM_ITEM_HEIGHT;
    fw_x = sm_x + sm_w;
    /* Align with the Firmware item row */
    int iy = sm_y + 2;
    for (int i = 0; i < SM_IDX_FIRMWARE; i++) {
        if (sm_items[i].separator) iy += SM_SEPARATOR_H;
        iy += SM_ITEM_HEIGHT;
    }
    if (sm_items[SM_IDX_FIRMWARE].separator) iy += SM_SEPARATOR_H;
    fw_y = iy;
    if (fw_y + fw_h > TASKBAR_Y)
        fw_y = TASKBAR_Y - fw_h;
    if (fw_y < 0)
        fw_y = 0;
}

static void compute_set_rect(void) {
    set_w = 148;
    set_h = 4 + 1 * SM_ITEM_HEIGHT;  /* one item: Control Panel */
    set_x = sm_x + sm_w;
    /* Align with the Settings item row */
    int iy = sm_y + 2;
    for (int i = 0; i < SM_IDX_SETTINGS; i++) {
        if (sm_items[i].separator) iy += SM_SEPARATOR_H;
        iy += SM_ITEM_HEIGHT;
    }
    if (sm_items[SM_IDX_SETTINGS].separator) iy += SM_SEPARATOR_H;
    set_y = iy;
    if (set_y + set_h > TASKBAR_Y)
        set_y = TASKBAR_Y - set_h;
    if (set_y < 0)
        set_y = 0;
}

/*==========================================================================
 * Public API
 *=========================================================================*/

void startmenu_init(void) {
    fos_scan();
    uf2_scan();
}

void startmenu_toggle(void) {
    if (sm_open) {
        startmenu_close();
    } else {
        sm_open = true;
        sm_hover = -1;
        sub_open = false;
        sub_hover = -1;
        fw_open = false;
        fw_hover = -1;
        set_open = false;
        set_hover = -1;
        compute_menu_rect();
        taskbar_invalidate();  /* redraw Start button in sunken state */
    }
}

void startmenu_close(void) {
    if (!sm_open) return;   /* already closed — avoid redundant repaint */
    sm_open = false;
    sub_open = false;
    fw_open = false;
    set_open = false;
    sm_ctx_open = false;
    sm_hover = -1;
    sub_hover = -1;
    fw_hover = -1;
    set_hover = -1;
    sm_ctx_hover = -1;
    sm_ctx_app_idx = -1;
    /* Free save-under buffers — full repaint restores everything */
    sm_su_valid = false;
    sm_su_which = 0;
    sm_ctx_su_valid = false;
    if (sm_su_buf)     { vPortFree(sm_su_buf);     sm_su_buf = NULL;     sm_su_cap = 0; }
    if (sm_ctx_su_buf) { vPortFree(sm_ctx_su_buf); sm_ctx_su_buf = NULL; sm_ctx_su_cap = 0; }
    /* Force full repaint to guarantee stale menu pixels are cleared,
     * even if the popup-close transition detector misses the change. */
    wm_force_full_repaint();
}

bool startmenu_is_open(void) {
    return sm_open;
}

/*==========================================================================
 * Action handler
 *=========================================================================*/

/* Declared in main.c */
extern void spawn_terminal_window(void);
extern void spawn_control_panel(void);

/* Terminal 16x16 icon (fn_icons.c) */
extern const uint8_t *fn_icon16_terminal_get(void);

static void execute_sub_item(int index) {
    startmenu_close();
    cursor_set_type(CURSOR_WAIT);
    wm_composite();  /* flush frame so hourglass is visible during load */
    extern const uint8_t default_icon_16x16[256];
    if (index < fos_app_count) {
        wm_set_pending_icon(fos_apps[index].has_icon
                            ? fos_apps[index].icon : default_icon_16x16);
        /* Look up 32x32 icon from file_assoc registry */
        {
            int fa_cnt;
            const fa_app_t *fa_list = file_assoc_get_apps(&fa_cnt);
            for (int i = 0; i < fa_cnt; i++) {
                if (strcmp(fa_list[i].path, fos_apps[index].path) == 0) {
                    if (fa_list[i].has_icon32)
                        wm_set_pending_icon32(fa_list[i].icon32);
                    break;
                }
            }
        }
        launch_elf_app(fos_apps[index].path);
    } else if (index == fos_app_count) {
        /* FRANK Navigator */
        spawn_filemanager_window();
    } else {
        /* Terminal (last item) */
        wm_set_pending_icon(fn_icon16_terminal_get());
        spawn_terminal_window();
    }
    cursor_set_type(CURSOR_ARROW);
}

static void do_flash_firmware(int index) {
    cursor_set_type(CURSOR_WAIT);
    wm_composite();

    /* Shut down the sound system to prevent noise/clicks during flash */
    snd_deinit();

    /* Show "Flashing firmware..." notification */
    gfx_fill_rect(160, 200, 320, 80, THEME_BUTTON_FACE);
    gfx_rect(160, 200, 320, 80, COLOR_DARK_GRAY);
    gfx_rect(161, 201, 318, 78, COLOR_WHITE);
    gfx_text_ui(190, 230, "Flashing firmware...", COLOR_BLACK, THEME_BUTTON_FACE);
    display_swap_buffers();

    load_firmware(uf2_files[index].path);
    /* If load_firmware returns (error), restore cursor */
    cursor_set_type(CURSOR_ARROW);
}

static void execute_fw_item(int index) {
    if (index < 0 || index >= uf2_file_count) return;
    startmenu_close();

    /* Build confirmation dialog text */
    snprintf(pending_fw_text, sizeof(pending_fw_text),
             "Launch \"%s\"?\n\n"
             "Screen will turn off during\n"
             "flashing. Please wait.\n\n"
             "To return to FRANK OS,\n"
             "hold Space and press Reset.\n"
             "If it fails, re-flash FOS.",
             uf2_files[index].name);

    pending_action = PENDING_FIRMWARE;
    pending_fw_index = index;
    dialog_show(HWND_NULL, "Launch Firmware", pending_fw_text,
                DLG_ICON_WARNING, DLG_BTN_OK | DLG_BTN_CANCEL);
}

static void execute_item(uint8_t id) {
    startmenu_close();
    switch (id) {
    case SM_ID_TERMINAL:
        spawn_terminal_window();
        break;
    case SM_ID_RUN:
        run_dialog_open();
        break;
    case SM_ID_REBOOT:
        pending_action = PENDING_REBOOT;
        dialog_show(HWND_NULL, "Reboot",
                    "Are you sure you want to\nreboot the system?",
                    DLG_ICON_WARNING, DLG_BTN_OK | DLG_BTN_CANCEL);
        break;
    }
}

void startmenu_check_pending(void) {
    if (pending_action == PENDING_NONE) return;

    uint16_t result = dialog_poll_result();
    if (result == 0) return;  /* dialog still open */

    uint8_t action = pending_action;
    int fw_idx = pending_fw_index;
    pending_action = PENDING_NONE;
    pending_fw_index = -1;

    if (result != DLG_RESULT_OK) return;  /* user cancelled */

    switch (action) {
    case PENDING_REBOOT:
        watchdog_reboot(0, 0, 0);
        break;
    case PENDING_FIRMWARE:
        do_flash_firmware(fw_idx);
        break;
    }
}

/*==========================================================================
 * Rendering
 *=========================================================================*/

/*==========================================================================
 * "FRANK" sidebar logo — pre-rendered 11×49 bold bitmap.
 * Stored top-to-bottom as F,R,A,N,K so it reads "FRANK" downward.
 * Each uint16_t is one row, MSB (bit 15) = leftmost pixel.
 *
 *  F: XXXXXXXXXXX    R: XXXXXXXXX__    A: ___XXXXX___
 *     XXXXXXXXXXX       XXX____XXX_       __XXXXXXX__
 *     XXX________       XXX____XXX_       XXX_____XXX
 *     XXX________       XXXXXXXXXX_       XXX_____XXX
 *     XXXXXXXXX__       XXXXXXXXX__       XXXXXXXXXXX
 *     XXXXXXXXX__       XXX_XXX____       XXXXXXXXXXX
 *     XXX________       XXX__XXX___       XXX_____XXX
 *     XXX________       XXX___XXX__       XXX_____XXX
 *     XXX________       XXX____XXX_       XXX_____XXX
 *
 *  N: XXXX____XXX    K: XXX____XXX_
 *     XXXX____XXX       XXX___XXX__
 *     XXXXX___XXX       XXX__XXX___
 *     XXX_XX__XXX       XXX_XXX____
 *     XXX__XX_XXX       XXXXXX_____
 *     XXX__XX_XXX       XXX_XXX____
 *     XXX___XXXXX       XXX__XXX___
 *     XXX____XXXX       XXX___XXX__
 *     XXX_____XXX       XXX____XXX_
 *=========================================================================*/
#define FRANK_LOGO_W  11
#define FRANK_LOGO_H  49

static const uint16_t frank_logo[FRANK_LOGO_H] = {
    /* F — 9 rows */
    0xFFE0, 0xFFE0, 0xE000, 0xE000, 0xFF80,
    0xFF80, 0xE000, 0xE000, 0xE000,
    /* gap */  0x0000,
    /* R — 9 rows */
    0xFF80, 0xE1C0, 0xE1C0, 0xFFC0, 0xFF80,
    0xEE00, 0xE700, 0xE380, 0xE1C0,
    /* gap */  0x0000,
    /* A — 9 rows */
    0x1F00, 0x3F80, 0xE0E0, 0xE0E0, 0xFFE0,
    0xFFE0, 0xE0E0, 0xE0E0, 0xE0E0,
    /* gap */  0x0000,
    /* N — 9 rows */
    0xF0E0, 0xF0E0, 0xF8E0, 0xECE0, 0xE6E0,
    0xE6E0, 0xE3E0, 0xE1E0, 0xE0E0,
    /* gap */  0x0000,
    /* K — 9 rows */
    0xE1C0, 0xE380, 0xE700, 0xEE00, 0xFC00,
    0xEE00, 0xE700, 0xE380, 0xE1C0,
};

/* Blit "FRANK" into the blue sidebar — each letter rotated 90° CCW,
 * reading bottom-to-top (F at bottom, K at top), aligned to bottom.
 * Original letters are 11 wide × 9 tall.  Rotated: 9 wide × 11 tall. */
static void draw_sidebar_logo(int sx, int sy, int bar_w, int bar_h) {
    #define LETTER_SRC_W  11  /* original letter width  */
    #define LETTER_SRC_H   9  /* original letter height */
    #define LETTER_ROT_W   9  /* rotated width  (= src height) */
    #define LETTER_ROT_H  11  /* rotated height (= src width)  */
    #define LETTER_GAP     1
    #define LETTER_COUNT   5
    #define LOGO_TOTAL_H  (LETTER_COUNT * LETTER_ROT_H + (LETTER_COUNT - 1) * LETTER_GAP)

    int logo_x = sx + (bar_w - LETTER_ROT_W) / 2;
    /* Bottom-align with 4px margin */
    int py = sy + bar_h - LOGO_TOTAL_H - 4;

    /* Draw letters in reverse order: K(4), N(3), A(2), R(1), F(0)
     * so "FRANK" reads bottom-to-top in the sidebar */
    for (int li = LETTER_COUNT - 1; li >= 0; li--) {
        int base = li * (LETTER_SRC_H + 1); /* +1 for gap entry */

        /* Rotated 90° CCW: screen (nx, ny) ← source (col=SRC_W-1-ny, row=nx) */
        for (int ny = 0; ny < LETTER_ROT_H; ny++) {
            int src_col = LETTER_SRC_W - 1 - ny;
            for (int nx = 0; nx < LETTER_ROT_W; nx++) {
                uint16_t row_bits = frank_logo[base + nx];
                if (row_bits & (1u << (15 - src_col))) {
                    int px = logo_x + nx;
                    int spy = py + ny;
                    if ((unsigned)px < (unsigned)display_width &&
                        (unsigned)spy < (unsigned)display_height)
                        display_set_pixel_fast(px, spy, COLOR_WHITE);
                }
            }
        }

        py += LETTER_ROT_H + LETTER_GAP;
    }
}

void startmenu_draw(void) {
    if (!sm_open) return;

    /* --- Save-under management for submenus ---
     * Track which submenu is open by identity (not just rect) so that
     * overlapping rects between different submenus are always handled.
     * Restore old → save new → draw.  On steady-state frames (same
     * submenu, same rect) the save/restore is skipped for speed. */
    {
        uint8_t cur_which = sub_open ? 1 : fw_open ? 2 : set_open ? 3 : 0;
        int16_t cx = 0, cy = 0, cw = 0, ch = 0;
        if (cur_which == 1) { cx = sub_x; cy = sub_y; cw = sub_w; ch = sub_h; }
        if (cur_which == 2) { cx = fw_x;  cy = fw_y;  cw = fw_w;  ch = fw_h;  }
        if (cur_which == 3) { cx = set_x; cy = set_y; cw = set_w; ch = set_h; }

        bool changed = (cur_which != sm_su_which) ||
                       (sm_su_valid && (cx != sm_su_x || cy != sm_su_y ||
                                        cw != sm_su_w || ch != sm_su_h));
        if (changed) {
            /* Restore old submenu area */
            if (sm_su_valid)
                fb_restore(sm_su_buf, sm_su_x, sm_su_y, sm_su_w, sm_su_h);
            sm_su_valid = false;
            sm_su_which = cur_which;
            /* Save new submenu area (framebuffer is now clean) */
            if (cw > 0 && ch > 0) {
                if (fb_save(&sm_su_buf, &sm_su_cap, cx, cy, cw, ch)) {
                    sm_su_x = cx; sm_su_y = cy;
                    sm_su_w = cw; sm_su_h = ch;
                    sm_su_valid = true;
                } else {
                    /* Heap full — fall back to full repaint on next
                     * submenu change so stale pixels get cleaned up. */
                    sm_su_which = 0;
                    wm_force_full_repaint();
                }
            }
        }
    }

    /* --- Save-under for context popup --- */
    if (sm_ctx_open && !sm_ctx_su_valid) {
        if (fb_save(&sm_ctx_su_buf, &sm_ctx_su_cap,
                    sm_ctx_x, sm_ctx_y, sm_ctx_w, sm_ctx_h)) {
            sm_ctx_su_x = sm_ctx_x; sm_ctx_su_y = sm_ctx_y;
            sm_ctx_su_w = sm_ctx_w; sm_ctx_su_h = sm_ctx_h;
            sm_ctx_su_valid = true;
        }
    }
    if (!sm_ctx_open && sm_ctx_su_valid) {
        fb_restore(sm_ctx_su_buf,
                   sm_ctx_su_x, sm_ctx_su_y, sm_ctx_su_w, sm_ctx_su_h);
        sm_ctx_su_valid = false;
    }

    /* Menu background */
    gfx_fill_rect(sm_x, sm_y, sm_w, sm_h, THEME_BUTTON_FACE);

    /* Raised border (single pixel: white top/left, black bottom/right) */
    gfx_hline(sm_x, sm_y, sm_w, COLOR_WHITE);
    gfx_vline(sm_x, sm_y, sm_h, COLOR_WHITE);
    gfx_hline(sm_x, sm_y + sm_h - 1, sm_w, COLOR_DARK_GRAY);
    gfx_vline(sm_x + sm_w - 1, sm_y, sm_h, COLOR_DARK_GRAY);

    /* Blue side bar (Win95 branding strip) */
    gfx_fill_rect(sm_x + 2, sm_y + 2, 22, sm_h - 4, COLOR_BLUE);
    draw_sidebar_logo(sm_x + 2, sm_y + 2, 22, sm_h - 4);

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
                icon = fn_icon16_terminal_get();
                label = "Terminal";
            }
            gfx_draw_icon_16(sub_x + 4, sy + 4, icon);
            gfx_text_ui(sub_x + 24, sy + (SM_ITEM_HEIGHT - FONT_UI_HEIGHT) / 2,
                        label, fg, bg);
            sy += SM_ITEM_HEIGHT;
        }
    }

    /* Draw Firmware submenu if open */
    if (fw_open) {
        gfx_fill_rect(fw_x, fw_y, fw_w, fw_h, THEME_BUTTON_FACE);
        gfx_hline(fw_x, fw_y, fw_w, COLOR_WHITE);
        gfx_vline(fw_x, fw_y, fw_h, COLOR_WHITE);
        gfx_hline(fw_x, fw_y + fw_h - 1, fw_w, COLOR_DARK_GRAY);
        gfx_vline(fw_x + fw_w - 1, fw_y, fw_h, COLOR_DARK_GRAY);

        int fy = fw_y + 2;
        if (uf2_file_count == 0) {
            gfx_text_ui(fw_x + 6, fy + (SM_ITEM_HEIGHT - FONT_UI_HEIGHT) / 2,
                        "(no .uf2 files)", COLOR_DARK_GRAY, THEME_BUTTON_FACE);
        } else {
            for (int i = 0; i < uf2_file_count; i++) {
                bool hovered = (i == fw_hover);
                uint8_t bg = hovered ? COLOR_BLUE : THEME_BUTTON_FACE;
                uint8_t fg = hovered ? COLOR_WHITE : COLOR_BLACK;
                gfx_fill_rect(fw_x + 2, fy, fw_w - 4, SM_ITEM_HEIGHT, bg);
                gfx_text_ui(fw_x + 6, fy + (SM_ITEM_HEIGHT - FONT_UI_HEIGHT) / 2,
                            uf2_files[i].name, fg, bg);
                fy += SM_ITEM_HEIGHT;
            }
        }
    }

    /* Draw Settings submenu if open */
    if (set_open) {
        gfx_fill_rect(set_x, set_y, set_w, set_h, THEME_BUTTON_FACE);
        gfx_hline(set_x, set_y, set_w, COLOR_WHITE);
        gfx_vline(set_x, set_y, set_h, COLOR_WHITE);
        gfx_hline(set_x, set_y + set_h - 1, set_w, COLOR_DARK_GRAY);
        gfx_vline(set_x + set_w - 1, set_y, set_h, COLOR_DARK_GRAY);

        bool hovered = (set_hover == 0);
        uint8_t bg = hovered ? COLOR_BLUE : THEME_BUTTON_FACE;
        uint8_t fg = hovered ? COLOR_WHITE : COLOR_BLACK;
        int sy = set_y + 2;
        gfx_fill_rect(set_x + 2, sy, set_w - 4, SM_ITEM_HEIGHT, bg);
        gfx_draw_icon_16(set_x + 4, sy + 4, cp_get_icon16());
        gfx_text_ui(set_x + 24, sy + (SM_ITEM_HEIGHT - FONT_UI_HEIGHT) / 2,
                    "Control Panel", fg, bg);
    }

    /* Draw right-click context popup (if open) */
    if (sm_ctx_open) {
        gfx_fill_rect(sm_ctx_x, sm_ctx_y, sm_ctx_w, sm_ctx_h,
                       THEME_BUTTON_FACE);
        gfx_hline(sm_ctx_x, sm_ctx_y, sm_ctx_w, COLOR_WHITE);
        gfx_vline(sm_ctx_x, sm_ctx_y, sm_ctx_h, COLOR_WHITE);
        gfx_hline(sm_ctx_x, sm_ctx_y + sm_ctx_h - 1,
                  sm_ctx_w, COLOR_DARK_GRAY);
        gfx_vline(sm_ctx_x + sm_ctx_w - 1, sm_ctx_y,
                  sm_ctx_h, COLOR_DARK_GRAY);

        bool hovered = (sm_ctx_hover == 0);
        uint8_t bg = hovered ? COLOR_BLUE : THEME_BUTTON_FACE;
        uint8_t fg = hovered ? COLOR_WHITE : COLOR_BLACK;
        gfx_fill_rect(sm_ctx_x + 2, sm_ctx_y + 2,
                       sm_ctx_w - 4, SM_CTX_ITEM_H, bg);
        gfx_text_ui(sm_ctx_x + 6,
                    sm_ctx_y + 2 + (SM_CTX_ITEM_H - FONT_UI_HEIGHT) / 2,
                    "Send to Desktop", fg, bg);
    }
}

/*==========================================================================
 * Mouse handling
 *=========================================================================*/

bool startmenu_mouse(uint8_t type, int16_t x, int16_t y) {
    if (!sm_open) return false;

    /* Check Settings submenu */
    if (set_open && x >= set_x && x < set_x + set_w &&
        y >= set_y && y < set_y + set_h) {
        if (type == WM_MOUSEMOVE || type == WM_LBUTTONDOWN) {
            int sy = set_y + 2;
            int new_set = -1;
            if (y >= sy && y < sy + SM_ITEM_HEIGHT)
                new_set = 0;
            if (new_set != set_hover) {
                set_hover = new_set;
                wm_mark_dirty();
            }
        }
        if (type == WM_LBUTTONUP && set_hover >= 0) {
            startmenu_close();
            spawn_control_panel();
        }
        return true;
    }

    /* Check firmware submenu first */
    if (fw_open && x >= fw_x && x < fw_x + fw_w &&
        y >= fw_y && y < fw_y + fw_h) {
        if (type == WM_MOUSEMOVE || type == WM_LBUTTONDOWN) {
            int fy = fw_y + 2;
            int new_fw = -1;
            for (int i = 0; i < uf2_file_count; i++) {
                if (y >= fy && y < fy + SM_ITEM_HEIGHT) {
                    new_fw = i;
                    break;
                }
                fy += SM_ITEM_HEIGHT;
            }
            if (new_fw != fw_hover) {
                fw_hover = new_fw;
                wm_mark_dirty();
            }
        }
        if (type == WM_LBUTTONUP && fw_hover >= 0) {
            execute_fw_item(fw_hover);
        }
        return true;
    }

    /* Check right-click context popup (drawn inside startmenu) */
    if (sm_ctx_open) {
        /* The popup should stay open while the cursor is inside EITHER
         * the popup rect OR the originating app row in Programs. */
        bool in_popup = (x >= sm_ctx_x && x < sm_ctx_x + sm_ctx_w &&
                         y >= sm_ctx_y && y < sm_ctx_y + sm_ctx_h);
        bool in_origin_row = false;
        if (sub_open && sm_ctx_app_idx >= 0) {
            int row_y = sub_y + 2 + sm_ctx_app_idx * SM_ITEM_HEIGHT;
            in_origin_row = (x >= sub_x && x < sub_x + sub_w &&
                             y >= row_y && y < row_y + SM_ITEM_HEIGHT);
        }

        if (in_popup) {
            if (type == WM_MOUSEMOVE || type == WM_LBUTTONDOWN) {
                int cy = sm_ctx_y + 2;
                int new_ch = -1;
                if (y >= cy && y < cy + SM_CTX_ITEM_H) new_ch = 0;
                if (new_ch != sm_ctx_hover) {
                    sm_ctx_hover = new_ch;
                    wm_mark_dirty();
                }
            }
            if (type == WM_LBUTTONUP && sm_ctx_hover >= 0) {
                /* "Send to Desktop" action */
                if (sm_ctx_app_idx >= 0 && sm_ctx_app_idx < fos_app_count)
                    desktop_add_shortcut(fos_apps[sm_ctx_app_idx].path);
                else if (sm_ctx_app_idx == fos_app_count)
                    desktop_add_shortcut(DESKTOP_BUILTIN_NAVIGATOR);
                else if (sm_ctx_app_idx == fos_app_count + 1)
                    desktop_add_shortcut(DESKTOP_BUILTIN_TERMINAL);
                sm_ctx_open = false;
                sm_ctx_hover = -1;
                sm_ctx_app_idx = -1;
                wm_mark_dirty();
            }
            return true;
        }
        if (in_origin_row) {
            /* Cursor on the originating app row — keep popup open,
             * but clear its hover since we're not inside it. */
            if (sm_ctx_hover != -1) {
                sm_ctx_hover = -1;
                wm_mark_dirty();
            }
            return true;
        }
        /* Cursor left both popup and originating row — close */
        sm_ctx_open = false;
        sm_ctx_hover = -1;
        sm_ctx_app_idx = -1;
        wm_mark_dirty();
        /* Fall through so the event is handled by menu areas below */
    }

    /* Check Programs submenu */
    if (sub_open && x >= sub_x && x < sub_x + sub_w &&
        y >= sub_y && y < sub_y + sub_h) {
        int sub_count = fos_app_count + 2;
        if (type == WM_MOUSEMOVE || type == WM_LBUTTONDOWN ||
            type == WM_RBUTTONDOWN) {
            int iy = sub_y + 2;
            int new_sub = -1;
            for (int i = 0; i < sub_count; i++) {
                if (y >= iy && y < iy + SM_ITEM_HEIGHT) {
                    new_sub = i;
                    break;
                }
                iy += SM_ITEM_HEIGHT;
            }
            if (new_sub != sub_hover) {
                sub_hover = new_sub;
                wm_mark_dirty();
            }
        }
        if (type == WM_LBUTTONUP && sub_hover >= 0) {
            execute_sub_item(sub_hover);
        }
        /* Right-click on any app in Programs → open inline context popup */
        if (type == WM_RBUTTONUP && sub_hover >= 0 &&
            sub_hover < fos_app_count + 2) {
            sm_ctx_app_idx = sub_hover;
            /* Position context popup at cursor, clamped to screen */
            sm_ctx_w = 18 * FONT_UI_WIDTH + 12; /* "Send to Desktop" width */
            sm_ctx_h = SM_CTX_ITEM_H + 4;       /* 1 item + borders */
            sm_ctx_x = x;
            sm_ctx_y = y;
            if (sm_ctx_x + sm_ctx_w > display_width)
                sm_ctx_x = display_width - sm_ctx_w;
            if (sm_ctx_y + sm_ctx_h > display_height)
                sm_ctx_y = display_height - sm_ctx_h;
            sm_ctx_hover = -1;
            sm_ctx_open = true;
            wm_mark_dirty();
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
                /* Save stale submenu rects before closing — erased
                 * by startmenu_draw() to avoid full-screen flash. */
                sub_open = false;
                sub_hover = -1;
                fw_open = false;
                fw_hover = -1;
                set_open = false;
                set_hover = -1;
                if (sm_hover == SM_IDX_PROGRAMS) {
                    sub_open = true;
                    compute_sub_rect();
                } else if (sm_hover == SM_IDX_FIRMWARE) {
                    fw_open = true;
                    compute_fw_rect();
                } else if (sm_hover == SM_IDX_SETTINGS) {
                    set_open = true;
                    compute_set_rect();
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

    /* Click outside — close.
     * If the click is on the taskbar (Start button), consume it so
     * taskbar_mouse_click doesn't immediately reopen the menu. */
    if (type == WM_LBUTTONDOWN) {
        startmenu_close();
        return (y >= TASKBAR_Y);
    }

    return false;
}

/*==========================================================================
 * Keyboard navigation
 *=========================================================================*/

bool startmenu_handle_key(uint8_t hid_code, uint8_t modifiers) {
    if (!sm_open) return false;
    (void)modifiers;

    /* Settings submenu keyboard handling */
    if (set_open) {
        switch (hid_code) {
        case 0x52: /* UP */
        case 0x51: /* DOWN */
            set_hover = 0;  /* only one item */
            wm_mark_dirty();
            return true;
        case 0x50: /* LEFT — close submenu */
            set_open = false;
            set_hover = -1;
            wm_mark_dirty();
            return true;
        case 0x28: /* ENTER */
            if (set_hover >= 0) {
                startmenu_close();
                spawn_control_panel();
            }
            return true;
        case 0x29: /* ESC */
            set_open = false;
            set_hover = -1;
            wm_mark_dirty();
            return true;
        }
        return true;
    }

    /* Firmware submenu keyboard handling */
    if (fw_open) {
        switch (hid_code) {
        case 0x52: /* UP */
            if (uf2_file_count > 0) {
                fw_hover--;
                if (fw_hover < 0) fw_hover = uf2_file_count - 1;
            }
            wm_mark_dirty();
            return true;
        case 0x51: /* DOWN */
            if (uf2_file_count > 0) {
                fw_hover++;
                if (fw_hover >= uf2_file_count) fw_hover = 0;
            }
            wm_mark_dirty();
            return true;
        case 0x50: /* LEFT — close submenu */
            fw_open = false;
            fw_hover = -1;
            wm_mark_dirty();
            return true;
        case 0x28: /* ENTER */
            if (fw_hover >= 0) execute_fw_item(fw_hover);
            return true;
        case 0x29: /* ESC */
            fw_open = false;
            fw_hover = -1;
            wm_mark_dirty();
            return true;
        }
        return true;
    }

    /* Programs submenu keyboard handling */
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

    /* Main menu keyboard handling */
    switch (hid_code) {
    case 0x52: /* UP */
        sm_hover--;
        if (sm_hover < 0) sm_hover = SM_ITEM_COUNT - 1;
        sub_open = false;
        fw_open = false;
        set_open = false;
        wm_mark_dirty();
        return true;
    case 0x51: /* DOWN */
        sm_hover++;
        if (sm_hover >= (int)SM_ITEM_COUNT) sm_hover = 0;
        sub_open = false;
        fw_open = false;
        set_open = false;
        wm_mark_dirty();
        return true;
    case 0x4F: /* RIGHT — open submenu if applicable */
        if (sm_hover >= 0 && sm_items[sm_hover].has_submenu) {
            sub_open = false; sub_hover = -1;
            fw_open = false;  fw_hover = -1;
            set_open = false; set_hover = -1;
            if (sm_hover == SM_IDX_PROGRAMS) {
                sub_open = true;
                sub_hover = 0;
                compute_sub_rect();
            } else if (sm_hover == SM_IDX_FIRMWARE) {
                fw_open = true;
                fw_hover = uf2_file_count > 0 ? 0 : -1;
                compute_fw_rect();
            } else if (sm_hover == SM_IDX_SETTINGS) {
                set_open = true;
                set_hover = 0;
                compute_set_rect();
            }
            wm_mark_dirty();
        }
        return true;
    case 0x28: /* ENTER */
        if (sm_hover >= 0) {
            if (sm_hover == SM_IDX_PROGRAMS) {
                fw_open = false; fw_hover = -1;
                set_open = false; set_hover = -1;
                sub_open = true;
                sub_hover = 0;
                compute_sub_rect();
                wm_mark_dirty();
            } else if (sm_hover == SM_IDX_FIRMWARE) {
                sub_open = false; sub_hover = -1;
                set_open = false; set_hover = -1;
                fw_open = true;
                fw_hover = uf2_file_count > 0 ? 0 : -1;
                compute_fw_rect();
                wm_mark_dirty();
            } else if (sm_hover == SM_IDX_SETTINGS) {
                sub_open = false; sub_hover = -1;
                fw_open = false;  fw_hover = -1;
                set_open = true;
                set_hover = 0;
                compute_set_rect();
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
