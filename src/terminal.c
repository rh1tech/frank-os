#include "terminal.h"
#include "window.h"
#include "window_event.h"
#include "window_draw.h"
#include "window_theme.h"
#include "font.h"
#include "display.h"
#include "FreeRTOS.h"
#include "semphr.h"
#include "timers.h"
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

/*==========================================================================
 * Text-mode buffer layout (MOS2-compatible)
 *
 * Each cell is 2 bytes: [character][color_attribute]
 * color_attribute = (bg << 4) | (fg & 0x0F)
 *
 * This is the SAME buffer returned by get_buffer() for MOS2 apps.
 * When MOS2 code calls save_console / restore_console / or writes
 * directly to the buffer, changes are reflected in the terminal.
 *=========================================================================*/

/* op_console computes buffer size as (screen_w * screen_h * bitness) >> 3.
 * With our values (320*240*4)/8 = 38400.  Allocate at least that much so
 * save/restore_console don't overflow, even though only 2800 bytes are
 * the actual text content (70*20*2). */
#define SCREEN_BUF_COMPAT_SIZE  ((320 * 240 * 4) >> 3)  /* 38400 */

/* Buffer access macros */
#define TB_OFF(row, col)   ((row) * TERM_COLS * 2 + (col) * 2)
#define TB_CHAR(t, r, c)   ((t)->textbuf[TB_OFF(r, c)])
#define TB_ATTR(t, r, c)   ((t)->textbuf[TB_OFF(r, c) + 1])
#define TB_PACK(fg, bg)    (((bg) << 4) | ((fg) & 0x0F))
#define TB_FG(attr)        ((attr) & 0x0F)
#define TB_BG(attr)        ((attr) >> 4)

/*==========================================================================
 * Terminal state
 *=========================================================================*/

struct terminal {
    /* Unified text-mode buffer — IS the MOS2 screen buffer */
    uint8_t *textbuf;
    size_t   textbuf_size;

    int      cursor_col, cursor_row;
    uint8_t  fg_color, bg_color;
    bool     cursor_visible;

    /* Keyboard input ring buffer */
    uint8_t  input_buf[64];
    uint8_t  in_head, in_tail;
    SemaphoreHandle_t input_sem;

    /* Window handle */
    hwnd_t   hwnd;

    /* Cursor blink timer */
    TimerHandle_t blink_timer;
};

/* Single global terminal instance */
static terminal_t g_terminal;
static terminal_t *g_active_terminal = NULL;

/*==========================================================================
 * Helpers
 *=========================================================================*/

static void terminal_scroll_up(terminal_t *t) {
    /* Move rows 1..TERM_ROWS-1 up to rows 0..TERM_ROWS-2 */
    memmove(t->textbuf,
            t->textbuf + TERM_COLS * 2,
            TERM_COLS * 2 * (TERM_ROWS - 1));
    /* Clear last row */
    uint8_t attr = TB_PACK(t->fg_color, t->bg_color);
    uint8_t *last = t->textbuf + TERM_COLS * 2 * (TERM_ROWS - 1);
    for (int i = 0; i < TERM_COLS; i++) {
        last[i * 2]     = ' ';
        last[i * 2 + 1] = attr;
    }
}

static void terminal_input_push(terminal_t *t, uint8_t ch) {
    uint8_t next = (t->in_head + 1) & 63;
    if (next == t->in_tail) return; /* full — drop */
    t->input_buf[t->in_head] = ch;
    t->in_head = next;
    xSemaphoreGive(t->input_sem);
}

/*==========================================================================
 * Paint handler — draws the character grid using 8x16 font
 *=========================================================================*/

static void terminal_paint(hwnd_t hwnd) {
    terminal_t *t = &g_terminal;
    if (t->hwnd != hwnd) return;

    wd_begin(hwnd);

    /* Draw character grid from text-mode buffer */
    for (int row = 0; row < TERM_ROWS; row++) {
        for (int col = 0; col < TERM_COLS; col++) {
            uint8_t ch   = TB_CHAR(t, row, col);
            uint8_t attr = TB_ATTR(t, row, col);
            uint8_t fg   = TB_FG(attr);
            uint8_t bg   = TB_BG(attr);

            int px = col * TERM_FONT_W;
            int py = row * TERM_FONT_H;

            /* Draw using 8x16 font */
            const uint8_t *glyph = font8x16_get_glyph(ch);
            for (int gr = 0; gr < TERM_FONT_H; gr++) {
                uint8_t bits = glyph[gr];
                for (int gc = 0; gc < TERM_FONT_W; gc++) {
                    wd_pixel(px + gc, py + gr,
                             (bits & (1 << gc)) ? fg : bg);
                }
            }
        }
    }

    /* Draw blinking block cursor */
    if (t->cursor_visible &&
        t->cursor_col >= 0 && t->cursor_col < TERM_COLS &&
        t->cursor_row >= 0 && t->cursor_row < TERM_ROWS) {
        int cx = t->cursor_col * TERM_FONT_W;
        int cy = t->cursor_row * TERM_FONT_H;
        for (int r = 0; r < TERM_FONT_H; r++)
            for (int c = 0; c < TERM_FONT_W; c++)
                wd_pixel(cx + c, cy + r, t->fg_color);
    }

    wd_end();
}

