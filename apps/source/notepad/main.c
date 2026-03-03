/*
 * FRANK OS — Notepad (standalone ELF app)
 * Copyright (c) 2025 Mikhail Matveev <xtreme@rh1.tech>
 * https://rh1.tech
 *
 * Full-featured text editor with menu bar, file I/O, clipboard,
 * find/replace, save-changes prompts, and syntax highlighting.
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

/* Syntax menu command IDs */
#define CMD_SYNTAX_PLAIN  400
#define CMD_SYNTAX_C      401
#define CMD_SYNTAX_CPP    402
#define CMD_SYNTAX_INI    403

/* Dev menu command IDs */
#define CMD_DEV_COMPILE   500
#define CMD_DEV_RUN       501

/* Syntax modes */
#define SYNTAX_PLAIN      0
#define SYNTAX_C          1
#define SYNTAX_CPP        2
#define SYNTAX_INI        3

/* Token types for syntax coloring */
#define TOK_NORMAL        0
#define TOK_KEYWORD       1
#define TOK_TYPE          2
#define TOK_STRING        3
#define TOK_COMMENT       4
#define TOK_PREPROC       5
#define TOK_NUMBER        6
#define TOK_SECTION       7   /* INI [section] */

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
    uint8_t      syntax_mode;
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

    bool has_dev = (np.syntax_mode == SYNTAX_C || np.syntax_mode == SYNTAX_CPP);
    bar.menu_count = has_dev ? 5 : 4;

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

    /* Syntax menu */
    menu_def_t *syntax = &bar.menus[2];
    strncpy(syntax->title, "Syntax", sizeof(syntax->title) - 1);
    syntax->accel_key = 0x16; /* HID 'S' */
    syntax->item_count = 4;

    /* Mark the active syntax with a bullet character */
    const char *labels[] = { "Plain Text", "C", "C++", "INI" };
    uint16_t cmds[] = { CMD_SYNTAX_PLAIN, CMD_SYNTAX_C,
                        CMD_SYNTAX_CPP, CMD_SYNTAX_INI };
    for (int i = 0; i < 4; i++) {
        char label_buf[20];
        if (i == np.syntax_mode) {
            label_buf[0] = '*';
            label_buf[1] = ' ';
            strncpy(label_buf + 2, labels[i], sizeof(label_buf) - 3);
            label_buf[sizeof(label_buf) - 1] = '\0';
        } else {
            label_buf[0] = ' ';
            label_buf[1] = ' ';
            strncpy(label_buf + 2, labels[i], sizeof(label_buf) - 3);
            label_buf[sizeof(label_buf) - 1] = '\0';
        }
        strncpy(syntax->items[i].text, label_buf,
                sizeof(syntax->items[i].text) - 1);
        syntax->items[i].command_id = cmds[i];
    }

    int next_menu = 3;

    /* Dev menu — only for C/C++ files */
    if (has_dev) {
        menu_def_t *dev = &bar.menus[next_menu++];
        strncpy(dev->title, "Dev", sizeof(dev->title) - 1);
        dev->accel_key = 0x07; /* HID 'D' */
        dev->item_count = 2;
        strncpy(dev->items[0].text, "Compile", sizeof(dev->items[0].text) - 1);
        dev->items[0].command_id = CMD_DEV_COMPILE;
        strncpy(dev->items[1].text, "Run", sizeof(dev->items[1].text) - 1);
        dev->items[1].command_id = CMD_DEV_RUN;
    }

    /* Help menu */
    menu_def_t *help = &bar.menus[next_menu];
    strncpy(help->title, "Help", sizeof(help->title) - 1);
    help->accel_key = 0x0B; /* HID 'H' */
    help->item_count = 1;
    strncpy(help->items[0].text, "About Notepad", sizeof(help->items[0].text) - 1);
    help->items[0].command_id = CMD_ABOUT;

    menu_set(hwnd, &bar);

    /* Mark frame dirty so the compositor repaints the menu bar.
     * Without this, menu changes after the initial paint are invisible
     * (e.g. Dev menu not appearing when a C file is opened via argv). */
    window_t *w = wm_get_window(hwnd);
    if (w) {
        w->flags |= WF_DIRTY | WF_FRAME_DIRTY;
        wm_mark_dirty();
    }
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
    np.syntax_mode = SYNTAX_PLAIN;
    np_setup_menu(np.hwnd);
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
 * Dev menu actions (Compile / Run)
 *=========================================================================*/

