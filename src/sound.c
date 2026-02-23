/*
 * Originally from Murmulator OS 2 by DnCraptor
 * https://github.com/DnCraptor/murmulator-os2
 *
 * Modified for FRANK OS — I2S audio only (no PWM).
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <pico.h>
#include <pico/multicore.h>
#include <pico/stdlib.h>
#include <pico/time.h>
#include "FreeRTOS.h"
#include "task.h"
#include "sound.h"
#include "audio.h"
#include "board_config.h"

#define SOUND_FREQUENCY 44100

static i2s_config_t i2s_config = {
    .sample_freq = SOUND_FREQUENCY,
    .channel_count = 2,
    .data_pin = I2S_DATA_PIN,
    .clock_pin_base = I2S_CLOCK_PIN_BASE,
    .pio = pio1,
    .sm = 0,
    .dma_channel = 0,
    .dma_buf = NULL,
    .dma_trans_count = 0,
    .volume = 0,
};

inline static void inInit(uint gpio) {
    gpio_init(gpio);
    gpio_set_dir(gpio, GPIO_IN);
    gpio_pull_up(gpio);
}

void init_sound() {
    i2s_config.sample_freq = SOUND_FREQUENCY;
    i2s_config.dma_trans_count = SOUND_FREQUENCY / 100;
    i2s_volume(&i2s_config, 0);
#ifdef WAV_IN_PIO
    //пин ввода звука
    inInit(WAV_IN_PIO);
#endif
}

void blimp(uint32_t count, uint32_t tiks_to_delay) {
    // TODO: implement via I2S tone generation
}

static repeating_timer_t m_timer = { 0 };
static volatile pcm_end_callback_t m_cb = NULL;

static bool __not_in_flash_func(timer_callback)(repeating_timer_t *rt) {
    /* Only call the callback when a DMA buffer is free.  The callback
     * advances playback state (offset, buffer swap) — calling it without
     * actually writing the data would silently drop audio chunks. */
    if (!i2s_is_buffer_free()) return true;

    size_t size = 0;
    char* buff = m_cb(&size);
    if (!buff || size == 0) return true;
    i2s_config.dma_buf = buff;
    i2s_config.dma_trans_count = size >> 1;
    i2s_dma_write_nb(&i2s_config, (const int16_t *)buff);
    return true;
}

void pcm_call() {
    return;
}

void pcm_cleanup(void) {
    cancel_repeating_timer(&m_timer);
    m_timer.delay_us = 0;
    i2s_volume(&i2s_config, 0);
    i2s_deinit(&i2s_config);
}

void pcm_setup(int hz) {
    if (m_timer.delay_us) {
        pcm_cleanup();
    }
    i2s_config.sample_freq = hz;
}

// size - in 16-bit values count
void pcm_set_buffer(int16_t* buff, uint8_t channels, size_t size, pcm_end_callback_t cb) {
    m_cb = cb;
    i2s_config.channel_count = channels;
    i2s_config.dma_trans_count = i2s_config.sample_freq / (size << 1); // Number of 32 bits words to transfer
    i2s_config.dma_buf = buff;
    i2s_volume(&i2s_config, 0);
    i2s_init(&i2s_config);
    i2s_dma_write(&i2s_config, buff);
    add_repeating_timer_us(1000000 * size * channels / i2s_config.sample_freq, timer_callback, NULL, &m_timer);
}

/* Direct I2S mode — task-context blocking writes, no timer/ISR.
 * Call pcm_init once, then pcm_write repeatedly from the main loop. */
void pcm_init(int sample_rate, int channels) {
    if (m_timer.delay_us) {
        cancel_repeating_timer(&m_timer);
        m_timer.delay_us = 0;
    }
    i2s_config.sample_freq = sample_rate;
    i2s_config.channel_count = channels;
    i2s_config.dma_trans_count = 1152; /* one full MP3 frame */
    i2s_volume(&i2s_config, 0);
    i2s_init(&i2s_config);
}

void pcm_write(const int16_t *samples, int count) {
    (void)count;
    i2s_dma_write(&i2s_config, samples);
}
