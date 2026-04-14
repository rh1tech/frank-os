/*
 * FRANK OS — Video Player (standalone ELF app)
 *
 * Fullscreen 320x240x256 video player.
 * Supports MPEG-1 (.mpg) via pl_mpeg and PSX STR (.str) via psx_str.
 * Streams from SD card — no file size limit.
 * Launched by opening video files from Navigator.
 * Space to pause, ESC to exit.
 *
 * MEMORY MODEL: same as Dendy — all mutable state heap-allocated in SRAM,
 * accessed through r9 register (-ffixed-r9).
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "m-os-api.h"
#include "frankos-app.h"
#include "lang.h"

/* App-local translations */
enum { AL_OPEN_VIDEO, AL_NO_MEMORY, AL_CANNOT_OPEN, AL_COUNT };
static const char *al_en[] = {
    [AL_OPEN_VIDEO]  = "Open a .mpg or .str video\nfile from the SD card.",
    [AL_NO_MEMORY]   = "Not enough memory.",
    [AL_CANNOT_OPEN] = "Cannot open file.",
};
static const char *al_ru[] = {
    [AL_OPEN_VIDEO]  = "\xD0\x9E\xD1\x82\xD0\xBA\xD1\x80\xD0\xBE\xD0\xB9\xD1\x82\xD0\xB5 \xD1\x84\xD0\xB0\xD0\xB9\xD0\xBB .mpg \xD0\xB8\xD0\xBB\xD0\xB8 .str\n\xD1\x81 SD-\xD0\xBA\xD0\xB0\xD1\x80\xD1\x82\xD1\x8B.",
    [AL_NO_MEMORY]   = "\xD0\x9D\xD0\xB5\xD0\xB4\xD0\xBE\xD1\x81\xD1\x82\xD0\xB0\xD1\x82\xD0\xBE\xD1\x87\xD0\xBD\xD0\xBE \xD0\xBF\xD0\xB0\xD0\xBC\xD1\x8F\xD1\x82\xD0\xB8.",
    [AL_CANNOT_OPEN] = "\xD0\x9D\xD0\xB5 \xD1\x83\xD0\xB4\xD0\xB0\xD0\xBB\xD0\xBE\xD1\x81\xD1\x8C \xD0\xBE\xD1\x82\xD0\xBA\xD1\x80\xD1\x8B\xD1\x82\xD1\x8C \xD1\x84\xD0\xB0\xD0\xB9\xD0\xBB.",
};
static const char *AL(int id) { return lang_get() == LANG_RU ? al_ru[id] : al_en[id]; }

#undef switch
#undef inline
#undef __force_inline
#undef abs

#define PLM_NO_STDIO
#define PLM_BUFFER_DEFAULT_SIZE (512 * 1024)

/* Route pl_mpeg allocations to SRAM first (fast) for hot audio/video structs.
 * Only the 512KB stream buffer will fall back to PSRAM. */
static inline void *plm_sram_malloc(size_t sz) {
    typedef void *(*fn_t)(size_t);
    /* Try SRAM first */
    void *p = ((fn_t)_sys_table_ptrs[529])(sz);
    if (p) return p;
    /* Fall back to PSRAM */
    return ((fn_t)_sys_table_ptrs[32])(sz);
}
#define PLM_MALLOC(sz)      plm_sram_malloc(sz)
#define PLM_FREE(p)         free(p)
#define PLM_REALLOC(p, sz)  realloc(p, sz)

#define PL_MPEG_IMPLEMENTATION
#include "pl_mpeg.h"

#define PSX_MALLOC(sz)  plm_sram_malloc(sz)
#define PSX_FREE(p)     free(p)
#define PSX_STR_IMPLEMENTATION
#include "psx_str.h"

#include <string.h>

#define HID_KEY_ESCAPE  0x29
#define HID_KEY_SPACE   0x2C
#define AUDIO_BUF_SAMPLES  2048  /* max(1152 MPEG, 2016 XA), rounded up */