static void np_dev_compile(void) {
    if (!np.filepath[0]) {
        dialog_show(np.hwnd, "Dev",
                    "Save the file first.",
                    DLG_ICON_WARNING, DLG_BTN_OK);
        return;
    }
    if (np.modified) np_do_save();
    /* filepath ends with .c, so pshell enters compile-only mode */
    app_launch_deferred("/fos/pshell", np.filepath);
}

static void np_dev_run(void) {
    if (!np.filepath[0]) {
        dialog_show(np.hwnd, "Dev",
                    "Save the file first.",
                    DLG_ICON_WARNING, DLG_BTN_OK);
        return;
    }
    if (np.modified) np_do_save();
    /* Strip .c extension — pshell will find the .c source and compile+run */
    char path[NP_PATH_MAX];
    strncpy(path, np.filepath, NP_PATH_MAX - 1);
    path[NP_PATH_MAX - 1] = '\0';
    int len = strlen(path);
    if (len > 2 && path[len - 2] == '.' && path[len - 1] == 'c')
        path[len - 2] = '\0';
    app_launch_deferred("/fos/pshell", path);
}

/*==========================================================================
 * Blink timer callback
 *=========================================================================*/

static void np_blink_callback(TimerHandle_t xTimer) {
    (void)xTimer;
    textarea_blink(&np.ta);
}

/*==========================================================================
 * Syntax highlighting — tokenizer
 *=========================================================================*/

/* Color mapping for token types (on white background) */
static uint8_t tok_color(uint8_t tok) {
    if (tok == TOK_KEYWORD)  return COLOR_BLUE;
    if (tok == TOK_TYPE)     return COLOR_CYAN;
    if (tok == TOK_STRING)   return COLOR_RED;
    if (tok == TOK_COMMENT)  return COLOR_GREEN;
    if (tok == TOK_PREPROC)  return COLOR_MAGENTA;
    if (tok == TOK_NUMBER)   return COLOR_BROWN;
    if (tok == TOK_SECTION)  return COLOR_BLUE;
    return COLOR_BLACK;  /* TOK_NORMAL */
}

static bool syn_is_alnum(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9') || c == '_';
}

static bool syn_is_digit(char c) {
    return c >= '0' && c <= '9';
}

/* Check if word matches any entry in a NULL-terminated list */
static bool syn_match_word(const char *buf, int32_t start, int32_t end,
                           const char * const *words) {
    int32_t wlen = end - start;
    for (int i = 0; words[i]; i++) {
        const char *w = words[i];
        int32_t j = 0;
        while (j < wlen && w[j] && buf[start + j] == w[j]) j++;
        if (j == wlen && w[j] == '\0') return true;
    }
    return false;
}

/* C/C++ keywords */
static const char * const c_keywords[] = {
    "auto", "break", "case", "const", "continue", "default", "do",
    "else", "enum", "extern", "for", "goto", "if", "inline",
    "register", "return", "sizeof", "static", "struct", "switch",
    "typedef", "union", "volatile", "while", NULL
};

static const char * const cpp_keywords[] = {
    "auto", "break", "case", "const", "continue", "default", "do",
    "else", "enum", "extern", "for", "goto", "if", "inline",
    "register", "return", "sizeof", "static", "struct", "switch",
    "typedef", "union", "volatile", "while",
    "class", "namespace", "template", "typename", "public", "private",
    "protected", "virtual", "override", "final", "new", "delete",
    "try", "catch", "throw", "using", "nullptr", "constexpr",
    "noexcept", "this", "operator", "dynamic_cast", "static_cast",
    "reinterpret_cast", "const_cast", NULL
};

