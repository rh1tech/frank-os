/*
 * pshell_vt100.c — VT100 terminal emulator for pshell on FRANK OS
 *
 * State machine processes output characters and translates VT100 escape
 * sequences into terminal buffer operations.  The buffer is rendered to
 * a FRANK OS window framebuffer using wd_fb_ptr() with an 8×16 font.
 *
 * Copyright (c) 2026 Mikhail Matveev <xtreme@rh1.tech>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "m-os-api.h"
#include "frankos-app.h"
#include "pshell_vt100.h"
#include "font.h"

#include <string.h>
#include <stdarg.h>

/* ═════════════════════════════════════════════════════════════════════════
 * ANSI → CGA/EGA color mapping
 * ═════════════════════════════════════════════════════════════════════════ */

/* ANSI color index (0-7) → CGA color index (used in attr byte) */
static const uint8_t ansi_to_cga[8] = {
    COLOR_BLACK,        /* 0 */
    COLOR_RED,          /* 1 */
    COLOR_GREEN,        /* 2 */
    COLOR_BROWN,        /* 3  (ANSI yellow → CGA brown at non-bright) */
    COLOR_BLUE,         /* 4 */
    COLOR_MAGENTA,      /* 5 */
    COLOR_CYAN,         /* 6 */
    COLOR_LIGHT_GRAY    /* 7 */
};

/* ═════════════════════════════════════════════════════════════════════════
 * Terminal state
 * ═════════════════════════════════════════════════════════════════════════ */

/* VT100 parser states */
enum { ST_NORMAL, ST_ESC_START, ST_CSI_PARAM, ST_CSI_INTER };

/* CSI parameter accumulation */
#define MAX_CSI_PARAMS  8

/* Text buffer: 2 bytes per cell [char][attr], row-major */
static uint8_t *textbuf;
static int      tb_cols, tb_rows;
static int      cursor_col, cursor_row;
static uint8_t  cur_fg, cur_bg;
static bool     bold_on, reverse_on;

/* VT100 parser state */
static int      vt_state;
static int      csi_params[MAX_CSI_PARAMS];
static int      csi_nparam;
static int      csi_cur_param;
static bool     csi_private;   /* '?' prefix in CSI sequence (DEC private) */

/* Cursor blink state */
static volatile bool cursor_visible;
static volatile bool cursor_enabled;  /* DEC private mode ?25h/?25l */

/* Active (has focus) flag — when false, flush_display() skips direct
 * framebuffer writes so pshell doesn't paint over other windows. */
static volatile bool vt_active = true;

/* Window handle for invalidation */
static hwnd_t   g_vt_hwnd;

/* Dirty flag — batches wm_invalidate() calls */
static volatile bool vt_dirty;

/* Shadow buffer for dirty-cell paint optimisation */
static uint8_t *shadow_buf;

/* Cached framebuffer info — set by vt100_paint() (compositor task),
 * used by flush_display()/toggle_cursor() for direct writes from
 * the shell/timer tasks without going through wm_invalidate(). */
static uint8_t *cached_fb;
static int16_t  cached_stride;
static int16_t  cached_clip_w;
static int16_t  cached_clip_h;

/* ── Input ring buffer ─────────────────────────────────────────────────── */
static volatile uint8_t inbuf[VT100_INBUF_SIZE];
static volatile int     in_head, in_tail;
static TaskHandle_t     in_waiter;  /* task waiting for input (notified on push) */

/* ═════════════════════════════════════════════════════════════════════════
 * Helpers
 * ═════════════════════════════════════════════════════════════════════════ */

/* Offset into textbuf for (row, col) */
#define TB_OFF(r, c)  (((r) * tb_cols + (c)) * 2)

static inline uint8_t make_attr(void) {
    uint8_t fg = cur_fg;
    uint8_t bg = cur_bg;
    if (bold_on) fg |= 8;
    if (reverse_on) { uint8_t t = fg; fg = bg; bg = t; }
    return (bg << 4) | (fg & 0x0F);
}

static void tb_put_char(int row, int col, uint8_t ch, uint8_t attr) {
    if (row < 0 || row >= tb_rows || col < 0 || col >= tb_cols) return;
    int off = TB_OFF(row, col);
    textbuf[off]     = ch;
    textbuf[off + 1] = attr;
}

