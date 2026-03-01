/*
 * FRANK OS — Notepad (standalone ELF app)
 * Copyright (c) 2025 Mikhail Matveev <xtreme@rh1.tech>
 * https://rh1.tech
 *
 * Full-featured text editor with menu bar, file I/O, clipboard,
 * find/replace, and save-changes prompts.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "m-os-api.h"
#include "frankos-app.h"

/* UART debug printf */
#define dbg_printf(...) ((int(*)(const char*, ...))_sys_table_ptrs[438])(__VA_ARGS__)

/* FatFS wrappers from m-os-api-ff.h */
#include "m-os-api-ff.h"

/*==========================================================================
 * Constants
 *=========================================================================*/

#define NP_TEXT_BUF_SIZE  (TEXTAREA_MAX_SIZE + 1)   /* 32KB + NUL */
#define NP_PATH_MAX       256

/* Menu command IDs */
#define CMD_NEW           100
#define CMD_OPEN          101
#define CMD_SAVE          102
#define CMD_SAVE_AS       103
#define CMD_EXIT          104

#define CMD_CUT           200
#define CMD_COPY          201
#define CMD_PASTE         202
#define CMD_SELECT_ALL    203
#define CMD_FIND          204
#define CMD_REPLACE       205

#define CMD_ABOUT         300

/* Pending action states (for save-changes dialog) */
#define PENDING_NONE      0
#define PENDING_NEW       1
#define PENDING_OPEN      2
#define PENDING_EXIT      3

/*==========================================================================
 * App state
 *=========================================================================*/

typedef struct {
    hwnd_t       hwnd;
    textarea_t   ta;
    char        *text_buf;
    char         filepath[NP_PATH_MAX];
    bool         modified;
    uint8_t      pending_action;
    TimerHandle_t blink_timer;
} notepad_t;

static notepad_t np;
static void *app_task;
static volatile bool app_closing;

/*==========================================================================
 * Menu setup
 *=========================================================================*/

static void np_setup_menu(hwnd_t hwnd) {
    menu_bar_t bar;
    memset(&bar, 0, sizeof(bar));
    bar.menu_count = 3;

    /* File menu */
    menu_def_t *file = &bar.menus[0];
    strncpy(file->title, "File", sizeof(file->title) - 1);
    file->accel_key = 0x09; /* HID 'F' */
    file->item_count = 6;
    strncpy(file->items[0].text, "New",        sizeof(file->items[0].text) - 1);
    file->items[0].command_id = CMD_NEW;
    strncpy(file->items[1].text, "Open...",    sizeof(file->items[1].text) - 1);
    file->items[1].command_id = CMD_OPEN;
    strncpy(file->items[2].text, "Save",       sizeof(file->items[2].text) - 1);
    file->items[2].command_id = CMD_SAVE;
    strncpy(file->items[3].text, "Save As...", sizeof(file->items[3].text) - 1);
    file->items[3].command_id = CMD_SAVE_AS;
    file->items[4].flags = MIF_SEPARATOR;
    strncpy(file->items[5].text, "Exit",       sizeof(file->items[5].text) - 1);
    file->items[5].command_id = CMD_EXIT;

    /* Edit menu */
    menu_def_t *edit = &bar.menus[1];
    strncpy(edit->title, "Edit", sizeof(edit->title) - 1);
    edit->accel_key = 0x08; /* HID 'E' */
    edit->item_count = 7;
    strncpy(edit->items[0].text, "Cut",        sizeof(edit->items[0].text) - 1);
    edit->items[0].command_id = CMD_CUT;
    strncpy(edit->items[1].text, "Copy",       sizeof(edit->items[1].text) - 1);
    edit->items[1].command_id = CMD_COPY;
    strncpy(edit->items[2].text, "Paste",      sizeof(edit->items[2].text) - 1);
    edit->items[2].command_id = CMD_PASTE;
    strncpy(edit->items[3].text, "Select All", sizeof(edit->items[3].text) - 1);
    edit->items[3].command_id = CMD_SELECT_ALL;
    edit->items[4].flags = MIF_SEPARATOR;
    strncpy(edit->items[5].text, "Find...",    sizeof(edit->items[5].text) - 1);
    edit->items[5].command_id = CMD_FIND;
    strncpy(edit->items[6].text, "Replace...", sizeof(edit->items[6].text) - 1);
    edit->items[6].command_id = CMD_REPLACE;

    /* Help menu */
    menu_def_t *help = &bar.menus[2];
    strncpy(help->title, "Help", sizeof(help->title) - 1);
    help->accel_key = 0x0B; /* HID 'H' */
    help->item_count = 1;
    strncpy(help->items[0].text, "About Notepad", sizeof(help->items[0].text) - 1);
    help->items[0].command_id = CMD_ABOUT;

    menu_set(hwnd, &bar);
}

