/*
 * FRANK OS — FrankAmp (standalone ELF app)
 * WinAmp 2.x style MP3 player
 * Copyright (c) 2026 Mikhail Matveev <xtreme@rh1.tech>
 * https://rh1.tech
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "m-os-api.h"
#include "frankos-app.h"
#include "lang.h"

/* App-local translations */
enum { AL_MUSIC, AL_PLAY, AL_PAUSE, AL_STOP, AL_NEXT, AL_PREV, AL_SHUFFLE, AL_REPEAT, AL_ADD, AL_REMOVE, AL_OPEN_AUDIO, AL_ABOUT, AL_OPEN_MENU, AL_COUNT };
static const char *al_en[] = {
    [AL_MUSIC]      = "Music",
    [AL_PLAY]       = "Play",
    [AL_PAUSE]      = "Pause",
    [AL_STOP]       = "Stop",
    [AL_NEXT]       = "Next Track",
    [AL_PREV]       = "Previous Track",
    [AL_SHUFFLE]    = "Shuffle",
    [AL_REPEAT]     = "Repeat",
    [AL_ADD]        = "+Add",
    [AL_REMOVE]     = "-Rem",
    [AL_OPEN_AUDIO] = "Open Audio",
    [AL_ABOUT]      = "About FrankAmp",
    [AL_OPEN_MENU]  = "Open.. Ctrl+O",
};
static const char *al_ru[] = {
    [AL_MUSIC]      = "\xD0\x9C\xD1\x83\xD0\xB7\xD1\x8B\xD0\xBA\xD0\xB0",
    [AL_PLAY]       = "\xD0\x92\xD0\xBE\xD1\x81\xD0\xBF\xD1\x80\xD0\xBE\xD0\xB8\xD0\xB7\xD0\xB2\xD0\xB5\xD1\x81\xD1\x82\xD0\xB8",
    [AL_PAUSE]      = "\xD0\x9F\xD0\xB0\xD1\x83\xD0\xB7\xD0\xB0",
    [AL_STOP]       = "\xD0\xA1\xD1\x82\xD0\xBE\xD0\xBF",
    [AL_NEXT]       = "\xD0\xA1\xD0\xBB\xD0\xB5\xD0\xB4\xD1\x83\xD1\x8E\xD1\x89\xD0\xB8\xD0\xB9 \xD1\x82\xD1\x80\xD0\xB5\xD0\xBA",
    [AL_PREV]       = "\xD0\x9F\xD1\x80\xD0\xB5\xD0\xB4\xD1\x8B\xD0\xB4\xD1\x83\xD1\x89\xD0\xB8\xD0\xB9 \xD1\x82\xD1\x80\xD0\xB5\xD0\xBA",
    [AL_SHUFFLE]    = "\xD0\x9F\xD0\xB5\xD1\x80\xD0\xB5\xD0\xBC\xD0\xB5\xD1\x88\xD0\xB0\xD1\x82\xD1\x8C",
    [AL_REPEAT]     = "\xD0\x9F\xD0\xBE\xD0\xB2\xD1\x82\xD0\xBE\xD1\x80\xD1\x8F\xD1\x82\xD1\x8C",
    [AL_ADD]        = "+\xD0\x94\xD0\xBE\xD0\xB1\xD0\xB0\xD0\xB2\xD0\xB8\xD1\x82\xD1\x8C",
    [AL_REMOVE]     = "-\xD0\xA3\xD0\xB4\xD0\xB0\xD0\xBB\xD0\xB8\xD1\x82\xD1\x8C",
    [AL_OPEN_AUDIO] = "\xD0\x9E\xD1\x82\xD0\xBA\xD1\x80\xD1\x8B\xD1\x82\xD1\x8C \xD0\xB0\xD1\x83\xD0\xB4\xD0\xB8\xD0\xBE",
    [AL_ABOUT]      = "\xD0\x9E FrankAmp",
    [AL_OPEN_MENU]  = "\xD0\x9E\xD1\x82\xD0\xBA\xD1\x80\xD1\x8B\xD1\x82\xD1\x8C  Ctrl+O",
};
static const char *AL(int id) { return lang_get() == LANG_RU ? al_ru[id] : al_en[id]; }

/* UART debug printf */
#define dbg_printf(...) ((int(*)(const char*, ...))_sys_table_ptrs[438])(__VA_ARGS__)

/* xorshift32 PRNG for shuffle mode */
static uint32_t _prng_state;
static inline void fa_srand(unsigned s) { _prng_state = s ? s : 1; }
static inline int fa_rand(void) {
    uint32_t x = _prng_state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    _prng_state = x;
    return (int)(x & 0x7FFFFFFF);
}

/*==========================================================================
 * Constants — single-window layout
 *=========================================================================*/

#define CLIENT_W       275
#define CLIENT_H       248

#define MAX_PLAYLIST    64
#define MAX_PATH_LEN   256
#define MAX_TITLE_LEN   64
#define READ_BUF_SIZE  8192
#define MAX_FRAME_SAMP 1152    /* max samples/channel for MPEG1 Layer3 */

/* PCM output buffer: one MP3 frame stereo + padding for DMA overread */
#define PCM_BUF_LEN    (MAX_FRAME_SAMP * 2 + 1024)

/* 7-segment digit dimensions */
#define DIGIT_W  13
#define DIGIT_H  23

/* Transport button geometry */
#define BTN_Y   68
#define BTN_H   26

/* Seek bar */
#define SEEK_Y  58
#define SEEK_H   6

/* Toggle / volume row */
#define TOGGLE_Y  96
#define TOGGLE_H  14

/* Playlist layout */
#define PL_SEP_Y   112
#define PL_TOP_Y   114
#define PL_ROW_H    14
#define PL_ROWS       8
#define PL_BOTTOM_Y 226
#define PL_BOTTOM_H  22

/* Menu command IDs */
#define CMD_OPEN       100
#define CMD_EXIT       101
#define CMD_PLAY       200
#define CMD_PAUSE      201
#define CMD_STOP       202
#define CMD_NEXT       203
#define CMD_PREV       204
#define CMD_SHUFFLE    205
#define CMD_REPEAT     206
#define CMD_ABOUT      300

/* USB HID key codes */
#define KEY_SPACE   0x2C
#define KEY_ENTER   0x28
#define KEY_Z       0x1D
#define KEY_X       0x1B
#define KEY_C       0x06
#define KEY_V       0x19
#define KEY_B       0x05
#define KEY_O       0x12
#define KEY_UP      0x52
#define KEY_DOWN    0x51
#define KEY_PLUS    0x2E  /* +/= */
#define KEY_MINUS   0x2D
#define KEY_DELETE  0x4C
#define KEY_ESC     0x29

/*==========================================================================
 * Types
 *=========================================================================*/

typedef enum { PS_STOPPED, PS_PLAYING, PS_PAUSED } play_state_t;
typedef enum { FMT_MP3, FMT_MOD, FMT_MIDI } audio_format_t;

typedef struct {
    char path[MAX_PATH_LEN];
    char title[MAX_TITLE_LEN];
} playlist_entry_t;

typedef struct {
    /* Single window */
    hwnd_t          hwnd;

    /* Playback state */
    play_state_t    play_state;
    audio_format_t  format;
    bool            shuffle, repeat;
    HMP3Decoder     decoder;
    FIL             mp3_file;
    bool            file_open;
    bool            audio_active;    /* pcm_init called */

    /* MOD playback state */
    modcontext      mod_ctx;
    uint8_t        *mod_data;       /* MOD file loaded into memory */
    uint32_t        mod_data_size;

    /* MIDI playback state */
    midi_opl_t     *midi;

    /* Audio read buffer */
    uint8_t         read_buf[READ_BUF_SIZE];
    int             read_valid, read_offset;

    /* PCM decode buffer — one frame, written to DMA via pcm_write */
    int16_t         pcm_buf[PCM_BUF_LEN];

    /* Track info */
    MP3FrameInfo    info;
    uint32_t        file_size;
    uint32_t        elapsed_ms;
    uint32_t        total_ms;

    /* UI state */
    uint8_t         volume;          /* 0=quiet, 100=loud */
    bool            vol_drag;
    uint16_t        scroll_offset, scroll_tick;
    int8_t          btn_pressed;     /* transport button index, -1=none */

    /* Playlist (dynamically allocated — may live in PSRAM) */
    playlist_entry_t *playlist;
    int              pl_count, pl_current, pl_selected, pl_scroll;
    scrollbar_t      pl_scrollbar;

    /* Deferred play: path set by WM task, picked up by app task */
    char            pending_path[MAX_PATH_LEN];

    /* Deferred command from event handler (compositor task) — executed
     * in the main loop (app task) so pcm_init/pcm_cleanup use the
     * correct FreeRTOS thread-local storage for the snd channel. */
    volatile int8_t pending_cmd;  /* 0=none, 1=play, 2=stop, 3=next, 4=prev */

    /* System */
    TimerHandle_t   ui_timer;
    volatile bool   closing;
} frankamp_t;

