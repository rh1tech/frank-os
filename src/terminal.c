/*
 * FRANK OS
 * Copyright (c) 2025 Mikhail Matveev <xtreme@rh1.tech>
 * https://rh1.tech
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "terminal.h"
#include "window.h"
#include "window_event.h"
#include "window_draw.h"
#include "window_theme.h"
#include "menu.h"
#include "font.h"
#include "display.h"
#include "dialog.h"
#include "FreeRTOS.h"
#include "semphr.h"
#include "timers.h"
#include "task.h"
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include "psram.h"
#include "pico/platform.h"

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

/* Only allocate the actual text content (70*20*2 = 2800 bytes).
 * op_console is clamped to get_buffer_size() so no overrun occurs. */

/* Buffer access macros */
#define TB_OFF(row, col)   ((row) * TERM_COLS * 2 + (col) * 2)
#define TB_CHAR(t, r, c)   ((t)->textbuf[TB_OFF(r, c)])
#define TB_ATTR(t, r, c)   ((t)->textbuf[TB_OFF(r, c) + 1])
#define TB_PACK(fg, bg)    (((bg) << 4) | ((fg) & 0x0F))
#define TB_FG(attr)        ((attr) & 0x0F)
#define TB_BG(attr)        ((attr) >> 4)

/*==========================================================================
 * Helpers
 *=========================================================================*/