typedef struct {
    volatile bool closing;
    volatile bool paused;
    uint8_t  key_state[256];
    void    *app_task;
    plm_t   *plm;
    psx_str_t *str;             /* PSX STR decoder (NULL for MPEG-1) */
    bool     is_str;            /* true if playing .str file */
    FIL     *fil;
    int16_t *audio_buf;
    uint8_t *y_tab;
    uint8_t *dt;                /* dither tables: 12 * 1024 bytes, SRAM */
    plm_frame_t *pending_frame;
    uint8_t  skip_count;        /* frame skip counter */
    uint8_t  dither_phase;      /* toggles each frame for temporal dithering */
    uint32_t time_debt;         /* ms lost to elapsed cap, repaid gradually */
    uint8_t  saved_volume;
    bool     audio_inited;      /* true after pcm_init called */
    int      video_w;
    int      video_h;
    int      offset_x;
    int      offset_y;
} app_globals_t;

register app_globals_t *G asm("r9");

/* ======================================================================
 * pl_mpeg buffer callbacks — stream from FatFS
 * ====================================================================== */

static void plm_load_cb(plm_buffer_t *buf, void *user) {
    FIL *fil = (FIL *)user;
    if (buf->discard_read_bytes)
        plm_buffer_discard_read_bytes(buf);
    size_t bytes_available = buf->capacity - buf->length;
    if (bytes_available > 32768) bytes_available = 32768;
    UINT br = 0;
    f_read(fil, buf->bytes + buf->length, (UINT)bytes_available, &br);
    buf->length += br;
    if (br == 0)
        buf->has_ended = TRUE;
}

static void plm_seek_cb(plm_buffer_t *buf, size_t offset, void *user) {
    (void)buf;
    f_lseek((FIL *)user, (FSIZE_t)offset);
}

static size_t plm_tell_cb(plm_buffer_t *buf, void *user) {
    (void)buf;
    return (size_t)f_tell((FIL *)user);
}

/* ======================================================================
 * RGB332 palette
 * ====================================================================== */

/* 6×6×6 color cube (216 entries) + 40-step grayscale ramp.
 * Index = r_level*36 + g_level*6 + b_level  (each 0-5).
 * 6 levels per channel (step=51) vs RGB332's 8/8/4 — much better blues. */
static void setup_palette(void) {
    for (int r = 0; r < 6; r++)
        for (int g = 0; g < 6; g++)
            for (int b = 0; b < 6; b++) {
                int idx = r * 36 + g * 6 + b;
                uint32_t rgb = ((uint32_t)(r * 51) << 16)
                             | ((uint32_t)(g * 51) << 8)
                             | (uint32_t)(b * 51);
                display_set_palette_entry((uint8_t)idx, rgb);
            }
    /* Extra grayscale ramp in slots 216-255 for smoother darks */
    for (int i = 0; i < 40; i++) {
        uint8_t v = (uint8_t)(i * 255 / 39);
        uint32_t rgb = ((uint32_t)v << 16) | ((uint32_t)v << 8) | v;
        display_set_palette_entry((uint8_t)(216 + i), rgb);
    }
}

/* ======================================================================
 * YCbCr → RGB332, BT.601 limited-range, 2×2 blocks, SRAM line buffers
 * ====================================================================== */

/* Dither tables: 4 Bayer positions × 3 channels (R,G,B) = 12 tables.
 * Each table is 1024 bytes, indexed by (scaled_Y + chroma_delta + 256).
 * Clamping + quantization + dither threshold baked in.
 * Total: 12 KB in SRAM.  Inner loop: 3 loads + 2 ORs per pixel. */

#define DT_SZ   1024
#define DT_BIAS 256

