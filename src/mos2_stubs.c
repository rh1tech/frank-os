/*
 * MOS2 compatibility stubs for FRANK OS.
 *
 * Provides no-op implementations for MOS2 subsystems that FRANK OS doesn't have
 * (graphics driver, PSRAM, USB mass storage, NES gamepad, etc.) and routes
 * console I/O through the FRANK OS terminal window.
 *
 * These satisfy the linker for functions referenced in sys_table.c and app.c.
 */

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include "FreeRTOS.h"
#include "task.h"
#include "ff.h"
#include "graphics.h"
#include "terminal.h"
#include "keyboard.h"
#include "font.h"
#include "psram.h"

/*==========================================================================
 * Global variable: keyboard character (MOS2 keyboard.c uses this)
 * Kept for backward compatibility — keyboard.c still writes to it.
 * Per-terminal code should use terminal_t.mos2_c instead.
 *=========================================================================*/
volatile int __c = 0;

/*==========================================================================
 * Global variable: PSRAM buffer size (referenced by cmd.c and app.c)
 * Named _var to avoid clash with the butter_psram_size() function in psram.h.
 *=========================================================================*/
uint32_t butter_psram_size_var = 0;

/*==========================================================================
 * Console I/O — route to active terminal
 *=========================================================================*/

void goutf(const char *restrict fmt, ...) {
    terminal_t *t = terminal_get_active();
    if (!t) return;
    va_list ap;
    va_start(ap, fmt);
    char buf[256];
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    terminal_puts(t, buf);
}

void fgoutf(FIL *f, const char *restrict fmt, ...) {
    if (f) {
        va_list ap;
        va_start(ap, fmt);
        char buf[256];
        vsnprintf(buf, sizeof(buf), fmt, ap);
        va_end(ap);
        UINT bw;
        f_write(f, buf, strlen(buf), &bw);
        return;
    }
    terminal_t *t = terminal_get_active();
    if (!t) return;
    va_list ap;
    va_start(ap, fmt);
    char buf[256];
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    terminal_puts(t, buf);
}

void __putc(char c) {
    terminal_t *t = terminal_get_active();
    if (t) terminal_putc(t, c);
}

void gouta(char *buf) {
    terminal_t *t = terminal_get_active();
    if (t) terminal_puts(t, buf);
}

void gbackspace(void) {
    terminal_t *t = terminal_get_active();
    if (t) terminal_putc(t, '\b');
}

char __getch(void) {
    /* MOS2 mechanism: block until the scancode handler sets mos2_c */
    terminal_t *t = terminal_get_active();
    TaskHandle_t th = xTaskGetCurrentTaskHandle();
    kbd_add_stdin_waiter(th);
    if (t) {
        while (!t->mos2_c) {
            ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(50));
        }
        char c = (char)(t->mos2_c & 0xFF);
        t->mos2_c = 0;
        return c;
    }
    /* Fallback: use global __c if no terminal (shouldn't happen) */
    while (!__c) {
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(50));
    }
    char c = (char)(__c & 0xFF);
    __c = 0;
    return c;
}

int __getc(FIL *f) {
    if (f) {
        uint8_t c;
        UINT br;
        if (f_read(f, &c, 1, &br) == FR_OK && br == 1)
            return c;
        return -1;
    }
    return (int)__getch();
}

char getch_now(void) {
    /* MOS2 mechanism: non-blocking read of mos2_c set by scancode handler */
    terminal_t *t = terminal_get_active();
    if (t && t->mos2_c) {
        char c = (char)(t->mos2_c & 0xFF);
        t->mos2_c = 0;
        return c;
    }
    /* On MOS2, keyboard scancodes are processed in a PIO ISR that preempts
     * any task and sets __c.  On FRANK OS, scancode processing runs in input_task
     * (priority 3).  App tasks run at priority 7 and would starve input_task
     * in a busy-wait loop.  Yield for 1 tick so input_task can process keys. */
    vTaskDelay(1);
    return 0;
}

/*==========================================================================
 * Console info
 *=========================================================================*/

uint32_t get_console_width(void) { return TERM_COLS; }
uint32_t get_console_height(void) { return TERM_ROWS; }
uint32_t get_screen_width(void) { return 320; }
uint32_t get_screen_height(void) { return 240; }
uint8_t get_console_bitness(void) { return 4; }
uint8_t get_screen_bitness(void) { return 4; }

void graphics_set_con_pos(int x, int y) {
    terminal_t *t = terminal_get_active();
    if (t) terminal_set_cursor(t, x, y);
}

