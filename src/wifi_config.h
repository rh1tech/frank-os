/*
 * FRANK OS — WiFi Configuration Persistence
 * Copyright (c) 2026 Mikhail Matveev <xtreme@rh1.tech>
 * https://rh1.tech
 *
 * Stores WiFi SSID and password in /fos/wifi.dat for auto-reconnect on boot.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef WIFI_CONFIG_H
#define WIFI_CONFIG_H

#include <stdint.h>

#define WIFI_CONFIG_PATH    "/fos/wifi.dat"
#define WIFI_CONFIG_MAGIC   0x57494649  /* "WIFI" */

typedef struct {
    uint32_t magic;
    uint8_t  version;           /* 1 */
    uint8_t  auto_connect;      /* 1 = auto-connect on boot */
    uint8_t  rx_pin;            /* netcard PIO UART RX GPIO (0 = board default) */
    uint8_t  tx_pin;            /* netcard PIO UART TX GPIO (0 = board default) */
    char     ssid[33];          /* null-terminated, max 32 chars */
    char     password[65];      /* null-terminated, max 64 chars */
} wifi_config_t;

/* Read from /fos/wifi.dat (safe if no SD card or file missing) */
void wifi_config_load(void);

/* Write to /fos/wifi.dat */
void wifi_config_save(void);

/* Live pointer to in-memory config */
wifi_config_t *wifi_config_get(void);

/* Clear saved config (zero struct and delete file) */
void wifi_config_clear(void);

#endif /* WIFI_CONFIG_H */
