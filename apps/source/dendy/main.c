/*
 * FRANK OS — Dendy NES Emulator (standalone ELF app)
 *
 * Fullscreen 320x240x256 app using QuickNES.
 * Keyboard-only input (PS/2 or USB-HID via FRANK OS keyboard driver).
 * Double-ESC to exit.
 *
 * MEMORY MODEL:
 * FRANK OS ELF loader places .data and .bss in PSRAM, which does NOT
 * support writes via normal ARM store instructions on RP2350.  All mutable
 * state is therefore heap-allocated (pvPortMalloc returns SRAM) and accessed
 * through a pointer held in ARM register r9 (compiled with -ffixed-r9).
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "m-os-api.h"
#include "frankos-app.h"
#include "lang.h"

/* App-local translations */
enum { AL_OPEN_ROM, AL_COUNT };
static const char *al_en[] = { [AL_OPEN_ROM] = "Open a .nes ROM file from\nthe SD card." };
static const char *al_ru[] = { [AL_OPEN_ROM] = "\xD0\x9E\xD1\x82\xD0\xBA\xD1\x80\xD0\xBE\xD0\xB9\xD1\x82\xD0\xB5 \xD1\x84\xD0\xB0\xD0\xB9\xD0\xBB .nes ROM\n\xD1\x81 SD-\xD0\xBA\xD0\xB0\xD1\x80\xD1\x82\xD1\x8B." };
static const char *AL(int id) { return lang_get() == LANG_RU ? al_ru[id] : al_en[id]; }

#undef switch
#undef inline
#undef __force_inline

#include "quicknes.h"

#include <string.h>

/* ======================================================================
 * HID scancode constants
 * ====================================================================== */

#define HID_KEY_ESCAPE      0x29
#define HID_KEY_ENTER       0x28
#define HID_KEY_SPACE       0x2C
#define HID_KEY_UP          0x52
#define HID_KEY_DOWN        0x51
#define HID_KEY_LEFT        0x50
#define HID_KEY_RIGHT       0x4F
#define HID_KEY_Z           0x1D
#define HID_KEY_X           0x1B
#define HID_KEY_A           0x04
#define HID_KEY_S           0x16
#define HID_KEY_Q           0x14
#define HID_KEY_W           0x1A
#define HID_KEY_LSHIFT      0xE1
#define HID_KEY_RSHIFT      0xE5

/* QuickNES joypad bits */
#define NES_A       0x01
#define NES_B       0x02
#define NES_SELECT  0x04
#define NES_START   0x08
#define NES_UP      0x10
#define NES_DOWN    0x20
#define NES_LEFT    0x40
#define NES_RIGHT   0x80

/* NES display constants */
#define NES_WIDTH   256
#define NES_HEIGHT  240
#define NES_PITCH   (256 + 16)   /* QuickNES pixel buffer pitch */

/* Audio — 22050 Hz is enough for NES sound quality and halves
 * the audio processing overhead vs 44100 Hz. */
#define NES_SAMPLE_RATE 22050

/* ======================================================================
 * App globals struct — ALL mutable state lives here, heap-allocated.
 * ====================================================================== */

typedef struct {
    void    *qnes_state;     /* MUST BE FIRST — quicknes.cpp reads r9[0] */
    volatile bool closing;
    uint8_t  key_state[256]; /* HID scancode -> pressed (1/0) */
    bool     rom_loaded;
    void    *app_task;       /* FreeRTOS task handle */
    uint8_t *rom_buf;        /* ROM data buffer (heap-allocated) */
    int16_t *audio_buf;      /* stereo interleave buffer for pcm_write */
    uint8_t  saved_volume;   /* system volume before we changed it */
} app_globals_t;

register app_globals_t *G asm("r9");

/* ======================================================================
 * NES palette -> 8bpp palette mapping
 *
 * Set the 256-color display palette to match the NES color table,
 * then map each frame's palette indices to display palette entries.
 * ====================================================================== */

