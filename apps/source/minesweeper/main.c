/*
 * FRANK OS — Minesweeper (standalone ELF app)
 * Copyright (c) 2025 Mikhail Matveev <xtreme@rh1.tech>
 * https://rh1.tech
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "m-os-api.h"
#include "frankos-app.h"

/* m-os-api.h ships a broken rand() that always returns 1.
 * Use a proper xorshift32 PRNG with different names. */
static uint32_t _prng_state;
static inline void ms_srand(unsigned s) { _prng_state = s ? s : 1; }
static inline int ms_rand(void) {
    uint32_t x = _prng_state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    _prng_state = x;
    return (int)(x & 0x7FFFFFFF);
}

/*==========================================================================
 * Constants
 *=========================================================================*/

#define MS_CELL_SIZE    16
#define MS_MARGIN        6
#define MS_BEVEL_W       3
#define MS_PANEL_BEVEL   2
#define MS_STATUS_H     33
#define MS_STATUS_GAP    6
#define MS_DIGIT_W      13
#define MS_DIGIT_H      23
#define MS_SMILEY_W     24
#define MS_SMILEY_H     24

#define MS_MAX_COLS     30
#define MS_MAX_ROWS     16

/* Cell encoding */
#define CELL_VALUE_MASK 0x0F
#define CELL_MINE       0x09
#define CELL_REVEALED   0x10
#define CELL_FLAGGED    0x20

/* Difficulty presets */
static const struct { uint8_t cols, rows; uint16_t mines; } ms_presets[] = {
    {  9,  9,  10 },   /* Beginner */
    { 16, 16,  40 },   /* Intermediate */
    { 30, 16,  99 },   /* Expert */
};

/* Menu command IDs */
#define CMD_NEW          1
#define CMD_BEGINNER     2
#define CMD_INTERMEDIATE 3
#define CMD_EXPERT       4
#define CMD_EXIT         5
#define CMD_ABOUT      100

/* Number colors (matching Win95) */
static const uint8_t num_colors[9] = {
    COLOR_BLACK,       /* 0 — unused */
    COLOR_LIGHT_BLUE,  /* 1 */
    COLOR_GREEN,       /* 2 */
    COLOR_LIGHT_RED,   /* 3 */
    COLOR_BLUE,        /* 4 */
    COLOR_RED,         /* 5 */
    COLOR_CYAN,        /* 6 */
    COLOR_BLACK,       /* 7 */
    COLOR_DARK_GRAY,   /* 8 */
};

/* 7-segment digit bitmasks:
 *   AAA        bit 6=A  5=B  4=C  3=D  2=E  1=F  0=G
 *  F   B
 *   GGG
 *  E   C
 *   DDD
 */
static const uint8_t seg_digits[10] = {
    0x7E, /* 0: ABCDEF  */
    0x30, /* 1: BC       */
    0x6D, /* 2: ABDEG    */
    0x79, /* 3: ABCDG    */
    0x33, /* 4: BCFG     */
    0x5B, /* 5: ACDFG    */
    0x5F, /* 6: ACDEFG   */
    0x70, /* 7: ABC      */
    0x7F, /* 8: ABCDEFG  */
    0x7B, /* 9: ABCDFG   */
};

/*==========================================================================
 * Game state
 *=========================================================================*/

typedef enum { MS_READY, MS_PLAYING, MS_WON, MS_LOST } ms_state_t;

typedef struct {
    hwnd_t        hwnd;
    uint8_t       cols, rows;
    uint16_t      mine_count;
    uint16_t      flags_placed;
    uint16_t      cells_revealed;
    uint8_t       difficulty;       /* 0=beginner, 1=intermediate, 2=expert */
    ms_state_t    state;
    bool          first_click;

    uint8_t       cells[MS_MAX_ROWS][MS_MAX_COLS];

    uint16_t      timer_seconds;
    TimerHandle_t timer_handle;

    bool          smiley_pressed;
    bool          left_down;
    bool          right_down;

    /* Cached layout (client coords) */
    int16_t       grid_x, grid_y;
    int16_t       status_x, status_y;
    int16_t       panel_w;
} minesweeper_t;

/* Task handle for blocking main() until window closes */
static void *app_task;

/*==========================================================================
 * Layout helpers
 *=========================================================================*/

static void compute_layout(minesweeper_t *ms) {
    int16_t pad = MS_MARGIN + MS_BEVEL_W + MS_PANEL_BEVEL;
    ms->grid_x = pad;
    ms->grid_y = pad + MS_STATUS_H + MS_PANEL_BEVEL + MS_STATUS_GAP + MS_PANEL_BEVEL;
    ms->status_x = pad;
    ms->status_y = pad;
    ms->panel_w = ms->cols * MS_CELL_SIZE;
}

static void get_client_size(minesweeper_t *ms, int16_t *cw, int16_t *ch) {
    int16_t grid_w = ms->cols * MS_CELL_SIZE;
    int16_t grid_h = ms->rows * MS_CELL_SIZE;
    *cw = grid_w + 22;   /* pad + grid + pad */
    *ch = grid_h + 65;
}

