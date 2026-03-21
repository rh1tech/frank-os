/*
 * MurmNES - ROM Selector Menu
 * Displays cartridges with cover art from SD card metadata.
 * Uses 8-bit indexed mode with a fixed 6x6x6 RGB color cube palette.
 * SPDX-License-Identifier: MIT
 */

#include "rom_selector.h"
#include "board_config.h"
#include "pico/stdlib.h"
#include "nespad.h"
#include "ps2kbd_wrapper.h"
#include "ff.h"
#include "hardware/watchdog.h"
#include "hardware/xip_cache.h"
#include <string.h>
#include <stdio.h>

#ifdef USB_HID_ENABLED
#include "usbhid.h"
#endif

/* Symbols from main_pico.c */
extern volatile const uint8_t *pending_pixels;
extern volatile long pending_pitch;
extern volatile uint32_t vsync_flag;
extern uint32_t rgb565_palette_32[2][256];
extern int pal_write_idx;
extern volatile int pending_pal_idx;
extern uint8_t test_pixels[];
extern void audio_fill_silence(int count);

#define SCREEN_W 256
#define SCREEN_H 240
#define SAMPLE_RATE 44100

/* ─── Fixed 6x6x6 RGB color cube palette (216 colors) ────────────── */

/* Palette layout:
 *   0       = black (background)
 *   1-216   = 6x6x6 RGB cube
 *   217-255 = reserved for UI (cartridge body, text, etc.)
 */
#define PAL_BLACK     0
#define PAL_CUBE_BASE 1
#define PAL_CART_BODY  217  /* NES cart gray */
#define PAL_CART_LIGHT 218  /* top edge highlight */
#define PAL_CART_DARK  219  /* bottom/right shadow */
#define PAL_CART_LABEL 220  /* dark label background */
#define PAL_CART_RIDGE 221  /* ridge lines */
#define PAL_CART_SLOT  222  /* connector slot */
#define PAL_WHITE      223
#define PAL_GRAY       224
#define PAL_BG         225

/* Welcome screen logo palette entries (226-233 are free) */
#define PAL_LOGO_LGRAY  226
#define PAL_LOGO_MGRAY  227
#define PAL_LOGO_GRAY   228
#define PAL_LOGO_DGRAY  229
#define PAL_LOGO_VDGRAY 230
#define PAL_LOGO_RED    231
#define PAL_LOGO_CIRCLE 232
#define PAL_LOGO_SHADOW 233

static const uint8_t cube_levels[6] = {0, 51, 102, 153, 204, 255};

static uint16_t rgb565(uint8_t r, uint8_t g, uint8_t b) {
    return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);
}

static void setup_selector_palette(void) {
    uint32_t *pal = rgb565_palette_32[pal_write_idx];

    /* Entry 0: black */
    pal[0] = 0;

    /* 6x6x6 color cube */
    for (int r = 0; r < 6; r++)
        for (int g = 0; g < 6; g++)
            for (int b = 0; b < 6; b++) {
                int idx = PAL_CUBE_BASE + r * 36 + g * 6 + b;
                uint16_t c = rgb565(cube_levels[r], cube_levels[g], cube_levels[b]);
                pal[idx] = c | ((uint32_t)c << 16);
            }

    /* UI colors — NES cartridge style */
    uint16_t c;
    c = rgb565(0x88, 0x88, 0x88); pal[PAL_CART_BODY]  = c | ((uint32_t)c << 16);
    c = rgb565(0xA0, 0xA0, 0xA0); pal[PAL_CART_LIGHT] = c | ((uint32_t)c << 16);
    c = rgb565(0x60, 0x60, 0x60); pal[PAL_CART_DARK]  = c | ((uint32_t)c << 16);
    c = rgb565(0x18, 0x18, 0x20); pal[PAL_CART_LABEL] = c | ((uint32_t)c << 16);
    c = rgb565(0x70, 0x70, 0x70); pal[PAL_CART_RIDGE] = c | ((uint32_t)c << 16);
    c = rgb565(0x30, 0x30, 0x30); pal[PAL_CART_SLOT]  = c | ((uint32_t)c << 16);
    c = 0xFFFF;                    pal[PAL_WHITE]      = c | ((uint32_t)c << 16);
    c = rgb565(0x80, 0x80, 0x80); pal[PAL_GRAY]       = c | ((uint32_t)c << 16);
    c = rgb565(0x1A, 0x1A, 0x22); pal[PAL_BG]         = c | ((uint32_t)c << 16);

    pending_pal_idx = pal_write_idx;
    pal_write_idx ^= 1;
}

/* Map an RGB555 pixel to the nearest 6x6x6 cube palette index */
static uint8_t rgb555_to_pal(uint16_t p) {
    uint8_t r5 = (p >> 10) & 0x1F;
    uint8_t g5 = (p >> 5) & 0x1F;
    uint8_t b5 = p & 0x1F;
    /* Scale 0-31 to 0-5 (nearest cube level) */
    int ri = (r5 * 5 + 15) / 31;
    int gi = (g5 * 5 + 15) / 31;
    int bi = (b5 * 5 + 15) / 31;
    return (uint8_t)(PAL_CUBE_BASE + ri * 36 + gi * 6 + bi);
}

/* ─── Framebuffer helpers (double-buffered to avoid tearing) ──────── */

static uint8_t sel_backbuf[SCREEN_W * SCREEN_H];
static uint8_t *fb;       /* buffer being drawn to */
static uint8_t *fb_show;  /* buffer being displayed by Core 1 */

static inline void fb_pixel(int x, int y, uint8_t color) {
    if (x >= 0 && x < SCREEN_W && y >= 0 && y < SCREEN_H)
        fb[y * SCREEN_W + x] = color;
}

static void fb_fill(uint8_t color) {
    memset(fb, color, SCREEN_W * SCREEN_H);
}

static void fb_rect(int x, int y, int w, int h, uint8_t color) {
    for (int yy = y; yy < y + h && yy < SCREEN_H; yy++) {
        if (yy < 0) continue;
        int x0 = x < 0 ? 0 : x;
        int x1 = (x + w) > SCREEN_W ? SCREEN_W : (x + w);
        if (x0 < x1) memset(&fb[yy * SCREEN_W + x0], color, x1 - x0);
    }
}

static void fb_hline(int x, int y, int w, uint8_t color) {
    if (y < 0 || y >= SCREEN_H) return;
    int x0 = x < 0 ? 0 : x;
    int x1 = (x + w) > SCREEN_W ? SCREEN_W : (x + w);
    if (x0 < x1) memset(&fb[y * SCREEN_W + x0], color, x1 - x0);
}

static void fb_vline(int x, int y, int h, uint8_t color) {
    if (x < 0 || x >= SCREEN_W) return;
    for (int yy = y; yy < y + h && yy < SCREEN_H; yy++)
        if (yy >= 0) fb[yy * SCREEN_W + x] = color;
}

/* ─── 5x7 font ───────────────────────────────────────────────────── */

