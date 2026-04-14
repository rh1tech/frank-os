/*
 * FRANK OS — Calculator (standalone ELF app)
 * Copyright (c) 2026 Mikhail Matveev <xtreme@rh1.tech>
 * https://rh1.tech
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "m-os-api.h"
#include "frankos-app.h"
#include "lang.h"

/* App-local translations */
enum { AL_ABOUT, AL_COUNT };
static const char *al_en[] = { [AL_ABOUT] = "About Calculator" };
static const char *al_ru[] = { [AL_ABOUT] = "\xD0\x9E \xD0\x9A\xD0\xB0\xD0\xBB\xD1\x8C\xD0\xBA\xD1\x83\xD0\xBB\xD1\x8F\xD1\x82\xD0\xBE\xD1\x80\xD0\xB5" };
static const char *AL(int id) { return lang_get() == LANG_RU ? al_ru[id] : al_en[id]; }

#define dbg_printf(...) ((int(*)(const char*, ...))_sys_table_ptrs[438])(__VA_ARGS__)

/*==========================================================================
 * Layout
 *=========================================================================*/

#define MARGIN      6
#define BTN_W      36
#define BTN_H      22
#define BTN_GAP     2
#define DISP_H     22
#define COLS        5
#define ROWS        6
#define GRID_X      MARGIN
#define GRID_Y      (MARGIN + DISP_H + MARGIN)

#define CLIENT_W    (MARGIN*2 + COLS*BTN_W + (COLS-1)*BTN_GAP)
#define CLIENT_H    (GRID_Y + ROWS*BTN_H + (ROWS-1)*BTN_GAP + MARGIN)

#define MAX_DISP    16

/*==========================================================================
 * Menu command IDs
 *=========================================================================*/

#define CMD_EXIT    100
#define CMD_COPY    101
#define CMD_ABOUT   200

/*==========================================================================
 * Calculator state — allocated via calloc, stored in win->user_data
 *=========================================================================*/

typedef struct {
    hwnd_t hwnd;

    /* Button labels — RAM copy of const data.
     * Copied in calc_create() from .text.startup context where
     * .rodata relocations work; used by paint handler in .text. */
    char labels[ROWS][COLS][4];

    /* About dialog strings — RAM copies */
    char about_title[20];
    char about_text[128];

    /* Display */
    char    disp[MAX_DISP + 4];
    double  disp_val;
    double  accum;
    double  memory;
    char    pending_op;
    int     pressed_row;
    int     pressed_col;
    bool    has_dot;
    bool    new_input;
    bool    has_mem;
    bool    is_error;
} calc_t;

static void *app_task;
static volatile bool app_closing;

/*==========================================================================
 * Const label source (accessed only from .text.startup via calc_create)
 *=========================================================================*/

static const char labels_src[ROWS][COLS][4] = {
    { "MC",  "MR",  "MS",  "M+",  "Bk"  },
    { "CE",  "C",   "+/-", "sq",  "/"    },
    { "7",   "8",   "9",   "*",   "%"    },
    { "4",   "5",   "6",   "-",   "1/x"  },
    { "1",   "2",   "3",   "+",   "="    },
    { "0",   "",    ".",   "",    ""     },
};

/*==========================================================================
 * Helpers
 *=========================================================================*/

static int btn_colspan(int row, int col) {
    if (row == 5 && (col == 0 || col == 3)) return 2;
    return 1;
}

/*==========================================================================
 * Formatting
 *=========================================================================*/

static void format_disp(calc_t *c) {
    if (c->is_error) { strcpy(c->disp, "Error"); return; }
    double v = c->disp_val;
    /* Use int (not long long) to avoid __aeabi_d2lz / __aeabi_l2d
     * which are not provided by m-os-api-math.c */
    int iv = (int)v;
    if (v == (double)iv && !c->has_dot &&
        v >= -999999999.0 && !(v > 999999999.0)) {
        char buf[16];
        int p = 0;
        bool neg = iv < 0;
        if (neg) iv = -iv;
        if (iv == 0) buf[p++] = '0';
        else while (iv > 0) { buf[p++] = '0' + iv % 10; iv /= 10; }
        if (neg) buf[p++] = '-';
        for (int i = 0; i < p; i++) c->disp[i] = buf[p - 1 - i];
        c->disp[p] = '\0';
    } else {
        snprintf(c->disp, sizeof(c->disp), "%.10g", v);
    }
}