/* Convert client size to outer frame size (with title bar, border, menu) */
static void client_to_frame(int16_t cw, int16_t ch, int16_t *fw, int16_t *fh) {
    *fw = cw + 2 * THEME_BORDER_WIDTH;
    *fh = ch + THEME_TITLE_HEIGHT + THEME_MENU_HEIGHT + 2 * THEME_BORDER_WIDTH;
}

/*==========================================================================
 * Timer callback
 *=========================================================================*/

static void timer_callback(TimerHandle_t xTimer) {
    minesweeper_t *ms = (minesweeper_t *)pvTimerGetTimerID(xTimer);
    if (ms->state == MS_PLAYING && ms->timer_seconds < 999) {
        ms->timer_seconds++;
        wm_invalidate(ms->hwnd);
    }
}

/*==========================================================================
 * Game logic
 *=========================================================================*/

static void place_mines(minesweeper_t *ms, int safe_r, int safe_c) {
    ms_srand((unsigned)xTaskGetTickCount());
    uint16_t placed = 0;
    while (placed < ms->mine_count) {
        int r = ms_rand() % ms->rows;
        int c = ms_rand() % ms->cols;
        /* Skip safe zone (clicked cell + 8 neighbors) */
        if (abs(r - safe_r) <= 1 && abs(c - safe_c) <= 1)
            continue;
        if ((ms->cells[r][c] & CELL_VALUE_MASK) == CELL_MINE)
            continue;
        ms->cells[r][c] = (ms->cells[r][c] & ~CELL_VALUE_MASK) | CELL_MINE;
        placed++;
    }
    /* Compute adjacent counts */
    for (int r = 0; r < ms->rows; r++) {
        for (int c = 0; c < ms->cols; c++) {
            if ((ms->cells[r][c] & CELL_VALUE_MASK) == CELL_MINE)
                continue;
            int count = 0;
            for (int dr = -1; dr <= 1; dr++) {
                for (int dc = -1; dc <= 1; dc++) {
                    int nr = r + dr, nc = c + dc;
                    if (nr >= 0 && nr < ms->rows && nc >= 0 && nc < ms->cols) {
                        if ((ms->cells[nr][nc] & CELL_VALUE_MASK) == CELL_MINE)
                            count++;
                    }
                }
            }
            ms->cells[r][c] = (ms->cells[r][c] & ~CELL_VALUE_MASK) | (uint8_t)count;
        }
    }
}

static void reveal_cell(minesweeper_t *ms, int r, int c);

static void flood_fill(minesweeper_t *ms, int start_r, int start_c) {
    /* BFS queue — max 480 entries for 30x16 grid */
    static uint16_t queue[MS_MAX_COLS * MS_MAX_ROWS];
    int head = 0, tail = 0;
    queue[tail++] = (uint16_t)(start_r * MS_MAX_COLS + start_c);
    ms->cells[start_r][start_c] |= CELL_REVEALED;
    ms->cells_revealed++;

    while (head < tail) {
        uint16_t idx = queue[head++];
        int r = idx / MS_MAX_COLS;
        int c = idx % MS_MAX_COLS;
        uint8_t val = ms->cells[r][c] & CELL_VALUE_MASK;
        if (val != 0) continue; /* only expand zeros */

        for (int dr = -1; dr <= 1; dr++) {
            for (int dc = -1; dc <= 1; dc++) {
                int nr = r + dr, nc = c + dc;
                if (nr < 0 || nr >= ms->rows || nc < 0 || nc >= ms->cols)
                    continue;
                if (ms->cells[nr][nc] & (CELL_REVEALED | CELL_FLAGGED))
                    continue;
                ms->cells[nr][nc] |= CELL_REVEALED;
                ms->cells_revealed++;
                queue[tail++] = (uint16_t)(nr * MS_MAX_COLS + nc);
            }
        }
    }
}

static void reveal_cell(minesweeper_t *ms, int r, int c) {
    if (ms->cells[r][c] & (CELL_REVEALED | CELL_FLAGGED))
        return;

    uint8_t val = ms->cells[r][c] & CELL_VALUE_MASK;
    if (val == CELL_MINE) {
        /* Game over */
        ms->state = MS_LOST;
        ms->cells[r][c] |= CELL_REVEALED;
        /* Reveal all mines */
        for (int rr = 0; rr < ms->rows; rr++) {
            for (int cc = 0; cc < ms->cols; cc++) {
                uint8_t cv = ms->cells[rr][cc] & CELL_VALUE_MASK;
                if (cv == CELL_MINE)
                    ms->cells[rr][cc] |= CELL_REVEALED;
            }
        }
        if (ms->timer_handle)
            xTimerStop(ms->timer_handle, 0);
        return;
    }

    if (val == 0) {
        flood_fill(ms, r, c);
    } else {
        ms->cells[r][c] |= CELL_REVEALED;
        ms->cells_revealed++;
    }

    /* Win check */
    if (ms->cells_revealed == (uint16_t)(ms->cols * ms->rows - ms->mine_count)) {
        ms->state = MS_WON;
        if (ms->timer_handle)
            xTimerStop(ms->timer_handle, 0);
    }
}