static const uint8_t glyphs[][7] = {
    [' '-' '] = {0,0,0,0,0,0,0},
    ['!'-' '] = {0x04,0x04,0x04,0x04,0x00,0x04,0x00},
    ['"'-' '] = {0x0A,0x0A,0x0A,0x00,0x00,0x00,0x00},
    ['#'-' '] = {0x0A,0x0A,0x1F,0x0A,0x1F,0x0A,0x0A},
    ['$'-' '] = {0x04,0x0F,0x14,0x0E,0x05,0x1E,0x04},
    ['%'-' '] = {0x19,0x1A,0x04,0x08,0x0B,0x13,0x00},
    ['&'-' '] = {0x0C,0x12,0x14,0x08,0x15,0x12,0x0D},
    ['\''-' '] = {0x04,0x04,0x00,0x00,0x00,0x00,0x00},
    ['('-' '] = {0x02,0x04,0x08,0x08,0x08,0x04,0x02},
    [')'-' '] = {0x08,0x04,0x02,0x02,0x02,0x04,0x08},
    ['*'-' '] = {0x00,0x04,0x15,0x0E,0x15,0x04,0x00},
    ['+'-' '] = {0x00,0x04,0x04,0x1F,0x04,0x04,0x00},
    [','-' '] = {0x00,0x00,0x00,0x00,0x0C,0x04,0x08},
    ['-'-' '] = {0x00,0x00,0x00,0x1F,0x00,0x00,0x00},
    ['.'-' '] = {0x00,0x00,0x00,0x00,0x00,0x0C,0x0C},
    ['/'-' '] = {0x01,0x02,0x04,0x08,0x10,0x00,0x00},
    ['0'-' '] = {0x0E,0x11,0x13,0x15,0x19,0x11,0x0E},
    ['1'-' '] = {0x04,0x0C,0x04,0x04,0x04,0x04,0x0E},
    ['2'-' '] = {0x0E,0x11,0x01,0x02,0x04,0x08,0x1F},
    ['3'-' '] = {0x1E,0x01,0x01,0x0E,0x01,0x01,0x1E},
    ['4'-' '] = {0x02,0x06,0x0A,0x12,0x1F,0x02,0x02},
    ['5'-' '] = {0x1F,0x10,0x10,0x1E,0x01,0x01,0x1E},
    ['6'-' '] = {0x0E,0x10,0x10,0x1E,0x11,0x11,0x0E},
    ['7'-' '] = {0x1F,0x01,0x02,0x04,0x08,0x08,0x08},
    ['8'-' '] = {0x0E,0x11,0x11,0x0E,0x11,0x11,0x0E},
    ['9'-' '] = {0x0E,0x11,0x11,0x0F,0x01,0x01,0x0E},
    [':'-' '] = {0x00,0x0C,0x0C,0x00,0x0C,0x0C,0x00},
    [';'-' '] = {0x00,0x0C,0x0C,0x00,0x0C,0x04,0x08},
    ['<'-' '] = {0x02,0x04,0x08,0x10,0x08,0x04,0x02},
    ['='-' '] = {0x00,0x00,0x1F,0x00,0x1F,0x00,0x00},
    ['>'-' '] = {0x08,0x04,0x02,0x01,0x02,0x04,0x08},
    ['?'-' '] = {0x0E,0x11,0x01,0x02,0x04,0x00,0x04},
    ['@'-' '] = {0x0E,0x11,0x17,0x15,0x17,0x10,0x0E},
    ['A'-' '] = {0x0E,0x11,0x11,0x1F,0x11,0x11,0x11},
    ['B'-' '] = {0x1E,0x11,0x11,0x1E,0x11,0x11,0x1E},
    ['C'-' '] = {0x0E,0x11,0x10,0x10,0x10,0x11,0x0E},
    ['D'-' '] = {0x1E,0x11,0x11,0x11,0x11,0x11,0x1E},
    ['E'-' '] = {0x1F,0x10,0x10,0x1E,0x10,0x10,0x1F},
    ['F'-' '] = {0x1F,0x10,0x10,0x1E,0x10,0x10,0x10},
    ['G'-' '] = {0x0E,0x11,0x10,0x17,0x11,0x11,0x0E},
    ['H'-' '] = {0x11,0x11,0x11,0x1F,0x11,0x11,0x11},
    ['I'-' '] = {0x1F,0x04,0x04,0x04,0x04,0x04,0x1F},
    ['J'-' '] = {0x07,0x02,0x02,0x02,0x12,0x12,0x0C},
    ['K'-' '] = {0x11,0x12,0x14,0x18,0x14,0x12,0x11},
    ['L'-' '] = {0x10,0x10,0x10,0x10,0x10,0x10,0x1F},
    ['M'-' '] = {0x11,0x1B,0x15,0x15,0x11,0x11,0x11},
    ['N'-' '] = {0x11,0x19,0x15,0x13,0x11,0x11,0x11},
    ['O'-' '] = {0x0E,0x11,0x11,0x11,0x11,0x11,0x0E},
    ['P'-' '] = {0x1E,0x11,0x11,0x1E,0x10,0x10,0x10},
    ['Q'-' '] = {0x0E,0x11,0x11,0x11,0x15,0x12,0x0D},
    ['R'-' '] = {0x1E,0x11,0x11,0x1E,0x14,0x12,0x11},
    ['S'-' '] = {0x0F,0x10,0x10,0x0E,0x01,0x01,0x1E},
    ['T'-' '] = {0x1F,0x04,0x04,0x04,0x04,0x04,0x04},
    ['U'-' '] = {0x11,0x11,0x11,0x11,0x11,0x11,0x0E},
    ['V'-' '] = {0x11,0x11,0x11,0x11,0x0A,0x0A,0x04},
    ['W'-' '] = {0x11,0x11,0x11,0x15,0x15,0x15,0x0A},
    ['X'-' '] = {0x11,0x0A,0x04,0x04,0x04,0x0A,0x11},
    ['Y'-' '] = {0x11,0x0A,0x04,0x04,0x04,0x04,0x04},
    ['Z'-' '] = {0x1F,0x02,0x04,0x08,0x10,0x10,0x1F},
    ['['-' '] = {0x0E,0x08,0x08,0x08,0x08,0x08,0x0E},
    ['\\'-' '] = {0x10,0x08,0x04,0x02,0x01,0x00,0x00},
    [']'-' '] = {0x0E,0x02,0x02,0x02,0x02,0x02,0x0E},
    ['^'-' '] = {0x04,0x0A,0x11,0x00,0x00,0x00,0x00},
    ['_'-' '] = {0x00,0x00,0x00,0x00,0x00,0x00,0x1F},
    ['{'-' '] = {0x02,0x04,0x04,0x08,0x04,0x04,0x02},
    ['|'-' '] = {0x04,0x04,0x04,0x04,0x04,0x04,0x04},
    ['}'-' '] = {0x08,0x04,0x04,0x02,0x04,0x04,0x08},
    ['~'-' '] = {0x00,0x00,0x08,0x15,0x02,0x00,0x00},
};

static void fb_char(int x, int y, char ch, uint8_t color) {
    int c = (unsigned char)ch;
    if (c >= 'a' && c <= 'z') c = c - 'a' + 'A';
    int idx = c - ' ';
    if (idx < 0 || idx >= (int)(sizeof(glyphs)/sizeof(glyphs[0]))) return;
    const uint8_t *g = glyphs[idx];
    for (int row = 0; row < 7; row++) {
        uint8_t bits = g[row];
        for (int col = 0; col < 5; col++) {
            if (bits & (1u << (4 - col)))
                fb_pixel(x + col, y + row, color);
        }
    }
}

static void fb_text(int x, int y, const char *s, uint8_t color) {
    for (; *s; s++) { fb_char(x, y, *s, color); x += 6; }
}

static void fb_text_center(int y, const char *s, uint8_t color) {
    int x = (SCREEN_W - (int)strlen(s) * 6) / 2;
    fb_text(x, y, s, color);
}

/* ─── CRC32 ───────────────────────────────────────────────────────── */

static uint32_t crc32_table[256];
static bool crc32_ready = false;

static void crc32_init(void) {
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t c = i;
        for (int j = 0; j < 8; j++)
            c = (c >> 1) ^ (0xEDB88320 & (-(c & 1)));
        crc32_table[i] = c;
    }
    crc32_ready = true;
}

static uint32_t crc32_file(FIL *fil, int skip) {
    if (!crc32_ready) crc32_init();
    uint32_t crc = 0xFFFFFFFF;
    uint8_t buf[512];
    f_lseek(fil, skip);
    while (1) {
        UINT br;
        if (f_read(fil, buf, sizeof(buf), &br) != FR_OK || br == 0) break;
        for (UINT i = 0; i < br; i++)
            crc = crc32_table[(crc ^ buf[i]) & 0xFF] ^ (crc >> 8);
    }
    return crc ^ 0xFFFFFFFF;
}

/* ─── ROM list (allocated in PSRAM to save SRAM) ──────────────────── */

#define MAX_ROMS 128

typedef struct {
    char filename[64];
    uint32_t crc;
    bool crc_valid;
} rom_entry_t;

