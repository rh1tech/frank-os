/*
 * FRANK OS
 * Copyright (c) 2026 Mikhail Matveev <xtreme@rh1.tech>
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
#include "hardware/dma.h"
#include "hardware/structs/pio.h"
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
#include "run_dialog.h"
#include "file_assoc.h"
#include "desktop.h"
#include "sysmenu.h"
#include "filemanager.h"
#include "cursor.h"
#include "font.h"
#include "disphstx.h"
#include "psram.h"
#include "swap.h"
#include "alttab.h"
#include "sound.h"
#include "snd.h"
#include "startup_sound.h"
#include "settings.h"
#include "control_panel.h"
#include "network_settings.h"
#include "serial.h"
#include "netcard.h"
#include "wifi_config.h"
#ifdef PSRAM_MAX_FREQ_MHZ
#include "psram_init.h"
#endif

/*==========================================================================
 * UF2 Over-Mode Boot Support
 *
 * ZERO_BLOCK: 4KB placeholder at 0x10EFF000 (just below OS code).
 * When flashing user firmware, sector 0 is saved here with a magic marker.
 *
 * before_main(): constructor that runs before main().  If SRAM magic is
 * set (from a previous load_firmware + watchdog reboot), it loads the
 * firmware's original SP and Reset from ZERO_BLOCK and jumps to it.
 * If Space key is held, it skips the jump and falls through to FRANK OS.
 *=========================================================================*/
#define ZERO_BLOCK_OFFSET   ((16ul << 20) - (1ul << 20) - (4ul << 10))  /* 0xEFF000 */
#define ZERO_BLOCK_ADDRESS  (XIP_BASE + ZERO_BLOCK_OFFSET)              /* 0x10EFF000 */
#define FLASH_MAGIC_OVER    0x3836d91au
#define SRAM_MAGIC_BOOT     0x383da910u
#define SRAM_MAGIC_ADDR     ((volatile uint32_t *)(0x20000000 + (512 << 10) - 8))
#define SRAM_UIFORCE_ADDR   ((volatile uint32_t *)(0x20000000 + (512 << 10) - 4))
#define SRAM_UIFORCE_MAGIC  0x17F00FFFu

/* 4KB erase block placeholder — placed at 0x10EFF000 by linker */
const uint8_t erase_block[4096] __attribute__((aligned(4096), section(".erase_block"), used)) = {0};

/* PS/2 GPIO registers for bare-metal Space key check */
#define BM_IO_BANK0_BASE   0x40028000u
#define BM_PADS_BANK0_BASE 0x40038000u
#define BM_SIO_BASE         0xD0000000u
#define BM_GPIO_IN_REG      (*(volatile uint32_t *)(BM_SIO_BASE + 0x008))
#define BM_PS2_CLK_PIN      2
#define BM_PS2_DATA_PIN     3

