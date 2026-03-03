/*
 * frankos_main.c — Frank OS entry point for MMBasic
 *
 * Creates an 8×16 monospaced terminal window (79×24 cells) and runs
 * the PicoMite MMBasic BASIC interpreter inside it.  Keyboard events
 * are translated to ANSI/VT100 sequences and fed to the interpreter.
 * Text output is rendered directly into the Frank OS framebuffer.
 *
 * Copyright (c) 2026 Mikhail Matveev <xtreme@rh1.tech>
 * https://rh1.tech
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/* configSTACK_DEPTH_TYPE is used in m-os-api-tasks.h (FreeRTOS task.h) but
 * not defined by MMBasic's FreeRTOSConfig.h.  Define it before the include. */
#ifndef configSTACK_DEPTH_TYPE
#define configSTACK_DEPTH_TYPE  uint16_t
#endif
#include "m-os-api.h"
#include "frankos-app.h"

#undef switch
#undef inline
#undef __force_inline
#undef abs

#include <string.h>
#include <stdint.h>
#include <stdbool.h>

#include "font.h"     /* font_8x16[4096]: 256 glyphs × 16 rows, 8 px wide */

/* ── Terminal geometry ──────────────────────────────────────────────────── */
#define BASIC_COLS          79
#define BASIC_ROWS          24
#define BASIC_FW             8   /* glyph width  in pixels */
#define BASIC_FH            16   /* glyph height in pixels */

#define BASIC_CLIENT_W  (BASIC_COLS * BASIC_FW)   /* 632 */
#define BASIC_CLIENT_H  (BASIC_ROWS * BASIC_FH)   /* 384 */

/* Frank OS window outer dimensions (title + border, no menu bar). */
#define BASIC_WIN_W  (BASIC_CLIENT_W + 2 * THEME_BORDER_WIDTH)
#define BASIC_WIN_H  (BASIC_CLIENT_H + THEME_TITLE_HEIGHT + 2 * THEME_BORDER_WIDTH)

/* ── Text cell format ───────────────────────────────────────────────────── */
/* attr byte: high nibble = background color index (0-15 = CGA/Frank OS COLOR_*)
 *            low  nibble = foreground color index */
typedef struct { uint8_t ch; uint8_t attr; } cell_t;

/* Default attribute: fg = COLOR_LIGHT_GRAY (7), bg = COLOR_BLACK (0). */
#define DEFAULT_ATTR  0x07u

/* ── Text buffer (volatile: written by BASIC task, read by paint task).
 * Declared as pointer-to-row so g_textbuf[r][c] syntax still works.
 * Allocated from PSRAM via calloc() at the start of main(). ──────────── */
static volatile cell_t  (*g_textbuf)[BASIC_COLS] = NULL;
static volatile int       g_cur_col  = 0;
static volatile int       g_cur_row  = 0;
static volatile bool      g_cur_vis  = false;  /* cursor blink state */

/* ── Keyboard ring buffer ───────────────────────────────────────────────── */
#define KBUF_SZ  512
static volatile uint8_t   g_kbuf[KBUF_SZ];
static volatile int       g_kbuf_head = 0;
static volatile int       g_kbuf_tail = 0;

/* ── App-wide state ─────────────────────────────────────────────────────── */
static hwnd_t             g_hwnd    = HWND_NULL;
static TaskHandle_t       g_task    = NULL;

/* Closing flag and MMAbort — also read by frankos_platform.c */
volatile bool             g_closing = false;

/* Autorun path — set from argv[1], consumed by basic_run_interpreter() */
char                      g_autorun_path[256];

/* MMAbort is declared in PicoMite.c and wrapped; updated here on Ctrl+C. */
extern volatile int       MMAbort;

/* Platform init + interpreter entry — in frankos_platform.c */
extern void               basic_platform_init(void);
extern void               basic_run_interpreter(void);

/* ═════════════════════════════════════════════════════════════════════════
 * Keyboard ring buffer
 * ═════════════════════════════════════════════════════════════════════════ */

/* Push one byte into ring buffer and wake BASIC task. */
void basic_kbuf_push(int c)
{
    int next = (g_kbuf_head + 1) % KBUF_SZ;
    if (next != g_kbuf_tail) {
        g_kbuf[g_kbuf_head] = (uint8_t)(c & 0xFF);
        g_kbuf_head = next;
    }
    /* Wake the interpreter if it is sleeping in __wrap_getConsole(). */
    if (g_task)
        xTaskNotifyGive(g_task);
}