static void __not_in_flash_func(terminal_scroll_up)(terminal_t *t) {
    /* Move rows 1..TERM_ROWS-1 up to rows 0..TERM_ROWS-2.
     * Manual loop instead of memmove() because memmove is in flash and
     * calling it on a PSRAM buffer causes CS0 (flash instruction fetch) +
     * CS1 (PSRAM data) QMI bus contention that hangs the system. */
    volatile uint8_t *dst = t->textbuf;
    volatile uint8_t *src = t->textbuf + TERM_COLS * 2;
    int n = TERM_COLS * 2 * (TERM_ROWS - 1);
    for (int i = 0; i < n; i++) dst[i] = src[i];
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
 * Terminal ↔ Window association via user_data pointer
 *=========================================================================*/

terminal_t *terminal_from_hwnd(hwnd_t hwnd) {
    window_t *win = wm_get_window(hwnd);
    return win ? (terminal_t *)win->user_data : NULL;
}

hwnd_t terminal_get_hwnd(terminal_t *t) {
    return t ? t->hwnd : HWND_NULL;
}

/*==========================================================================
 * Per-task terminal via FreeRTOS TLS
 *=========================================================================*/

void terminal_set_task_terminal(terminal_t *t) {
    vTaskSetThreadLocalStoragePointer(xTaskGetCurrentTaskHandle(),
                                     TERMINAL_TLS_SLOT, t);
}

terminal_t *terminal_get_task_terminal(void) {
    if (xTaskGetSchedulerState() != taskSCHEDULER_RUNNING)
        return NULL;
    return (terminal_t *)pvTaskGetThreadLocalStoragePointer(
        xTaskGetCurrentTaskHandle(), TERMINAL_TLS_SLOT);
}

/*==========================================================================
 * Paint handler — draws the character grid using 8x16 font
 *
 * When the textbuf lives in PSRAM, thousands of individual byte reads
 * from the uncached XIP window (0x15000000) interleave with flash
 * instruction fetches through the shared QMI bus, which can cause
 * bus hangs.  We snapshot the textbuf into a SRAM shadow buffer once
 * via memcpy, then render entirely from SRAM.
 *=========================================================================*/

static uint8_t paint_shadow[TERM_TEXTBUF_SIZE];

static void __not_in_flash_func(terminal_paint)(hwnd_t hwnd) {
    terminal_t *t = terminal_from_hwnd(hwnd);
    if (!t || !t->textbuf) return;

    /* Snapshot textbuf into SRAM — one bulk copy instead of thousands
     * of individual PSRAM reads during the rendering loop.
     * Manual loop instead of memcpy() because memcpy is in flash and
     * calling it on a PSRAM source causes QMI bus contention (CS0
     * instruction fetch + CS1 data read simultaneously → bus hang).
     * volatile source prevents the compiler from converting this back
     * into a memcpy call. */
    {
        volatile uint8_t *src = t->textbuf;
        for (int i = 0; i < TERM_TEXTBUF_SIZE; i++)
            paint_shadow[i] = src[i];
    }

    /* Compute client-area origin in screen coordinates directly,
     * bypassing wd_begin/wd_end to avoid per-pixel clipping overhead. */
    window_t *win = wm_get_window(hwnd);
    if (!win) return;
    int ox, oy;
    if (win->flags & WF_BORDER) {
        point_t origin = theme_client_origin(&win->frame, win->flags);
        ox = origin.x;
        oy = origin.y;
    } else {
        ox = win->frame.x;
        oy = win->frame.y;
    }

    /* Draw character grid using fast glyph blitter */
    for (int row = 0; row < TERM_ROWS; row++) {
        int sy = oy + row * TERM_FONT_H;
        if (sy + TERM_FONT_H <= 0 || sy >= FB_HEIGHT) continue;

        for (int col = 0; col < TERM_COLS; col++) {
            int sx = ox + col * TERM_FONT_W;
            if (sx + TERM_FONT_W <= 0 || sx >= DISPLAY_WIDTH) continue;

            int off = (row * TERM_COLS + col) * 2;
            uint8_t ch   = paint_shadow[off];
            uint8_t attr = paint_shadow[off + 1];
            uint8_t fg   = TB_FG(attr);
            uint8_t bg   = TB_BG(attr);

            const uint8_t *glyph = font8x16_get_glyph(ch);

            /* Fast path: even x and fully on-screen */
            if (!(sx & 1) &&
                sx >= 0 && (sx + TERM_FONT_W) <= DISPLAY_WIDTH &&
                sy >= 0 && (sy + TERM_FONT_H) <= FB_HEIGHT) {
                display_blit_glyph_8wide(sx, sy, glyph, TERM_FONT_H, fg, bg);
            } else {
                /* Fallback: per-pixel for partially clipped chars */
                for (int gr = 0; gr < TERM_FONT_H; gr++) {
                    int py = sy + gr;
                    if ((unsigned)py >= (unsigned)FB_HEIGHT) continue;
                    uint8_t bits = glyph[gr];
                    for (int gc = 0; gc < TERM_FONT_W; gc++) {
                        int px = sx + gc;
                        if ((unsigned)px >= (unsigned)DISPLAY_WIDTH) continue;
                        display_set_pixel_fast(px, py,
                                               (bits & (1 << gc)) ? fg : bg);
                    }
                }
            }
        }
    }

    /* Draw blinking DOS-style underline cursor (bottom 2 scanlines) */
    if (t->cursor_visible &&
        t->cursor_col >= 0 && t->cursor_col < TERM_COLS &&
        t->cursor_row >= 0 && t->cursor_row < TERM_ROWS) {
        int cx = ox + t->cursor_col * TERM_FONT_W;
        int cy = oy + t->cursor_row * TERM_FONT_H;
        display_hline_safe(cx, cy + TERM_FONT_H - 2, TERM_FONT_W, t->fg_color);
        display_hline_safe(cx, cy + TERM_FONT_H - 1, TERM_FONT_W, t->fg_color);
    }
}

/*==========================================================================
 * Event handler — keyboard input
 *=========================================================================*/

/* Terminal menu command IDs */
#define TCMD_FILE_EXIT    1
#define TCMD_HELP_ABOUT 100

/* Force-close: signal shell, destroy window immediately */
static void terminal_force_close(terminal_t *t, hwnd_t hwnd) {
    t->closing = true;
    if (t->input_sem) xSemaphoreGive(t->input_sem);
    if (t->blink_timer) {
        xTimerStop(t->blink_timer, 0);
        xTimerDelete(t->blink_timer, 0);
        t->blink_timer = NULL;
    }
    {
        window_t *w = wm_get_window(hwnd);
        if (w) w->user_data = NULL;
    }
    wm_destroy_window(hwnd);
    t->hwnd = HWND_NULL;
}

static bool terminal_event(hwnd_t hwnd, const window_event_t *event) {
    terminal_t *t = terminal_from_hwnd(hwnd);
    if (!t) return false;

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

    case WM_COMMAND:
        switch (event->command.id) {
        case TCMD_FILE_EXIT:
            terminal_force_close(t, hwnd);
            return true;
        case TCMD_HELP_ABOUT:
            dialog_show(hwnd, "About",
                        "FRANK OS\n\nVersion " FRANK_VERSION_STR "\n"
                        "Copyright (c) 2026 Mikhail Matveev\n"
                        "rh1.tech",
                        DLG_ICON_INFO, DLG_BTN_OK);
            return true;
        }
        return false;

    case WM_CLOSE:
        terminal_force_close(t, hwnd);
        return true;

    default:
        return false;
    }
}