/* Task handle for blocking main() */
static void *app_task;
static volatile bool app_closing;

/*==========================================================================
 * 7-segment bitmasks
 *=========================================================================*/

/*   AAA        bit 6=A  5=B  4=C  3=D  2=E  1=F  0=G
 *  F   B
 *   GGG
 *  E   C
 *   DDD
 */
static const uint8_t seg_digits[10] = {
    0x7E, 0x30, 0x6D, 0x79, 0x33,
    0x5B, 0x5F, 0x70, 0x7F, 0x7B,
};

/*==========================================================================
 * Audio engine — file I/O and decoding
 *=========================================================================*/

/* Skip ID3v2 tag at the start of an MP3 file.
 * Many files embed album art (hundreds of KB) that would exhaust the
 * read buffer and retry count before reaching actual MP3 frames. */
static void skip_id3v2(frankamp_t *fa) {
    uint8_t hdr[10];
    UINT br;
    if (f_read(&fa->mp3_file, hdr, 10, &br) != FR_OK || br < 10) {
        f_lseek(&fa->mp3_file, 0);
        return;
    }
    if (hdr[0] == 'I' && hdr[1] == 'D' && hdr[2] == '3') {
        /* Size is a 28-bit syncsafe integer (7 bits per byte) */
        uint32_t tag_size = ((uint32_t)(hdr[6] & 0x7F) << 21) |
                            ((uint32_t)(hdr[7] & 0x7F) << 14) |
                            ((uint32_t)(hdr[8] & 0x7F) <<  7) |
                             (uint32_t)(hdr[9] & 0x7F);
        tag_size += 10;  /* add header itself */
        dbg_printf("[frankamp] skipping ID3v2 tag (%u bytes)\n", tag_size);
        f_lseek(&fa->mp3_file, tag_size);
    } else {
        /* No ID3v2 tag — rewind */
        f_lseek(&fa->mp3_file, 0);
    }
}

static int refill_read_buf(frankamp_t *fa) {
    /* Shift remaining data to front */
    int remain = fa->read_valid - fa->read_offset;
    if (remain > 0 && fa->read_offset > 0)
        memmove(fa->read_buf, fa->read_buf + fa->read_offset, remain);
    else if (remain < 0)
        remain = 0;
    fa->read_offset = 0;
    fa->read_valid = remain;

    UINT br = 0;
    FRESULT res = f_read(&fa->mp3_file,
                         fa->read_buf + fa->read_valid,
                         READ_BUF_SIZE - fa->read_valid, &br);
    if (res != FR_OK)
        return -1;
    fa->read_valid += (int)br;
    return fa->read_valid;
}

/* Decode one MP3 frame into fa->pcm_buf.
 * Returns the number of stereo frames decoded, or -1 on error/EOF. */
static int decode_frame(frankamp_t *fa) {
    int16_t *out = fa->pcm_buf;

    /* Zero the buffer (including padding) for clean DMA overreads */
    memset(out, 0, PCM_BUF_LEN * sizeof(int16_t));

    for (int retry = 0; retry < 10; retry++) {
        /* Ensure we have data */
        if (fa->read_valid - fa->read_offset < 512) {
            if (refill_read_buf(fa) <= 0 && fa->read_valid == 0)
                return -1;  /* EOF or read error */
        }

        /* Find sync word */
        unsigned char *ptr = fa->read_buf + fa->read_offset;
        int avail = fa->read_valid - fa->read_offset;
        int sync = MP3FindSyncWord(ptr, avail);
        if (sync < 0) {
            fa->read_offset = fa->read_valid;  /* discard */
            continue;
        }
        fa->read_offset += sync;

        /* Decode */
        ptr = fa->read_buf + fa->read_offset;
        avail = fa->read_valid - fa->read_offset;
        int err = MP3Decode(fa->decoder, &ptr, &avail, out, 0);

        /* Update read offset — ptr was advanced by MP3Decode */
        fa->read_offset = (int)(ptr - fa->read_buf);

        if (err == 0) {
            MP3GetLastFrameInfo(fa->decoder, &fa->info);
            int nch = fa->info.nChans;
            if (nch < 1) nch = 1;
            int stereo_frames = fa->info.outputSamps / nch;

            /* Apply software volume: 0=quiet, 100=loud */
            if (fa->volume < 100) {
                int shift = ((100 - fa->volume) * 8 + 50) / 100;
                if (shift > 0) {
                    int total = fa->info.outputSamps;
                    for (int i = 0; i < total; i++)
                        out[i] >>= shift;
                }
            }

            /* If mono, duplicate L to R */
            if (nch == 1) {
                for (int i = stereo_frames - 1; i >= 0; i--) {
                    out[i * 2 + 1] = out[i];
                    out[i * 2]     = out[i];
                }
            }

            return stereo_frames;
        }
        /* Decode error — skip a byte and retry */
        fa->read_offset++;
    }
    return -1;
}

/* Estimate total duration from bitrate and file size */
static void estimate_duration(frankamp_t *fa) {
    if (fa->info.bitrate > 0)
        fa->total_ms = (uint32_t)((uint64_t)fa->file_size * 8000 / fa->info.bitrate);
    else
        fa->total_ms = 0;
}

/*==========================================================================
 * Format detection
 *=========================================================================*/

static audio_format_t detect_format(const char *path) {
    const char *dot = 0;
    for (const char *p = path; *p; p++)
        if (*p == '.') dot = p;
    if (dot) {
        if ((dot[1]=='m'||dot[1]=='M') && (dot[2]=='o'||dot[2]=='O')
            && (dot[3]=='d'||dot[3]=='D') && dot[4]=='\0')
            return FMT_MOD;
        if ((dot[1]=='m'||dot[1]=='M') && (dot[2]=='i'||dot[2]=='I')
            && (dot[3]=='d'||dot[3]=='D')) {
            if (dot[4]=='\0') return FMT_MIDI;
            if ((dot[4]=='i'||dot[4]=='I') && dot[5]=='\0') return FMT_MIDI;
        }
    }
    return FMT_MP3;
}

/*==========================================================================
 * MOD decoding
 *=========================================================================*/

#define MOD_CHUNK_FRAMES  1024  /* ~23ms at 44100 Hz */

static int decode_frame_mod(frankamp_t *fa) {
    hxcmod_fillbuffer(&fa->mod_ctx, (msample *)fa->pcm_buf,
                      MOD_CHUNK_FRAMES, 0);
    /* Apply volume */
    if (fa->volume < 100) {
        int shift = ((100 - fa->volume) * 8 + 50) / 100;
        if (shift > 0)
            for (int i = 0; i < MOD_CHUNK_FRAMES * 2; i++)
                fa->pcm_buf[i] >>= shift;
    }
    return MOD_CHUNK_FRAMES;
}

/*==========================================================================
 * MIDI decoding
 *=========================================================================*/

#define MIDI_CHUNK_FRAMES  1024  /* ~23ms at 44100 Hz */

static int decode_frame_midi(frankamp_t *fa) {
    int nf = midi_opl_render(fa->midi, fa->pcm_buf, MIDI_CHUNK_FRAMES);
    if (nf > 0) {
        /* Apply OPL gain (×8) and user volume in one 32-bit step.
         * The OPL output is already >>1 and clamped to int16 (±18k peak
         * at 9 voices).  Applying <<3 then >>vol_shift separately would
         * clip at ≥5 voices, destroying dynamics.  Combining them into
         * one net shift preserves the full signal. */
        int vol_shift = (fa->volume < 100)
                      ? ((100 - fa->volume) * 8 + 50) / 100
                      : 0;
        int net_shift = 4 - vol_shift;  /* <<4 base gain minus >>vol attenuation */
        for (int i = 0; i < nf * 2; i++) {
            int32_t s = fa->pcm_buf[i];
            s = (net_shift >= 0) ? (s << net_shift) : (s >> -net_shift);
            if (s > 32767) s = 32767;
            else if (s < -32768) s = -32768;
            fa->pcm_buf[i] = (int16_t)s;
        }
    }
    return nf;
}