static void scroll_up(void) {
    /* Move rows 1..last to 0..last-1 */
    int row_bytes = tb_cols * 2;
    memmove(textbuf, textbuf + row_bytes, (tb_rows - 1) * row_bytes);
    /* Clear last row */
    uint8_t attr = make_attr();
    int last_off = (tb_rows - 1) * row_bytes;
    for (int c = 0; c < tb_cols; c++) {
        textbuf[last_off + c * 2]     = ' ';
        textbuf[last_off + c * 2 + 1] = attr;
    }
}

static void invalidate(void) {
    vt_dirty = true;
}

/* Render a single cell directly to framebuffer.
 * fb = base pointer from wd_fb_ptr(0,0), stride = row pitch in bytes.
 * Uses 2-bit LUT for branch-free glyph rendering (ZX Spectrum technique). */
static inline void render_cell(uint8_t *fb, int16_t stride,
                                int row, int col,
                                uint8_t ch, uint8_t eff_attr) {
    uint8_t fg = eff_attr & 0x0Fu;
    uint8_t bg = (eff_attr >> 4) & 0x0Fu;
    const uint8_t *glyph = &font_8x16[(uint8_t)ch * VT100_FONT_H];
    uint8_t lut[4] = {
        (uint8_t)((bg << 4) | bg),   /* 00: bg bg */
        (uint8_t)((fg << 4) | bg),   /* 01: fg bg */
        (uint8_t)((bg << 4) | fg),   /* 10: bg fg */
        (uint8_t)((fg << 4) | fg)    /* 11: fg fg */
    };
    int col_off = (col * VT100_FONT_W) / 2;
    for (int gy = 0; gy < VT100_FONT_H; gy++) {
        uint8_t bits = glyph[gy];
        uint8_t *drow = fb + (row * VT100_FONT_H + gy) * stride + col_off;
        drow[0] = lut[(bits >> 0) & 3];
        drow[1] = lut[(bits >> 2) & 3];
        drow[2] = lut[(bits >> 4) & 3];
        drow[3] = lut[(bits >> 6) & 3];
    }
}

/* Flush pending changes — render dirty cells directly to cached framebuffer.
 * Falls back to wm_invalidate() if no cached pointer is available yet.
 * Called from getch (natural end-of-output-burst) and cursor blink timer. */
static void flush_display(void) {
    if (!vt_dirty || g_vt_hwnd == 0) return;

    /* When inactive (no focus), keep vt_dirty set but don't write to
     * the framebuffer — the next vt100_paint() or refocus will pick
     * up the pending changes. */
    if (!vt_active) return;

    vt_dirty = false;

    uint8_t *fb = cached_fb;
    if (!fb || !textbuf) {
        /* No cached framebuffer yet — fall back to WM round-trip */
        wm_invalidate(g_vt_hwnd);
        return;
    }

    int16_t stride = cached_stride;
    int vis_cols = cached_clip_w / VT100_FONT_W;
    int vis_rows = cached_clip_h / VT100_FONT_H;
    if (vis_cols > tb_cols) vis_cols = tb_cols;
    if (vis_rows > tb_rows) vis_rows = tb_rows;

    for (int row = 0; row < vis_rows; row++) {
        for (int col = 0; col < vis_cols; col++) {
            int off = TB_OFF(row, col);
            uint8_t ch   = textbuf[off];
            uint8_t attr = textbuf[off + 1];

            /* Apply cursor inversion */
            bool is_cursor = (row == cursor_row && col == cursor_col
                              && cursor_visible && cursor_enabled);
            uint8_t eff_attr = is_cursor ? (uint8_t)((attr >> 4) | (attr << 4))
                                         : attr;

            /* Skip unchanged cells */
            if (shadow_buf[off] == ch && shadow_buf[off + 1] == eff_attr)
                continue;
            shadow_buf[off]     = ch;
            shadow_buf[off + 1] = eff_attr;

            render_cell(fb, stride, row, col, ch, eff_attr);
        }
    }
}

/* ═════════════════════════════════════════════════════════════════════════
 * CSI command execution
 * ═════════════════════════════════════════════════════════════════════════ */