/* C/C++ type names */
static const char * const c_types[] = {
    "void", "char", "short", "int", "long", "float", "double",
    "signed", "unsigned", "bool", "true", "false", "NULL",
    "int8_t", "int16_t", "int32_t", "int64_t",
    "uint8_t", "uint16_t", "uint32_t", "uint64_t",
    "size_t", "ssize_t", "ptrdiff_t", "intptr_t", "uintptr_t",
    "FILE", "UINT", "BOOL", "DWORD", "BYTE", "WORD", NULL
};

/*
 * Tokenize a single line of C/C++ code.
 * tok_out[i] receives the token type for buf[line_start + i].
 * in_comment: pointer to multi-line comment state (carried across lines).
 */
static void syn_tokenize_c(const char *buf, int32_t line_start,
                            int32_t line_end, uint8_t *tok_out,
                            bool *in_comment, bool is_cpp) {
    int32_t len = line_end - line_start;
    int32_t i = 0;

    /* Default: normal */
    for (int32_t j = 0; j < len; j++) tok_out[j] = TOK_NORMAL;

    while (i < len) {
        char c = buf[line_start + i];

        /* Inside multi-line comment */
        if (*in_comment) {
            int32_t start = i;
            while (i < len) {
                if (buf[line_start + i] == '*' && i + 1 < len &&
                    buf[line_start + i + 1] == '/') {
                    i += 2;
                    *in_comment = false;
                    break;
                }
                i++;
            }
            for (int32_t j = start; j < i; j++) tok_out[j] = TOK_COMMENT;
            continue;
        }

        /* Single-line comment */
        if (c == '/' && i + 1 < len && buf[line_start + i + 1] == '/') {
            for (int32_t j = i; j < len; j++) tok_out[j] = TOK_COMMENT;
            break;
        }

        /* Multi-line comment start */
        if (c == '/' && i + 1 < len && buf[line_start + i + 1] == '*') {
            int32_t start = i;
            i += 2;
            *in_comment = true;
            while (i < len) {
                if (buf[line_start + i] == '*' && i + 1 < len &&
                    buf[line_start + i + 1] == '/') {
                    i += 2;
                    *in_comment = false;
                    break;
                }
                i++;
            }
            for (int32_t j = start; j < i; j++) tok_out[j] = TOK_COMMENT;
            continue;
        }

        /* Preprocessor directive */
        if (c == '#') {
            /* Check if first non-space char on line */
            bool first = true;
            for (int32_t j = 0; j < i; j++) {
                char ch = buf[line_start + j];
                if (ch != ' ' && ch != '\t') { first = false; break; }
            }
            if (first) {
                for (int32_t j = i; j < len; j++) tok_out[j] = TOK_PREPROC;
                break;
            }
        }

        /* String literal */
        if (c == '"' || c == '\'') {
            char quote = c;
            int32_t start = i;
            i++;
            while (i < len) {
                if (buf[line_start + i] == '\\' && i + 1 < len) {
                    i += 2; /* skip escaped char */
                    continue;
                }
                if (buf[line_start + i] == quote) {
                    i++;
                    break;
                }
                i++;
            }
            for (int32_t j = start; j < i; j++) tok_out[j] = TOK_STRING;
            continue;
        }

        /* Number */
        if (syn_is_digit(c) || (c == '.' && i + 1 < len &&
            syn_is_digit(buf[line_start + i + 1]))) {
            /* Check that previous char is not alphanumeric (word boundary) */
            bool at_boundary = (i == 0) ||
                !syn_is_alnum(buf[line_start + i - 1]);
            if (at_boundary) {
                int32_t start = i;
                /* Handle 0x hex prefix */
                if (c == '0' && i + 1 < len &&
                    (buf[line_start + i + 1] == 'x' ||
                     buf[line_start + i + 1] == 'X')) {
                    i += 2;
                    while (i < len && (syn_is_digit(buf[line_start + i]) ||
                           (buf[line_start + i] >= 'a' &&
                            buf[line_start + i] <= 'f') ||
                           (buf[line_start + i] >= 'A' &&
                            buf[line_start + i] <= 'F')))
                        i++;
                } else {
                    while (i < len && (syn_is_digit(buf[line_start + i]) ||
                           buf[line_start + i] == '.'))
                        i++;
                }
                /* Suffix: u, l, f, etc. */
                while (i < len && (buf[line_start + i] == 'u' ||
                       buf[line_start + i] == 'U' ||
                       buf[line_start + i] == 'l' ||
                       buf[line_start + i] == 'L' ||
                       buf[line_start + i] == 'f' ||
                       buf[line_start + i] == 'F'))
                    i++;
                for (int32_t j = start; j < i; j++) tok_out[j] = TOK_NUMBER;
                continue;
            }
        }

        /* Identifier / keyword / type */
        if (syn_is_alnum(c) && !syn_is_digit(c)) {
            int32_t start = i;
            while (i < len && syn_is_alnum(buf[line_start + i])) i++;

            const char * const *kw = is_cpp ? cpp_keywords : c_keywords;
            if (syn_match_word(buf, line_start + start,
                               line_start + i, kw)) {
                for (int32_t j = start; j < i; j++)
                    tok_out[j] = TOK_KEYWORD;
            } else if (syn_match_word(buf, line_start + start,
                                       line_start + i, c_types)) {
                for (int32_t j = start; j < i; j++)
                    tok_out[j] = TOK_TYPE;
            }
            continue;
        }

        i++;
    }
}