static void set_val(calc_t *c, double v) {
    c->disp_val = v; c->has_dot = false; format_disp(c);
}

/*==========================================================================
 * Calculator logic
 *=========================================================================*/

static double apply_op(double l, char op, double r) {
    if (op == '+') return l + r;
    if (op == '-') return l - r;
    if (op == '*') return l * r;
    if (op == '/') return r != 0.0 ? l / r : 0.0;
    return r;
}

static void do_digit(calc_t *c, char ch) {
    if (c->is_error) return;
    if (c->new_input) { c->disp[0] = '\0'; c->has_dot = false; c->new_input = false; }
    int len = strlen(c->disp);
    if (len == 1 && c->disp[0] == '0' && ch != '.') { c->disp[0] = ch; goto reparse; }
    if (len >= MAX_DISP) return;
    if (ch == '.') { if (c->has_dot) return; c->has_dot = true; if (!len) { c->disp[0]='0'; len++; } }
    c->disp[len] = ch; c->disp[len + 1] = '\0';
reparse:;
    {
        const char *s = c->disp;
        double sign = 1, intp = 0, frac = 0, dv = 1;
        bool pd = false;
        if (*s == '-') { sign = -1; s++; }
        while (*s) {
            if (*s == '.') { pd = true; s++; continue; }
            if (pd) { frac = frac * 10 + (*s - '0'); dv *= 10; }
            else intp = intp * 10 + (*s - '0');
            s++;
        }
        c->disp_val = sign * (intp + frac / dv);
    }
}

static void do_op(calc_t *c, char op) {
    if (c->is_error) return;
    if (c->pending_op && !c->new_input) {
        c->accum = apply_op(c->accum, c->pending_op, c->disp_val);
        set_val(c, c->accum);
    } else c->accum = c->disp_val;
    c->pending_op = op; c->new_input = true;
}

static void do_equals(calc_t *c) {
    if (c->is_error) return;
    if (c->pending_op) {
        if (c->pending_op == '/' && c->disp_val == 0.0) {
            c->is_error = true; strcpy(c->disp, "Error"); c->pending_op = 0; return;
        }
        set_val(c, apply_op(c->accum, c->pending_op, c->disp_val));
        c->pending_op = 0;
    }
    c->new_input = true;
}

static void do_clear(calc_t *c) {
    c->disp_val = 0; c->accum = 0; c->pending_op = 0;
    c->has_dot = false; c->new_input = true; c->is_error = false;
    strcpy(c->disp, "0");
}

static void do_clear_entry(calc_t *c) {
    c->disp_val = 0; c->has_dot = false; c->is_error = false;
    strcpy(c->disp, "0");
}

static void do_backspace(calc_t *c) {
    if (c->is_error || c->new_input) return;
    int len = strlen(c->disp);
    if (len <= 1) { do_clear(c); return; }
    if (c->disp[len - 1] == '.') c->has_dot = false;
    c->disp[len - 1] = '\0';
    {
        const char *s = c->disp;
        double sign = 1, intp = 0, frac = 0, dv = 1;
        bool pd = false;
        if (*s == '-') { sign = -1; s++; }
        while (*s) {
            if (*s == '.') { pd = true; s++; continue; }
            if (pd) { frac = frac * 10 + (*s - '0'); dv *= 10; }
            else intp = intp * 10 + (*s - '0');
            s++;
        }
        c->disp_val = sign * (intp + frac / dv);
    }
}

static void do_negate(calc_t *c) {
    if (c->is_error) return;
    c->disp_val = -c->disp_val; format_disp(c);
}

static void do_percent(calc_t *c) {
    if (c->is_error) return;
    c->disp_val = c->accum * c->disp_val / 100.0;
    set_val(c, c->disp_val); c->new_input = true;
}

static void do_sqrt(calc_t *c) {
    if (c->is_error) return;
    if (c->disp_val < 0) { c->is_error = true; strcpy(c->disp, "Error"); return; }
    double x = c->disp_val;
    if (x == 0) { set_val(c, 0); c->new_input = true; return; }
    double r = x;
    for (int i = 0; i < 30; i++) r = (r + x / r) * 0.5;
    set_val(c, r); c->new_input = true;
}

