/*
 * MurmNES - NES Emulator for RP2350
 * Settings Menu Implementation
 * Copyright (c) 2026 Mikhail Matveev <xtreme@rh1.tech>
 * https://rh1.tech | https://github.com/rh1tech/murmnes
 * SPDX-License-Identifier: MIT
 */

#include "settings.h"
#include "quicknes.h"
#include "pico/stdlib.h"
#include "nespad.h"
#include "ps2kbd_wrapper.h"
#include "ff.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#ifdef USB_HID_ENABLED
#include "usbhid.h"
#endif

/* Symbols from main_pico.c — made non-static for menu access */
extern volatile const uint8_t *pending_pixels;
extern volatile long pending_pitch;
extern volatile uint32_t vsync_flag;
extern uint32_t rgb565_palette_32[2][256];
extern int pal_write_idx;
extern volatile int pending_pal_idx;
extern void audio_fill_silence(int count);

#define SAMPLE_RATE 44100

/* Screen dimensions (NES resolution) */
#define SCREEN_WIDTH 256
#define SCREEN_HEIGHT 240

/* Font constants (5x7 bitmap font, same as murmgenesis) */
#define FONT_WIDTH 6    /* 5px glyph + 1px spacing */
#define FONT_HEIGHT 7
#define LINE_HEIGHT 12

/* UI layout — centered in visible area (x=8..247, y=8..231) */
#define MENU_TITLE_Y 24
#define MENU_START_Y 52
#define MENU_X 24
#define VALUE_X 140

/* Palette indices for menu rendering */
#define PAL_BG      1
#define PAL_WHITE   2
#define PAL_YELLOW  3
#define PAL_GRAY    4

/* Menu items */
typedef enum {
    MENU_PLAYER1,
    MENU_PLAYER2,
    MENU_SEPARATOR1,
    MENU_AUDIO,
    MENU_VOLUME,
    MENU_SEPARATOR2,
    MENU_SAVE_GAME,
    MENU_LOAD_GAME,
    MENU_SEPARATOR3,
    MENU_RESET,
    MENU_EXIT,
    MENU_ITEM_COUNT
} menu_item_t;

/* Whether a save file exists for the current ROM */
static bool save_exists = false;

/* Temporary status message shown after save/load (countdown in frames) */
static const char *status_msg = NULL;
static int status_frames = 0;

/* Input mode names */
static const char *input_mode_names[] = {"ANY", "NES GAMEPAD 1", "NES GAMEPAD 2", "USB GAMEPAD 1", "USB GAMEPAD 2", "KEYBOARD", "DISABLED"};

/* Audio mode names */
static const char *audio_mode_names[] = {"HDMI", "I2S", "DISABLED"};

/* Global settings */
settings_t g_settings = {
    .p1_mode = INPUT_MODE_ANY,
    .p2_mode = INPUT_MODE_DISABLED,
    .audio_mode = AUDIO_MODE_HDMI,
    .volume = 100,
};

/* Local copy for editing */
static settings_t edit_settings;

/* ─── 5x7 bitmap font glyphs ──────────────────────────────────────── */

