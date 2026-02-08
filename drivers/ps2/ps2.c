/**
 * Unified PS/2 Driver for RP2350
 * 
 * Single PIO program shared between keyboard and mouse state machines.
 * Uses interrupt-driven streaming mode for performant, non-blocking operation.
 * 
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "ps2.h"
#include "ps2.pio.h"

#include <pico/stdlib.h>
#include <hardware/gpio.h>
#include <hardware/clocks.h>
#include <hardware/irq.h>
#include <hardware/sync.h>
#include <string.h>
#include <stdio.h>

//=============================================================================
// PS/2 Mouse Commands
//=============================================================================

#define PS2_CMD_RESET             0xFF
#define PS2_CMD_RESEND            0xFE
#define PS2_CMD_SET_DEFAULTS      0xF6
#define PS2_CMD_DISABLE_STREAM    0xF5
#define PS2_CMD_ENABLE_STREAM     0xF4
#define PS2_CMD_SET_SAMPLE_RATE   0xF3
#define PS2_CMD_GET_DEVICE_ID     0xF2
#define PS2_CMD_SET_REMOTE        0xF0
#define PS2_CMD_READ_DATA         0xEB
#define PS2_CMD_SET_RESOLUTION    0xE8
#define PS2_CMD_SET_SCALING_1_1   0xE6

#define PS2_RESP_ACK              0xFA
#define PS2_RESP_BAT_OK           0xAA

//=============================================================================
// Driver State
//=============================================================================

static PIO ps2_pio = NULL;
static uint ps2_program_offset = 0;

// Keyboard state machine
static uint kbd_sm = 0;
static uint kbd_clk_pin = 0;
static bool kbd_initialized = false;

// Mouse state machine
static uint mouse_sm = 0;
static uint mouse_clk_pin = 0;
static uint mouse_data_pin = 0;
static bool mouse_pio_initialized = false;

// Mouse device state
static ps2_mouse_state_t mouse_state = {0};
static uint8_t mouse_packet[4];
static uint8_t mouse_packet_idx = 0;
static uint8_t mouse_packet_size = 3;

// Ring buffer for interrupt-driven reception
#define MOUSE_RX_BUFFER_SIZE 128
static volatile uint8_t mouse_rx_buffer[MOUSE_RX_BUFFER_SIZE];
static volatile uint8_t mouse_rx_head = 0;  // ISR writes here
static volatile uint8_t mouse_rx_tail = 0;  // Main loop reads from here
static volatile bool mouse_streaming = false;

// Error counters
static uint32_t mouse_frame_errors = 0;
static uint32_t mouse_parity_errors = 0;
static uint32_t mouse_sync_errors = 0;

//=============================================================================
// PIO Helpers
//=============================================================================

static void pio_sm_stop(PIO pio, uint sm) {
    pio_sm_set_enabled(pio, sm, false);
}

static void pio_sm_restart_rx(PIO pio, uint sm) {
    // Clear FIFOs, restart SM, and jump to program start
    pio_sm_clear_fifos(pio, sm);
    pio_sm_restart(pio, sm);
    // Jump to program start (wrap_target = offset 0)
    pio_sm_exec(pio, sm, pio_encode_jmp(ps2_program_offset));
    pio_sm_set_enabled(pio, sm, true);
}

//=============================================================================
// Mouse PIO Interrupt Handler
//=============================================================================

static void mouse_pio_irq_handler(void) {
    // Read all available frames from PIO FIFO into ring buffer
    while (!pio_sm_is_rx_fifo_empty(ps2_pio, mouse_sm)) {
        uint32_t raw = pio_sm_get(ps2_pio, mouse_sm);
        
        // Skip all-zero frames (noise/glitch)
        if (raw == 0) continue;
        
        // Decode frame
        int result = ps2_rx_decode_frame(raw);
        if (result >= 0) {
            // Valid byte - add to ring buffer
            uint8_t next_head = (mouse_rx_head + 1) % MOUSE_RX_BUFFER_SIZE;
            if (next_head != mouse_rx_tail) {  // Not full
                mouse_rx_buffer[mouse_rx_head] = (uint8_t)result;
                mouse_rx_head = next_head;
            }
            // If buffer full, drop the byte (better than blocking ISR)
        } else {
            // Track errors but don't block
            if (result == -1) mouse_frame_errors++;
            else mouse_parity_errors++;
        }
    }
}

static void mouse_enable_irq(void) {
    // Use IRQ_1 to avoid conflicts with other drivers that might use IRQ_0
    uint irq_num = (ps2_pio == pio0) ? PIO0_IRQ_1 : PIO1_IRQ_1;

    // Enable RXNEMPTY interrupt for mouse state machine on IRQ index 1
    pio_set_irqn_source_enabled(ps2_pio, 1, pis_sm0_rx_fifo_not_empty + mouse_sm, true);

    // Set up interrupt handler
    irq_set_exclusive_handler(irq_num, mouse_pio_irq_handler);
    irq_set_enabled(irq_num, true);
}

static void mouse_disable_irq(void) {
    uint irq_num = (ps2_pio == pio0) ? PIO0_IRQ_1 : PIO1_IRQ_1;

    // Disable interrupt
    irq_set_enabled(irq_num, false);
    pio_set_irqn_source_enabled(ps2_pio, 1, pis_sm0_rx_fifo_not_empty + mouse_sm, false);
}

// Get byte from ring buffer (non-blocking, called from main loop)
static int mouse_rx_get_byte(void) {
    if (mouse_rx_head == mouse_rx_tail) {
        return -1;  // Buffer empty
    }
    uint8_t data = mouse_rx_buffer[mouse_rx_tail];
    mouse_rx_tail = (mouse_rx_tail + 1) % MOUSE_RX_BUFFER_SIZE;
    return data;
}

// Check how many bytes available in ring buffer
static uint8_t mouse_rx_available(void) {
    return (mouse_rx_head - mouse_rx_tail + MOUSE_RX_BUFFER_SIZE) % MOUSE_RX_BUFFER_SIZE;
}

//=============================================================================
// Mouse Host-to-Device Communication (bit-bang while PIO stopped)
//=============================================================================

static inline void mouse_clk_low(void) {
    gpio_set_dir(mouse_clk_pin, GPIO_OUT);
    gpio_put(mouse_clk_pin, 0);
}

static inline void mouse_clk_release(void) {
    gpio_set_dir(mouse_clk_pin, GPIO_IN);
}

static inline void mouse_data_low(void) {
    gpio_set_dir(mouse_data_pin, GPIO_OUT);
    gpio_put(mouse_data_pin, 0);
}

static inline void mouse_data_high(void) {
    gpio_set_dir(mouse_data_pin, GPIO_OUT);
    gpio_put(mouse_data_pin, 1);
}

static inline void mouse_data_release(void) {
    gpio_set_dir(mouse_data_pin, GPIO_IN);
}

static inline bool mouse_read_clk(void) {
    return gpio_get(mouse_clk_pin);
}

static inline bool mouse_read_data(void) {
    return gpio_get(mouse_data_pin);
}

static bool mouse_wait_clk(bool state, uint32_t timeout_us) {
    absolute_time_t deadline = make_timeout_time_us(timeout_us);
    while (gpio_get(mouse_clk_pin) != state) {
        if (time_reached(deadline)) return false;
    }
    return true;
}

static bool mouse_wait_data(bool state, uint32_t timeout_us) {
    absolute_time_t deadline = make_timeout_time_us(timeout_us);
    while (gpio_get(mouse_data_pin) != state) {
        if (time_reached(deadline)) return false;
    }
    return true;
}

static uint8_t calc_odd_parity(uint8_t data) {
    uint8_t parity = 1;
    while (data) {
        parity ^= (data & 1);
        data >>= 1;
    }
    return parity;
}

/**
 * Send a byte to mouse using PS/2 host-to-device protocol.
 * Stops PIO and disables interrupt during transmission.
 */