static void do_reciprocal(calc_t *c) {
    if (c->is_error) return;
    if (c->disp_val == 0.0) { c->is_error = true; strcpy(c->disp, "Error"); return; }
    set_val(c, 1.0 / c->disp_val); c->new_input = true;
}

/*==========================================================================
 * Button dispatch
 *=========================================================================*/

static void handle_btn(calc_t *c, int row, int col) {
    if (row == 5) {
        if (col <= 1) { do_digit(c, '0'); return; }
        if (col == 2) { do_digit(c, '.'); return; }
        return;
    }
    if (row == 0) {
        if (col == 0) { c->memory = 0; c->has_mem = false; }
        else if (col == 1) { set_val(c, c->memory); c->new_input = true; }
        else if (col == 2) { c->memory = c->disp_val; c->has_mem = true; }
        else if (col == 3) { c->memory += c->disp_val; c->has_mem = true; }
        else if (col == 4) do_backspace(c);
        return;
    }
    if (row == 1) {
        if (col == 0) do_clear_entry(c);
        else if (col == 1) do_clear(c);
        else if (col == 2) do_negate(c);
        else if (col == 3) do_sqrt(c);
        else if (col == 4) do_op(c, '/');
        return;
    }
    if (col <= 2) { do_digit(c, '0' + (4 - row) * 3 + col + 1); return; }
    if (col == 3) {
        if (row == 2) do_op(c, '*');
        else if (row == 3) do_op(c, '-');
        else if (row == 4) do_op(c, '+');
        return;
    }
    if (col == 4) {
        if (row == 2) do_percent(c);
        else if (row == 3) do_reciprocal(c);
        else if (row == 4) do_equals(c);
    }
}

/*==========================================================================
 * Hit testing
 *=========================================================================*/

static bool hit_btn(calc_t *c, int16_t mx, int16_t my, int *r, int *col) {
    for (int row = 0; row < ROWS; row++) {
        for (int cc = 0; cc < COLS; cc++) {
            if (c->labels[row][cc][0] == '\0') continue;
            int16_t bx = GRID_X + cc * (BTN_W + BTN_GAP);
            int16_t by = GRID_Y + row * (BTN_H + BTN_GAP);
            int cs = btn_colspan(row, cc);
            int16_t bw = BTN_W * cs + BTN_GAP * (cs - 1);
            if (mx >= bx && mx < bx + bw && my >= by && my < by + BTN_H) {
                *r = row; *col = cc;
                return true;
            }
        }
    }
    return false;
}

/*==========================================================================
 * Paint
 *=========================================================================*/

static void calc_paint(hwnd_t hwnd) {
    window_t *win = wm_get_window(hwnd);
    if (!win || !win->user_data) return;
    calc_t *c = (calc_t *)win->user_data;

    wd_begin(hwnd);
    wd_fill_rect(0, 0, CLIENT_W, CLIENT_H, THEME_BUTTON_FACE);

    /* Sunken display */
    int16_t dw = CLIENT_W - MARGIN * 2;
    wd_hline(MARGIN, MARGIN, dw, COLOR_DARK_GRAY);
    wd_vline(MARGIN, MARGIN, DISP_H, COLOR_DARK_GRAY);
    wd_hline(MARGIN, MARGIN + DISP_H - 1, dw, COLOR_WHITE);
    wd_vline(MARGIN + dw - 1, MARGIN, DISP_H, COLOR_WHITE);
    wd_fill_rect(MARGIN + 1, MARGIN + 1, dw - 2, DISP_H - 2, COLOR_WHITE);

    if (c->has_mem)
        wd_char_ui(MARGIN + 3, MARGIN + (DISP_H - FONT_UI_HEIGHT) / 2,
                   'M', COLOR_BLACK, COLOR_WHITE);

    int tlen = strlen(c->disp);
    wd_text_ui(MARGIN + dw - 4 - tlen * FONT_UI_WIDTH,
               MARGIN + (DISP_H - FONT_UI_HEIGHT) / 2,
               c->disp, COLOR_BLACK, COLOR_WHITE);

    /* Buttons — drawn manually (wd_button has ABI issues) */
    for (int row = 0; row < ROWS; row++) {
        for (int col = 0; col < COLS; col++) {
            if (c->labels[row][col][0] == '\0') continue;
            int16_t bx = GRID_X + col * (BTN_W + BTN_GAP);
            int16_t by = GRID_Y + row * (BTN_H + BTN_GAP);
            int cs = btn_colspan(row, col);
            int16_t bw = BTN_W * cs + BTN_GAP * (cs - 1);
            bool pr = (row == c->pressed_row && col == c->pressed_col);

            if (pr) {
                wd_hline(bx, by, bw, COLOR_DARK_GRAY);
                wd_vline(bx, by, BTN_H, COLOR_DARK_GRAY);
                wd_hline(bx, by + BTN_H - 1, bw, COLOR_WHITE);
                wd_vline(bx + bw - 1, by, BTN_H, COLOR_WHITE);
            } else {
                wd_hline(bx, by, bw, COLOR_WHITE);
                wd_vline(bx, by, BTN_H, COLOR_WHITE);
                wd_hline(bx, by + BTN_H - 1, bw, COLOR_DARK_GRAY);
                wd_vline(bx + bw - 1, by, BTN_H, COLOR_DARK_GRAY);
            }
            wd_fill_rect(bx + 1, by + 1, bw - 2, BTN_H - 2, THEME_BUTTON_FACE);

            const char *lbl = c->labels[row][col];
            int llen = strlen(lbl);
            int16_t tx = bx + (bw - llen * FONT_UI_WIDTH) / 2;
            int16_t ty = by + (BTN_H - FONT_UI_HEIGHT) / 2;
            if (pr) { tx++; ty++; }
            uint8_t fg = COLOR_BLACK;
            if (row >= 1 && col >= 3) fg = COLOR_RED;
            wd_text_ui(tx, ty, lbl, fg, THEME_BUTTON_FACE);
        }
    }

    wd_end();
}