static const uint8_t glyphs_5x7[][7] = {
    [' '-' '] = {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
    ['!'-' '] = {0x04, 0x04, 0x04, 0x04, 0x00, 0x04, 0x00},
    ['"'-' '] = {0x0A, 0x0A, 0x0A, 0x00, 0x00, 0x00, 0x00},
    ['#'-' '] = {0x0A, 0x0A, 0x1F, 0x0A, 0x1F, 0x0A, 0x0A},
    ['$'-' '] = {0x04, 0x0F, 0x14, 0x0E, 0x05, 0x1E, 0x04},
    ['%'-' '] = {0x19, 0x1A, 0x04, 0x08, 0x0B, 0x13, 0x00},
    ['&'-' '] = {0x0C, 0x12, 0x14, 0x08, 0x15, 0x12, 0x0D},
    ['\''-' '] = {0x04, 0x04, 0x00, 0x00, 0x00, 0x00, 0x00},
    ['('-' '] = {0x02, 0x04, 0x08, 0x08, 0x08, 0x04, 0x02},
    [')'-' '] = {0x08, 0x04, 0x02, 0x02, 0x02, 0x04, 0x08},
    ['*'-' '] = {0x00, 0x04, 0x15, 0x0E, 0x15, 0x04, 0x00},
    ['+'-' '] = {0x00, 0x04, 0x04, 0x1F, 0x04, 0x04, 0x00},
    [','-' '] = {0x00, 0x00, 0x00, 0x00, 0x0C, 0x04, 0x08},
    ['-'-' '] = {0x00, 0x00, 0x00, 0x1F, 0x00, 0x00, 0x00},
    ['.'-' '] = {0x00, 0x00, 0x00, 0x00, 0x00, 0x0C, 0x0C},
    ['/'-' '] = {0x01, 0x02, 0x04, 0x08, 0x10, 0x00, 0x00},
    ['0'-' '] = {0x0E, 0x11, 0x13, 0x15, 0x19, 0x11, 0x0E},
    ['1'-' '] = {0x04, 0x0C, 0x04, 0x04, 0x04, 0x04, 0x0E},
    ['2'-' '] = {0x0E, 0x11, 0x01, 0x02, 0x04, 0x08, 0x1F},
    ['3'-' '] = {0x1E, 0x01, 0x01, 0x0E, 0x01, 0x01, 0x1E},
    ['4'-' '] = {0x02, 0x06, 0x0A, 0x12, 0x1F, 0x02, 0x02},
    ['5'-' '] = {0x1F, 0x10, 0x10, 0x1E, 0x01, 0x01, 0x1E},
    ['6'-' '] = {0x0E, 0x10, 0x10, 0x1E, 0x11, 0x11, 0x0E},
    ['7'-' '] = {0x1F, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08},
    ['8'-' '] = {0x0E, 0x11, 0x11, 0x0E, 0x11, 0x11, 0x0E},
    ['9'-' '] = {0x0E, 0x11, 0x11, 0x0F, 0x01, 0x01, 0x0E},
    [':'-' '] = {0x00, 0x0C, 0x0C, 0x00, 0x0C, 0x0C, 0x00},
    [';'-' '] = {0x00, 0x0C, 0x0C, 0x00, 0x0C, 0x04, 0x08},
    ['<'-' '] = {0x02, 0x04, 0x08, 0x10, 0x08, 0x04, 0x02},
    ['='-' '] = {0x00, 0x00, 0x1F, 0x00, 0x1F, 0x00, 0x00},
    ['>'-' '] = {0x08, 0x04, 0x02, 0x01, 0x02, 0x04, 0x08},
    ['?'-' '] = {0x0E, 0x11, 0x01, 0x02, 0x04, 0x00, 0x04},
    ['@'-' '] = {0x0E, 0x11, 0x17, 0x15, 0x17, 0x10, 0x0E},
    ['A'-' '] = {0x0E, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11},
    ['B'-' '] = {0x1E, 0x11, 0x11, 0x1E, 0x11, 0x11, 0x1E},
    ['C'-' '] = {0x0E, 0x11, 0x10, 0x10, 0x10, 0x11, 0x0E},
    ['D'-' '] = {0x1E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x1E},
    ['E'-' '] = {0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x1F},
    ['F'-' '] = {0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x10},
    ['G'-' '] = {0x0E, 0x11, 0x10, 0x17, 0x11, 0x11, 0x0E},
    ['H'-' '] = {0x11, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11},
    ['I'-' '] = {0x1F, 0x04, 0x04, 0x04, 0x04, 0x04, 0x1F},
    ['J'-' '] = {0x07, 0x02, 0x02, 0x02, 0x12, 0x12, 0x0C},
    ['K'-' '] = {0x11, 0x12, 0x14, 0x18, 0x14, 0x12, 0x11},
    ['L'-' '] = {0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x1F},
    ['M'-' '] = {0x11, 0x1B, 0x15, 0x15, 0x11, 0x11, 0x11},
    ['N'-' '] = {0x11, 0x19, 0x15, 0x13, 0x11, 0x11, 0x11},
    ['O'-' '] = {0x0E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E},
    ['P'-' '] = {0x1E, 0x11, 0x11, 0x1E, 0x10, 0x10, 0x10},
    ['Q'-' '] = {0x0E, 0x11, 0x11, 0x11, 0x15, 0x12, 0x0D},
    ['R'-' '] = {0x1E, 0x11, 0x11, 0x1E, 0x14, 0x12, 0x11},
    ['S'-' '] = {0x0F, 0x10, 0x10, 0x0E, 0x01, 0x01, 0x1E},
    ['T'-' '] = {0x1F, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04},
    ['U'-' '] = {0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E},
    ['V'-' '] = {0x11, 0x11, 0x11, 0x11, 0x0A, 0x0A, 0x04},
    ['W'-' '] = {0x11, 0x11, 0x11, 0x15, 0x15, 0x15, 0x0A},
    ['X'-' '] = {0x11, 0x0A, 0x04, 0x04, 0x04, 0x0A, 0x11},
    ['Y'-' '] = {0x11, 0x0A, 0x04, 0x04, 0x04, 0x04, 0x04},
    ['Z'-' '] = {0x1F, 0x02, 0x04, 0x08, 0x10, 0x10, 0x1F},
    ['['-' '] = {0x0E, 0x08, 0x08, 0x08, 0x08, 0x08, 0x0E},
    ['\\'-' '] = {0x10, 0x08, 0x04, 0x02, 0x01, 0x00, 0x00},
    [']'-' '] = {0x0E, 0x02, 0x02, 0x02, 0x02, 0x02, 0x0E},
    ['^'-' '] = {0x04, 0x0A, 0x11, 0x00, 0x00, 0x00, 0x00},
    ['_'-' '] = {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x1F},
    ['{'-' '] = {0x02, 0x04, 0x04, 0x08, 0x04, 0x04, 0x02},
    ['|'-' '] = {0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04},
    ['}'-' '] = {0x08, 0x04, 0x04, 0x02, 0x04, 0x04, 0x08},
    ['~'-' '] = {0x00, 0x00, 0x08, 0x15, 0x02, 0x00, 0x00},
};

static const uint8_t *glyph_5x7(char ch) {
    static const uint8_t glyph_space[7] = {0};
    int c = (unsigned char)ch;
    if (c >= 'a' && c <= 'z') c = c - 'a' + 'A';
    int idx = c - ' ';
    if (idx >= 0 && idx < (int)(sizeof(glyphs_5x7)/sizeof(glyphs_5x7[0])))
        return glyphs_5x7[idx];
    return glyph_space;
}

/* ─── Drawing primitives ──────────────────────────────────────────── */

static void draw_char(uint8_t *screen, int x, int y, char ch, uint8_t color) {
    const uint8_t *rows = glyph_5x7(ch);
    for (int row = 0; row < FONT_HEIGHT; ++row) {
        int yy = y + row;
        if (yy < 0 || yy >= SCREEN_HEIGHT) continue;
        uint8_t bits = rows[row];
        for (int col = 0; col < 5; ++col) {
            int xx = x + col;
            if (xx < 0 || xx >= SCREEN_WIDTH) continue;
            if (bits & (1u << (4 - col))) {
                screen[yy * SCREEN_WIDTH + xx] = color;
            }
        }
    }
}

static void draw_text(uint8_t *screen, int x, int y, const char *text, uint8_t color) {
    for (const char *p = text; *p; ++p) {
        draw_char(screen, x, y, *p, color);
        x += FONT_WIDTH;
    }
}

static void fill_rect(uint8_t *screen, int x, int y, int w, int h, uint8_t color) {
    if (w <= 0 || h <= 0) return;
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > SCREEN_WIDTH) w = SCREEN_WIDTH - x;
    if (y + h > SCREEN_HEIGHT) h = SCREEN_HEIGHT - y;
    if (w <= 0 || h <= 0) return;
    for (int yy = y; yy < y + h; ++yy) {
        memset(&screen[yy * SCREEN_WIDTH + x], color, (size_t)w);
    }
}

