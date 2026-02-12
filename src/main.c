/*
 * FRANK OS
 * Copyright (c) 2025 Mikhail Matveev <xtreme@rh1.tech>
 * https://rh1.tech
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <stdio.h>
#include <string.h>
#include "pico/stdlib.h"
#include "pico/multicore.h"
#include "hardware/clocks.h"
#include "hardware/gpio.h"
#include "hardware/structs/qmi.h"
#include "hardware/watchdog.h"
#include "tusb.h"
#include "FreeRTOS.h"
#include "task.h"
#include "board_config.h"
#include "display.h"
#include "window.h"
#include "window_event.h"
#include "ps2.h"
#include "keyboard.h"
#include "sdcard_init.h"
#include "terminal.h"
#include "shell.h"
#include "menu.h"
#include "taskbar.h"
#include "startmenu.h"
#include "sysmenu.h"
#include "filemanager.h"
#include "disphstx.h"
#include "psram.h"
#ifdef PSRAM_MAX_FREQ_MHZ
#include "psram_init.h"
#endif

#define LED_PIN PICO_DEFAULT_LED_PIN

/* Dirty flag — set by input_task, read by compositor_task (both on Core 0) */
static volatile bool g_video_dirty = true;

static inline void led_init(void) {
    gpio_init(LED_PIN);
    gpio_set_dir(LED_PIN, GPIO_OUT);
}

/*==========================================================================
 * Crash dump — stored in .uninitialized_data so it survives warm resets
 * (watchdog, soft reset) but is random on power-on.
 *=========================================================================*/
typedef struct {
    uint32_t magic;     /* 0xDEAD0001 if valid */
    uint32_t pc;
    uint32_t lr;
    uint32_t exc_lr;
    uint32_t sp;
    uint32_t cfsr;
    uint32_t hfsr;
    uint32_t bfar;      /* Bus Fault Address Register */
    uint32_t r0, r1, r2, r3, r12;  /* stacked regs */
} crash_dump_t;

static crash_dump_t __attribute__((section(".uninitialized_data")))
    g_crash_dump;

// Hard fault handler — saves crash info to SRAM, reboots via watchdog.
// Named isr_hardfault to override the weak symbol in Pico SDK's vector table.
void __attribute__((used, naked)) isr_hardfault(void) {
    __asm volatile(
        "tst lr, #4         \n"
        "ite eq              \n"
        "mrseq r0, msp       \n"
        "mrsne r0, psp       \n"
        "mov r1, lr          \n"
        "b hardfault_c_handler \n"
    );
}

void __attribute__((used)) hardfault_c_handler(uint32_t *stack, uint32_t lr_val) {
    g_crash_dump.magic  = 0xDEAD0001;
    g_crash_dump.pc     = stack[6];
    g_crash_dump.lr     = stack[5];
    g_crash_dump.exc_lr = lr_val;
    g_crash_dump.sp     = (uint32_t)stack;
    g_crash_dump.cfsr   = *(volatile uint32_t *)0xE000ED28;
    g_crash_dump.hfsr   = *(volatile uint32_t *)0xE000ED2C;
    g_crash_dump.bfar   = *(volatile uint32_t *)0xE000ED38;
    g_crash_dump.r0     = stack[0];
    g_crash_dump.r1     = stack[1];
    g_crash_dump.r2     = stack[2];
    g_crash_dump.r3     = stack[3];
    g_crash_dump.r12    = stack[4];
    __asm volatile ("dsb 0xF" ::: "memory");
    // Reboot via watchdog in ~2 seconds.  SDK function handles RP2350's
    // separate TICKS peripheral for the watchdog tick source.
    watchdog_enable(2000, false);
    // Blink LED until watchdog fires
    *(volatile unsigned int *)0xd0000038 = (1u << 25);
    for (;;) {
        *(volatile unsigned int *)0xd0000018 = (1u << 25);
        for (volatile int i = 0; i < 3000000; i++);
        *(volatile unsigned int *)0xd0000020 = (1u << 25);
        for (volatile int i = 0; i < 3000000; i++);
    }
}

static void usb_service_task(void *params) {
    (void)params;
    for (;;) {
        tud_task();
        vTaskDelay(1);
    }
}

static void heartbeat_task(void *params) {
    (void)params;
    for (;;) {
        gpio_put(LED_PIN, 1);
        vTaskDelay(pdMS_TO_TICKS(500));
        gpio_put(LED_PIN, 0);
        vTaskDelay(pdMS_TO_TICKS(500));
    }
}