static void csi_execute(char cmd) {
    int p0 = (csi_nparam > 0) ? csi_params[0] : 0;
    int p1 = (csi_nparam > 1) ? csi_params[1] : 0;
    uint8_t attr;
    int off;

    if (cmd == 'H' || cmd == 'f') {
        /* CUP — cursor position (1-based params, default 1;1) */
        int row = (p0 > 0) ? p0 - 1 : 0;
        int col = (p1 > 0) ? p1 - 1 : 0;
        if (row >= tb_rows) row = tb_rows - 1;
        if (col >= tb_cols) col = tb_cols - 1;
        cursor_row = row;
        cursor_col = col;
    }
    else if (cmd == 'A') {
        /* CUU — cursor up */
        int n = (p0 > 0) ? p0 : 1;
        cursor_row -= n;
        if (cursor_row < 0) cursor_row = 0;
    }
    else if (cmd == 'B') {
        /* CUD — cursor down */
        int n = (p0 > 0) ? p0 : 1;
        cursor_row += n;
        if (cursor_row >= tb_rows) cursor_row = tb_rows - 1;
    }
    else if (cmd == 'C') {
        /* CUF — cursor forward */
        int n = (p0 > 0) ? p0 : 1;
        cursor_col += n;
        if (cursor_col >= tb_cols) cursor_col = tb_cols - 1;
    }
    else if (cmd == 'D') {
        /* CUB — cursor back */
        int n = (p0 > 0) ? p0 : 1;
        cursor_col -= n;
        if (cursor_col < 0) cursor_col = 0;
    }
    else if (cmd == 'J') {
        /* ED — erase in display */
        attr = make_attr();
        if (p0 == 0) {
            /* Clear from cursor to end of screen */
            for (int c = cursor_col; c < tb_cols; c++)
                tb_put_char(cursor_row, c, ' ', attr);
            for (int r = cursor_row + 1; r < tb_rows; r++)
                for (int c = 0; c < tb_cols; c++)
                    tb_put_char(r, c, ' ', attr);
        } else if (p0 == 1) {
            /* Clear from start to cursor */
            for (int r = 0; r < cursor_row; r++)
                for (int c = 0; c < tb_cols; c++)
                    tb_put_char(r, c, ' ', attr);
            for (int c = 0; c <= cursor_col; c++)
                tb_put_char(cursor_row, c, ' ', attr);
        } else if (p0 == 2) {
            /* Clear entire screen */
            for (int r = 0; r < tb_rows; r++)
                for (int c = 0; c < tb_cols; c++)
                    tb_put_char(r, c, ' ', attr);
        }
    }
    else if (cmd == 'K') {
        /* EL — erase in line */
        attr = make_attr();
        if (p0 == 0) {
            /* Clear from cursor to end of line */
            for (int c = cursor_col; c < tb_cols; c++)
                tb_put_char(cursor_row, c, ' ', attr);
        } else if (p0 == 1) {
            /* Clear from start of line to cursor */
            for (int c = 0; c <= cursor_col; c++)
                tb_put_char(cursor_row, c, ' ', attr);
        } else if (p0 == 2) {
            /* Clear entire line */
            for (int c = 0; c < tb_cols; c++)
                tb_put_char(cursor_row, c, ' ', attr);
        }
    }
    else if (cmd == 'm') {
        /* SGR — select graphic rendition */
        if (csi_nparam == 0) {
            /* ESC[m = reset */
            cur_fg = COLOR_LIGHT_GRAY;
            cur_bg = COLOR_BLACK;
            bold_on = false;
            reverse_on = false;
            return;
        }
        for (int i = 0; i < csi_nparam; i++) {
            int p = csi_params[i];
            if (p == 0) {
                cur_fg = COLOR_LIGHT_GRAY;
                cur_bg = COLOR_BLACK;
                bold_on = false;
                reverse_on = false;
            } else if (p == 1) {
                bold_on = true;
            } else if (p == 5) {
                /* Blink → map to bright background (or ignore) */
            } else if (p == 7) {
                reverse_on = true;
            } else if (p == 22) {
                bold_on = false;
            } else if (p == 27) {
                reverse_on = false;
            } else if (p >= 30 && p <= 37) {
                cur_fg = ansi_to_cga[p - 30];
            } else if (p >= 40 && p <= 47) {
                cur_bg = ansi_to_cga[p - 40];
            } else if (p >= 90 && p <= 97) {
                /* Bright foreground */
                cur_fg = ansi_to_cga[p - 90] | 8;
            } else if (p >= 100 && p <= 107) {
                /* Bright background */
                cur_bg = ansi_to_cga[p - 100] | 8;
            }
        }
    }
    else if (cmd == 'n') {
        /* DSR — device status report */
        if (p0 == 6) {
            /* Cursor position report: push ESC[row;colR into input */
            char resp[24];
            int len = snprintf(resp, sizeof(resp), "\033[%d;%dR",
                               cursor_row + 1, cursor_col + 1);
            for (int i = 0; i < len; i++)
                vt100_input_push(resp[i]);
        }
    }
    else if (cmd == 'L') {
        /* IL — insert line(s) */
        int n = (p0 > 0) ? p0 : 1;
        if (cursor_row + n < tb_rows) {
            int row_bytes = tb_cols * 2;
            memmove(textbuf + (cursor_row + n) * row_bytes,
                    textbuf + cursor_row * row_bytes,
                    (tb_rows - cursor_row - n) * row_bytes);
        }
        attr = make_attr();
        for (int i = 0; i < n && cursor_row + i < tb_rows; i++)
            for (int c = 0; c < tb_cols; c++)
                tb_put_char(cursor_row + i, c, ' ', attr);
    }
    else if (cmd == 'M') {
        /* DL — delete line(s) */
        int n = (p0 > 0) ? p0 : 1;
        if (cursor_row + n < tb_rows) {
            int row_bytes = tb_cols * 2;
            memmove(textbuf + cursor_row * row_bytes,
                    textbuf + (cursor_row + n) * row_bytes,
                    (tb_rows - cursor_row - n) * row_bytes);
        }
        attr = make_attr();
        for (int r = tb_rows - n; r < tb_rows; r++)
            for (int c = 0; c < tb_cols; c++)
                tb_put_char(r, c, ' ', attr);
    }
    else if (cmd == 'P') {
        /* DCH — delete character(s) */
        int n = (p0 > 0) ? p0 : 1;
        off = TB_OFF(cursor_row, 0);
        if (cursor_col + n < tb_cols) {
            memmove(textbuf + off + cursor_col * 2,
                    textbuf + off + (cursor_col + n) * 2,
                    (tb_cols - cursor_col - n) * 2);
        }
        attr = make_attr();
        for (int c = tb_cols - n; c < tb_cols; c++)
            tb_put_char(cursor_row, c, ' ', attr);
    }
    else if (cmd == '@') {
        /* ICH — insert character(s) */
        int n = (p0 > 0) ? p0 : 1;
        off = TB_OFF(cursor_row, 0);
        if (cursor_col + n < tb_cols) {
            memmove(textbuf + off + (cursor_col + n) * 2,
                    textbuf + off + cursor_col * 2,
                    (tb_cols - cursor_col - n) * 2);
        }
        attr = make_attr();
        for (int i = 0; i < n && cursor_col + i < tb_cols; i++)
            tb_put_char(cursor_row, cursor_col + i, ' ', attr);
    }
    else if (cmd == 'h' && csi_private) {
        /* DECSET — DEC private mode set */
        if (p0 == 25) cursor_enabled = true;   /* ?25h = show cursor */
    }
    else if (cmd == 'l' && csi_private) {
        /* DECRST — DEC private mode reset */
        if (p0 == 25) cursor_enabled = false;  /* ?25l = hide cursor */
    }
}

