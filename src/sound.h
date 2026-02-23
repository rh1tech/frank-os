/*
 * Originally from Murmulator OS 2 by DnCraptor
 * https://github.com/DnCraptor/murmulator-os2
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

void init_sound();

void blimp(uint32_t count, uint32_t tiks_to_delay);
void pcm_setup(int hz);
void pcm_cleanup(void);

typedef char* (*pcm_end_callback_t)(size_t* size);
void pcm_set_buffer(int16_t* buff, uint8_t channels, size_t size, pcm_end_callback_t cb);

/* Direct I2S access — call from task context (blocking).
 * Avoids the ISR-driven timer_callback path entirely. */
void pcm_init(int sample_rate, int channels);
void pcm_write(const int16_t *samples, int count);

// internal call on core#1
void pcm_call();