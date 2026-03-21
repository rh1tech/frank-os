/*
 * MurmNES - NES Emulator for RP2350
 * Copyright (c) 2026 Mikhail Matveev <xtreme@rh1.tech>
 * https://rh1.tech | https://github.com/rh1tech/murmnes
 * SPDX-License-Identifier: MIT
 *
 * FRANK OS port: mutable state is heap-allocated and accessed via the
 * r9 register pattern for consistency with other FRANK OS ELF apps.
 */

#include "quicknes.h"
#include "nes_emu.h"
#include "nes_state.h"
#include "data_reader.h"
#include "abstract_file.h"
#include "ff.h"
#include <stdio.h>
#include <string.h>

#define PIXEL_BUF_SIZE ((256 + 16) * (240 + 2))

/* All mutable state bundled into a heap-allocated struct,
 * accessed via the r9 register pattern. */
struct qnes_state_t {
    Nes_Emu *emu;
    uint8_t *pixel_bufs[2]; /* allocated separately (may be in PSRAM) */
    int back_buf;
    int front_buf;
    bool initialized;
    bool rom_loaded;
    void *ext_tile_cache_buf;
    long ext_tile_cache_size;
    Nes_State *save_load_state;
    int prev_sprite_count;
};

/* Access the qnes_state_t pointer via the r9 register.
 * All code is compiled with -ffixed-r9.  main.c sets r9 to a
 * heap-allocated app_globals_t whose FIRST member is void *qnes_state.
 * So *(void **)r9 gives us the qnes_state_t pointer. */
static inline qnes_state_t *S(void) {
    void **r9_ptr;
    __asm__ volatile ("mov %0, r9" : "=r" (r9_ptr));
    return (qnes_state_t *)r9_ptr[0];
}

/* Size of qnes_state_t for external allocation */
extern "C" unsigned long qnes_state_size(void) {
    return sizeof(qnes_state_t);
}

void qnes_set_pixel_bufs(void *buf0, void *buf1)
{
    qnes_state_t *s = S();
    s->pixel_bufs[0] = (uint8_t *)buf0;
    s->pixel_bufs[1] = (uint8_t *)buf1;
}

void qnes_set_tile_cache_buf(void *buf, long size)
{
    S()->ext_tile_cache_buf = buf;
    S()->ext_tile_cache_size = size;
}

void *qnes_get_tile_cache_buf(long *out_size)
{
    if (out_size) *out_size = S()->ext_tile_cache_size;
    return S()->ext_tile_cache_buf;
}

int qnes_init(long sample_rate)
{
    qnes_state_t *s = S();
    if (s->initialized)
        return 0;

    s->emu = new Nes_Emu;
    if (!s->emu) {
        printf("qnes: failed to allocate Nes_Emu\n");
        return -1;
    }

    s->back_buf = 0;
    s->front_buf = 0;
    s->emu->set_pixels(s->pixel_bufs[s->back_buf], 256 + 16);

    const char *err = s->emu->set_sample_rate(sample_rate);
    if (err) {
        printf("set_sample_rate: %s\n", err);
        return -1;
    }

    s->emu->set_palette_range(0);
    s->initialized = true;
    return 0;
}

int qnes_load_rom(const void *data, long size)
{
    qnes_state_t *s = S();
    if (!s->initialized)
        return -1;

    Mem_File_Reader reader(data, size);
    Auto_File_Reader in(reader);
    const char *err = s->emu->load_ines(in);
    if (err) {
        printf("load_ines: %s\n", err);
        return -1;
    }

    s->rom_loaded = true;
    return 0;
}

int qnes_load_rom_inplace(const void *data, long size)
{
    qnes_state_t *s = S();
    if (!s->initialized)
        return -1;

    const char *err = s->emu->load_ines_data(data, size);
    if (err) {
        printf("load_ines_data: %s\n", err);
        return -1;
    }

    s->rom_loaded = true;
    return 0;
}