/*==========================================================================
 * Menu
 *=========================================================================*/

static void setup_menu(hwnd_t hwnd) {
    menu_bar_t bar;
    memset(&bar, 0, sizeof(bar));
    bar.menu_count = 3;

    menu_def_t *file = &bar.menus[0];
    strncpy(file->title, L(STR_FILE), sizeof(file->title) - 1);
    file->accel_key = 0x09;
    file->item_count = 1;
    strncpy(file->items[0].text, L(STR_FM_EXIT), sizeof(file->items[0].text) - 1);
    file->items[0].command_id = CMD_EXIT;

    menu_def_t *edit = &bar.menus[1];
    strncpy(edit->title, L(STR_EDIT), sizeof(edit->title) - 1);
    edit->accel_key = 0x08;
    edit->item_count = 1;
    strncpy(edit->items[0].text, L(STR_FM_COPY_MENU), sizeof(edit->items[0].text) - 1);
    edit->items[0].command_id = CMD_COPY;
    edit->items[0].accel_key = 0x06;

    menu_def_t *help = &bar.menus[2];
    strncpy(help->title, L(STR_HELP), sizeof(help->title) - 1);
    help->accel_key = 0x0B;
    help->item_count = 1;
    strncpy(help->items[0].text, L(STR_FM_ABOUT_MENU), sizeof(help->items[0].text) - 1);
    help->items[0].command_id = CMD_ABOUT;
    help->items[0].accel_key = 0x3A;

    menu_set(hwnd, &bar);
}

/*==========================================================================
 * Event handler
 *=========================================================================*/