/* Update the 256-color display palette so that QuickNES pixel values
 * map DIRECTLY to the correct RGB colors.  This eliminates the need
 * for a per-pixel software palette lookup during rendering.
 *
 * QuickNES pixel value i → frame_palette[i] → NES color table → RGB.
 * We set display_palette[i] = RGB(NES_color_table[frame_palette[i]]).
 * Then pixel values written by QuickNES are directly valid 8bpp indices. */
static void update_display_palette(void) {
    int pal_size = 0;
    const int16_t *pal = qnes_get_palette(&pal_size);
    const qnes_rgb_t *colors = qnes_get_color_table();
    if (!pal || !colors) return;

    for (int i = 0; i < pal_size && i < 256; i++) {
        int idx = pal[i];
        if (idx < 0 || idx >= 512) idx = 0x0F;
        if (idx >= 256) idx &= 0x3F;
        const qnes_rgb_t *c = &colors[idx];
        uint32_t rgb = ((uint32_t)c->r << 16) | ((uint32_t)c->g << 8) | c->b;
        display_set_palette_entry((uint8_t)i, rgb);
    }
}

/* ======================================================================
 * Render NES frame to 320x240 framebuffer
 *
 * NES output: 256x240 indexed pixels (with 8px overscan top/bottom).
 * Display: 320x240 @ 8bpp.
 * Center the 256-wide image in the 320-wide display (32px black border).
 * ====================================================================== */

/* Blit QuickNES pixel buffer to display framebuffer.
 * Since update_display_palette() maps pixel values directly to colors,
 * this is a pure copy — no per-pixel palette lookup needed.
 * Only copies the 256-pixel-wide NES image into the center of the
 * 320-pixel-wide display (32px black borders). */
static void render_frame(void) {
    const uint8_t *pixels = qnes_get_pixels();
    if (!pixels) return;

    uint8_t *fb = display_get_framebuffer();
    if (!fb) return;

    for (int y = 0; y < 240; y++) {
        memcpy(fb + y * 320 + 32, pixels + y * NES_PITCH, 256);
    }
}

/* ======================================================================
 * Keyboard -> NES joypad mapping
 *
 * Default mapping:
 *   Arrow keys  = D-pad
 *   Z / A       = NES A
 *   X / S       = NES B
 *   Enter       = Start
 *   RShift/Space = Select
 * ====================================================================== */

static int build_joypad(void) {
    const uint8_t *k = G->key_state;
    int joy = 0;

    if (k[HID_KEY_UP])     joy |= NES_UP;
    if (k[HID_KEY_DOWN])   joy |= NES_DOWN;
    if (k[HID_KEY_LEFT])   joy |= NES_LEFT;
    if (k[HID_KEY_RIGHT])  joy |= NES_RIGHT;
    if (k[HID_KEY_Z] || k[HID_KEY_A])   joy |= NES_A;
    if (k[HID_KEY_X] || k[HID_KEY_S])   joy |= NES_B;
    if (k[HID_KEY_ENTER])               joy |= NES_START;
    if (k[HID_KEY_RSHIFT] || k[HID_KEY_SPACE]) joy |= NES_SELECT;

    return joy;
}

/* ======================================================================
 * Input processing — poll keyboard, update key state, handle ESC-ESC
 * ====================================================================== */

static void process_input(void) {
    keyboard_poll();

    app_key_event_t ev;
    while (keyboard_get_event(&ev)) {
        if (ev.hid_code < 256)
            G->key_state[ev.hid_code] = ev.pressed ? 1 : 0;

        /* ESC = exit */
        if (ev.hid_code == HID_KEY_ESCAPE && ev.pressed) {
            G->closing = true;
            return;
        }
    }
}

/* ======================================================================
 * ROM loading from SD card via FatFS
 * ====================================================================== */

