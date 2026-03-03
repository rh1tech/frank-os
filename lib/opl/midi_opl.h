/*
 * MIDI OPL FM synthesizer — render-based API
 * Adapted from Chocolate Doom / murmdoom i_oplmusic.c + opl_pico.c
 *
 * Copyright(C) 1993-1996 Id Software, Inc.
 * Copyright(C) 2005-2014 Simon Howard
 * Copyright(C) 2021-2022 Graham Sanderson
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef MIDI_OPL_H
#define MIDI_OPL_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct midi_opl midi_opl_t;

/* Allocate and init OPL synthesizer (44100 Hz stereo) */
midi_opl_t *midi_opl_init(void);

/* Load a .mid file. Returns true on success. */
bool midi_opl_load(midi_opl_t *ctx, const char *filepath);

/* Render PCM audio into buf (interleaved stereo int16_t).
 * max_frames = number of stereo frames (buf must hold max_frames*2 int16_t).
 * Returns number of frames rendered (0 when song is finished and not looping). */
int midi_opl_render(midi_opl_t *ctx, int16_t *buf, int max_frames);

/* Returns true if still playing. */
bool midi_opl_playing(midi_opl_t *ctx);

/* Enable/disable looping. */
void midi_opl_set_loop(midi_opl_t *ctx, bool loop);

/* Free all resources. */
void midi_opl_free(midi_opl_t *ctx);

#ifdef __cplusplus
}
#endif

#endif /* MIDI_OPL_H */
