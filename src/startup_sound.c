/*
 * FRANK OS — Startup Sound
 * Copyright (c) 2025 Mikhail Matveev <xtreme@rh1.tech>
 * https://rh1.tech
 *
 * Plays the embedded WAV file (8-bit unsigned PCM, mono, 22050 Hz) through
 * the sound mixer.  Runs as a one-shot FreeRTOS task that streams audio
 * via snd_write(), then cleans up and deletes itself.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "startup_sound.h"
#include "snd.h"

#include <stdint.h>
#include <string.h>
#include "FreeRTOS.h"
#include "task.h"

/* Embedded WAV (placed in .rodata / flash by startup_wav.S) */
extern const uint8_t  startup_wav_data[];
extern const uint32_t startup_wav_size;

/* Number of stereo frames per write chunk (~20 ms at 22050 Hz) */
#define CHUNK_FRAMES 441

/*--------------------------------------------------------------------------
 * Find the "data" sub-chunk inside a RIFF/WAVE file.
 *--------------------------------------------------------------------------*/
static const uint8_t *wav_find_data(const uint8_t *wav, uint32_t wav_len,
                                    uint32_t *out_len) {
    if (wav_len < 44) return NULL;
    if (memcmp(wav, "RIFF", 4) != 0) return NULL;
    if (memcmp(wav + 8, "WAVE", 4) != 0) return NULL;

    uint32_t pos = 12;
    while (pos + 8 <= wav_len) {
        uint32_t chunk_size = (uint32_t)wav[pos + 4]
                            | ((uint32_t)wav[pos + 5] << 8)
                            | ((uint32_t)wav[pos + 6] << 16)
                            | ((uint32_t)wav[pos + 7] << 24);
        if (memcmp(wav + pos, "data", 4) == 0) {
            uint32_t data_start = pos + 8;
            uint32_t data_len = chunk_size;
            if (data_start + data_len > wav_len)
                data_len = wav_len - data_start;
            *out_len = data_len;
            return wav + data_start;
        }
        pos += 8 + chunk_size;
        if (pos & 1) pos++;
    }
    return NULL;
}

/*--------------------------------------------------------------------------
 * Startup sound task — plays through the sound mixer
 *--------------------------------------------------------------------------*/
static void startup_sound_task(void *params) {
    (void)params;

    /* Locate PCM data in the embedded WAV */
    uint32_t pcm_len = 0;
    const uint8_t *pcm = wav_find_data(startup_wav_data, startup_wav_size,
                                       &pcm_len);
    if (!pcm || pcm_len == 0) {
        vTaskDelete(NULL);
        return;
    }

    /* Open a mixer channel at the WAV's native sample rate */
    int ch = snd_open(22050);
    if (ch < 0) {
        vTaskDelete(NULL);
        return;
    }

    /* Stream: convert 8-bit unsigned mono -> 16-bit signed stereo in chunks,
     * then push each chunk to the ring buffer via snd_write(). */
    int16_t chunk[CHUNK_FRAMES * 2];  /* L+R interleaved */
    uint32_t pos = 0;

    while (pos < pcm_len) {
        uint32_t frames = CHUNK_FRAMES;
        if (pos + frames > pcm_len)
            frames = pcm_len - pos;

        /* 8-bit unsigned (0..255) -> 16-bit signed, mono->stereo */
        for (uint32_t i = 0; i < frames; i++) {
            int16_t s = ((int16_t)pcm[pos + i] - 128) * 26;
            chunk[i * 2]     = s;
            chunk[i * 2 + 1] = s;
        }
        /* Pad remainder of last chunk with silence */
        for (uint32_t i = frames; i < CHUNK_FRAMES; i++) {
            chunk[i * 2]     = 0;
            chunk[i * 2 + 1] = 0;
        }

        snd_write(ch, chunk, (int)frames);
        pos += frames;
    }

    /* Let the ring buffer drain before closing.  At 22050 Hz resampled to
     * 44100, up to 2048 source frames = ~93 ms worst case. */
    vTaskDelay(pdMS_TO_TICKS(120));

    snd_close(ch);
    vTaskDelete(NULL);
}

void startup_sound_start(void) {
    /* Priority 3: above compositor (2) so boot-time SD I/O and window
     * init don't starve the audio ring buffer, causing dropouts. */
    xTaskCreate(startup_sound_task, "startsnd", 1024, NULL, 3, NULL);
}