/*==========================================================================
 * Cursor blink timer callback
 *=========================================================================*/

static void blink_callback(TimerHandle_t xTimer) {
    terminal_t *t = (terminal_t *)pvTimerGetTimerID(xTimer);
    if (!t) return;
    t->cursor_visible = !t->cursor_visible;
    wm_invalidate(t->hwnd);
}

/*==========================================================================
 * Stdin waiter notification (per-terminal)
 *=========================================================================*/

void terminal_notify_stdin_ready(terminal_t *t) {
    if (!t) return;
    for (int i = 0; i < t->mos2_num_stdin_waiters; i++) {
        if (t->mos2_stdin_waiters[i]) {
            xTaskNotifyGive(t->mos2_stdin_waiters[i]);
        }
    }
}

/*==========================================================================
 * Public API
 *=========================================================================*/

hwnd_t terminal_create(void) {
    terminal_t *t = (terminal_t *)pvPortMalloc(sizeof(terminal_t));
    if (!t) return HWND_NULL;
    memset(t, 0, sizeof(*t));

    t->fg_color = COLOR_WHITE;
    t->bg_color = COLOR_BLACK;
    t->cursor_visible = true;

    /* Allocate text-mode buffer.
     * TODO: PSRAM textbufs cause QMI bus hangs when ISRs fire during
     * uncached PSRAM access (write buffer drain + flash fetch contention).
     * Force SRAM until the QMI interleaving issue is resolved. */
    t->textbuf_size = TERM_TEXTBUF_SIZE;
#if 0  /* PSRAM disabled — causes bus hang, see above */
    if (psram_is_available())
        t->textbuf = (uint8_t *)psram_alloc(t->textbuf_size);
#endif
    if (!t->textbuf)
        t->textbuf = (uint8_t *)pvPortMalloc(t->textbuf_size);
    if (!t->textbuf) {
        vPortFree(t);
        return HWND_NULL;
    }
    printf("[terminal_create] textbuf=%p size=%u\n",
           t->textbuf, (unsigned)t->textbuf_size);
    /* Fill with spaces, white-on-black */
    uint8_t attr = TB_PACK(COLOR_WHITE, COLOR_BLACK);
    for (int i = 0; i < TERM_COLS * TERM_ROWS; i++) {
        t->textbuf[i * 2]     = ' ';
        t->textbuf[i * 2 + 1] = attr;
    }

    /* Create input semaphore */
    t->input_sem = xSemaphoreCreateCounting(64, 0);

    /* Compute outer window size:
     * client = 560 x 320 (70 cols * 8px, 20 rows * 16px)
     * + title bar + menu bar + borders */
    int16_t client_w = TERM_COLS * TERM_FONT_W;  /* 560 */
    int16_t client_h = TERM_ROWS * TERM_FONT_H;  /* 320 */
    int16_t outer_w = client_w + 2 * THEME_BORDER_WIDTH;
    int16_t outer_h = client_h + THEME_TITLE_HEIGHT + THEME_MENU_HEIGHT +
                      2 * THEME_BORDER_WIDTH;

    t->hwnd = wm_create_window(
        10, 10, outer_w, outer_h,
        "Terminal",
        WF_CLOSABLE | WF_MOVABLE | WF_BORDER | WF_MENUBAR,
        terminal_event,
        terminal_paint
    );

    if (t->hwnd == HWND_NULL) {
        vSemaphoreDelete(t->input_sem);
        vPortFree(t->textbuf);
        vPortFree(t);
        return HWND_NULL;
    }

    /* Set black background and store terminal pointer in user_data */
    window_t *win = wm_get_window(t->hwnd);
    if (win) {
        win->bg_color = COLOR_BLACK;
        win->user_data = t;
    }

    /* Attach menu bar: File (Exit), Help (About) */
    {
        menu_bar_t bar;
        memset(&bar, 0, sizeof(bar));
        bar.menu_count = 2;

        /* File menu — Alt+F (HID 'F' = 0x09) */
        menu_def_t *file = &bar.menus[0];
        strncpy(file->title, "File", sizeof(file->title) - 1);
        file->accel_key = 0x09;  /* HID_KEY_F */
        file->item_count = 1;
        strncpy(file->items[0].text, "Exit", sizeof(file->items[0].text) - 1);
        file->items[0].command_id = TCMD_FILE_EXIT;

        /* Help menu — Alt+H (HID 'H' = 0x0B) */
        menu_def_t *help = &bar.menus[1];
        strncpy(help->title, "Help", sizeof(help->title) - 1);
        help->accel_key = 0x0B;  /* HID_KEY_H */
        help->item_count = 1;
        strncpy(help->items[0].text, "About", sizeof(help->items[0].text) - 1);
        help->items[0].command_id = TCMD_HELP_ABOUT;

        menu_set(t->hwnd, &bar);
    }

    /* Start cursor blink timer (500ms) — pass terminal_t* as timer ID */
    t->blink_timer = xTimerCreate("tblink", pdMS_TO_TICKS(500),
                                   pdTRUE, (void *)t, blink_callback);
    xTimerStart(t->blink_timer, 0);

    return t->hwnd;
}