/* Pop one byte; returns -1 if empty. Called by __wrap_getConsole(). */
int basic_kbuf_pop(void)
{
    if (g_kbuf_tail == g_kbuf_head)
        return -1;
    int c = g_kbuf[g_kbuf_tail];
    g_kbuf_tail = (g_kbuf_tail + 1) % KBUF_SZ;
    return c;
}

/* Number of bytes waiting in the ring buffer. */
int basic_kbuf_avail(void)
{
    int n = g_kbuf_head - g_kbuf_tail;
    if (n < 0) n += KBUF_SZ;
    return n;
}

/* Yield to FreeRTOS for 1 tick.  Called from PicoMite.c getConsole()
 * when the keyboard buffer is empty, so the BASIC task does not
 * busy-loop and starve other tasks (WM, cursor blink, etc.). */
void basic_yield(void)
{
    vTaskDelay(1);
}

/* ═════════════════════════════════════════════════════════════════════════
 * Text buffer helpers
 * ═════════════════════════════════════════════════════════════════════════ */

static void tbuf_clear(void)
{
    for (int r = 0; r < BASIC_ROWS; r++)
        for (int c = 0; c < BASIC_COLS; c++) {
            g_textbuf[r][c].ch   = ' ';
            g_textbuf[r][c].attr = DEFAULT_ATTR;
        }
    g_cur_col = 0;
    g_cur_row = 0;
}

/* Non-static wrapper so frankos_platform.c's ClearScreen() can call it
 * without needing to know the cell_t type or g_textbuf's pointer type. */
void basic_tbuf_clear(void) { tbuf_clear(); }

static void tbuf_scroll_up(void)
{
    for (int r = 0; r < BASIC_ROWS - 1; r++)
        for (int c = 0; c < BASIC_COLS; c++)
            g_textbuf[r][c] = g_textbuf[r + 1][c];
    for (int c = 0; c < BASIC_COLS; c++) {
        g_textbuf[BASIC_ROWS - 1][c].ch   = ' ';
        g_textbuf[BASIC_ROWS - 1][c].attr = DEFAULT_ATTR;
    }
}

/*
 * basic_textbuf_putc — emit one character into the terminal buffer.
 * Called from frankos_platform.c's DisplayPutC() stub.
 * Handles CR (\r), LF (\n), BS (\b) and normal printable chars.
 * Scrolls the screen when the cursor reaches the last row.
 */
void basic_textbuf_putc(int c)
{
    if (c == '\r') {
        g_cur_col = 0;
        goto done;
    }
    if (c == '\n') {
        g_cur_col = 0;
        g_cur_row++;
        if (g_cur_row >= BASIC_ROWS) {
            tbuf_scroll_up();
            g_cur_row = BASIC_ROWS - 1;
        }
        goto done;
    }
    if (c == '\b') {
        if (g_cur_col > 0) {
            g_cur_col--;
        } else if (g_cur_row > 0) {
            g_cur_row--;
            g_cur_col = BASIC_COLS - 1;
        }
        g_textbuf[g_cur_row][g_cur_col].ch   = ' ';
        g_textbuf[g_cur_row][g_cur_col].attr  = DEFAULT_ATTR;
        goto done;
    }
    if (c < 0x20)
        goto done;   /* ignore other control chars */

    /* Wrap to next line if needed. */
    if (g_cur_col >= BASIC_COLS) {
        g_cur_col = 0;
        g_cur_row++;
        if (g_cur_row >= BASIC_ROWS) {
            tbuf_scroll_up();
            g_cur_row = BASIC_ROWS - 1;
        }
    }
    g_textbuf[g_cur_row][g_cur_col].ch   = (uint8_t)c;
    g_textbuf[g_cur_row][g_cur_col].attr = DEFAULT_ATTR;
    g_cur_col++;

done:
    if (g_hwnd != HWND_NULL)
        wm_invalidate(g_hwnd);
}

/* ═════════════════════════════════════════════════════════════════════════
 * Paint callback — runs on the WM / compositor task
 * ═════════════════════════════════════════════════════════════════════════ */

/*
 * Shadow buffer for dirty-cell tracking.  Stores the last-painted state
 * as (ch, effective_attr) — where effective_attr already has cursor
 * inversion baked in.  Only cells that differ from the shadow are
 * redrawn, which cuts per-frame work from 1896 cells to typically 1-5.
 */
static cell_t      g_shadow[BASIC_ROWS][BASIC_COLS];
static uint8_t    *g_last_dst = NULL;  /* detect window moves */