static void draw_hline(uint8_t *screen, int x, int y, int w, uint8_t color) {
    if (y < 0 || y >= SCREEN_HEIGHT) return;
    if (x < 0) { w += x; x = 0; }
    if (x + w > SCREEN_WIDTH) w = SCREEN_WIDTH - x;
    if (w <= 0) return;
    memset(&screen[y * SCREEN_WIDTH + x], color, (size_t)w);
}

/* ─── Menu item helpers ───────────────────────────────────────────── */

static const char *get_menu_label(menu_item_t item) {
    switch (item) {
        case MENU_PLAYER1:   return "PLAYER 1";
        case MENU_PLAYER2:   return "PLAYER 2";
        case MENU_AUDIO:     return "AUDIO";
        case MENU_VOLUME:    return "VOLUME";
        case MENU_SAVE_GAME: return (status_frames > 0) ? status_msg : "SAVE GAME";
        case MENU_LOAD_GAME: return save_exists ? "LOAD GAME" : "LOAD GAME (-)";
        case MENU_RESET:     return "BACK TO ROM SELECTOR";
        case MENU_EXIT:      return "BACK TO GAME";
        default:             return "";
    }
}

static bool is_selectable(menu_item_t item) {
    if (item == MENU_SEPARATOR1 || item == MENU_SEPARATOR2 || item == MENU_SEPARATOR3)
        return false;
    if (item == MENU_LOAD_GAME && !save_exists)
        return false;
    return true;
}

