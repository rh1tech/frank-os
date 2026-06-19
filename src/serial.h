/*
 * FRANK OS — PIO UART Serial Driver (ESP-01 Netcard)
 * Copyright (c) 2026 Mikhail Matveev <xtreme@rh1.tech>
 * https://rh1.tech
 *
 * PIO-based UART on PIO2 (gpio base=16) for the ESP-01 netcard.
 * PIO0 = PS/2, PIO1 = I2S audio; only PIO2 can reach pins 38/39.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef SERIAL_H
#define SERIAL_H

#include <stdbool.h>
#include <stdint.h>

/* Initialize PIO UART on PIO2 (claims 2 state machines for TX and RX).
 * Pins default to NETCARD_PIN_RX/TX (or the values saved in wifi_config). */
void serial_init(void);

/* Reconfigure the running PIO UART to use new RX/TX GPIO pins at runtime.
 * Pins must be in the PIO2 window (16..47). Safe to call after serial_init();
 * releases the previous pins, resets the RX ring buffer, and re-arms the SMs. */
void serial_set_pins(uint8_t rx_pin, uint8_t tx_pin);

/* Currently active RX/TX GPIO pins. */
uint8_t serial_get_rx_pin(void);
uint8_t serial_get_tx_pin(void);

/* Send a single character */
void serial_send_char(char c);

/* Send a null-terminated string */
void serial_send_string(const char *s);

/* Send raw binary data */
void serial_send_data(const uint8_t *data, uint16_t len);

/* Returns true if at least one byte is available in the RX ring buffer */
bool serial_readable(void);

/* Read one byte from the RX ring buffer (blocks if empty) */
uint8_t serial_read_byte(void);

#endif /* SERIAL_H */