static void chord_reveal(minesweeper_t *ms, int r, int c) {
    if (!(ms->cells[r][c] & CELL_REVEALED))
        return;
    uint8_t val = ms->cells[r][c] & CELL_VALUE_MASK;
    if (val == 0 || val >= CELL_MINE)
        return;

    /* Count adjacent flags */
    int adj_flags = 0;
    for (int dr = -1; dr <= 1; dr++) {
        for (int dc = -1; dc <= 1; dc++) {
            int nr = r + dr, nc = c + dc;
            if (nr >= 0 && nr < ms->rows && nc >= 0 && nc < ms->cols) {
                if (ms->cells[nr][nc] & CELL_FLAGGED)
                    adj_flags++;
            }
        }
    }

    if (adj_flags != val) return;

    /* Reveal all unflagged hidden neighbors */
    for (int dr = -1; dr <= 1; dr++) {
        for (int dc = -1; dc <= 1; dc++) {
            int nr = r + dr, nc = c + dc;
            if (nr >= 0 && nr < ms->rows && nc >= 0 && nc < ms->cols) {
                if (!(ms->cells[nr][nc] & (CELL_REVEALED | CELL_FLAGGED)))
                    reveal_cell(ms, nr, nc);
            }
            if (ms->state == MS_LOST) return;
        }
    }
}

static void reset_game(minesweeper_t *ms) {
    ms->cols = ms_presets[ms->difficulty].cols;
    ms->rows = ms_presets[ms->difficulty].rows;
    ms->mine_count = ms_presets[ms->difficulty].mines;
    ms->flags_placed = 0;
    ms->cells_revealed = 0;
    ms->state = MS_READY;
    ms->first_click = true;
    ms->timer_seconds = 0;
    ms->smiley_pressed = false;
    ms->left_down = false;
    ms->right_down = false;
    memset(ms->cells, 0, sizeof(ms->cells));

    if (ms->timer_handle)
        xTimerStop(ms->timer_handle, 0);

    compute_layout(ms);

    /* Resize window */
    int16_t cw, ch, fw, fh;
    get_client_size(ms, &cw, &ch);
    client_to_frame(cw, ch, &fw, &fh);

    window_t *win = wm_get_window(ms->hwnd);
    if (win) {
        /* Center on screen */
        int16_t x = (DISPLAY_WIDTH - fw) / 2;
        int16_t y = (DISPLAY_HEIGHT - TASKBAR_HEIGHT - fh) / 2;
        if (y < 0) y = 0;
        wm_set_window_rect(ms->hwnd, x, y, fw, fh);
    }

    wm_invalidate(ms->hwnd);
}

/*==========================================================================
 * Rendering — 7-segment LED digits
 *=========================================================================*/

static void draw_seg_h(int16_t x, int16_t y, int16_t w, uint8_t color) {
    /* Horizontal segment: thin rectangle */
    wd_hline(x + 1, y, w - 2, color);
}

static void draw_seg_v(int16_t x, int16_t y, int16_t h, uint8_t color) {
    /* Vertical segment: thin rectangle */
    for (int16_t i = 1; i < h - 1; i++)
        wd_pixel(x, y + i, color);
}

static void draw_digit(int16_t x, int16_t y, int digit, uint8_t fg, uint8_t bg) {
    /* Fill background */
    wd_fill_rect(x, y, MS_DIGIT_W, MS_DIGIT_H, bg);

    if (digit < 0 || digit > 9) return;
    uint8_t segs = seg_digits[digit];

    int16_t sw = MS_DIGIT_W;     /* 13 */
    int16_t sh = MS_DIGIT_H / 2; /* 11 */

    /* A: top horizontal */
    if (segs & 0x40) draw_seg_h(x + 1, y + 1, sw - 2, fg);
    /* B: top-right vertical */
    if (segs & 0x20) draw_seg_v(x + sw - 2, y + 1, sh, fg);
    /* C: bottom-right vertical */
    if (segs & 0x10) draw_seg_v(x + sw - 2, y + sh, sh, fg);
    /* D: bottom horizontal */
    if (segs & 0x08) draw_seg_h(x + 1, y + MS_DIGIT_H - 2, sw - 2, fg);
    /* E: bottom-left vertical */
    if (segs & 0x04) draw_seg_v(x + 1, y + sh, sh, fg);
    /* F: top-left vertical */
    if (segs & 0x02) draw_seg_v(x + 1, y + 1, sh, fg);
    /* G: middle horizontal */
    if (segs & 0x01) draw_seg_h(x + 1, y + sh, sw - 2, fg);
}

static void draw_counter(int16_t x, int16_t y, int value) {
    /* 3-digit 7-segment LED counter */
    /* Sunken bevel around digits */
    int16_t w = MS_DIGIT_W * 3 + 4;
    int16_t h = MS_DIGIT_H + 4;
    wd_hline(x, y, w, COLOR_DARK_GRAY);
    wd_vline(x, y, h, COLOR_DARK_GRAY);
    wd_hline(x, y + h - 1, w, COLOR_WHITE);
    wd_vline(x + w - 1, y, h, COLOR_WHITE);
    wd_fill_rect(x + 1, y + 1, w - 2, h - 2, COLOR_BLACK);

    if (value < -99) value = -99;
    if (value > 999) value = 999;

    int d2, d1, d0;
    if (value < 0) {
        /* Show minus as blank, then digits of abs */
        int av = -value;
        d2 = -1; /* blank */
        d1 = av / 10;
        d0 = av % 10;
    } else {
        d2 = value / 100;
        d1 = (value / 10) % 10;
        d0 = value % 10;
    }

    draw_digit(x + 2, y + 2, d2, COLOR_LIGHT_RED, COLOR_BLACK);
    draw_digit(x + 2 + MS_DIGIT_W, y + 2, d1, COLOR_LIGHT_RED, COLOR_BLACK);
    draw_digit(x + 2 + MS_DIGIT_W * 2, y + 2, d0, COLOR_LIGHT_RED, COLOR_BLACK);
}