static bool mouse_send_byte(uint8_t data) {
    uint8_t parity = calc_odd_parity(data);
    
    // Disable interrupt and stop PIO to take over GPIO
    if (mouse_streaming) {
        mouse_disable_irq();
    }
    pio_sm_set_enabled(ps2_pio, mouse_sm, false);
    
    // Re-init GPIO for bit-bang (switch from PIO to SIO)
    gpio_init(mouse_clk_pin);
    gpio_init(mouse_data_pin);
    gpio_pull_up(mouse_clk_pin);
    gpio_pull_up(mouse_data_pin);
    gpio_set_dir(mouse_clk_pin, GPIO_IN);
    gpio_set_dir(mouse_data_pin, GPIO_IN);
    
    // Wait for bus idle
    sleep_us(50);
    
    // 1. Inhibit communication - pull clock low >100us
    mouse_clk_low();
    busy_wait_us_32(150);
    
    // 2. Request-to-send - pull data low
    mouse_data_low();
    busy_wait_us_32(10);
    
    // 3. Release clock - device will start clocking
    mouse_clk_release();
    
    // 4. Wait for device to pull clock low
    if (!mouse_wait_clk(false, 15000)) {
        mouse_data_release();
        goto restart_pio;
    }
    
    // 5. Send 8 data bits on falling clock edges
    for (int i = 0; i < 8; i++) {
        if (data & (1 << i)) {
            mouse_data_release();
        } else {
            mouse_data_low();
        }
        if (!mouse_wait_clk(true, 5000)) goto fail;
        if (!mouse_wait_clk(false, 5000)) goto fail;
    }
    
    // 6. Send parity bit
    if (parity) {
        mouse_data_release();
    } else {
        mouse_data_low();
    }
    if (!mouse_wait_clk(true, 5000)) goto fail;
    if (!mouse_wait_clk(false, 5000)) goto fail;
    
    // 7. Release data for stop bit
    mouse_data_release();
    if (!mouse_wait_clk(true, 5000)) goto fail;
    
    // 8. Wait for ACK (device pulls data low)
    if (!mouse_wait_data(false, 5000)) goto fail;
    if (!mouse_wait_clk(false, 5000)) goto fail;
    if (!mouse_wait_clk(true, 5000)) goto fail;
    if (!mouse_wait_data(true, 5000)) goto fail;
    
    // Reinit GPIO for PIO control IMMEDIATELY
    pio_gpio_init(ps2_pio, mouse_clk_pin);
    pio_gpio_init(ps2_pio, mouse_data_pin);
    gpio_pull_up(mouse_clk_pin);
    gpio_pull_up(mouse_data_pin);
    
    // Clear FIFO, jump to start, and enable - don't use pio_sm_restart
    pio_sm_clear_fifos(ps2_pio, mouse_sm);
    pio_sm_exec(ps2_pio, mouse_sm, pio_encode_jmp(ps2_program_offset));
    pio_sm_set_enabled(ps2_pio, mouse_sm, true);
    
    // Re-enable interrupt if streaming was active
    if (mouse_streaming) {
        mouse_enable_irq();
    }
    return true;
    
fail:
    mouse_data_release();
    mouse_clk_release();
    
restart_pio:
    pio_gpio_init(ps2_pio, mouse_clk_pin);
    pio_gpio_init(ps2_pio, mouse_data_pin);
    gpio_pull_up(mouse_clk_pin);
    gpio_pull_up(mouse_data_pin);
    pio_sm_restart_rx(ps2_pio, mouse_sm);
    
    // Re-enable interrupt if streaming was active
    if (mouse_streaming) {
        mouse_enable_irq();
    }
    return false;
}