void graphics_set_con_color(uint8_t fg, uint8_t bg) {
    terminal_t *t = terminal_get_active();
    if (t) terminal_set_color(t, fg, bg);
}

void clrScr(uint8_t color) {
    terminal_t *t = terminal_get_active();
    if (t) terminal_clear(t, color);
}

int graphics_con_x(void) {
    terminal_t *t = terminal_get_active();
    return t ? terminal_get_cursor_col(t) : 0;
}

int graphics_con_y(void) {
    terminal_t *t = terminal_get_active();
    return t ? terminal_get_cursor_row(t) : 0;
}

/*==========================================================================
 * Graphics driver — stubs (FRANK OS uses its own windowing system)
 *=========================================================================*/

static graphics_driver_t *g_driver = NULL;

/* Override buffer — set by apps via graphics_set_buffer() */
static uint8_t *s_override_buf = NULL;

/*==========================================================================
 * draw_text — the fundamental drawing primitive for MOS2 text UI.
 * Routes to the active terminal's character grid.
 *=========================================================================*/

void draw_text(const char *s, int x, int y, uint8_t color, uint8_t bgcolor) {
    /* If a graphics driver is installed and has draw_text, use it */
    if (g_driver && g_driver->draw_text) {
        g_driver->draw_text(s, x, y, color, bgcolor);
        return;
    }
    /* Otherwise route to terminal */
    terminal_t *t = terminal_get_active();
    if (t) terminal_draw_text(t, s, x, y, color, bgcolor);
}

/*==========================================================================
 * draw_panel — draws a box border with CP866 line-drawing characters.
 * Ported from MOS2 graphics.c.
 *=========================================================================*/

void draw_panel(color_schema_t *pcs, int left, int top, int width, int height,
                char *title, char *bottom) {
    char line[82]; /* max TERM_COLS + 2 */
    if (width > 80) width = 80;
    if (width < 2) return;

    uint8_t fg = pcs->FOREGROUND_FIELD_COLOR;
    uint8_t bg = pcs->BACKGROUND_FIELD_COLOR;

    /* Top border: ╔═══╗ */
    for (int i = 1; i < width - 1; i++) line[i] = 0xCD;
    line[0]         = 0xC9;
    line[width - 1] = 0xBB;
    line[width]     = 0;
    draw_text(line, left, top, fg, bg);

    if (title) {
        int sl = strlen(title);
        if (sl > width - 4) sl = width - 4;
        int title_left = left + (width - sl) / 2;
        char tbuf[82];
        snprintf(tbuf, sizeof(tbuf), " %.*s ", sl, title);
        draw_text(tbuf, title_left, top, fg, bg);
    }

    /* Middle rows: ║     ║ */
    memset(line, ' ', width);
    line[0]         = 0xBA;
    line[width - 1] = 0xBA;
    line[width]     = 0;
    for (int y = top + 1; y < top + height - 1; y++) {
        draw_text(line, left, y, fg, bg);
    }

    /* Bottom border: ╚═══╝ */
    for (int i = 1; i < width - 1; i++) line[i] = 0xCD;
    line[0]         = 0xC8;
    line[width - 1] = 0xBC;
    line[width]     = 0;
    draw_text(line, left, top + height - 1, fg, bg);

    if (bottom) {
        int sl = strlen(bottom);
        if (sl > width - 4) sl = width - 4;
        int bottom_left = left + (width - sl) / 2;
        char bbuf[82];
        snprintf(bbuf, sizeof(bbuf), " %.*s ", sl, bottom);
        draw_text(bbuf, bottom_left, top + height - 1, fg, bg);
    }
}

/*==========================================================================
 * draw_label — draws a text string padded to width with fg/bg from schema.
 * Ported from MOS2 graphics.c.
 *=========================================================================*/

void draw_label(color_schema_t *pcs, int left, int top, int width,
                char *txt, bool selected, bool highlighted) {
    char line[82];
    if (width > 80) width = 80;
    if (width < 1) return;

    bool fin = false;
    for (int i = 0; i < width; i++) {
        if (!fin) {
            if (!txt[i]) {
                fin = true;
                line[i] = ' ';
            } else {
                line[i] = txt[i];
            }
        } else {
            line[i] = ' ';
        }
    }
    line[width] = 0;

    int fgc = selected ? pcs->FOREGROUND_SELECTED_COLOR
            : highlighted ? pcs->HIGHLIGHTED_FIELD_COLOR
            : pcs->FOREGROUND_FIELD_COLOR;
    int bgc = selected ? pcs->BACKGROUND_SELECTED_COLOR
            : pcs->BACKGROUND_FIELD_COLOR;
    draw_text(line, left, top, fgc, bgc);
}