/*==========================================================================
 * Title bar update
 *=========================================================================*/

static void np_update_title(void) {
    window_t *win = wm_get_window(np.hwnd);
    if (!win) return;

    const char *name = "Untitled";
    if (np.filepath[0]) {
        /* Find basename */
        const char *slash = np.filepath;
        const char *p = np.filepath;
        while (*p) {
            if (*p == '/') slash = p + 1;
            p++;
        }
        name = slash;
    }

    if (np.modified) {
        char title[24];
        int nlen = 0;
        const char *s = name;
        title[nlen++] = '*';
        while (*s && nlen < 18) title[nlen++] = *s++;
        const char *suffix = " - Notepad";
        const char *q = suffix;
        while (*q && nlen < 23) title[nlen++] = *q++;
        title[nlen] = '\0';
        memcpy(win->title, title, nlen + 1);
    } else {
        char title[24];
        int nlen = 0;
        const char *s = name;
        while (*s && nlen < 18) title[nlen++] = *s++;
        const char *suffix = " - Notepad";
        const char *q = suffix;
        while (*q && nlen < 23) title[nlen++] = *q++;
        title[nlen] = '\0';
        memcpy(win->title, title, nlen + 1);
    }

    wm_invalidate(np.hwnd);
    taskbar_invalidate();
}

/*==========================================================================
 * File I/O
 *=========================================================================*/

/* Check if buffer looks like a binary file (has NUL bytes or too many
 * non-printable characters in the first portion). */
static bool np_is_binary(const char *buf, uint32_t len) {
    uint32_t check = len > 4096 ? 4096 : len;
    uint32_t ctrl = 0;
    for (uint32_t i = 0; i < check; i++) {
        unsigned char c = (unsigned char)buf[i];
        if (c == 0) return true;  /* NUL byte = binary */
        if (c < 0x20 && c != '\n' && c != '\r' && c != '\t')
            ctrl++;
    }
    /* More than 10% control characters = binary */
    return (check > 0 && ctrl * 10 > check);
}

static bool np_load_file(const char *path) {
    FIL fil;
    if (f_open(&fil, path, FA_READ) != FR_OK)
        return false;

    UINT br;
    uint32_t size = f_size(&fil);
    if (size >= NP_TEXT_BUF_SIZE)
        size = NP_TEXT_BUF_SIZE - 1;

    if (f_read(&fil, np.text_buf, size, &br) != FR_OK) {
        f_close(&fil);
        return false;
    }
    f_close(&fil);

    np.text_buf[br] = '\0';

    /* Reject binary files */
    if (np_is_binary(np.text_buf, br)) {
        np.text_buf[0] = '\0';
        dialog_show(np.hwnd, "Notepad",
                    "Cannot open this file.\n"
                    "It appears to be a binary file.",
                    DLG_ICON_ERROR, DLG_BTN_OK);
        return false;
    }

    textarea_set_text(&np.ta, np.text_buf, (int32_t)br);

    strncpy(np.filepath, path, NP_PATH_MAX - 1);
    np.filepath[NP_PATH_MAX - 1] = '\0';
    np.modified = false;
    np_update_title();
    return true;
}

static bool np_save_file(const char *path) {
    FIL fil;
    if (f_open(&fil, path, FA_WRITE | FA_CREATE_ALWAYS) != FR_OK)
        return false;

    const char *text = textarea_get_text(&np.ta);
    int32_t len = textarea_get_length(&np.ta);
    UINT bw;
    if (f_write(&fil, text, (UINT)len, &bw) != FR_OK || (int32_t)bw != len) {
        f_close(&fil);
        return false;
    }
    f_close(&fil);

    strncpy(np.filepath, path, NP_PATH_MAX - 1);
    np.filepath[NP_PATH_MAX - 1] = '\0';
    np.modified = false;
    np_update_title();
    return true;
}

/*==========================================================================
 * Get current directory from filepath
 *=========================================================================*/