/**
 * Get a byte from mouse PIO FIFO with timeout.
 */
static int mouse_get_byte(uint32_t timeout_ms) {
    absolute_time_t deadline = make_timeout_time_ms(timeout_ms);
    
    while (!time_reached(deadline)) {
        if (!pio_sm_is_rx_fifo_empty(ps2_pio, mouse_sm)) {
            uint32_t raw = pio_sm_get(ps2_pio, mouse_sm);
            
            // Skip all-zero frames (noise/glitch)
            if (raw == 0) {
                continue;
            }
            
            int result = ps2_rx_decode_frame(raw);
            if (result >= 0) {
                return result;
            }
            // Frame/parity error - try next
            if (result == -1) mouse_frame_errors++;
            else mouse_parity_errors++;
        }
        sleep_us(100);
    }
    return -1;
}

/**
 * Send command and wait for ACK.
 */
static bool mouse_send_command(uint8_t cmd) {
    if (!mouse_send_byte(cmd)) {
        printf("Mouse: send_byte(0x%02X) failed\n", cmd);
        return false;
    }
    
    int resp = mouse_get_byte(100);
    if (resp != PS2_RESP_ACK) {
        printf("Mouse: cmd 0x%02X got 0x%02X (expected ACK)\n", cmd, resp);
        return false;
    }
    return true;
}