/*==========================================================================
 * Rendering — smiley face button
 *=========================================================================*/

static void draw_smiley(minesweeper_t *ms) {
    int16_t panel_inner_w = ms->panel_w;
    int16_t sx = ms->status_x + (panel_inner_w - MS_SMILEY_W) / 2;
    int16_t sy = ms->status_y + (MS_STATUS_H - MS_SMILEY_H) / 2;

    /* Button bevel */
    if (ms->smiley_pressed) {
        wd_hline(sx, sy, MS_SMILEY_W, COLOR_DARK_GRAY);
        wd_vline(sx, sy, MS_SMILEY_H, COLOR_DARK_GRAY);
        wd_hline(sx, sy + MS_SMILEY_H - 1, MS_SMILEY_W, COLOR_WHITE);
        wd_vline(sx + MS_SMILEY_W - 1, sy, MS_SMILEY_H, COLOR_WHITE);
    } else {
        wd_hline(sx, sy, MS_SMILEY_W, COLOR_WHITE);
        wd_vline(sx, sy, MS_SMILEY_H, COLOR_WHITE);
        wd_hline(sx + 1, sy + MS_SMILEY_H - 1, MS_SMILEY_W - 1, COLOR_DARK_GRAY);
        wd_vline(sx + MS_SMILEY_W - 1, sy + 1, MS_SMILEY_H - 1, COLOR_DARK_GRAY);
        wd_hline(sx + 1, sy + MS_SMILEY_H - 2, MS_SMILEY_W - 2, COLOR_DARK_GRAY);
        wd_vline(sx + MS_SMILEY_W - 2, sy + 1, MS_SMILEY_H - 2, COLOR_DARK_GRAY);
    }
    wd_fill_rect(sx + 2, sy + 2, MS_SMILEY_W - 4, MS_SMILEY_H - 4,
                 THEME_BUTTON_FACE);

    /* Yellow circle (approximate) */
    int16_t cx = sx + MS_SMILEY_W / 2;
    int16_t cy = sy + MS_SMILEY_H / 2;
    /* Draw a filled circle radius ~8 */
    for (int dy = -7; dy <= 7; dy++) {
        for (int dx = -7; dx <= 7; dx++) {
            if (dx * dx + dy * dy <= 56) /* ~7.5^2 */
                wd_pixel(cx + dx, cy + dy, COLOR_YELLOW);
        }
    }

    /* Draw face based on state */
    if (ms->state == MS_LOST) {
        /* Dead: X eyes, frown */
        /* Left eye X */
        wd_pixel(cx - 3, cy - 3, COLOR_BLACK);
        wd_pixel(cx - 2, cy - 2, COLOR_BLACK);
        wd_pixel(cx - 4, cy - 2, COLOR_BLACK);
        wd_pixel(cx - 3, cy - 1, COLOR_BLACK);
        wd_pixel(cx - 2, cy - 4, COLOR_BLACK);
        wd_pixel(cx - 4, cy - 4, COLOR_BLACK);
        /* Right eye X */
        wd_pixel(cx + 3, cy - 3, COLOR_BLACK);
        wd_pixel(cx + 2, cy - 2, COLOR_BLACK);
        wd_pixel(cx + 4, cy - 2, COLOR_BLACK);
        wd_pixel(cx + 3, cy - 1, COLOR_BLACK);
        wd_pixel(cx + 2, cy - 4, COLOR_BLACK);
        wd_pixel(cx + 4, cy - 4, COLOR_BLACK);
        /* Frown */
        wd_pixel(cx - 3, cy + 4, COLOR_BLACK);
        wd_pixel(cx - 2, cy + 3, COLOR_BLACK);
        wd_pixel(cx - 1, cy + 3, COLOR_BLACK);
        wd_pixel(cx,     cy + 3, COLOR_BLACK);
        wd_pixel(cx + 1, cy + 3, COLOR_BLACK);
        wd_pixel(cx + 2, cy + 3, COLOR_BLACK);
        wd_pixel(cx + 3, cy + 4, COLOR_BLACK);
    } else if (ms->state == MS_WON) {
        /* Cool: sunglasses, smile */
        /* Sunglasses bar */
        wd_hline(cx - 5, cy - 3, 11, COLOR_BLACK);
        /* Left lens */
        wd_fill_rect(cx - 5, cy - 3, 4, 3, COLOR_BLACK);
        /* Right lens */
        wd_fill_rect(cx + 2, cy - 3, 4, 3, COLOR_BLACK);
        /* Smile */
        wd_pixel(cx - 3, cy + 2, COLOR_BLACK);
        wd_pixel(cx - 2, cy + 3, COLOR_BLACK);
        wd_pixel(cx - 1, cy + 3, COLOR_BLACK);
        wd_pixel(cx,     cy + 3, COLOR_BLACK);
        wd_pixel(cx + 1, cy + 3, COLOR_BLACK);
        wd_pixel(cx + 2, cy + 3, COLOR_BLACK);
        wd_pixel(cx + 3, cy + 2, COLOR_BLACK);
    } else if (ms->left_down && ms->state == MS_PLAYING) {
        /* Surprised: dot eyes, O mouth */
        wd_pixel(cx - 3, cy - 2, COLOR_BLACK);
        wd_pixel(cx + 3, cy - 2, COLOR_BLACK);
        /* O mouth */
        wd_pixel(cx,     cy + 2, COLOR_BLACK);
        wd_pixel(cx - 1, cy + 3, COLOR_BLACK);
        wd_pixel(cx + 1, cy + 3, COLOR_BLACK);
        wd_pixel(cx,     cy + 4, COLOR_BLACK);
    } else {
        /* Normal: dot eyes, smile */
        wd_pixel(cx - 3, cy - 2, COLOR_BLACK);
        wd_pixel(cx + 3, cy - 2, COLOR_BLACK);
        /* Smile */
        wd_pixel(cx - 3, cy + 2, COLOR_BLACK);
        wd_pixel(cx - 2, cy + 3, COLOR_BLACK);
        wd_pixel(cx - 1, cy + 3, COLOR_BLACK);
        wd_pixel(cx,     cy + 3, COLOR_BLACK);
        wd_pixel(cx + 1, cy + 3, COLOR_BLACK);
        wd_pixel(cx + 2, cy + 3, COLOR_BLACK);
        wd_pixel(cx + 3, cy + 2, COLOR_BLACK);
    }
}