/*
 * Tokenize a single line of INI file.
 */
static void syn_tokenize_ini(const char *buf, int32_t line_start,
                              int32_t line_end, uint8_t *tok_out) {
    int32_t len = line_end - line_start;
    for (int32_t j = 0; j < len; j++) tok_out[j] = TOK_NORMAL;
    if (len == 0) return;

    /* Skip leading whitespace */
    int32_t i = 0;
    while (i < len && (buf[line_start + i] == ' ' ||
           buf[line_start + i] == '\t')) i++;
    if (i >= len) return;

    char first = buf[line_start + i];

    /* Comment line (;  or #) */
    if (first == ';' || first == '#') {
        for (int32_t j = i; j < len; j++) tok_out[j] = TOK_COMMENT;
        return;
    }

    /* Section header [section] */
    if (first == '[') {
        for (int32_t j = i; j < len; j++) {
            tok_out[j] = TOK_SECTION;
            if (buf[line_start + j] == ']') {
                /* Color any trailing text as normal */
                break;
            }
        }
        return;
    }

    /* Key = Value: color the key as keyword, = as normal, value as string */
    int32_t eq_pos = -1;
    for (int32_t j = i; j < len; j++) {
        if (buf[line_start + j] == '=') { eq_pos = j; break; }
    }
    if (eq_pos >= 0) {
        /* Key part */
        for (int32_t j = i; j < eq_pos; j++) tok_out[j] = TOK_KEYWORD;
        /* Value part */
        for (int32_t j = eq_pos + 1; j < len; j++) tok_out[j] = TOK_STRING;
    }
}

/*
 * Detect syntax mode from file extension.
 */
static uint8_t np_detect_syntax(const char *path) {
    if (!path || !path[0]) return SYNTAX_PLAIN;

    /* Find last dot */
    const char *dot = NULL;
    const char *p = path;
    while (*p) {
        if (*p == '.') dot = p;
        p++;
    }
    if (!dot) return SYNTAX_PLAIN;
    dot++; /* skip the dot */

    /* Case-insensitive extension match */
    char ext[8];
    int i = 0;
    while (dot[i] && i < 7) {
        char c = dot[i];
        if (c >= 'A' && c <= 'Z') c += 32;
        ext[i] = c;
        i++;
    }
    ext[i] = '\0';

    /* C files */
    if (ext[0] == 'c' && ext[1] == '\0') return SYNTAX_C;
    if (ext[0] == 'h' && ext[1] == '\0') return SYNTAX_C;

    /* C++ files */
    if (ext[0] == 'c' && ext[1] == 'p' && ext[2] == 'p' && ext[3] == '\0')
        return SYNTAX_CPP;
    if (ext[0] == 'c' && ext[1] == 'x' && ext[2] == 'x' && ext[3] == '\0')
        return SYNTAX_CPP;
    if (ext[0] == 'c' && ext[1] == 'c' && ext[2] == '\0')
        return SYNTAX_CPP;
    if (ext[0] == 'h' && ext[1] == 'p' && ext[2] == 'p' && ext[3] == '\0')
        return SYNTAX_CPP;
    if (ext[0] == 'h' && ext[1] == 'x' && ext[2] == 'x' && ext[3] == '\0')
        return SYNTAX_CPP;

    /* INI files */
    if (ext[0] == 'i' && ext[1] == 'n' && ext[2] == 'i' && ext[3] == '\0')
        return SYNTAX_INI;
    if (ext[0] == 'c' && ext[1] == 'f' && ext[2] == 'g' && ext[3] == '\0')
        return SYNTAX_INI;

    return SYNTAX_PLAIN;
}