/*
 * Render dirty cells from the text buffer into the Frank OS window
 * framebuffer.  Uses a shadow buffer comparison to skip unchanged cells.
 *
 * Framebuffer is nibble-packed 4bpp: high nibble = left (even) pixel,
 *                                     low  nibble = right (odd) pixel.
 * font_8x16[ch*16 + row] has LSB = leftmost pixel.
 */
static void basic_paint(hwnd_t hwnd)
{
    wd_begin(hwnd);

    bool fs = wm_is_fullscreen(hwnd);

    /* In fullscreen (640×480), the 79×24 text grid (632×384) doesn't
     * fill the screen.  Centre it and fill the margins with black. */
    int16_t x_off = 0, y_off = 0;
    if (fs) {
        x_off = (DISPLAY_WIDTH  - BASIC_CLIENT_W) / 2;  /* 4 */
        y_off = (DISPLAY_HEIGHT - BASIC_CLIENT_H) / 2;  /* 48 */
    }

    int16_t stride;
    uint8_t *dst = wd_fb_ptr(x_off, y_off, &stride);
    if (!dst) {
        wd_end();
        return;
    }

    /* Fill black margins in fullscreen on first paint / mode switch */
    static bool prev_fs = false;
    if (fs && !prev_fs) {
        /* Top bar */
        wd_fill_rect(0, 0, DISPLAY_WIDTH, y_off, COLOR_BLACK);
        /* Bottom bar */
        wd_fill_rect(0, y_off + BASIC_CLIENT_H, DISPLAY_WIDTH,
                     DISPLAY_HEIGHT - y_off - BASIC_CLIENT_H, COLOR_BLACK);
        /* Left strip */
        wd_fill_rect(0, y_off, x_off, BASIC_CLIENT_H, COLOR_BLACK);
        /* Right strip */
        wd_fill_rect(x_off + BASIC_CLIENT_W, y_off,
                     DISPLAY_WIDTH - x_off - BASIC_CLIENT_W,
                     BASIC_CLIENT_H, COLOR_BLACK);
    }
    prev_fs = fs;

    /* ── Compute visible columns / rows ─────────────────────────────
     * When the window extends past the screen edge, writing the full
     * 79×24 grid would overflow the framebuffer row (stride=320 bytes).
     * Probe wd_fb_ptr at the last pixel of each column/row to find
     * how many complete cells fit.  The checks are monotonic, so we
     * break on the first failure.
     * ─────────────────────────────────────────────────────────────── */
    int vis_cols = BASIC_COLS;
    int vis_rows = BASIC_ROWS;
    if (!fs) {
        int16_t dummy;
        /* Horizontal clipping */
        if (!wd_fb_ptr(x_off + BASIC_CLIENT_W - 1, y_off, &dummy)) {
            vis_cols = 0;
            for (int c = 0; c < BASIC_COLS; c++) {
                if (wd_fb_ptr(x_off + (c + 1) * BASIC_FW - 1, y_off, &dummy))
                    vis_cols = c + 1;
                else
                    break;
            }
        }
        /* Vertical clipping */
        if (!wd_fb_ptr(x_off, y_off + BASIC_CLIENT_H - 1, &dummy)) {
            vis_rows = 0;
            for (int r = 0; r < BASIC_ROWS; r++) {
                if (wd_fb_ptr(x_off, y_off + (r + 1) * BASIC_FH - 1, &dummy))
                    vis_rows = r + 1;
                else
                    break;
            }
        }
    }

    if (vis_cols == 0 || vis_rows == 0) {
        wd_end();
        return;
    }

    /* Force full repaint when the framebuffer origin changes (window
     * was moved or fullscreen toggled) — background is re-filled. */
    bool force = (dst != g_last_dst);
    g_last_dst = dst;

    for (int row = 0; row < vis_rows; row++) {
        for (int col = 0; col < vis_cols; col++) {
            uint8_t ch   = g_textbuf[row][col].ch;
            uint8_t attr = g_textbuf[row][col].attr;

            /* Cursor: invert fg/bg on current cell when visible. */
            bool cursor = (row == g_cur_row && col == g_cur_col && g_cur_vis);
            uint8_t eff_attr = cursor ? (uint8_t)((attr >> 4) | (attr << 4))
                                      : attr;

            /* Skip unchanged cells (shadow tracks last-painted state). */
            if (!force &&
                g_shadow[row][col].ch == ch &&
                g_shadow[row][col].attr == eff_attr)
                continue;

            g_shadow[row][col].ch   = ch;
            g_shadow[row][col].attr = eff_attr;

            uint8_t fg = eff_attr & 0x0Fu;
            uint8_t bg = (eff_attr >> 4) & 0x0Fu;
            const uint8_t *glyph = &font_8x16[(uint8_t)ch * BASIC_FH];

            for (int gy = 0; gy < BASIC_FH; gy++) {
                uint8_t bits = glyph[gy];
                uint8_t *drow = dst
                              + (row * BASIC_FH + gy) * stride
                              + (col * BASIC_FW) / 2;

                for (int bx = 0; bx < BASIC_FW / 2; bx++) {
                    uint8_t left  = (bits & 0x01) ? fg : bg;
                    uint8_t right = (bits & 0x02) ? fg : bg;
                    *drow++ = (left << 4) | right;
                    bits >>= 2;
                }
            }
        }
    }

    wd_end();
}