static int next_selectable(int sel, int dir) {
    int s = sel;
    do {
        s += dir;
        if (s < 0) s = MENU_ITEM_COUNT - 1;
        if (s >= MENU_ITEM_COUNT) s = 0;
        if (s == sel) break;
    } while (!is_selectable((menu_item_t)s));
    return s;
}

static char volume_text_buf[8];

static const char *get_value_text(menu_item_t item) {
    switch (item) {
        case MENU_PLAYER1: return input_mode_names[edit_settings.p1_mode];
        case MENU_PLAYER2: return input_mode_names[edit_settings.p2_mode];
        case MENU_AUDIO:   return audio_mode_names[edit_settings.audio_mode];
        case MENU_VOLUME:
            snprintf(volume_text_buf, sizeof(volume_text_buf), "%d%%", edit_settings.volume);
            return volume_text_buf;
        default:           return NULL;
    }
}

/* Check if a specific mode (not Any/Disabled) is already taken by the other player */
static bool mode_taken_by_other(uint8_t mode, uint8_t other_mode) {
    if (mode == INPUT_MODE_ANY || mode == INPUT_MODE_DISABLED)
        return false;
    return mode == other_mode;
}

static void change_value(menu_item_t item, int dir) {
    switch (item) {
        case MENU_PLAYER1: {
            /* P1: Any (only if P2=Disabled), NES1, NES2, USB, Keyboard — never Disabled.
             * Skip modes already claimed by P2. */
            uint8_t m = edit_settings.p1_mode;
            for (int i = 0; i < INPUT_MODE_COUNT; i++) {
                m = (uint8_t)((m + INPUT_MODE_COUNT + dir) % INPUT_MODE_COUNT);
                if (m == INPUT_MODE_DISABLED) continue;
                if (m == INPUT_MODE_ANY && edit_settings.p2_mode != INPUT_MODE_DISABLED) continue;
                if (mode_taken_by_other(m, edit_settings.p2_mode)) continue;
                break;
            }
            edit_settings.p1_mode = m;
            break;
        }
        case MENU_PLAYER2: {
            /* P2: NES1, NES2, USB, Keyboard, Disabled — never Any.
             * Skip modes already claimed by P1. */
            uint8_t m = edit_settings.p2_mode;
            for (int i = 0; i < INPUT_MODE_COUNT; i++) {
                m = (uint8_t)((m + INPUT_MODE_COUNT + dir) % INPUT_MODE_COUNT);
                if (m == INPUT_MODE_ANY) continue;
                if (mode_taken_by_other(m, edit_settings.p1_mode)) continue;
                break;
            }
            edit_settings.p2_mode = m;
            /* If P2 enabled and P1 is Any, force P1 to first available device */
            if (edit_settings.p2_mode != INPUT_MODE_DISABLED &&
                edit_settings.p1_mode == INPUT_MODE_ANY) {
                /* Pick first device not taken by P2 */
                for (uint8_t c = INPUT_MODE_NES1; c <= INPUT_MODE_KEYBOARD; c++) {
                    if (c != edit_settings.p2_mode) {
                        edit_settings.p1_mode = c;
                        break;
                    }
                }
            }
            break;
        }
        case MENU_AUDIO:
            edit_settings.audio_mode = (uint8_t)((edit_settings.audio_mode + AUDIO_MODE_COUNT + dir) % AUDIO_MODE_COUNT);
            break;
        case MENU_VOLUME: {
            int v = (int)edit_settings.volume + dir * VOLUME_STEP;
            if (v < VOLUME_MIN) v = VOLUME_MIN;
            if (v > VOLUME_MAX) v = VOLUME_MAX;
            edit_settings.volume = (uint8_t)v;
            break;
        }
        default:
            break;
    }
}

/* ─── Menu drawing ────────────────────────────────────────────────── */