/* ═════════════════════════════════════════════════════════════════════════
 * VT100 state machine — processes one character at a time
 * ═════════════════════════════════════════════════════════════════════════ */

void vt100_putc(char c) {
    uint8_t uc = (uint8_t)c;

    if (vt_state == ST_NORMAL) {
        if (uc == '\033') {
            vt_state = ST_ESC_START;
        } else if (uc == '\n') {
            cursor_col = 0;
            cursor_row++;
            if (cursor_row >= tb_rows) {
                scroll_up();
                cursor_row = tb_rows - 1;
            }
            invalidate();
        } else if (uc == '\r') {
            cursor_col = 0;
        } else if (uc == '\b') {
            if (cursor_col > 0)
                cursor_col--;
        } else if (uc == '\t') {
            int next = (cursor_col + 8) & ~7;
            if (next > tb_cols) next = tb_cols;
            while (cursor_col < next) {
                tb_put_char(cursor_row, cursor_col, ' ', make_attr());
                cursor_col++;
            }
            if (cursor_col >= tb_cols) cursor_col = tb_cols - 1;
            invalidate();
        } else if (uc == '\007') {
            /* BEL — ignore */
        } else if (uc >= 0x20) {
            /* Printable character */
            if (cursor_col >= tb_cols) {
                /* Wrap to next line */
                cursor_col = 0;
                cursor_row++;
                if (cursor_row >= tb_rows) {
                    scroll_up();
                    cursor_row = tb_rows - 1;
                }
            }
            tb_put_char(cursor_row, cursor_col, uc, make_attr());
            cursor_col++;
            invalidate();
        }
    }
    else if (vt_state == ST_ESC_START) {
        if (uc == '[') {
            /* CSI introducer */
            vt_state = ST_CSI_PARAM;
            csi_nparam = 0;
            csi_cur_param = 0;
            csi_private = false;
            memset(csi_params, 0, sizeof(csi_params));
        } else {
            /* Unknown ESC sequence — ignore and return to normal */
            vt_state = ST_NORMAL;
        }
    }
    else if (vt_state == ST_CSI_PARAM) {
        if (uc >= '0' && uc <= '9') {
            csi_cur_param = csi_cur_param * 10 + (uc - '0');
        } else if (uc == ';') {
            if (csi_nparam < MAX_CSI_PARAMS)
                csi_params[csi_nparam++] = csi_cur_param;
            csi_cur_param = 0;
        } else if (uc == '?') {
            /* DEC private mode prefix */
            csi_private = true;
        } else if (uc >= 0x20 && uc <= 0x2F) {
            /* Intermediate bytes — transition to CSI_INTER */
            vt_state = ST_CSI_INTER;
        } else if ((uc >= 'A' && uc <= 'Z') || (uc >= 'a' && uc <= 'z') || uc == '@') {
            /* Final byte — push last param and execute */
            if (csi_nparam < MAX_CSI_PARAMS)
                csi_params[csi_nparam++] = csi_cur_param;
            csi_execute((char)uc);
            vt_state = ST_NORMAL;
            invalidate();
        } else {
            vt_state = ST_NORMAL;
        }
    }
    else if (vt_state == ST_CSI_INTER) {
        /* Consuming intermediate bytes; wait for final byte */
        if ((uc >= 'A' && uc <= 'Z') || (uc >= 'a' && uc <= 'z') || uc == '@') {
            /* Push last param and execute (though most intermediates are ignored) */
            if (csi_nparam < MAX_CSI_PARAMS)
                csi_params[csi_nparam++] = csi_cur_param;
            csi_execute((char)uc);
            vt_state = ST_NORMAL;
            invalidate();
        }
        /* Otherwise keep consuming */
    }
}