/*==========================================================================
 * Playback control
 *=========================================================================*/

static void play_stop(frankamp_t *fa) {
    if (fa->play_state == PS_STOPPED)
        return;

    fa->play_state = PS_STOPPED;

    if (fa->audio_active) {
        pcm_cleanup();
        fa->audio_active = false;
    }

    if (fa->format == FMT_MIDI) {
        if (fa->midi) {
            midi_opl_free(fa->midi);
            fa->midi = 0;
        }
    } else if (fa->format == FMT_MOD) {
        hxcmod_unload(&fa->mod_ctx);
        if (fa->mod_data) {
            psram_free(fa->mod_data);  /* handles both PSRAM and SRAM */
            fa->mod_data = 0;
        }
    } else {
        if (fa->decoder) {
            MP3FreeDecoder(fa->decoder);
            fa->decoder = 0;
        }

        if (fa->file_open) {
            f_close(&fa->mp3_file);
            fa->file_open = false;
        }
    }

    fa->elapsed_ms = 0;
}

static bool play_start(frankamp_t *fa, int playlist_idx) {
    play_stop(fa);

    if (playlist_idx < 0 || playlist_idx >= fa->pl_count)
        return false;

    fa->pl_current = playlist_idx;
    const char *path = fa->playlist[playlist_idx].path;
    fa->format = detect_format(path);

    if (fa->format == FMT_MIDI) {
        /* === MIDI playback === */
        fa->midi = midi_opl_init();
        if (!fa->midi) {
            dbg_printf("[frankamp] midi_opl_init failed\n");
            return false;
        }
        midi_opl_set_loop(fa->midi, fa->repeat);
        if (!midi_opl_load(fa->midi, path)) {
            dbg_printf("[frankamp] midi_opl_load failed: %s\n", path);
            midi_opl_free(fa->midi);
            fa->midi = 0;
            return false;
        }

        fa->info.samprate = 44100;
        fa->info.nChans = 2;
        fa->info.bitrate = 0;
        fa->total_ms = 0;  /* MIDI duration unknown */
        fa->elapsed_ms = 0;

        pcm_init(44100, 2);
        fa->audio_active = true;
        fa->play_state = PS_PLAYING;

        /* Decode and write first chunk to kick off DMA */
        int nf = decode_frame_midi(fa);
        if (nf > 0) pcm_write(fa->pcm_buf, nf);

        dbg_printf("[frankamp] playing MIDI: %s (44100 Hz, stereo, OPL FM)\n",
                   path);
    } else if (fa->format == FMT_MOD) {
        /* === MOD playback === */
        FIL f;
        FRESULT res = f_open(&f, path, FA_READ | FA_OPEN_EXISTING);
        if (res != FR_OK) {
            dbg_printf("[frankamp] f_open MOD failed: %d\n", res);
            return false;
        }
        fa->mod_data_size = (uint32_t)f_size(&f);
        /* MOD files can be 100KB+; use PSRAM if available */
        if (psram_is_available())
            fa->mod_data = (uint8_t *)psram_alloc(fa->mod_data_size);
        else
            fa->mod_data = (uint8_t *)malloc(fa->mod_data_size);
        if (!fa->mod_data) {
            dbg_printf("[frankamp] MOD alloc failed (%u bytes)\n",
                       fa->mod_data_size);
            f_close(&f);
            return false;
        }
        UINT br;
        res = f_read(&f, fa->mod_data, fa->mod_data_size, &br);
        f_close(&f);
        if (res != FR_OK || br != fa->mod_data_size) {
            dbg_printf("[frankamp] MOD read failed\n");
            free(fa->mod_data);
            fa->mod_data = 0;
            return false;
        }

        hxcmod_init(&fa->mod_ctx);
        hxcmod_setcfg(&fa->mod_ctx, 44100, 1, 1);
        if (!hxcmod_load(&fa->mod_ctx, fa->mod_data, (int)fa->mod_data_size)) {
            dbg_printf("[frankamp] hxcmod_load failed\n");
            free(fa->mod_data);
            fa->mod_data = 0;
            return false;
        }

        fa->info.samprate = 44100;
        fa->info.nChans = 2;
        fa->info.bitrate = 0;
        fa->total_ms = 0;  /* MOD loops — no fixed duration */
        fa->elapsed_ms = 0;

        pcm_init(44100, 2);
        fa->audio_active = true;
        fa->play_state = PS_PLAYING;

        /* Decode and write first chunk to kick off DMA */
        int nf = decode_frame_mod(fa);
        pcm_write(fa->pcm_buf, nf);

        dbg_printf("[frankamp] playing MOD: %s (44100 Hz, stereo)\n", path);
    } else {
        /* === MP3 playback === */
        FRESULT res = f_open(&fa->mp3_file, path, FA_READ | FA_OPEN_EXISTING);
        if (res != FR_OK) {
            dbg_printf("[frankamp] f_open failed: %d\n", res);
            return false;
        }
        fa->file_open = true;
        fa->file_size = (uint32_t)f_size(&fa->mp3_file);

        fa->decoder = MP3InitDecoder();
        if (!fa->decoder) {
            dbg_printf("[frankamp] MP3InitDecoder failed\n");
            f_close(&fa->mp3_file);
            fa->file_open = false;
            return false;
        }

        fa->read_valid = 0;
        fa->read_offset = 0;
        skip_id3v2(fa);

        int nf = decode_frame(fa);
        if (nf <= 0) {
            dbg_printf("[frankamp] first decode failed\n");
            play_stop(fa);
            return false;
        }

        estimate_duration(fa);
        fa->elapsed_ms = 0;

        int sr = fa->info.samprate;
        if (sr <= 0) sr = 44100;
        pcm_init(sr, 2);
        fa->audio_active = true;
        fa->play_state = PS_PLAYING;

        pcm_write(fa->pcm_buf, nf);

        dbg_printf("[frankamp] playing: %s (%d Hz, %d kbps, %d ch)\n",
                   path, fa->info.samprate, fa->info.bitrate / 1000,
                   fa->info.nChans);
    }
    return true;
}

static void play_pause(frankamp_t *fa) {
    if (fa->play_state == PS_PLAYING)
        fa->play_state = PS_PAUSED;
    else if (fa->play_state == PS_PAUSED)
        fa->play_state = PS_PLAYING;
}

static void play_next(frankamp_t *fa) {
    if (fa->pl_count == 0) return;
    int next;
    if (fa->shuffle) {
        fa_srand((unsigned)xTaskGetTickCount());
        next = fa_rand() % fa->pl_count;
    } else {
        next = fa->pl_current + 1;
        if (next >= fa->pl_count) {
            if (fa->repeat)
                next = 0;
            else {
                play_stop(fa);
                return;
            }
        }
    }
    play_start(fa, next);
}

static void play_prev(frankamp_t *fa) {
    if (fa->pl_count == 0) return;
    /* If >3s into track, restart; otherwise go to previous */
    if (fa->elapsed_ms > 3000) {
        play_start(fa, fa->pl_current);
        return;
    }
    int prev = fa->pl_current - 1;
    if (prev < 0) prev = fa->pl_count - 1;
    play_start(fa, prev);
}

/* Decode one frame and push to I2S (blocking).  Returns false at EOF. */
static bool audio_step(frankamp_t *fa) {
    if (fa->format == FMT_MIDI) {
        int nf = decode_frame_midi(fa);
        if (nf <= 0) return false;
        pcm_write(fa->pcm_buf, nf);
        fa->elapsed_ms += nf * 1000 / 44100;
        return midi_opl_playing(fa->midi);
    }
    if (fa->format == FMT_MOD) {
        int nf = decode_frame_mod(fa);
        pcm_write(fa->pcm_buf, nf);
        fa->elapsed_ms += MOD_CHUNK_FRAMES * 1000 / 44100;
        return true;  /* MOD loops forever */
    }
    int nf = decode_frame(fa);
    if (nf <= 0)
        return false;
    pcm_write(fa->pcm_buf, nf);
    fa->elapsed_ms += (uint32_t)((uint64_t)nf * 1000 / fa->info.samprate);
    return true;
}

/*==========================================================================
 * Playlist management
 *=========================================================================*/

static void extract_title(const char *path, char *title, int max_len) {
    /* Find last '/' */
    const char *name = path;
    for (const char *p = path; *p; p++)
        if (*p == '/')
            name = p + 1;
    /* Copy without extension */
    int i = 0;
    for (; name[i] && name[i] != '.' && i < max_len - 1; i++)
        title[i] = name[i];
    title[i] = '\0';
}