static void draw_settings_menu(uint8_t *screen, int selected) {
    /* Clear screen */
    memset(screen, PAL_BG, SCREEN_WIDTH * SCREEN_HEIGHT);

    /* Title */
    const char *title = "SETTINGS";
    int title_x = (SCREEN_WIDTH - (int)strlen(title) * FONT_WIDTH) / 2;
    draw_text(screen, title_x, MENU_TITLE_Y, title, PAL_WHITE);

    /* Separator below title */
    draw_hline(screen, MENU_X, MENU_TITLE_Y + FONT_HEIGHT + 4, SCREEN_WIDTH - 2 * MENU_X, PAL_GRAY);

    /* Menu items */
    int y = MENU_START_Y;
    for (int i = 0; i < MENU_ITEM_COUNT; i++) {
        menu_item_t item = (menu_item_t)i;

        if (item == MENU_SEPARATOR1 || item == MENU_SEPARATOR2 || item == MENU_SEPARATOR3) {
            draw_hline(screen, MENU_X, y + LINE_HEIGHT / 2, SCREEN_WIDTH - 2 * MENU_X, PAL_GRAY);
            y += LINE_HEIGHT;
            continue;
        }

        uint8_t color;
        if (item == MENU_LOAD_GAME && !save_exists)
            color = PAL_GRAY;
        else
            color = (i == selected) ? PAL_YELLOW : PAL_WHITE;

        /* Selection indicator */
        if (i == selected) {
            draw_char(screen, MENU_X - 12, y, '>', color);
        }

        /* Label */
        draw_text(screen, MENU_X, y, get_menu_label(item), color);

        /* Value with arrows */
        const char *val = get_value_text(item);
        if (val) {
            char buf[24];
            snprintf(buf, sizeof(buf), "< %s >", val);
            draw_text(screen, VALUE_X, y, buf, color);
        }

        y += LINE_HEIGHT;
    }

    /* Help text at bottom */
    const char *help = "UP/DOWN:SELECT  LEFT/RIGHT:CHANGE";
    int help_x = (SCREEN_WIDTH - (int)strlen(help) * FONT_WIDTH) / 2;
    draw_text(screen, help_x, 220, help, PAL_GRAY);
}

/* ─── Menu palette setup ──────────────────────────────────────────── */

static void setup_menu_palette(void) {
    /* Use 16-bit RGB565 doubled into 32-bit words for scanline callback */
    uint32_t *pal = rgb565_palette_32[pal_write_idx];

    /* Clear all entries to black */
    for (int i = 0; i < 256; i++) {
        pal[i] = 0;
    }

    /* Background: very dark blue-gray */
    uint16_t bg = ((0x08 & 0xF8) << 8) | ((0x08 & 0xFC) << 3) | (0x10 >> 3);
    pal[PAL_BG] = bg | ((uint32_t)bg << 16);

    /* White */
    uint16_t white = 0xFFFF;
    pal[PAL_WHITE] = white | ((uint32_t)white << 16);

    /* Yellow */
    uint16_t yellow = ((0xFF & 0xF8) << 8) | ((0xFF & 0xFC) << 3) | (0x00 >> 3);
    pal[PAL_YELLOW] = yellow | ((uint32_t)yellow << 16);

    /* Gray */
    uint16_t gray = ((0x80 & 0xF8) << 8) | ((0x80 & 0xFC) << 3) | (0x80 >> 3);
    pal[PAL_GRAY] = gray | ((uint32_t)gray << 16);

    pending_pal_idx = pal_write_idx;
    pal_write_idx ^= 1;
}

/* ─── Input reading (merge all sources) ───────────────────────────── */

#define BTN_UP    0x10
#define BTN_DOWN  0x20
#define BTN_LEFT  0x40
#define BTN_RIGHT 0x80
#define BTN_A     0x01
#define BTN_B     0x02
#define BTN_START 0x08
#define BTN_SEL   0x04

static int read_menu_buttons(void) {
    nespad_read();
    ps2kbd_tick();

    int buttons = 0;

    /* NES gamepad (either player) */
    uint32_t pad = nespad_state | nespad_state2;
    if (pad & DPAD_A)      buttons |= BTN_A;
    if (pad & DPAD_B)      buttons |= BTN_B;
    if (pad & DPAD_SELECT) buttons |= BTN_SEL;
    if (pad & DPAD_START)  buttons |= BTN_START;
    if (pad & DPAD_UP)     buttons |= BTN_UP;
    if (pad & DPAD_DOWN)   buttons |= BTN_DOWN;
    if (pad & DPAD_LEFT)   buttons |= BTN_LEFT;
    if (pad & DPAD_RIGHT)  buttons |= BTN_RIGHT;

    /* PS/2 keyboard */
    uint16_t kbd = ps2kbd_get_state();
#ifdef USB_HID_ENABLED
    kbd |= usbhid_get_kbd_state();
#endif
    if (kbd & KBD_STATE_UP)     buttons |= BTN_UP;
    if (kbd & KBD_STATE_DOWN)   buttons |= BTN_DOWN;
    if (kbd & KBD_STATE_LEFT)   buttons |= BTN_LEFT;
    if (kbd & KBD_STATE_RIGHT)  buttons |= BTN_RIGHT;
    if (kbd & KBD_STATE_A)      buttons |= BTN_A;
    if (kbd & KBD_STATE_B)      buttons |= BTN_B;
    if (kbd & KBD_STATE_SELECT) buttons |= BTN_SEL;
    if (kbd & KBD_STATE_START)  buttons |= BTN_START;
    if (kbd & KBD_STATE_ESC)    buttons |= BTN_B;  /* ESC = back */

#ifdef USB_HID_ENABLED
    if (usbhid_gamepad_connected()) {
        usbhid_gamepad_state_t gp;
        usbhid_get_gamepad_state(&gp);
        if (gp.dpad & 0x01) buttons |= BTN_UP;
        if (gp.dpad & 0x02) buttons |= BTN_DOWN;
        if (gp.dpad & 0x04) buttons |= BTN_LEFT;
        if (gp.dpad & 0x08) buttons |= BTN_RIGHT;
        if (gp.buttons & 0x01) buttons |= BTN_A;
        if (gp.buttons & 0x02) buttons |= BTN_B;
        if (gp.buttons & 0x40) buttons |= BTN_START;
        if (gp.buttons & 0x80) buttons |= BTN_SEL;
    }
#endif

    return buttons;
}