/* ═════════════════════════════════════════════════════════════════════════
 * Window event callback
 * ═════════════════════════════════════════════════════════════════════════ */

/* Push a NUL-terminated string into the keyboard ring buffer. */
static void push_vt(const char *s)
{
    while (*s)
        basic_kbuf_push((unsigned char)*s++);
}

static bool basic_event(hwnd_t hwnd, const window_event_t *event)
{
    (void)hwnd;

    if (event->type == WM_CLOSE) {
        if (wm_is_fullscreen(hwnd))
            wm_toggle_fullscreen(hwnd);
        g_closing = true;
        MMAbort   = 1;
        if (g_task)
            xTaskNotifyGive(g_task);
        return true;
    }

    /* WM_CHAR: printable characters and control chars (Enter, BS, Tab, Esc…) */
    if (event->type == WM_CHAR) {
        char ch = event->charev.ch;
        if (ch == '\x03') {   /* Ctrl+C */
            MMAbort = 1;
        }
        if (ch != '\0')
            basic_kbuf_push((unsigned char)ch);
        return true;
    }

    /* WM_KEYDOWN: navigation and function keys only (they have no WM_CHAR). */
    if (event->type == WM_KEYDOWN) {
        uint8_t sc  = event->key.scancode;
        uint8_t mod = event->key.modifiers;

        /* Ctrl+C via raw scan: HID 'c' = 0x06 */
        if ((mod & KMOD_CTRL) && sc == 0x06) {
            MMAbort = 1;
            basic_kbuf_push(3);
            return true;
        }

        /* Alt+Enter → toggle fullscreen */
        if (sc == 0x28 && (mod & KMOD_ALT)) {
            wm_toggle_fullscreen(hwnd);
            return true;
        }

        /* Enter, Backspace, Tab, Esc — the OS only sends WM_CHAR for
         * printable ASCII (0x20-0x7E), so these must be handled here. */
        switch (sc) {
        case 0x28: basic_kbuf_push('\r');  return true;  /* Enter     */
        case 0x58: basic_kbuf_push('\r');  return true;  /* KP Enter  */
        case 0x2A: basic_kbuf_push('\b');  return true;  /* Backspace */
        case 0x2B: basic_kbuf_push('\t');  return true;  /* Tab       */
        case 0x29: basic_kbuf_push(0x1B);  return true;  /* Esc       */
        case 0x4C: basic_kbuf_push(0x7F);  return true;  /* Delete    */
        }

        /* HID scancodes → VT100 / xterm escape sequences that MMInkey()
         * in PicoMite.c parses back to UP/DOWN/LEFT/RIGHT/HOME/…/F1…F12. */
        switch (sc) {
        case 0x52: push_vt("\x1b[A");   return true;  /* Up       */
        case 0x51: push_vt("\x1b[B");   return true;  /* Down     */
        case 0x4F: push_vt("\x1b[C");   return true;  /* Right    */
        case 0x50: push_vt("\x1b[D");   return true;  /* Left     */
        case 0x4A: push_vt("\x1b[1~");  return true;  /* Home     */
        case 0x4D: push_vt("\x1b[4~");  return true;  /* End      */
        case 0x4B: push_vt("\x1b[5~");  return true;  /* Page Up  */
        case 0x4E: push_vt("\x1b[6~");  return true;  /* Page Dn  */
        case 0x49: push_vt("\x1b[2~");  return true;  /* Insert   */
        case 0x3A: push_vt("\x1bOP");   return true;  /* F1       */
        case 0x3B: push_vt("\x1bOQ");   return true;  /* F2       */
        case 0x3C: push_vt("\x1bOR");   return true;  /* F3       */
        case 0x3D: push_vt("\x1bOS");   return true;  /* F4       */
        case 0x3E: push_vt("\x1b[15~"); return true;  /* F5       */
        case 0x3F: push_vt("\x1b[17~"); return true;  /* F6       */
        case 0x40: push_vt("\x1b[18~"); return true;  /* F7       */
        case 0x41: push_vt("\x1b[19~"); return true;  /* F8       */
        case 0x42: push_vt("\x1b[20~"); return true;  /* F9       */
        case 0x43: push_vt("\x1b[21~"); return true;  /* F10      */
        case 0x44: push_vt("\x1b[23~"); return true;  /* F11      */
        case 0x45: push_vt("\x1b[24~"); return true;  /* F12      */
        default:   break;
        }
        return false;
    }

    return false;
}