/* Placed in PSRAM at 4MB offset (after images at 3MB) */
#define ROMLIST_PSRAM_BASE (0x11000000 + 4 * 1024 * 1024)
static rom_entry_t *rom_list;  /* -> PSRAM */
static int rom_count = 0;
static bool sd_mount_succeeded = false;

static int scan_roms(void) {
    rom_count = 0;
    DIR dir;
    if (f_opendir(&dir, "/nes") != FR_OK) return 0;
    FILINFO fno;
    while (f_readdir(&dir, &fno) == FR_OK && fno.fname[0] != '\0' && rom_count < MAX_ROMS) {
        if (fno.fattrib & AM_DIR) continue;
        size_t len = strlen(fno.fname);
        if (len < 4) continue;
        if ((fno.fname[len-4] != '.') ||
            (fno.fname[len-3] != 'n' && fno.fname[len-3] != 'N') ||
            (fno.fname[len-2] != 'e' && fno.fname[len-2] != 'E') ||
            (fno.fname[len-1] != 's' && fno.fname[len-1] != 'S'))
            continue;
        strncpy(rom_list[rom_count].filename, fno.fname, sizeof(rom_list[0].filename) - 1);
        rom_list[rom_count].crc_valid = false;
        rom_count++;
    }
    f_closedir(&dir);
    return rom_count;
}

static void ensure_crc(int idx) {
    if (rom_list[idx].crc_valid) return;
    char path[ROM_PATH_MAX];
    snprintf(path, sizeof(path), "/nes/%s", rom_list[idx].filename);
    FIL fil;
    if (f_open(&fil, path, FA_READ) == FR_OK) {
        rom_list[idx].crc = crc32_file(&fil, 16);
        rom_list[idx].crc_valid = true;
        f_close(&fil);
        printf("CRC32(%s) = %08lX\n", rom_list[idx].filename, (unsigned long)rom_list[idx].crc);
    }
}

/* ─── CRC cache on SD card ────────────────────────────────────────── */

#define CRC_CACHE_PATH "/nes/.crc_cache"
#define LAST_ROM_PATH  "/nes/.last_rom"

static void load_crc_cache(void) {
    FIL fil;
    if (f_open(&fil, CRC_CACHE_PATH, FA_READ) != FR_OK) return;
    char line[128];
    while (f_gets(line, sizeof(line), &fil)) {
        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq = '\0';
        uint32_t crc = 0;
        for (const char *p = eq + 1; *p && *p != '\n' && *p != '\r'; p++) {
            crc <<= 4;
            if (*p >= '0' && *p <= '9') crc |= (*p - '0');
            else if (*p >= 'A' && *p <= 'F') crc |= (*p - 'A' + 10);
            else if (*p >= 'a' && *p <= 'f') crc |= (*p - 'a' + 10);
        }
        for (int i = 0; i < rom_count; i++) {
            if (strcmp(rom_list[i].filename, line) == 0) {
                rom_list[i].crc = crc;
                rom_list[i].crc_valid = true;
                break;
            }
        }
    }
    f_close(&fil);
}

static void save_crc_cache(void) {
    FIL fil;
    if (f_open(&fil, CRC_CACHE_PATH, FA_WRITE | FA_CREATE_ALWAYS) != FR_OK) return;
    for (int i = 0; i < rom_count; i++) {
        if (!rom_list[i].crc_valid) continue;
        char line[128];
        snprintf(line, sizeof(line), "%s=%08lX\n",
                 rom_list[i].filename, (unsigned long)rom_list[i].crc);
        UINT bw;
        f_write(&fil, line, strlen(line), &bw);
    }
    f_close(&fil);
}

/* ─── Last selected ROM persistence ───────────────────────────────── */

static int last_selected_rom = 0;

static void load_last_rom(void) {
    FIL fil;
    if (f_open(&fil, LAST_ROM_PATH, FA_READ) != FR_OK) return;
    char name[64];
    if (f_gets(name, sizeof(name), &fil)) {
        /* Strip trailing newline/CR */
        size_t len = strlen(name);
        while (len > 0 && (name[len-1] == '\n' || name[len-1] == '\r'))
            name[--len] = '\0';
        for (int i = 0; i < rom_count; i++) {
            if (strcmp(rom_list[i].filename, name) == 0) {
                last_selected_rom = i;
                break;
            }
        }
    }
    f_close(&fil);
}

static void save_last_rom(int selected) {
    FIL fil;
    if (f_open(&fil, LAST_ROM_PATH, FA_WRITE | FA_CREATE_ALWAYS) != FR_OK) return;
    f_puts(rom_list[selected].filename, &fil);
    f_puts("\n", &fil);
    f_close(&fil);
}

/* ─── Metadata (titles pre-loaded, images loaded on-the-fly) ──────── */

typedef struct {
    char title[64];    /* game title from metadata, or empty */
    char desc[256];    /* game description, truncated */
    char year[8];      /* release year, e.g. "1990" */
    char genre[48];    /* genre string */
    char players[8];   /* number of players, e.g. "1" or "2" */
} rom_meta_t;

/* Also in PSRAM (after rom_list) */
#define ROMMETA_PSRAM_BASE (ROMLIST_PSRAM_BASE + MAX_ROMS * sizeof(rom_entry_t))
static rom_meta_t *rom_meta;  /* -> PSRAM */

/* Single image buffer in SRAM — avoids PSRAM/QMI bus contention with HDMI */
#define IMG_BUF_BYTES (40 * 1024)
static uint8_t img_sram_buf[IMG_BUF_BYTES] __attribute__((aligned(4)));
static uint16_t cur_img_w, cur_img_h;
static uint16_t *cur_img_pixels;  /* -> img_sram_buf or NULL */
static int cur_img_idx = -1;

/* Extract text between <tag> and </tag> from buf into dst (max dst_size-1 chars) */
static void extract_xml_tag(const char *buf, const char *tag, char *dst, int dst_size) {
    dst[0] = '\0';
    char open[32], close[32];
    snprintf(open, sizeof(open), "<%s>", tag);
    snprintf(close, sizeof(close), "</%s>", tag);
    const char *start = strstr(buf, open);
    if (!start) return;
    start += strlen(open);
    const char *end = strstr(start, close);
    if (!end) return;
    int src_len = end - start;
    /* Copy while collapsing newlines and runs of whitespace into single spaces */
    int di = 0;
    bool prev_space = false;
    int max_out = dst_size - 4;  /* reserve room for "..." */
    int si;
    for (si = 0; si < src_len && di < max_out; si++) {
        char c = start[si];
        if (c == '\n' || c == '\r' || c == '\t') c = ' ';
        if (c == ' ') {
            if (!prev_space && di > 0) { dst[di++] = ' '; prev_space = true; }
        } else {
            dst[di++] = c;
            prev_space = false;
        }
    }
    /* If we didn't consume all source chars, text was truncated */
    if (si < src_len) {
        while (di > 0 && dst[di - 1] == ' ') di--;
        dst[di++] = '.'; dst[di++] = '.'; dst[di++] = '.';
    }
    dst[di] = '\0';
}

static void load_rom_title(int idx) {
    rom_meta[idx].title[0] = '\0';
    rom_meta[idx].desc[0] = '\0';
    rom_meta[idx].year[0] = '\0';
    rom_meta[idx].genre[0] = '\0';
    rom_meta[idx].players[0] = '\0';

    if (!rom_list[idx].crc_valid) return;
    uint32_t crc = rom_list[idx].crc;
    char hex_char = "0123456789ABCDEF"[(crc >> 28) & 0xF];

    char path[128];
    snprintf(path, sizeof(path), "/nes/metadata/descr/%c/%08lX.txt", hex_char, (unsigned long)crc);
    FIL fil;
    if (f_open(&fil, path, FA_READ) == FR_OK) {
        /* Read enough for all fields — metadata files are typically 0.5-1.5KB */
        char buf[1536];
        UINT br;
        if (f_read(&fil, buf, sizeof(buf) - 1, &br) == FR_OK) {
            buf[br] = '\0';
            extract_xml_tag(buf, "name", rom_meta[idx].title, sizeof(rom_meta[idx].title));
            extract_xml_tag(buf, "desc", rom_meta[idx].desc, sizeof(rom_meta[idx].desc));
            extract_xml_tag(buf, "genre", rom_meta[idx].genre, sizeof(rom_meta[idx].genre));
            extract_xml_tag(buf, "players", rom_meta[idx].players, sizeof(rom_meta[idx].players));

            /* Extract year from releasedate (format: YYYYMMDDTHHMMSS) */
            char datestr[32];
            extract_xml_tag(buf, "releasedate", datestr, sizeof(datestr));
            if (datestr[0] && strlen(datestr) >= 4) {
                memcpy(rom_meta[idx].year, datestr, 4);
                rom_meta[idx].year[4] = '\0';
            }
        }
        f_close(&fil);
    }
}