static void init_tables(void) {
    int p, i;

    G->y_tab = (uint8_t *)pvPortMalloc(256);
    for (i = 0; i < 256; i++) {
        int v = ((i - 16) * 76309) >> 16;
        if (v < 0) v = 0; else if (v > 255) v = 255;
        G->y_tab[i] = (uint8_t)v;
    }

    /* 6×6×6 cube: step = 51, 2×2 Bayer thresholds = step × {0, 0.5, 0.75, 0.25}
     * = {0, 25, 38, 13}.  R table outputs r_level*36, G outputs g_level*6,
     * B outputs b_level.  Pixel index = R + G + B (addition, not OR). */
    int th[4] = {0, 25, 38, 13};

    G->dt = (uint8_t *)pvPortMalloc(12 * DT_SZ);
    for (p = 0; p < 4; p++) {
        uint8_t *dr = G->dt + (p * 3 + 0) * DT_SZ;
        uint8_t *dg = G->dt + (p * 3 + 1) * DT_SZ;
        uint8_t *db = G->dt + (p * 3 + 2) * DT_SZ;
        for (i = 0; i < DT_SZ; i++) {
            int v = i - DT_BIAS;
            if (v < 0) v = 0; if (v > 255) v = 255;

            int rv = (v + th[p]) * 5 / 255; if (rv > 5) rv = 5;
            int gv = (v + th[p]) * 5 / 255; if (gv > 5) gv = 5;
            int bv = (v + th[p]) * 5 / 255; if (bv > 5) bv = 5;
            dr[i] = (uint8_t)(rv * 36);
            dg[i] = (uint8_t)(gv * 6);
            db[i] = (uint8_t)(bv);
        }
    }
}

static void on_audio(plm_t *mpeg, plm_samples_t *samples, void *user);

#define LINE_BUF_W 336

/* 1:1 renderer with 4×4 blue-noise-like dithering.
 * Uses the same 4 threshold tables but varies assignment per chroma block
 * based on (col%2, row%2), creating a non-repeating 4×4 pattern.
 * Looks much smoother than regular 2×2 Bayer with no extra cost. */
static void render_1x_rows(uint8_t *fb, plm_frame_t *frame,
                            int row_start, int row_end) {
    int cols = (int)frame->width >> 1;
    int yw = (int)frame->y.width;
    int cw = (int)frame->cb.width;
    int ox = G->offset_x;
    int oy = G->offset_y;
    const uint8_t *ytab = G->y_tab;

    if (cols > 160) cols = 160;

    const uint8_t *r0 = G->dt + 0*DT_SZ + DT_BIAS;
    const uint8_t *g0 = G->dt + 1*DT_SZ + DT_BIAS;
    const uint8_t *b0 = G->dt + 2*DT_SZ + DT_BIAS;
    const uint8_t *r1 = G->dt + 3*DT_SZ + DT_BIAS;
    const uint8_t *g1 = G->dt + 4*DT_SZ + DT_BIAS;
    const uint8_t *b1 = G->dt + 5*DT_SZ + DT_BIAS;
    const uint8_t *r2 = G->dt + 6*DT_SZ + DT_BIAS;
    const uint8_t *g2 = G->dt + 7*DT_SZ + DT_BIAS;
    const uint8_t *b2 = G->dt + 8*DT_SZ + DT_BIAS;
    const uint8_t *r3 = G->dt + 9*DT_SZ + DT_BIAS;
    const uint8_t *g3 = G->dt +10*DT_SZ + DT_BIAS;
    const uint8_t *b3 = G->dt +11*DT_SZ + DT_BIAS;

    uint8_t yl0[LINE_BUF_W], yl1[LINE_BUF_W];
    uint8_t cbl[LINE_BUF_W/2], crl[LINE_BUF_W/2];

    for (int row = row_start; row < row_end; row++) {
        memcpy(yl0, frame->y.data  + row*2*yw,      cols*2);
        memcpy(yl1, frame->y.data  + row*2*yw + yw,  cols*2);
        memcpy(cbl, frame->cb.data + row*cw,          cols);
        memcpy(crl, frame->cr.data + row*cw,          cols);

        int di = (oy + row*2) * 320 + ox;
        int yi = 0;

        for (int col = 0; col < cols; col++) {
            int cr = crl[col] - 128;
            int cb = cbl[col] - 128;
            int rd = (cr * 104597) >> 16;
            int gd = (cb * 25674 + cr * 53278) >> 16;
            int bd = (cb * 132201) >> 16;
            int yy;

            yy = ytab[yl0[yi]];
            fb[di]     = r0[yy+rd] + g0[yy-gd] + b0[yy+bd];
            yy = ytab[yl0[yi+1]];
            fb[di+1]   = r1[yy+rd] + g1[yy-gd] + b1[yy+bd];
            yy = ytab[yl1[yi]];
            fb[di+320] = r2[yy+rd] + g2[yy-gd] + b2[yy+bd];
            yy = ytab[yl1[yi+1]];
            fb[di+321] = r3[yy+rd] + g3[yy-gd] + b3[yy+bd];

            yi += 2;
            di += 2;
        }
    }
}