int qnes_emulate_frame(int joypad1, int joypad2)
{
    qnes_state_t *s = S();
    if (!s->rom_loaded)
        return -1;

    const char *err = s->emu->emulate_frame(joypad1, joypad2);
    if (err)
        return -1;

    /* Count visible sprites for blink detection */
    {
        const uint8_t *oam = s->emu->oam_data();
        int vis = 0;
        for (int i = 0; i < 64; i++)
            if (oam[i * 4] < 0xF0) vis++;
        s->emu->visible_sprite_count = vis;
    }

    /* Frame complete -- swap buffers */
    s->front_buf = s->back_buf;
    s->back_buf ^= 1;

    /* Sprite blink persistence */
    {
        int cur_count = s->emu->visible_sprite_count;

        if (s->prev_sprite_count - cur_count >= 3) {
            uint8_t *cur = s->pixel_bufs[s->front_buf];
            const uint8_t *prev = s->pixel_bufs[s->back_buf];
            for (int i = 0; i < PIXEL_BUF_SIZE; i++) {
                if (!(cur[i] & 0x10) && (prev[i] & 0x10))
                    cur[i] = prev[i];
            }
        }
        s->prev_sprite_count = cur_count;
    }

    s->emu->set_pixels(s->pixel_bufs[s->back_buf], 256 + 16);
    return 0;
}

const uint8_t *qnes_get_pixels(void)
{
    qnes_state_t *s = S();
    if (!s->rom_loaded)
        return 0;

    return s->emu->frame().pixels;
}

const int16_t *qnes_get_palette(int *out_size)
{
    qnes_state_t *s = S();
    if (!s->rom_loaded)
        return 0;

    const Nes_Emu::frame_t &f = s->emu->frame();
    if (out_size)
        *out_size = f.palette_size;
    return f.palette;
}

const qnes_rgb_t *qnes_get_color_table(void)
{
    /* nes_colors is a static const array -- lives in .rodata (flash), safe */
    return (const qnes_rgb_t *)Nes_Emu::nes_colors;
}

long qnes_read_samples(int16_t *out, long max_samples)
{
    qnes_state_t *s = S();
    if (!s->rom_loaded)
        return 0;

    return s->emu->read_samples(out, max_samples);
}

/* FatFS-backed Data_Writer: streams directly to an open file (no malloc) */
class FatFS_Writer : public Data_Writer {
    FIL *fil;
public:
    FatFS_Writer(FIL *f) : fil(f) {}
    const char *write(const void *p, long n) {
        UINT bw;
        FRESULT fr = f_write(fil, p, (UINT)n, &bw);
        if (fr != FR_OK || bw != (UINT)n) return "SD write error";
        return 0;
    }
};

/* FatFS-backed Data_Reader: streams directly from an open file (no malloc) */
class FatFS_Reader : public Data_Reader {
    FIL *fil;
public:
    FatFS_Reader(FIL *f, long size) : fil(f) { set_remain(size); }
    const char *read_v(void *p, int n) {
        UINT br;
        FRESULT fr = f_read(fil, p, (UINT)n, &br);
        if (fr != FR_OK || br != (UINT)n) return "SD read error";
        return 0;
    }
    const char *skip_v(int n) {
        FRESULT fr = f_lseek(fil, f_tell(fil) + n);
        if (fr != FR_OK) return "SD seek error";
        return 0;
    }
};

static Nes_State *get_save_load_state(void) {
    qnes_state_t *s = S();
    if (!s->save_load_state) {
        s->save_load_state = new Nes_State;
    }
    return s->save_load_state;
}

int qnes_save_state(qnes_file_t file)
{
    qnes_state_t *s = S();
    if (!s->rom_loaded) return -1;

    Nes_State *state = get_save_load_state();
    if (!state) return -1;

    s->emu->save_state(state);

    FatFS_Writer writer((FIL *)file);
    const char *err = state->write(Auto_File_Writer(writer));
    if (err) {
        printf("qnes_save_state: %s\n", err);
        return -1;
    }
    return 0;
}

int qnes_load_state(qnes_file_t file, long file_size)
{
    qnes_state_t *s = S();
    if (!s->rom_loaded) return -1;

    FatFS_Reader reader((FIL *)file, file_size);
    Auto_File_Reader in(reader);

    Nes_State *state = get_save_load_state();
    if (!state) return -1;

    const char *err = state->read(in);
    if (err) {
        printf("qnes_load_state: %s\n", err);
        return -1;
    }

    s->emu->load_state(*state);
    return 0;
}

void qnes_reset(int full_reset)
{
    qnes_state_t *s = S();
    if (s->rom_loaded)
        s->emu->reset(full_reset != 0);
}

void qnes_close(void)
{
    qnes_state_t *s = S();
    if (s->rom_loaded) {
        s->emu->close();
        s->rom_loaded = false;
    }
}
