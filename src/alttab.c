/*
 * FRANK OS — Alt+Tab Window Switcher Overlay
 * Copyright (c) 2026 Mikhail Matveev <xtreme@rh1.tech>
 *
 * Windows 95-style Alt+Tab overlay: shows a centered bar with icons
 * and titles of all open app windows.  Pressing Tab while holding
 * Alt cycles through entries; releasing Alt commits the selection.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "alttab.h"
#include "lang.h"
#include "window.h"
#include "swap.h"
#include "gfx.h"
#include "font.h"
#include "display.h"

#include "desktop.h"

#include <string.h>

/* ── Layout constants ─────────────────────────────────────────── */

#define AT_ICON_SIZE      32       /* 32x32 pixel icons */
#define AT_CELL_W         48       /* width of each icon cell */
#define AT_CELL_H         48       /* height of each icon cell (icon + gap) */
#define AT_PAD_X          8        /* horizontal padding inside the box */
#define AT_PAD_TOP        8        /* padding above icons */
#define AT_PAD_BOTTOM     4        /* padding below title text */
#define AT_TITLE_H        (FONT_UI_HEIGHT + 4) /* title text area height */
#define AT_SEL_PAD        3        /* padding around selected icon highlight */
#define AT_MAX_ENTRIES    (WM_MAX_WINDOWS + 1)  /* +1 for Desktop entry */

/* ── State ────────────────────────────────────────────────────── */

static bool     at_active = false;
static uint8_t  at_count  = 0;
static uint8_t  at_sel    = 0;         /* selected index */
static hwnd_t   at_original_focus;     /* focus before overlay opened */

typedef struct {
    hwnd_t        hwnd;
    const uint8_t *icon;               /* pointer to 32x32 icon data */
    char          title[24];
} at_entry_t;

static at_entry_t at_entries[AT_MAX_ENTRIES];

/* Overlay geometry (screen coordinates) */
static int16_t at_x, at_y, at_w, at_h;

/* ── Helpers ──────────────────────────────────────────────────── */

extern const uint8_t default_icon_16x16[256];

/* Default 32x32 icon (upscaled from default_icon_16x16 on first use) */
static uint8_t at_default_icon32[1024];
static bool at_default_ready = false;

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

static const uint8_t *get_default_icon32(void) {
    if (!at_default_ready) {
        nn_upscale_16_to_32(default_icon_16x16, at_default_icon32);
        at_default_ready = true;
    }
    return at_default_icon32;
}

/* Build the entry list from current windows.
 * Includes all ALIVE | BORDER windows regardless of minimised/suspended state.
 * Order: top of z-stack first (most recent), so the first entry is the
 * currently focused window and the second is the "switch to" target. */
static void build_list(void) {
    at_count = 0;
    /* Iterate windows 1..WM_MAX_WINDOWS.  We want z-order top→bottom,
     * so sort by z_order descending. Simple selection: WM_MAX_WINDOWS ≤ 16. */
    hwnd_t sorted[WM_MAX_WINDOWS];
    uint8_t n = 0;
    for (hwnd_t h = 1; h <= WM_MAX_WINDOWS; h++) {
        window_t *w = wm_get_window(h);
        if (!w) continue;
        if (!(w->flags & WF_ALIVE)) continue;
        if (!(w->flags & WF_BORDER)) continue;  /* skip borderless popups */
        sorted[n++] = h;
    }
    /* Sort by z_order descending (topmost first) */
    for (uint8_t i = 0; i < n; i++) {
        for (uint8_t j = i + 1; j < n; j++) {
            window_t *wi = wm_get_window(sorted[i]);
            window_t *wj = wm_get_window(sorted[j]);
            if (wj->z_order > wi->z_order) {
                hwnd_t tmp = sorted[i];
                sorted[i] = sorted[j];
                sorted[j] = tmp;
            }
        }
    }
    /* Fill entries */
    for (uint8_t i = 0; i < n && at_count < AT_MAX_ENTRIES; i++) {
        window_t *w = wm_get_window(sorted[i]);
        at_entry_t *e = &at_entries[at_count];
        e->hwnd = sorted[i];
        e->icon = w->icon32 ? w->icon32 : get_default_icon32();
        strncpy(e->title, w->title, sizeof(e->title) - 1);
        e->title[sizeof(e->title) - 1] = '\0';
        at_count++;
    }

    /* Append a "Desktop" entry when shortcuts exist */
    if (desktop_has_shortcuts() && at_count < AT_MAX_ENTRIES) {
        at_entry_t *e = &at_entries[at_count];
        e->hwnd = HWND_NULL;  /* sentinel: means "desktop" */
        e->icon = desktop_get_icon32();
        strncpy(e->title, L(STR_ALTTAB_DESKTOP), sizeof(e->title) - 1);
        e->title[sizeof(e->title) - 1] = '\0';
        at_count++;
    }
}

/* Compute overlay position (centered on screen) */
static void calc_geometry(void) {
    at_w = at_count * AT_CELL_W + AT_PAD_X * 2;
    at_h = AT_PAD_TOP + AT_CELL_H + AT_TITLE_H + AT_PAD_BOTTOM;

    /* Clamp width to screen */
    if (at_w > display_width - 16)
        at_w = display_width - 16;

    at_x = (display_width  - at_w) / 2;
    at_y = (display_height - at_h) / 2;
}

/* ── Public API ───────────────────────────────────────────────── */