static int playlist_add(frankamp_t *fa, const char *path) {
    if (fa->pl_count >= MAX_PLAYLIST)
        return -1;
    int idx = fa->pl_count;
    strncpy(fa->playlist[idx].path, path, MAX_PATH_LEN - 1);
    fa->playlist[idx].path[MAX_PATH_LEN - 1] = '\0';
    extract_title(path, fa->playlist[idx].title, MAX_TITLE_LEN);
    fa->pl_count++;
    return idx;
}

static void playlist_remove(frankamp_t *fa, int idx) {
    if (idx < 0 || idx >= fa->pl_count)
        return;
    /* If removing currently playing track, stop */
    if (idx == fa->pl_current && fa->play_state != PS_STOPPED)
        play_stop(fa);
    /* Shift down */
    for (int i = idx; i < fa->pl_count - 1; i++)
        fa->playlist[i] = fa->playlist[i + 1];
    fa->pl_count--;
    /* Adjust indices */
    if (fa->pl_current > idx)
        fa->pl_current--;
    if (fa->pl_current >= fa->pl_count)
        fa->pl_current = fa->pl_count - 1;
    if (fa->pl_selected > idx)
        fa->pl_selected--;
    if (fa->pl_selected >= fa->pl_count)
        fa->pl_selected = fa->pl_count - 1;
}

/* Keep selected item visible in the playlist scroll region */
static void ensure_visible(frankamp_t *fa) {
    if (fa->pl_selected < fa->pl_scroll)
        fa->pl_scroll = fa->pl_selected;
    else if (fa->pl_selected >= fa->pl_scroll + PL_ROWS)
        fa->pl_scroll = fa->pl_selected - PL_ROWS + 1;
    if (fa->pl_scroll < 0)
        fa->pl_scroll = 0;
}

/*==========================================================================
 * Timer callback (UI updates)
 *=========================================================================*/

static void ui_timer_cb(TimerHandle_t xTimer) {
    frankamp_t *fa = (frankamp_t *)pvTimerGetTimerID(xTimer);
    if (!fa) return;

    /* Scroll title text every 300ms */
    fa->scroll_tick++;
    if (fa->scroll_tick >= 3) {
        fa->scroll_tick = 0;
        fa->scroll_offset++;
    }

    wm_invalidate(fa->hwnd);
}

/*==========================================================================
 * Rendering — 7-segment LED digits
 *=========================================================================*/

static void draw_seg_h(int16_t x, int16_t y, int16_t w, uint8_t color) {
    wd_hline(x + 1, y, w - 2, color);
}

static void draw_seg_v(int16_t x, int16_t y, int16_t h, uint8_t color) {
    for (int16_t i = 1; i < h - 1; i++)
        wd_pixel(x, y + i, color);
}

static void draw_digit(int16_t x, int16_t y, int digit, uint8_t fg, uint8_t bg) {
    wd_fill_rect(x, y, DIGIT_W, DIGIT_H, bg);
    if (digit < 0 || digit > 9) return;
    uint8_t segs = seg_digits[digit];
    int16_t sw = DIGIT_W;
    int16_t sh = DIGIT_H / 2;
    if (segs & 0x40) draw_seg_h(x + 1, y + 1, sw - 2, fg);
    if (segs & 0x20) draw_seg_v(x + sw - 2, y + 1, sh, fg);
    if (segs & 0x10) draw_seg_v(x + sw - 2, y + sh, sh, fg);
    if (segs & 0x08) draw_seg_h(x + 1, y + DIGIT_H - 2, sw - 2, fg);
    if (segs & 0x04) draw_seg_v(x + 1, y + sh, sh, fg);
    if (segs & 0x02) draw_seg_v(x + 1, y + 1, sh, fg);
    if (segs & 0x01) draw_seg_h(x + 1, y + sh, sw - 2, fg);
}

static void draw_time(int16_t x, int16_t y, uint32_t ms) {
    uint32_t total_sec = ms / 1000;
    int mm = (int)(total_sec / 60);
    int ss = (int)(total_sec % 60);
    if (mm > 99) mm = 99;

    draw_digit(x, y, mm / 10, COLOR_GREEN, COLOR_WHITE);
    draw_digit(x + DIGIT_W + 1, y, mm % 10, COLOR_GREEN, COLOR_WHITE);
    /* Colon */
    int16_t cx = x + DIGIT_W * 2 + 3;
    wd_pixel(cx, y + DIGIT_H / 3, COLOR_GREEN);
    wd_pixel(cx, y + DIGIT_H * 2 / 3, COLOR_GREEN);
    wd_pixel(cx + 1, y + DIGIT_H / 3, COLOR_GREEN);
    wd_pixel(cx + 1, y + DIGIT_H * 2 / 3, COLOR_GREEN);
    draw_digit(cx + 4, y, ss / 10, COLOR_GREEN, COLOR_WHITE);
    draw_digit(cx + 4 + DIGIT_W + 1, y, ss % 10, COLOR_GREEN, COLOR_WHITE);
}

/*==========================================================================
 * Rendering — buttons
 *=========================================================================*/

static void draw_3d_button(int16_t x, int16_t y, int16_t w, int16_t h,
                           bool pressed) {
    wd_fill_rect(x, y, w, h, COLOR_LIGHT_GRAY);
    if (pressed) {
        wd_hline(x, y, w, COLOR_DARK_GRAY);
        wd_vline(x, y, h, COLOR_DARK_GRAY);
        wd_hline(x, y + h - 1, w, COLOR_WHITE);
        wd_vline(x + w - 1, y, h, COLOR_WHITE);
    } else {
        wd_hline(x, y, w, COLOR_WHITE);
        wd_vline(x, y, h, COLOR_WHITE);
        wd_hline(x, y + h - 1, w, COLOR_DARK_GRAY);
        wd_vline(x + w - 1, y, h, COLOR_DARK_GRAY);
    }
}

/* Transport button icon drawing */
static void draw_icon_prev(int16_t x, int16_t y, int16_t w, int16_t h) {
    int16_t cx = x + w / 2, cy = y + h / 2;
    /* Bar */
    wd_vline(cx - 6, cy - 5, 11, COLOR_BLACK);
    wd_vline(cx - 5, cy - 5, 11, COLOR_BLACK);
    /* Triangle pointing left */
    for (int i = 0; i < 6; i++)
        wd_vline(cx - 4 + i, cy - i, i * 2 + 1, COLOR_BLACK);
}

static void draw_icon_play(int16_t x, int16_t y, int16_t w, int16_t h) {
    int16_t cx = x + w / 2, cy = y + h / 2;
    for (int i = 0; i < 8; i++)
        wd_vline(cx - 4 + i, cy - 7 + i, 15 - i * 2, COLOR_BLACK);
}

static void draw_icon_pause(int16_t x, int16_t y, int16_t w, int16_t h) {
    int16_t cx = x + w / 2, cy = y + h / 2;
    wd_fill_rect(cx - 5, cy - 5, 4, 11, COLOR_BLACK);
    wd_fill_rect(cx + 2, cy - 5, 4, 11, COLOR_BLACK);
}

static void draw_icon_stop(int16_t x, int16_t y, int16_t w, int16_t h) {
    int16_t cx = x + w / 2, cy = y + h / 2;
    wd_fill_rect(cx - 5, cy - 5, 10, 10, COLOR_BLACK);
}

static void draw_icon_next(int16_t x, int16_t y, int16_t w, int16_t h) {
    int16_t cx = x + w / 2, cy = y + h / 2;
    /* Triangle pointing right */
    for (int i = 0; i < 6; i++)
        wd_vline(cx + 4 - i, cy - i, i * 2 + 1, COLOR_BLACK);
    /* Bar */
    wd_vline(cx + 5, cy - 5, 11, COLOR_BLACK);
    wd_vline(cx + 6, cy - 5, 11, COLOR_BLACK);
}

/* Transport button definitions */
typedef struct {
    int16_t x, w;
    void (*draw_icon)(int16_t, int16_t, int16_t, int16_t);
} btn_def_t;

static const btn_def_t transport_btns[] = {
    {   4, 38, draw_icon_prev  },  /* 0: Prev */
    {  44, 42, draw_icon_play  },  /* 1: Play */
    {  88, 38, draw_icon_pause },  /* 2: Pause */
    { 128, 38, draw_icon_stop  },  /* 3: Stop */
    { 168, 38, draw_icon_next  },  /* 4: Next */
};
#define NUM_TRANSPORT 5