/* Load cover art for a single ROM from SD into the shared image buffer.
 * SD must already be mounted by the caller. */
static void load_rom_image(int idx) {
    cur_img_pixels = NULL;
    cur_img_w = 0;
    cur_img_h = 0;
    cur_img_idx = idx;

    if (!rom_list[idx].crc_valid) return;
    uint32_t crc = rom_list[idx].crc;
    char hex_char = "0123456789ABCDEF"[(crc >> 28) & 0xF];

    char path[128];
    snprintf(path, sizeof(path), "/nes/metadata/Images/160/%c/%08lX.555", hex_char, (unsigned long)crc);

    FIL fil;
    if (f_open(&fil, path, FA_READ) != FR_OK) return;

    uint8_t hdr[4];
    UINT br;
    if (f_read(&fil, hdr, 4, &br) != FR_OK || br != 4) { f_close(&fil); return; }

    uint16_t w = hdr[0] | (hdr[1] << 8);
    uint16_t h = hdr[2] | (hdr[3] << 8);
    if (w == 0 || w > 320 || h == 0 || h > 240) { f_close(&fil); return; }

    uint32_t data_size = (uint32_t)w * h * 2;
    if (data_size > IMG_BUF_BYTES) { f_close(&fil); return; }

    if (f_read(&fil, img_sram_buf, data_size, &br) == FR_OK && br == data_size) {
        cur_img_pixels = (uint16_t *)img_sram_buf;
        cur_img_w = w;
        cur_img_h = h;
    }
    f_close(&fil);
}

/* ─── NES-style cartridge rendering (pixel-perfect) ──────────────── */

/*
 * NES cart layout (from reference pixel art):
 *   ┌─┬──────┬──────────────┬─┐   ← top edge with corner cuts
 *   │ │RIDGES│              │ │
 *   │ │      │   LABEL      │ │   ← ridges on LEFT, label on RIGHT
 *   │ │      │   (image)    │ │
 *   │ │      │              │ │
 *   │ │      ├──────────────┤ │
 *   │ │      │    ▽         │ │   ← arrow below label
 *   ├─┴──────┴──────────────┴─┤
 *   │        BOTTOM BASE       │   ← connector base
 *   └─────────────────────────-┘
 */

#define CART_W    134
#define CART_H    158
#define CART_X    ((SCREEN_W - CART_W) / 2)
#define CART_Y    10

/* Left outer strip (thin edge) */
#define LSTRIP_W  4

/* Ridge panel (left side, inside the strip) */
#define RIDGE_X   (CART_X + LSTRIP_W + 1)
#define RIDGE_W   30
#define RIDGE_TOP (CART_Y + 4)

/* Label area (right side) */
#define LABEL_X   (RIDGE_X + RIDGE_W + 3)
#define LABEL_Y   (CART_Y + 5)
#define LABEL_W   (CART_X + CART_W - LSTRIP_W - 1 - LABEL_X)

/* Bottom base section */
#define BASE_H    24
#define BASE_Y    (CART_Y + CART_H - BASE_H)

/* Label height goes down to above the base */
#define LABEL_H   (BASE_Y - 22 - LABEL_Y)

/* Arrow section (between label bottom and base) */
#define ARROW_Y   (LABEL_Y + LABEL_H + 6)

/* ─── Cartridge body drawing (position-parameterized) ─────────────── */

static void draw_cart_at(int cx, int cy, int rom_idx) {
    int base_y  = cy + CART_H - BASE_H;
    int ridge_x = cx + LSTRIP_W + 1;
    int label_x = ridge_x + RIDGE_W + 3;
    int label_y = cy + 5;
    int label_w = cx + CART_W - LSTRIP_W - 1 - label_x;
    int label_h = base_y - 22 - label_y;
    int arrow_y = label_y + label_h + 6;

    /* Main body */
    fb_rect(cx, cy, CART_W, CART_H, PAL_CART_BODY);

    /* Corner cuts */
    fb_rect(cx, cy, 3, 2, PAL_BG);
    fb_rect(cx + CART_W - 3, cy, 3, 2, PAL_BG);
    fb_rect(cx, cy + CART_H - 15, 9, 15, PAL_BG);
    fb_rect(cx + CART_W - 9, cy + CART_H - 15, 9, 15, PAL_BG);

    /* Outer edges */
    fb_hline(cx + 3, cy, CART_W - 6, PAL_CART_LIGHT);
    fb_vline(cx, cy + 2, CART_H - 17, PAL_CART_LIGHT);
    fb_vline(cx + CART_W - 1, cy + 2, CART_H - 17, PAL_CART_DARK);
    fb_hline(cx + 9, cy + CART_H - 1, CART_W - 18, PAL_CART_DARK);

    /* Left outer strip */
    fb_vline(cx + LSTRIP_W, cy + 2, CART_H - BASE_H - 2, PAL_CART_DARK);

    /* Ridge panel */
    int ridge_top  = cy + 4;
    int ridge_bot  = base_y - 4;
    int ridge_count = (ridge_bot - ridge_top) / 5;
    for (int i = 0; i < ridge_count; i++) {
        int ry = ridge_top + i * 5;
        fb_hline(ridge_x, ry,     RIDGE_W, PAL_CART_RIDGE);
        fb_hline(ridge_x, ry + 1, RIDGE_W, PAL_CART_DARK);
        fb_hline(ridge_x, ry + 2, RIDGE_W, PAL_CART_BODY);
    }

    /* Separator */
    fb_vline(label_x - 2, cy + 3, CART_H - BASE_H - 5, PAL_CART_DARK);
    fb_vline(label_x - 1, cy + 3, CART_H - BASE_H - 5, PAL_CART_LIGHT);

    /* Label area — use lighter fill when no cover art */
    bool has_img = (cur_img_pixels && cur_img_idx == rom_idx);
    fb_rect(label_x, label_y, label_w, label_h, has_img ? PAL_CART_LABEL : PAL_CART_DARK);
    fb_hline(label_x - 1, label_y - 1, label_w + 2, PAL_CART_DARK);
    fb_hline(label_x - 1, label_y + label_h, label_w + 2, PAL_CART_DARK);
    fb_vline(label_x - 1, label_y - 1, label_h + 2, PAL_CART_DARK);
    fb_vline(label_x + label_w, label_y - 1, label_h + 2, PAL_CART_DARK);

    /* Cover art or placeholder border */
    if (cur_img_pixels && cur_img_idx == rom_idx) {
        int iw = cur_img_w;
        int ih = cur_img_h;
        if (iw > label_w) { ih = ih * label_w / iw; iw = label_w; }
        if (ih > label_h) ih = label_h;
        int ix = label_x + (label_w - iw) / 2;
        int iy = label_y;
        for (int y = 0; y < ih; y++) {
            int sy = y * cur_img_h / ih;
            for (int x = 0; x < iw; x++) {
                int sx = x * cur_img_w / iw;
                uint16_t px = cur_img_pixels[sy * cur_img_w + sx];
                fb_pixel(ix + x, iy + y, rgb555_to_pal(px));
            }
        }
    } else {
        /* No image: draw a dark gray inset border */
        int m = 4;  /* margin inside label */
        fb_hline(label_x + m, label_y + m, label_w - m * 2, PAL_CART_RIDGE);
        fb_hline(label_x + m, label_y + label_h - m - 1, label_w - m * 2, PAL_CART_RIDGE);
        fb_vline(label_x + m, label_y + m, label_h - m * 2, PAL_CART_RIDGE);
        fb_vline(label_x + label_w - m - 1, label_y + m, label_h - m * 2, PAL_CART_RIDGE);
    }

    /* Arrow */
    int ax = label_x + label_w / 2;
    for (int row = 0; row < 5; row++)
        fb_hline(ax - (4 - row), arrow_y + row, (4 - row) * 2 + 1, PAL_CART_DARK);

    /* Base divider */
    fb_hline(cx + 1, base_y, CART_W - 2, PAL_CART_DARK);
    fb_hline(cx + 1, base_y + 1, CART_W - 2, PAL_CART_LIGHT);
}

