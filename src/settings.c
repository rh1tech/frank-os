/*
 * FRANK OS — Persistent Settings
 * Copyright (c) 2026 Mikhail Matveev <xtreme@rh1.tech>
 * https://rh1.tech
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "settings.h"
#include "window_theme.h"
#include "board_config.h"
#include "sdcard_init.h"
#include "ff.h"
#include <string.h>

static settings_t g_settings;

static void settings_defaults(void) {
    memset(&g_settings, 0, sizeof(g_settings));
    g_settings.magic         = SETTINGS_MAGIC;
    g_settings.version       = 1;
    g_settings.volume        = 0;       /* max volume */
    g_settings.desktop_color = 3;       /* COLOR_CYAN */
    g_settings.dblclick_ms   = 400;
    g_settings.cpu_freq_mhz  = CPU_CLOCK_MHZ;
    g_settings.psram_freq_mhz = 0;     /* compile default */
    g_settings.rtc_scl_pin   = RTC_PIN_SCL;
    g_settings.rtc_sda_pin   = RTC_PIN_SDA;
}

void settings_load(void) {
    settings_defaults();

    if (!sdcard_is_mounted()) return;

    FIL f;
    if (f_open(&f, SETTINGS_PATH, FA_READ) != FR_OK) return;

    settings_t tmp;
    UINT br;
    if (f_read(&f, &tmp, sizeof(tmp), &br) == FR_OK &&
        br == sizeof(tmp) &&
        tmp.magic == SETTINGS_MAGIC &&
        tmp.version == 1) {
        g_settings = tmp;
        /* Clamp values */
        if (g_settings.volume > 4) g_settings.volume = 4;
        if (g_settings.desktop_color > 15) g_settings.desktop_color = 3;
        if (g_settings.dblclick_ms < 200 || g_settings.dblclick_ms > 800)
            g_settings.dblclick_ms = 400;
        if (g_settings.theme_id >= THEME_COUNT)
            g_settings.theme_id = THEME_ID_WIN95;
        /* Older settings files stored 0 here — fall back to board defaults. */
        if (g_settings.rtc_scl_pin == 0) g_settings.rtc_scl_pin = RTC_PIN_SCL;
        if (g_settings.rtc_sda_pin == 0) g_settings.rtc_sda_pin = RTC_PIN_SDA;
        /* Apply saved theme */
        theme_set(g_settings.theme_id);
    }
    f_close(&f);
}

void settings_save(void) {
    if (!sdcard_is_mounted()) return;

    FIL f;
    if (f_open(&f, SETTINGS_PATH, FA_WRITE | FA_CREATE_ALWAYS) != FR_OK)
        return;

    UINT bw;
    f_write(&f, &g_settings, sizeof(g_settings), &bw);
    f_close(&f);
}

settings_t *settings_get(void) {
    return &g_settings;
}