/*==========================================================================
 * Rendering — cells
 *=========================================================================*/

static void draw_mine_icon(int16_t cx, int16_t cy) {
    /* Black circle + radiating lines */
    for (int dy = -2; dy <= 2; dy++)
        for (int dx = -2; dx <= 2; dx++)
            if (dx * dx + dy * dy <= 5)
                wd_pixel(cx + dx, cy + dy, COLOR_BLACK);
    /* Lines */
    wd_pixel(cx, cy - 4, COLOR_BLACK);
    wd_pixel(cx, cy + 4, COLOR_BLACK);
    wd_pixel(cx - 4, cy, COLOR_BLACK);
    wd_pixel(cx + 4, cy, COLOR_BLACK);
    wd_pixel(cx - 3, cy - 3, COLOR_BLACK);
    wd_pixel(cx + 3, cy - 3, COLOR_BLACK);
    wd_pixel(cx - 3, cy + 3, COLOR_BLACK);
    wd_pixel(cx + 3, cy + 3, COLOR_BLACK);
    /* Highlight dot */
    wd_pixel(cx - 1, cy - 1, COLOR_WHITE);
}

static void draw_flag_icon(int16_t x, int16_t y) {
    /* Red flag at cell origin */
    int16_t fx = x + 6, fy = y + 2;
    /* Flag pole */
    wd_vline(fx + 1, fy, 10, COLOR_BLACK);
    /* Flag triangle */
    for (int row = 0; row < 5; row++) {
        for (int col = 0; col < 5 - row; col++) {
            wd_pixel(fx - col, fy + row, COLOR_LIGHT_RED);
        }
    }
    /* Base */
    wd_hline(fx - 2, fy + 10, 6, COLOR_BLACK);
    wd_hline(fx - 3, fy + 11, 8, COLOR_BLACK);
}

static void draw_cell(minesweeper_t *ms, int r, int c) {
    int16_t x = ms->grid_x + c * MS_CELL_SIZE;
    int16_t y = ms->grid_y + r * MS_CELL_SIZE;
    uint8_t cell = ms->cells[r][c];
    uint8_t val = cell & CELL_VALUE_MASK;
    bool revealed = (cell & CELL_REVEALED) != 0;
    bool flagged = (cell & CELL_FLAGGED) != 0;

    if (!revealed && !flagged) {
        /* Hidden: raised bevel */
        wd_bevel_rect(x, y, MS_CELL_SIZE, MS_CELL_SIZE,
                      COLOR_WHITE, COLOR_DARK_GRAY, THEME_BUTTON_FACE);
    } else if (!revealed && flagged) {
        /* Flagged: raised bevel + flag */
        wd_bevel_rect(x, y, MS_CELL_SIZE, MS_CELL_SIZE,
                      COLOR_WHITE, COLOR_DARK_GRAY, THEME_BUTTON_FACE);
        if (ms->state == MS_LOST && val != CELL_MINE) {
            /* Wrong flag on loss: show mine + red X */
            wd_fill_rect(x + 1, y + 1, MS_CELL_SIZE - 2, MS_CELL_SIZE - 2,
                         THEME_BUTTON_FACE);
            draw_mine_icon(x + MS_CELL_SIZE / 2, y + MS_CELL_SIZE / 2);
            /* Red X */
            for (int i = 0; i < MS_CELL_SIZE - 4; i++) {
                wd_pixel(x + 2 + i, y + 2 + i, COLOR_LIGHT_RED);
                wd_pixel(x + MS_CELL_SIZE - 3 - i, y + 2 + i, COLOR_LIGHT_RED);
            }
        } else {
            draw_flag_icon(x, y);
        }
    } else {
        /* Revealed */
        /* Flat: light gray fill + dark gray top/left edge */
        wd_fill_rect(x, y, MS_CELL_SIZE, MS_CELL_SIZE, THEME_BUTTON_FACE);
        wd_hline(x, y, MS_CELL_SIZE, COLOR_DARK_GRAY);
        wd_vline(x, y, MS_CELL_SIZE, COLOR_DARK_GRAY);

        if (val == CELL_MINE) {
            if (ms->state == MS_LOST && r >= 0) {
                wd_fill_rect(x + 1, y + 1, MS_CELL_SIZE - 1, MS_CELL_SIZE - 1,
                             COLOR_LIGHT_RED);
            }
            draw_mine_icon(x + MS_CELL_SIZE / 2, y + MS_CELL_SIZE / 2);
        } else if (val > 0 && val <= 8) {
            char ch = '0' + val;
            /* Center the 6x12 UI font character in 16x16 cell */
            int16_t tx = x + (MS_CELL_SIZE - FONT_UI_WIDTH) / 2;
            int16_t ty = y + (MS_CELL_SIZE - FONT_UI_HEIGHT) / 2;
            uint8_t bg = THEME_BUTTON_FACE;
            /* Draw the digit char with flat background */
            wd_fill_rect(x + 1, y + 1, MS_CELL_SIZE - 1, MS_CELL_SIZE - 1, bg);
            wd_hline(x, y, MS_CELL_SIZE, COLOR_DARK_GRAY);
            wd_vline(x, y, MS_CELL_SIZE, COLOR_DARK_GRAY);
            wd_char_ui(tx, ty, ch, num_colors[val], bg);
        }
    }
}