/* ─── Info panel state machine ─────────────────────────────────────── */

typedef enum {
    INFO_HIDDEN,
    INFO_SLIDING_IN,
    INFO_SHOWN,
    INFO_SLIDING_OUT,
} info_state_t;

#define INFO_ANIM_FRAMES 10
static info_state_t info_state = INFO_HIDDEN;
static int info_anim_frame = 0;

/* Cart X when info is fully shown: only 20% visible on left */
#define INFO_CART_X  (-(CART_W * 80 / 100))

static int info_ease(int frame) {
    int t = frame * 256 / INFO_ANIM_FRAMES;
    return t * (512 - t) / 256;
}

static int info_cart_x(void) {
    switch (info_state) {
    case INFO_HIDDEN:
        return CART_X;
    case INFO_SLIDING_IN: {
        int p = info_ease(info_anim_frame);
        return CART_X + (INFO_CART_X - CART_X) * p / 256;
    }
    case INFO_SHOWN:
        return INFO_CART_X;
    case INFO_SLIDING_OUT: {
        int p = info_ease(info_anim_frame);
        return INFO_CART_X + (CART_X - INFO_CART_X) * p / 256;
    }
    }
    return CART_X;
}

/* ─── Word-wrapped text drawing ───────────────────────────────────── */

static int fb_text_wrap(int x, int y, int max_w, const char *s, uint8_t color, int max_lines) {
    int char_w = 6;
    int line_h = 9;
    int max_chars = max_w / char_w;
    if (max_chars < 1) max_chars = 1;
    int line = 0;
    const char *p = s;
    while (*p && line < max_lines) {
        int len = (int)strlen(p);
        if (len <= max_chars) {
            fb_text(x, y, p, color);
            y += line_h;
            break;
        }
        /* Find last space within max_chars */
        int brk = max_chars;
        for (int i = max_chars; i > 0; i--) {
            if (p[i] == ' ') { brk = i; break; }
        }
        char tmp[64];
        int copy = brk;
        if (copy > 63) copy = 63;
        memcpy(tmp, p, copy);
        tmp[copy] = '\0';
        /* Last allowed line with more text: add "..." */
        if (line == max_lines - 1 && (int)strlen(p) > brk) {
            if (copy > 3) copy -= 3;
            tmp[copy] = '.'; tmp[copy+1] = '.'; tmp[copy+2] = '.'; tmp[copy+3] = '\0';
        }
        fb_text(x, y, tmp, color);
        y += line_h;
        line++;
        p += brk;
        while (*p == ' ') p++;
    }
    return y;
}

/* ─── UI text (fixed position, drawn separately from cart) ────────── */

static void draw_info_panel(int selected) {
    /* Info text area starts after the cartridge's visible sliver */
    int text_x = INFO_CART_X + CART_W + 8;
    int text_w = SCREEN_W - text_x - 6;
    int ty = CART_Y + 4;

    /* Title */
    const char *title = rom_meta[selected].title[0] ? rom_meta[selected].title : rom_list[selected].filename;
    char dt[40];
    int max_c = text_w / 6;
    if (max_c > 39) max_c = 39;
    int tlen = (int)strlen(title);
    if (tlen > max_c) {
        int cut = max_c > 3 ? max_c - 3 : 0;
        memcpy(dt, title, cut);
        dt[cut] = '.'; dt[cut+1] = '.'; dt[cut+2] = '.'; dt[cut+3] = '\0';
    } else {
        memcpy(dt, title, tlen); dt[tlen] = '\0';
    }
    fb_text(text_x, ty, dt, PAL_WHITE);
    ty += 14;

    /* Separator line */
    fb_hline(text_x, ty, text_w, PAL_CART_RIDGE);
    ty += 6;

    /* Year */
    if (rom_meta[selected].year[0]) {
        char line[48];
        snprintf(line, sizeof(line), "YEAR: %s", rom_meta[selected].year);
        fb_text(text_x, ty, line, PAL_GRAY);
        ty += 12;
    }

    /* Players */
    if (rom_meta[selected].players[0]) {
        char line[48];
        snprintf(line, sizeof(line), "PLAYERS: %s", rom_meta[selected].players);
        fb_text(text_x, ty, line, PAL_GRAY);
        ty += 12;
    }

    /* Genre */
    if (rom_meta[selected].genre[0]) {
        char gline[48];
        int glen = (int)strlen(rom_meta[selected].genre);
        int gmax = text_w / 6;
        if (gmax > 47) gmax = 47;
        if (glen > gmax) {
            memcpy(gline, rom_meta[selected].genre, gmax - 3);
            gline[gmax - 3] = '.'; gline[gmax - 2] = '.'; gline[gmax - 1] = '.';
            gline[gmax] = '\0';
        } else {
            strncpy(gline, rom_meta[selected].genre, 47);
            gline[47] = '\0';
        }
        fb_text(text_x, ty, gline, PAL_GRAY);
        ty += 12;
    }

    /* Description */
    if (rom_meta[selected].desc[0]) {
        ty += 4;
        int max_desc_lines = (CART_Y + CART_H + 18 - ty) / 9;
        if (max_desc_lines > 14) max_desc_lines = 14;
        if (max_desc_lines > 0)
            fb_text_wrap(text_x, ty, text_w, rom_meta[selected].desc, PAL_GRAY, max_desc_lines);
    }
}

static void draw_selector_text(int selected) {
    bool info_visible = (info_state == INFO_SHOWN || info_state == INFO_SLIDING_IN
                         || info_state == INFO_SLIDING_OUT);

    if (!info_visible) {
        const char *title = rom_meta[selected].title[0] ? rom_meta[selected].title : rom_list[selected].filename;
        char dt[40];
        int max_c = (SCREEN_W - 20) / 6;
        if (max_c > 39) max_c = 39;
        int tlen = (int)strlen(title);
        if (tlen > max_c) {
            int cut = max_c > 3 ? max_c - 3 : 0;
            memcpy(dt, title, cut);
            dt[cut] = '.'; dt[cut+1] = '.'; dt[cut+2] = '.'; dt[cut+3] = '\0';
        } else {
            memcpy(dt, title, tlen); dt[tlen] = '\0';
        }
        fb_text_center(CART_Y + CART_H + 16, dt, PAL_WHITE);

        char counter[16];
        snprintf(counter, sizeof(counter), "%d / %d", selected + 1, rom_count);
        fb_text_center(CART_Y + CART_H + 30, counter, PAL_GRAY);
    }

    /* Hint text */
    if (info_state == INFO_SHOWN)
        fb_text_center(SCREEN_H - 16, "< DOWN >   A: START", PAL_GRAY);
    else if (info_state == INFO_HIDDEN)
        fb_text_center(SCREEN_H - 16, "< LEFT/RIGHT/UP >   A: START", PAL_GRAY);
}

/* ─── Animation ───────────────────────────────────────────────────── */

/* Idle float: gentle 2px sine bob, 36 frames (~0.6s period). */
static int bounce_offset(uint32_t frame) {
    int t = (int)(frame % 36);
    if (t < 6) return 0;
    if (t < 9) return 1;
    if (t < 15) return 2;
    if (t < 18) return 1;
    if (t < 24) return 0;
    if (t < 27) return -1;
    if (t < 33) return -2;
    return -1;
}

