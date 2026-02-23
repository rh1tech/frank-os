/*
 * FRANK OS — I2S Audio Driver (PIO1 + DMA ping-pong)
 * Ported from murmgenesis by Mikhail Matveev
 *
 * Uses two DMA channels in ping-pong configuration:
 * - Channel A plays buffer 0, then triggers channel B
 * - Channel B plays buffer 1, then triggers channel A
 *
 * Each channel completion raises DMA_IRQ_0; the IRQ handler re-arms the
 * completed channel (reset read addr + transfer count) and marks its buffer
 * free for the CPU to refill.
 *
 * Hardware constraints (M2 board):
 *   PIO0  — reserved for PS/2 keyboard/mouse
 *   PIO1  — used here for I2S
 *   DMA 14/15 + DMA_IRQ_1 — reserved for DispHSTX (Core 1)
 *   DMA 10/11 + DMA_IRQ_0 — used here for audio (Core 0)
 *
 * Copyright (c) 2025 Mikhail Matveev <xtreme@rh1.tech>
 * https://rh1.tech
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "audio.h"
#include "board_config.h"
#include "audio_i2s.pio.h"

#include <stdio.h>
#include <string.h>

#include "pico/stdlib.h"
#include "hardware/gpio.h"
#include "hardware/dma.h"
#include "hardware/clocks.h"
#include "hardware/sync.h"
#include "hardware/irq.h"
#include "hardware/resets.h"

/*==========================================================================
 * Constants — keep audio away from DispHSTX (DMA 14/15, DMA_IRQ_1)
 *==========================================================================*/

#define AUDIO_DMA_IRQ     DMA_IRQ_0

#define AUDIO_DMA_CH_A    10
#define AUDIO_DMA_CH_B    11

#define DMA_BUFFER_COUNT  2

/* Maximum DMA transfer size in 32-bit words (stereo frames).
 * 1152 accommodates a full MPEG1 Layer 3 frame for direct-write mode. */
#define DMA_BUFFER_MAX_SAMPLES 1152

static uint32_t __attribute__((aligned(4)))
    dma_buffers[DMA_BUFFER_COUNT][DMA_BUFFER_MAX_SAMPLES];

/* Bitmask of buffers the CPU is allowed to write (1 = free) */
static volatile uint32_t dma_buffers_free_mask = 0;

/* Pre-roll: fill both buffers before starting playback */
#define PREROLL_BUFFERS 2
static volatile int preroll_count = 0;

static int  dma_channel_a = -1;
static int  dma_channel_b = -1;
static PIO  audio_pio;
static uint audio_sm;
static uint pio_program_offset;
static uint32_t dma_transfer_count;

static volatile bool audio_running = false;

static void audio_dma_irq_handler(void);

/*==========================================================================
 * i2s_init — PIO1 + DMA ping-pong setup
 *==========================================================================*/