/**
 * Send command with parameter.
 */
static bool mouse_send_command_param(uint8_t cmd, uint8_t param) {
    if (!mouse_send_command(cmd)) return false;
    return mouse_send_command(param);
}

//=============================================================================
// Mouse Packet Processing
//=============================================================================

static uint8_t mouse_last_buttons = 0;
static uint8_t mouse_reported_buttons = 0;  /* last value returned by get_state */
static uint32_t mouse_valid_packet_count = 0;

static void mouse_process_packet(void) {
    uint8_t status = mouse_packet[0];

    // Validate sync bit (bit 3 must be 1)
    if (!(status & 0x08)) {
        mouse_sync_errors++;
        return;
    }

    // Skip on overflow - data is unreliable
    if (status & 0xC0) {
        return;
    }

    // Get raw movement bytes
    uint8_t x_raw = mouse_packet[1];
    uint8_t y_raw = mouse_packet[2];

    // Sign bits from status byte (9th bit for 9-bit two's complement)
    bool x_neg = (status & 0x10) != 0;
    bool y_neg = (status & 0x20) != 0;

    // 9-bit two's complement: range is -256 to +255
    int16_t dx = x_neg ? ((int16_t)x_raw - 256) : (int16_t)x_raw;
    int16_t dy = y_neg ? ((int16_t)y_raw - 256) : (int16_t)y_raw;

    // Clamp corrupt pattern: sign bit set but raw byte is tiny
    // This gives dx like -256, -255 etc. Clamp to reasonable max
    if (x_neg && x_raw < 32) dx = -16;
    if (y_neg && y_raw < 32) dy = -16;

    // Track valid packets for debugging
    mouse_valid_packet_count++;

    // Extract button state from packet
    uint8_t new_buttons = status & 0x07;

    if (new_buttons != mouse_last_buttons) {
        mouse_last_buttons = new_buttons;
    }

    // Update button state
    mouse_state.buttons = new_buttons;

    // Accumulate deltas with overflow protection (clamp to int16_t range)
    int32_t new_dx = (int32_t)mouse_state.delta_x + dx;
    int32_t new_dy = (int32_t)mouse_state.delta_y + dy;
    if (new_dx > 32767) new_dx = 32767;
    if (new_dx < -32768) new_dx = -32768;
    if (new_dy > 32767) new_dy = 32767;
    if (new_dy < -32768) new_dy = -32768;
    mouse_state.delta_x = (int16_t)new_dx;
    mouse_state.delta_y = (int16_t)new_dy;

    // Wheel (IntelliMouse)
    if (mouse_packet_size == 4) {
        int8_t wheel = (int8_t)(mouse_packet[3] & 0x0F);
        if (wheel & 0x08) wheel |= 0xF0;
        mouse_state.wheel += wheel;
    }
}

