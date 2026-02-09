#ifndef TERMINAL_H
#define TERMINAL_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "window.h"

/* Terminal console dimensions (8x16 font in 560x320 client area) */
#define TERM_COLS   70
#define TERM_ROWS   20
#define TERM_FONT_W  8
#define TERM_FONT_H  16

/*
 * Text-mode buffer layout (MOS2-compatible):
 *   Each cell is 2 bytes: [character][color_attribute]
 *   color_attribute = (bg << 4) | (fg & 0x0F)
 *   Row-major: cell(col, row) = buf[row * TERM_COLS * 2 + col * 2]
 */
#define TERM_TEXTBUF_SIZE   (TERM_COLS * TERM_ROWS * 2)  /* 4800 bytes */

typedef struct terminal terminal_t;

/* Create a terminal window. Returns the window handle. */
hwnd_t terminal_create(void);

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

/* Get the active terminal (for routing API calls) */
terminal_t *terminal_get_active(void);

/* Text-mode buffer access (for MOS2 get_buffer() / save/restore console) */
uint8_t *terminal_get_textbuf(terminal_t *t);
size_t   terminal_get_textbuf_size(terminal_t *t);

/* Force repaint of the active terminal (call after direct textbuf writes) */
void terminal_invalidate_active(void);

#endif /* TERMINAL_H */