__attribute__((constructor))
static void before_main(void) {
    /* Skip if UI-force magic is set (user held Space last time) */
    if (*SRAM_UIFORCE_ADDR == SRAM_UIFORCE_MAGIC) {
        *SRAM_UIFORCE_ADDR = 0;
        return;
    }

    if (*SRAM_MAGIC_ADDR != SRAM_MAGIC_BOOT)
        return;

    /* Clear SRAM magic so we don't loop */
    *SRAM_MAGIC_ADDR = 0;

    /* Check if Space key is held — raw PS/2 GPIO polling.
     * If Space detected, fall through to FRANK OS. */
    {
        /* Configure GPIO 2 (CLK) and GPIO 3 (DATA) as SIO inputs with pull-ups */
        *(volatile uint32_t *)(BM_IO_BANK0_BASE + 4 + BM_PS2_CLK_PIN * 8) = 5;
        *(volatile uint32_t *)(BM_IO_BANK0_BASE + 4 + BM_PS2_DATA_PIN * 8) = 5;
        uint32_t pad = (1u << 6) | (1u << 3) | (1u << 1); /* IE | PUE | SCHMITT */
        *(volatile uint32_t *)(BM_PADS_BANK0_BASE + 4 + BM_PS2_CLK_PIN * 4) = pad;
        *(volatile uint32_t *)(BM_PADS_BANK0_BASE + 4 + BM_PS2_DATA_PIN * 4) = pad;
        for (volatile int d = 0; d < 5000; d++) ;

        uint32_t clk_mask  = 1u << BM_PS2_CLK_PIN;
        uint32_t data_mask = 1u << BM_PS2_DATA_PIN;

        for (int attempt = 0; attempt < 30; attempt++) {
            int timeout = 200000;
            while ((BM_GPIO_IN_REG & clk_mask) && --timeout > 0) ;
            if (timeout <= 0) continue;
            timeout = 50000;
            while (!(BM_GPIO_IN_REG & clk_mask) && --timeout > 0) ;
            if (timeout <= 0) continue;

            uint8_t data = 0;
            int ok = 1;
            for (int i = 0; i < 8 && ok; i++) {
                timeout = 50000;
                while ((BM_GPIO_IN_REG & clk_mask) && --timeout > 0) ;
                if (timeout <= 0) { ok = 0; break; }
                if (BM_GPIO_IN_REG & data_mask) data |= (1u << i);
                timeout = 50000;
                while (!(BM_GPIO_IN_REG & clk_mask) && --timeout > 0) ;
                if (timeout <= 0) { ok = 0; break; }
            }
            if (!ok) continue;
            /* Skip parity + stop bits */
            timeout = 50000;
            while ((BM_GPIO_IN_REG & clk_mask) && --timeout > 0) ;
            timeout = 50000;
            while (!(BM_GPIO_IN_REG & clk_mask) && --timeout > 0) ;
            timeout = 50000;
            while ((BM_GPIO_IN_REG & clk_mask) && --timeout > 0) ;
            timeout = 50000;
            while (!(BM_GPIO_IN_REG & clk_mask) && --timeout > 0) ;

            if (data == 0x29) {
                /* Space detected — set UI force magic and fall through */
                *SRAM_UIFORCE_ADDR = SRAM_UIFORCE_MAGIC;
                return;
            }
        }
    }

    /* No Space key — check ZERO_BLOCK magic and jump to firmware */
    if (((volatile uint32_t *)ZERO_BLOCK_ADDRESS)[1023] == FLASH_MAGIC_OVER) {
        __asm volatile (
            "ldr r0, =%[zb_addr]\n"
            "ldmia r0, {r0, r1}\n"
            "msr msp, r0\n"
            "bx r1\n"
            :: [zb_addr] "X" (ZERO_BLOCK_ADDRESS)
            : "r0", "r1", "memory"
        );
        __builtin_unreachable();
    }

    /* ZERO_BLOCK not valid — fall through to normal FRANK OS boot */
}

/* ---- Static allocation support for FreeRTOS ---- */
static StaticTask_t idle_tcb;
static StackType_t  idle_stack[configMINIMAL_STACK_SIZE];

void vApplicationGetIdleTaskMemory(StaticTask_t **ppxIdleTaskTCBBuffer,
                                   StackType_t **ppxIdleTaskStackBuffer,
                                   configSTACK_DEPTH_TYPE *puxIdleTaskStackSize) {
    *ppxIdleTaskTCBBuffer   = &idle_tcb;
    *ppxIdleTaskStackBuffer = idle_stack;
    *puxIdleTaskStackSize   = configMINIMAL_STACK_SIZE;
}

static StaticTask_t timer_tcb;
static StackType_t  timer_stack[configTIMER_TASK_STACK_DEPTH];

void vApplicationGetTimerTaskMemory(StaticTask_t **ppxTimerTaskTCBBuffer,
                                    StackType_t **ppxTimerTaskStackBuffer,
                                    configSTACK_DEPTH_TYPE *puxTimerTaskStackSize) {
    *ppxTimerTaskTCBBuffer   = &timer_tcb;
    *ppxTimerTaskStackBuffer = timer_stack;
    *puxTimerTaskStackSize   = configTIMER_TASK_STACK_DEPTH;
}

/* Dirty flag — set by input_task, read by compositor_task (both on Core 0) */
static volatile bool g_video_dirty = true;

/* Deferred spawn/open flags — set by input_task, consumed by compositor_task.
 * Avoids calling heavy WM/allocation functions from the small input_task stack
 * and prevents races with the compositor. */