/*==========================================================================
 * Event handler — keyboard input
 *=========================================================================*/

static bool terminal_event(hwnd_t hwnd, const window_event_t *event) {
    terminal_t *t = &g_terminal;
    if (t->hwnd != hwnd) return false;

    switch (event->type) {
    case WM_CHAR:
        terminal_input_push(t, (uint8_t)event->charev.ch);
        return true;

    case WM_KEYDOWN:
        switch (event->key.scancode) {
        case 0x28: terminal_input_push(t, '\n');  return true;
        case 0x29: terminal_input_push(t, 0x1B);  return true;
        case 0x2A: terminal_input_push(t, '\b');  return true;
        case 0x2B: terminal_input_push(t, '\t');  return true;
        }
        return false;

    case WM_CLOSE:
        return true;

    default:
        return false;
    }
}

/*==========================================================================
 * Cursor blink timer callback
 *=========================================================================*/

static void blink_callback(TimerHandle_t xTimer) {
    (void)xTimer;
    terminal_t *t = &g_terminal;
    t->cursor_visible = !t->cursor_visible;
    wm_invalidate(t->hwnd);
}

/*==========================================================================
 * Public API
 *=========================================================================*/

hwnd_t terminal_create(void) {
    terminal_t *t = &g_terminal;
    memset(t, 0, sizeof(*t));

    t->fg_color = COLOR_WHITE;
    t->bg_color = COLOR_BLACK;
    t->cursor_visible = true;

    /* Allocate text-mode buffer (large enough for MOS2 op_console compat) */
    t->textbuf_size = SCREEN_BUF_COMPAT_SIZE;
    t->textbuf = (uint8_t *)pvPortMalloc(t->textbuf_size);
    if (t->textbuf) {
        /* Fill with spaces, white-on-black */
        uint8_t attr = TB_PACK(COLOR_WHITE, COLOR_BLACK);
        for (int i = 0; i < TERM_COLS * TERM_ROWS; i++) {
            t->textbuf[i * 2]     = ' ';
            t->textbuf[i * 2 + 1] = attr;
        }
        /* Zero any padding beyond the text area */
        if (t->textbuf_size > TERM_TEXTBUF_SIZE) {
            memset(t->textbuf + TERM_TEXTBUF_SIZE, 0,
                   t->textbuf_size - TERM_TEXTBUF_SIZE);
        }
    }

    /* Create input semaphore */
    t->input_sem = xSemaphoreCreateCounting(64, 0);

    /* Compute outer window size:
     * client = 560 x 320 (70 cols * 8px, 20 rows * 16px)
     * + title bar + borders */
    int16_t client_w = TERM_COLS * TERM_FONT_W;  /* 560 */
    int16_t client_h = TERM_ROWS * TERM_FONT_H;  /* 320 */
    int16_t outer_w = client_w + 2 * THEME_BORDER_WIDTH;
    int16_t outer_h = client_h + THEME_TITLE_HEIGHT + 2 * THEME_BORDER_WIDTH;

    t->hwnd = wm_create_window(
        10, 10, outer_w, outer_h,
        "Terminal",
        WF_CLOSABLE | WF_MOVABLE | WF_BORDER,
        terminal_event,
        terminal_paint
    );

    /* Set black background */
    window_t *win = wm_get_window(t->hwnd);
    if (win) win->bg_color = COLOR_BLACK;

    /* Start cursor blink timer (500ms) */
    t->blink_timer = xTimerCreate("tblink", pdMS_TO_TICKS(500),
                                   pdTRUE, NULL, blink_callback);
    xTimerStart(t->blink_timer, 0);

    g_active_terminal = t;

    return t->hwnd;
}