void vt100_puts_nl(const char *s) {
    while (*s)
        vt100_putc(*s++);
    vt100_putc('\n');
}

int vt100_printf(const char *fmt, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    for (int i = 0; i < n && buf[i]; i++)
        vt100_putc(buf[i]);
    return n;
}

/* ═════════════════════════════════════════════════════════════════════════
 * Input ring buffer
 * ═════════════════════════════════════════════════════════════════════════ */

void vt100_input_push(int c) {
    int next = (in_head + 1) % VT100_INBUF_SIZE;
    if (next != in_tail) {
        inbuf[in_head] = (uint8_t)(c & 0xFF);
        in_head = next;
    }
    if (in_waiter)
        xTaskNotifyGive(in_waiter);
}

void vt100_input_push_str(const char *s) {
    while (*s)
        vt100_input_push((unsigned char)*s++);
}

int vt100_getch(void) {
    /* Flush pending display updates before blocking */
    flush_display();
    while (in_tail == in_head)
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(100));
    int c = inbuf[in_tail];
    in_tail = (in_tail + 1) % VT100_INBUF_SIZE;
    return c;
}

int vt100_getch_timeout(int us) {
    /* Flush pending display updates */
    flush_display();
    /* Check if data available */
    if (in_tail != in_head) {
        int c = inbuf[in_tail];
        in_tail = (in_tail + 1) % VT100_INBUF_SIZE;
        return c;
    }
    /* Truly non-blocking when us <= 0 */
    if (us <= 0)
        return -1;
    /* Wait with timeout */
    int ticks = us / 1000;  /* microseconds to ms */
    if (ticks < 1) ticks = 1;
    ticks = pdMS_TO_TICKS(ticks);
    if (ticks < 1) ticks = 1;
    ulTaskNotifyTake(pdTRUE, ticks);
    if (in_tail != in_head) {
        int c = inbuf[in_tail];
        in_tail = (in_tail + 1) % VT100_INBUF_SIZE;
        return c;
    }
    return -1; /* PICO_ERROR_TIMEOUT */
}