/* ─── Save state (SD card) ────────────────────────────────────────── */

/* Shared FATFS instance to save ~1.1KB of static SRAM */
static FATFS shared_fs;

static void get_save_path(char *path, size_t path_size) {
    snprintf(path, path_size, "/nes/.save/%s.sav", g_rom_name);
}

static bool check_save_exists(void) {
    if (g_rom_name[0] == '\0') {
        printf("check_save: no rom name\n");
        return false;
    }

    char path[128];
    get_save_path(path, sizeof(path));
    printf("check_save: path=%s\n", path);

    
    FRESULT fr = f_mount(&shared_fs, "", 1);
    if (fr != FR_OK) {
        printf("check_save: mount failed (%d)\n", fr);
        return false;
    }

    FILINFO fno;
    bool exists = (f_stat(path, &fno) == FR_OK);
    f_unmount("");
    printf("check_save: exists=%d\n", exists);
    return exists;
}

static const char *save_error = NULL;

static bool do_save_game(void) {
    
    save_error = NULL;

    printf("do_save: rom_name='%s'\n", g_rom_name);
    if (g_rom_name[0] == '\0') { save_error = "NO ROM NAME"; return false; }

    FRESULT fr = f_mount(&shared_fs, "", 1);
    printf("do_save: f_mount=%d\n", fr);
    if (fr != FR_OK) { save_error = "SD MOUNT FAIL"; return false; }

    f_mkdir("/nes");
    f_mkdir("/nes/.save");

    char path[128];
    get_save_path(path, sizeof(path));
    printf("do_save: saving to %s\n", path);

    FIL file;
    FRESULT fro = f_open(&file, path, FA_WRITE | FA_CREATE_ALWAYS);
    printf("do_save: f_open=%d\n", fro);
    if (fro != FR_OK) {
        f_unmount("");
        save_error = "FILE OPEN FAIL";
        return false;
    }

    /* Stream state directly to file — no intermediate buffer */
    int ret = qnes_save_state(&file);
    printf("do_save: qnes_save_state=%d\n", ret);
    f_close(&file);
    f_unmount("");

    if (ret != 0) { save_error = "STATE FAILED"; return false; }

    printf("do_save: OK\n");
    return true;
}

static bool do_load_game(void) {
    if (g_rom_name[0] == '\0') return false;

    
    if (f_mount(&shared_fs, "", 1) != FR_OK) return false;

    char path[128];
    get_save_path(path, sizeof(path));
    printf("do_load: loading from %s\n", path);

    FIL file;
    if (f_open(&file, path, FA_READ) != FR_OK) {
        f_unmount("");
        return false;
    }

    long fsize = (long)f_size(&file);

    /* Stream state directly from file — no intermediate buffer */
    int ret = qnes_load_state(&file, fsize);
    printf("do_load: qnes_load_state=%d\n", ret);
    f_close(&file);
    f_unmount("");

    return ret == 0;
}

/* ─── Settings persistence (SD card) ──────────────────────────────── */

#define SETTINGS_PATH "/nes/.settings"

static bool parse_ini_line(const char *line, const char *key, char *value, size_t value_size) {
    while (*line == ' ' || *line == '\t') line++;
    size_t key_len = strlen(key);
    if (strncmp(line, key, key_len) != 0) return false;
    line += key_len;
    while (*line == ' ' || *line == '\t') line++;
    if (*line != '=') return false;
    line++;
    while (*line == ' ' || *line == '\t') line++;
    size_t i = 0;
    while (*line && *line != '\n' && *line != '\r' && i < value_size - 1) {
        value[i++] = *line++;
    }
    value[i] = '\0';
    while (i > 0 && (value[i-1] == ' ' || value[i-1] == '\t')) {
        value[--i] = '\0';
    }
    return true;
}