/* Scroll animation */
#define SCROLL_FRAMES  8
static int scroll_dir   = 0;  /* -1 = left, +1 = right, 0 = idle */
static int scroll_frame = 0;
static int scroll_from  = 0;  /* ROM index of outgoing cartridge */

/* Ease-out: t*(2-t) mapped to 0..256 for t in 0..SCROLL_FRAMES */
static int ease_out(int frame) {
    int t = frame * 256 / SCROLL_FRAMES;
    return t * (512 - t) / 256;
}

static void draw_scene(int selected, int bounce_idx) {
    fb_fill(PAL_BG);

    if (scroll_dir != 0 && scroll_frame < SCROLL_FRAMES) {
        /* Scroll transition: old cart flies out, new cart slides in */
        int progress = ease_out(scroll_frame);  /* 0..256 */
        int travel = (SCREEN_W / 2 + CART_W);

        int out_x = CART_X + (-scroll_dir * travel * progress / 256);
        int in_x  = CART_X + (scroll_dir * travel * (256 - progress) / 256);

        draw_cart_at(out_x, CART_Y, scroll_from);
        draw_cart_at(in_x, CART_Y, selected);
    } else if (info_state != INFO_HIDDEN) {
        /* Info panel: cart slides to the left, no bounce */
        int cx = info_cart_x();
        draw_cart_at(cx, CART_Y, selected);
        if (info_state == INFO_SHOWN)
            draw_info_panel(selected);
    } else {
        /* Idle: gentle float */
        int by = bounce_offset(bounce_idx);
        draw_cart_at(CART_X, CART_Y + by, selected);
    }

    draw_selector_text(selected);
}

/* ─── Input ───────────────────────────────────────────────────────── */

#define BTN_LEFT  0x01
#define BTN_RIGHT 0x02
#define BTN_A     0x04
#define BTN_START 0x08
#define BTN_UP    0x10
#define BTN_DOWN  0x20

static int read_selector_buttons(void) {
    nespad_read();
    ps2kbd_tick();
    int buttons = 0;
    uint32_t pad = nespad_state | nespad_state2;
    if (pad & DPAD_LEFT)  buttons |= BTN_LEFT;
    if (pad & DPAD_RIGHT) buttons |= BTN_RIGHT;
    if (pad & DPAD_UP)    buttons |= BTN_UP;
    if (pad & DPAD_DOWN)  buttons |= BTN_DOWN;
    if (pad & DPAD_A)     buttons |= BTN_A;
    if (pad & DPAD_START) buttons |= BTN_START;
    uint16_t kbd = ps2kbd_get_state();
#ifdef USB_HID_ENABLED
    kbd |= usbhid_get_kbd_state();
#endif
    if (kbd & KBD_STATE_LEFT)  buttons |= BTN_LEFT;
    if (kbd & KBD_STATE_RIGHT) buttons |= BTN_RIGHT;
    if (kbd & KBD_STATE_UP)    buttons |= BTN_UP;
    if (kbd & KBD_STATE_DOWN)  buttons |= BTN_DOWN;
    if (kbd & KBD_STATE_A)     buttons |= BTN_A;
    if (kbd & KBD_STATE_START) buttons |= BTN_START;
#ifdef USB_HID_ENABLED
    if (usbhid_gamepad_connected()) {
        usbhid_gamepad_state_t gp;
        usbhid_get_gamepad_state(&gp);
        if (gp.dpad & 0x01) buttons |= BTN_UP;
        if (gp.dpad & 0x02) buttons |= BTN_DOWN;
        if (gp.dpad & 0x04) buttons |= BTN_LEFT;
        if (gp.dpad & 0x08) buttons |= BTN_RIGHT;
        if (gp.buttons & 0x01) buttons |= BTN_A;
        if (gp.buttons & 0x40) buttons |= BTN_START;
    }
#endif
    return buttons;
}

/* ─── Main selector loop ──────────────────────────────────────────── */

static void selector_wait_vsync(void) {
    while (pending_pixels != NULL) {
#ifdef USB_HID_ENABLED
        usbhid_task();
#endif
        __asm volatile ("wfe");
    }
    vsync_flag = 0;
}

/* ─── Preload: ALL SD access before HDMI starts ───────────────────── */

/* On-demand ROM loading: single ROM loaded to PSRAM base on selection */
#define ROM_PSRAM_BASE   0x11000000
#define ROM_PSRAM_MAX    (2 * 1024 * 1024)

static int selected_rom_idx = 0;

void *rom_selector_get_rom_data(void) {
    return (void *)ROM_PSRAM_BASE;
}

static void set_rom_name(const char *filename) {
    extern char g_rom_name[64];
    size_t nlen = strlen(filename);
    if (nlen >= 4) nlen -= 4;
    if (nlen >= 64) nlen = 63;
    memcpy(g_rom_name, filename, nlen);
    g_rom_name[nlen] = '\0';
}

int rom_selector_preload(long *out_rom_size) {
    *out_rom_size = 0;
    rom_list = (rom_entry_t *)ROMLIST_PSRAM_BASE;
    rom_meta = (rom_meta_t *)ROMMETA_PSRAM_BASE;
    memset(rom_list, 0, MAX_ROMS * sizeof(rom_entry_t));
    memset(rom_meta, 0, MAX_ROMS * sizeof(rom_meta_t));

    static FATFS sel_fs;
    if (f_mount(&sel_fs, "", 1) != FR_OK) {
        sd_mount_succeeded = false;
        return 0;
    }
    sd_mount_succeeded = true;

    int count = scan_roms();
    printf("ROM selector: found %d ROMs\n", count);
    if (count == 0) { f_unmount(""); return 0; }

    /* Load CRC cache from SD — avoids recomputing on every boot */
    load_crc_cache();

    /* Compute CRCs only for files not found in the cache */
    bool cache_dirty = false;
    for (int i = 0; i < rom_count; i++) {
        if (!rom_list[i].crc_valid) {
            ensure_crc(i);
            cache_dirty = true;
        }
    }
    if (cache_dirty) save_crc_cache();

    /* Load game titles from metadata */
    for (int i = 0; i < rom_count; i++)
        load_rom_title(i);

    /* Remember which ROM was last selected */
    load_last_rom();

    f_unmount("");

    /* Flush PSRAM metadata so it survives xip_cache_invalidate_all() */
    xip_cache_clean_all();

    return count;
}

/* ─── Show: images loaded from SD on-the-fly ──────────────────────── */

