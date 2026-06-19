/*
 * FRANK OS — ESP-01 Netcard AT Command Client
 * Copyright (c) 2026 Mikhail Matveev <xtreme@rh1.tech>
 * https://rh1.tech
 *
 * AT command client for the frank-netcard ESP-01 firmware.
 * Communicates over PIO UART using serial.h.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef NETCARD_H
#define NETCARD_H

#include <stdbool.h>
#include <stdint.h>

/* Tray icon state for network activity visualization */
typedef enum {
    NET_ICON_NOACT,
    NET_ICON_TX,
    NET_ICON_RX,
    NET_ICON_TX_RX
} net_icon_state_t;

/* Async callback types */
typedef void (*nc_data_cb_t)(uint8_t socket_id, const uint8_t *data, uint16_t len);
typedef void (*nc_close_cb_t)(uint8_t socket_id);
typedef void (*nc_wifi_cb_t)(bool connected, const char *ip);
typedef void (*nc_scan_cb_t)(const char *ssid, int rssi, int enc, int ch);

/* Initialization — spawns the netcard FreeRTOS task.
 * Call after serial_init(). Non-blocking. */
void netcard_init_async(void);

/* WiFi */
bool netcard_wifi_join(const char *ssid, const char *pass);
void netcard_wifi_quit(void);
bool netcard_wifi_connected(void);
const char *netcard_wifi_ip(void);

/* WiFi scan — calls cb for each network found, returns count or -1 on error */
int  netcard_wifi_scan(nc_scan_cb_t cb);

/* DNS resolve — returns true on success, writes IP to ip_out */
bool netcard_resolve(const char *hostname, char *ip_out, int ip_out_size);

/* TCP ping — returns connect latency in ms, or -1 on failure */
int  netcard_ping(const char *host, uint16_t port);

/* Sockets (ids 0-3) */
bool netcard_socket_open(uint8_t id, bool tls, const char *host, uint16_t port);
bool netcard_socket_send(uint8_t id, const uint8_t *data, uint16_t len);
void netcard_socket_close(uint8_t id);

/* Async callbacks */
void netcard_set_data_callback(nc_data_cb_t cb);
void netcard_set_close_callback(nc_close_cb_t cb);
void netcard_set_wifi_callback(nc_wifi_cb_t cb);

/* Is netcard hardware available (modem responded to AT probe)? */
bool netcard_is_available(void);

/* Tray icon state based on recent TX/RX activity */
net_icon_state_t netcard_get_icon_state(void);

/* Non-blocking command interface for GUI use.
 * These post commands to the netcard task queue — results are
 * delivered via callbacks or state queries. */
typedef void (*nc_cmd_done_cb_t)(bool success);
void netcard_request_scan(nc_scan_cb_t scan_cb, nc_cmd_done_cb_t done_cb);
void netcard_request_join(const char *ssid, const char *pass, nc_cmd_done_cb_t done_cb);
void netcard_request_quit(nc_cmd_done_cb_t done_cb);

/* Reconfigure the netcard PIO UART to use new RX/TX GPIO pins, then re-probe
 * for the modem. On success the pins are persisted and netcard_is_available()
 * becomes true. The done_cb reports whether a modem answered on those pins. */
void netcard_request_setpins(uint8_t rx_pin, uint8_t tx_pin, nc_cmd_done_cb_t done_cb);

#endif /* NETCARD_H */
