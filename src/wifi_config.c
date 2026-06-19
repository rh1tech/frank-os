/*
 * FRANK OS — WiFi Configuration Persistence
 * Copyright (c) 2026 Mikhail Matveev <xtreme@rh1.tech>
 * https://rh1.tech
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "wifi_config.h"
#include "board_config.h"
#include "ff.h"
#include <string.h>
#include <stdio.h>

/* Heap-allocated to reduce BSS (~108 bytes saved) */
static wifi_config_t *wifi_cfg_ptr;
#define wifi_cfg (*wifi_cfg_ptr)

extern void *pvPortMalloc(unsigned int);

static void wifi_config_defaults(void) {
    memset(&wifi_cfg, 0, sizeof(wifi_cfg));
    wifi_cfg.magic = WIFI_CONFIG_MAGIC;
    wifi_cfg.version = 1;
    wifi_cfg.auto_connect = 1;
    wifi_cfg.rx_pin = NETCARD_PIN_RX;
    wifi_cfg.tx_pin = NETCARD_PIN_TX;
}

void wifi_config_load(void) {
    if (!wifi_cfg_ptr)
        wifi_cfg_ptr = (wifi_config_t *)pvPortMalloc(sizeof(wifi_config_t));
    if (!wifi_cfg_ptr) return;
    wifi_config_defaults();

    FIL f;
    if (f_open(&f, WIFI_CONFIG_PATH, FA_READ) != FR_OK)
        return;

    UINT br;
    wifi_config_t tmp;
    if (f_read(&f, &tmp, sizeof(tmp), &br) == FR_OK &&
        br == sizeof(tmp) &&
        tmp.magic == WIFI_CONFIG_MAGIC &&
        tmp.version == 1) {
        /* Ensure null termination */
        tmp.ssid[32] = '\0';
        tmp.password[64] = '\0';
        wifi_cfg = tmp;
        /* Older configs stored 0 here (was reserved padding) — fall back
         * to the board default pins so the netcard still probes. */
        if (wifi_cfg.rx_pin == 0) wifi_cfg.rx_pin = NETCARD_PIN_RX;
        if (wifi_cfg.tx_pin == 0) wifi_cfg.tx_pin = NETCARD_PIN_TX;
    }

    f_close(&f);
    printf("WiFi config: %s\n", wifi_cfg.ssid[0] ? wifi_cfg.ssid : "(none)");
}

void wifi_config_save(void) {
    wifi_cfg.magic = WIFI_CONFIG_MAGIC;
    wifi_cfg.version = 1;

    FIL f;
    if (f_open(&f, WIFI_CONFIG_PATH, FA_WRITE | FA_CREATE_ALWAYS) != FR_OK)
        return;

    UINT bw;
    f_write(&f, &wifi_cfg, sizeof(wifi_cfg), &bw);
    f_close(&f);
}

wifi_config_t *wifi_config_get(void) {
    return wifi_cfg_ptr;
}

void wifi_config_clear(void) {
    wifi_config_defaults();
    f_unlink(WIFI_CONFIG_PATH);
}
