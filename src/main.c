#include <stdio.h>
#include <string.h>
#include "pico/stdlib.h"
#include "pico/multicore.h"
#include "hardware/clocks.h"
#include "hardware/gpio.h"
#include "tusb.h"
#include "FreeRTOS.h"
#include "task.h"
#include "board_config.h"
#include "display.h"
#include "window.h"
#include "window_event.h"
#include "ps2.h"
#include "keyboard.h"

#define LED_PIN PICO_DEFAULT_LED_PIN

/* Dirty flag — set by input_task, read by compositor_task (both on Core 0) */
static volatile bool g_video_dirty = true;

static inline void led_init(void) {
    gpio_init(LED_PIN);
    gpio_set_dir(LED_PIN, GPIO_OUT);
}

// Hard fault handler — double-blink pattern using raw SIO (no SDK calls).
// Named isr_hardfault to override the weak symbol in Pico SDK's vector table.
void __attribute__((used)) isr_hardfault(void) {
    *(volatile unsigned int *)0xd0000038 = (1u << 25);
    for (;;) {
        for (int b = 0; b < 2; b++) {
            *(volatile unsigned int *)0xd0000018 = (1u << 25);
            for (volatile int i = 0; i < 10000000; i++);
            *(volatile unsigned int *)0xd0000020 = (1u << 25);
            for (volatile int i = 0; i < 10000000; i++);
        }
        for (volatile int i = 0; i < 60000000; i++);
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
    int count = 0;
    for (;;) {
        gpio_put(LED_PIN, 1);
        printf("tick %d\n", count++);
        stdio_flush();
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
        if (g_video_dirty) {
            g_video_dirty = false;

            wm_dispatch_events();
            wm_composite();

            display_wait_vsync();
        }
        vTaskDelay(1);
    }
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
            printf("KEY %s: ascii=0x%02X hid=0x%02X\n",
                   kev.pressed ? "DN" : "UP", kev.ascii, kev.hid_code);
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

            /* Post mouse move event */
            window_event_t ev;
            memset(&ev, 0, sizeof(ev));
            ev.type = WM_MOUSEMOVE;
            ev.mouse.x = cur_x;
            ev.mouse.y = cur_y;
            ev.mouse.buttons = buttons;
            wm_post_event_focused(&ev);

            /* Detect button transitions */
            uint8_t changed = buttons ^ prev_buttons;
            if (changed & 0x01) { /* left button */
                memset(&ev, 0, sizeof(ev));
                ev.type = (buttons & 0x01) ? WM_LBUTTONDOWN : WM_LBUTTONUP;
                ev.mouse.x = cur_x;
                ev.mouse.y = cur_y;
                ev.mouse.buttons = buttons;
                wm_post_event_focused(&ev);
            }
            if (changed & 0x02) { /* right button */
                memset(&ev, 0, sizeof(ev));
                ev.type = (buttons & 0x02) ? WM_RBUTTONDOWN : WM_RBUTTONUP;
                ev.mouse.x = cur_x;
                ev.mouse.y = cur_y;
                ev.mouse.buttons = buttons;
                wm_post_event_focused(&ev);
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

    stdio_init_all();
    for (int i = 0; i < 8; i++) { sleep_ms(500); }

    led_init();

    printf("\n== Rhea OS ==\n");
    printf("CPU: %lu MHz\n", (unsigned long)(clock_get_hz(clk_sys) / 1000000));
    printf("Scheduler: FreeRTOS\n");
    printf("Display: DispHSTX DVI\n");
    stdio_flush();

    printf("Initializing display...\n"); stdio_flush();
    display_init();
    printf("Display initialized\n"); stdio_flush();

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

    hwnd_t test_win = wm_create_window(100, 80, 300, 200,
                                        "Test Window", WSTYLE_DEFAULT,
                                        NULL, NULL);
    wm_set_focus(test_win);
    printf("Test window created (hwnd=%d)\n", test_win); stdio_flush();

    /* One-shot composite so the first frame is visible */
    wm_composite();

    xTaskCreate(usb_service_task, "usb", 256, NULL, configMAX_PRIORITIES - 1, NULL);
    xTaskCreate(heartbeat_task, "heartbeat", 512, NULL, 1, NULL);
    xTaskCreate(input_task, "input", 512, NULL, 3, NULL);
    xTaskCreate(compositor_task, "compositor", 1024, NULL, 2, NULL);

    // xTaskCreate leaves BASEPRI raised (ulCriticalNesting = 0xaaaaaaaa by design).
    // Clear it so printf works before the scheduler starts.
    __asm volatile ("msr basepri, %0" :: "r"(0));

    printf("Starting scheduler...\n"); stdio_flush();

    vTaskStartScheduler();
    for (;;) { tight_loop_contents(); }
}
