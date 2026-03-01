/*
 * pshell_vt100.h — VT100 terminal emulator for pshell on FRANK OS
 *
 * Provides a text-mode terminal grid with VT100 escape sequence parsing.
 * Characters written via vt100_putc() are processed through a state machine
 * that interprets cursor movement, clearing, and color attribute sequences.
 * The terminal buffer is rendered to a FRANK OS window framebuffer via
 * vt100_paint().
 *
 * Copyright (c) 2026 Mikhail Matveev <xtreme@rh1.tech>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef PSHELL_VT100_H
#define PSHELL_VT100_H

#include <stdint.h>
#include <stdbool.h>

/* Forward-declare hwnd_t (from frankos-app.h) */
typedef uint8_t hwnd_t;

/* ── Terminal dimensions ──────────────────────────────────────────────── */
#define VT100_DEFAULT_COLS   70
#define VT100_DEFAULT_ROWS   20
#define VT100_MAX_COLS       80
#define VT100_MAX_ROWS       30
#define VT100_FONT_W          8
#define VT100_FONT_H         16

/* ── Input ring buffer size ───────────────────────────────────────────── */
#define VT100_INBUF_SIZE    256

/* ── Initialisation & lifecycle ───────────────────────────────────────── */
void    vt100_init(int cols, int rows);
void    vt100_destroy(void);

/* ── Output: process characters through VT100 state machine ──────────── */
void    vt100_putc(char c);
void    vt100_puts_nl(const char *s);   /* puts with trailing \n */
int     vt100_printf(const char *fmt, ...) __attribute__((__format__(__printf__, 1, 2)));

/* ── Input: keyboard ring buffer ──────────────────────────────────────── */
int     vt100_getch(void);              /* blocking read */
int     vt100_getch_timeout(int us);    /* non-blocking with timeout */
void    vt100_ungetc(int c);            /* push char back */
void    vt100_input_push(int c);        /* push from event handler */
void    vt100_input_push_str(const char *s);

/* ── Rendering ────────────────────────────────────────────────────────── */
void    vt100_paint(hwnd_t hwnd);

/* ── Resize ───────────────────────────────────────────────────────────── */
void    vt100_resize(int cols, int rows);
void    vt100_get_size(int *cols, int *rows);

/* ── Cursor blink ─────────────────────────────────────────────────────── */
void    vt100_toggle_cursor(void);

/* ── Invalidate shadow buffer (force full repaint on next paint) ──────── */
void    vt100_invalidate(void);

/* ── Flush input buffer and clear stale notifications ─────────────────── */
void    vt100_input_flush(void);

/* ── Window handle (set by main.c after window creation) ──────────────── */
void    vt100_set_hwnd(hwnd_t hwnd);

/* ── Input waiter task (call from shell task to enable notify-based I/O) ─ */
void    vt100_set_waiter(void *task_handle);

#endif /* PSHELL_VT100_H */