/* 2× upscale renderer for video ≤ 160×120 → 320×240.
 * Dithering applied per SOURCE pixel (not per display pixel): each source
 * pixel uses one Bayer position based on its (col,row) parity, then fills
 * its 2×2 display block with that single value. Adjacent source pixels
 * get different thresholds, breaking up RGB332 banding at zero extra cost
 * vs. non-dithered (still 1 lookup per source pixel). */
static void render_2x(uint8_t *fb, plm_frame_t *frame) {
    int cols = (int)frame->width >> 1;
    int rows = (int)frame->height >> 1;
    int yw = (int)frame->y.width;
    int cw = (int)frame->cb.width;
    int ox = G->offset_x;
    int oy = G->offset_y;
    const uint8_t *ytab = G->y_tab;

    if (cols > 80) cols = 80;
    if (rows > 60) rows = 60;

    /* 4 Bayer positions — one per source pixel in each 2×2 chroma block:
     *   TL = pos 0,  TR = pos 1,  BL = pos 2,  BR = pos 3 */
    const uint8_t *rp[4], *gp[4], *bp[4];
    for (int p = 0; p < 4; p++) {
        rp[p] = G->dt + (p*3+0)*DT_SZ + DT_BIAS;
        gp[p] = G->dt + (p*3+1)*DT_SZ + DT_BIAS;
        bp[p] = G->dt + (p*3+2)*DT_SZ + DT_BIAS;
    }

    uint8_t yl0[LINE_BUF_W], yl1[LINE_BUF_W];
    uint8_t cbl[LINE_BUF_W/2], crl[LINE_BUF_W/2];

    for (int row = 0; row < rows; row++) {
        memcpy(yl0, frame->y.data  + row*2*yw,      cols*2);
        memcpy(yl1, frame->y.data  + row*2*yw + yw,  cols*2);
        memcpy(cbl, frame->cb.data + row*cw,          cols);
        memcpy(crl, frame->cr.data + row*cw,          cols);

        int di0 = (oy + row*4)     * 320 + ox;
        int di1 = (oy + row*4 + 1) * 320 + ox;
        int di2 = (oy + row*4 + 2) * 320 + ox;
        int di3 = (oy + row*4 + 3) * 320 + ox;
        int yi = 0;

        for (int col = 0; col < cols; col++) {
            int cr = crl[col] - 128;
            int cb = cbl[col] - 128;
            int rd = (cr * 104597) >> 16;
            int gd = (cb * 25674 + cr * 53278) >> 16;
            int bd = (cb * 132201) >> 16;
            int yy;
            uint8_t px;

            /* TL source pixel — Bayer pos 0, fill 2×2 */
            yy = ytab[yl0[yi]];
            px = rp[0][yy+rd] + gp[0][yy-gd] + bp[0][yy+bd];
            fb[di0]   = px; fb[di0+1] = px;
            fb[di1]   = px; fb[di1+1] = px;

            /* TR source pixel — Bayer pos 1 */
            yy = ytab[yl0[yi+1]];
            px = rp[1][yy+rd] + gp[1][yy-gd] + bp[1][yy+bd];
            fb[di0+2] = px; fb[di0+3] = px;
            fb[di1+2] = px; fb[di1+3] = px;

            /* BL source pixel — Bayer pos 2 */
            yy = ytab[yl1[yi]];
            px = rp[2][yy+rd] + gp[2][yy-gd] + bp[2][yy+bd];
            fb[di2]   = px; fb[di2+1] = px;
            fb[di3]   = px; fb[di3+1] = px;

            /* BR source pixel — Bayer pos 3 */
            yy = ytab[yl1[yi+1]];
            px = rp[3][yy+rd] + gp[3][yy-gd] + bp[3][yy+bd];
            fb[di2+2] = px; fb[di2+3] = px;
            fb[di3+2] = px; fb[di3+3] = px;

            yi += 2;
            di0 += 4; di1 += 4; di2 += 4; di3 += 4;
        }
    }
}