bool rom_selector_show(long *out_rom_size) {
    /* Double buffer: draw to one buffer while Core 1 displays the other */
    fb = test_pixels;
    fb_show = sel_backbuf;
    setup_selector_palette();

    /* Mount SD once for the entire selector session to avoid
     * repeated disk_initialize/PIO reinit on every image load. */
    static FATFS show_fs;
    bool sd_ok = (f_mount(&show_fs, "", 1) == FR_OK);

    int selected = last_selected_rom;
    int prev_buttons = read_selector_buttons();  /* ignore held buttons from previous screen */
    uint32_t hold_counter = 0;
    uint32_t frame_count = 0;
    cur_img_idx = -1;
    scroll_dir = 0;
    scroll_frame = 0;
    info_state = INFO_HIDDEN;
    info_anim_frame = 0;

    /* Load cover art for the initial selection */
    if (sd_ok) load_rom_image(selected);

    while (1) {
        selector_wait_vsync();

        /* Always redraw: bounce animation runs continuously */
        draw_scene(selected, frame_count);
        /* Swap double buffers */
        uint8_t *tmp = fb;
        fb = fb_show;
        fb_show = tmp;

        audio_fill_silence(SAMPLE_RATE / 60);
        pending_pitch = SCREEN_W;
        pending_pixels = fb_show;

        frame_count++;

        /* Advance scroll animation */
        if (scroll_dir != 0) {
            scroll_frame++;
            if (scroll_frame >= SCROLL_FRAMES) {
                scroll_dir = 0;
                scroll_frame = 0;
            }
        }

        /* Advance info panel animation */
        if (info_state == INFO_SLIDING_IN) {
            info_anim_frame++;
            if (info_anim_frame >= INFO_ANIM_FRAMES) {
                info_state = INFO_SHOWN;
                info_anim_frame = 0;
            }
        } else if (info_state == INFO_SLIDING_OUT) {
            info_anim_frame++;
            if (info_anim_frame >= INFO_ANIM_FRAMES) {
                info_state = INFO_HIDDEN;
                info_anim_frame = 0;
            }
        }

        /* Input happens after posting */
        int buttons = read_selector_buttons();
        int pressed = buttons & ~prev_buttons;
        if (buttons != 0 && buttons == prev_buttons) {
            hold_counter++;
            if (hold_counter > 20 && (hold_counter % 5) == 0)
                pressed = buttons;
        } else {
            hold_counter = 0;
        }
        prev_buttons = buttons;

        /* Info panel: UP opens, DOWN closes */
        if (info_state == INFO_HIDDEN && scroll_dir == 0 && (pressed & BTN_UP)) {
            info_state = INFO_SLIDING_IN;
            info_anim_frame = 0;
        }
        if ((info_state == INFO_SHOWN) && (pressed & BTN_DOWN)) {
            info_state = INFO_SLIDING_OUT;
            info_anim_frame = 0;
        }

        /* Only accept navigation/selection when info is hidden and not mid-scroll */
        bool can_navigate = (scroll_dir == 0 && info_state == INFO_HIDDEN);

        if (can_navigate) {
            if (pressed & BTN_LEFT) {
                scroll_from = selected;
                selected = (selected - 1 + rom_count) % rom_count;
                scroll_dir = -1;
                scroll_frame = 0;
                if (sd_ok) load_rom_image(selected);
            }
            if (pressed & BTN_RIGHT) {
                scroll_from = selected;
                selected = (selected + 1) % rom_count;
                scroll_dir = 1;
                scroll_frame = 0;
                if (sd_ok) load_rom_image(selected);
            }
        }

        if (pressed & (BTN_A | BTN_START)) {
            /* If info panel is open, close it first before starting */
            if (info_state != INFO_HIDDEN) {
                info_state = INFO_SLIDING_OUT;
                info_anim_frame = 0;
                /* Animate the slide-out, then proceed to load */
                while (info_state == INFO_SLIDING_OUT) {
                    selector_wait_vsync();
                    draw_scene(selected, frame_count);
                    uint8_t *tmp2 = fb; fb = fb_show; fb_show = tmp2;
                    audio_fill_silence(SAMPLE_RATE / 60);
                    pending_pitch = SCREEN_W;
                    pending_pixels = fb_show;
                    frame_count++;
                    info_anim_frame++;
                    if (info_anim_frame >= INFO_ANIM_FRAMES) {
                        info_state = INFO_HIDDEN;
                        info_anim_frame = 0;
                    }
                }
            }

            /* Wait for any scroll to finish too */
            if (scroll_dir != 0) continue;

            /* Load ROM from SD into PSRAM on demand */
            long loaded_size = 0;
            if (sd_ok) {
                char rpath[ROM_PATH_MAX];
                snprintf(rpath, sizeof(rpath), "/nes/%s",
                         rom_list[selected].filename);
                FIL rfil;
                if (f_open(&rfil, rpath, FA_READ) == FR_OK) {
                    FSIZE_t rfsz = f_size(&rfil);
                    if (rfsz >= 16 && rfsz <= ROM_PSRAM_MAX) {
                        uint8_t *dst = (uint8_t *)0x15000000;
                        UINT rbr;
                        if (f_read(&rfil, dst, (UINT)rfsz, &rbr) == FR_OK
                            && rbr == (UINT)rfsz) {
                            loaded_size = (long)rfsz;
                        }
                    }
                    f_close(&rfil);
                }
            }
            if (loaded_size <= 0) {
                printf("Selected: %s — load FAILED\n",
                       rom_list[selected].filename);
                fb_fill(PAL_BG);
                fb_text_center(SCREEN_H / 2 - 4, "ROM FAILED TO LOAD", PAL_WHITE);
                fb_text_center(SCREEN_H / 2 + 10, "CHECK FILE ON SD CARD", PAL_GRAY);
                uint8_t *tmp2 = fb; fb = fb_show; fb_show = tmp2;
                selector_wait_vsync();
                audio_fill_silence(SAMPLE_RATE / 60);
                pending_pitch = SCREEN_W;
                pending_pixels = fb_show;
                for (int i = 0; i < 120; i++) {
                    selector_wait_vsync();
                    audio_fill_silence(SAMPLE_RATE / 60);
                    pending_pitch = SCREEN_W;
                    pending_pixels = fb_show;
                }
                continue;
            }
            selected_rom_idx = selected;
            last_selected_rom = selected;
            set_rom_name(rom_list[selected].filename);
            *out_rom_size = loaded_size;
            printf("Selected: %s (%ld bytes)\n",
                   rom_list[selected].filename, loaded_size);
            save_last_rom(selected);
            f_unmount("");
            return true;
        }
    }
}

/* ─── SD card status ──────────────────────────────────────────────── */

bool rom_selector_sd_ok(void) {
    return sd_mount_succeeded;
}

/* ─── NES controller pixel art (26x12) ────────────────────────────── */

/* Extracted from gridded reference image via Python/PIL. Palette:
 * 0=transparent 1=black 2=white 3=#CBCBC9 4=#BDBDBB
 * 5=#8F8F8F 6=#716F70 7=#3F3F3F 8=#E50011(red) */
#define LOGO_W 26
#define LOGO_H 12

static const uint8_t nes_logo[LOGO_H][LOGO_W] = {
    {4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4}, /*  0: body top */
    {4,1,1,1,1,1,1,1,4,4,4,4,4,4,4,4,1,1,1,1,1,1,1,1,1,4}, /*  1: border + center */
    {4,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,8,8,8,8,8,1,1,4}, /*  2: border + red accent */
    {4,1,1,1,1,1,1,1,4,4,4,4,4,4,4,4,1,1,1,1,1,1,1,1,1,4}, /*  3: border */
    {4,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,4}, /*  4: inner border */
    {4,1,1,4,4,4,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,4}, /*  5: d-pad top */
    {4,1,4,4,1,4,4,1,4,4,4,4,4,4,4,4,1,4,4,4,4,4,4,4,1,4}, /*  6: d-pad + body */
    {4,1,4,1,1,1,4,1,4,1,1,4,4,1,1,4,1,4,8,8,4,8,8,4,1,4}, /*  7: d-pad cross + buttons */
    {4,1,4,4,1,4,4,1,4,4,4,4,4,4,4,4,1,4,8,8,4,8,8,4,1,4}, /*  8: d-pad + buttons */
    {4,1,1,4,4,4,1,1,1,1,1,1,1,1,1,1,1,4,4,4,4,4,4,4,1,4}, /*  9: d-pad bottom */
    {4,1,1,1,1,1,1,1,4,4,4,4,4,4,4,4,1,1,1,1,1,1,1,1,1,4}, /* 10: bottom border */
    {4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4}, /* 11: body bottom */
};

/* Map logo pixel values (0-8) to framebuffer palette indices */
static const uint8_t logo_pal_map[9] = {
    0,                /* 0 = transparent (not drawn) */
    PAL_BLACK,        /* 1 = black */
    PAL_WHITE,        /* 2 = white */
    PAL_LOGO_LGRAY,   /* 3 = light gray #C0 */
    PAL_LOGO_MGRAY,   /* 4 = medium gray #A0 */
    PAL_LOGO_GRAY,    /* 5 = gray #80 */
    PAL_LOGO_DGRAY,   /* 6 = dark gray #60 */
    PAL_LOGO_VDGRAY,  /* 7 = very dark gray #40 */
    PAL_LOGO_RED,     /* 8 = red */
};

/* ─── Welcome screen helpers ──────────────────────────────────────── */