/* ═════════════════════════════════════════════════════════════════════════
 * Cursor blink timer (500 ms period)
 * ═════════════════════════════════════════════════════════════════════════ */

static void blink_cb(TimerHandle_t t)
{
    (void)t;
    g_cur_vis = !g_cur_vis;
    if (g_hwnd != HWND_NULL)
        wm_invalidate(g_hwnd);
}

/* ═════════════════════════════════════════════════════════════════════════
 * Frank OS app entry point
 * ═════════════════════════════════════════════════════════════════════════ */

int main(int argc, char **argv)
{
    /* ── Singleton: if already running, focus existing window ──── */
    hwnd_t existing = wm_find_window_by_title("MMBasic");
    if (existing != HWND_NULL) {
        wm_set_focus(existing);
        return 0;
    }

    printf("[basic] main() start\n");
    g_task = xTaskGetCurrentTaskHandle();
    printf("[basic] task handle: %p\n", (void*)g_task);

    /* Store autorun path from file association launch */
    g_autorun_path[0] = '\0';
    if (argc > 1 && argv[1] && argv[1][0]) {
        strncpy(g_autorun_path, argv[1], sizeof(g_autorun_path) - 1);
        g_autorun_path[sizeof(g_autorun_path) - 1] = '\0';
    }

    /* Allocate text buffer from PSRAM heap before any display calls. */
    if (!g_textbuf)
        g_textbuf = (volatile cell_t (*)[BASIC_COLS])
                    calloc(BASIC_ROWS, sizeof(*g_textbuf));
    printf("[basic] g_textbuf: %p\n", (void*)g_textbuf);
    if (!g_textbuf) {
        printf("[basic] FATAL: g_textbuf calloc failed\n");
        return 1;  /* fatal: no display buffer */
    }

    tbuf_clear();
    printf("[basic] tbuf_clear done\n");

    /* Centre the window (no taskbar overlap). */
    int16_t fw = (int16_t)BASIC_WIN_W;
    int16_t fh = (int16_t)BASIC_WIN_H;
    int16_t x  = (int16_t)((DISPLAY_WIDTH  - fw) / 2);
    int16_t y  = (int16_t)((DISPLAY_HEIGHT - TASKBAR_HEIGHT - fh) / 2);
    if (y < 0) y = 0;
    printf("[basic] wm_create_window %dx%d at (%d,%d)\n", fw, fh, x, y);

    g_hwnd = wm_create_window(x, y, fw, fh, "MMBasic",
                              WSTYLE_DIALOG | WF_FULLSCREENABLE,
                              basic_event, basic_paint);
    printf("[basic] g_hwnd: %p\n", (void*)(uintptr_t)g_hwnd);
    if (g_hwnd == HWND_NULL) {
        printf("[basic] FATAL: wm_create_window returned NULL\n");
        return 1;
    }

    wm_show_window(g_hwnd);
    wm_set_focus(g_hwnd);
    taskbar_invalidate();

    /* Start cursor blink (500 ms, auto-reload). */
    TimerHandle_t blink_tmr = xTimerCreate("BLINK",
                                            pdMS_TO_TICKS(500),
                                            pdTRUE, NULL, blink_cb);
    if (blink_tmr)
        xTimerStart(blink_tmr, 0);

    /* Initialise platform and run the interpreter (blocks until exit). */
    basic_platform_init();
    basic_run_interpreter();

    /* Tear down. */
    if (blink_tmr) {
        xTimerStop(blink_tmr, 0);
        xTimerDelete(blink_tmr, 0);
    }
    wm_destroy_window(g_hwnd);
    g_hwnd = HWND_NULL;
    taskbar_invalidate();

    return 0;
}

uint32_t __app_flags(void) { return APPFLAG_SINGLETON; }