static void on_video(plm_t *mpeg, plm_frame_t *frame, void *user) {
    (void)mpeg; (void)user;

    /* Adaptive frame skip: skip render when behind, always render
     * at least every 3rd frame for visual continuity. */
    G->skip_count++;
    if (G->time_debt > 20 && G->skip_count < 3) return;
    G->skip_count = 0;

    uint8_t *fb = display_get_framebuffer();
    if (!fb) return;

    if ((int)frame->width <= 160 && (int)frame->height <= 120)
        render_2x(fb, frame);
    else {
        int rows = (int)frame->height >> 1;
        if (rows > 120) rows = 120;
        render_1x_rows(fb, frame, 0, rows);
    }
}

/* ======================================================================
 * Audio callback
 * ====================================================================== */

static void on_audio(plm_t *mpeg, plm_samples_t *samples, void *user) {
    (void)mpeg; (void)user;
    int16_t *out = G->audio_buf;
    int count = (int)samples->count;
    for (int i = 0; i < count * 2; i++) {
        int v = (int)(samples->interleaved[i] * 8192.0f);  /* 25% — matches Dendy's >>2 */
        if (v >  32767) v =  32767;
        if (v < -32767) v = -32767;
        out[i] = (int16_t)v;
    }
    pcm_write(out, count);
}

/* ======================================================================
 * PSX STR I/O callbacks
 * ====================================================================== */

static int str_read_cb(void *buf, uint32_t size, void *user) {
    UINT br = 0;
    f_read((FIL *)user, buf, (UINT)size, &br);
    return (int)br;
}

static void str_seek_cb(uint32_t offset, void *user) {
    f_lseek((FIL *)user, (FSIZE_t)offset);
}

static uint32_t str_tell_cb(void *user) {
    return (uint32_t)f_tell((FIL *)user);
}

/* PSX STR video callback — convert psx_frame_t to plm_frame_t for reuse
 * of existing dither renderers. The plane layouts are identical. */
static void on_str_video(psx_str_t *str, psx_frame_t *frame, void *user) {
    (void)str; (void)user;

    G->skip_count++;
    if (G->time_debt > 20 && G->skip_count < 3) return;
    G->skip_count = 0;

    uint8_t *fb = display_get_framebuffer();
    if (!fb) return;

    /* Wrap psx_frame_t as plm_frame_t for the existing renderers */
    plm_frame_t pf;
    pf.width  = frame->width;
    pf.height = frame->height;
    pf.y.width  = frame->y.width;
    pf.y.height = frame->y.height;
    pf.y.data   = frame->y.data;
    pf.cb.width  = frame->cb.width;
    pf.cb.height = frame->cb.height;
    pf.cb.data   = frame->cb.data;
    pf.cr.width  = frame->cr.width;
    pf.cr.height = frame->cr.height;
    pf.cr.data   = frame->cr.data;

    if ((int)pf.width <= 160 && (int)pf.height <= 120)
        render_2x(fb, &pf);
    else {
        int rows = (int)pf.height >> 1;
        if (rows > 120) rows = 120;
        render_1x_rows(fb, &pf, 0, rows);
    }
}