void i2s_init(i2s_config_t *config) {
    audio_pio = config->pio;
    dma_transfer_count = config->dma_trans_count;

    /* Full hardware reset of PIO1 (PS/2 uses PIO0, HSTX is separate) */
    reset_block(RESETS_RESET_PIO1_BITS);
    unreset_block_wait(RESETS_RESET_PIO1_BITS);

    /* Clear audio DMA IRQ flags */
    dma_hw->ints0 = (1u << AUDIO_DMA_CH_A) | (1u << AUDIO_DMA_CH_B);

    /* Configure GPIO pins for PIO1 */
    gpio_set_function(config->data_pin, GPIO_FUNC_PIO1);
    gpio_set_function(config->clock_pin_base, GPIO_FUNC_PIO1);
    gpio_set_function(config->clock_pin_base + 1, GPIO_FUNC_PIO1);

    gpio_set_drive_strength(config->data_pin, GPIO_DRIVE_STRENGTH_12MA);
    gpio_set_drive_strength(config->clock_pin_base, GPIO_DRIVE_STRENGTH_12MA);
    gpio_set_drive_strength(config->clock_pin_base + 1, GPIO_DRIVE_STRENGTH_12MA);

    /* Claim a state machine on PIO1 */
    audio_sm = pio_claim_unused_sm(audio_pio, true);
    config->sm = audio_sm;

    /* Load PIO program */
    pio_program_offset = pio_add_program(audio_pio, &audio_i2s_program);
    audio_i2s_program_init(audio_pio, audio_sm, pio_program_offset,
                           config->data_pin, config->clock_pin_base);

    /* Drain the TX FIFO */
    pio_sm_clear_fifos(audio_pio, audio_sm);

    /* Set clock divider for the requested sample rate */
    uint32_t sys_clk = clock_get_hz(clk_sys);
    uint32_t divider = sys_clk * 4 / config->sample_freq;
    pio_sm_set_clkdiv_int_frac(audio_pio, audio_sm,
                                divider >> 8u, divider & 0xffu);

    /* Clamp transfer count to static buffer size */
    dma_transfer_count = config->dma_trans_count;
    if (dma_transfer_count == 0) dma_transfer_count = 1;
    if (dma_transfer_count > DMA_BUFFER_MAX_SAMPLES)
        dma_transfer_count = DMA_BUFFER_MAX_SAMPLES;
    config->dma_trans_count = (uint16_t)dma_transfer_count;

    /* Silence the DMA buffers */
    memset(dma_buffers, 0, sizeof(dma_buffers));
    config->dma_buf = (uint16_t *)(void *)dma_buffers[0];

    /* Claim fixed DMA channels (abort first in case of leftover state) */
    dma_channel_abort(AUDIO_DMA_CH_A);
    dma_channel_abort(AUDIO_DMA_CH_B);
    while (dma_channel_is_busy(AUDIO_DMA_CH_A) ||
           dma_channel_is_busy(AUDIO_DMA_CH_B)) {
        tight_loop_contents();
    }
    dma_channel_unclaim(AUDIO_DMA_CH_A);
    dma_channel_unclaim(AUDIO_DMA_CH_B);
    dma_channel_claim(AUDIO_DMA_CH_A);
    dma_channel_claim(AUDIO_DMA_CH_B);
    dma_channel_a = AUDIO_DMA_CH_A;
    dma_channel_b = AUDIO_DMA_CH_B;
    config->dma_channel = (uint8_t)dma_channel_a;

    /* Configure DMA channels in ping-pong chain */
    dma_channel_config cfg_a = dma_channel_get_default_config(dma_channel_a);
    channel_config_set_read_increment(&cfg_a, true);
    channel_config_set_write_increment(&cfg_a, false);
    channel_config_set_transfer_data_size(&cfg_a, DMA_SIZE_32);
    channel_config_set_dreq(&cfg_a, pio_get_dreq(audio_pio, audio_sm, true));
    channel_config_set_chain_to(&cfg_a, dma_channel_b);

    dma_channel_config cfg_b = dma_channel_get_default_config(dma_channel_b);
    channel_config_set_read_increment(&cfg_b, true);
    channel_config_set_write_increment(&cfg_b, false);
    channel_config_set_transfer_data_size(&cfg_b, DMA_SIZE_32);
    channel_config_set_dreq(&cfg_b, pio_get_dreq(audio_pio, audio_sm, true));
    channel_config_set_chain_to(&cfg_b, dma_channel_a);

    dma_channel_configure(dma_channel_a, &cfg_a,
                          &audio_pio->txf[audio_sm],
                          dma_buffers[0], dma_transfer_count, false);

    dma_channel_configure(dma_channel_b, &cfg_b,
                          &audio_pio->txf[audio_sm],
                          dma_buffers[1], dma_transfer_count, false);

    /* Set up DMA_IRQ_0 handler (DispHSTX uses DMA_IRQ_1 on Core 1) */
    irq_set_exclusive_handler(AUDIO_DMA_IRQ, audio_dma_irq_handler);
    irq_set_priority(AUDIO_DMA_IRQ, 0x80);
    irq_set_enabled(AUDIO_DMA_IRQ, true);

    dma_hw->ints0 = (1u << dma_channel_a) | (1u << dma_channel_b);
    dma_channel_set_irq0_enabled(dma_channel_a, true);
    dma_channel_set_irq0_enabled(dma_channel_b, true);

    /* Enable the PIO state machine */
    pio_sm_set_enabled(audio_pio, audio_sm, true);

    /* Initialize playback state */
    preroll_count = 0;
    dma_buffers_free_mask = (1u << DMA_BUFFER_COUNT) - 1u;  /* both free */
    audio_running = false;
}