/*==========================================================================
 * Compositor task — runs on Core 0 under FreeRTOS.
 *
 * DispHSTX manages DVI output on Core 1 internally.  The compositor
 * runs here, dispatching window events and recompositing the scene
 * when input marks the display dirty.
 *=========================================================================*/
static void compositor_task(void *params) {
    (void)params;

    for (;;) {
        /* Recomposite when input arrives OR when windows are
         * invalidated (e.g. terminal output, cursor blink). */
        if (g_video_dirty || wm_needs_composite()) {
            g_video_dirty = false;

            wm_dispatch_events();
            wm_composite();

            display_wait_vsync();
        }
        vTaskDelay(1);
    }
}

/*==========================================================================
 * Spawn a new terminal window with its own shell task
 *=========================================================================*/
void spawn_terminal_window(void) {
    extern const uint8_t default_icon_16x16[256];
    wm_set_pending_icon(default_icon_16x16);
    hwnd_t hwnd = terminal_create();
    if (hwnd == HWND_NULL) {
        printf("spawn_terminal_window: out of memory\n");
        return;
    }
    terminal_t *t = terminal_from_hwnd(hwnd);
    if (!t) return;
    shell_start(t);
    wm_set_focus(hwnd);
    taskbar_invalidate();
}

static void input_task(void *params) {
    (void)params;

    /* Absolute cursor position — start at screen center */
    int16_t cur_x = DISPLAY_WIDTH / 2;
    int16_t cur_y = DISPLAY_HEIGHT / 2;
    uint8_t prev_buttons = 0;

    for (;;) {
        /* Poll keyboard */
        keyboard_poll();

        key_event_t kev;
        while (keyboard_get_event(&kev)) {
            /* Intercept Win+T: spawn a new terminal window
             * hid_code uses HID usage codes, not PS/2 scancodes */
            if (kev.pressed &&
                (kev.modifiers & (KBD_MOD_LGUI | KBD_MOD_RGUI)) &&
                kev.hid_code == 0x17 /* HID_KEY_T */) {
                spawn_terminal_window();
                g_video_dirty = true;
                continue;
            }

            /* Win+E: open file manager */
            if (kev.pressed &&
                (kev.modifiers & (KBD_MOD_LGUI | KBD_MOD_RGUI)) &&
                kev.hid_code == 0x08 /* HID_KEY_E */) {
                spawn_filemanager_window();
                g_video_dirty = true;
                continue;
            }

            /* Win key (alone): toggle start menu
             * HID GUI key codes: LGUI=0xE3, RGUI=0xE7 */
            if (kev.pressed &&
                (kev.hid_code == 0xE3 || kev.hid_code == 0xE7) &&
                !(kev.modifiers & ~(KBD_MOD_LGUI | KBD_MOD_RGUI))) {
                startmenu_toggle();
                g_video_dirty = true;
                continue;
            }

            /* Route keyboard to open menus/overlays first */
            if (kev.pressed) {
                if (startmenu_is_open() &&
                    startmenu_handle_key(kev.hid_code, kev.modifiers)) {
                    g_video_dirty = true;
                    continue;
                }
                if (sysmenu_is_open() &&
                    sysmenu_handle_key(kev.hid_code, kev.modifiers)) {
                    g_video_dirty = true;
                    continue;
                }
                if (menu_popup_is_open() &&
                    menu_popup_handle_key(kev.hid_code, kev.modifiers)) {
                    g_video_dirty = true;
                    continue;
                }
                if (menu_is_open() &&
                    menu_handle_key(kev.hid_code, kev.modifiers)) {
                    g_video_dirty = true;
                    continue;
                }
            }

            /* Intercept Alt+Tab: cycle focus between windows */
            if (kev.pressed && (kev.modifiers & KBD_MOD_ALT) &&
                kev.hid_code == 0x2B /* HID_KEY_TAB */) {
                wm_cycle_focus();
                g_video_dirty = true;
                continue;
            }

            /* Alt+Space: open system menu for focused window */
            if (kev.pressed && (kev.modifiers & KBD_MOD_ALT) &&
                kev.hid_code == 0x2C /* HID_KEY_SPACE */) {
                hwnd_t focus = wm_get_focus();
                if (focus != HWND_NULL) {
                    sysmenu_open(focus);
                    g_video_dirty = true;
                }
                continue;
            }

            /* Alt+F4: close focused window */
            if (kev.pressed && (kev.modifiers & KBD_MOD_ALT) &&
                kev.hid_code == 0x3D /* HID_KEY_F4 */) {
                hwnd_t focus = wm_get_focus();
                if (focus != HWND_NULL) {
                    window_event_t we = {0};
                    we.type = WM_CLOSE;
                    wm_post_event(focus, &we);
                    g_video_dirty = true;
                }
                continue;
            }

            /* Alt+letter: open menu bar for focused window */
            if (kev.pressed && (kev.modifiers & KBD_MOD_ALT) &&
                kev.hid_code >= 0x04 && kev.hid_code <= 0x1D) {
                hwnd_t focus = wm_get_focus();
                if (focus != HWND_NULL && menu_try_alt_key(focus, kev.hid_code)) {
                    g_video_dirty = true;
                    continue;
                }
            }

            window_event_t we = {0};
            if (kev.pressed) {
                /* Send WM_KEYDOWN for all key presses */
                we.type = WM_KEYDOWN;
                we.key.scancode = kev.hid_code;
                we.key.modifiers = 0;
                if (kev.modifiers & KBD_MOD_SHIFT) we.key.modifiers |= KMOD_SHIFT;
                if (kev.modifiers & KBD_MOD_CTRL)  we.key.modifiers |= KMOD_CTRL;
                if (kev.modifiers & KBD_MOD_ALT)   we.key.modifiers |= KMOD_ALT;
                wm_post_event_focused(&we);

                /* Also send WM_CHAR for printable ASCII */
                if (kev.ascii >= 0x20 && kev.ascii <= 0x7E) {
                    we.type = WM_CHAR;
                    we.charev.ch = kev.ascii;
                    wm_post_event_focused(&we);
                }
            } else {
                we.type = WM_KEYUP;
                we.key.scancode = kev.hid_code;
                wm_post_event_focused(&we);
            }
            g_video_dirty = true;
        }

        /* Poll mouse */
        int16_t dx, dy;
        int8_t wheel;
        uint8_t buttons;
        if (ps2_mouse_get_state(&dx, &dy, &wheel, &buttons)) {
            /* Convert deltas to absolute — PS/2 Y is inverted */
            cur_x += dx;
            cur_y -= dy;

            /* Clamp to screen bounds */
            if (cur_x < 0) cur_x = 0;
            if (cur_x >= DISPLAY_WIDTH) cur_x = DISPLAY_WIDTH - 1;
            if (cur_y < 0) cur_y = 0;
            if (cur_y >= DISPLAY_HEIGHT) cur_y = DISPLAY_HEIGHT - 1;

            wm_set_cursor_pos(cur_x, cur_y);
            wm_set_mouse_buttons(buttons);

            /* Route all mouse events through the WM handler for
             * hit-testing, focus management, and title-bar dragging */
            wm_handle_mouse_input(WM_MOUSEMOVE, cur_x, cur_y, buttons);

            /* Detect button transitions */
            uint8_t changed = buttons ^ prev_buttons;
            if (changed & 0x01) { /* left button */
                uint8_t type = (buttons & 0x01) ? WM_LBUTTONDOWN : WM_LBUTTONUP;
                wm_handle_mouse_input(type, cur_x, cur_y, buttons);
            }
            if (changed & 0x02) { /* right button */
                uint8_t type = (buttons & 0x02) ? WM_RBUTTONDOWN : WM_RBUTTONUP;
                wm_handle_mouse_input(type, cur_x, cur_y, buttons);
            }
            prev_buttons = buttons;

            /* Signal compositor to recomposite */
            g_video_dirty = true;
        }

        vTaskDelay(pdMS_TO_TICKS(8));
    }
}

