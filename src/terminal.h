/*
 * FRANK OS
 * Copyright (c) 2025 Mikhail Matveev <xtreme@rh1.tech>
 * https://rh1.tech
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef TERMINAL_H
#define TERMINAL_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "window.h"
#include "keyboard.h"
#include "FreeRTOS.h"
#include "task.h"
#include "semphr.h"
#include "timers.h"

/* Terminal console dimensions (8x16 font in 560x320 client area) */
#define TERM_COLS   70   /* default columns */
#define TERM_ROWS   20   /* default rows */
#define TERM_FONT_W  8
#define TERM_FONT_H  16

/* Maximum grid dimensions (fullscreen 640x480 borderless) */
#define TERM_MAX_COLS  80
#define TERM_MAX_ROWS  30
#define TERM_MAX_TEXTBUF_SIZE  (TERM_MAX_COLS * TERM_MAX_ROWS * 2)  /* 4800 bytes */

/*
 * Text-mode buffer layout (MOS2-compatible):
 *   Each cell is 2 bytes: [character][color_attribute]
 *   color_attribute = (bg << 4) | (fg & 0x0F)
 *   Row-major: cell(col, row) = buf[row * cols * 2 + col * 2]
 */
#define TERM_TEXTBUF_SIZE   (TERM_COLS * TERM_ROWS * 2)  /* default size */

/* Maximum stdin waiters per terminal (MOS2 apps) */
#define MAX_STDIN_WAITERS 4

/* FreeRTOS TLS slot for per-task terminal pointer */
#define TERMINAL_TLS_SLOT 1

#ifndef TERMINAL_T_DEFINED
#define TERMINAL_T_DEFINED
typedef struct terminal terminal_t;
#endif

struct terminal {
    /* Unified text-mode buffer — IS the MOS2 screen buffer */
    uint8_t *textbuf;
    size_t   textbuf_size;

    /* Current grid dimensions (dynamic — changes on resize) */
    int      cols, rows;

    int      cursor_col, cursor_row;
    uint8_t  fg_color, bg_color;
    bool     cursor_visible;

    /* Keyboard input ring buffer (for terminal_getch) */
    uint8_t  input_buf[64];
    uint8_t  in_head, in_tail;
    SemaphoreHandle_t input_sem;

    /* Window handle */
    hwnd_t   hwnd;

    /* Cursor blink timer */
    TimerHandle_t blink_timer;

    /* MOS2 keyboard state — per-terminal */
    volatile int           mos2_c;                  /* replaces global __c */
    scancode_handler_t     mos2_scancode_handler;   /* replaces global g_scancode_handler */
    cp866_handler_t        mos2_cp866_handler;      /* replaces global g_cp866_handler */
    TaskHandle_t           mos2_stdin_waiters[MAX_STDIN_WAITERS];
    int                    mos2_num_stdin_waiters;

    /* Closing flag — set by WM_CLOSE handler, checked by shell task */
    volatile bool          closing;

    /* Shell task handle — so terminal_force_close can wake the shell
     * when it's blocked in ulTaskNotifyTake waiting for a child app */
    TaskHandle_t           shell_task;
};

/* Create a terminal window. Returns the window handle. */
hwnd_t terminal_create(void);

/* Destroy a terminal: stop blink timer, destroy window, free memory */
void terminal_destroy(terminal_t *t);

/* Look up a terminal from its window handle */
terminal_t *terminal_from_hwnd(hwnd_t hwnd);

/* Get the window handle from a terminal */
hwnd_t terminal_get_hwnd(terminal_t *t);

/* Per-task terminal via FreeRTOS TLS */
void terminal_set_task_terminal(terminal_t *t);
terminal_t *terminal_get_task_terminal(void);

/* Console output */
void terminal_putc(terminal_t *t, char c);
void terminal_puts(terminal_t *t, const char *s);
void terminal_printf(terminal_t *t, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));
void terminal_clear(terminal_t *t, uint8_t color);

/* Cursor positioning and colors */
void terminal_set_cursor(terminal_t *t, int col, int row);
void terminal_set_color(terminal_t *t, uint8_t fg, uint8_t bg);

/* Direct grid write — writes string at (col, row) without moving cursor */
void terminal_draw_text(terminal_t *t, const char *str, int col, int row,
                        uint8_t fg, uint8_t bg);

/* Cursor position queries */
int terminal_get_cursor_col(terminal_t *t);
int terminal_get_cursor_row(terminal_t *t);

/* Keyboard input */
int terminal_getch(terminal_t *t);       /* blocking */
int terminal_getch_now(terminal_t *t);   /* non-blocking, -1 if none */

/* Get the active terminal (for routing API calls):
 * 1. Try TLS slot for current task
 * 2. Fall back to focused window's terminal */
terminal_t *terminal_get_active(void);

/* Text-mode buffer access (for MOS2 get_buffer() / save/restore console) */
uint8_t *terminal_get_textbuf(terminal_t *t);
size_t   terminal_get_textbuf_size(terminal_t *t);

/* Resize terminal grid to fit new client dimensions */
void terminal_resize(terminal_t *t, int client_w, int client_h);

/* Query current grid dimensions */
int terminal_get_cols(terminal_t *t);
int terminal_get_rows(terminal_t *t);

/* Force repaint of the active terminal (call after direct textbuf writes) */
void terminal_invalidate_active(void);

/* Notify per-terminal stdin waiters (called from keyboard.c) */
void terminal_notify_stdin_ready(terminal_t *t);

/* Spawn a new terminal window with its own shell task (defined in main.c) */
void spawn_terminal_window(void);

#endif /* TERMINAL_H */
