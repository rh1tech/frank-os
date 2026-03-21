/*
 * MurmNES - Lightweight UART logging (TX-only, no stdio/mutex overhead)
 * Bypasses Pico SDK stdio layer to avoid spinlock contention with HDMI DMA.
 */

#ifndef UART_LOGGING_H
#define UART_LOGGING_H

#include "hardware/uart.h"
#include "hardware/gpio.h"
#include <stdio.h>

#define LOG_UART       uart0
#define LOG_UART_TX    0
#define LOG_UART_BAUD  115200

#ifdef __cplusplus
extern "C" {
#endif

static inline void uart_logging_init(void) {
    uart_init(LOG_UART, LOG_UART_BAUD);
    gpio_set_function(LOG_UART_TX, GPIO_FUNC_UART);
    /* TX only — no RX pin, no IRQs */
    uart_set_irqs_enabled(LOG_UART, false, false);
}

/* Register as stdio driver so printf works (call after uart_logging_init) */
void uart_logging_register(void);

#ifdef __cplusplus
}
#endif

#endif /* UART_LOGGING_H */
