/*
 * MurmNES - NES Emulator for RP2350
 * Copyright (c) 2026 Mikhail Matveev <xtreme@rh1.tech>
 * https://rh1.tech | https://github.com/rh1tech/murmnes
 * SPDX-License-Identifier: MIT
 */

#ifndef BOARD_CONFIG_H
#define BOARD_CONFIG_H

/*
 * murmnes M2 Board Configuration
 *
 * HDMI: GPIO 12-19 (HSTX)
 * SD:   CLK=6, CMD=7, DAT0=4, DAT3=5
 * PS/2: CLK=2, DATA=3
 * NES Gamepad: CLK=20, LATCH=21, DATA=26
 */

#define BOARD_M2

/* PS/2 Keyboard */
#define KBD_CLOCK_PIN 2
#define KBD_DATA_PIN  3
#define PS2_PIN_CLK   2
#define PS2_PIN_DATA  3

/* SD Card (PIO-SPI) */
#define SDCARD_PIN_SPI0_SCK  6
#define SDCARD_PIN_SPI0_MOSI 7
#define SDCARD_PIN_SPI0_MISO 4
#define SDCARD_PIN_SPI0_CS   5

/* NES Gamepad */
#define NESPAD_CLK_PIN   20
#define NESPAD_LATCH_PIN 21
#define NESPAD_DATA_PIN  26

/* I2S Audio (optional external DAC) */
#define I2S_DATA_PIN       9
#define I2S_CLOCK_PIN_BASE 10

/* PSRAM (auto-detect CS pin at runtime) */
#define PSRAM_CS_PIN_RP2350B 47
#define PSRAM_CS_PIN_RP2350A 8

#endif /* BOARD_CONFIG_H */