int main(void) {
    if (!set_sys_clock_khz(CPU_CLOCK_MHZ * 1000, false)) {
        set_sys_clock_khz(252 * 1000, true);
    }

    /* Remap XIP ATRANS3 so that address 0x10FFF000 maps to flash 0x3FF000.
     * MOS2 apps read function pointers from a sys_table at 0x10FFF000.
     * On 4MB flash, that address is beyond physical flash (returns 0xFF).
     * ATRANS3 covers 0x10C00000–0x10FFFFFF. Setting BASE=0 maps this
     * window to flash 0x000000–0x3FFFFF, so 0x10FFF000 → flash 0x3FF000. */
    qmi_hw->atrans[3] = (0x400u << QMI_ATRANS3_SIZE_LSB)
                       | (0x000u << QMI_ATRANS3_BASE_LSB);

    stdio_init_all();
    for (int i = 0; i < 8; i++) { sleep_ms(500); }

    led_init();

    // Check for saved HardFault info from previous crash
    if (g_crash_dump.magic == 0xDEAD0001) {
        printf("\n!!! PREVIOUS HARDFAULT !!!\n");
        printf("PC  = %08lX\n", g_crash_dump.pc);
        printf("LR  = %08lX\n", g_crash_dump.lr);
        printf("EXC_LR = %08lX  SP = %08lX\n", g_crash_dump.exc_lr, g_crash_dump.sp);
        printf("CFSR = %08lX  HFSR = %08lX\n", g_crash_dump.cfsr, g_crash_dump.hfsr);
        printf("BFAR = %08lX\n", g_crash_dump.bfar);
        printf("R0=%08lX R1=%08lX R2=%08lX R3=%08lX R12=%08lX\n",
               g_crash_dump.r0, g_crash_dump.r1, g_crash_dump.r2,
               g_crash_dump.r3, g_crash_dump.r12);
        stdio_flush();
        g_crash_dump.magic = 0;
    }

    printf("\n== FRANK OS ==\n");
    printf("CPU: %lu MHz\n", (unsigned long)(clock_get_hz(clk_sys) / 1000000));
    printf("Scheduler: FreeRTOS\n");
    printf("Display: DispHSTX DVI\n");
    stdio_flush();

    printf("Initializing display...\n"); stdio_flush();
    display_init();
    printf("Display initialized\n"); stdio_flush();

    /* Initialize PSRAM (if HW init is compiled in) and probe size */
#ifdef PSRAM_MAX_FREQ_MHZ
    uint psram_pin = get_psram_pin();
    printf("PSRAM: init on CS pin %u\n", psram_pin); stdio_flush();
    psram_init(psram_pin);
#endif
    psram_heap_init();
    printf("PSRAM: %u KB\n", psram_detected_bytes() / 1024);
    stdio_flush();

    printf("Initializing PS/2 (unified driver)...\n"); stdio_flush();
    if (ps2_init(pio0, PS2_PIN_CLK, PS2_MOUSE_CLK)) {
        printf("PS/2 PIO initialized (kbd CLK=%d, mouse CLK=%d)\n",
               PS2_PIN_CLK, PS2_MOUSE_CLK);
        if (ps2_mouse_init_device()) {
            printf("PS/2 mouse initialized (wheel=%d)\n", ps2_mouse_has_wheel());
        } else {
            printf("PS/2 mouse device init failed\n");
        }
    } else {
        printf("PS/2 PIO init failed\n");
    }
    stdio_flush();

    wm_init();

    /* Mount SD card */
    printf("Mounting SD card...\n"); stdio_flush();
    if (sdcard_mount()) {
        printf("SD card mounted\n");
    } else {
        printf("SD card mount failed (continuing without SD)\n");
    }
    stdio_flush();

    /* Install multicore lockout handler on Core 1 so flash_block() can
     * safely pause Core 1 during flash erase/program operations.
     * Done after SD mount to avoid SIO FIFO IRQ interfering with SPI init. */
    DispHstxCore1Exec(multicore_lockout_victim_init);
    DispHstxCore1Wait();

    /* Initialize taskbar — clean desktop on boot, no auto-terminal */
    taskbar_init();
    printf("Taskbar initialized\n"); stdio_flush();

    /* One-shot composite so the first frame (desktop + taskbar) is visible */
    wm_composite();

    xTaskCreate(usb_service_task, "usb", 256, NULL, configMAX_PRIORITIES - 1, NULL);
    xTaskCreate(heartbeat_task, "heartbeat", 512, NULL, 1, NULL);
    xTaskCreate(input_task, "input", 1024, NULL, 3, NULL);
    xTaskCreate(compositor_task, "compositor", 1024, NULL, 2, NULL);

    // xTaskCreate leaves BASEPRI raised (ulCriticalNesting = 0xaaaaaaaa by design).
    // Clear it so printf works before the scheduler starts.
    __asm volatile ("msr basepri, %0" :: "r"(0));

    printf("Starting scheduler...\n"); stdio_flush();

    vTaskStartScheduler();
    for (;;) { tight_loop_contents(); }
}