//=============================================================================
// Mouse Device Initialization
//=============================================================================

static bool mouse_enable_intellimouse(void) {
    // Magic sequence to enable IntelliMouse
    if (!mouse_send_command_param(PS2_CMD_SET_SAMPLE_RATE, 200)) return false;
    if (!mouse_send_command_param(PS2_CMD_SET_SAMPLE_RATE, 100)) return false;
    if (!mouse_send_command_param(PS2_CMD_SET_SAMPLE_RATE, 80)) return false;
    
    if (!mouse_send_command(PS2_CMD_GET_DEVICE_ID)) return false;
    
    int id = mouse_get_byte(100);
    printf("Mouse: Device ID after magic: 0x%02X\n", id);
    
    if (id == 0x03 || id == 0x04) {
        mouse_packet_size = 4;
        mouse_state.has_wheel = 1;
        return true;
    }
    return false;
}

static bool mouse_reset_and_init(void) {
    printf("Mouse: Sending reset...\n");
    
    // Drain any garbage from FIFO first
    while (!pio_sm_is_rx_fifo_empty(ps2_pio, mouse_sm)) {
        pio_sm_get(ps2_pio, mouse_sm);
    }
    
    if (!mouse_send_byte(PS2_CMD_RESET)) {
        printf("Mouse: Reset send failed\n");
        return false;
    }
    
    // Mouse reset takes 300-500ms for self-test
    // Wait for ACK (0xFA), then BAT OK (0xAA), then device ID (0x00)
    int resp = mouse_get_byte(2000);  // Longer timeout for reset
    printf("Mouse: Response 1: 0x%02X\n", resp);
    
    if (resp == PS2_RESP_ACK) {
        // Got ACK, wait for BAT
        resp = mouse_get_byte(2000);
        printf("Mouse: Response 2: 0x%02X\n", resp);
    }
    
    if (resp != PS2_RESP_BAT_OK) {
        printf("Mouse: BAT failed (got 0x%02X)\n", resp);
        return false;
    }
    
    // Get device ID
    int id = mouse_get_byte(100);
    printf("Mouse: Device ID: 0x%02X\n", id);
    
    // Try IntelliMouse
    if (mouse_enable_intellimouse()) {
        printf("Mouse: IntelliMouse enabled\n");
    }
    
    // Configure - 200Hz sample rate, high resolution (8 counts/mm)
    mouse_send_command_param(PS2_CMD_SET_SAMPLE_RATE, 200);
    mouse_send_command_param(PS2_CMD_SET_RESOLUTION, 3);  // 0=1cnt/mm, 1=2, 2=4, 3=8
    mouse_send_command(PS2_CMD_SET_SCALING_1_1);
    
    // Enable streaming mode FIRST (before enabling IRQ!)
    // The ACK for this command must be received via polling, not IRQ
    if (!mouse_send_command(PS2_CMD_ENABLE_STREAM)) {
        printf("Mouse: Enable stream failed\n");
        return false;
    }
    
    // Clear ring buffer before enabling interrupt reception
    mouse_rx_head = 0;
    mouse_rx_tail = 0;
    
    // NOW enable PIO interrupt for non-blocking reception of mouse data
    mouse_enable_irq();
    mouse_streaming = true;
    
    printf("Mouse: Streaming mode enabled with interrupts\n");
    return true;
}

//=============================================================================
// Public API - Initialization
//=============================================================================