bool alttab_is_active(void) {
    return at_active;
}

void alttab_open(void) {
    build_list();
    if (at_count < 1) return;  /* no windows at all */

    at_active = true;
    at_original_focus = wm_get_focus();
    at_sel = (at_count >= 2) ? 1 : 0;  /* pre-select second entry, or only entry */
    calc_geometry();
    /* Deactivate the current window so it appears inactive while the
     * overlay is visible — focus is restored on commit or cancel. */
    wm_set_focus(HWND_NULL);
}

void alttab_cycle(void) {
    if (!at_active || at_count == 0) return;
    at_sel = (at_sel + 1) % at_count;
}

void alttab_commit(void) {
    if (!at_active) return;
    at_active = false;

    hwnd_t target = at_entries[at_sel].hwnd;

    /* "Desktop" entry — minimize all windows and give desktop focus */
    if (target == HWND_NULL) {
        /* Minimize every visible window */
        for (hwnd_t h = 1; h <= WM_MAX_WINDOWS; h++) {
            window_t *w = wm_get_window(h);
            if (!w) continue;
            if (!(w->flags & WF_ALIVE)) continue;
            if (w->state == WS_MINIMIZED) continue;
            if (!(w->flags & WF_VISIBLE)) continue;
            wm_minimize_window(h);
        }
        wm_set_focus(HWND_NULL);
        swap_switch_to(HWND_NULL);
        desktop_focus();
        wm_force_full_repaint();
        return;
    }

    window_t *w = wm_get_window(target);
    if (!w) {
        wm_force_full_repaint();
        return;
    }

    /* Restore minimized window */
    if (w->state == WS_MINIMIZED) {
        wm_restore_window(target);
    }

    /* Resume suspended app + set focus */
    swap_switch_to(target);
    wm_set_focus(target);
    wm_force_full_repaint();
}

void alttab_cancel(void) {
    if (!at_active) return;
    at_active = false;
    /* Restore focus to the window that was active before the overlay */
    wm_set_focus(at_original_focus);
    wm_force_full_repaint();
}

void alttab_draw(void) {
    if (!at_active) return;

    /* ── Background box with Win95-style raised bevel ── */
    gfx_fill_rect(at_x, at_y, at_w, at_h, COLOR_LIGHT_GRAY);

    /* Outer highlight (top-left = white, bottom-right = dark gray) */
    gfx_hline(at_x, at_y, at_w, COLOR_WHITE);
    gfx_vline(at_x, at_y, at_h, COLOR_WHITE);
    gfx_hline(at_x, at_y + at_h - 1, at_w, COLOR_DARK_GRAY);
    gfx_vline(at_x + at_w - 1, at_y, at_h, COLOR_DARK_GRAY);

    /* Inner shadow (1px inset) */
    gfx_hline(at_x + 1, at_y + 1, at_w - 2, COLOR_WHITE);
    gfx_vline(at_x + 1, at_y + 1, at_h - 2, COLOR_WHITE);
    gfx_hline(at_x + 1, at_y + at_h - 2, at_w - 2, COLOR_BLACK);
    gfx_vline(at_x + at_w - 2, at_y + 1, at_h - 2, COLOR_BLACK);

    /* ── Draw icon cells ── */
    int icons_total_w = at_count * AT_CELL_W;
    int icons_x0 = at_x + (at_w - icons_total_w) / 2;  /* center icons */
    int icons_y0 = at_y + AT_PAD_TOP;

    for (uint8_t i = 0; i < at_count; i++) {
        at_entry_t *e = &at_entries[i];
        int cx = icons_x0 + i * AT_CELL_W;
        int icon_x = cx + (AT_CELL_W - AT_ICON_SIZE) / 2;
        int icon_y = icons_y0 + 4;

        /* Selection highlight: sunken rectangle around the icon */
        if (i == at_sel) {
            int sx = icon_x - AT_SEL_PAD;
            int sy = icon_y - AT_SEL_PAD;
            int sw = AT_ICON_SIZE + AT_SEL_PAD * 2;
            int sh = AT_ICON_SIZE + AT_SEL_PAD * 2;

            /* Sunken bevel */
            gfx_hline(sx, sy, sw, COLOR_DARK_GRAY);
            gfx_vline(sx, sy, sh, COLOR_DARK_GRAY);
            gfx_hline(sx, sy + sh - 1, sw, COLOR_WHITE);
            gfx_vline(sx + sw - 1, sy, sh, COLOR_WHITE);

            /* Fill inside */
            gfx_fill_rect(sx + 1, sy + 1, sw - 2, sh - 2, COLOR_BLUE);
        }

        /* Draw 32x32 icon */
        gfx_draw_icon_32(icon_x, icon_y, e->icon);
    }

    /* ── Title of selected entry (centered below icons, clipped) ── */
    if (at_sel < at_count) {
        const char *title = at_entries[at_sel].title;
        int title_w = gfx_utf8_charcount(title) * FONT_UI_WIDTH;
        int tx = at_x + (at_w - title_w) / 2;
        int ty = icons_y0 + AT_CELL_H;

        /* Clamp text start to overlay bounds */
        if (tx < at_x + AT_PAD_X)
            tx = at_x + AT_PAD_X;

        gfx_text_ui_clipped(tx, ty, title, COLOR_BLACK, COLOR_LIGHT_GRAY,
                            at_x + 2, at_y, at_w - 4, at_h);
    }
}