/*==========================================================================
 * Rendering — sunken panel
 *=========================================================================*/

static void draw_sunken_panel(int16_t x, int16_t y, int16_t w, int16_t h) {
    wd_hline(x, y, w, COLOR_DARK_GRAY);
    wd_vline(x, y, h, COLOR_DARK_GRAY);
    wd_hline(x, y + h - 1, w, COLOR_WHITE);
    wd_vline(x + w - 1, y, h, COLOR_WHITE);
}

/*==========================================================================
 * Rendering — single-window paint
 *=========================================================================*/

static void main_paint(hwnd_t hwnd) {
    window_t *win = wm_get_window(hwnd);
    if (!win) return;
    frankamp_t *fa = (frankamp_t *)win->user_data;
    if (!fa) return;

    wd_begin(hwnd);
    wd_clear(COLOR_LIGHT_GRAY);

    /* ---- Display panel (sunken bevel, y=0..55, white fill) ---- */
    draw_sunken_panel(0, 0, CLIENT_W, 56);
    wd_fill_rect(1, 1, CLIENT_W - 2, 54, COLOR_WHITE);

    /* 7-segment time display (top-left of panel, green on white) */
    draw_time(8, 4, fa->elapsed_ms);

    /* Scrolling track title (right of time, black on white) */
    if (fa->pl_current >= 0 && fa->pl_current < fa->pl_count) {
        const char *title = fa->playlist[fa->pl_current].title;
        int tlen = (int)strlen(title);
        int visible_chars = (193 / FONT_UI_WIDTH);

        if (tlen <= visible_chars) {
            wd_text_ui(75, 6, title, COLOR_BLACK, COLOR_WHITE);
        } else {
            char scroll_buf[34];
            int off = (int)(fa->scroll_offset % (tlen + 5));
            int j = 0;
            for (int i = 0; i < visible_chars && j < 33; i++) {
                int si = (off + i) % (tlen + 5);
                scroll_buf[j++] = (si < tlen) ? title[si] : ' ';
            }
            scroll_buf[j] = '\0';
            wd_text_ui(75, 6, scroll_buf, COLOR_BLACK, COLOR_WHITE);
        }
    } else {
        wd_text_ui(75, 6, "No track loaded", COLOR_DARK_GRAY, COLOR_WHITE);
    }

    /* Bitrate info line (dark gray on white) */
    if (fa->play_state != PS_STOPPED) {
        char info_str[40];
        if (fa->format == FMT_MIDI) {
            snprintf(info_str, sizeof(info_str), "MIDI  %d Hz  OPL FM",
                     fa->info.samprate);
        } else if (fa->format == FMT_MOD) {
            snprintf(info_str, sizeof(info_str), "MOD  %d Hz  stereo",
                     fa->info.samprate);
        } else if (fa->info.bitrate > 0) {
            snprintf(info_str, sizeof(info_str), "%d kbps  %d Hz  %s",
                     fa->info.bitrate / 1000,
                     fa->info.samprate,
                     fa->info.nChans == 2 ? "stereo" : "mono");
        } else {
            info_str[0] = '\0';
        }
        if (info_str[0])
            wd_text_ui(75, 20, info_str, COLOR_DARK_GRAY, COLOR_WHITE);
    }

    /* Play state indicator (green on white for active, gray for stopped) */
    if (fa->play_state == PS_PLAYING)
        wd_text_ui(8, 32, "> Playing", COLOR_GREEN, COLOR_WHITE);
    else if (fa->play_state == PS_PAUSED)
        wd_text_ui(8, 32, "|| Paused", COLOR_GREEN, COLOR_WHITE);
    else
        wd_text_ui(8, 32, "  Stopped", COLOR_DARK_GRAY, COLOR_WHITE);

    /* Track N/M (dark gray on white) */
    if (fa->pl_count > 0) {
        char track_str[24];
        snprintf(track_str, sizeof(track_str), "Track %d/%d",
                 fa->pl_current + 1, fa->pl_count);
        wd_text_ui(8, 44, track_str, COLOR_DARK_GRAY, COLOR_WHITE);
    }

    /* ---- Seek bar (y=58, h=6, sunken track) ---- */
    wd_hline(4, SEEK_Y, CLIENT_W - 8, COLOR_DARK_GRAY);
    wd_hline(4, SEEK_Y + SEEK_H - 1, CLIENT_W - 8, COLOR_WHITE);
    wd_fill_rect(4, SEEK_Y + 1, CLIENT_W - 8, SEEK_H - 2, COLOR_LIGHT_GRAY);
    if (fa->total_ms > 0 && fa->elapsed_ms > 0) {
        int fill = (int)((uint64_t)(CLIENT_W - 8) * fa->elapsed_ms / fa->total_ms);
        if (fill > CLIENT_W - 8) fill = CLIENT_W - 8;
        wd_fill_rect(4, SEEK_Y + 1, (int16_t)fill, SEEK_H - 2, COLOR_BLUE);
    }

    /* ---- Transport buttons (y=68, h=26) ---- */
    for (int i = 0; i < NUM_TRANSPORT; i++) {
        bool pressed = (fa->btn_pressed == i);
        draw_3d_button(transport_btns[i].x, BTN_Y,
                       transport_btns[i].w, BTN_H, pressed);
        transport_btns[i].draw_icon(transport_btns[i].x, BTN_Y,
                                    transport_btns[i].w, BTN_H);
    }

    /* Open button (standard Win95 button) */
    wd_button(228, BTN_Y, 50, BTN_H, L(STR_APP_OPEN), false, false);

    /* ---- Toggles + Volume row (y=96, h=14) ---- */
    draw_3d_button(4, TOGGLE_Y, 34, TOGGLE_H, fa->shuffle);
    wd_text_ui(9, TOGGLE_Y + 1, "SH", COLOR_BLACK, COLOR_LIGHT_GRAY);

    draw_3d_button(40, TOGGLE_Y, 34, TOGGLE_H, fa->repeat);
    wd_text_ui(45, TOGGLE_Y + 1, "RP", COLOR_BLACK, COLOR_LIGHT_GRAY);

    /* Volume label (black on gray) */
    wd_text_ui(88, 97, "VOL", COLOR_BLACK, COLOR_LIGHT_GRAY);

    /* Volume slider track (sunken groove) */
    int16_t vtrack_x = 108;
    int16_t vtrack_w = 159;
    wd_hline(vtrack_x, 100, vtrack_w, COLOR_DARK_GRAY);
    wd_hline(vtrack_x, 104, vtrack_w, COLOR_WHITE);
    wd_fill_rect(vtrack_x, 101, vtrack_w, 3, COLOR_LIGHT_GRAY);

    /* Fill portion (blue) */
    int vfill = vtrack_w * fa->volume / 100;
    if (vfill > vtrack_w) vfill = vtrack_w;
    if (vfill > 0)
        wd_fill_rect(vtrack_x, 101, (int16_t)vfill, 3, COLOR_BLUE);

    /* Volume thumb (raised 3D knob) */
    int16_t thumb_x = vtrack_x + (int16_t)vfill - 3;
    if (thumb_x < vtrack_x) thumb_x = vtrack_x;
    wd_fill_rect(thumb_x, 97, 6, 10, COLOR_LIGHT_GRAY);
    wd_hline(thumb_x, 97, 6, COLOR_WHITE);
    wd_vline(thumb_x, 97, 10, COLOR_WHITE);
    wd_hline(thumb_x, 106, 6, COLOR_DARK_GRAY);
    wd_vline(thumb_x + 5, 97, 10, COLOR_DARK_GRAY);

    /* ---- Playlist separator (y=112, h=2) ---- */
    wd_hline(0, PL_SEP_Y, CLIENT_W, COLOR_DARK_GRAY);
    wd_hline(0, PL_SEP_Y + 1, CLIENT_W, COLOR_WHITE);

    /* ---- Playlist area ---- */
    /* Update scrollbar range and sync position */
    bool pl_need_scroll = (fa->pl_count > PL_ROWS);
    int16_t pl_w = pl_need_scroll ? (CLIENT_W - SCROLLBAR_WIDTH) : CLIENT_W;

    if (pl_need_scroll) {
        fa->pl_scrollbar.x = CLIENT_W - SCROLLBAR_WIDTH;
        fa->pl_scrollbar.y = PL_TOP_Y;
        fa->pl_scrollbar.w = SCROLLBAR_WIDTH;
        fa->pl_scrollbar.h = PL_ROWS * PL_ROW_H;
        fa->pl_scrollbar.visible = true;
        scrollbar_set_range(&fa->pl_scrollbar, fa->pl_count, PL_ROWS);
        scrollbar_set_pos(&fa->pl_scrollbar, fa->pl_scroll);
    } else {
        fa->pl_scrollbar.visible = false;
    }

    wd_fill_rect(0, PL_TOP_Y, pl_w, PL_ROWS * PL_ROW_H, COLOR_WHITE);

    /* Playlist rows */
    for (int row = 0; row < PL_ROWS; row++) {
        int idx = fa->pl_scroll + row;
        int16_t ry = PL_TOP_Y + row * PL_ROW_H;

        if (idx >= fa->pl_count)
            continue;

        uint8_t fg, bg;
        if (idx == fa->pl_selected) {
            fg = COLOR_WHITE;
            bg = COLOR_BLUE;
        } else if (idx == fa->pl_current && fa->play_state != PS_STOPPED) {
            fg = COLOR_BLACK;
            bg = COLOR_LIGHT_GRAY;
        } else {
            fg = COLOR_BLACK;
            bg = COLOR_WHITE;
        }

        wd_fill_rect(0, ry, pl_w, PL_ROW_H, bg);

        char line[48];
        const char *prefix = (idx == fa->pl_current &&
                              fa->play_state != PS_STOPPED) ? "\x10" : " ";
        snprintf(line, sizeof(line), "%s%d. %s",
                 prefix, idx + 1, fa->playlist[idx].title);
        wd_text_ui(4, ry + 1, line, fg, bg);
    }

    /* Playlist scrollbar */
    if (pl_need_scroll)
        scrollbar_paint(&fa->pl_scrollbar);

    /* ---- Bottom bar (y=226, h=22, gray) ---- */
    wd_fill_rect(0, PL_BOTTOM_Y, CLIENT_W, PL_BOTTOM_H, COLOR_LIGHT_GRAY);
    wd_hline(0, PL_BOTTOM_Y, CLIENT_W, COLOR_DARK_GRAY);

    /* +Add button (standard Win95 button) */
    wd_button(4, 229, 60, 16, AL(AL_ADD), false, false);

    /* -Rem button (standard Win95 button) */
    wd_button(68, 229, 60, 16, AL(AL_REMOVE), false, false);

    /* Track count (black on gray) */
    char count_str[20];
    snprintf(count_str, sizeof(count_str), "%d tracks", fa->pl_count);
    int16_t count_x = CLIENT_W - (int16_t)(gfx_utf8_charcount(count_str) * FONT_UI_WIDTH) - 8;
    wd_text_ui(count_x, 231, count_str, COLOR_BLACK, COLOR_LIGHT_GRAY);

    wd_end();
}