/* PSX STR audio callback — XA-ADPCM samples are already int16_t PCM.
 * Init audio on first callback since STR has no upfront header. */
static void on_str_audio(psx_str_t *str, psx_audio_t *audio, void *user) {
    (void)str; (void)user;
    if (audio->count <= 0) return;

    if (!G->audio_inited) {
        pcm_init(audio->sample_rate, audio->channels);
        G->audio_inited = true;
    }

    /* Scale down to 25% volume to match MPEG path */
    int16_t *out = G->audio_buf;
    int n = audio->count * audio->channels;
    for (int i = 0; i < n; i++) {
        int v = audio->samples[i] >> 2;
        out[i] = (int16_t)v;
    }
    pcm_write(out, audio->count);
}

/* ======================================================================
 * Input
 * ====================================================================== */

static void process_input(void) {
    keyboard_poll();
    app_key_event_t ev;
    while (keyboard_get_event(&ev)) {
        if (ev.hid_code < 256)
            G->key_state[ev.hid_code] = ev.pressed ? 1 : 0;
        if (!ev.pressed) continue;
        if (ev.hid_code == HID_KEY_ESCAPE) { G->closing = true; return; }
        if (ev.hid_code == HID_KEY_SPACE)    G->paused = !G->paused;
    }
}

/* ======================================================================
 * Entry point
 * ====================================================================== */

