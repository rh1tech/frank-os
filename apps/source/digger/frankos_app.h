/*
 * frankos_app.h - FRANK OS Shared App State for Digger
 *
 * Copyright (c) 2026 Mikhail Matveev <xtreme@rh1.tech>
 * https://rh1.tech
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef DIGGER_FRANKOS_APP_H
#define DIGGER_FRANKOS_APP_H

#include <stdint.h>
#include <stdbool.h>

#include "frankos-app.h"

/* Display constants matching original CGA resolution */
#define DIGGER_WIDTH   320
#define DIGGER_HEIGHT  200
#define DIGGER_FB_W    320
#define DIGGER_FB_H    240

/* Vertical offset to center 200-line game area in 240-line framebuffer */
#define DIGGER_Y_OFFSET 20

/* Internal framebuffer stride: 320 pixels / 2 = 160 bytes (nibble-packed) */
#define DIGGER_FB_STRIDE (DIGGER_FB_W / 2)

/* Key event FIFO size */
#define DIGGER_KBLEN 30

/* Audio: 22050 Hz sample rate, 22050 / 12.5 = 1764 samples per frame */
#define DIGGER_AUDIO_SAMPLE_RATE       22050
#define DIGGER_AUDIO_SAMPLES_PER_FRAME 1764

/* Menu command IDs */
#define CMD_NEW_GAME     1
#define CMD_SOUND        2
#define CMD_MUSIC        3
#define CMD_EXIT         4
#define CMD_ABOUT        5

/* Shared state between game loop (app task) and FRANK OS callbacks (WM task) */
typedef struct {
    void           *app_task;           /* FreeRTOS task handle */
    hwnd_t          app_hwnd;           /* Window handle */
    volatile bool   closing;            /* WM_CLOSE received */
    volatile bool   restart;            /* New Game requested */

    /* Internal framebuffer: 4-bit nibble-packed, 320x240 */
    uint8_t        *framebuffer;

    /* CGA palette 0-3 mapped to FRANK OS COLOR_* values */
    uint8_t         cga_to_color[4];

    /* PCM audio buffer (stereo, 3528 * 2 int16_t) */
    int16_t        *audio_buf;
    bool            audio_initialized;
    bool            audio_paused;

    /* Key state array: one entry per HID scancode */
    volatile uint8_t key_state[256];

    /* Key event FIFO for getkey() */
    int16_t         key_fifo[DIGGER_KBLEN];
    volatile int16_t key_fifo_len;
} digger_app_t;

/* Global app state pointer */
extern digger_app_t *g_app;

/* Paint callback (called from WM task) */
void digger_paint(hwnd_t hwnd);

/* Event handler (called from WM task) */
bool digger_event(hwnd_t hwnd, const window_event_t *event);

/* Audio fill (called from game loop) */
void audio_fill_and_submit(void);

/* Screen update (called from game loop) */
void doscreenupdate(void);

/* Menu setup and update */
void digger_setup_menu(void);
void digger_update_menu(void);

/* GetAsyncKeyState for rp2350_kbd.h compatibility */
bool GetAsyncKeyState(int key);

/* rp2350_kbd.h-compatible macros using GetAsyncKeyState */
#define rightpressed  (GetAsyncKeyState(keycodes[0][0]))
#define uppressed     (GetAsyncKeyState(keycodes[1][0]))
#define leftpressed   (GetAsyncKeyState(keycodes[2][0]))
#define downpressed   (GetAsyncKeyState(keycodes[3][0]))
#define f1pressed     (GetAsyncKeyState(keycodes[4][0]))
#define right2pressed (GetAsyncKeyState(keycodes[5][0]))
#define up2pressed    (GetAsyncKeyState(keycodes[6][0]))
#define left2pressed  (GetAsyncKeyState(keycodes[7][0]))
#define down2pressed  (GetAsyncKeyState(keycodes[8][0]))
#define f12pressed    (GetAsyncKeyState(keycodes[9][0]))

#endif /* DIGGER_FRANKOS_APP_H */
