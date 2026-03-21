/*
 * MurmNES - Raw UART stdio driver
 * Registers a lightweight stdio driver that writes to UART TX
 * without the overhead of pico_enable_stdio_uart's mutex/IRQ.
 */

#include "uart_logging.h"
#include "pico/stdio.h"
#include "pico/stdio/driver.h"

static void uart_out_chars(const char *buf, int len) {
    for (int i = 0; i < len; i++) {
        if (buf[i] == '\n')
            uart_putc_raw(LOG_UART, '\r');
        uart_putc_raw(LOG_UART, buf[i]);
    }
}

stdio_driver_t uart_log_driver = {
    .out_chars = uart_out_chars,
#if PICO_STDIO_ENABLE_CRLF_SUPPORT
    .crlf_enabled = false,
#endif
};

void uart_logging_register(void) {
    stdio_set_driver_enabled(&uart_log_driver, true);
}
