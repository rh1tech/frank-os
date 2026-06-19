/*
 * FRANK OS — PIO UART Serial Driver (ESP-01 Netcard)
 * Copyright (c) 2026 Mikhail Matveev <xtreme@rh1.tech>
 * https://rh1.tech
 *
 * ESP-01 wiring (pins from ESP-01's perspective):
 *   ESP-01 RX <- GPIO39 (our TX)
 *   ESP-01 TX -> GPIO38 (our RX)
 *
 * We use PIO-based UART on PIO2 with GPIO base = 16 so it can reach pins
 * 38/39 (RP2350B 48-pin package). PIO0 is used by PS/2 (pins 0-3) and
 * PIO1 by I2S audio (pins 9-11), both of which require GPIO base = 0 and
 * therefore cannot address pins ≥ 32.
 *
 * Uses PIO RX interrupt to drain into a 2KB ring buffer so we never
 * lose bytes even when the main loop is busy.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <stdio.h>
#include <string.h>
#include "pico/stdlib.h"
#include "hardware/pio.h"
#include "hardware/irq.h"
#include "hardware/gpio.h"
#include "board_config.h"
#include "serial.h"
#include "wifi_config.h"
#include "FreeRTOS.h"
#include "uart_tx.pio.h"
#include "uart_rx.pio.h"

/* PIO UART on PIO2. PIO0 = PS/2 (base 0), PIO1 = I2S (base 0);
 * only PIO2 is free to set GPIO base = 16 so pins 38/39 are reachable. */
#define SERIAL_PIO      pio2
#define SERIAL_PIO_IRQ  PIO2_IRQ_0
#define SERIAL_PIO_GPIO_BASE  16

/* ESP-01 pins: our TX = GPIO39 (to ESP RX), our RX = GPIO38 (from ESP TX) */
#define PIN_TX          NETCARD_PIN_TX
#define PIN_RX          NETCARD_PIN_RX

#define SERIAL_BAUD     NETCARD_BAUD

/* PIO2 GPIO base window: pins must fall in [16, 47]. */
#define SERIAL_PIN_MIN  16
#define SERIAL_PIN_MAX  47

/* Interrupt-driven RX ring buffer (must be power of 2).
 * Web pages arrive as multi-KB +SRECV streams — the netcard task must
 * be able to drain the FIFO without losing bytes even under scheduling
 * jitter.  2KB comfortably holds several +SRECV events. */
#define RX_BUF_SIZE     2048
#define RX_BUF_MASK     (RX_BUF_SIZE - 1)

static uint tx_offset, rx_offset;
static uint tx_sm, rx_sm;

static bool    serial_inited;       /* true after first serial_init() */
static uint8_t cur_tx_pin, cur_rx_pin;

static volatile uint8_t  *rx_buf;   /* heap-allocated in serial_init */
static volatile uint16_t rx_head;   /* written by ISR */
static volatile uint16_t rx_tail;   /* read by main loop */

static inline uint8_t pio_uart_read_byte_raw(void) {
    return (uint8_t)(pio_sm_get(SERIAL_PIO, rx_sm) >> 24);
}

/* PIO IRQ handler — drains PIO RX FIFO into the ring buffer */
static void pio_rx_irq_handler(void) {
    while (!pio_sm_is_rx_fifo_empty(SERIAL_PIO, rx_sm)) {
        uint8_t c = pio_uart_read_byte_raw();
        uint16_t next_head = (rx_head + 1) & RX_BUF_MASK;
        if (next_head != rx_tail) {     /* drop byte if buffer full */
            rx_buf[rx_head] = c;
            rx_head = next_head;
        }
    }
}

/* (Re)assign the TX/RX state machines to the given pins. Disables the SMs,
 * releases any previously-assigned pins back to inputs, resets the RX ring
 * buffer, then re-initialises both SM programs on the new pins. */
static void serial_apply_pins(uint8_t rx_pin, uint8_t tx_pin) {
    pio_sm_set_enabled(SERIAL_PIO, tx_sm, false);
    pio_sm_set_enabled(SERIAL_PIO, rx_sm, false);

    /* Return the old pins to plain inputs so they stop driving the bus. */
    if (serial_inited) {
        gpio_init(cur_tx_pin);
        gpio_set_dir(cur_tx_pin, GPIO_IN);
        gpio_init(cur_rx_pin);
        gpio_set_dir(cur_rx_pin, GPIO_IN);
    }

    rx_head = 0;
    rx_tail = 0;

    uart_tx_program_init(SERIAL_PIO, tx_sm, tx_offset, tx_pin, SERIAL_BAUD);
    uart_rx_program_init(SERIAL_PIO, rx_sm, rx_offset, rx_pin, SERIAL_BAUD);

    cur_tx_pin = tx_pin;
    cur_rx_pin = rx_pin;
}

