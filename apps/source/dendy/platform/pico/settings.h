/*
 * MurmNES - NES Emulator for RP2350
 * Copyright (c) 2026 Mikhail Matveev <xtreme@rh1.tech>
 * https://rh1.tech | https://github.com/rh1tech/murmnes
 * SPDX-License-Identifier: MIT
 */

#ifndef SETTINGS_H
#define SETTINGS_H

#include <stdint.h>
#include <stdbool.h>

// Input mode values
#define INPUT_MODE_ANY      0
#define INPUT_MODE_NES1     1
#define INPUT_MODE_NES2     2
#define INPUT_MODE_USB1     3
#define INPUT_MODE_USB2     4
#define INPUT_MODE_KEYBOARD 5
#define INPUT_MODE_DISABLED 6
#define INPUT_MODE_COUNT    7

// Audio output mode
#define AUDIO_MODE_HDMI     0
#define AUDIO_MODE_I2S      1
#define AUDIO_MODE_DISABLED 2
#define AUDIO_MODE_COUNT    3

// Volume range (0-100, step 10)
#define VOLUME_MIN  0
#define VOLUME_MAX  100
#define VOLUME_STEP 10

typedef struct {
    uint8_t p1_mode;      // Player 1 input mode (INPUT_MODE_*)
    uint8_t p2_mode;      // Player 2 input mode (INPUT_MODE_*)
    uint8_t audio_mode;   // Audio output (AUDIO_MODE_*)
    uint8_t volume;       // Master volume 0-100
} settings_t;

extern settings_t g_settings;

typedef enum {
    SETTINGS_RESULT_EXIT,
    SETTINGS_RESULT_RESET,
} settings_result_t;

/**
 * Load settings from SD card (/nes/.settings)
 * If file doesn't exist, uses defaults.
 */
void settings_load(void);

/**
 * Save current settings to SD card (/nes/.settings)
 */
void settings_save(void);

/* Current ROM name (no path/extension) — set by main_pico.c during ROM load */
extern char g_rom_name[64];

/**
 * Check if menu hotkey is pressed (Start+Select, ESC, or F12)
 * Call this during the emulation loop.
 */
bool settings_check_hotkey(void);

/**
 * Display settings menu and block until user exits.
 * @param screen_buffer 256*240 byte buffer for menu rendering
 * @return SETTINGS_RESULT_CANCEL when user exits
 */
settings_result_t settings_menu_show(uint8_t *screen_buffer);

#endif /* SETTINGS_H */