bool ps2_init(PIO pio, uint kbd_clk, uint mouse_clk) {
    ps2_pio = pio;
    kbd_clk_pin = kbd_clk;
    mouse_clk_pin = mouse_clk;
    mouse_data_pin = mouse_clk + 1;
    
    printf("PS/2: Initializing on PIO%d\n", pio == pio0 ? 0 : 1);
    printf("PS/2: Keyboard CLK=%d DATA=%d\n", kbd_clk, kbd_clk + 1);
    printf("PS/2: Mouse CLK=%d DATA=%d\n", mouse_clk, mouse_clk + 1);
    
    // Add PIO program (only once)
    if (!pio_can_add_program(pio, &ps2_rx_program)) {
        printf("PS/2: Cannot add PIO program\n");
        return false;
    }
    ps2_program_offset = pio_add_program(pio, &ps2_rx_program);
    
    // Claim state machines
    int sm = pio_claim_unused_sm(pio, false);
    if (sm < 0) {
        printf("PS/2: No free SM for keyboard\n");
        return false;
    }
    kbd_sm = (uint)sm;
    
    sm = pio_claim_unused_sm(pio, false);
    if (sm < 0) {
        printf("PS/2: No free SM for mouse\n");
        pio_sm_unclaim(pio, kbd_sm);
        return false;
    }
    mouse_sm = (uint)sm;
    
    printf("PS/2: Keyboard SM=%d, Mouse SM=%d\n", kbd_sm, mouse_sm);
    
    // Initialize keyboard state machine
    ps2_rx_program_init(pio, kbd_sm, ps2_program_offset, kbd_clk);
    kbd_initialized = true;
    
    // Initialize mouse state machine
    ps2_rx_program_init(pio, mouse_sm, ps2_program_offset, mouse_clk);
    mouse_pio_initialized = true;
    
    return true;
}

bool ps2_mouse_pio_init(PIO pio, uint mouse_clk) {
    ps2_pio = pio;
    mouse_clk_pin = mouse_clk;
    mouse_data_pin = mouse_clk + 1;
    
    printf("PS/2 Mouse: Initializing on PIO%d\n", pio == pio0 ? 0 : 1);
    printf("PS/2 Mouse: CLK=%d DATA=%d\n", mouse_clk, mouse_clk + 1);
    
    // Add PIO program
    if (!pio_can_add_program(pio, &ps2_rx_program)) {
        printf("PS/2 Mouse: Cannot add PIO program\n");
        return false;
    }
    ps2_program_offset = pio_add_program(pio, &ps2_rx_program);
    
    // Claim state machine for mouse only
    int sm = pio_claim_unused_sm(pio, false);
    if (sm < 0) {
        printf("PS/2 Mouse: No free SM\n");
        return false;
    }
    mouse_sm = (uint)sm;
    
    printf("PS/2 Mouse: SM=%d\n", mouse_sm);
    
    // Initialize mouse state machine
    ps2_rx_program_init(pio, mouse_sm, ps2_program_offset, mouse_clk);
    mouse_pio_initialized = true;
    
    return true;
}

//=============================================================================
// Public API - Mouse
//=============================================================================

bool ps2_mouse_init_device(void) {
    if (!mouse_pio_initialized) {
        printf("Mouse: PIO not initialized\n");
        return false;
    }
    
    memset(&mouse_state, 0, sizeof(mouse_state));
    mouse_packet_idx = 0;
    mouse_packet_size = 3;
    mouse_frame_errors = 0;
    mouse_parity_errors = 0;
    mouse_sync_errors = 0;
    
    sleep_ms(100);
    
    // Check bus state
    printf("Mouse: CLK=%d DATA=%d\n", mouse_read_clk(), mouse_read_data());
    
    for (int attempt = 0; attempt < 3; attempt++) {
        printf("Mouse: Init attempt %d\n", attempt + 1);
        
        // Clear FIFO
        while (!pio_sm_is_rx_fifo_empty(ps2_pio, mouse_sm)) {
            pio_sm_get(ps2_pio, mouse_sm);
        }
        
        if (mouse_reset_and_init()) {
            mouse_state.initialized = 1;
            printf("Mouse: Init SUCCESS\n");
            return true;
        }
        
        sleep_ms(200);
    }
    
    printf("Mouse: Init FAILED\n");
    return false;
}

