/*
 * main.c — FRANK OS windowed app entry point for pshell
 *
 * Creates a terminal window, starts the pshell interpreter as a FreeRTOS
 * task, and routes keyboard events through a VT100 terminal emulator.
 *
 * Copyright (c) 2026 Mikhail Matveev <xtreme@rh1.tech>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "m-os-api.h"
#undef switch
#include "frankos-app.h"
#include "pshell_vt100.h"

#include <string.h>
#include <stdbool.h>

/* ── Menu command IDs ─────────────────────────────────────────────────── */
#define CMD_EXIT    100
#define CMD_ABOUT   200

/* ── App state ────────────────────────────────────────────────────────── */
static hwnd_t         g_hwnd     = 0;
static TaskHandle_t   g_main_task = NULL;
static TaskHandle_t   g_shell_task = NULL;
static volatile bool  g_closing  = false;

/* Forward declaration of the pshell entry point (in shell.c) */
extern int pshell_main(void);

/* ═════════════════════════════════════════════════════════════════════════
 * Menu setup
 * ═════════════════════════════════════════════════════════════════════════ */

static void setup_menu(hwnd_t hwnd) {
    menu_bar_t bar;
    memset(&bar, 0, sizeof(bar));
    bar.menu_count = 2;

    /* File menu */
    menu_def_t *file = &bar.menus[0];
    strncpy(file->title, "File", sizeof(file->title) - 1);
    file->accel_key = 0x09; /* Alt+F */
    file->item_count = 1;
    strncpy(file->items[0].text, "Exit", sizeof(file->items[0].text) - 1);
    file->items[0].command_id = CMD_EXIT;

    /* Help menu */
    menu_def_t *help = &bar.menus[1];
    strncpy(help->title, "Help", sizeof(help->title) - 1);
    help->accel_key = 0x0B; /* Alt+H */
    help->item_count = 1;
    strncpy(help->items[0].text, "About", sizeof(help->items[0].text) - 1);
    help->items[0].command_id = CMD_ABOUT;

    menu_set(hwnd, &bar);
}

/* ═════════════════════════════════════════════════════════════════════════
 * Event handler
 * ═════════════════════════════════════════════════════════════════════════ */

static bool pshell_event(hwnd_t hwnd, const window_event_t *event) {
    (void)hwnd;

    if (event->type == WM_CLOSE) {
        if (wm_is_fullscreen(hwnd))
            wm_toggle_fullscreen(hwnd);
        g_closing = true;
        /* Push Ctrl+C to wake the shell, then a quit signal */
        vt100_input_push(3);  /* Ctrl+C */
        if (g_main_task)
            xTaskNotifyGive(g_main_task);
        return true;
    }

    if (event->type == WM_COMMAND) {
        if (event->command.id == CMD_EXIT) {
            g_closing = true;
            vt100_input_push(3);
            if (g_main_task)
                xTaskNotifyGive(g_main_task);
            return true;
        }
        if (event->command.id == CMD_ABOUT) {
            dialog_show(hwnd, "About PShell",
                        "PShell - Pico Shell for FRANK OS\n\n"
                        "Interactive shell, vi editor, C compiler\n"
                        "Based on pshell by Thomas Edison\n\n"
                        "(c) 2026 Mikhail Matveev",
                        DLG_ICON_INFO, DLG_BTN_OK);
            return true;
        }
        return false;
    }

    if (event->type == WM_SIZE) {
        /* Compute new terminal dimensions from client area */
        int16_t w = event->size.w;
        int16_t h = event->size.h;
        int new_cols = w / VT100_FONT_W;
        int new_rows = h / VT100_FONT_H;
        if (new_cols < 20) new_cols = 20;
        if (new_rows < 5) new_rows = 5;
        if (new_cols > VT100_MAX_COLS) new_cols = VT100_MAX_COLS;
        if (new_rows > VT100_MAX_ROWS) new_rows = VT100_MAX_ROWS;
        vt100_resize(new_cols, new_rows);
        wm_invalidate(g_hwnd);
        return true;
    }

    if (event->type == WM_SETFOCUS) {
        vt100_invalidate();
        wm_invalidate(hwnd);
        return true;
    }

    /* WM_CHAR: printable ASCII characters */
    if (event->type == WM_CHAR) {
        char ch = event->charev.ch;
        if (ch != '\0')
            vt100_input_push((unsigned char)ch);
        return true;
    }

    /* WM_KEYDOWN: navigation keys, control keys, function keys */
    if (event->type == WM_KEYDOWN) {
        uint8_t sc  = event->key.scancode;
        uint8_t mod = event->key.modifiers;

        /* Ctrl+C via raw scan: HID 'c' = 0x06 */
        if ((mod & KMOD_CTRL) && sc == 0x06) {
            vt100_input_push(3);
            return true;
        }

        /* Alt+Enter → toggle fullscreen */
        if (sc == 0x28 && (mod & KMOD_ALT)) {
            wm_toggle_fullscreen(hwnd);
            return true;
        }

        /* Keys that don't generate WM_CHAR */
        if (sc == 0x28) { vt100_input_push('\r');  return true; } /* Enter */
        if (sc == 0x58) { vt100_input_push('\r');  return true; } /* KP Enter */
        if (sc == 0x2A) { vt100_input_push('\b');  return true; } /* Backspace */
        if (sc == 0x2B) { vt100_input_push('\t');  return true; } /* Tab */
        if (sc == 0x29) { vt100_input_push(0x1B);  return true; } /* Esc */

        /* Arrow keys → VT100 sequences */
        if (sc == 0x52) { /* Up */
            if (mod & KMOD_CTRL) vt100_input_push_str("\033[1;5A");
            else                 vt100_input_push_str("\033[A");
            return true;
        }
        if (sc == 0x51) { /* Down */
            if (mod & KMOD_CTRL) vt100_input_push_str("\033[1;5B");
            else                 vt100_input_push_str("\033[B");
            return true;
        }
        if (sc == 0x4F) { /* Right */
            if (mod & KMOD_CTRL) vt100_input_push_str("\033[1;5C");
            else                 vt100_input_push_str("\033[C");
            return true;
        }
        if (sc == 0x50) { /* Left */
            if (mod & KMOD_CTRL) vt100_input_push_str("\033[1;5D");
            else                 vt100_input_push_str("\033[D");
            return true;
        }

        /* Navigation keys → VT100 sequences */
        if (sc == 0x4A) { vt100_input_push_str("\033[H");   return true; } /* Home */
        if (sc == 0x4D) { vt100_input_push_str("\033[F");   return true; } /* End */
        if (sc == 0x4B) { vt100_input_push_str("\033[5~");  return true; } /* Page Up */
        if (sc == 0x4E) { vt100_input_push_str("\033[6~");  return true; } /* Page Down */
        if (sc == 0x49) { vt100_input_push_str("\033[2~");  return true; } /* Insert */
        if (sc == 0x4C) { vt100_input_push_str("\033[3~");  return true; } /* Delete */

        return false;
    }

    return false;
}