/*==========================================================================
 * Custom textarea paint with syntax highlighting
 *
 * Replicates the kernel's textarea_paint() logic, but applies
 * per-character foreground colors from the syntax tokenizer.
 *=========================================================================*/

/* Max characters per line we can highlight (wider lines fall back to normal) */
#define SYN_MAX_LINE  512

static void np_paint_textarea(textarea_t *ta) {
    int16_t tx = ta->rect_x;
    int16_t ty = ta->rect_y;
    int16_t tw = ta->rect_w;
    int16_t th = ta->rect_h;

    if (ta->vscroll.visible) tw -= SCROLLBAR_WIDTH;
    if (ta->hscroll.visible) th -= SCROLLBAR_WIDTH;

    /* NOTE: No full white background fill here.  Filling the entire text
     * area with white before redrawing causes visible flicker on the
     * single-buffer display (the user sees a white flash every 500ms
     * when the cursor blink timer fires).  Instead, each line's
     * background is filled individually in the rendering loop below,
     * and the remaining area below the last line is filled afterwards. */

    /* Selection range */
    int32_t sel_s, sel_e;
    if (ta->sel_anchor < 0) {
        sel_s = ta->cursor;
        sel_e = ta->cursor;
    } else if (ta->sel_anchor < ta->cursor) {
        sel_s = ta->sel_anchor;
        sel_e = ta->cursor;
    } else {
        sel_s = ta->cursor;
        sel_e = ta->sel_anchor;
    }

    /* Visible line range */
    int32_t first_line = ta->scroll_y / FONT_UI_HEIGHT;
    int32_t visible_lines = th / FONT_UI_HEIGHT + 2;

    /* For C/C++ multi-line comment state, we need to scan from the start
     * up to the first visible line. */
    bool in_comment = false;
    uint8_t mode = np.syntax_mode;

    if (mode == SYNTAX_C || mode == SYNTAX_CPP) {
        /* Quick scan to determine comment state at first_line */
        int32_t off = 0;
        int32_t line = 0;
        while (off < ta->len && line < first_line) {
            char c = ta->buf[off];
            if (in_comment) {
                if (c == '*' && off + 1 < ta->len && ta->buf[off + 1] == '/') {
                    in_comment = false;
                    off += 2;
                    continue;
                }
            } else {
                if (c == '/' && off + 1 < ta->len && ta->buf[off + 1] == '/') {
                    /* Single-line comment — skip to end of line */
                    while (off < ta->len && ta->buf[off] != '\n') off++;
                    if (off < ta->len) { off++; line++; }
                    continue;
                }
                if (c == '/' && off + 1 < ta->len && ta->buf[off + 1] == '*') {
                    in_comment = true;
                    off += 2;
                    continue;
                }
                if (c == '"' || c == '\'') {
                    char q = c;
                    off++;
                    while (off < ta->len && ta->buf[off] != '\n') {
                        if (ta->buf[off] == '\\' && off + 1 < ta->len) {
                            off += 2; continue;
                        }
                        if (ta->buf[off] == q) { off++; break; }
                        off++;
                    }
                    continue;
                }
            }
            if (c == '\n') line++;
            off++;
        }
    }

    /* Walk to first visible line's byte offset */
    int32_t offset = 0;
    int32_t cur_line = 0;
    while (offset < ta->len && cur_line < first_line) {
        if (ta->buf[offset] == '\n') cur_line++;
        offset++;
    }

    /* Token buffer for one line */
    uint8_t tok_buf[SYN_MAX_LINE];

    /* Draw visible lines */
    for (int32_t vl = 0; vl < visible_lines && offset <= ta->len; vl++) {
        int32_t line_num = first_line + vl;
        int32_t py = ty + line_num * FONT_UI_HEIGHT - ta->scroll_y;

        if (py >= ty + th) break;
        if (py + FONT_UI_HEIGHT > ty + th) {
            /* Line extends past text rect bottom — skip to avoid drawing
             * into the scrollbar area. Still tokenize for comment state. */
            int32_t ls = offset;
            while (offset < ta->len && ta->buf[offset] != '\n') offset++;
            if (mode == SYNTAX_C || mode == SYNTAX_CPP) {
                int32_t le = offset;
                int32_t llen = le - ls;
                if (llen <= SYN_MAX_LINE) {
                    syn_tokenize_c(ta->buf, ls, le, tok_buf,
                                   &in_comment, mode == SYNTAX_CPP);
                }
            }
            if (offset < ta->len) offset++;
            continue;
        }
        if (py < ty) {
            /* Skip line above visible area (but still tokenize for
             * multi-line comment state tracking) */
            int32_t ls = offset;
            while (offset < ta->len && ta->buf[offset] != '\n') offset++;
            if (mode == SYNTAX_C || mode == SYNTAX_CPP) {
                int32_t le = offset;
                int32_t llen = le - ls;
                if (llen <= SYN_MAX_LINE) {
                    syn_tokenize_c(ta->buf, ls, le, tok_buf,
                                   &in_comment, mode == SYNTAX_CPP);
                }
            }
            if (offset < ta->len) offset++;
            continue;
        }

        /* Fill this line's background (replaces the old full-area fill) */
        wd_fill_rect(tx, py, tw, FONT_UI_HEIGHT, COLOR_WHITE);

        /* Find end of line */
        int32_t line_start = offset;
        int32_t line_end = line_start;
        while (line_end < ta->len && ta->buf[line_end] != '\n') line_end++;

        int32_t line_len = line_end - line_start;

        /* Tokenize the line */
        if (mode != SYNTAX_PLAIN && line_len > 0 &&
            line_len <= SYN_MAX_LINE) {
            if (mode == SYNTAX_C || mode == SYNTAX_CPP) {
                syn_tokenize_c(ta->buf, line_start, line_end, tok_buf,
                               &in_comment, mode == SYNTAX_CPP);
            } else if (mode == SYNTAX_INI) {
                syn_tokenize_ini(ta->buf, line_start, line_end, tok_buf);
            }
        }

        /* Draw characters of this line */
        int32_t col = 0;
        for (int32_t i = line_start; i < line_end; i++, col++) {
            int32_t px = tx + col * FONT_UI_WIDTH - ta->scroll_x;

            if (px + FONT_UI_WIDTH <= tx) continue;
            if (px >= tx + tw) break;

            bool in_sel = (sel_s != sel_e && i >= sel_s && i < sel_e);
            uint8_t fg, bg;
            if (in_sel) {
                fg = COLOR_WHITE;
                bg = COLOR_BLUE;
            } else {
                bg = COLOR_WHITE;
                if (mode != SYNTAX_PLAIN && line_len <= SYN_MAX_LINE) {
                    fg = tok_color(tok_buf[i - line_start]);
                } else {
                    fg = COLOR_BLACK;
                }
            }

            wd_char_ui(px, py, ta->buf[i], fg, bg);
        }

        /* Selection highlight for trailing newline */
        if (sel_s != sel_e) {
            if (line_end >= sel_s && line_end < sel_e && line_end < ta->len) {
                int32_t end_px = tx + col * FONT_UI_WIDTH - ta->scroll_x;
                if (end_px < tx + tw) {
                    wd_fill_rect(end_px, py,
                                 FONT_UI_WIDTH, FONT_UI_HEIGHT, COLOR_BLUE);
                }
            }
        }

        /* Advance past newline */
        offset = line_end;
        if (offset < ta->len) offset++;
    }

    /* Fill area below last drawn line (empty space in text area) */
    {
        int32_t last_line = ta->scroll_y / FONT_UI_HEIGHT +
                            (th / FONT_UI_HEIGHT + 2);
        int32_t bottom_y = ty + last_line * FONT_UI_HEIGHT - ta->scroll_y;
        if (bottom_y < ty) bottom_y = ty;
        if (bottom_y < ty + th)
            wd_fill_rect(tx, bottom_y, tw, ty + th - bottom_y, COLOR_WHITE);
    }

    /* Draw cursor */
    if (ta->cursor_visible) {
        /* Compute cursor line and column */
        int32_t cline = 0, ccol = 0;
        for (int32_t i = 0; i < ta->cursor && i < ta->len; i++) {
            if (ta->buf[i] == '\n') { cline++; ccol = 0; }
            else ccol++;
        }
        int32_t cx = tx + ccol * FONT_UI_WIDTH - ta->scroll_x;
        int32_t cy = ty + cline * FONT_UI_HEIGHT - ta->scroll_y;

        if (cx >= tx && cx < tx + tw &&
            cy >= ty && cy + FONT_UI_HEIGHT <= ty + th) {
            for (int r = 0; r < FONT_UI_HEIGHT; r++) {
                wd_pixel(cx, cy + r, COLOR_BLACK);
            }
        }
    }

    /* Draw scrollbars */
    scrollbar_paint(&ta->vscroll);
    scrollbar_paint(&ta->hscroll);

    /* Corner box if both scrollbars visible */
    if (ta->vscroll.visible && ta->hscroll.visible) {
        wd_fill_rect(ta->rect_x + ta->rect_w - SCROLLBAR_WIDTH,
                     ta->rect_y + ta->rect_h - SCROLLBAR_WIDTH,
                     SCROLLBAR_WIDTH, SCROLLBAR_WIDTH, THEME_BUTTON_FACE);
    }
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
        /* Restart cursor blink and repaint when focus returns */
        np.ta.cursor_visible = true;
        if (np.blink_timer) xTimerStart(np.blink_timer, 0);
        wm_invalidate(np.hwnd);
        return true;
    }

    else if (event->type == WM_KILLFOCUS) {
        /* Stop cursor blink when window loses focus */
        if (np.blink_timer) xTimerStop(np.blink_timer, 0);
        np.ta.cursor_visible = false;
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

        /* Syntax menu commands */
        else if (cmd == CMD_SYNTAX_PLAIN) {
            np.syntax_mode = SYNTAX_PLAIN;
            np_setup_menu(np.hwnd);
            wm_invalidate(np.hwnd);
            return true;
        }
        else if (cmd == CMD_SYNTAX_C) {
            np.syntax_mode = SYNTAX_C;
            np_setup_menu(np.hwnd);
            wm_invalidate(np.hwnd);
            return true;
        }
        else if (cmd == CMD_SYNTAX_CPP) {
            np.syntax_mode = SYNTAX_CPP;
            np_setup_menu(np.hwnd);
            wm_invalidate(np.hwnd);
            return true;
        }
        else if (cmd == CMD_SYNTAX_INI) {
            np.syntax_mode = SYNTAX_INI;
            np_setup_menu(np.hwnd);
            wm_invalidate(np.hwnd);
            return true;
        }

        /* Dev menu commands */
        else if (cmd == CMD_DEV_COMPILE) {
            np_dev_compile();
            return true;
        }
        else if (cmd == CMD_DEV_RUN) {
            np_dev_run();
            return true;
        }

        /* Dialog results */
        else if (cmd == DLG_RESULT_FILE) {
            /* Open dialog returned a file */
            const char *path = file_dialog_get_path();
            if (path && path[0]) {
                np_load_file(path);
                np.syntax_mode = np_detect_syntax(np.filepath);
                np_setup_menu(np.hwnd);
            }
            wm_invalidate(np.hwnd);
            return true;
        }
        else if (cmd == DLG_RESULT_FILE_SAVE) {
            /* Save dialog returned a path */
            const char *path = file_dialog_get_path();
            if (path && path[0]) {
                np_save_file(path);
                np.syntax_mode = np_detect_syntax(np.filepath);
                np_setup_menu(np.hwnd);
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
    np_paint_textarea(&np.ta);
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
    np.syntax_mode = SYNTAX_PLAIN;

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
        np.syntax_mode = np_detect_syntax(np.filepath);
        np_setup_menu(np.hwnd);
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