/*==========================================================================
 * Rendering — main paint
 *=========================================================================*/

static void draw_sunken_panel(int16_t x, int16_t y, int16_t w, int16_t h) {
    /* 2px sunken bevel */
    wd_hline(x, y, w, COLOR_DARK_GRAY);
    wd_vline(x, y, h, COLOR_DARK_GRAY);
    wd_hline(x + 1, y + 1, w - 2, COLOR_BLACK);
    wd_vline(x + 1, y + 1, h - 2, COLOR_BLACK);
    wd_hline(x + 1, y + h - 1, w - 1, COLOR_WHITE);
    wd_vline(x + w - 1, y + 1, h - 1, COLOR_WHITE);
    wd_hline(x + 1, y + h - 2, w - 2, THEME_BUTTON_FACE);
    wd_vline(x + w - 2, y + 1, h - 2, THEME_BUTTON_FACE);
}

static void draw_raised_border(int16_t x, int16_t y, int16_t w, int16_t h) {
    /* 3px outer raised bevel */
    for (int i = 0; i < MS_BEVEL_W; i++) {
        wd_hline(x + i, y + i, w - 2 * i, COLOR_WHITE);
        wd_vline(x + i, y + i, h - 2 * i, COLOR_WHITE);
        wd_hline(x + i, y + h - 1 - i, w - 2 * i, COLOR_DARK_GRAY);
        wd_vline(x + w - 1 - i, y + i, h - 2 * i, COLOR_DARK_GRAY);
    }
}

static void ms_paint(hwnd_t hwnd) {
    window_t *win = wm_get_window(hwnd);
    if (!win) return;
    minesweeper_t *ms = (minesweeper_t *)win->user_data;
    if (!ms) return;

    wd_begin(hwnd);

    /* Client area background */
    wd_clear(THEME_BUTTON_FACE);

    int16_t cw, ch;
    get_client_size(ms, &cw, &ch);

    /* Outer raised bevel */
    draw_raised_border(MS_MARGIN, MS_MARGIN,
                       cw - 2 * MS_MARGIN, ch - 2 * MS_MARGIN);

    /* Status panel (sunken) */
    int16_t sp_x = MS_MARGIN + MS_BEVEL_W;
    int16_t sp_y = MS_MARGIN + MS_BEVEL_W;
    int16_t sp_w = ms->panel_w + 2 * MS_PANEL_BEVEL;
    int16_t sp_h = MS_STATUS_H + 2 * MS_PANEL_BEVEL;
    draw_sunken_panel(sp_x, sp_y, sp_w, sp_h);
    wd_fill_rect(sp_x + MS_PANEL_BEVEL, sp_y + MS_PANEL_BEVEL,
                 ms->panel_w, MS_STATUS_H, THEME_BUTTON_FACE);

    /* Mine counter (left) */
    int mine_display = (int)ms->mine_count - (int)ms->flags_placed;
    draw_counter(ms->status_x + 3, ms->status_y + 3, mine_display);

    /* Timer (right) */
    draw_counter(ms->status_x + ms->panel_w - 3 - (MS_DIGIT_W * 3 + 4),
                 ms->status_y + 3, (int)ms->timer_seconds);

    /* Smiley button */
    draw_smiley(ms);

    /* Grid panel (sunken) */
    int16_t gp_x = MS_MARGIN + MS_BEVEL_W;
    int16_t gp_y = ms->grid_y - MS_PANEL_BEVEL;
    int16_t gp_w = ms->panel_w + 2 * MS_PANEL_BEVEL;
    int16_t gp_h = ms->rows * MS_CELL_SIZE + 2 * MS_PANEL_BEVEL;
    draw_sunken_panel(gp_x, gp_y, gp_w, gp_h);

    /* Draw cells */
    for (int r = 0; r < ms->rows; r++)
        for (int c = 0; c < ms->cols; c++)
            draw_cell(ms, r, c);

    wd_end();
}