static int parse_input_mode(const char *value) {
    if (strcmp(value, "any") == 0 || strcmp(value, "0") == 0) return INPUT_MODE_ANY;
    if (strcmp(value, "nes1") == 0 || strcmp(value, "1") == 0) return INPUT_MODE_NES1;
    if (strcmp(value, "nes2") == 0 || strcmp(value, "2") == 0) return INPUT_MODE_NES2;
    if (strcmp(value, "usb1") == 0 || strcmp(value, "3") == 0) return INPUT_MODE_USB1;
    if (strcmp(value, "usb2") == 0 || strcmp(value, "4") == 0) return INPUT_MODE_USB2;
    if (strcmp(value, "keyboard") == 0 || strcmp(value, "5") == 0) return INPUT_MODE_KEYBOARD;
    if (strcmp(value, "disabled") == 0 || strcmp(value, "6") == 0) return INPUT_MODE_DISABLED;
    return -1;
}

static const char *input_mode_ini_names[] = {"any", "nes1", "nes2", "usb1", "usb2", "keyboard", "disabled"};
static const char *audio_mode_ini_names[] = {"hdmi", "i2s", "disabled"};

void settings_load(void) {

    if (f_mount(&shared_fs, "", 1) != FR_OK) return;

    FIL file;
    if (f_open(&file, SETTINGS_PATH, FA_READ) != FR_OK) {
        f_unmount("");
        return;
    }

    char line[128], value[64];
    while (f_gets(line, sizeof(line), &file)) {
        if (parse_ini_line(line, "player1", value, sizeof(value))) {
            int m = parse_input_mode(value);
            if (m >= 0) g_settings.p1_mode = (uint8_t)m;
        }
        else if (parse_ini_line(line, "player2", value, sizeof(value))) {
            int m = parse_input_mode(value);
            if (m >= 0) g_settings.p2_mode = (uint8_t)m;
        }
        else if (parse_ini_line(line, "audio", value, sizeof(value))) {
            if (strcmp(value, "i2s") == 0 || strcmp(value, "1") == 0)
                g_settings.audio_mode = AUDIO_MODE_I2S;
            else if (strcmp(value, "disabled") == 0 || strcmp(value, "2") == 0)
                g_settings.audio_mode = AUDIO_MODE_DISABLED;
            else
                g_settings.audio_mode = AUDIO_MODE_HDMI;
        }
        else if (parse_ini_line(line, "volume", value, sizeof(value))) {
            int v = atoi(value);
            if (v >= VOLUME_MIN && v <= VOLUME_MAX)
                g_settings.volume = (uint8_t)v;
        }
    }

    f_close(&file);
    f_unmount("");
    printf("Settings loaded from %s\n", SETTINGS_PATH);
}

void settings_save(void) {

    if (f_mount(&shared_fs, "", 1) != FR_OK) return;

    f_mkdir("/nes");

    FIL file;
    if (f_open(&file, SETTINGS_PATH, FA_WRITE | FA_CREATE_ALWAYS) != FR_OK) {
        f_unmount("");
        return;
    }

    char buf[256];
    snprintf(buf, sizeof(buf),
        "; MurmNES Settings\n"
        "player1 = %s\n"
        "player2 = %s\n"
        "audio = %s\n"
        "volume = %d\n",
        input_mode_ini_names[g_settings.p1_mode],
        input_mode_ini_names[g_settings.p2_mode],
        audio_mode_ini_names[g_settings.audio_mode],
        g_settings.volume);

    UINT bw;
    f_write(&file, buf, strlen(buf), &bw);
    f_close(&file);
    f_unmount("");
    printf("Settings saved to %s\n", SETTINGS_PATH);
}

/* ─── Hotkey detection ────────────────────────────────────────────── */

bool settings_check_hotkey(void) {
    /* NES gamepad: Start + Select */
    bool triggered = (nespad_state & DPAD_SELECT) && (nespad_state & DPAD_START);

    /* PS/2 / USB keyboard: ESC or F12 */
    uint16_t kbd = ps2kbd_get_state();
#ifdef USB_HID_ENABLED
    kbd |= usbhid_get_kbd_state();
#endif
    if (kbd & KBD_STATE_F12) triggered = true;

#ifdef USB_HID_ENABLED
    /* USB gamepad: Start + Select */
    if (usbhid_gamepad_connected()) {
        usbhid_gamepad_state_t gp;
        usbhid_get_gamepad_state(&gp);
        if ((gp.buttons & 0x40) && (gp.buttons & 0x80)) triggered = true;
    }
#endif

    return triggered;
}