static void np_get_dir(char *dir, int max_len) {
    if (np.filepath[0]) {
        strncpy(dir, np.filepath, max_len - 1);
        dir[max_len - 1] = '\0';
        char *slash = dir;
        char *p = dir;
        while (*p) {
            if (*p == '/') slash = p;
            p++;
        }
        if (slash == dir) {
            dir[0] = '/';
            dir[1] = '\0';
        } else {
            *slash = '\0';
        }
    } else {
        dir[0] = '/';
        dir[1] = '\0';
    }
}

/* Get just the filename from filepath */
static const char *np_get_filename(void) {
    if (!np.filepath[0]) return NULL;
    const char *slash = np.filepath;
    const char *p = np.filepath;
    while (*p) {
        if (*p == '/') slash = p + 1;
        p++;
    }
    return slash;
}

/*==========================================================================
 * Actions
 *=========================================================================*/

static void np_do_save_as(void);

static void np_do_new(void) {
    np.text_buf[0] = '\0';
    textarea_set_text(&np.ta, "", 0);
    np.filepath[0] = '\0';
    np.modified = false;
    np_update_title();
}

static void np_do_open(void) {
    char dir[NP_PATH_MAX];
    np_get_dir(dir, NP_PATH_MAX);
    file_dialog_open(np.hwnd, "Open", dir, NULL);
}

static void np_do_save(void) {
    if (np.filepath[0]) {
        np_save_file(np.filepath);
    } else {
        np_do_save_as();
    }
}

static void np_do_save_as(void) {
    char dir[NP_PATH_MAX];
    np_get_dir(dir, NP_PATH_MAX);
    const char *fname = np_get_filename();
    file_dialog_save(np.hwnd, "Save As", dir, NULL,
                     fname ? fname : "untitled.txt");
}

static void np_do_exit(void) {
    app_closing = true;
    wm_destroy_window(np.hwnd);
    xTaskNotifyGive(app_task);
}

static void np_prompt_save(uint8_t pending) {
    np.pending_action = pending;
    dialog_show(np.hwnd, "Notepad",
                "The text has been changed.\n"
                "Do you want to save the changes?",
                DLG_ICON_WARNING,
                DLG_BTN_YES | DLG_BTN_NO | DLG_BTN_CANCEL);
}

static void np_resume_pending(void) {
    uint8_t action = np.pending_action;
    np.pending_action = PENDING_NONE;

    if (action == PENDING_NEW) {
        np_do_new();
    } else if (action == PENDING_OPEN) {
        np_do_open();
    } else if (action == PENDING_EXIT) {
        np_do_exit();
    }
}

/*==========================================================================
 * Blink timer callback
 *=========================================================================*/

static void np_blink_callback(TimerHandle_t xTimer) {
    (void)xTimer;
    textarea_blink(&np.ta);
}

/*==========================================================================
 * Event handler (must use if/else if chains, no switch)
 *=========================================================================*/