/*==========================================================================
 * i2s_deinit — clean teardown of PIO + DMA resources
 *==========================================================================*/
void i2s_deinit(i2s_config_t *config) {
    (void)config;

    /* 1. Stop producing new audio */
    audio_running = false;

    /* 2. Stop PIO state machine */
    pio_sm_set_enabled(audio_pio, audio_sm, false);

    /* 3. Disable DMA IRQ */
    irq_set_enabled(AUDIO_DMA_IRQ, false);

    /* 4. Abort and unclaim both DMA channels, clear IRQ flags */
    if (dma_channel_a >= 0) {
        dma_channel_set_irq0_enabled(dma_channel_a, false);
        dma_channel_abort(dma_channel_a);
        dma_hw->ints0 = (1u << dma_channel_a);
        dma_channel_unclaim(dma_channel_a);
        dma_channel_a = -1;
    }

    if (dma_channel_b >= 0) {
        dma_channel_set_irq0_enabled(dma_channel_b, false);
        dma_channel_abort(dma_channel_b);
        dma_hw->ints0 = (1u << dma_channel_b);
        dma_channel_unclaim(dma_channel_b);
        dma_channel_b = -1;
    }

    /* 5. Release PIO resources so subsequent i2s_init() can reclaim them */
    pio_sm_unclaim(audio_pio, audio_sm);
    pio_remove_program(audio_pio, &audio_i2s_program, pio_program_offset);

    /* 6. Reset state variables */
    dma_buffers_free_mask = (1u << DMA_BUFFER_COUNT) - 1u;
    preroll_count = 0;
}

/*==========================================================================
 * i2s_dma_write — fill a free buffer and kick off DMA when pre-roll is done
 *==========================================================================*/
void i2s_dma_write(i2s_config_t *config, const int16_t *samples) {
    uint32_t sample_count = dma_transfer_count;

    /* Wait for a free buffer, then claim it (atomically vs DMA IRQ) */
    uint8_t buf_index = 0;
    while (true) {
        uint32_t irq_state = save_and_disable_interrupts();
        uint32_t free_mask = dma_buffers_free_mask;

        if (!audio_running) {
            /* Pre-roll fills buffer 0 then buffer 1 */
            buf_index = (uint8_t)preroll_count;
            if (buf_index < DMA_BUFFER_COUNT &&
                (free_mask & (1u << buf_index))) {
                dma_buffers_free_mask &= ~(1u << buf_index);
                restore_interrupts(irq_state);
                break;
            }
        } else {
            if (free_mask) {
                buf_index = (free_mask & 1u) ? 0 : 1;
                dma_buffers_free_mask &= ~(1u << buf_index);
                restore_interrupts(irq_state);
                break;
            }
        }

        restore_interrupts(irq_state);
        tight_loop_contents();
    }

    uint32_t *write_ptr = dma_buffers[buf_index];
    int16_t  *write_ptr16 = (int16_t *)(void *)write_ptr;

    if (config->volume == 0) {
        memcpy(write_ptr, samples, sample_count * sizeof(uint32_t));
    } else {
        /* Volume adjustment: right-shift each 16-bit sample */
        for (uint32_t i = 0; i < sample_count * 2; i++) {
            write_ptr16[i] = samples[i] >> config->volume;
        }
    }

    /* Pad remainder with silence */
    if (sample_count < dma_transfer_count) {
        memset(&write_ptr[sample_count], 0,
               (dma_transfer_count - sample_count) * sizeof(uint32_t));
    }

    /* Memory barrier — ensure writes are visible before DMA reads */
    __dmb();

    if (!audio_running) {
        preroll_count++;
        if (preroll_count >= PREROLL_BUFFERS) {
            dma_channel_start(dma_channel_a);
            audio_running = true;
        }
    }
}