void terminal_putc(terminal_t *t, char c) {
    if (!t || !t->textbuf) return;

    switch (c) {
    case '\n':
        t->cursor_col = 0;
        t->cursor_row++;
        break;
    case '\r':
        t->cursor_col = 0;
        break;
    case '\b':
        if (t->cursor_col > 0) {
            t->cursor_col--;
            TB_CHAR(t, t->cursor_row, t->cursor_col) = ' ';
            TB_ATTR(t, t->cursor_row, t->cursor_col) =
                TB_PACK(t->fg_color, t->bg_color);
        }
        break;
    case '\t':
        t->cursor_col = (t->cursor_col + 8) & ~7;
        if (t->cursor_col >= TERM_COLS) {
            t->cursor_col = 0;
            t->cursor_row++;
        }
        break;
    default:
        if (t->cursor_col >= TERM_COLS) {
            t->cursor_col = 0;
            t->cursor_row++;
        }
        if (t->cursor_row >= TERM_ROWS) {
            terminal_scroll_up(t);
            t->cursor_row = TERM_ROWS - 1;
        }
        TB_CHAR(t, t->cursor_row, t->cursor_col) = (uint8_t)c;
        TB_ATTR(t, t->cursor_row, t->cursor_col) =
            TB_PACK(t->fg_color, t->bg_color);
        t->cursor_col++;
        break;
    }

    /* Handle scroll after newline/tab */
    if (t->cursor_row >= TERM_ROWS) {
        terminal_scroll_up(t);
        t->cursor_row = TERM_ROWS - 1;
    }

    wm_invalidate(t->hwnd);
}

void terminal_puts(terminal_t *t, const char *s) {
    if (!t || !s) return;
    while (*s) terminal_putc(t, *s++);
}

void terminal_printf(terminal_t *t, const char *fmt, ...) {
    if (!t) return;
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    terminal_puts(t, buf);
}

void terminal_clear(terminal_t *t, uint8_t color) {
    if (!t || !t->textbuf) return;
    t->bg_color = color;
    uint8_t attr = TB_PACK(t->fg_color, color);
    for (int i = 0; i < TERM_COLS * TERM_ROWS; i++) {
        t->textbuf[i * 2]     = ' ';
        t->textbuf[i * 2 + 1] = attr;
    }
    t->cursor_col = 0;
    t->cursor_row = 0;
    wm_invalidate(t->hwnd);
}

void terminal_set_cursor(terminal_t *t, int col, int row) {
    if (!t) return;
    if (col >= 0 && col < TERM_COLS) t->cursor_col = col;
    if (row >= 0 && row < TERM_ROWS) t->cursor_row = row;
}

void terminal_set_color(terminal_t *t, uint8_t fg, uint8_t bg) {
    if (!t) return;
    t->fg_color = fg & 0x0F;
    t->bg_color = bg & 0x0F;
}

void terminal_draw_text(terminal_t *t, const char *str, int col, int row,
                        uint8_t fg, uint8_t bg) {
    if (!t || !t->textbuf || !str) return;
    if (row < 0 || row >= TERM_ROWS) return;
    uint8_t attr = TB_PACK(fg & 0x0F, bg & 0x0F);
    for (int i = 0; str[i] != '\0'; i++) {
        int c = col + i;
        if (c < 0) continue;
        if (c >= TERM_COLS) break;
        TB_CHAR(t, row, c) = (uint8_t)str[i];
        TB_ATTR(t, row, c) = attr;
    }
    wm_invalidate(t->hwnd);
}

int terminal_get_cursor_col(terminal_t *t) {
    return t ? t->cursor_col : 0;
}

int terminal_get_cursor_row(terminal_t *t) {
    return t ? t->cursor_row : 0;
}

int terminal_getch(terminal_t *t) {
    if (!t) return -1;
    xSemaphoreTake(t->input_sem, portMAX_DELAY);
    uint8_t ch = t->input_buf[t->in_tail];
    t->in_tail = (t->in_tail + 1) & 63;
    return ch;
}

int terminal_getch_now(terminal_t *t) {
    if (!t) return -1;
    if (xSemaphoreTake(t->input_sem, 0) != pdTRUE) return -1;
    uint8_t ch = t->input_buf[t->in_tail];
    t->in_tail = (t->in_tail + 1) & 63;
    return ch;
}

terminal_t *terminal_get_active(void) {
    return g_active_terminal;
}

uint8_t *terminal_get_textbuf(terminal_t *t) {
    return t ? t->textbuf : NULL;
}

size_t terminal_get_textbuf_size(terminal_t *t) {
    return t ? t->textbuf_size : 0;
}

void terminal_invalidate_active(void) {
    terminal_t *t = terminal_get_active();
    if (t) wm_invalidate(t->hwnd);
}
