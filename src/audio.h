/*
 * FRANK OS — I2S Audio Driver (PIO1 + DMA ping-pong)
 * Copyright (c) 2025 Mikhail Matveev <xtreme@rh1.tech>
 * https://rh1.tech
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <stdint.h>
#include "hardware/pio.h"

typedef struct {
    uint32_t sample_freq;
    uint8_t  channel_count;
    uint     data_pin;
    uint     clock_pin_base;
    PIO      pio;
    uint     sm;
    uint8_t  dma_channel;
    uint16_t *dma_buf;
    uint16_t dma_trans_count;
    uint8_t  volume;
} i2s_config_t;

void i2s_init(i2s_config_t *config);
void i2s_deinit(i2s_config_t *config);
void i2s_dma_write(i2s_config_t *config, const int16_t *samples);
bool i2s_dma_write_nb(i2s_config_t *config, const int16_t *samples);
bool i2s_is_buffer_free(void);
void i2s_volume(i2s_config_t *config, uint8_t volume);
void i2s_increase_volume(i2s_config_t *config);
void i2s_decrease_volume(i2s_config_t *config);