static volatile bool g_spawn_terminal_pending       = false;
static volatile bool g_spawn_navigator_pending      = false;
static volatile bool g_open_run_dialog_pending      = false;
static volatile bool g_spawn_control_panel_pending  = false;
static volatile bool g_spawn_network_settings_pending = false;

/* Boot cursor state: cursor is hidden after the hourglass timeout until
 * the user first moves the mouse.  Non-static so wm_composite can skip
 * stamping the cursor while hidden. */
volatile bool boot_cursor_hidden = false;

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

// Forward declaration — used by the vector table fixup in main().
void isr_hardfault(void);

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

/* Render a string at character grid (col, row) on the BSOD blue screen */
static void bsod_text(int col, int row, const char *s) {
    int x = col * 8, y = row * 16;
    while (*s) {
        const uint8_t *g = font8x16_get_glyph((uint8_t)*s);
        display_blit_glyph_8wide(x, y, g, 16, COLOR_WHITE, COLOR_BLUE);
        x += 8;
        s++;
    }
}

/* Render a 32-bit hex value into buf (must hold 9 bytes) */
static void hex32(char *buf, uint32_t v) {
    static const char hex[] = "0123456789ABCDEF";
    for (int i = 7; i >= 0; i--) {
        buf[i] = hex[v & 0xF];
        v >>= 4;
    }
    buf[8] = '\0';
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

    /* ---- Kill all audio immediately ----
     * Abort DMA channels 10/11 (audio ping-pong) and disable all
     * PIO1 state machines so the I2S DAC goes silent.  We poke
     * hardware registers directly because normal driver APIs are
     * unsafe from an exception handler. */
    dma_hw->abort = (1u << 10) | (1u << 11);          /* abort audio DMA */
    while (dma_hw->abort & ((1u << 10) | (1u << 11)))  /* wait until done */
        tight_loop_contents();
    pio1_hw->ctrl = 0;                                 /* disable all PIO1 SMs */

    /* ---- Blue Screen of Death ---- */
    display_clear(COLOR_BLUE);

    char h[9];

    bsod_text(2, 2, "FRANK OS");
    bsod_text(2, 4, "A fatal exception has occurred at 0x");
    hex32(h, g_crash_dump.pc);
    bsod_text(38, 4, h);

    bsod_text(2, 6, "* Press reset to restart the computer.");
    bsod_text(2, 7, "* The system will auto-restart in 10 seconds.");

    bsod_text(2, 9, "Technical information:");

    /* PC  LR  SP */
    {
        char line[80];
        memset(line, 0, sizeof(line));
        memcpy(line, "PC=", 3);
        hex32(line + 3, g_crash_dump.pc);
        memcpy(line + 11, "  LR=", 5);
        hex32(line + 16, g_crash_dump.lr);
        memcpy(line + 24, "  SP=", 5);
        hex32(line + 29, g_crash_dump.sp);
        bsod_text(2, 11, line);
    }

    /* CFSR  HFSR  BFAR */
    {
        char line[80];
        memset(line, 0, sizeof(line));
        memcpy(line, "CFSR=", 5);
        hex32(line + 5, g_crash_dump.cfsr);
        memcpy(line + 13, "  HFSR=", 7);
        hex32(line + 20, g_crash_dump.hfsr);
        memcpy(line + 28, "  BFAR=", 7);
        hex32(line + 35, g_crash_dump.bfar);
        bsod_text(2, 12, line);
    }

    /* R0  R1  R2  R3 */
    {
        char line[80];
        memset(line, 0, sizeof(line));
        memcpy(line, "R0=", 3);
        hex32(line + 3, g_crash_dump.r0);
        memcpy(line + 11, "  R1=", 5);
        hex32(line + 16, g_crash_dump.r1);
        memcpy(line + 24, "  R2=", 5);
        hex32(line + 29, g_crash_dump.r2);
        memcpy(line + 37, "  R3=", 5);
        hex32(line + 42, g_crash_dump.r3);
        bsod_text(2, 13, line);
    }

    display_swap_buffers();

    // Reboot via watchdog in ~10 seconds so the user can read the BSOD.
    watchdog_enable(10000, false);
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


/*==========================================================================
 * Compositor task — runs on Core 0 under FreeRTOS.
 *
 * DispHSTX manages DVI output on Core 1 internally.  The compositor
 * runs here, dispatching window events and recompositing the scene
 * when input marks the display dirty.
 *=========================================================================*/
static void compositor_task(void *params) {
    (void)params;

    /* Show hourglass for 1 second, then hide cursor until first mouse move */
    TickType_t boot_deadline = xTaskGetTickCount() + pdMS_TO_TICKS(1000);
    bool boot_cursor_active = true;

    for (;;) {
        /* In fullscreen 8bpp mode, the app owns the display.
         * Skip compositor (no windows, no cursor, no recomposite).
         * Short sleep so we resume quickly when mode is restored.
         * Force full repaint on the first frame back in desktop mode. */
        if (display_video_mode == VIDEO_MODE_320x240x256) {
            display_compositor_idle = 1;
            do { vTaskDelay(pdMS_TO_TICKS(10)); }
            while (display_video_mode == VIDEO_MODE_320x240x256);
            display_compositor_idle = 0;
            wm_force_full_repaint();
            g_video_dirty = true;
            continue;
        }

        /* End boot sequence: show taskbar and hide cursor */
        if (boot_cursor_active && xTaskGetTickCount() >= boot_deadline) {
            boot_cursor_active = false;
            swap_init();
            startmenu_init();
            file_assoc_scan();
            desktop_init();
            taskbar_init();
            cursor_set_type(CURSOR_ARROW);
            cursor_overlay_erase();
            boot_cursor_hidden = true;
            wm_force_full_repaint();
            /* Focus desktop if shortcuts exist and no windows are open */
            if (desktop_has_shortcuts()) desktop_focus();
        }

        /* Process deferred swap resumes — an exiting app sets a flag,
         * and we do the actual stack restore here (on our own stack). */
        swap_process_deferred();

        /* Check for deferred start menu actions (dialog confirmations) */
        startmenu_check_pending();
        run_dialog_check_pending();
        app_launch_check_pending();

        /* Deferred spawns from input_task hotkeys — handled here on the
         * compositor task so WM calls are always on the right task/stack.
         * If the focused window is fullscreen, exit it first so the new
         * window/dialog appears over a normal desktop. */
        if (g_spawn_terminal_pending || g_spawn_navigator_pending ||
            g_open_run_dialog_pending || g_spawn_control_panel_pending ||
            g_spawn_network_settings_pending) {
            hwnd_t fs_win = wm_get_focus();
            if (fs_win != HWND_NULL && wm_is_fullscreen(fs_win)) {
                wm_toggle_fullscreen(fs_win);
                g_video_dirty = true;
            }
        }
        if (g_spawn_terminal_pending) {
            g_spawn_terminal_pending = false;
            spawn_terminal_window();
        }
        if (g_spawn_navigator_pending) {
            g_spawn_navigator_pending = false;
            spawn_filemanager_window();
        }
        if (g_open_run_dialog_pending) {
            g_open_run_dialog_pending = false;
            run_dialog_open();
        }
        if (g_spawn_control_panel_pending) {
            g_spawn_control_panel_pending = false;
            wm_set_pending_icon(cp_get_icon16());
            wm_set_pending_icon32(cp_get_icon32());
            control_panel_create();
        }
        if (g_spawn_network_settings_pending) {
            g_spawn_network_settings_pending = false;
            extern const uint8_t *net_icon16_connect_get(void);
            extern const uint8_t *net_icon32_connect_get(void);
            wm_set_pending_icon(net_icon16_connect_get());
            wm_set_pending_icon32(net_icon32_connect_get());
            network_settings_create();
        }

        /* Update clock in taskbar every minute (even without input) */
        taskbar_tick();

        /* Recomposite when input arrives OR when windows are
         * invalidated (e.g. terminal output, cursor blink).
         * Always drain both flags to avoid a stale-flag repeat. */
        {
            bool input   = g_video_dirty;
            bool content = wm_needs_composite();
            if (input || content) {
                g_video_dirty = false;

                wm_dispatch_events();
                wm_composite();
            }
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
    if (!shell_start(t)) {
        printf("spawn_terminal_window: shell_start failed\n");
        terminal_destroy(t);
        return;
    }
    wm_set_focus(hwnd);
    taskbar_invalidate();
}

/*==========================================================================
 * Spawn a new Control Panel window
 *=========================================================================*/
void spawn_control_panel(void) {
    g_spawn_control_panel_pending = true;
    g_video_dirty = true;
}

void spawn_network_settings(void) {
    g_spawn_network_settings_pending = true;
    g_video_dirty = true;
}

static void input_task(void *params) {
    (void)params;

    /* Absolute cursor position — start at screen center */
    int16_t cur_x = display_width / 2;
    int16_t cur_y = display_height / 2;
    uint8_t prev_buttons = 0;

    for (;;) {
        /* In fullscreen 8bpp mode, the app owns the display and keyboard.
         * Skip all input processing so keyboard_poll/get_event are available
         * exclusively to the fullscreen app, and mouse cursor is suppressed. */
        if (display_video_mode == VIDEO_MODE_320x240x256) {
            do { vTaskDelay(pdMS_TO_TICKS(10)); }
            while (display_video_mode == VIDEO_MODE_320x240x256);
            continue;
        }

        /* Poll keyboard */
        keyboard_poll();

        key_event_t kev;
        /* Track Win key: only toggle start menu if released with no combo */
        static bool win_combo_used = false;

        while (keyboard_get_event(&kev)) {
            /* Intercept Win+T: spawn a new terminal window (deferred).
             * Heavy WM/alloc calls run on compositor task, not here. */
            if (kev.pressed &&
                (kev.modifiers & (KBD_MOD_LGUI | KBD_MOD_RGUI)) &&
                kev.hid_code == 0x17 /* HID_KEY_T */) {
                win_combo_used = true;
                g_spawn_terminal_pending = true;
                g_video_dirty = true;
                continue;
            }

            /* Win+E: open Navigator (deferred) */
            if (kev.pressed &&
                (kev.modifiers & (KBD_MOD_LGUI | KBD_MOD_RGUI)) &&
                kev.hid_code == 0x08 /* HID_KEY_E */) {
                win_combo_used = true;
                g_spawn_navigator_pending = true;
                g_video_dirty = true;
                continue;
            }

            /* Win+R: open Run dialog (deferred) */
            if (kev.pressed &&
                (kev.modifiers & (KBD_MOD_LGUI | KBD_MOD_RGUI)) &&
                kev.hid_code == 0x15 /* HID_KEY_R */) {
                win_combo_used = true;
                g_open_run_dialog_pending = true;
                g_video_dirty = true;
                continue;
            }

            /* Win key pressed: just arm the start menu, don't fire yet.
             * HID GUI key codes: LGUI=0xE3, RGUI=0xE7 */
            if (kev.pressed &&
                (kev.hid_code == 0xE3 || kev.hid_code == 0xE7)) {
                win_combo_used = false;    /* reset — fresh press */
                continue;
            }

            /* Win key released: toggle start menu only if no combo was used */
            if (!kev.pressed &&
                (kev.hid_code == 0xE3 || kev.hid_code == 0xE7)) {
                if (!win_combo_used) {
                    startmenu_toggle();
                    g_video_dirty = true;
                }
                win_combo_used = false;
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
                if (taskbar_popup_is_open() &&
                    taskbar_popup_handle_key(kev.hid_code, kev.modifiers)) {
                    g_video_dirty = true;
                    continue;
                }
                if (net_popup_is_open() &&
                    net_popup_handle_key(kev.hid_code, kev.modifiers)) {
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

            /* Keyboard move mode: arrow keys move window, Enter/Esc exits.
             * Only handle key-down events so the Enter key-up from the
             * sysmenu "Move" selection doesn't immediately deactivate. */
            if (kev.pressed && wm_is_keyboard_move_active()) {
                if (wm_keyboard_move_key(kev.hid_code)) {
                    g_video_dirty = true;
                    continue;
                }
            }

            /* Alt+Tab overlay: open on first press, cycle on repeat.
             * If the focused window is fullscreen, exit it first. */
            if (kev.pressed && (kev.modifiers & KBD_MOD_ALT) &&
                kev.hid_code == 0x2B /* HID_KEY_TAB */) {
                hwnd_t fs_focus = wm_get_focus();
                if (fs_focus != HWND_NULL && wm_is_fullscreen(fs_focus))
                    wm_toggle_fullscreen(fs_focus);
                if (!alttab_is_active())
                    alttab_open();
                else
                    alttab_cycle();
                g_video_dirty = true;
                continue;
            }

            /* Alt release while overlay active → commit selection */
            if (!kev.pressed &&
                (kev.hid_code == 0xE2 || kev.hid_code == 0xE6) &&
                alttab_is_active()) {
                alttab_commit();
                g_video_dirty = true;
                continue;
            }

            /* Escape while overlay active → cancel */
            if (kev.pressed && kev.hid_code == 0x29 /* HID_KEY_ESCAPE */ &&
                alttab_is_active()) {
                alttab_cancel();
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

                /* Route to desktop keyboard handler when no window has focus */
                if (wm_get_focus() == HWND_NULL && desktop_has_focus()) {
                    desktop_key(we.key.scancode, we.key.modifiers);
                } else {
                    wm_post_event_focused(&we);
                }

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
            /* First mouse movement after boot: show the arrow cursor */
            if (boot_cursor_hidden) {
                boot_cursor_hidden = false;
            }

            /* Convert deltas to absolute — PS/2 Y is inverted */
            cur_x += dx;
            cur_y -= dy;

            /* Clamp to screen bounds */
            if (cur_x < 0) cur_x = 0;
            if (cur_x >= display_width) cur_x = display_width - 1;
            if (cur_y < 0) cur_y = 0;
            if (cur_y >= display_height) cur_y = display_height - 1;

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

            /* Always dispatch mouse events so apps can track hover.
             * The compositor handles selective repainting efficiently. */
            g_video_dirty = true;
            if (!changed && !buttons) {
                /* Passive move — also update cursor overlay directly
                 * for low-latency cursor rendering */
                if (!boot_cursor_hidden)
                    cursor_overlay_move(cur_x, cur_y);
            }
        }

        vTaskDelay(pdMS_TO_TICKS(8));
    }
}

/*---------------------------------------------------------------------------
 * Flash QMI timing — must be called BEFORE changing the system clock.
 * Calculates divisor and RX delay from the target CPU speed and
 * FLASH_MAX_FREQ_MHZ (set via CMake -DFLASH_SPEED=xx, default 66).
 *-------------------------------------------------------------------------*/
#ifndef FLASH_MAX_FREQ_MHZ
#define FLASH_MAX_FREQ_MHZ 66
#endif

static void __no_inline_not_in_flash_func(set_flash_timings)(int cpu_mhz) {
    const int clock_hz = cpu_mhz * 1000000;
    const int max_flash_freq = FLASH_MAX_FREQ_MHZ * 1000000;

    int divisor = (clock_hz + max_flash_freq - (max_flash_freq >> 4) - 1) / max_flash_freq;
    if (divisor == 1 && clock_hz >= 166000000) {
        divisor = 2;
    }

    int rxdelay = divisor;
    if (clock_hz / divisor > 100000000 && clock_hz >= 166000000) {
        rxdelay += 1;
    }

    qmi_hw->m[0].timing = 0x60007000 |
                        rxdelay << QMI_M0_TIMING_RXDELAY_LSB |
                        divisor << QMI_M0_TIMING_CLKDIV_LSB;
}

int main(void) {
#if CPU_CLOCK_MHZ > 252
    vreg_disable_voltage_limit();
    vreg_set_voltage(CPU_VOLTAGE);
    set_flash_timings(CPU_CLOCK_MHZ);
    sleep_ms(100);
#endif

    if (!set_sys_clock_khz(CPU_CLOCK_MHZ * 1000, false)) {
        set_sys_clock_khz(252 * 1000, true);
    }

    /* ATRANS3: With 16MB flash, 0x10FFF000 maps directly to flash 0xFFF000.
     * Set identity mapping for the top 4MB window (0x10C00000–0x10FFFFFF
     * → flash 0xC00000–0xFFFFFF) so the sys_table at 0x10FFF000 works. */
    qmi_hw->atrans[3] = (0x400u << QMI_ATRANS3_SIZE_LSB)
                       | (0xC00u << QMI_ATRANS3_BASE_LSB);

    /* Clear SRAM UI-force magic left over from Space-escape boot */
    *SRAM_UIFORCE_ADDR = 0;

    /* Fix RAM vector table for Space-escape boot scenario.
     * After firmware flash, sector 0 at XIP offset 0 contains the firmware's
     * vector table (with only word[1] patched to FRANK OS Reset).  The crt0
     * runtime_init copies those firmware vectors into the RAM vector table.
     * On normal boot the vectors at offset 0 are FRANK OS's own, so the
     * copy is correct.  After firmware flash + Space escape, they're the
     * firmware's vectors, so FreeRTOS exception handlers (SVCall, PendSV,
     * SysTick) point to firmware code — causing configASSERT failure in
     * xPortStartScheduler and a hang.
     *
     * Fix by writing the correct handler addresses unconditionally.
     * On normal boot this is a harmless no-op (same values). */
    {
        extern uint32_t ram_vector_table[];
        extern void isr_svcall(void);
        extern void isr_pendsv(void);
        extern void isr_systick(void);
        /* isr_hardfault is defined in this file (below) */
        ram_vector_table[3]  = (uint32_t)(uintptr_t)isr_hardfault;
        ram_vector_table[11] = (uint32_t)(uintptr_t)isr_svcall;
        ram_vector_table[14] = (uint32_t)(uintptr_t)isr_pendsv;
        ram_vector_table[15] = (uint32_t)(uintptr_t)isr_systick;
    }

    stdio_init_all();

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
        bool mouse_ok = false;
        for (int attempt = 0; attempt < 5 && !mouse_ok; attempt++) {
            if (attempt) {
                printf("PS/2 mouse init retry %d...\n", attempt);
                sleep_ms(100);
            }
            mouse_ok = ps2_mouse_init_device();
        }
        if (mouse_ok) {
            printf("PS/2 mouse initialized (wheel=%d)\n", ps2_mouse_has_wheel());
        } else {
            printf("PS/2 mouse device init failed after 5 attempts\n");
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

    /* Load persistent settings from /fos/settings.dat (safe if no SD) */
    settings_load();

    /* Initialize sound mixer (starts I2S at 44100 Hz, DMA plays silence) */
    snd_init();

    /* Apply saved volume from settings */
    snd_set_volume(settings_get()->volume);

    /* Initialize PIO UART for ESP-01 netcard (uses PIO1 SM1+SM2).
     * MUST be after snd_init() because audio resets PIO1 entirely. */
    printf("Initializing PIO UART for netcard...\n"); stdio_flush();
    serial_init();

    /* Load saved WiFi config from /fos/wifi.dat */
    wifi_config_load();

    /* Spawn netcard task — probes modem, auto-reconnects if config exists */
    netcard_init_async();

    /* Install multicore lockout handler on Core 1 so flash_block() can
     * safely pause Core 1 during flash erase/program operations.
     * Done after SD mount to avoid SIO FIFO IRQ interfering with SPI init. */
    DispHstxCore1Exec(multicore_lockout_victim_init);
    DispHstxCore1Wait();

    /* Play startup sound (spawns a one-shot FreeRTOS task) */
    startup_sound_start();

    /* Show hourglass cursor during startup — taskbar appears after delay */
    cursor_set_type(CURSOR_WAIT);

    /* One-shot composite: desktop + hourglass, no taskbar yet */
    wm_composite();

    xTaskCreate(usb_service_task, "usb", 256, NULL, configMAX_PRIORITIES - 1, NULL);
    xTaskCreate(input_task, "input", 1024, NULL, 3, NULL);
    xTaskCreate(compositor_task, "compositor", 4096, NULL, 2, NULL);

    // xTaskCreate leaves BASEPRI raised (ulCriticalNesting = 0xaaaaaaaa by design).
    // Clear it so printf works before the scheduler starts.
    __asm volatile ("msr basepri, %0" :: "r"(0));

    printf("Starting scheduler...\n"); stdio_flush();

    vTaskStartScheduler();
    for (;;) { tight_loop_contents(); }
}