static bool load_rom(const char *path) {
    FIL fil;
    FRESULT fr = f_open(&fil, path, FA_READ);
    if (fr != FR_OK) {
        serial_printf("dendy: f_open failed: %s (err %d)\n", path, fr);
        return false;
    }

    FSIZE_t fsize = f_size(&fil);
    if (fsize < 16 || fsize > 512 * 1024) {
        serial_printf("dendy: invalid ROM size %lu\n", (unsigned long)fsize);
        f_close(&fil);
        return false;
    }

    /* Allocate ROM buffer in PSRAM (ROMs can be up to 512KB) */
    G->rom_buf = (uint8_t *)psram_alloc((size_t)fsize);
    if (!G->rom_buf) {
        serial_printf("dendy: psram_alloc failed for ROM (%lu bytes)\n", (unsigned long)fsize);
        f_close(&fil);
        return false;
    }

    UINT br;
    fr = f_read(&fil, G->rom_buf, (UINT)fsize, &br);
    f_close(&fil);

    if (fr != FR_OK || br != (UINT)fsize) {
        serial_printf("dendy: read error (%d, got %u/%lu)\n", fr, br, (unsigned long)fsize);
        psram_free(G->rom_buf);
        G->rom_buf = NULL;
        return false;
    }

    if (qnes_load_rom(G->rom_buf, (long)fsize) != 0) {
        serial_printf("dendy: qnes_load_rom failed\n");
        psram_free(G->rom_buf);
        G->rom_buf = NULL;
        return false;
    }

    /* ROM data has been copied by QuickNES -- free our buffer */
    psram_free(G->rom_buf);
    G->rom_buf = NULL;
    G->rom_loaded = true;
    serial_printf("dendy: ROM loaded OK (%lu bytes)\n", (unsigned long)fsize);
    return true;
}

/* ======================================================================
 * Audio: push NES mono samples as stereo to I2S via pcm_write
 * ====================================================================== */

static void push_audio(void) {
    int16_t tmp[256];
    int16_t *stereo = G->audio_buf;

    /* Drain all available samples (NES may produce more than one
     * buffer's worth per frame depending on sample rate). */
    for (;;) {
        long n = qnes_read_samples(tmp, 256);
        if (n <= 0) break;

        /* Interleave mono -> stereo, attenuate to avoid distortion */
        for (long i = 0; i < n; i++) {
            int16_t s = tmp[i] >> 2;
            stereo[i * 2]     = s;
            stereo[i * 2 + 1] = s;
        }
        pcm_write(stereo, (int)n);
    }
}

/* ======================================================================
 * Entry point
 * ====================================================================== */