/*==========================================================================
 * draw_button — draws a centered text button.
 * Ported from MOS2 graphics.c.
 *=========================================================================*/

void draw_button(color_schema_t *pcs, int left, int top, int width,
                 const char *txt, bool selected) {
    int len = strlen(txt);
    if (len > 39) return;
    char tmp[42];
    if (width > 40) width = 40;

    int start = (width - len) / 2;
    for (int i = 0; i < start; i++) tmp[i] = ' ';

    bool fin = false;
    int j = 0;
    for (int i = start; i < width; i++) {
        if (!fin) {
            if (!txt[j]) {
                fin = true;
                tmp[i] = ' ';
            } else {
                tmp[i] = txt[j++];
            }
        } else {
            tmp[i] = ' ';
        }
    }
    tmp[width] = 0;

    draw_text(tmp, left, top,
              pcs->FOREGROUND_F_BTN_COLOR,
              selected ? pcs->BACKGROUND_SEL_BTN_COLOR
                       : pcs->BACKGROUND_F_BTN_COLOR);
}

/*==========================================================================
 * draw_box — draws a panel with interior labels and optional line content.
 * Ported from MOS2 graphics.c.
 *=========================================================================*/

void draw_box(color_schema_t *pcs, int left, int top, int width, int height,
              const char *title, const lines_t *plines) {
    draw_panel(pcs, left, top, width, height, (char *)title, 0);

    /* Fill interior with blank labels */
    for (int y = top + 1; y < top + height - 1; y++) {
        draw_label(pcs, left + 1, y, width - 2, "", false, false);
    }

    /* Draw content lines */
    if (plines) {
        for (int i = 0, y = top + 1 + plines->toff; i < plines->sz; i++, y++) {
            const line_t *pl = plines->plns + i;
            uint8_t off;
            if (pl->off < 0) {
                size_t slen = strnlen(pl->txt, width);
                off = (width - 2 > (int)slen) ? (width - slen) >> 1 : 0;
            } else {
                off = pl->off;
            }
            draw_label(pcs, left + 1 + off, y, width - 2 - off,
                        pl->txt, false, false);
        }
    }
}

/*==========================================================================
 * Other graphics stubs
 *=========================================================================*/

void draw_window(const char *title, uint32_t x, uint32_t y, uint32_t w, uint32_t h) {
    char line[82];
    if (w > 81) w = 81;
    if (w < 2 || h < 2) return;
    w--;
    h--;
    /* Top border: ╔═══╗ */
    memset(line, 0xCD, w);
    line[0] = 0xC9;
    line[w] = 0xBB;
    line[w + 1] = 0;
    draw_text(line, x, y, 11, 1);
    /* Bottom border: ╚═══╝ */
    line[0] = 0xC8;
    line[w] = 0xBC;
    draw_text(line, x, h + y, 11, 1);
    /* Middle rows: ║     ║ */
    memset(line, ' ', w);
    line[0] = line[w] = 0xBA;
    line[w + 1] = 0;
    for (uint32_t i = 1; i < h; i++) {
        draw_text(line, x, y + i, 11, 1);
    }
    /* Centered title */
    int tlen = snprintf(line, w, " %s ", title);
    if (tlen > 0) {
        line[tlen] = 0;
        int toff = (w - tlen) / 2;
        if (toff < 0) toff = 0;
        draw_text(line, x + toff, y, 14, 3);
    }
}
bool graphics_set_mode(int mode) { (void)mode; return true; }
void graphics_lock_buffer(bool lock) { (void)lock; }
void graphics_set_offset(int x, int y) { (void)x; (void)y; }
void graphics_set_buffer(uint8_t *buf) {
    s_override_buf = buf;  /* NULL restores default (terminal textbuf) */
}
void graphics_set_bgcolor(uint32_t color) {
    (void)color; /* pixel-mode screen bg — not relevant for text mode */
}
void cleanup_graphics(void) {}
void install_graphics_driver(graphics_driver_t *drv) { g_driver = drv; }
graphics_driver_t *get_graphics_driver(void) { return g_driver; }
uint8_t *get_buffer(void) {
    if (s_override_buf) return s_override_buf;
    terminal_t *t = terminal_get_active();
    return t ? terminal_get_textbuf(t) : NULL;
}
size_t get_buffer_size(void) {
    if (s_override_buf) return ((320 * 240 * 4) >> 3);
    terminal_t *t = terminal_get_active();
    return t ? terminal_get_textbuf_size(t) : 0;
}
bool is_buffer_text(void) { return true; }
int graphics_get_mode(void) { return 0; }
bool graphics_is_mode_text(int mode) { (void)mode; return true; }
int graphics_get_default_mode(void) { return 0; }
void set_cursor_color(uint8_t color) { (void)color; }