/*==========================================================================
 * Hit testing
 *=========================================================================*/

static bool hit_smiley(minesweeper_t *ms, int16_t mx, int16_t my) {
    int16_t sx = ms->status_x + (ms->panel_w - MS_SMILEY_W) / 2;
    int16_t sy = ms->status_y + (MS_STATUS_H - MS_SMILEY_H) / 2;
    return mx >= sx && mx < sx + MS_SMILEY_W &&
           my >= sy && my < sy + MS_SMILEY_H;
}

static bool hit_grid(minesweeper_t *ms, int16_t mx, int16_t my,
                     int *out_r, int *out_c) {
    int16_t lx = mx - ms->grid_x;
    int16_t ly = my - ms->grid_y;
    if (lx < 0 || ly < 0) return false;
    int c = lx / MS_CELL_SIZE;
    int r = ly / MS_CELL_SIZE;
    if (c >= ms->cols || r >= ms->rows) return false;
    *out_r = r;
    *out_c = c;
    return true;
}

/*==========================================================================
 * Event handler
 *=========================================================================*/

static bool ms_event(hwnd_t hwnd, const window_event_t *ev) {
    window_t *win = wm_get_window(hwnd);
    if (!win || !win->user_data) return false;
    minesweeper_t *ms = (minesweeper_t *)win->user_data;

    if (ev->type == WM_LBUTTONDOWN) {
        int16_t mx = ev->mouse.x, my = ev->mouse.y;
        if (hit_smiley(ms, mx, my)) {
            ms->smiley_pressed = true;
            wm_invalidate(hwnd);
            return true;
        }
        int r, c;
        if (hit_grid(ms, mx, my, &r, &c)) {
            ms->left_down = true;
            if (ms->state != MS_LOST && ms->state != MS_WON)
                wm_invalidate(hwnd); /* show surprised face */
        }
        return true;
    }

    if (ev->type == WM_LBUTTONUP) {
        int16_t mx = ev->mouse.x, my = ev->mouse.y;

        if (ms->smiley_pressed) {
            ms->smiley_pressed = false;
            if (hit_smiley(ms, mx, my))
                reset_game(ms);
            else
                wm_invalidate(hwnd);
            return true;
        }

        if (ms->left_down) {
            ms->left_down = false;
            int r, c;
            if (hit_grid(ms, mx, my, &r, &c)) {
                if (ms->right_down) {
                    /* Chord click */
                    if (ms->state == MS_PLAYING)
                        chord_reveal(ms, r, c);
                } else if (ms->state != MS_LOST && ms->state != MS_WON) {
                    if (ms->first_click) {
                        ms->first_click = false;
                        ms->state = MS_PLAYING;
                        place_mines(ms, r, c);
                        if (ms->timer_handle)
                            xTimerStart(ms->timer_handle, 0);
                    }
                    reveal_cell(ms, r, c);
                }
            }
            wm_invalidate(hwnd);
        }
        return true;
    }

    if (ev->type == WM_RBUTTONDOWN) {
        int16_t mx = ev->mouse.x, my = ev->mouse.y;
        ms->right_down = true;
        int r, c;
        if (hit_grid(ms, mx, my, &r, &c)) {
            if (ms->state != MS_LOST && ms->state != MS_WON &&
                !(ms->cells[r][c] & CELL_REVEALED)) {
                if (ms->cells[r][c] & CELL_FLAGGED) {
                    ms->cells[r][c] &= ~CELL_FLAGGED;
                    ms->flags_placed--;
                } else {
                    ms->cells[r][c] |= CELL_FLAGGED;
                    ms->flags_placed++;
                }
                wm_invalidate(hwnd);
            }
        }
        return true;
    }

    if (ev->type == WM_RBUTTONUP) {
        if (ms->left_down && ms->right_down) {
            /* Chord on button-up */
            int16_t mx = ev->mouse.x, my = ev->mouse.y;
            int r, c;
            if (hit_grid(ms, mx, my, &r, &c) && ms->state == MS_PLAYING) {
                chord_reveal(ms, r, c);
                wm_invalidate(hwnd);
            }
        }
        ms->right_down = false;
        return true;
    }

    if (ev->type == WM_COMMAND) {
        if (ev->command.id == CMD_NEW) {
            reset_game(ms);
            return true;
        }
        if (ev->command.id == CMD_BEGINNER) {
            ms->difficulty = 0;
            reset_game(ms);
            return true;
        }
        if (ev->command.id == CMD_INTERMEDIATE) {
            ms->difficulty = 1;
            reset_game(ms);
            return true;
        }
        if (ev->command.id == CMD_EXPERT) {
            ms->difficulty = 2;
            reset_game(ms);
            return true;
        }
        if (ev->command.id == CMD_EXIT) {
            window_event_t ce = {0};
            ce.type = WM_CLOSE;
            wm_post_event(hwnd, &ce);
            return true;
        }
        if (ev->command.id == CMD_ABOUT) {
            dialog_show(hwnd, "About Minesweeper",
                        "Minesweeper\n\nFRANK OS v" FRANK_VERSION_STR
                        " (c) 2026\nMikhail Matveev",
                        DLG_ICON_INFO, DLG_BTN_OK);
            return true;
        }
        return false;
    }

    if (ev->type == WM_KEYDOWN) {
        /* F2 = new game (HID code 0x3B) */
        if (ev->key.scancode == 0x3B) {
            reset_game(ms);
            return true;
        }
        return false;
    }

    if (ev->type == WM_CLOSE) {
        if (ms->timer_handle) {
            xTimerStop(ms->timer_handle, 0);
            xTimerDelete(ms->timer_handle, 0);
            ms->timer_handle = (TimerHandle_t)0;
        }
        win->user_data = (void*)0;
        /* Wake main() — it will do the heavyweight cleanup.
         * We must NOT call wm_destroy_window/free here because
         * main() returning causes the ELF loader to free our code,
         * and we'd still be executing from it. */
        xTaskNotifyGive(app_task);
        return true;
    }

    return false;
}