/*==========================================================================
 * Hit testing
 *=========================================================================*/

static int hit_transport(int16_t mx, int16_t my) {
    if (my < BTN_Y || my >= BTN_Y + BTN_H) return -1;
    for (int i = 0; i < NUM_TRANSPORT; i++) {
        if (mx >= transport_btns[i].x &&
            mx < transport_btns[i].x + transport_btns[i].w)
            return i;
    }
    return -1;
}

static bool hit_open(int16_t mx, int16_t my) {
    return mx >= 228 && mx < 270 && my >= BTN_Y && my < BTN_Y + BTN_H;
}

static int hit_toggle(int16_t mx, int16_t my) {
    if (my < TOGGLE_Y || my >= TOGGLE_Y + TOGGLE_H) return -1;
    if (mx >= 4 && mx < 38) return 0;   /* SH */
    if (mx >= 40 && mx < 74) return 1;  /* RP */
    return -1;
}

static bool hit_vol_slider(int16_t mx, int16_t my) {
    return mx >= 108 && mx < 267 && my >= 95 && my < 110;
}

static int vol_from_mouse(int16_t mx) {
    int16_t track_start = 108;
    int16_t track_end = 267;
    int v = (mx - track_start) * 100 / (track_end - track_start);
    if (v < 0) v = 0;
    if (v > 100) v = 100;
    return v;
}

static int hit_playlist_row(frankamp_t *fa, int16_t mx, int16_t my) {
    if (my < PL_TOP_Y || my >= PL_TOP_Y + PL_ROWS * PL_ROW_H) return -1;
    /* Exclude scrollbar area */
    if (fa->pl_scrollbar.visible && mx >= CLIENT_W - SCROLLBAR_WIDTH) return -1;
    return (my - PL_TOP_Y) / PL_ROW_H;
}

static bool hit_add_btn(int16_t mx, int16_t my) {
    return mx >= 4 && mx < 54 && my >= 229 && my < 245;
}

static bool hit_rem_btn(int16_t mx, int16_t my) {
    return mx >= 58 && mx < 113 && my >= 229 && my < 245;
}

/*==========================================================================
 * Action helpers (shared by event handler, menu, and keyboard)
 *=========================================================================*/

/* Defer playback commands to the main loop (app task context) so that
 * pcm_init / pcm_cleanup use the correct FreeRTOS thread-local storage.
 * The event handler runs in the compositor task — wrong TLS for audio. */
#define PCMD_NONE 0
#define PCMD_PLAY 1
#define PCMD_STOP 2
#define PCMD_NEXT 3
#define PCMD_PREV 4

static void do_play(frankamp_t *fa) {
    if (fa->play_state == PS_STOPPED) {
        fa->pending_cmd = PCMD_PLAY;
    } else if (fa->play_state == PS_PAUSED) {
        play_pause(fa);  /* pause/unpause is safe — no pcm calls */
    }
}

static void do_open(frankamp_t *fa) {
    file_dialog_open(fa->hwnd, AL(AL_OPEN_AUDIO), "/", ".mp3;.mod;.mid;.midi");
}

/*==========================================================================
 * Event handler — single window
 *=========================================================================*/