static void setup_welcome_palette(void) {
    setup_selector_palette();
    /* Add logo-specific entries to the palette buffer that was just written */
    int buf = pal_write_idx ^ 1;
    uint32_t *pal = rgb565_palette_32[buf];

    uint16_t c;
    c = rgb565(0xCB, 0xCB, 0xC9); pal[PAL_LOGO_LGRAY]  = c | ((uint32_t)c << 16);
    c = rgb565(0xBD, 0xBD, 0xBB); pal[PAL_LOGO_MGRAY]  = c | ((uint32_t)c << 16);
    c = rgb565(0x8F, 0x8F, 0x8F); pal[PAL_LOGO_GRAY]    = c | ((uint32_t)c << 16);
    c = rgb565(0x71, 0x6F, 0x70); pal[PAL_LOGO_DGRAY]   = c | ((uint32_t)c << 16);
    c = rgb565(0x3F, 0x3F, 0x3F); pal[PAL_LOGO_VDGRAY]  = c | ((uint32_t)c << 16);
    c = rgb565(0xE5, 0x00, 0x11); pal[PAL_LOGO_RED]     = c | ((uint32_t)c << 16);
    c = rgb565(0x90, 0x90, 0x90); pal[PAL_LOGO_CIRCLE]  = c | ((uint32_t)c << 16);
    c = rgb565(0x60, 0x60, 0x60); pal[PAL_LOGO_SHADOW]  = c | ((uint32_t)c << 16);
}

static void draw_filled_circle(int cx, int cy, int r, uint8_t color) {
    int r2 = r * r;
    for (int y = cy - r; y <= cy + r; y++) {
        if (y < 0 || y >= SCREEN_H) continue;
        int dy = y - cy;
        int dx_max_sq = r2 - dy * dy;
        /* Integer square root to find horizontal span */
        int dx = 0;
        while ((dx + 1) * (dx + 1) <= dx_max_sq) dx++;
        int x0 = cx - dx;
        int x1 = cx + dx;
        if (x0 < 0) x0 = 0;
        if (x1 >= SCREEN_W) x1 = SCREEN_W - 1;
        if (x0 <= x1) memset(&fb[y * SCREEN_W + x0], color, x1 - x0 + 1);
    }
}

static void draw_logo_3x(int ox, int oy) {
    for (int y = 0; y < LOGO_H; y++) {
        for (int x = 0; x < LOGO_W; x++) {
            uint8_t px = nes_logo[y][x];
            if (px == 0) continue;
            uint8_t c = logo_pal_map[px];
            int dx = ox + x * 3;
            int dy = oy + y * 3;
            for (int sy = 0; sy < 3; sy++)
                for (int sx = 0; sx < 3; sx++)
                    fb_pixel(dx + sx, dy + sy, c);
        }
    }
}

/* Draw an elliptical shadow on the circle surface.
 * As the controller floats higher (larger bounce), the shadow grows
 * wider/taller (object further from surface) and gets lighter. */
static void draw_logo_shadow(int cx, int cy, int bounce, int circle_r) {
    /* Shadow sits below the controller, on the circle surface */
    int logo_h_half = (LOGO_H * 3 / 2);
    int shadow_cy = cy + logo_h_half + 4 - bounce;
    /* Shadow size: bigger when controller is higher (further from surface) */
    int base_rx = (LOGO_W * 3 / 2) - 4;
    int base_ry = 3;
    int rx = base_rx + (-bounce);
    int ry = base_ry + (-bounce) / 2;
    if (rx < 4) rx = 4;
    if (ry < 2) ry = 2;
    /* Only draw shadow pixels that are inside the circle */
    int cr2 = circle_r * circle_r;
    for (int y = shadow_cy - ry; y <= shadow_cy + ry; y++) {
        if (y < 0 || y >= SCREEN_H) continue;
        int dy_s = y - shadow_cy;
        int dy_c = y - cy;
        if (dy_c * dy_c > cr2) continue;
        for (int x = cx - rx; x <= cx + rx; x++) {
            if (x < 0 || x >= SCREEN_W) continue;
            int dx_s = x - cx;
            int dx_c = x - cx;
            /* Inside ellipse? Skip rows that produce only 1px wide strip */
            if (dx_s * dx_s * ry * ry + dy_s * dy_s * rx * rx < rx * rx * ry * ry) {
                /* Inside circle? */
                if (dx_c * dx_c + dy_c * dy_c <= cr2) {
                    fb_pixel(x, y, PAL_LOGO_SHADOW);
                }
            }
        }
    }
}

/* ─── Welcome screen ──────────────────────────────────────────────── */

void welcome_screen_show(void) {
    fb = test_pixels;
    fb_show = sel_backbuf;
    setup_welcome_palette();

#ifdef MURMNES_VERSION
    char version_str[16];
    snprintf(version_str, sizeof(version_str), "V%s", MURMNES_VERSION);
#else
    const char *version_str = "V1.00";
#endif

    uint32_t frame = 0;
    int prev_buttons = 0xFF;  /* ignore initial button state */

    while (1) {
        selector_wait_vsync();

        fb_fill(PAL_BG);

        /* Light gray circle behind the logo */
        int circle_cx = SCREEN_W / 2;
        int circle_cy = 68;
        int circle_r = 44;
        draw_filled_circle(circle_cx, circle_cy, circle_r, PAL_LOGO_CIRCLE);

        /* NES controller with shadow */
        int logo_x = circle_cx - (LOGO_W * 3 / 2);
        int logo_y = circle_cy - (LOGO_H * 3 / 2);
        draw_logo_shadow(circle_cx, circle_cy, 0, circle_r);
        draw_logo_3x(logo_x, logo_y);

        /* Text */
        fb_text_center(118, "MURMNES", PAL_WHITE);
        fb_text_center(132, version_str, PAL_GRAY);

        fb_text_center(152, "BY MIKHAIL MATVEEV", PAL_GRAY);
        fb_text_center(164, "<XTREME@RH1.TECH>", PAL_GRAY);

        fb_text_center(184, "RH1.TECH", PAL_GRAY);

        /* Blinking "PRESS START" after 2 seconds (120 frames) */
        if (frame >= 120 && ((frame / 30) & 1) == 0) {
            fb_text_center(218, "PRESS START", PAL_WHITE);
        }

        /* Swap buffers and present */
        uint8_t *tmp = fb;
        fb = fb_show;
        fb_show = tmp;
        audio_fill_silence(SAMPLE_RATE / 60);
        pending_pitch = SCREEN_W;
        pending_pixels = fb_show;
        frame++;

        /* Check input after the initial settle period */
        if (frame >= 120) {
            int buttons = read_selector_buttons();
            int pressed = buttons & ~prev_buttons;
            prev_buttons = buttons;
            if (pressed & (BTN_A | BTN_START))
                break;
        } else {
            prev_buttons = read_selector_buttons();
        }

        /* Auto-continue after 10 seconds */
        if (frame >= 600)
            break;
    }

    /* Wait for buttons to be released before proceeding */
    for (int i = 0; i < 60; i++) {
        selector_wait_vsync();
        if (read_selector_buttons() == 0) break;
        audio_fill_silence(SAMPLE_RATE / 60);
        pending_pitch = SCREEN_W;
        pending_pixels = fb_show;
    }
}

/* ─── SD card error screen ────────────────────────────────────────── */

void sd_error_show(void) {
    fb = test_pixels;
    fb_show = sel_backbuf;
    /* Palette should already be set from welcome screen */

    for (int i = 0; i < 300; i++) {  /* 5 seconds at 60fps */
        selector_wait_vsync();

        fb_fill(PAL_BG);
        fb_text_center(100, "NO SD CARD DETECTED", PAL_WHITE);
        fb_text_center(120, "INSERT SD CARD WITH .NES FILES", PAL_GRAY);
        fb_text_center(132, "IN /NES FOLDER AND RESTART", PAL_GRAY);

        uint8_t *tmp = fb;
        fb = fb_show;
        fb_show = tmp;
        audio_fill_silence(SAMPLE_RATE / 60);
        pending_pitch = SCREEN_W;
        pending_pixels = fb_show;
    }
}