/* ═════════════════════════════════════════════════════════════════════════
 * Paint callback — delegates to VT100 terminal renderer
 * ═════════════════════════════════════════════════════════════════════════ */

static void pshell_paint(hwnd_t hwnd) {
    vt100_paint(hwnd);
}

/* ═════════════════════════════════════════════════════════════════════════
 * Cursor blink timer (500 ms)
 * ═════════════════════════════════════════════════════════════════════════ */

static void blink_cb(TimerHandle_t t) {
    (void)t;
    vt100_toggle_cursor();
}

/* ═════════════════════════════════════════════════════════════════════════
 * Shell worker task — runs pshell_main() in its own FreeRTOS task
 * ═════════════════════════════════════════════════════════════════════════ */

static void shell_task_fn(void *param) {
    (void)param;

    /* Register this task as the input waiter so vt100_getch() works */
    vt100_set_waiter(xTaskGetCurrentTaskHandle());

    pshell_main();

    /* Shell exited — signal close */
    g_closing = true;
    if (g_main_task)
        xTaskNotifyGive(g_main_task);
    vTaskDelete(NULL);
}

/* ═════════════════════════════════════════════════════════════════════════
 * FRANK OS app entry point
 * ═════════════════════════════════════════════════════════════════════════ */

int main(int argc, char **argv) {
    (void)argc; (void)argv;

    /* Singleton: if already running, focus existing window */
    hwnd_t existing = wm_find_window_by_title("PShell");
    if (existing != 0) {
        wm_set_focus(existing);
        return 0;
    }

    g_main_task = xTaskGetCurrentTaskHandle();
    g_closing = false;

    /* Initialise VT100 terminal (70×20 default) */
    vt100_init(VT100_DEFAULT_COLS, VT100_DEFAULT_ROWS);

    /* Compute window dimensions */
    int16_t client_w = VT100_DEFAULT_COLS * VT100_FONT_W;    /* 560 */
    int16_t client_h = VT100_DEFAULT_ROWS * VT100_FONT_H;    /* 320 */
    int16_t win_w = client_w + 2 * THEME_BORDER_WIDTH;
    int16_t win_h = client_h + THEME_TITLE_HEIGHT + THEME_MENU_HEIGHT
                  + 2 * THEME_BORDER_WIDTH;

    /* Centre window */
    int16_t x = (DISPLAY_WIDTH - win_w) / 2;
    int16_t y = (DISPLAY_HEIGHT - TASKBAR_HEIGHT - win_h) / 2;
    if (y < 0) y = 0;

    g_hwnd = wm_create_window(x, y, win_w, win_h, "PShell",
                              WSTYLE_DEFAULT | WF_MENUBAR,
                              pshell_event, pshell_paint);
    if (g_hwnd == 0) {
        vt100_destroy();
        return 1;
    }

    vt100_set_hwnd(g_hwnd);
    setup_menu(g_hwnd);

    window_t *win = wm_get_window(g_hwnd);
    if (win) win->bg_color = COLOR_BLACK;

    wm_show_window(g_hwnd);
    wm_set_focus(g_hwnd);
    taskbar_invalidate();

    /* Start cursor blink timer (500 ms, auto-reload) */
    TimerHandle_t blink_tmr = xTimerCreate("pshBlink",
                                            pdMS_TO_TICKS(500),
                                            pdTRUE, NULL, blink_cb);
    if (blink_tmr)
        xTimerStart(blink_tmr, 0);

    /* Start shell in a separate task (4K words = 16KB stack).
     * Keep this lean — FreeRTOS heap is only 128KB and cc needs ~82KB. */
    xTaskCreate(shell_task_fn, "pshell", 4096, NULL,
                tskIDLE_PRIORITY + 1, &g_shell_task);

    /* Main loop — block until close */
    while (!g_closing)
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

    /* Give the shell task a moment to notice g_closing */
    vTaskDelay(pdMS_TO_TICKS(100));

    /* Tear down */
    if (blink_tmr) {
        xTimerStop(blink_tmr, 0);
        xTimerDelete(blink_tmr, 0);
    }

    /* Delete shell task if still running */
    if (g_shell_task) {
        vTaskDelete(g_shell_task);
        g_shell_task = NULL;
    }

    wm_destroy_window(g_hwnd);
    g_hwnd = 0;
    taskbar_invalidate();

    vt100_destroy();

    return 0;
}

uint32_t __app_flags(void) { return APPFLAG_SINGLETON; }