uint8_t *graphics_get_font_table(void) { return (uint8_t *)font_8x8; }
uint8_t graphics_get_font_width(void) { return 8; }
uint8_t graphics_get_font_height(void) { return 8; }
bool graphics_set_font(uint8_t w, uint8_t h) { (void)w; (void)h; return false; }
bool graphics_set_ext_font(uint8_t *table, uint8_t w, uint8_t h) { (void)table; (void)w; (void)h; return false; }

void set_vga_dma_handler_impl(dma_handler_impl_fn impl) { (void)impl; }
void set_vga_clkdiv(uint32_t pixel_clock, uint32_t line_size) { (void)pixel_clock; (void)line_size; }
void vga_dma_channel_set_read_addr(const volatile void *addr) { (void)addr; }

/*==========================================================================
 * PSRAM — memory-mapped access via QSPI CS1 (uncached window 0x15000000)
 *
 * When PSRAM hardware is present and initialized, these provide direct
 * memory-mapped read/write.  When absent, butter_psram_size() returns 0
 * and the stubs return zeros / no-op (same as before).
 *=========================================================================*/

uint32_t init_psram(void) {
    uint32_t sz = butter_psram_size();
    butter_psram_size_var = sz;
    return sz;
}
uint32_t psram_size(void) { return butter_psram_size(); }
void psram_cleanup(void) {}

void write8psram(uint32_t a, uint8_t v)   { *(volatile uint8_t  *)(PSRAM_BASE + a) = v; }
void write16psram(uint32_t a, uint16_t v) { *(volatile uint16_t *)(PSRAM_BASE + a) = v; }
void write32psram(uint32_t a, uint32_t v) { *(volatile uint32_t *)(PSRAM_BASE + a) = v; }
uint8_t  read8psram(uint32_t a)  { return *(volatile uint8_t  *)(PSRAM_BASE + a); }
uint16_t read16psram(uint32_t a) { return *(volatile uint16_t *)(PSRAM_BASE + a); }
uint32_t read32psram(uint32_t a) { return *(volatile uint32_t *)(PSRAM_BASE + a); }

void psram_id(uint8_t rx[8]) { memset(rx, 0, 8); }

void writepsram(uint32_t a, uint8_t *b, size_t sz) {
    memcpy((void *)(PSRAM_BASE + a), b, sz);
}
void readpsram(uint8_t *b, uint32_t a, size_t sz) {
    memcpy(b, (const void *)(PSRAM_BASE + a), sz);
}

/*==========================================================================
 * USB mass storage — stubs
 *=========================================================================*/

void init_pico_usb_drive(void) {}
void pico_usb_drive_heartbeat(void) {}
bool tud_msc_ejected(void) { return false; }
void set_tud_msc_ejected(bool v) { (void)v; }
void usb_driver(bool on) { (void)on; }
bool set_usb_detached_handler(void (*h)(void)) { (void)h; return false; }

/*==========================================================================
 * NES gamepad — stub
 *=========================================================================*/

void nespad_stat(uint8_t *pad1, uint8_t *pad2) { *pad1 = 0; *pad2 = 0; }

/*==========================================================================
 * PS/2 keyboard extras
 *=========================================================================*/

uint8_t get_leds_stat(void) { return 0; }

/*==========================================================================
 * System info
 *=========================================================================*/

uint32_t get_cpu_ram_size(void) { return 520 * 1024; }  /* RP2350: 520KB SRAM */
uint32_t get_cpu_flash_size(void) { return 4 * 1024 * 1024; } /* 4MB flash */
uint32_t get_cpu_flash_jedec_id(void) { return 0; }

/*==========================================================================
 * FatFS mount — return the FATFS used by sdcard_init.c
 *=========================================================================*/

extern FATFS fatfs;  /* defined in sdcard_init.c, passed to f_mount() */

FATFS *get_mount_fs(void) { return &fatfs; }

/*==========================================================================
 * show_logo — minimal implementation
 *=========================================================================*/

void show_logo(bool with_top) {
    (void)with_top;
    goutf("MOS\n");
}