static bool main_event(hwnd_t hwnd, const window_event_t *ev) {
    window_t *win = wm_get_window(hwnd);
    if (!win || !win->user_data) return false;
    frankamp_t *fa = (frankamp_t *)win->user_data;

    /* ---- Mouse down ---- */
    if (ev->type == WM_LBUTTONDOWN) {
        int16_t mx = ev->mouse.x, my = ev->mouse.y;

        /* Volume slider drag */
        if (hit_vol_slider(mx, my)) {
            fa->vol_drag = true;
            fa->volume = (uint8_t)vol_from_mouse(mx);
            wm_invalidate(hwnd);
            return true;
        }

        /* Transport buttons */
        int btn = hit_transport(mx, my);
        if (btn >= 0) {
            fa->btn_pressed = (int8_t)btn;
            wm_invalidate(hwnd);
            return true;
        }

        /* Open button */
        if (hit_open(mx, my)) {
            fa->btn_pressed = 5;
            wm_invalidate(hwnd);
            return true;
        }

        /* Toggle buttons */
        int tog = hit_toggle(mx, my);
        if (tog == 0) {
            fa->shuffle = !fa->shuffle;
            wm_invalidate(hwnd);
            return true;
        }
        if (tog == 1) {
            fa->repeat = !fa->repeat;
            wm_invalidate(hwnd);
            return true;
        }

        /* Playlist scrollbar */
        {
            int32_t new_pos;
            if (scrollbar_event(&fa->pl_scrollbar, ev, &new_pos)) {
                fa->pl_scroll = new_pos;
                wm_invalidate(hwnd);
                return true;
            }
        }

        /* Playlist row click */
        int plrow = hit_playlist_row(fa, mx, my);
        if (plrow >= 0) {
            int idx = fa->pl_scroll + plrow;
            if (idx >= 0 && idx < fa->pl_count) {
                if (fa->pl_selected == idx) {
                    /* Second click on same item: play it (deferred) */
                    fa->pending_cmd = PCMD_PLAY;
                } else {
                    fa->pl_selected = idx;
                }
                wm_invalidate(hwnd);
            }
            return true;
        }

        /* Bottom bar: +Add button */
        if (hit_add_btn(mx, my)) {
            do_open(fa);
            return true;
        }

        /* Bottom bar: -Rem button */
        if (hit_rem_btn(mx, my)) {
            playlist_remove(fa, fa->pl_selected);
            wm_invalidate(hwnd);
            return true;
        }

        return true;
    }

    /* ---- Mouse up ---- */
    if (ev->type == WM_LBUTTONUP) {
        int16_t mx = ev->mouse.x, my = ev->mouse.y;

        /* Forward to scrollbar (release thumb drag) */
        {
            int32_t new_pos;
            if (scrollbar_event(&fa->pl_scrollbar, ev, &new_pos)) {
                fa->pl_scroll = new_pos;
                wm_invalidate(hwnd);
                return true;
            }
        }

        if (fa->vol_drag) {
            fa->vol_drag = false;
            fa->volume = (uint8_t)vol_from_mouse(mx);
            wm_invalidate(hwnd);
            return true;
        }

        int btn = fa->btn_pressed;
        fa->btn_pressed = -1;

        if (btn == 5 && hit_open(mx, my)) {
            do_open(fa);
            wm_invalidate(hwnd);
            return true;
        }

        if (btn >= 0 && btn < NUM_TRANSPORT) {
            int actual = hit_transport(mx, my);
            if (actual == btn) {
                if (btn == 0) fa->pending_cmd = PCMD_PREV;
                else if (btn == 1) do_play(fa);
                else if (btn == 2) play_pause(fa);
                else if (btn == 3) fa->pending_cmd = PCMD_STOP;
                else if (btn == 4) fa->pending_cmd = PCMD_NEXT;
            }
        }
        wm_invalidate(hwnd);
        return true;
    }

    /* ---- Mouse move ---- */
    if (ev->type == WM_MOUSEMOVE) {
        /* Forward to scrollbar (thumb drag tracking) */
        {
            int32_t new_pos;
            if (scrollbar_event(&fa->pl_scrollbar, ev, &new_pos)) {
                fa->pl_scroll = new_pos;
                wm_invalidate(hwnd);
                return true;
            }
        }
        if (fa->vol_drag) {
            fa->volume = (uint8_t)vol_from_mouse(ev->mouse.x);
            wm_invalidate(hwnd);
        }
        return true;
    }

    /* ---- Menu + dialog commands ---- */
    if (ev->type == WM_COMMAND) {
        uint16_t cmd = ev->command.id;

        /* File dialog result — defer play_start to the app task */
        if (cmd == DLG_RESULT_FILE) {
            const char *path = file_dialog_get_path();
            if (path && path[0]) {
                int idx = playlist_add(fa, path);
                if (idx >= 0) {
                    fa->pl_selected = idx;
                    ensure_visible(fa);
                    if (fa->play_state == PS_STOPPED) {
                        strncpy(fa->pending_path, fa->playlist[idx].path,
                                MAX_PATH_LEN - 1);
                        fa->pending_path[MAX_PATH_LEN - 1] = '\0';
                        xTaskNotifyGive(app_task);
                    }
                }
            }
            wm_invalidate(hwnd);
            return true;
        }

        /* Menu commands */
        if (cmd == CMD_OPEN)    { do_open(fa); return true; }
        if (cmd == CMD_EXIT)    {
            window_event_t close_ev;
            memset(&close_ev, 0, sizeof(close_ev));
            close_ev.type = WM_CLOSE;
            wm_post_event(hwnd, &close_ev);
            return true;
        }
        if (cmd == CMD_PLAY)    { do_play(fa); wm_invalidate(hwnd); return true; }
        if (cmd == CMD_PAUSE)   { play_pause(fa); wm_invalidate(hwnd); return true; }
        if (cmd == CMD_STOP)    { fa->pending_cmd = PCMD_STOP; wm_invalidate(hwnd); return true; }
        if (cmd == CMD_NEXT)    { fa->pending_cmd = PCMD_NEXT; wm_invalidate(hwnd); return true; }
        if (cmd == CMD_PREV)    { fa->pending_cmd = PCMD_PREV; wm_invalidate(hwnd); return true; }
        if (cmd == CMD_SHUFFLE) { fa->shuffle = !fa->shuffle; wm_invalidate(hwnd); return true; }
        if (cmd == CMD_REPEAT)  { fa->repeat = !fa->repeat; wm_invalidate(hwnd); return true; }
        if (cmd == CMD_ABOUT) {
            dialog_show(hwnd, AL(AL_ABOUT),
                        "FrankAmp\n\nFRANK OS v" FRANK_VERSION_STR
                        "\n(c) 2026 Mikhail Matveev\n"
                        "<xtreme@rh1.tech>\n"
                        "github.com/rh1tech/frank-os",
                        DLG_ICON_INFO, DLG_BTN_OK);
            return true;
        }

        return false;
    }

    /* ---- Keyboard ---- */
    if (ev->type == WM_KEYDOWN) {
        uint8_t key = ev->key.scancode;

        /* Space: play/pause toggle */
        if (key == KEY_SPACE) {
            if (fa->play_state == PS_PLAYING || fa->play_state == PS_PAUSED)
                play_pause(fa);
            else if (fa->pl_count > 0)
                fa->pending_cmd = PCMD_PLAY;
            wm_invalidate(hwnd);
            return true;
        }

        /* Enter: play selected track */
        if (key == KEY_ENTER) {
            if (fa->pl_selected >= 0 && fa->pl_selected < fa->pl_count)
                fa->pending_cmd = PCMD_PLAY;
            wm_invalidate(hwnd);
            return true;
        }

        /* Z: previous track */
        if (key == KEY_Z) { fa->pending_cmd = PCMD_PREV; wm_invalidate(hwnd); return true; }

        /* X: play from stop */
        if (key == KEY_X) { do_play(fa); wm_invalidate(hwnd); return true; }

        /* C: pause/unpause */
        if (key == KEY_C) { play_pause(fa); wm_invalidate(hwnd); return true; }

        /* V: stop */
        if (key == KEY_V) { fa->pending_cmd = PCMD_STOP; wm_invalidate(hwnd); return true; }

        /* B: next track */
        if (key == KEY_B) { fa->pending_cmd = PCMD_NEXT; wm_invalidate(hwnd); return true; }

        /* O: open file */
        if (key == KEY_O) { do_open(fa); return true; }

        /* Up arrow: playlist select previous */
        if (key == KEY_UP) {
            if (fa->pl_selected > 0) {
                fa->pl_selected--;
                ensure_visible(fa);
                wm_invalidate(hwnd);
            }
            return true;
        }

        /* Down arrow: playlist select next */
        if (key == KEY_DOWN) {
            if (fa->pl_selected < fa->pl_count - 1) {
                fa->pl_selected++;
                ensure_visible(fa);
                wm_invalidate(hwnd);
            }
            return true;
        }

        /* +/=: volume up */
        if (key == KEY_PLUS) {
            if (fa->volume <= 95) fa->volume += 5;
            else fa->volume = 100;
            wm_invalidate(hwnd);
            return true;
        }

        /* -: volume down */
        if (key == KEY_MINUS) {
            if (fa->volume >= 5) fa->volume -= 5;
            else fa->volume = 0;
            wm_invalidate(hwnd);
            return true;
        }

        /* Delete: remove selected from playlist */
        if (key == KEY_DELETE) {
            if (fa->pl_selected >= 0 && fa->pl_selected < fa->pl_count) {
                playlist_remove(fa, fa->pl_selected);
                if (fa->pl_selected >= fa->pl_count && fa->pl_count > 0)
                    fa->pl_selected = fa->pl_count - 1;
                ensure_visible(fa);
                wm_invalidate(hwnd);
            }
            return true;
        }

        /* Escape: stop playback */
        if (key == KEY_ESC) {
            play_stop(fa);
            wm_invalidate(hwnd);
            return true;
        }

        /* F1: about */
        if (key == 0x3A) {
            window_event_t ce = {0}; ce.type = WM_COMMAND; ce.command.id = CMD_ABOUT;
            wm_post_event(hwnd, &ce); return true;
        }

        return false;
    }

    /* ---- Drop files (file association / multi-open) ---- */
    if (ev->type == WM_DROPFILES) {
        if (ev->dropfiles.file_path) {
            int idx = playlist_add(fa, ev->dropfiles.file_path);
            if (idx >= 0 && fa->play_state == PS_STOPPED) {
                strncpy(fa->pending_path, fa->playlist[idx].path,
                        MAX_PATH_LEN - 1);
                fa->pending_path[MAX_PATH_LEN - 1] = '\0';
                xTaskNotifyGive(app_task);
            }
            wm_invalidate(hwnd);
        }
        return true;
    }

    /* ---- Close ---- */
    if (ev->type == WM_CLOSE) {
        dbg_printf("[frankamp] WM_CLOSE\n");

        /* Stop audio DMA to unblock any blocked pcm_write() in the app task.
         * Do NOT free MIDI/MP3/MOD resources here — the app task may still
         * be inside audio_step(). Cleanup happens after the main loop exits. */
        if (fa->audio_active) {
            pcm_cleanup();
            fa->audio_active = false;
        }

        if (fa->ui_timer) {
            xTimerStop(fa->ui_timer, portMAX_DELAY);
            xTimerDelete(fa->ui_timer, portMAX_DELAY);
            fa->ui_timer = (TimerHandle_t)0;
        }

        win->user_data = (void *)0;
        app_closing = true;
        xTaskNotifyGive(app_task);
        return true;
    }

    return false;
}