int main(int argc, char **argv) {
    if (argc < 2 || !argv[1] || !argv[1][0]) {
        dialog_show(HWND_NULL, "Video Player",
                    AL(AL_OPEN_VIDEO),
                    DLG_ICON_INFO, DLG_BTN_OK);
        return 0;
    }

    {
        extern uint32_t __app_flags(void);
        uintptr_t code_addr = (uintptr_t)__app_flags;
        if (code_addr >= 0x15000000 && code_addr < 0x15800000) {
            dialog_show(HWND_NULL, "Video Player",
                        "Not enough SRAM to run.\n"
                        "Close other apps and retry.",
                        DLG_ICON_ERROR, DLG_BTN_OK);
            return 0;
        }
    }

    app_globals_t *globals = (app_globals_t *)pvPortMalloc(sizeof(app_globals_t));
    if (!globals) {
        dialog_show(HWND_NULL, "Video Player", AL(AL_NO_MEMORY),
                    DLG_ICON_ERROR, DLG_BTN_OK);
        return 1;
    }
    memset(globals, 0, sizeof(app_globals_t));
    G = globals;
    G->app_task = xTaskGetCurrentTaskHandle();
    init_tables();

    /* Detect file format by extension */
    const char *path = argv[1];
    int plen = 0;
    while (path[plen]) plen++;
    G->is_str = (plen >= 4 &&
                 (path[plen-4] == '.') &&
                 ((path[plen-3] == 's') || (path[plen-3] == 'S')) &&
                 ((path[plen-2] == 't') || (path[plen-2] == 'T')) &&
                 ((path[plen-1] == 'r') || (path[plen-1] == 'R')));

    /* Open file */
    G->fil = (FIL *)pvPortMalloc(sizeof(FIL));
    if (!G->fil || f_open(G->fil, argv[1], FA_READ) != FR_OK) {
        dialog_show(HWND_NULL, "Video Player", AL(AL_CANNOT_OPEN),
                    DLG_ICON_ERROR, DLG_BTN_OK);
        if (G->fil) vPortFree(G->fil);
        vPortFree(globals);
        return 1;
    }

    /* Allocate shared audio buffer (used by both paths) */
    G->audio_buf = (int16_t *)pvPortMalloc(AUDIO_BUF_SAMPLES * 2 * sizeof(int16_t));
    if (!G->audio_buf) {
        dialog_show(HWND_NULL, "Video Player", AL(AL_NO_MEMORY),
                    DLG_ICON_ERROR, DLG_BTN_OK);
        f_close(G->fil); vPortFree(G->fil); vPortFree(globals);
        return 1;
    }

    int samplerate = 0;

    if (G->is_str) {
        /* ---- PSX STR path ---- */
        G->str = psx_str_create(str_read_cb, str_seek_cb, str_tell_cb, G->fil);
        if (!G->str) {
            dialog_show(HWND_NULL, "Video Player", "Cannot init STR decoder.",
                        DLG_ICON_ERROR, DLG_BTN_OK);
            f_close(G->fil); vPortFree(G->fil);
            vPortFree(G->audio_buf); vPortFree(globals);
            return 1;
        }

        /* Read first few sectors to discover dimensions */
        for (int i = 0; i < 20 && psx_str_get_width(G->str) == 0; i++)
            psx_str_decode_sector(G->str);

        /* Rewind to start for actual playback */
        f_lseek(G->fil, 0);

        G->video_w = psx_str_get_width(G->str);
        G->video_h = psx_str_get_height(G->str);
        if (G->video_w <= 0 || G->video_h <= 0) {
            dialog_show(HWND_NULL, "Video Player", "Invalid STR file.",
                        DLG_ICON_ERROR, DLG_BTN_OK);
            psx_str_destroy(G->str);
            f_close(G->fil); vPortFree(G->fil);
            vPortFree(G->audio_buf); vPortFree(globals);
            return 1;
        }

        psx_str_set_video_callback(G->str, on_str_video, NULL);
        psx_str_set_audio_callback(G->str, on_str_audio, NULL);

        serial_printf("video: %dx%d STR @ %d fps\n",
                      G->video_w, G->video_h,
                      psx_str_get_fps(G->str));
    } else {
        /* ---- MPEG-1 path ---- */
        FSIZE_t file_size = f_size(G->fil);
        plm_buffer_t *buf = plm_buffer_create_with_callbacks(
            plm_load_cb, plm_seek_cb, plm_tell_cb,
            (size_t)file_size, G->fil);
        G->plm = plm_create_with_buffer(buf, TRUE);
        if (!G->plm) {
            dialog_show(HWND_NULL, "Video Player", "Not a valid MPEG-1 file.",
                        DLG_ICON_ERROR, DLG_BTN_OK);
            f_close(G->fil); vPortFree(G->fil);
            vPortFree(G->audio_buf); vPortFree(globals);
            return 1;
        }

        G->video_w = plm_get_width(G->plm);
        G->video_h = plm_get_height(G->plm);
        if (G->video_w <= 0 || G->video_h <= 0) {
            dialog_show(HWND_NULL, "Video Player", "Invalid video dimensions.",
                        DLG_ICON_ERROR, DLG_BTN_OK);
            plm_destroy(G->plm);
            vPortFree(G->audio_buf); vPortFree(globals);
            return 1;
        }

        samplerate = plm_get_samplerate(G->plm);
        serial_printf("video: %dx%d @ %d fps, audio %d Hz\n",
                      G->video_w, G->video_h,
                      (int)plm_get_framerate(G->plm), samplerate);
    }

    /* Center on 320x240 display; account for 2× upscale if small */
    int disp_w = G->video_w, disp_h = G->video_h;
    if (G->video_w <= 160 && G->video_h <= 120) {
        disp_w *= 2; disp_h *= 2;
    }
    G->offset_x = (320 - disp_w) / 2;
    G->offset_y = (240 - disp_h) / 2;
    if (G->offset_x < 0) G->offset_x = 0;
    if (G->offset_y < 0) G->offset_y = 0;

    if (display_set_video_mode(VIDEO_MODE_320x240x256) != 0) {
        dialog_show(HWND_NULL, "Video Player", "Failed to switch video mode.",
                    DLG_ICON_ERROR, DLG_BTN_OK);
        if (G->is_str) psx_str_destroy(G->str);
        else plm_destroy(G->plm);
        vPortFree(G->audio_buf); vPortFree(globals);
        return 1;
    }

    setup_palette();
    display_clear(0);

    /* MPEG audio setup (STR audio inits lazily on first audio sector) */
    if (!G->is_str) {
        if (samplerate > 0) {
            pcm_init(samplerate, 2);
            plm_set_audio_enabled(G->plm, TRUE);
            G->audio_inited = true;
        } else {
            plm_set_audio_enabled(G->plm, FALSE);
        }
        plm_set_video_decode_callback(G->plm, on_video, NULL);
        if (samplerate > 0) {
            plm_set_audio_decode_callback(G->plm, on_audio, NULL);
            plm_set_audio_lead_time(G->plm, 0.2f);
        }
    }

    {
        typedef uint8_t (*get_vol_t)(void);
        G->saved_volume = ((get_vol_t)_sys_table_ptrs[535])();
    }

    /* ---- Main loop ---- */
    uint32_t last_tick = xTaskGetTickCount();

    if (G->is_str) {
        /* STR: sector-driven playback at ~150 sectors/sec (15fps × ~10 sectors/frame).
         * We pace by time: each sector ≈ 6.67ms. Feed sectors to match elapsed time. */
        uint32_t sectors_decoded = 0;
        while (!G->closing) {
            process_input();
            if (G->closing) break;

            if (G->paused) {
                ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(50));
                last_tick = xTaskGetTickCount();
                continue;
            }

            uint32_t now = xTaskGetTickCount();
            uint32_t elapsed_ms = now - last_tick;

            /* How many sectors should have been decoded by now?
             * 150 sectors/sec = 3 sectors per 20ms */
            uint32_t target = (elapsed_ms * 150) / 1000;

            /* Decode up to 10 sectors per iteration to stay responsive */
            int batch = 0;
            while (sectors_decoded < target && batch < 10) {
                if (!psx_str_decode_sector(G->str)) {
                    G->closing = true;
                    break;
                }
                sectors_decoded++;
                batch++;
            }

            /* If we're way behind, skip ahead */
            if (sectors_decoded + 30 < target) {
                sectors_decoded = target;
                G->time_debt += 20;
            }

            /* Yield a tick if caught up to avoid busy-spinning */
            if (sectors_decoded >= target)
                ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(1));
        }
    } else {
        /* MPEG-1: time-driven plm_decode */
        while (!G->closing) {
            process_input();
            if (G->closing) break;

            if (G->paused) {
                ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(50));
                last_tick = xTaskGetTickCount();
                continue;
            }

            uint32_t now = xTaskGetTickCount();
            uint32_t elapsed_ms = now - last_tick;
            last_tick = now;

            uint32_t debt = G->time_debt;
            uint32_t repay = (debt > 20) ? 20 : debt;
            uint32_t feed = elapsed_ms + repay;
            if (feed > 150) {
                G->time_debt = debt - repay + (feed - 150);
                feed = 150;
            } else {
                G->time_debt = debt - repay;
            }

            plm_decode(G->plm, (float)feed * 0.001f);

            if (plm_has_ended(G->plm)) break;
        }
    }

    /* Cleanup */
    if (G->audio_inited) pcm_cleanup();

    if (G->is_str)
        psx_str_destroy(G->str);
    else
        plm_destroy(G->plm);

    f_close(G->fil);
    vPortFree(G->fil);

    display_set_video_mode(VIDEO_MODE_640x480x16);
    {
        typedef void (*fn_t)(void);
        ((fn_t)_sys_table_ptrs[533])();
    }
    taskbar_invalidate();

    vPortFree(G->audio_buf);
    vPortFree(G->y_tab);
    vPortFree(G->dt);
    vPortFree(globals);
    return 0;
}

uint32_t __app_flags(void) { return APPFLAG_SINGLETON; }