void ps2_mouse_poll(void) {
    // Process bytes from the interrupt-filled ring buffer
    // Limit processing to prevent runaway loops
    int bytes_processed = 0;
    const int MAX_BYTES_PER_POLL = 32;  // Max ~8 packets per poll

    while (mouse_rx_available() > 0 && bytes_processed < MAX_BYTES_PER_POLL) {
        int data = mouse_rx_get_byte();
        if (data < 0) break;
        bytes_processed++;

        // If we're starting a new packet, validate sync bit
        if (mouse_packet_idx == 0) {
            // Byte 0 must have bit 3 set (sync bit)
            if (!(data & 0x08)) {
                mouse_sync_errors++;
                // If too many sync errors, reset packet state
                if (mouse_sync_errors > 100) {
                    mouse_sync_errors = 0;
                    mouse_packet_idx = 0;
                }
                continue;  // Skip this byte, try next as potential packet start
            }
        }

        mouse_packet[mouse_packet_idx++] = (uint8_t)data;

        // Process complete packet
        if (mouse_packet_idx >= mouse_packet_size) {
            mouse_process_packet();
            mouse_packet_idx = 0;
        }
    }
}

bool ps2_mouse_get_state(int16_t *dx, int16_t *dy, int8_t *wheel, uint8_t *buttons) {
    // Process any pending data from the ring buffer
    ps2_mouse_poll();

    // Disable only the mouse PIO interrupt (not all interrupts)
    // This avoids blocking HDMI and audio
    uint irq_num = (ps2_pio == pio0) ? PIO0_IRQ_1 : PIO1_IRQ_1;
    irq_set_enabled(irq_num, false);

    bool has_data = (mouse_state.delta_x != 0 ||
                     mouse_state.delta_y != 0 ||
                     mouse_state.wheel != 0 ||
                     mouse_state.buttons != mouse_reported_buttons);

    if (dx) *dx = mouse_state.delta_x;
    if (dy) *dy = mouse_state.delta_y;
    if (wheel) *wheel = mouse_state.wheel;
    if (buttons) *buttons = mouse_state.buttons;

    mouse_state.delta_x = 0;
    mouse_state.delta_y = 0;
    mouse_state.wheel = 0;
    mouse_reported_buttons = mouse_state.buttons;

    // Re-enable mouse interrupt
    irq_set_enabled(irq_num, true);

    return has_data;
}

bool ps2_mouse_is_initialized(void) {
    return mouse_state.initialized != 0;
}

bool ps2_mouse_has_wheel(void) {
    return mouse_state.has_wheel != 0;
}

void ps2_mouse_get_errors(uint32_t *frame_err, uint32_t *parity_err, uint32_t *sync_err) {
    if (frame_err) *frame_err = mouse_frame_errors;
    if (parity_err) *parity_err = mouse_parity_errors;
    if (sync_err) *sync_err = mouse_sync_errors;
}

//=============================================================================
// Public API - Keyboard (raw access)
//=============================================================================

bool ps2_kbd_has_data(void) {
    return !pio_sm_is_rx_fifo_empty(ps2_pio, kbd_sm);
}

uint32_t ps2_kbd_get_raw(void) {
    if (pio_sm_is_rx_fifo_empty(ps2_pio, kbd_sm)) {
        return 0;
    }
    return pio_sm_get(ps2_pio, kbd_sm);
}

int ps2_kbd_get_byte(void) {
    if (pio_sm_is_rx_fifo_empty(ps2_pio, kbd_sm)) {
        return -1;
    }
    uint32_t raw = pio_sm_get(ps2_pio, kbd_sm);
    return ps2_rx_decode_frame(raw);
}
