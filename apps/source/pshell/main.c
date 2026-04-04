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
#include "cc/cc.h"

#include <string.h>
#include <stdbool.h>

/* ── Menu command IDs ─────────────────────────────────────────────────── */
#define CMD_EXIT    100
#define CMD_ABOUT   200

/* ── App state ────────────────────────────────────────────────────────── */
static hwnd_t         g_hwnd      = 0;
static TaskHandle_t   g_main_task  = NULL;
static TaskHandle_t   g_shell_task = NULL;
static volatile bool  g_closing    = false;
static TimerHandle_t  g_blink_tmr  = NULL;

/* Executable mode: when non-NULL, pshell runs this file and exits */
static const char    *g_exec_file  = NULL;

/* Forward declaration of the pshell entry point (in shell.c) */
extern int pshell_main(void);

/* Forward declaration: exec mode — run a cc binary and return (in shell.c) */
extern int pshell_exec(const char *file);

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
        vt100_set_active(true);                   /* re-enable FB writes */
        /* Resume the shell task so it can process I/O again */
        if (g_shell_task) vTaskResume(g_shell_task);
        if (g_blink_tmr) xTimerStart(g_blink_tmr, 0);
        wm_invalidate(hwnd);                      /* trigger full repaint */
        return true;
    }

    if (event->type == WM_KILLFOCUS) {
        if (g_blink_tmr) xTimerStop(g_blink_tmr, 0);
        vt100_set_active(false);                  /* suppress FB writes */
        /* Suspend the shell task so it cannot write to the framebuffer
         * while another window is in the foreground. */
        if (g_shell_task) vTaskSuspend(g_shell_task);
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

    if (g_exec_file)
        pshell_exec(g_exec_file);
    else
        pshell_main();

    /* Shell exited — clear handle BEFORE self-delete so the main task's
     * cleanup path sees NULL and skips the force-delete (double vTaskDelete
     * on an already-deleted TCB is undefined behaviour in FreeRTOS). */
    g_shell_task = NULL;
    g_closing = true;
    if (g_main_task)
        xTaskNotifyGive(g_main_task);
    vTaskDelete(NULL);
}

/* ═════════════════════════════════════════════════════════════════════════
 * FRANK OS app entry point
 * ═════════════════════════════════════════════════════════════════════════ */

int main(int argc, char **argv) {
    bool exec_mode = (argc > 1 && argv[1] != NULL);

    /* Singleton: if already running in shell mode, focus existing window */
    if (!exec_mode) {
        hwnd_t existing = wm_find_window_by_title("PShell");
        if (existing != 0) {
            wm_set_focus(existing);
            return 0;
        }
    }

    g_main_task = xTaskGetCurrentTaskHandle();
    g_closing = false;
    g_exec_file = exec_mode ? argv[1] : NULL;

    /* Initialise VT100 terminal (70×20 default) */
    vt100_init(VT100_DEFAULT_COLS, VT100_DEFAULT_ROWS);

    /* Compute window dimensions */
    int16_t client_w = VT100_DEFAULT_COLS * VT100_FONT_W;    /* 560 */
    int16_t client_h = VT100_DEFAULT_ROWS * VT100_FONT_H;    /* 320 */
    int16_t win_w = client_w + 2 * THEME_BORDER_WIDTH;
    int16_t win_h;
    uint16_t style;

    if (exec_mode) {
        /* No menu bar in exec mode */
        win_h = client_h + THEME_TITLE_HEIGHT + 2 * THEME_BORDER_WIDTH;
        style = WSTYLE_DEFAULT | WF_FULLSCREENABLE;
    } else {
        win_h = client_h + THEME_TITLE_HEIGHT + THEME_MENU_HEIGHT
              + 2 * THEME_BORDER_WIDTH;
        style = WSTYLE_DEFAULT | WF_MENUBAR | WF_FULLSCREENABLE;
    }

    /* Build window title — use filename for exec mode */
    const char *title = "PShell";
    char exec_title[64];
    if (exec_mode) {
        const char *slash = strrchr(argv[1], '/');
        const char *base = slash ? slash + 1 : argv[1];
        snprintf(exec_title, sizeof(exec_title), "%.60s", base);
        title = exec_title;
    }

    /* Centre window */
    int16_t x = (DISPLAY_WIDTH - win_w) / 2;
    int16_t y = (DISPLAY_HEIGHT - TASKBAR_HEIGHT - win_h) / 2;
    if (y < 0) y = 0;

    g_hwnd = wm_create_window(x, y, win_w, win_h, title,
                              style,
                              pshell_event, pshell_paint);
    if (g_hwnd == 0) {
        vt100_destroy();
        return 1;
    }

    vt100_set_hwnd(g_hwnd);
    if (!exec_mode)
        setup_menu(g_hwnd);

    window_t *win = wm_get_window(g_hwnd);
    if (win) win->bg_color = COLOR_BLACK;

    wm_show_window(g_hwnd);
    wm_set_focus(g_hwnd);
    taskbar_invalidate();

    /* Start cursor blink timer (500 ms, auto-reload) */
    g_blink_tmr = xTimerCreate("pshBlink",
                                pdMS_TO_TICKS(500),
                                pdTRUE, NULL, blink_cb);
    if (g_blink_tmr)
        xTimerStart(g_blink_tmr, 0);

    /* Run shell directly on the main task (shared stack) instead of
     * creating a separate FreeRTOS task.  xTaskCreate would need to
     * allocate the stack from the FreeRTOS SRAM heap which has very
     * limited free space — this avoids the allocation entirely.
     * The swap system saves/restores the shared stack to PSRAM on
     * app switch, so suspend/resume works correctly. */
    g_shell_task = g_main_task;
    vt100_set_waiter(g_shell_task);

    if (g_exec_file)
        pshell_exec(g_exec_file);
    else
        pshell_main();

    g_shell_task = NULL;

    /* Tear down — use portMAX_DELAY so timer stop/delete fully complete
     * before we return (the callback is a function pointer into ELF code;
     * a pending timer fire after ELF unload causes a jump into freed memory). */
    if (g_blink_tmr) {
        xTimerStop(g_blink_tmr, portMAX_DELAY);
        xTimerDelete(g_blink_tmr, portMAX_DELAY);
        g_blink_tmr = NULL;
    }

    /* Free cc's persistent code/data buffer before ELF unload */
    cc_cleanup();

    wm_destroy_window(g_hwnd);
    g_hwnd = 0;
    taskbar_invalidate();

    vt100_destroy();

    return 0;
}

uint32_t __app_flags(void) { return APPFLAG_SINGLETON; }