void vt100_ungetc(int c) {
    /* Push character back to head of input buffer */
    int prev = (in_tail - 1 + VT100_INBUF_SIZE) % VT100_INBUF_SIZE;
    if (prev != in_head) {
        in_tail = prev;
        inbuf[in_tail] = (uint8_t)(c & 0xFF);
    }
}

void vt100_input_flush(void) {
    /* Drain ring buffer */
    in_head = in_tail = 0;
    /* Clear any stale FreeRTOS task notifications so the next
     * ulTaskNotifyTake blocks properly instead of returning stale counts */
    if (in_waiter)
        ulTaskNotifyTake(pdTRUE, 0);
}

/* ═════════════════════════════════════════════════════════════════════════
 * Initialisation & lifecycle
 * ═════════════════════════════════════════════════════════════════════════ */

void vt100_init(int cols, int rows) {
    if (cols > VT100_MAX_COLS) cols = VT100_MAX_COLS;
    if (rows > VT100_MAX_ROWS) rows = VT100_MAX_ROWS;
    tb_cols = cols;
    tb_rows = rows;

    int buf_size = cols * rows * 2;
    textbuf = (uint8_t *)malloc(buf_size);
    shadow_buf = (uint8_t *)malloc(buf_size);
    if (!textbuf || !shadow_buf) return;

    /* Fill with spaces, default attribute (light gray on black) */
    uint8_t def_attr = (COLOR_BLACK << 4) | COLOR_LIGHT_GRAY;
    for (int i = 0; i < cols * rows; i++) {
        textbuf[i * 2]     = ' ';
        textbuf[i * 2 + 1] = def_attr;
    }
    /* Mark shadow as all-different to force initial full paint */
    memset(shadow_buf, 0xFF, buf_size);

    cursor_col = 0;
    cursor_row = 0;
    cur_fg = COLOR_LIGHT_GRAY;
    cur_bg = COLOR_BLACK;
    bold_on = false;
    reverse_on = false;
    cursor_visible = true;
    cursor_enabled = true;
    vt_dirty = false;
    vt_state = ST_NORMAL;
    cached_fb = NULL;

    /* Input ring buffer */
    in_head = 0;
    in_tail = 0;
    in_waiter = NULL;  /* Set by vt100_set_waiter() from shell task */
}

void vt100_destroy(void) {
    if (textbuf) { free(textbuf); textbuf = NULL; }
    if (shadow_buf) { free(shadow_buf); shadow_buf = NULL; }
    in_waiter = NULL;
}

void vt100_set_waiter(void *task_handle) {
    in_waiter = (TaskHandle_t)task_handle;
}

void vt100_set_hwnd(hwnd_t hwnd) {
    g_vt_hwnd = hwnd;
}

/* ═════════════════════════════════════════════════════════════════════════
 * Resize
 * ═════════════════════════════════════════════════════════════════════════ */

void vt100_resize(int cols, int rows) {
    if (cols > VT100_MAX_COLS) cols = VT100_MAX_COLS;
    if (rows > VT100_MAX_ROWS) rows = VT100_MAX_ROWS;
    if (cols == tb_cols && rows == tb_rows) return;

    int new_size = cols * rows * 2;
    uint8_t *new_buf = (uint8_t *)malloc(new_size);
    uint8_t *new_shadow = (uint8_t *)malloc(new_size);
    if (!new_buf || !new_shadow) {
        if (new_buf) free(new_buf);
        if (new_shadow) free(new_shadow);
        return;
    }

    uint8_t def_attr = (COLOR_BLACK << 4) | COLOR_LIGHT_GRAY;
    for (int i = 0; i < cols * rows; i++) {
        new_buf[i * 2]     = ' ';
        new_buf[i * 2 + 1] = def_attr;
    }

    /* Copy what fits from old buffer */
    int copy_rows = (rows < tb_rows) ? rows : tb_rows;
    int copy_cols = (cols < tb_cols) ? cols : tb_cols;
    for (int r = 0; r < copy_rows; r++)
        for (int c = 0; c < copy_cols; c++) {
            int old_off = (r * tb_cols + c) * 2;
            int new_off = (r * cols + c) * 2;
            new_buf[new_off]     = textbuf[old_off];
            new_buf[new_off + 1] = textbuf[old_off + 1];
        }

    memset(new_shadow, 0xFF, new_size); /* force full repaint */

    free(textbuf);
    free(shadow_buf);
    textbuf = new_buf;
    shadow_buf = new_shadow;
    tb_cols = cols;
    tb_rows = rows;

    if (cursor_col >= cols) cursor_col = cols - 1;
    if (cursor_row >= rows) cursor_row = rows - 1;
    cached_fb = NULL;
}