int main(int argc, char **argv) {
    /* Check for ROM path argument — show dialog if launched without one */
    if (argc < 2 || !argv[1] || !argv[1][0]) {
        dialog_show(HWND_NULL, "Dendy",
                    AL(AL_OPEN_ROM),
                    DLG_ICON_INFO, DLG_BTN_OK);
        return 0;
    }

    /* Check if code was placed in SRAM (fast) or PSRAM (too slow).
     * The ELF loader sets code placement based on free heap at load time.
     * If we ended up in PSRAM, emulation will be unplayably slow. */
    {
        extern uint32_t __app_flags(void);
        uintptr_t code_addr = (uintptr_t)__app_flags;
        if (code_addr >= 0x15000000 && code_addr < 0x15800000) {
            dialog_show(HWND_NULL, "Dendy",
                        "Not enough SRAM to run.\n"
                        "Close other apps and retry.",
                        DLG_ICON_ERROR, DLG_BTN_OK);
            return 0;
        }
    }

    /* Allocate globals in SRAM */
    app_globals_t *globals = (app_globals_t *)pvPortMalloc(sizeof(app_globals_t));
    if (!globals) { serial_printf("dendy: globals alloc failed\n"); return 1; }
    memset(globals, 0, sizeof(app_globals_t));
    G = globals;

    G->app_task = xTaskGetCurrentTaskHandle();

    /* Allocate QuickNES state struct in SRAM (must be first, before qnes_init).
     * This pointer is stored at G[0] (first field of app_globals_t) so that
     * quicknes.cpp can access it via the r9 register. */
    unsigned long qs_size = qnes_state_size();
    G->qnes_state = pvPortMalloc(qs_size);
    if (!G->qnes_state) {
        serial_printf("dendy: qnes_state alloc failed (%lu bytes)\n", qs_size);
        vPortFree(globals);
        return 1;
    }
    memset(G->qnes_state, 0, qs_size);

    /* Allocate pixel buffers in PSRAM (too large for SRAM heap).
     * Each buffer: (256+16) * (240+2) = 65,824 bytes. */
    {
        #define PIXEL_BUF_SIZE ((256 + 16) * (240 + 2))
        void *pb0 = psram_alloc(PIXEL_BUF_SIZE);
        void *pb1 = psram_alloc(PIXEL_BUF_SIZE);
        if (!pb0 || !pb1) {
            serial_printf("dendy: pixel buf alloc failed\n");
            if (pb0) psram_free(pb0);
            if (pb1) psram_free(pb1);
            vPortFree(G->qnes_state);
            vPortFree(globals);
            return 1;
        }
        memset(pb0, 0, PIXEL_BUF_SIZE);
        memset(pb1, 0, PIXEL_BUF_SIZE);
        qnes_set_pixel_bufs(pb0, pb1);
    }

    /* Allocate stereo audio buffer */
    G->audio_buf = (int16_t *)pvPortMalloc(256 * 2 * sizeof(int16_t));
    if (!G->audio_buf) {
        serial_printf("dendy: audio buf alloc failed\n");
        vPortFree(G->qnes_state);
        vPortFree(globals);
        return 1;
    }

    /* Initialize QuickNES */
    if (qnes_init(NES_SAMPLE_RATE) != 0) {
        serial_printf("dendy: qnes_init failed\n");
        vPortFree(G->audio_buf);
        vPortFree(G->qnes_state);
        vPortFree(globals);
        return 1;
    }

    /* Load ROM */
    if (!load_rom(argv[1])) {
        vPortFree(G->audio_buf);
        vPortFree(G->qnes_state);
        vPortFree(globals);
        return 1;
    }

    /* Switch to fullscreen 320x240x256 video mode */
    if (display_set_video_mode(VIDEO_MODE_320x240x256) != 0) {
        qnes_close();
        vPortFree(G->audio_buf);
        vPortFree(G->qnes_state);
        vPortFree(globals);
        return 1;
    }

    /* Initialize all 256 palette entries to black, then clear framebuffer.
     * This ensures the first DVI frame after mode switch is valid. */
    for (int i = 0; i < 256; i++)
        display_set_palette_entry((uint8_t)i, 0x000000);
    display_clear(0);

    /* Initialize audio: mono NES samples played as stereo I2S.
     * Channel samples are attenuated (>>2), so boost system volume
     * to compensate.  Restore original volume on exit. */
    pcm_init(NES_SAMPLE_RATE, 2);
    {
        typedef uint8_t (*get_vol_t)(void);
        typedef void (*set_vol_t)(uint8_t);
        G->saved_volume = ((get_vol_t)_sys_table_ptrs[535])();
        uint8_t new_vol = (G->saved_volume >= 2) ? G->saved_volume - 2 : 0;
        ((set_vol_t)_sys_table_ptrs[534])(new_vol);
    }

    /* Clear screen to black */
    display_clear(0);

    /* Main emulation loop */
    while (!G->closing) {
        process_input();
        if (G->closing) break;

        int joypad = build_joypad();
        qnes_emulate_frame(joypad, 0);

        update_display_palette();
        render_frame();
        push_audio();
    }

    /* Restore system volume */
    {
        typedef void (*set_vol_t)(uint8_t);
        ((set_vol_t)_sys_table_ptrs[534])(G->saved_volume);
    }
    qnes_close();
    pcm_cleanup();

    display_set_video_mode(VIDEO_MODE_640x480x16);
    /* Force full desktop repaint — ensures no black regions remain */
    {
        typedef void (*fn_t)(void);
        ((fn_t)_sys_table_ptrs[533])();  /* wm_force_full_repaint */
    }
    taskbar_invalidate();


    vPortFree(G->audio_buf);
    vPortFree(G->qnes_state);
    vPortFree(globals);
    return 0;
}

uint32_t __app_flags(void) { return APPFLAG_SINGLETON; }