/*==========================================================================
 * Menu setup
 *=========================================================================*/

static void setup_menu(hwnd_t hwnd) {
    menu_bar_t bar;
    memset(&bar, 0, sizeof(bar));
    bar.menu_count = 2;

    /* Game menu */
    menu_def_t *game = &bar.menus[0];
    strncpy(game->title, "Game", sizeof(game->title) - 1);
    game->accel_key = 0x0A; /* HID 'G' */
    game->item_count = 7;

    strncpy(game->items[0].text, "New", sizeof(game->items[0].text) - 1);
    game->items[0].command_id = CMD_NEW;
    game->items[0].accel_key = 0x3B; /* F2 */

    game->items[1].flags = MIF_SEPARATOR;

    strncpy(game->items[2].text, "Beginner", sizeof(game->items[2].text) - 1);
    game->items[2].command_id = CMD_BEGINNER;

    strncpy(game->items[3].text, "Intermediate", sizeof(game->items[3].text) - 1);
    game->items[3].command_id = CMD_INTERMEDIATE;

    strncpy(game->items[4].text, "Expert", sizeof(game->items[4].text) - 1);
    game->items[4].command_id = CMD_EXPERT;

    game->items[5].flags = MIF_SEPARATOR;

    strncpy(game->items[6].text, "Exit", sizeof(game->items[6].text) - 1);
    game->items[6].command_id = CMD_EXIT;

    /* Help menu */
    menu_def_t *help = &bar.menus[1];
    strncpy(help->title, "Help", sizeof(help->title) - 1);
    help->accel_key = 0x0B; /* HID 'H' */
    help->item_count = 1;

    strncpy(help->items[0].text, "About", sizeof(help->items[0].text) - 1);
    help->items[0].command_id = CMD_ABOUT;

    menu_set(hwnd, &bar);
}

/*==========================================================================
 * Window creation
 *=========================================================================*/

static hwnd_t minesweeper_create(void) {
    minesweeper_t *ms = calloc(1, sizeof(minesweeper_t));
    if (!ms) return HWND_NULL;

    ms->difficulty = 0; /* beginner */
    ms->cols = ms_presets[0].cols;
    ms->rows = ms_presets[0].rows;
    ms->mine_count = ms_presets[0].mines;
    ms->state = MS_READY;
    ms->first_click = true;
    compute_layout(ms);

    int16_t cw, ch, fw, fh;
    get_client_size(ms, &cw, &ch);
    client_to_frame(cw, ch, &fw, &fh);

    int16_t x = (DISPLAY_WIDTH - fw) / 2;
    int16_t y = (DISPLAY_HEIGHT - TASKBAR_HEIGHT - fh) / 2;
    if (y < 0) y = 0;

    hwnd_t hwnd = wm_create_window(x, y, fw, fh, "Minesweeper",
                                    WSTYLE_DIALOG | WF_MENUBAR,
                                    ms_event, ms_paint);
    if (hwnd == HWND_NULL) {
        free(ms);
        return HWND_NULL;
    }

    ms->hwnd = hwnd;

    window_t *win = wm_get_window(hwnd);
    if (win) {
        win->user_data = ms;
        win->bg_color = THEME_BUTTON_FACE;
    }

    setup_menu(hwnd);

    /* Create timer (1 second interval) */
    ms->timer_handle = xTimerCreate("ms_tmr", pdMS_TO_TICKS(1000),
                                     pdTRUE, (void *)ms, timer_callback);

    wm_show_window(hwnd);
    return hwnd;
}

/*==========================================================================
 * Entry point
 *=========================================================================*/

int main(int argc, char **argv) {
    (void)argc;
    (void)argv;

    app_task = xTaskGetCurrentTaskHandle();

    hwnd_t hwnd = minesweeper_create();
    if (hwnd == HWND_NULL)
        return 1;

    wm_set_focus(hwnd);
    taskbar_invalidate();

    /* Block until the window is closed */
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

    /* Cleanup — safe here because our code is still alive until main returns */
    window_t *win = wm_get_window(hwnd);
    void *ms = win ? win->user_data : (void*)0;
    wm_destroy_window(hwnd);
    if (ms) free(ms);
    taskbar_invalidate();

    return 0;
}