static bool np_event(hwnd_t hwnd, const window_event_t *event) {
    (void)hwnd;

    if (event->type == WM_CLOSE) {
        if (np.modified) {
            np_prompt_save(PENDING_EXIT);
        } else {
            np_do_exit();
        }
        return true;
    }

    else if (event->type == WM_SIZE) {
        int16_t w = event->size.w;
        int16_t h = event->size.h;
        textarea_set_rect(&np.ta, 4, 2, w - 8, h - 4);
        wm_invalidate(np.hwnd);
        return true;
    }

    else if (event->type == WM_SETFOCUS) {
        /* Ensure textarea repaints when focus returns (e.g. after dialog) */
        wm_invalidate(np.hwnd);
        return true;
    }

    else if (event->type == WM_COMMAND) {
        uint16_t cmd = event->command.id;

        /* Menu commands */
        if (cmd == CMD_NEW) {
            if (np.modified) {
                np_prompt_save(PENDING_NEW);
            } else {
                np_do_new();
            }
            return true;
        }
        else if (cmd == CMD_OPEN) {
            if (np.modified) {
                np_prompt_save(PENDING_OPEN);
            } else {
                np_do_open();
            }
            return true;
        }
        else if (cmd == CMD_SAVE) {
            np_do_save();
            return true;
        }
        else if (cmd == CMD_SAVE_AS) {
            np_do_save_as();
            return true;
        }
        else if (cmd == CMD_EXIT) {
            if (np.modified) {
                np_prompt_save(PENDING_EXIT);
            } else {
                np_do_exit();
            }
            return true;
        }
        else if (cmd == CMD_CUT) {
            textarea_cut(&np.ta);
            np.modified = true;
            np_update_title();
            return true;
        }
        else if (cmd == CMD_COPY) {
            textarea_copy(&np.ta);
            return true;
        }
        else if (cmd == CMD_PASTE) {
            textarea_paste(&np.ta);
            np.modified = true;
            np_update_title();
            return true;
        }
        else if (cmd == CMD_SELECT_ALL) {
            textarea_select_all(&np.ta);
            return true;
        }
        else if (cmd == CMD_FIND) {
            find_dialog_show(np.hwnd);
            return true;
        }
        else if (cmd == CMD_REPLACE) {
            replace_dialog_show(np.hwnd);
            return true;
        }
        else if (cmd == CMD_ABOUT) {
            dialog_show(np.hwnd, "About Notepad",
                        "Notepad\n\nFRANK OS v" FRANK_VERSION_STR
                        " (c) 2026\nMikhail Matveev",
                        DLG_ICON_INFO, DLG_BTN_OK);
            return true;
        }

        /* Dialog results */
        else if (cmd == DLG_RESULT_FILE) {
            /* Open dialog returned a file */
            const char *path = file_dialog_get_path();
            if (path && path[0]) {
                np_load_file(path);
            }
            wm_invalidate(np.hwnd);
            return true;
        }
        else if (cmd == DLG_RESULT_FILE_SAVE) {
            /* Save dialog returned a path */
            const char *path = file_dialog_get_path();
            if (path && path[0]) {
                np_save_file(path);
            }
            /* If there's a pending action (from "Save changes?" → Yes → Save As),
             * resume it now that the file has been saved */
            if (np.pending_action != PENDING_NONE) {
                np_resume_pending();
            }
            wm_invalidate(np.hwnd);
            return true;
        }
        else if (cmd == DLG_RESULT_YES) {
            /* Save changes: save, then resume pending action */
            if (np.filepath[0]) {
                np_save_file(np.filepath);
                np_resume_pending();
            } else {
                /* Need Save As first — save the pending action
                 * and open save dialog. The resume will happen
                 * after the save dialog completes. */
                np_do_save_as();
            }
            wm_invalidate(np.hwnd);
            return true;
        }
        else if (cmd == DLG_RESULT_NO) {
            /* Don't save — just resume pending action */
            np.modified = false;
            np_resume_pending();
            wm_invalidate(np.hwnd);
            return true;
        }
        else if (cmd == DLG_RESULT_CANCEL) {
            /* Cancel — abort pending action */
            np.pending_action = PENDING_NONE;
            wm_invalidate(np.hwnd);
            return true;
        }
        else if (cmd == DLG_RESULT_FIND_NEXT) {
            const char *needle = find_dialog_get_text();
            bool cs = find_dialog_case_sensitive();
            textarea_find(&np.ta, needle, cs, true);
            wm_invalidate(np.hwnd);
            return true;
        }
        else if (cmd == DLG_RESULT_REPLACE) {
            const char *needle = find_dialog_get_text();
            const char *repl = find_dialog_get_replace_text();
            bool cs = find_dialog_case_sensitive();
            if (textarea_replace(&np.ta, needle, repl, cs)) {
                np.modified = true;
                np_update_title();
            }
            wm_invalidate(np.hwnd);
            return true;
        }
        else if (cmd == DLG_RESULT_REPLACE_ALL) {
            const char *needle = find_dialog_get_text();
            const char *repl = find_dialog_get_replace_text();
            bool cs = find_dialog_case_sensitive();
            int count = textarea_replace_all(&np.ta, needle, repl, cs);
            if (count > 0) {
                np.modified = true;
                np_update_title();
            }
            wm_invalidate(np.hwnd);
            return true;
        }
        else if (cmd == DLG_RESULT_OK) {
            /* About dialog closed — just repaint */
            wm_invalidate(np.hwnd);
            return true;
        }

        return false;
    }

    else if (event->type == WM_KEYDOWN) {
        /* Track modification on editing keys */
        uint8_t sc = event->key.scancode;
        uint8_t mod = event->key.modifiers;
        bool ctrl = (mod & KMOD_CTRL) != 0;

        /* Ctrl+S — save */
        if (ctrl && sc == 0x16) { /* HID 's' */
            np_do_save();
            return true;
        }
        /* Ctrl+O — open */
        if (ctrl && sc == 0x12) { /* HID 'o' */
            if (np.modified) {
                np_prompt_save(PENDING_OPEN);
            } else {
                np_do_open();
            }
            return true;
        }
        /* Ctrl+N — new */
        if (ctrl && sc == 0x11) { /* HID 'n' */
            if (np.modified) {
                np_prompt_save(PENDING_NEW);
            } else {
                np_do_new();
            }
            return true;
        }
        /* Ctrl+F — find */
        if (ctrl && sc == 0x09) { /* HID 'f' */
            find_dialog_show(np.hwnd);
            return true;
        }
        /* Ctrl+H — replace */
        if (ctrl && sc == 0x0B) { /* HID 'h' */
            replace_dialog_show(np.hwnd);
            return true;
        }

        /* Forward to textarea — detect modifications */
        bool was_modified = np.modified;
        int32_t old_len = textarea_get_length(&np.ta);
        bool handled = textarea_event(&np.ta, event);

        if (handled && !was_modified) {
            int32_t new_len = textarea_get_length(&np.ta);
            if (new_len != old_len) {
                np.modified = true;
                np_update_title();
            }
        }
        return handled;
    }

    else if (event->type == WM_CHAR) {
        bool was_modified = np.modified;
        int32_t old_len = textarea_get_length(&np.ta);
        bool handled = textarea_event(&np.ta, event);

        if (handled && !was_modified) {
            int32_t new_len = textarea_get_length(&np.ta);
            if (new_len != old_len) {
                np.modified = true;
                np_update_title();
            }
        }
        return handled;
    }

    else if (event->type == WM_LBUTTONDOWN ||
             event->type == WM_LBUTTONUP ||
             event->type == WM_MOUSEMOVE) {
        return textarea_event(&np.ta, event);
    }

    return false;
}