/*==========================================================================
 * Menu setup
 *=========================================================================*/

static void setup_menu(hwnd_t hwnd) {
    menu_bar_t bar;
    memset(&bar, 0, sizeof(bar));
    bar.menu_count = 3;

    /* File menu */
    menu_def_t *file = &bar.menus[0];
    strncpy(file->title, L(STR_FILE), sizeof(file->title) - 1);
    file->accel_key = 0x09; /* HID 'F' */
    file->item_count = 3;

    strncpy(file->items[0].text, AL(AL_OPEN_MENU), sizeof(file->items[0].text) - 1);
    file->items[0].command_id = CMD_OPEN;
    file->items[0].accel_key = 0x12;

    file->items[1].flags = MIF_SEPARATOR;

    strncpy(file->items[2].text, L(STR_FM_EXIT), sizeof(file->items[2].text) - 1);
    file->items[2].command_id = CMD_EXIT;

    /* Music menu */
    menu_def_t *music = &bar.menus[1];
    strncpy(music->title, AL(AL_MUSIC), sizeof(music->title) - 1);
    music->accel_key = 0x10; /* HID 'M' */
    music->item_count = 8;

    strncpy(music->items[0].text, AL(AL_PLAY), sizeof(music->items[0].text) - 1);
    music->items[0].command_id = CMD_PLAY;

    strncpy(music->items[1].text, AL(AL_PAUSE), sizeof(music->items[1].text) - 1);
    music->items[1].command_id = CMD_PAUSE;

    strncpy(music->items[2].text, AL(AL_STOP), sizeof(music->items[2].text) - 1);
    music->items[2].command_id = CMD_STOP;

    music->items[3].flags = MIF_SEPARATOR;

    strncpy(music->items[4].text, AL(AL_NEXT), sizeof(music->items[4].text) - 1);
    music->items[4].command_id = CMD_NEXT;

    strncpy(music->items[5].text, AL(AL_PREV), sizeof(music->items[5].text) - 1);
    music->items[5].command_id = CMD_PREV;

    strncpy(music->items[6].text, AL(AL_SHUFFLE), sizeof(music->items[6].text) - 1);
    music->items[6].command_id = CMD_SHUFFLE;

    strncpy(music->items[7].text, AL(AL_REPEAT), sizeof(music->items[7].text) - 1);
    music->items[7].command_id = CMD_REPEAT;

    /* Help menu */
    menu_def_t *help = &bar.menus[2];
    strncpy(help->title, L(STR_HELP), sizeof(help->title) - 1);
    help->accel_key = 0x0B; /* HID 'H' */
    help->item_count = 1;
    strncpy(help->items[0].text, L(STR_FM_ABOUT_MENU), sizeof(help->items[0].text) - 1);
    help->items[0].command_id = CMD_ABOUT;
    help->items[0].accel_key = 0x3A;

    menu_set(hwnd, &bar);
}

/*==========================================================================
 * Window creation
 *=========================================================================*/

static hwnd_t create_main_window(frankamp_t *fa) {
    int16_t fw = CLIENT_W + 2 * THEME_BORDER_WIDTH;
    int16_t fh = CLIENT_H + THEME_TITLE_HEIGHT + THEME_MENU_HEIGHT +
                 2 * THEME_BORDER_WIDTH;
    int16_t x = (DISPLAY_WIDTH - fw) / 2;
    int16_t y = (DISPLAY_HEIGHT - TASKBAR_HEIGHT - fh) / 2;
    if (y < 0) y = 0;

    hwnd_t hwnd = wm_create_window(x, y, fw, fh, "FrankAmp",
                                    WSTYLE_DIALOG | WF_MENUBAR,
                                    main_event, main_paint);
    if (hwnd == HWND_NULL) return HWND_NULL;

    window_t *win = wm_get_window(hwnd);
    if (win) {
        win->user_data = fa;
        win->bg_color = COLOR_BLACK;
    }

    setup_menu(hwnd);

    return hwnd;
}

/*==========================================================================
 * App flags — declare background capability
 *=========================================================================*/

uint32_t __app_flags(void) { return APPFLAG_BACKGROUND; }

/*==========================================================================
 * Entry point
 *=========================================================================*/

int main(int argc, char **argv) {
    (void)argc;

    /* Single-instance guard — exit if already running */
    for (hwnd_t h = 1; h <= WM_MAX_WINDOWS; h++) {
        window_t *w = wm_get_window(h);
        if (w && (w->flags & WF_ALIVE) && 0 == strncmp(w->title, "FrankAmp", 8))
            return 0;
    }

    app_task = xTaskGetCurrentTaskHandle();
    app_closing = false;

    frankamp_t *fa = calloc(1, sizeof(frankamp_t));
    if (!fa) return 1;

    fa->btn_pressed = -1;
    fa->pl_current = -1;
    fa->pl_selected = 0;
    fa->volume = 68;     /* ~68% of max volume (0=quiet, 100=loud) */
    scrollbar_init(&fa->pl_scrollbar, false);  /* vertical playlist scrollbar */

    /* Playlist is large (~20 KB) but not time-critical — allocate
     * separately so it can spill to PSRAM, keeping SRAM free for the
     * Helix MP3 decoder (~15 KB via pvPortMalloc). */
    fa->playlist = calloc(MAX_PLAYLIST, sizeof(playlist_entry_t));
    if (!fa->playlist) {
        dbg_printf("[frankamp] playlist allocation failed\n");
        free(fa);
        return 1;
    }

    /* Create single window with menu bar */
    fa->hwnd = create_main_window(fa);
    if (fa->hwnd == HWND_NULL) {
        free(fa);
        return 1;
    }

    /* Create UI timer (100ms) */
    fa->ui_timer = xTimerCreate("fa_tmr", pdMS_TO_TICKS(100),
                                 pdTRUE, (void *)fa, ui_timer_cb);
    if (fa->ui_timer)
        xTimerStart(fa->ui_timer, 0);

    wm_show_window(fa->hwnd);
    wm_set_focus(fa->hwnd);
    taskbar_invalidate();

    /* If file paths were passed as arguments, add them all and play */
    if (argc > 1) {
        for (int i = 1; i < argc; i++) {
            if (argv[i] && argv[i][0])
                playlist_add(fa, argv[i]);
        }
        if (fa->pl_count > 0)
            play_start(fa, 0);
    }

    dbg_printf("[frankamp] entering main loop\n");

    while (!app_closing) {
        /* Check for deferred play request from the WM task */
        if (fa->pending_path[0] && fa->play_state == PS_STOPPED) {
            int idx = -1;
            for (int i = 0; i < fa->pl_count; i++) {
                if (strcmp(fa->playlist[i].path, fa->pending_path) == 0) {
                    idx = i;
                    break;
                }
            }
            fa->pending_path[0] = '\0';
            if (idx >= 0)
                play_start(fa, idx);
        }

        /* Process deferred transport commands (must run in app task
         * context so pcm_init/pcm_cleanup use the correct TLS). */
        if (fa->pending_cmd) {
            int8_t cmd = fa->pending_cmd;
            fa->pending_cmd = PCMD_NONE;
            if (cmd == PCMD_PLAY) {
                int idx = fa->pl_selected >= 0 ? fa->pl_selected : 0;
                if (fa->pl_count > 0) play_start(fa, idx);
            } else if (cmd == PCMD_STOP) play_stop(fa);
            else if (cmd == PCMD_NEXT) play_next(fa);
            else if (cmd == PCMD_PREV) play_prev(fa);
        }

        if (!app_closing && fa->play_state == PS_PLAYING) {
            if (!audio_step(fa))
                play_next(fa);
        } else {
            ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(50));
        }
    }

    dbg_printf("[frankamp] exited main loop\n");

    /* Safe to free playback resources now — no other task is using them */
    play_stop(fa);

    wm_destroy_window(fa->hwnd);
    taskbar_invalidate();

    /* fa is freed by pallocs cleanup */
    return 0;
}