/* ─── Menu main loop ──────────────────────────────────────────────── */

static void menu_wait_vsync(void) {
    while (pending_pixels != NULL) {
#ifdef USB_HID_ENABLED
        usbhid_task();
#endif
        __asm volatile ("wfe");
    }
    vsync_flag = 0;
}

settings_result_t settings_menu_show(uint8_t *screen_buffer) {
    /* Copy current settings for editing */
    edit_settings = g_settings;

    /* Check if a save file exists for this ROM */
    save_exists = check_save_exists();
    status_frames = 0;

    /* Set up menu palette */
    setup_menu_palette();

    int selected = MENU_PLAYER1;

    /* Auto-repeat state */
    uint32_t hold_counter = 0;
    const uint32_t REPEAT_DELAY = 10;
    const uint32_t REPEAT_RATE = 3;

    /* Wait for all buttons to be released before entering menu */
    for (int i = 0; i < 30; i++) {
        menu_wait_vsync();
        int btn = read_menu_buttons();
        if (btn == 0) break;
        audio_fill_silence(SAMPLE_RATE / 60);
        draw_settings_menu(screen_buffer, selected);
        pending_pitch = SCREEN_WIDTH;
        pending_pixels = screen_buffer;
    }
    int prev_buttons = read_menu_buttons();  /* ignore any still-held buttons */

    while (1) {
        menu_wait_vsync();

        int buttons = read_menu_buttons();

        /* Determine which buttons are newly pressed (edge detection + repeat) */
        int pressed = buttons & ~prev_buttons;
        if (buttons != 0 && buttons == prev_buttons) {
            hold_counter++;
            if (hold_counter > REPEAT_DELAY && (hold_counter % REPEAT_RATE) == 0) {
                pressed = buttons;
            }
        } else {
            hold_counter = 0;
        }
        prev_buttons = buttons;

        /* Navigation */
        if (pressed & BTN_UP) {
            selected = next_selectable(selected, -1);
        }
        if (pressed & BTN_DOWN) {
            selected = next_selectable(selected, 1);
        }

        /* Value change */
        if (pressed & BTN_LEFT) {
            change_value((menu_item_t)selected, -1);
        }
        if (pressed & BTN_RIGHT) {
            change_value((menu_item_t)selected, 1);
        }

        /* Confirm */
        if (pressed & (BTN_A | BTN_START)) {
            if (selected == MENU_EXIT) {
                g_settings = edit_settings;
                settings_save();
                break;
            }
            if (selected == MENU_SAVE_GAME) {
                bool ok = do_save_game();
                if (ok) {
                    save_exists = true;
                    status_msg = "SAVED.";
                } else {
                    status_msg = save_error ? save_error : "SAVE FAILED.";
                }
                status_frames = 120;
                continue;
            }
            if (selected == MENU_LOAD_GAME && save_exists) {
                do_load_game();
                g_settings = edit_settings;
                settings_save();
                break;  /* exit menu after loading state */
            }
            if (selected == MENU_RESET) {
                g_settings = edit_settings;
                settings_save();
                /* Wait for buttons to be released before returning */
                for (int i = 0; i < 60; i++) {
                    menu_wait_vsync();
                    audio_fill_silence(SAMPLE_RATE / 60);
                    draw_settings_menu(screen_buffer, selected);
                    pending_pitch = SCREEN_WIDTH;
                    pending_pixels = screen_buffer;
                    if (read_menu_buttons() == 0) break;
                }
                return SETTINGS_RESULT_RESET;
            }
            /* For value items, A/Start cycles forward */
            change_value((menu_item_t)selected, 1);
        }

        /* Back / Exit */
        if (pressed & BTN_B) {
            g_settings = edit_settings;
            settings_save();
            break;
        }

        /* Tick status message countdown */
        if (status_frames > 0) status_frames--;

        /* Draw and display */
        draw_settings_menu(screen_buffer, selected);
        audio_fill_silence(SAMPLE_RATE / 60);
        pending_pitch = SCREEN_WIDTH;
        pending_pixels = screen_buffer;
    }

    /* Wait for buttons to be released before returning to game */
    for (int i = 0; i < 60; i++) {
        menu_wait_vsync();
        int btn = read_menu_buttons();
        audio_fill_silence(SAMPLE_RATE / 60);
        draw_settings_menu(screen_buffer, selected);
        pending_pitch = SCREEN_WIDTH;
        pending_pixels = screen_buffer;
        if (btn == 0) break;
    }

    return SETTINGS_RESULT_EXIT;
}