/*==========================================================================
 * i2s_dma_write_nb — non-blocking variant (safe to call from ISR context)
 *
 * Returns true if data was written, false if no DMA buffer is free.
 *==========================================================================*/
bool i2s_dma_write_nb(i2s_config_t *config, const int16_t *samples) {
    uint32_t sample_count = dma_transfer_count;
    uint8_t buf_index = 0;

    uint32_t irq_state = save_and_disable_interrupts();
    uint32_t free_mask = dma_buffers_free_mask;

    if (!audio_running) {
        buf_index = (uint8_t)preroll_count;
        if (buf_index >= DMA_BUFFER_COUNT ||
            !(free_mask & (1u << buf_index))) {
            restore_interrupts(irq_state);
            return false;
        }
        dma_buffers_free_mask &= ~(1u << buf_index);
    } else {
        if (!free_mask) {
            restore_interrupts(irq_state);
            return false;
        }
        buf_index = (free_mask & 1u) ? 0 : 1;
        dma_buffers_free_mask &= ~(1u << buf_index);
    }
    restore_interrupts(irq_state);

    uint32_t *write_ptr = dma_buffers[buf_index];
    int16_t  *write_ptr16 = (int16_t *)(void *)write_ptr;

    if (config->volume == 0) {
        memcpy(write_ptr, samples, sample_count * sizeof(uint32_t));
    } else {
        for (uint32_t i = 0; i < sample_count * 2; i++)
            write_ptr16[i] = samples[i] >> config->volume;
    }

    if (sample_count < dma_transfer_count) {
        memset(&write_ptr[sample_count], 0,
               (dma_transfer_count - sample_count) * sizeof(uint32_t));
    }

    __dmb();

    if (!audio_running) {
        preroll_count++;
        if (preroll_count >= PREROLL_BUFFERS) {
            dma_channel_start(dma_channel_a);
            audio_running = true;
        }
    }

    return true;
}

/*==========================================================================
 * i2s_is_buffer_free — check if a DMA buffer can accept data (ISR-safe)
 *==========================================================================*/
bool i2s_is_buffer_free(void) {
    if (!audio_running)
        return preroll_count < DMA_BUFFER_COUNT;
    return dma_buffers_free_mask != 0;
}

/*==========================================================================
 * Volume helpers
 *==========================================================================*/
void i2s_volume(i2s_config_t *config, uint8_t volume) {
    if (volume > 16) volume = 16;
    config->volume = volume;
}

void i2s_increase_volume(i2s_config_t *config) {
    if (config->volume > 0) config->volume--;
}

void i2s_decrease_volume(i2s_config_t *config) {
    if (config->volume < 16) config->volume++;
}

/*==========================================================================
 * DMA IRQ handler — re-arms the completed channel for ping-pong
 *==========================================================================*/
static void audio_dma_irq_handler(void) {
    uint32_t ints = dma_hw->ints0;
    uint32_t mask = 0;
    if (dma_channel_a >= 0) mask |= (1u << dma_channel_a);
    if (dma_channel_b >= 0) mask |= (1u << dma_channel_b);
    ints &= mask;
    if (!ints) return;

    if ((dma_channel_a >= 0) && (ints & (1u << dma_channel_a))) {
        dma_hw->ints0 = (1u << dma_channel_a);
        dma_channel_set_read_addr(dma_channel_a, dma_buffers[0], false);
        dma_channel_set_trans_count(dma_channel_a, dma_transfer_count, false);
        dma_buffers_free_mask |= 1u;
    }

    if ((dma_channel_b >= 0) && (ints & (1u << dma_channel_b))) {
        dma_hw->ints0 = (1u << dma_channel_b);
        dma_channel_set_read_addr(dma_channel_b, dma_buffers[1], false);
        dma_channel_set_trans_count(dma_channel_b, dma_transfer_count, false);
        dma_buffers_free_mask |= 2u;
    }
}