void serial_init(void) {
    /* Allocate RX ring buffer from heap to keep BSS small */
    rx_buf = (volatile uint8_t *)pvPortMalloc(RX_BUF_SIZE);

    /* PIO2 gpio base must be set BEFORE claiming SMs or initialising
     * programs so pin offsets resolve against the 16-47 window. */
    int base_rc = pio_set_gpio_base(SERIAL_PIO, SERIAL_PIO_GPIO_BASE);
    if (base_rc != 0) {
        printf("PIO UART: pio_set_gpio_base failed (%d)\n", base_rc);
    }

    tx_sm = pio_claim_unused_sm(SERIAL_PIO, true);
    rx_sm = pio_claim_unused_sm(SERIAL_PIO, true);

    tx_offset = pio_add_program(SERIAL_PIO, &uart_tx_program);
    rx_offset = pio_add_program(SERIAL_PIO, &uart_rx_program);

    /* Pick pins: saved config (if valid) overrides the board defaults. */
    uint8_t rx = PIN_RX, tx = PIN_TX;
    wifi_config_t *cfg = wifi_config_get();
    if (cfg && cfg->rx_pin >= SERIAL_PIN_MIN && cfg->rx_pin <= SERIAL_PIN_MAX &&
        cfg->tx_pin >= SERIAL_PIN_MIN && cfg->tx_pin <= SERIAL_PIN_MAX &&
        cfg->rx_pin != cfg->tx_pin) {
        rx = cfg->rx_pin;
        tx = cfg->tx_pin;
    }

    serial_apply_pins(rx, tx);
    serial_inited = true;

    printf("PIO UART: TX sm=%u pin=%d, RX sm=%u pin=%d, baud=%u\n",
           (unsigned)tx_sm, cur_tx_pin,
           (unsigned)rx_sm, cur_rx_pin,
           SERIAL_BAUD);

    /* Enable RXFIFO not-empty interrupt for our RX state machine */
    pio_set_irqn_source_enabled(SERIAL_PIO, 0, pis_sm0_rx_fifo_not_empty + rx_sm, true);
    irq_set_exclusive_handler(SERIAL_PIO_IRQ, pio_rx_irq_handler);
    irq_set_enabled(SERIAL_PIO_IRQ, true);

    printf("PIO UART: ready (IRQ-buffered, %u byte RX buf)\n", RX_BUF_SIZE);
}

void serial_set_pins(uint8_t rx_pin, uint8_t tx_pin) {
    if (!serial_inited)
        return;
    if (rx_pin < SERIAL_PIN_MIN || rx_pin > SERIAL_PIN_MAX ||
        tx_pin < SERIAL_PIN_MIN || tx_pin > SERIAL_PIN_MAX ||
        rx_pin == tx_pin)
        return;
    if (rx_pin == cur_rx_pin && tx_pin == cur_tx_pin)
        return;

    /* Quiet the ISR while we tear down and rebuild the state machines. */
    irq_set_enabled(SERIAL_PIO_IRQ, false);
    serial_apply_pins(rx_pin, tx_pin);
    irq_set_enabled(SERIAL_PIO_IRQ, true);

    printf("PIO UART: reconfigured TX pin=%d, RX pin=%d\n", cur_tx_pin, cur_rx_pin);
}

uint8_t serial_get_rx_pin(void) { return cur_rx_pin; }
uint8_t serial_get_tx_pin(void) { return cur_tx_pin; }

void serial_send_char(char c) {
    pio_sm_put_blocking(SERIAL_PIO, tx_sm, (uint32_t)c);
}

void serial_send_string(const char *s) {
    while (*s)
        pio_sm_put_blocking(SERIAL_PIO, tx_sm, (uint32_t)*s++);
}

void serial_send_data(const uint8_t *data, uint16_t len) {
    for (uint16_t i = 0; i < len; i++)
        pio_sm_put_blocking(SERIAL_PIO, tx_sm, (uint32_t)data[i]);
}

bool serial_readable(void) {
    return rx_head != rx_tail;
}

uint8_t serial_read_byte(void) {
    while (rx_head == rx_tail)
        tight_loop_contents();
    uint8_t c = rx_buf[rx_tail];
    rx_tail = (rx_tail + 1) & RX_BUF_MASK;
    return c;
}