/*==========================================================================
 * Paint handler
 *=========================================================================*/

static void np_paint(hwnd_t hwnd) {
    wd_begin(hwnd);
    textarea_paint(&np.ta);
    wd_end();
}

/*==========================================================================
 * Entry point
 *=========================================================================*/

int main(int argc, char **argv) {
    app_task = xTaskGetCurrentTaskHandle();
    app_closing = false;

    /* Allocate text buffer */
    np.text_buf = (char *)malloc(NP_TEXT_BUF_SIZE);
    if (!np.text_buf) {
        dbg_printf("[notepad] failed to allocate text buffer\n");
        return 1;
    }
    np.text_buf[0] = '\0';

    /* Create window */
    int16_t win_w = 500 + 2 * THEME_BORDER_WIDTH;
    int16_t win_h = 350 + THEME_TITLE_HEIGHT + THEME_MENU_HEIGHT +
                    2 * THEME_BORDER_WIDTH;

    np.hwnd = wm_create_window(40, 20, win_w, win_h,
                                 "Untitled - Notepad",
                                 WSTYLE_DEFAULT | WF_MENUBAR,
                                 np_event, np_paint);
    if (np.hwnd == HWND_NULL) {
        free(np.text_buf);
        return 1;
    }

    /* Set up menu bar */
    np_setup_menu(np.hwnd);

    /* Initialize textarea */
    textarea_init(&np.ta, np.text_buf, NP_TEXT_BUF_SIZE, np.hwnd);

    /* Set textarea to fill client area (with padding) */
    rect_t cr = wm_get_client_rect(np.hwnd);
    textarea_set_rect(&np.ta, 4, 2, cr.w - 8, cr.h - 4);

    /* Initialize state */
    np.filepath[0] = '\0';
    np.modified = false;
    np.pending_action = PENDING_NONE;

    /* Start cursor blink timer */
    np.blink_timer = xTimerCreate("npblink", pdMS_TO_TICKS(500),
                                    pdTRUE, NULL, np_blink_callback);
    if (np.blink_timer) xTimerStart(np.blink_timer, 0);

    wm_show_window(np.hwnd);
    wm_set_focus(np.hwnd);
    taskbar_invalidate();

    /* If a file path was passed as argument, open it */
    if (argc > 1 && argv[1] && argv[1][0]) {
        np_load_file(argv[1]);
    }

    dbg_printf("[notepad] started\n");

    /* Main loop — block until close */
    while (!app_closing) {
        uint32_t nv = ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        (void)nv;
    }

    /* Cleanup */
    if (np.blink_timer) {
        xTimerStop(np.blink_timer, 0);
        xTimerDelete(np.blink_timer, 0);
        np.blink_timer = NULL;
    }
    find_dialog_close();
    free(np.text_buf);

    dbg_printf("[notepad] exited\n");
    return 0;
}