void terminal_destroy(terminal_t *t) {
    if (!t) return;

    /* Stop blink timer */
    if (t->blink_timer) {
        xTimerStop(t->blink_timer, portMAX_DELAY);
        xTimerDelete(t->blink_timer, portMAX_DELAY);
        t->blink_timer = NULL;
    }

    /* Clear user_data before destroying window */
    window_t *win = wm_get_window(t->hwnd);
    if (win) win->user_data = NULL;

    /* Destroy the window */
    if (t->hwnd != HWND_NULL) {
        wm_destroy_window(t->hwnd);
        t->hwnd = HWND_NULL;
    }

    /* Delete input semaphore */
    if (t->input_sem) {
        vSemaphoreDelete(t->input_sem);
        t->input_sem = NULL;
    }

    /* Free text buffer (psram_free handles both PSRAM and SRAM pointers) */
    if (t->textbuf) {
        psram_free(t->textbuf);
        t->textbuf = NULL;
    }

    /* Free the terminal struct itself */
    vPortFree(t);
}

void __not_in_flash_func(terminal_putc)(terminal_t *t, char c) {
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

void __not_in_flash_func(terminal_clear)(terminal_t *t, uint8_t color) {
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

/* Run from SRAM to avoid QMI bus contention between PSRAM data writes
 * (CS1) and flash instruction fetches (CS0) through the shared QMI. */
void __not_in_flash_func(terminal_draw_text)(terminal_t *t, const char *str,
                        int col, int row, uint8_t fg, uint8_t bg) {
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
    if (t->closing) return -1;
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
    /* 1. Try TLS slot for current task */
    terminal_t *t = terminal_get_task_terminal();
    if (t) return t;

    /* 2. Fall back to focused window's terminal */
    hwnd_t focus = wm_get_focus();
    if (focus != HWND_NULL) {
        t = terminal_from_hwnd(focus);
        if (t) return t;
    }

    return NULL;
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