static bool calc_event(hwnd_t hwnd, const window_event_t *ev) {
    window_t *win = wm_get_window(hwnd);
    if (!win || !win->user_data) return false;
    calc_t *c = (calc_t *)win->user_data;

    if (ev->type == WM_CLOSE) {
        win->user_data = NULL;
        app_closing = true;
        xTaskNotifyGive(app_task);
        return true;
    }

    if (ev->type == WM_COMMAND) {
        if (ev->command.id == CMD_EXIT) {
            win->user_data = NULL;
            app_closing = true;
            xTaskNotifyGive(app_task);
            return true;
        }
        if (ev->command.id == CMD_ABOUT) {
            dialog_show(hwnd, c->about_title, c->about_text,
                        DLG_ICON_INFO, DLG_BTN_OK);
            return true;
        }
        if (ev->command.id == DLG_RESULT_OK) {
            wm_invalidate(hwnd);
            return true;
        }
        return false;
    }

    if (ev->type == WM_LBUTTONDOWN) {
        int r, col;
        if (hit_btn(c, ev->mouse.x, ev->mouse.y, &r, &col)) {
            c->pressed_row = r;
            c->pressed_col = col;
            wm_invalidate(hwnd);
        }
        return true;
    }

    if (ev->type == WM_LBUTTONUP) {
        if (c->pressed_row >= 0) {
            int oldr = c->pressed_row, oldc = c->pressed_col;
            c->pressed_row = -1;
            c->pressed_col = -1;
            int r, col;
            if (hit_btn(c, ev->mouse.x, ev->mouse.y, &r, &col) &&
                r == oldr && col == oldc)
                handle_btn(c, r, col);
            wm_invalidate(hwnd);
        }
        return true;
    }

    if (ev->type == WM_CHAR) {
        char ch = ev->charev.ch;
        if (ch >= '0' && ch <= '9') do_digit(c, ch);
        else if (ch == '.') do_digit(c, '.');
        else if (ch == '+' || ch == '-' || ch == '*' || ch == '/') do_op(c, ch);
        else if (ch == '=' || ch == '\r') do_equals(c);
        wm_invalidate(hwnd);
        return true;
    }

    if (ev->type == WM_KEYDOWN) {
        if (ev->key.scancode == 0x2A) { do_backspace(c); wm_invalidate(hwnd); return true; }
        if (ev->key.scancode == 0x29) { do_clear(c); wm_invalidate(hwnd); return true; }
        if (ev->key.scancode == 0x3A) { /* F1 */
            window_event_t ce = {0}; ce.type = WM_COMMAND; ce.command.id = CMD_ABOUT;
            wm_post_event(hwnd, &ce); return true;
        }
        return false;
    }

    return false;
}

/*==========================================================================
 * Create calculator window (called from main / .text.startup)
 *=========================================================================*/

static hwnd_t calc_create(void) {
    calc_t *c = calloc(1, sizeof(calc_t));
    if (!c) return HWND_NULL;

    /* Copy const data to RAM (paint/event handlers in .text may not
     * see .rodata correctly due to ELF loader relocation issues) */
    memcpy(c->labels, labels_src, sizeof(c->labels));
    strncpy(c->about_title, AL(AL_ABOUT), sizeof(c->about_title) - 1);
    c->about_title[sizeof(c->about_title) - 1] = '\0';
    snprintf(c->about_text, sizeof(c->about_text),
             "Calculator\n\nFRANK OS v" FRANK_VERSION_STR
             "\n(c) 2026 Mikhail Matveev\n"
             "<xtreme@rh1.tech>\n"
             "github.com/rh1tech/frank-os");

    c->pressed_row = -1;
    c->pressed_col = -1;
    c->new_input = true;
    strcpy(c->disp, "0");

    int16_t fw = CLIENT_W + 2 * THEME_BORDER_WIDTH;
    int16_t fh = CLIENT_H + THEME_TITLE_HEIGHT + THEME_MENU_HEIGHT
               + 2 * THEME_BORDER_WIDTH;
    int16_t x = (DISPLAY_WIDTH - fw) / 2;
    int16_t y = (DISPLAY_HEIGHT - TASKBAR_HEIGHT - fh) / 2;
    if (y < 0) y = 0;

    hwnd_t hwnd = wm_create_window(x, y, fw, fh, "Calculator",
                                    WSTYLE_DIALOG | WF_MENUBAR,
                                    calc_event, calc_paint);
    if (hwnd == HWND_NULL) {
        free(c);
        return HWND_NULL;
    }

    c->hwnd = hwnd;

    window_t *win = wm_get_window(hwnd);
    if (win) {
        win->user_data = c;
        win->bg_color = THEME_BUTTON_FACE;
    }

    setup_menu(hwnd);
    return hwnd;
}

/*==========================================================================
 * Entry point
 *=========================================================================*/

int main(int argc, char **argv) {
    (void)argc; (void)argv;

    app_task = xTaskGetCurrentTaskHandle();
    app_closing = false;

    hwnd_t hwnd = calc_create();
    if (hwnd == HWND_NULL) return 1;

    wm_show_window(hwnd);
    wm_set_focus(hwnd);
    taskbar_invalidate();

    while (!app_closing)
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

    wm_destroy_window(hwnd);
    taskbar_invalidate();
    return 0;
}
