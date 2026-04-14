/*
 * FRANK OS — Persistent Settings
 * Copyright (c) 2026 Mikhail Matveev <xtreme@rh1.tech>
 * https://rh1.tech
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef SETTINGS_H
#define SETTINGS_H

#include <stdint.h>

#define SETTINGS_MAGIC   0x46534554  /* "FSET" */
#define SETTINGS_PATH    "/fos/settings.dat"

typedef struct {
    uint32_t magic;
    uint8_t  version;          /* 1 */
    uint8_t  volume;           /* 0-4 (right-shift, 0=max, 4=mute) */
    uint8_t  desktop_color;    /* palette index 0-15 (default: COLOR_CYAN=3) */
    uint8_t  _pad0;
    uint16_t dblclick_ms;      /* 200-800 (default: 400) */
    uint16_t cpu_freq_mhz;     /* 252, 378, 504 */
    uint16_t psram_freq_mhz;   /* 0=compile default, 133, 166 */
    uint8_t  theme_id;         /* 0=Win95, 1=Simple */
    uint8_t  language;         /* 0=English, 1=Russian */
    uint8_t  input_toggle;    /* 0=Alt+Shift, 1=Ctrl+Shift, 2=Alt+Space */
    uint8_t  reserved[15];     /* pad to 32 bytes */
} settings_t;

/* Read from /fos/settings.dat (safe if no SD card) */
void settings_load(void);

/* Write to /fos/settings.dat */
void settings_save(void);

/* Live pointer to in-memory settings */
settings_t *settings_get(void);

#endif /* SETTINGS_H */