void vt100_get_size(int *cols, int *rows) {
    *cols = tb_cols;
    *rows = tb_rows;
}

void vt100_invalidate(void) {
    if (shadow_buf)
        memset(shadow_buf, 0xFF, tb_cols * tb_rows * 2);
    vt_dirty = true;
}

/* ═════════════════════════════════════════════════════════════════════════
 * Cursor blink
 * ═════════════════════════════════════════════════════════════════════════ */

void vt100_toggle_cursor(void) {
    if (!vt_active) return;          /* no-op when unfocused */
    cursor_visible = !cursor_visible;
    /* Mark dirty so flush_display() re-evaluates the cursor cell
     * (shadow has old cursor state → comparison fails → cell re-rendered).
     * Also flushes any pending text output. */
    vt_dirty = true;
    flush_display();
}

void vt100_flush(void) {
    flush_display();
}

void vt100_set_active(bool active) {
    vt_active = active;
    if (active) {
        /* Force full repaint on next paint — shadow mismatch redraws everything */
        vt100_invalidate();
    } else {
        /* Null out the cached framebuffer pointer so even a racing
         * flush_display() from the shell task cannot write to the
         * screen while we don't own it. */
        cached_fb = NULL;
    }
}

/* ═════════════════════════════════════════════════════════════════════════
 * Paint callback — render terminal buffer to window framebuffer
 *
 * Framebuffer format: 4-bit nibble-packed
 *   high nibble = left (even-x) pixel, low nibble = right (odd-x) pixel
 *   stride = FB_STRIDE (320) bytes per row
 *
 * font_8x16[] glyphs: LSB = leftmost pixel (matches MMBasic pattern)
 * ═════════════════════════════════════════════════════════════════════════ */

void vt100_paint(hwnd_t hwnd) {
    if (!textbuf) return;

    wd_begin(hwnd);
    int16_t stride;
    uint8_t *dst = wd_fb_ptr(0, 0, &stride);
    if (!dst) {
        wd_end();
        return;
    }

    /* Compute visible area for clipping */
    int16_t clip_w, clip_h;
    wd_get_clip_size(&clip_w, &clip_h);

    /* Cache framebuffer info for direct access from shell/timer tasks.
     * Only cache when active (has focus) — otherwise the shell task
     * must not write to the display behind other windows. */
    if (vt_active) {
        cached_fb      = dst;
        cached_stride  = stride;
        cached_clip_w  = clip_w;
        cached_clip_h  = clip_h;
    }

    int vis_cols = tb_cols;
    int vis_rows = tb_rows;
    if (vis_cols * VT100_FONT_W > clip_w)
        vis_cols = clip_w / VT100_FONT_W;
    if (vis_rows * VT100_FONT_H > clip_h)
        vis_rows = clip_h / VT100_FONT_H;

    if (vis_cols == 0 || vis_rows == 0) {
        wd_end();
        return;
    }

    /* Full repaint — WM triggered this (window move, expose, etc.) */
    for (int row = 0; row < vis_rows; row++) {
        for (int col = 0; col < vis_cols; col++) {
            int off = TB_OFF(row, col);
            uint8_t ch   = textbuf[off];
            uint8_t attr = textbuf[off + 1];

            /* Cursor: invert fg/bg on current cell when visible and enabled */
            bool is_cursor = (row == cursor_row && col == cursor_col
                              && cursor_visible && cursor_enabled);
            uint8_t eff_attr = is_cursor ? (uint8_t)((attr >> 4) | (attr << 4))
                                         : attr;

            /* Sync shadow buffer */
            shadow_buf[off]     = ch;
            shadow_buf[off + 1] = eff_attr;

            render_cell(dst, stride, row, col, ch, eff_attr);
        }
    }

    vt_dirty = false;
    wd_end();
}
