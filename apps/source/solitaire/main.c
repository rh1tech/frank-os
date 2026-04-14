/*
 * FRANK OS — Solitaire (Klondike) (standalone ELF app)
 * Copyright (c) 2026 Mikhail Matveev <xtreme@rh1.tech>
 * https://rh1.tech
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "m-os-api.h"
#include "frankos-app.h"
#include "lang.h"

/* App-local translations */
enum { AL_NEW_GAME, AL_DRAW_ONE, AL_DRAW_THREE, AL_CONGRATS, AL_CONGRATS_MSG, AL_ABOUT, AL_GAME, AL_COUNT };
static const char *al_en[] = {
    [AL_NEW_GAME]    = "New Game   F2",
    [AL_DRAW_ONE]    = "Draw One",
    [AL_DRAW_THREE]  = "Draw Three",
    [AL_CONGRATS]    = "Congratulations!",
    [AL_CONGRATS_MSG]= "You won!",
    [AL_ABOUT]       = "About Solitaire",
    [AL_GAME]        = "Game",
};
static const char *al_ru[] = {
    [AL_NEW_GAME]    = "\xD0\x9D\xD0\xBE\xD0\xB2\xD0\xB0\xD1\x8F \xD0\xB8\xD0\xB3\xD1\x80\xD0\xB0 F2",
    [AL_DRAW_ONE]    = "\xD0\x9F\xD0\xBE \xD0\xBE\xD0\xB4\xD0\xBD\xD0\xBE\xD0\xB9",
    [AL_DRAW_THREE]  = "\xD0\x9F\xD0\xBE \xD1\x82\xD1\x80\xD0\xB8",
    [AL_CONGRATS]    = "\xD0\x9F\xD0\xBE\xD0\xB7\xD0\xB4\xD1\x80\xD0\xB0\xD0\xB2\xD0\xBB\xD1\x8F\xD0\xB5\xD0\xBC!",
    [AL_CONGRATS_MSG]= "\xD0\x92\xD1\x8B \xD0\xB2\xD1\x8B\xD0\xB8\xD0\xB3\xD1\x80\xD0\xB0\xD0\xBB\xD0\xB8!",
    [AL_ABOUT]       = "\xD0\x9E \xD0\x9F\xD0\xB0\xD1\x81\xD1\x8C\xD1\x8F\xD0\xBD\xD1\x81\xD0\xB5",
    [AL_GAME]        = "\xD0\x98\xD0\xB3\xD1\x80\xD0\xB0",
};
static const char *AL(int id) { return lang_get() == LANG_RU ? al_ru[id] : al_en[id]; }

/* UART debug printf */
#define dbg_printf(...) ((int(*)(const char*, ...))_sys_table_ptrs[438])(__VA_ARGS__)

/* m-os-api.h ships a broken rand() that always returns 1.
 * Use a proper xorshift32 PRNG with different names. */
static uint32_t _prng_state;
static inline void sol_srand(unsigned s) { _prng_state = s ? s : 1; }
static inline int sol_rand(void) {
    uint32_t x = _prng_state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    _prng_state = x;
    return (int)(x & 0x7FFFFFFF);
}

/*==========================================================================
 * Card encoding
 *=========================================================================*/

#define CARD_EMPTY  0xFF

#define CARD_RANK(c)       ((c) & 0x0F)
#define CARD_SUIT(c)       (((c) >> 4) & 0x03)
#define MAKE_CARD(s, r)    ((uint8_t)(((s) << 4) | (r)))

/* Suits: 0=Spades, 1=Hearts, 2=Diamonds, 3=Clubs */
#define SUIT_SPADES   0
#define SUIT_HEARTS   1
#define SUIT_DIAMONDS 2
#define SUIT_CLUBS    3

/* Red suits: Hearts(1), Diamonds(2) */
static inline bool card_is_red(uint8_t c) {
    uint8_t s = CARD_SUIT(c);
    return (s == SUIT_HEARTS || s == SUIT_DIAMONDS);
}

static const char *rank_str[] = {
    "A","2","3","4","5","6","7","8","9","10","J","Q","K"
};

/*==========================================================================
 * Layout constants
 *=========================================================================*/

#define CARD_W          45
#define CARD_H          60
#define COL_GAP         12
#define TOP_MARGIN       8
#define LEFT_MARGIN     12
#define ROW_GAP         16
#define CASCADE_DOWN     3   /* face-down vertical offset */
#define CASCADE_UP      14   /* face-up vertical offset */
#define STATUS_H        16   /* status bar height at bottom */

#define CLIENT_W       411   /* 7*45 + 6*12 + 2*12 (incl right margin) */
#define CLIENT_H       290

/* Top row Y */
#define TOP_ROW_Y       TOP_MARGIN

/* Tableau Y */
#define TAB_Y           (TOP_MARGIN + CARD_H + ROW_GAP)

/* Column X helper */
#define COL_X(col)      (LEFT_MARGIN + (col) * (CARD_W + COL_GAP))

/* Source IDs for selection */
#define SEL_NONE        (-1)
#define SEL_WASTE        7
#define SEL_FOUND_BASE   8   /* 8,9,10,11 = four foundations */

/* Menu command IDs */
#define CMD_NEW          1
#define CMD_DRAW_ONE     2
#define CMD_DRAW_THREE   3
#define CMD_EXIT         4
#define CMD_ABOUT        5

/*==========================================================================
 * Suit symbol bitmaps (5x5 pixel art)
 *=========================================================================*/

/* Each suit is a 5x7 bitmap packed as 7 bytes, each byte = 5 bits (MSB first) */
static const uint8_t suit_bmp_spades[] = {
    0x04, /* ..#.. */
    0x0E, /* .###. */
    0x1F, /* ##### */
    0x1F, /* ##### */
    0x0E, /* .###. */
    0x04, /* ..#.. */
    0x0E, /* .###. */
};

static const uint8_t suit_bmp_hearts[] = {
    0x0A, /* .#.#. */
    0x1F, /* ##### */
    0x1F, /* ##### */
    0x1F, /* ##### */
    0x0E, /* .###. */
    0x04, /* ..#.. */
    0x00, /* ..... */
};

static const uint8_t suit_bmp_diamonds[] = {
    0x04, /* ..#.. */
    0x0E, /* .###. */
    0x1F, /* ##### */
    0x0E, /* .###. */
    0x04, /* ..#.. */
    0x00, /* ..... */
    0x00, /* ..... */
};

static const uint8_t suit_bmp_clubs[] = {
    0x04, /* ..#.. */
    0x0E, /* .###. */
    0x04, /* ..#.. */
    0x1B, /* ##.## */
    0x1F, /* ##### */
    0x04, /* ..#.. */
    0x0E, /* .###. */
};

static const uint8_t *suit_bmps[] = {
    suit_bmp_spades, suit_bmp_hearts, suit_bmp_diamonds, suit_bmp_clubs
};

/*==========================================================================
 * Direct framebuffer rendering (bypasses compositor for smooth drag)
 *
 * wd_fb_ptr() in sol_paint returns a pointer into the draw buffer.
 * After display_swap_buffers() that memory IS the show buffer —
 * the one DVI hardware continuously scans.  Writing to it is instant.
 *
 * Pixel format: 4-bit packed, 2 px/byte.
 *   even x → high nibble (bits 7-4)
 *   odd  x → low  nibble (bits 3-0)
 * stride = FB_STRIDE = 320 bytes/line.
 *=========================================================================*/

/* Visible (clipped) client dimensions — set each frame in sol_paint().
 * When the window extends past the right/bottom screen edge, vis_w/vis_h
 * are smaller than CLIENT_W/CLIENT_H, preventing scanline overflow. */
static int16_t vis_w = CLIENT_W;
static int16_t vis_h = CLIENT_H;

static inline void fb_px(uint8_t *base, int16_t stride,
                          int16_t x, int16_t y, uint8_t c) {
    if (x < 0 || x >= vis_w || y < 0 || y >= vis_h) return;
    uint8_t *p = base + y * stride + (x >> 1);
    if (x & 1)
        *p = (*p & 0xF0) | (c & 0x0F);
    else
        *p = (*p & 0x0F) | ((c & 0x0F) << 4);
}

static void fb_hline(uint8_t *base, int16_t stride,
                      int16_t x, int16_t y, int16_t w, uint8_t c) {
    if (y < 0 || y >= vis_h || w <= 0) return;
    if (x < 0) { w += x; x = 0; }
    if (x + w > vis_w) w = vis_w - x;
    if (w <= 0) return;
    uint8_t *row = base + y * stride;
    int16_t px = x, end = x + w;
    if (px & 1) {
        uint8_t *p = row + (px >> 1);
        *p = (*p & 0xF0) | (c & 0x0F);
        px++;
    }
    uint8_t dc = ((c & 0x0F) << 4) | (c & 0x0F);
    int nbytes = (end - px) >> 1;
    if (nbytes > 0)
        memset(row + (px >> 1), dc, nbytes);
    px += nbytes << 1;
    if (px < end) {
        uint8_t *p = row + (px >> 1);
        *p = (*p & 0x0F) | ((c & 0x0F) << 4);
    }
}

static void fb_vline(uint8_t *base, int16_t stride,
                      int16_t x, int16_t y, int16_t h, uint8_t c) {
    if (x < 0 || x >= vis_w || h <= 0) return;
    if (y < 0) { h += y; y = 0; }
    if (y + h > vis_h) h = vis_h - y;
    int off = x >> 1;
    if (x & 1) {
        uint8_t mask_keep = 0xF0;
        uint8_t mask_set  = c & 0x0F;
        for (int16_t r = 0; r < h; r++) {
            uint8_t *p = base + (y + r) * stride + off;
            *p = (*p & mask_keep) | mask_set;
        }
    } else {
        uint8_t mask_keep = 0x0F;
        uint8_t mask_set  = (c & 0x0F) << 4;
        for (int16_t r = 0; r < h; r++) {
            uint8_t *p = base + (y + r) * stride + off;
            *p = (*p & mask_keep) | mask_set;
        }
    }
}

static void fb_fill(uint8_t *base, int16_t stride,
                     int16_t x, int16_t y, int16_t w, int16_t h, uint8_t c) {
    for (int16_t r = 0; r < h; r++)
        fb_hline(base, stride, x, y + r, w, c);
}

/* 4×6 mini-font for rank characters (direct FB rendering) */
static const uint8_t mini_glyphs[][6] = {
    /* [0] '0' */ {0x6, 0x9, 0x9, 0x9, 0x6, 0x0},
    /* [1] '1' */ {0x2, 0x6, 0x2, 0x2, 0x7, 0x0},
    /* [2] '2' */ {0x6, 0x9, 0x2, 0x4, 0xF, 0x0},
    /* [3] '3' */ {0xE, 0x1, 0x6, 0x1, 0xE, 0x0},
    /* [4] '4' */ {0x9, 0x9, 0xF, 0x1, 0x1, 0x0},
    /* [5] '5' */ {0xF, 0x8, 0xE, 0x1, 0xE, 0x0},
    /* [6] '6' */ {0x6, 0x8, 0xE, 0x9, 0x6, 0x0},
    /* [7] '7' */ {0xF, 0x1, 0x2, 0x2, 0x2, 0x0},
    /* [8] '8' */ {0x6, 0x9, 0x6, 0x9, 0x6, 0x0},
    /* [9] '9' */ {0x6, 0x9, 0x7, 0x1, 0x6, 0x0},
    /* [10] 'A' */ {0x6, 0x9, 0xF, 0x9, 0x9, 0x0},
    /* [11] 'J' */ {0x1, 0x1, 0x1, 0x9, 0x6, 0x0},
    /* [12] 'Q' */ {0x6, 0x9, 0x9, 0x6, 0x1, 0x0},
    /* [13] 'K' */ {0x9, 0xA, 0xC, 0xA, 0x9, 0x0},
};

static int mini_idx(char ch) {
    if (ch >= '0' && ch <= '9') return ch - '0';
    if (ch == 'A') return 10;
    if (ch == 'J') return 11;
    if (ch == 'Q') return 12;
    if (ch == 'K') return 13;
    return -1;
}

static void fb_glyph(uint8_t *base, int16_t stride,
                      int16_t x, int16_t y, int idx, uint8_t c) {
    if (idx < 0) return;
    const uint8_t *g = mini_glyphs[idx];
    for (int r = 0; r < 6; r++)
        for (int col = 0; col < 4; col++)
            if (g[r] & (1 << (3 - col)))
                fb_px(base, stride, x + col, y + r, c);
}

static void fb_suit(uint8_t *base, int16_t stride,
                     int16_t x, int16_t y, uint8_t suit, uint8_t c) {
    const uint8_t *bmp = suit_bmps[suit];
    for (int r = 0; r < 7; r++)
        for (int col = 0; col < 5; col++)
            if (bmp[r] & (1 << (4 - col)))
                fb_px(base, stride, x + col, y + r, c);
}

static void fb_card_face(uint8_t *base, int16_t stride,
                          int16_t x, int16_t y, uint8_t card) {
    /* White fill */
    fb_fill(base, stride, x + 1, y + 1, CARD_W - 2, CARD_H - 2, COLOR_WHITE);
    /* Black border */
    fb_hline(base, stride, x + 1, y, CARD_W - 2, COLOR_BLACK);
    fb_hline(base, stride, x + 1, y + CARD_H - 1, CARD_W - 2, COLOR_BLACK);
    fb_vline(base, stride, x, y + 1, CARD_H - 2, COLOR_BLACK);
    fb_vline(base, stride, x + CARD_W - 1, y + 1, CARD_H - 2, COLOR_BLACK);

    uint8_t suit = CARD_SUIT(card);
    uint8_t rank = CARD_RANK(card);
    uint8_t clr = card_is_red(card) ? COLOR_RED : COLOR_BLACK;

    /* Rank text top-left */
    const char *rs = rank_str[rank];
    int16_t tx = x + 3;
    while (*rs) {
        fb_glyph(base, stride, tx, y + 3, mini_idx(*rs), clr);
        tx += 5;
        rs++;
    }

    /* Suit symbol below rank */
    fb_suit(base, stride, x + 3, y + 10, suit, clr);

    /* Large centered suit */
    fb_suit(base, stride, x + (CARD_W - 5) / 2, y + (CARD_H - 7) / 2,
            suit, clr);
}

static void fb_card_back(uint8_t *base, int16_t stride,
                          int16_t x, int16_t y) {
    /* Black border */
    fb_hline(base, stride, x + 1, y, CARD_W - 2, COLOR_BLACK);
    fb_hline(base, stride, x + 1, y + CARD_H - 1, CARD_W - 2, COLOR_BLACK);
    fb_vline(base, stride, x, y + 1, CARD_H - 2, COLOR_BLACK);
    fb_vline(base, stride, x + CARD_W - 1, y + 1, CARD_H - 2, COLOR_BLACK);
    /* White inner border */
    fb_hline(base, stride, x + 2, y + 1, CARD_W - 4, COLOR_WHITE);
    fb_hline(base, stride, x + 2, y + CARD_H - 2, CARD_W - 4, COLOR_WHITE);
    fb_vline(base, stride, x + 1, y + 2, CARD_H - 4, COLOR_WHITE);
    fb_vline(base, stride, x + CARD_W - 2, y + 2, CARD_H - 4, COLOR_WHITE);
    /* Blue fill with grid */
    fb_fill(base, stride, x + 2, y + 2, CARD_W - 4, CARD_H - 4, COLOR_BLUE);
    for (int16_t py = y + 4; py < y + CARD_H - 2; py += 4)
        fb_hline(base, stride, x + 3, py, CARD_W - 6, COLOR_LIGHT_BLUE);
    for (int16_t px = x + 4; px < x + CARD_W - 2; px += 4)
        fb_vline(base, stride, px, y + 3, CARD_H - 6, COLOR_LIGHT_BLUE);
}

static void fb_empty_slot(uint8_t *base, int16_t stride,
                           int16_t x, int16_t y) {
    fb_hline(base, stride, x + 2, y, CARD_W - 4, COLOR_DARK_GRAY);
    fb_hline(base, stride, x + 2, y + CARD_H - 1, CARD_W - 4, COLOR_DARK_GRAY);
    fb_vline(base, stride, x, y + 2, CARD_H - 4, COLOR_DARK_GRAY);
    fb_vline(base, stride, x + CARD_W - 1, y + 2, CARD_H - 4, COLOR_DARK_GRAY);
    fb_px(base, stride, x + 1, y + 1, COLOR_DARK_GRAY);
    fb_px(base, stride, x + CARD_W - 2, y + 1, COLOR_DARK_GRAY);
    fb_px(base, stride, x + 1, y + CARD_H - 2, COLOR_DARK_GRAY);
    fb_px(base, stride, x + CARD_W - 2, y + CARD_H - 2, COLOR_DARK_GRAY);
}

static void fb_foundation_slot(uint8_t *base, int16_t stride,
                                int16_t x, int16_t y, int suit_idx) {
    fb_empty_slot(base, stride, x, y);
    fb_suit(base, stride, x + (CARD_W - 5) / 2, y + (CARD_H - 7) / 2,
            (uint8_t)suit_idx, COLOR_DARK_GRAY);
}

/*==========================================================================
 * Save-under buffer for drag dirty-rect updates
 *=========================================================================*/

#define SAVE_BPR_MAX   24   /* max bytes per row: ceil(CARD_W/2) + 1 */
#define SAVE_H_MAX    200   /* max rows (covers 10+ cascaded cards) */
#define SAVE_BUF_SIZE (SAVE_BPR_MAX * SAVE_H_MAX)

/* Save a horizontal byte-strip from the framebuffer */
static void fb_save_rect(uint8_t *fb, int16_t stride,
                          uint8_t *buf, int16_t byte_off,
                          int16_t bpr, int16_t y, int16_t h) {
    for (int16_t r = 0; r < h; r++) {
        int16_t ry = y + r;
        if (ry >= 0 && ry < CLIENT_H)
            memcpy(buf + r * bpr, fb + ry * stride + byte_off, bpr);
    }
}

/* Restore a horizontal byte-strip to the framebuffer */
static void fb_restore_rect(uint8_t *fb, int16_t stride,
                              const uint8_t *buf, int16_t byte_off,
                              int16_t bpr, int16_t y, int16_t h) {
    for (int16_t r = 0; r < h; r++) {
        int16_t ry = y + r;
        if (ry >= 0 && ry < CLIENT_H)
            memcpy(fb + ry * stride + byte_off, buf + r * bpr, bpr);
    }
}

/* fb_dragged_cards, fb_tableau_card, drag helpers defined after state struct */

/*==========================================================================
 * Game state
 *=========================================================================*/

typedef struct {
    uint8_t stock[24];
    int     stock_count;
    uint8_t waste[24];
    int     waste_count;
    uint8_t foundation[4][13];
    int     found_count[4];
    uint8_t tableau[7][20];
    int     tab_count[7];
    int     tab_faceup[7];

    int     draw_mode;       /* 1 or 3 */

    /* Drag-and-drop state */
    bool     mouse_down;     /* left button held */
    bool     dragging;       /* visual drag active (past threshold) */
    int      drag_source;    /* 0-6=tableau, SEL_WASTE, SEL_FOUND_BASE+i */
    int      drag_index;     /* card index in source */
    int      drag_count;     /* number of cards dragged */
    int16_t  drag_start_x;  /* mouse pos at button down */
    int16_t  drag_start_y;
    int16_t  drag_off_x;    /* mouse-to-card-origin offset */
    int16_t  drag_off_y;
    int16_t  drag_mx;       /* current mouse pos */
    int16_t  drag_my;

    /* Direct show-buffer pointer for dirty drag updates.
     * Set at the end of sol_paint; after display_swap_buffers()
     * this memory IS the show buffer that DVI hardware scans. */
    uint8_t *show_fb;
    int16_t  fb_stride;

    /* Save-under: pixels beneath the dragged card */
    uint8_t  save_under[SAVE_BUF_SIZE];
    int16_t  save_byte_off;
    int16_t  save_bpr;
    int16_t  save_y, save_h;
    bool     save_valid;

    uint32_t rng_state;
    uint32_t start_tick;
    bool     timer_running;
    bool     game_won;

    /* Double-click detection */
    uint32_t last_click_tick;
    int16_t  last_click_x;
    int16_t  last_click_y;

    TimerHandle_t timer;
    hwnd_t        hwnd;
} sol_state_t;

static void *app_task;
static volatile bool app_closing;

/*==========================================================================
 * Timer callback
 *=========================================================================*/

static void timer_callback(TimerHandle_t xTimer) {
    sol_state_t *st = (sol_state_t *)pvTimerGetTimerID(xTimer);
    if (st->timer_running && !st->game_won)
        wm_invalidate(st->hwnd);
}

/*==========================================================================
 * Shuffle and deal
 *=========================================================================*/

static void shuffle_and_deal(sol_state_t *st) {
    /* Build ordered deck */
    uint8_t deck[52];
    int n = 0;
    for (int s = 0; s < 4; s++)
        for (int r = 0; r < 13; r++)
            deck[n++] = MAKE_CARD(s, r);

    /* Fisher-Yates shuffle */
    sol_srand((unsigned)xTaskGetTickCount());
    for (int i = 51; i > 0; i--) {
        int j = sol_rand() % (i + 1);
        uint8_t tmp = deck[i];
        deck[i] = deck[j];
        deck[j] = tmp;
    }

    /* Deal to tableau: col i gets i+1 cards, top card face-up */
    int di = 0;
    for (int col = 0; col < 7; col++) {
        st->tab_count[col] = col + 1;
        st->tab_faceup[col] = col; /* last card is face-up */
        for (int row = 0; row <= col; row++) {
            st->tableau[col][row] = deck[di++];
        }
    }

    /* Remaining 24 cards go to stock */
    st->stock_count = 52 - 28;
    for (int i = 0; i < st->stock_count; i++)
        st->stock[i] = deck[di++];

    st->waste_count = 0;

    for (int i = 0; i < 4; i++)
        st->found_count[i] = 0;
}

static void new_game(sol_state_t *st) {
    memset(st->stock, CARD_EMPTY, sizeof(st->stock));
    memset(st->waste, CARD_EMPTY, sizeof(st->waste));
    memset(st->foundation, CARD_EMPTY, sizeof(st->foundation));
    memset(st->tableau, CARD_EMPTY, sizeof(st->tableau));
    memset(st->tab_count, 0, sizeof(st->tab_count));
    memset(st->tab_faceup, 0, sizeof(st->tab_faceup));
    memset(st->found_count, 0, sizeof(st->found_count));

    st->stock_count = 0;
    st->waste_count = 0;
    st->mouse_down = false;
    st->dragging = false;
    st->drag_source = SEL_NONE;
    st->show_fb = NULL;
    st->save_valid = false;
    st->start_tick = 0;
    st->timer_running = false;
    st->game_won = false;
    st->last_click_tick = 0;
    st->last_click_x = -100;
    st->last_click_y = -100;

    if (st->timer) {
        xTimerStop(st->timer, 0);
        xTimerChangePeriod(st->timer, pdMS_TO_TICKS(1000), 0);
    }

    shuffle_and_deal(st);
    wm_invalidate(st->hwnd);
}

/*==========================================================================
 * Move validation
 *=========================================================================*/

static bool can_move_to_foundation(sol_state_t *st, uint8_t card) {
    if (card == CARD_EMPTY) return false;
    uint8_t suit = CARD_SUIT(card);
    uint8_t rank = CARD_RANK(card);
    int fc = st->found_count[suit];
    if (fc == 0)
        return (rank == 0); /* Ace */
    return (rank == CARD_RANK(st->foundation[suit][fc - 1]) + 1);
}

static bool can_move_to_tableau(sol_state_t *st, int dest_col, uint8_t card) {
    if (card == CARD_EMPTY) return false;
    int dc = st->tab_count[dest_col];
    if (dc == 0) {
        /* Empty column: only Kings */
        return (CARD_RANK(card) == 12);
    }
    uint8_t bottom = st->tableau[dest_col][dc - 1];
    /* Opposite color and rank one less */
    if (card_is_red(card) == card_is_red(bottom)) return false;
    return (CARD_RANK(card) == CARD_RANK(bottom) - 1);
}

/*==========================================================================
 * Move execution
 *=========================================================================*/

static void ensure_timer_started(sol_state_t *st) {
    if (!st->timer_running) {
        st->timer_running = true;
        st->start_tick = xTaskGetTickCount();
        if (st->timer)
            xTimerStart(st->timer, 0);
    }
}

static void check_win(sol_state_t *st) {
    if (st->found_count[0] == 13 && st->found_count[1] == 13 &&
        st->found_count[2] == 13 && st->found_count[3] == 13) {
        st->game_won = true;
        st->timer_running = false;
        if (st->timer)
            xTimerStop(st->timer, 0);
        wm_invalidate(st->hwnd);
        dialog_show(st->hwnd, AL(AL_CONGRATS),
                    AL(AL_CONGRATS_MSG),
                    DLG_ICON_INFO, DLG_BTN_OK);
    }
}

/* Flip the top face-down card in a tableau column if needed */
static void flip_tableau_top(sol_state_t *st, int col) {
    if (st->tab_count[col] > 0 && st->tab_faceup[col] >= st->tab_count[col]) {
        st->tab_faceup[col] = st->tab_count[col] - 1;
    }
}

static void do_stock_click(sol_state_t *st) {
    ensure_timer_started(st);

    if (st->stock_count == 0) {
        /* Recycle waste back to stock */
        if (st->waste_count == 0) return;
        for (int i = st->waste_count - 1; i >= 0; i--)
            st->stock[st->stock_count++] = st->waste[i];
        st->waste_count = 0;
    } else {
        /* Draw card(s) from stock to waste */
        int to_draw = st->draw_mode;
        if (to_draw > st->stock_count)
            to_draw = st->stock_count;
        for (int i = 0; i < to_draw; i++) {
            st->stock_count--;
            st->waste[st->waste_count++] = st->stock[st->stock_count];
        }
    }
}

static bool move_card_to_foundation(sol_state_t *st, uint8_t card) {
    if (!can_move_to_foundation(st, card)) return false;
    uint8_t suit = CARD_SUIT(card);
    st->foundation[suit][st->found_count[suit]] = card;
    st->found_count[suit]++;
    return true;
}

static void move_from_waste_to(sol_state_t *st, int dest_col) {
    if (st->waste_count == 0) return;
    uint8_t card = st->waste[st->waste_count - 1];

    if (dest_col >= 0 && dest_col < 7) {
        if (!can_move_to_tableau(st, dest_col, card)) return;
        st->waste_count--;
        st->tableau[dest_col][st->tab_count[dest_col]] = card;
        st->tab_count[dest_col]++;
        ensure_timer_started(st);
    }
}

static void move_from_waste_to_foundation(sol_state_t *st) {
    if (st->waste_count == 0) return;
    uint8_t card = st->waste[st->waste_count - 1];
    if (move_card_to_foundation(st, card)) {
        st->waste_count--;
        ensure_timer_started(st);
        check_win(st);
    }
}

static void move_from_tableau_to_foundation(sol_state_t *st, int src_col) {
    int sc = st->tab_count[src_col];
    if (sc == 0) return;
    uint8_t card = st->tableau[src_col][sc - 1];
    if (move_card_to_foundation(st, card)) {
        st->tableau[src_col][sc - 1] = CARD_EMPTY;
        st->tab_count[src_col]--;
        flip_tableau_top(st, src_col);
        ensure_timer_started(st);
        check_win(st);
    }
}

static void move_tableau_cards(sol_state_t *st, int src_col, int src_idx,
                               int count, int dest_col) {
    if (!can_move_to_tableau(st, dest_col, st->tableau[src_col][src_idx]))
        return;

    /* Copy cards */
    int dst_start = st->tab_count[dest_col];
    for (int i = 0; i < count; i++) {
        st->tableau[dest_col][dst_start + i] = st->tableau[src_col][src_idx + i];
        st->tableau[src_col][src_idx + i] = CARD_EMPTY;
    }
    st->tab_count[dest_col] += count;
    st->tab_count[src_col] -= count;
    flip_tableau_top(st, src_col);
    ensure_timer_started(st);
}

static void move_from_foundation_to_tableau(sol_state_t *st, int found_idx,
                                             int dest_col) {
    int fc = st->found_count[found_idx];
    if (fc == 0) return;
    uint8_t card = st->foundation[found_idx][fc - 1];
    if (!can_move_to_tableau(st, dest_col, card)) return;
    st->foundation[found_idx][fc - 1] = CARD_EMPTY;
    st->found_count[found_idx]--;
    st->tableau[dest_col][st->tab_count[dest_col]] = card;
    st->tab_count[dest_col]++;
    ensure_timer_started(st);
}

/*==========================================================================
 * Auto-foundation: move all possible cards to foundations
 *=========================================================================*/

static bool auto_foundation_one(sol_state_t *st) {
    /* Try waste */
    if (st->waste_count > 0) {
        uint8_t card = st->waste[st->waste_count - 1];
        if (can_move_to_foundation(st, card)) {
            move_from_waste_to_foundation(st);
            return true;
        }
    }
    /* Try each tableau column top card */
    for (int col = 0; col < 7; col++) {
        int sc = st->tab_count[col];
        if (sc == 0) continue;
        uint8_t card = st->tableau[col][sc - 1];
        if (can_move_to_foundation(st, card)) {
            move_from_tableau_to_foundation(st, col);
            return true;
        }
    }
    return false;
}

static void auto_foundation_all(sol_state_t *st) {
    ensure_timer_started(st);
    while (auto_foundation_one(st))
        ; /* keep going */
}

/*==========================================================================
 * Card rendering
 *=========================================================================*/

static void draw_suit_symbol(int16_t x, int16_t y, uint8_t suit, uint8_t color) {
    const uint8_t *bmp = suit_bmps[suit];
    for (int row = 0; row < 7; row++) {
        uint8_t bits = bmp[row];
        for (int col = 0; col < 5; col++) {
            if (bits & (1 << (4 - col)))
                wd_pixel(x + col, y + row, color);
        }
    }
}

static void draw_card_face(int16_t x, int16_t y, uint8_t card) {
    /* White fill */
    wd_fill_rect(x + 1, y + 1, CARD_W - 2, CARD_H - 2, COLOR_WHITE);
    /* Black border */
    wd_hline(x + 1, y, CARD_W - 2, COLOR_BLACK);
    wd_hline(x + 1, y + CARD_H - 1, CARD_W - 2, COLOR_BLACK);
    wd_vline(x, y + 1, CARD_H - 2, COLOR_BLACK);
    wd_vline(x + CARD_W - 1, y + 1, CARD_H - 2, COLOR_BLACK);

    uint8_t suit = CARD_SUIT(card);
    uint8_t rank = CARD_RANK(card);
    uint8_t color = card_is_red(card) ? COLOR_RED : COLOR_BLACK;

    /* Draw rank text top-left */
    const char *rs = rank_str[rank];
    int16_t tx = x + 3;
    int16_t ty = y + 3;
    while (*rs) {
        wd_char_ui(tx, ty, *rs, color, COLOR_WHITE);
        tx += FONT_UI_WIDTH;
        rs++;
    }

    /* Draw suit symbol below rank */
    draw_suit_symbol(x + 3, y + 3 + FONT_UI_HEIGHT + 1, suit, color);

    /* Draw large centered suit symbol */
    draw_suit_symbol(x + (CARD_W - 5) / 2, y + (CARD_H - 7) / 2, suit, color);
}

static void draw_card_back(int16_t x, int16_t y) {
    /* Black border */
    wd_hline(x + 1, y, CARD_W - 2, COLOR_BLACK);
    wd_hline(x + 1, y + CARD_H - 1, CARD_W - 2, COLOR_BLACK);
    wd_vline(x, y + 1, CARD_H - 2, COLOR_BLACK);
    wd_vline(x + CARD_W - 1, y + 1, CARD_H - 2, COLOR_BLACK);

    /* White inner border */
    wd_hline(x + 2, y + 1, CARD_W - 4, COLOR_WHITE);
    wd_hline(x + 2, y + CARD_H - 2, CARD_W - 4, COLOR_WHITE);
    wd_vline(x + 1, y + 2, CARD_H - 4, COLOR_WHITE);
    wd_vline(x + CARD_W - 2, y + 2, CARD_H - 4, COLOR_WHITE);

    /* Solid blue fill with simple grid pattern */
    wd_fill_rect(x + 2, y + 2, CARD_W - 4, CARD_H - 4, COLOR_BLUE);
    /* Light blue grid lines every 4px for a woven look */
    for (int16_t py = y + 4; py < y + CARD_H - 2; py += 4)
        wd_hline(x + 3, py, CARD_W - 6, COLOR_LIGHT_BLUE);
    for (int16_t px = x + 4; px < x + CARD_W - 2; px += 4)
        wd_vline(px, y + 3, CARD_H - 6, COLOR_LIGHT_BLUE);
}

static void draw_empty_slot(int16_t x, int16_t y) {
    /* Rounded outline slightly darker than felt */
    wd_hline(x + 2, y, CARD_W - 4, COLOR_DARK_GRAY);
    wd_hline(x + 2, y + CARD_H - 1, CARD_W - 4, COLOR_DARK_GRAY);
    wd_vline(x, y + 2, CARD_H - 4, COLOR_DARK_GRAY);
    wd_vline(x + CARD_W - 1, y + 2, CARD_H - 4, COLOR_DARK_GRAY);
    /* corners */
    wd_pixel(x + 1, y + 1, COLOR_DARK_GRAY);
    wd_pixel(x + CARD_W - 2, y + 1, COLOR_DARK_GRAY);
    wd_pixel(x + 1, y + CARD_H - 2, COLOR_DARK_GRAY);
    wd_pixel(x + CARD_W - 2, y + CARD_H - 2, COLOR_DARK_GRAY);
}

static void draw_foundation_slot(int16_t x, int16_t y, int suit_idx) {
    draw_empty_slot(x, y);
    /* Faint suit symbol in center */
    draw_suit_symbol(x + (CARD_W - 5) / 2, y + (CARD_H - 7) / 2,
                     (uint8_t)suit_idx, COLOR_DARK_GRAY);
}

/*==========================================================================
 * Full scene rendering
 *=========================================================================*/

/* Helper: compute Y position of a card in a tableau column */
static int16_t tableau_card_y(sol_state_t *st, int col, int row) {
    int facedown = st->tab_faceup[col];
    if (row < facedown)
        return TAB_Y + row * CASCADE_DOWN;
    return TAB_Y + facedown * CASCADE_DOWN + (row - facedown) * CASCADE_UP;
}

static void draw_tableau_card(int16_t cx, int16_t card_y, sol_state_t *st,
                              int col, int row, int visible_count) {
    if (row < st->tab_faceup[col]) {
        if (row == visible_count - 1) {
            /* Last visible card is face-down: draw full card back */
            draw_card_back(cx, card_y);
        } else {
            /* Face-down: draw top strip of card back */
            int16_t strip_h = CASCADE_DOWN;
            wd_hline(cx + 1, card_y, CARD_W - 2, COLOR_BLACK);
            wd_vline(cx, card_y + 1, strip_h - 1, COLOR_BLACK);
            wd_vline(cx + CARD_W - 1, card_y + 1, strip_h - 1, COLOR_BLACK);
            wd_fill_rect(cx + 1, card_y + 1, CARD_W - 2, strip_h - 1, COLOR_BLUE);
        }
    } else if (row == visible_count - 1) {
        /* Last visible card: draw full */
        draw_card_face(cx, card_y, st->tableau[col][row]);
    } else {
        /* Face-up but not last: draw top strip */
        int16_t strip_h = CASCADE_UP;
        uint8_t card = st->tableau[col][row];
        wd_hline(cx + 1, card_y, CARD_W - 2, COLOR_BLACK);
        wd_vline(cx, card_y + 1, strip_h - 1, COLOR_BLACK);
        wd_vline(cx + CARD_W - 1, card_y + 1, strip_h - 1, COLOR_BLACK);
        wd_fill_rect(cx + 1, card_y + 1, CARD_W - 2, strip_h - 1,
                     COLOR_WHITE);
        uint8_t suit = CARD_SUIT(card);
        uint8_t clr = card_is_red(card) ? COLOR_RED : COLOR_BLACK;
        const char *rs = rank_str[CARD_RANK(card)];
        int16_t tx = cx + 3;
        int16_t ty = card_y + 1;
        while (*rs) {
            wd_char_ui(tx, ty, *rs, clr, COLOR_WHITE);
            tx += FONT_UI_WIDTH;
            rs++;
        }
        draw_suit_symbol(tx + 1, ty, suit, clr);
    }
}

/* Helper: draw the dragged card(s) at a given position */
static void draw_dragged_cards(sol_state_t *st, int16_t nx, int16_t ny) {
    if (st->drag_source == SEL_WASTE) {
        draw_card_face(nx, ny, st->waste[st->waste_count - 1]);
    } else if (st->drag_source >= SEL_FOUND_BASE) {
        int fi = st->drag_source - SEL_FOUND_BASE;
        draw_card_face(nx, ny, st->foundation[fi][st->found_count[fi] - 1]);
    } else if (st->drag_source >= 0 && st->drag_source < 7) {
        for (int i = 0; i < st->drag_count; i++) {
            uint8_t card =
                st->tableau[st->drag_source][st->drag_index + i];
            draw_card_face(nx, ny + i * CASCADE_UP, card);
        }
    }
}

/* Implement fb_dragged_cards (forward-declared above state struct) */
static void fb_dragged_cards(uint8_t *base, int16_t stride,
                              sol_state_t *st, int16_t nx, int16_t ny) {
    if (st->drag_source == SEL_WASTE) {
        fb_card_face(base, stride, nx, ny,
                     st->waste[st->waste_count - 1]);
    } else if (st->drag_source >= SEL_FOUND_BASE) {
        int fi = st->drag_source - SEL_FOUND_BASE;
        fb_card_face(base, stride, nx, ny,
                     st->foundation[fi][st->found_count[fi] - 1]);
    } else if (st->drag_source >= 0 && st->drag_source < 7) {
        for (int i = 0; i < st->drag_count; i++) {
            uint8_t card =
                st->tableau[st->drag_source][st->drag_index + i];
            fb_card_face(base, stride, nx, ny + i * CASCADE_UP, card);
        }
    }
}

/*==========================================================================
 * Drag dirty-update helpers (operate on show buffer)
 *=========================================================================*/

static int16_t drag_total_h(sol_state_t *st) {
    int16_t h = CARD_H;
    if (st->drag_count > 1)
        h += (st->drag_count - 1) * CASCADE_UP;
    return h;
}

/* Save the rectangle under the dragged card(s) */
static void drag_save(uint8_t *fb, int16_t stride, sol_state_t *st,
                       int16_t nx, int16_t ny) {
    int16_t dh = drag_total_h(st);
    int16_t cy = ny, ch = dh;
    if (cy < 0) { ch += cy; cy = 0; }
    if (cy + ch > CLIENT_H) ch = CLIENT_H - cy;
    if (ch <= 0 || ch > SAVE_H_MAX) { st->save_valid = false; return; }

    int16_t b0 = nx >> 1;
    int16_t b1 = (nx + CARD_W + 1) >> 1;
    int16_t bpr = b1 - b0;
    if (bpr > SAVE_BPR_MAX) { st->save_valid = false; return; }

    st->save_byte_off = b0;
    st->save_bpr = bpr;
    st->save_y = cy;
    st->save_h = ch;
    fb_save_rect(fb, stride, st->save_under, b0, bpr, cy, ch);
    st->save_valid = true;
}

/* Restore the saved pixels under the dragged card(s) */
static void drag_restore(uint8_t *fb, int16_t stride, sol_state_t *st) {
    if (!st->save_valid) return;
    fb_restore_rect(fb, stride, st->save_under,
                    st->save_byte_off, st->save_bpr,
                    st->save_y, st->save_h);
    st->save_valid = false;
}

/* Erase the source card(s) from the show buffer and reveal card underneath */
static void fb_erase_source(uint8_t *fb, int16_t stride, sol_state_t *st) {
    if (st->drag_source >= 0 && st->drag_source < 7) {
        int col = st->drag_source;
        int16_t cx = COL_X(col);
        int16_t top_y = tableau_card_y(st, col, st->drag_index);
        int last = st->tab_count[col] - 1;
        int16_t bot_y = tableau_card_y(st, col, last) + CARD_H;
        fb_fill(fb, stride, cx, top_y, CARD_W, bot_y - top_y, COLOR_GREEN);
        if (st->drag_index > 0) {
            int row = st->drag_index - 1;
            int16_t ry = tableau_card_y(st, col, row);
            if (row < st->tab_faceup[col])
                fb_card_back(fb, stride, cx, ry);
            else
                fb_card_face(fb, stride, cx, ry, st->tableau[col][row]);
        } else {
            fb_empty_slot(fb, stride, cx, TAB_Y);
        }
    } else if (st->drag_source == SEL_WASTE) {
        fb_fill(fb, stride, COL_X(1), TOP_ROW_Y, CARD_W, CARD_H, COLOR_GREEN);
        if (st->waste_count > 1)
            fb_card_face(fb, stride, COL_X(1), TOP_ROW_Y,
                         st->waste[st->waste_count - 2]);
        else
            fb_empty_slot(fb, stride, COL_X(1), TOP_ROW_Y);
    } else if (st->drag_source >= SEL_FOUND_BASE) {
        int fi = st->drag_source - SEL_FOUND_BASE;
        int16_t fx = COL_X(3 + fi);
        fb_fill(fb, stride, fx, TOP_ROW_Y, CARD_W, CARD_H, COLOR_GREEN);
        if (st->found_count[fi] > 1)
            fb_card_face(fb, stride, fx, TOP_ROW_Y,
                         st->foundation[fi][st->found_count[fi] - 2]);
        else
            fb_foundation_slot(fb, stride, fx, TOP_ROW_Y, fi);
    }
}

/* Helper: draw a single tableau card row via fb_* */
static void fb_tableau_card(uint8_t *base, int16_t stride,
                             int16_t cx, int16_t card_y,
                             sol_state_t *st, int col, int row,
                             int visible_count) {
    if (row < st->tab_faceup[col]) {
        if (row == visible_count - 1) {
            /* Last visible card is face-down: draw full card back */
            fb_card_back(base, stride, cx, card_y);
        } else {
            /* Face-down: top strip of card back */
            int16_t strip_h = CASCADE_DOWN;
            fb_hline(base, stride, cx + 1, card_y, CARD_W - 2, COLOR_BLACK);
            fb_vline(base, stride, cx, card_y + 1, strip_h - 1, COLOR_BLACK);
            fb_vline(base, stride, cx + CARD_W - 1, card_y + 1,
                     strip_h - 1, COLOR_BLACK);
            fb_fill(base, stride, cx + 1, card_y + 1,
                    CARD_W - 2, strip_h - 1, COLOR_BLUE);
        }
    } else if (row == visible_count - 1) {
        /* Last visible card: draw full */
        fb_card_face(base, stride, cx, card_y, st->tableau[col][row]);
    } else {
        /* Face-up but not last: top strip with rank+suit */
        int16_t strip_h = CASCADE_UP;
        uint8_t card = st->tableau[col][row];
        fb_hline(base, stride, cx + 1, card_y, CARD_W - 2, COLOR_BLACK);
        fb_vline(base, stride, cx, card_y + 1, strip_h - 1, COLOR_BLACK);
        fb_vline(base, stride, cx + CARD_W - 1, card_y + 1,
                 strip_h - 1, COLOR_BLACK);
        fb_fill(base, stride, cx + 1, card_y + 1,
                CARD_W - 2, strip_h - 1, COLOR_WHITE);
        uint8_t suit = CARD_SUIT(card);
        uint8_t clr = card_is_red(card) ? COLOR_RED : COLOR_BLACK;
        const char *rs = rank_str[CARD_RANK(card)];
        int16_t tx = cx + 3;
        while (*rs) {
            fb_glyph(base, stride, tx, card_y + 2, mini_idx(*rs), clr);
            tx += 5;
            rs++;
        }
        fb_suit(base, stride, tx + 1, card_y + 2, suit, clr);
    }
}

static void sol_paint(hwnd_t hwnd) {
    window_t *win = wm_get_window(hwnd);
    if (!win) return;
    sol_state_t *st = (sol_state_t *)win->user_data;
    if (!st) return;

    /* Get direct framebuffer pointer (ZX Spectrum approach).
     * wd_begin() was already called by the compositor. */
    int16_t stride;
    uint8_t *fb = wd_fb_ptr(0, 0, &stride);
    if (!fb) return;

    /* Clamp drawing to visible client area to prevent scanline overflow
     * when the window extends past the right or bottom screen edge. */
    int16_t cw, ch;
    wd_get_clip_size(&cw, &ch);
    vis_w = (cw < CLIENT_W) ? cw : CLIENT_W;
    vis_h = (ch < CLIENT_H) ? ch : CLIENT_H;

    /* Drag partial update: skip full green fill to avoid flicker.
     * This runs inside wm_composite() after cursor erase and before
     * cursor stamp — no conflict with cursor save-under.
     * Fall through to full repaint when WF_FRAME_DIRTY is set
     * (structural change like window expose). */
    if (st->dragging && !(win->flags & WF_FRAME_DIRTY)) {
        if (st->save_valid)
            drag_restore(fb, stride, st);
        else
            fb_erase_source(fb, stride, st);
        int16_t nx = st->drag_mx - st->drag_off_x;
        int16_t ny = st->drag_my - st->drag_off_y;
        drag_save(fb, stride, st, nx, ny);
        fb_dragged_cards(fb, stride, st, nx, ny);
        st->show_fb = fb;
        st->fb_stride = stride;
        return;
    }

    /* Green felt background — fast memset via fb_fill */
    fb_fill(fb, stride, 0, 0, CLIENT_W, CLIENT_H, COLOR_GREEN);

    /* ---- Top row ---- */

    /* Stock pile (col 0) */
    if (st->stock_count > 0) {
        fb_card_back(fb, stride, COL_X(0), TOP_ROW_Y);
    } else {
        fb_empty_slot(fb, stride, COL_X(0), TOP_ROW_Y);
    }

    /* Waste pile (col 1) */
    {
        int waste_show = st->waste_count;
        if (st->dragging && st->drag_source == SEL_WASTE)
            waste_show--;
        if (waste_show > 0) {
            fb_card_face(fb, stride, COL_X(1), TOP_ROW_Y,
                         st->waste[waste_show - 1]);
        } else {
            fb_empty_slot(fb, stride, COL_X(1), TOP_ROW_Y);
        }
    }

    /* Foundations (cols 3-6) */
    for (int i = 0; i < 4; i++) {
        int16_t fx = COL_X(3 + i);
        int fc_show = st->found_count[i];
        if (st->dragging && st->drag_source == SEL_FOUND_BASE + i)
            fc_show--;
        if (fc_show > 0) {
            fb_card_face(fb, stride, fx, TOP_ROW_Y,
                         st->foundation[i][fc_show - 1]);
        } else {
            fb_foundation_slot(fb, stride, fx, TOP_ROW_Y, i);
        }
    }

    /* ---- Tableau ---- */
    for (int col = 0; col < 7; col++) {
        int16_t cx = COL_X(col);
        int visible_count = st->tab_count[col];

        /* Hide dragged cards from their source column */
        if (st->dragging && st->drag_source >= 0 &&
            st->drag_source < 7 && st->drag_source == col) {
            visible_count = st->drag_index;
        }

        if (visible_count == 0) {
            fb_empty_slot(fb, stride, cx, TAB_Y);
            continue;
        }

        for (int row = 0; row < visible_count; row++) {
            int16_t card_y = tableau_card_y(st, col, row);
            fb_tableau_card(fb, stride, cx, card_y, st, col, row,
                            visible_count);
        }
    }

    /* ---- Status bar ---- */
    int16_t bar_y = CLIENT_H - STATUS_H;
    fb_fill(fb, stride, 0, bar_y, CLIENT_W, STATUS_H, THEME_BUTTON_FACE);
    fb_hline(fb, stride, 0, bar_y, CLIENT_W, COLOR_DARK_GRAY);

    /* Timer display — use system font via wd_text_ui (context already active) */
    char timebuf[20];
    uint32_t elapsed = st->timer_running
        ? (xTaskGetTickCount() - st->start_tick) / configTICK_RATE_HZ : 0;
    uint32_t mins = elapsed / 60;
    uint32_t secs = elapsed % 60;
    if (mins > 99) mins = 99;
    timebuf[0] = 'T'; timebuf[1] = 'i'; timebuf[2] = 'm'; timebuf[3] = 'e';
    timebuf[4] = ':'; timebuf[5] = ' ';
    timebuf[6] = '0' + (char)(mins / 10);
    timebuf[7] = '0' + (char)(mins % 10);
    timebuf[8] = ':';
    timebuf[9] = '0' + (char)(secs / 10);
    timebuf[10] = '0' + (char)(secs % 10);
    timebuf[11] = '\0';
    wd_text_ui(4, bar_y + 2, timebuf, COLOR_BLACK, THEME_BUTTON_FACE);

    /* Dragged cards on top of everything (with save-under) */
    if (st->dragging) {
        int16_t nx = st->drag_mx - st->drag_off_x;
        int16_t ny = st->drag_my - st->drag_off_y;
        drag_save(fb, stride, st, nx, ny);
        fb_dragged_cards(fb, stride, st, nx, ny);
    }

    /* Save fb pointer — after display_swap_buffers() this memory
     * becomes the show buffer (DVI hardware scans it).
     * MOUSEMOVE writes dirty updates directly here. */
    st->show_fb = fb;
    st->fb_stride = stride;
}

/*==========================================================================
 * Hit testing: screen coords -> game element
 *=========================================================================*/

/* Returns:
 *  -1 = nothing
 *  -2 = stock pile
 *   0..6 = tableau column (out_index = card index, out_count = cards from there)
 *   SEL_WASTE(7) = waste pile top card
 *   SEL_FOUND_BASE+i(8..11) = foundation i
 */
static int hit_test(sol_state_t *st, int16_t mx, int16_t my,
                    int *out_index, int *out_count) {
    *out_index = 0;
    *out_count = 1;

    /* Stock pile */
    if (mx >= COL_X(0) && mx < COL_X(0) + CARD_W &&
        my >= TOP_ROW_Y && my < TOP_ROW_Y + CARD_H) {
        return -2; /* stock */
    }

    /* Waste pile */
    if (mx >= COL_X(1) && mx < COL_X(1) + CARD_W &&
        my >= TOP_ROW_Y && my < TOP_ROW_Y + CARD_H) {
        if (st->waste_count > 0) {
            *out_index = st->waste_count - 1;
            return SEL_WASTE;
        }
        return -1;
    }

    /* Foundations */
    for (int i = 0; i < 4; i++) {
        int16_t fx = COL_X(3 + i);
        if (mx >= fx && mx < fx + CARD_W &&
            my >= TOP_ROW_Y && my < TOP_ROW_Y + CARD_H) {
            *out_index = 0;
            return SEL_FOUND_BASE + i;
        }
    }

    /* Tableau columns */
    for (int col = 0; col < 7; col++) {
        int16_t cx = COL_X(col);
        if (mx < cx || mx >= cx + CARD_W) continue;
        if (my < TAB_Y) continue;

        int tc = st->tab_count[col];
        if (tc == 0) {
            /* Empty column slot */
            if (my < TAB_Y + CARD_H) {
                *out_index = 0;
                *out_count = 0;
                return col;
            }
            continue;
        }

        /* Find which card was clicked (test from bottom up) */
        int facedown_rows = st->tab_faceup[col];
        for (int row = tc - 1; row >= 0; row--) {
            int16_t card_y;
            if (row < facedown_rows) {
                card_y = TAB_Y + row * CASCADE_DOWN;
            } else {
                card_y = TAB_Y + facedown_rows * CASCADE_DOWN +
                         (row - facedown_rows) * CASCADE_UP;
            }

            int16_t card_bottom;
            if (row == tc - 1) {
                card_bottom = card_y + CARD_H;
            } else if (row < facedown_rows) {
                card_bottom = card_y + CASCADE_DOWN;
            } else {
                card_bottom = card_y + CASCADE_UP;
            }

            if (my >= card_y && my < card_bottom) {
                /* Only allow clicking face-up cards */
                if (row < facedown_rows) return -1;
                *out_index = row;
                *out_count = tc - row;
                return col;
            }
        }
    }

    return -1;
}

/*==========================================================================
 * Drop target: where would the card land at this mouse position?
 *=========================================================================*/

static int drop_target(int16_t mx, int16_t my) {
    /* Foundations */
    for (int i = 0; i < 4; i++) {
        int16_t fx = COL_X(3 + i);
        if (mx >= fx && mx < fx + CARD_W &&
            my >= TOP_ROW_Y && my < TOP_ROW_Y + CARD_H)
            return SEL_FOUND_BASE + i;
    }
    /* Tableau columns — generous vertical hit area */
    for (int col = 0; col < 7; col++) {
        int16_t cx = COL_X(col);
        if (mx >= cx && mx < cx + CARD_W && my >= TAB_Y)
            return col;
    }
    return -1;
}

/*==========================================================================
 * Execute drop: try to move dragged card(s) to the target
 *=========================================================================*/

static void execute_drop(sol_state_t *st, int target) {
    /* Drop on foundation (single card only) */
    if (target >= SEL_FOUND_BASE && target <= SEL_FOUND_BASE + 3) {
        if (st->drag_count != 1) return;
        if (st->drag_source == SEL_WASTE) {
            move_from_waste_to_foundation(st);
        } else if (st->drag_source >= 0 && st->drag_source < 7) {
            move_from_tableau_to_foundation(st, st->drag_source);
        }
        return;
    }
    /* Drop on tableau column */
    if (target >= 0 && target < 7) {
        if (st->drag_source == SEL_WASTE) {
            move_from_waste_to(st, target);
        } else if (st->drag_source >= SEL_FOUND_BASE &&
                   st->drag_source <= SEL_FOUND_BASE + 3) {
            int fi = st->drag_source - SEL_FOUND_BASE;
            move_from_foundation_to_tableau(st, fi, target);
        } else if (st->drag_source >= 0 && st->drag_source < 7) {
            move_tableau_cards(st, st->drag_source, st->drag_index,
                              st->drag_count, target);
        }
    }
}

/*==========================================================================
 * Forward declarations
 *=========================================================================*/

static void setup_menu(hwnd_t hwnd, sol_state_t *st);

static void setup_menu_checkmarks(sol_state_t *st) {
    setup_menu(st->hwnd, st);
}

/*==========================================================================
 * Event handler — drag-and-drop interaction
 *=========================================================================*/

#define DRAG_THRESHOLD 2

static bool sol_event(hwnd_t hwnd, const window_event_t *ev) {
    window_t *win = wm_get_window(hwnd);
    if (!win || !win->user_data) return false;
    sol_state_t *st = (sol_state_t *)win->user_data;

    /* ---- Mouse button down ---- */
    if (ev->type == WM_LBUTTONDOWN) {
        int16_t mx = ev->mouse.x, my = ev->mouse.y;
        if (st->game_won) return true;

        int hit_idx, hit_cnt;
        int target = hit_test(st, mx, my, &hit_idx, &hit_cnt);

        /* Stock pile: immediate action, no drag */
        if (target == -2) {
            do_stock_click(st);
            wm_invalidate(st->hwnd);
            return true;
        }

        /* Nothing clickable */
        if (target == -1) return true;

        /* Begin potential drag on a card */
        st->mouse_down = true;
        st->dragging = false;
        st->drag_start_x = mx;
        st->drag_start_y = my;
        st->drag_mx = mx;
        st->drag_my = my;

        if (target == SEL_WASTE && st->waste_count > 0) {
            st->drag_source = SEL_WASTE;
            st->drag_index = st->waste_count - 1;
            st->drag_count = 1;
            st->drag_off_x = mx - COL_X(1);
            st->drag_off_y = my - TOP_ROW_Y;
        } else if (target >= SEL_FOUND_BASE && target <= SEL_FOUND_BASE + 3) {
            int fi = target - SEL_FOUND_BASE;
            if (st->found_count[fi] == 0) { st->mouse_down = false; return true; }
            st->drag_source = target;
            st->drag_index = st->found_count[fi] - 1;
            st->drag_count = 1;
            st->drag_off_x = mx - COL_X(3 + fi);
            st->drag_off_y = my - TOP_ROW_Y;
        } else if (target >= 0 && target < 7 && hit_cnt > 0) {
            st->drag_source = target;
            st->drag_index = hit_idx;
            st->drag_count = hit_cnt;
            int16_t card_y = tableau_card_y(st, target, hit_idx);
            st->drag_off_x = mx - COL_X(target);
            st->drag_off_y = my - card_y;
        } else {
            st->mouse_down = false;
        }
        /* Fast-tick timer so MOUSEMOVE events get dispatched promptly */
        if (st->mouse_down && st->timer)
            xTimerChangePeriod(st->timer, pdMS_TO_TICKS(16), 0);
        return true;
    }

    /* ---- Mouse move ---- */
    if (ev->type == WM_MOUSEMOVE) {
        if (!st->mouse_down) return false;
        int16_t mx = ev->mouse.x, my = ev->mouse.y;
        st->drag_mx = mx;
        st->drag_my = my;

        if (!st->dragging) {
            int16_t dx = mx - st->drag_start_x;
            int16_t dy = my - st->drag_start_y;
            if (dx < 0) dx = -dx;
            if (dy < 0) dy = -dy;
            if (dx >= DRAG_THRESHOLD || dy >= DRAG_THRESHOLD) {
                st->dragging = true;
                /* Visual update deferred to sol_paint() which runs
                 * inside wm_composite after cursor erase. */
            }
        }
        /* Position updated; visual update deferred to sol_paint(). */

        /* Trigger sol_paint() in the compositor for visual update.
         * sol_paint does a partial drag update (no green fill)
         * inside wm_composite after cursor erase — no save-under conflict. */
        if (st->dragging)
            wm_invalidate(st->hwnd);
        return true;
    }

    /* ---- Mouse button up ---- */
    if (ev->type == WM_LBUTTONUP) {
        if (!st->mouse_down) return false;
        int16_t mx = ev->mouse.x, my = ev->mouse.y;
        bool was_dragging = st->dragging;
        st->mouse_down = false;
        st->dragging = false;

        if (was_dragging) {
            /* Restore normal 1-second timer */
            if (st->timer)
                xTimerChangePeriod(st->timer, pdMS_TO_TICKS(1000), 0);
            /* Drop the card(s) */
            st->save_valid = false;
            int target = drop_target(mx, my);
            if (target >= 0)
                execute_drop(st, target);
            wm_invalidate(st->hwnd);
        } else {
            /* It was a click (not a drag) — check for double-click */
            uint32_t now = xTaskGetTickCount();
            int16_t dx = mx - st->last_click_x;
            int16_t dy = my - st->last_click_y;
            if (dx < 0) dx = -dx;
            if (dy < 0) dy = -dy;
            bool is_dblclick = ((now - st->last_click_tick) < pdMS_TO_TICKS(400)
                                && dx < 5 && dy < 5);
            st->last_click_tick = now;
            st->last_click_x = mx;
            st->last_click_y = my;

            if (is_dblclick) {
                /* Double-click: auto-foundation */
                if (st->drag_source == SEL_WASTE) {
                    move_from_waste_to_foundation(st);
                } else if (st->drag_source >= 0 && st->drag_source < 7) {
                    int tc = st->tab_count[st->drag_source];
                    if (tc > 0 && st->drag_index == tc - 1)
                        move_from_tableau_to_foundation(st, st->drag_source);
                }
                wm_invalidate(st->hwnd);
            }
        }
        return true;
    }

    /* ---- Right-click: auto-foundation all ---- */
    if (ev->type == WM_RBUTTONDOWN) {
        if (!st->game_won) {
            st->mouse_down = false;
            st->dragging = false;
            auto_foundation_all(st);
            wm_invalidate(st->hwnd);
        }
        return true;
    }

    if (ev->type == WM_COMMAND) {
        if (ev->command.id == CMD_NEW) {
            new_game(st);
            return true;
        }
        if (ev->command.id == CMD_DRAW_ONE) {
            st->draw_mode = 1;
            setup_menu_checkmarks(st);
            new_game(st);
            return true;
        }
        if (ev->command.id == CMD_DRAW_THREE) {
            st->draw_mode = 3;
            setup_menu_checkmarks(st);
            new_game(st);
            return true;
        }
        if (ev->command.id == CMD_EXIT) {
            window_event_t ce = {0};
            ce.type = WM_CLOSE;
            wm_post_event(hwnd, &ce);
            return true;
        }
        if (ev->command.id == CMD_ABOUT) {
            dialog_show(hwnd, AL(AL_ABOUT),
                        "Solitaire\n\nFRANK OS v" FRANK_VERSION_STR
                        "\n(c) 2026 Mikhail Matveev\n"
                        "<xtreme@rh1.tech>\n"
                        "github.com/rh1tech/frank-os",
                        DLG_ICON_INFO, DLG_BTN_OK);
            return true;
        }
        return false;
    }

    if (ev->type == WM_KEYDOWN) {
        /* F2 = new game (HID code 0x3B) */
        if (ev->key.scancode == 0x3B) {
            new_game(st);
            return true;
        }
        /* F1 = about */
        if (ev->key.scancode == 0x3A) {
            window_event_t ce = {0}; ce.type = WM_COMMAND; ce.command.id = CMD_ABOUT;
            wm_post_event(hwnd, &ce); return true;
        }
        return false;
    }

    if (ev->type == WM_CLOSE) {
        dbg_printf("[solitaire] WM_CLOSE received!\n");
        if (st->timer) {
            xTimerStop(st->timer, portMAX_DELAY);
            xTimerDelete(st->timer, portMAX_DELAY);
            st->timer = (TimerHandle_t)0;
        }
        win->user_data = (void*)0;
        app_closing = true;
        xTaskNotifyGive(app_task);
        return true;
    }

    return false;
}

/*==========================================================================
 * Menu setup
 *=========================================================================*/

static void setup_menu(hwnd_t hwnd, sol_state_t *st) {
    menu_bar_t bar;
    memset(&bar, 0, sizeof(bar));
    bar.menu_count = 2;

    /* Game menu */
    menu_def_t *game = &bar.menus[0];
    strncpy(game->title, AL(AL_GAME), sizeof(game->title) - 1);
    game->accel_key = 0x0A; /* HID 'G' */
    game->item_count = 6;

    strncpy(game->items[0].text, AL(AL_NEW_GAME), sizeof(game->items[0].text) - 1);
    game->items[0].command_id = CMD_NEW;
    game->items[0].accel_key = 0x3B; /* F2 */

    game->items[1].flags = MIF_SEPARATOR;

    snprintf(game->items[2].text, sizeof(game->items[2].text),
             "%s %s", st->draw_mode == 1 ? "*" : " ", AL(AL_DRAW_ONE));
    game->items[2].command_id = CMD_DRAW_ONE;

    snprintf(game->items[3].text, sizeof(game->items[3].text),
             "%s %s", st->draw_mode == 3 ? "*" : " ", AL(AL_DRAW_THREE));
    game->items[3].command_id = CMD_DRAW_THREE;

    game->items[4].flags = MIF_SEPARATOR;

    strncpy(game->items[5].text, L(STR_FM_EXIT), sizeof(game->items[5].text) - 1);
    game->items[5].command_id = CMD_EXIT;

    /* Help menu */
    menu_def_t *help = &bar.menus[1];
    strncpy(help->title, L(STR_HELP), sizeof(help->title) - 1);
    help->accel_key = 0x0B; /* HID 'H' */
    help->item_count = 1;

    strncpy(help->items[0].text, L(STR_FM_ABOUT_MENU), sizeof(help->items[0].text) - 1);
    help->items[0].command_id = CMD_ABOUT;
    help->items[0].accel_key = 0x3A;

    menu_set(hwnd, &bar);
}

/*==========================================================================
 * Window creation and layout
 *=========================================================================*/

static void client_to_frame(int16_t cw, int16_t ch, int16_t *fw, int16_t *fh) {
    *fw = cw + 2 * THEME_BORDER_WIDTH;
    *fh = ch + THEME_TITLE_HEIGHT + THEME_MENU_HEIGHT + 2 * THEME_BORDER_WIDTH;
}

static hwnd_t solitaire_create(void) {
    sol_state_t *st = calloc(1, sizeof(sol_state_t));
    if (!st) return HWND_NULL;

    st->draw_mode = 1; /* Draw One default */
    st->drag_source = SEL_NONE;

    int16_t fw, fh;
    client_to_frame(CLIENT_W, CLIENT_H, &fw, &fh);

    int16_t x = (DISPLAY_WIDTH - fw) / 2;
    int16_t y = (DISPLAY_HEIGHT - TASKBAR_HEIGHT - fh) / 2;
    if (y < 0) y = 0;

    /* Icon is set by the launcher (Start Menu / file manager) from .ico file */
    hwnd_t hwnd = wm_create_window(x, y, fw, fh, "Solitaire",
                                    WSTYLE_DIALOG | WF_MENUBAR,
                                    sol_event, sol_paint);
    if (hwnd == HWND_NULL) {
        free(st);
        return HWND_NULL;
    }

    st->hwnd = hwnd;

    window_t *win = wm_get_window(hwnd);
    if (win) {
        win->user_data = st;
        win->bg_color = COLOR_GREEN;
    }

    setup_menu(hwnd, st);

    /* Create 1-second timer */
    st->timer = xTimerCreate("sol_tmr", pdMS_TO_TICKS(1000),
                              pdTRUE, (void *)st, timer_callback);

    shuffle_and_deal(st);

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
    app_closing = false;

    hwnd_t hwnd = solitaire_create();
    if (hwnd == HWND_NULL)
        return 1;

    wm_set_focus(hwnd);
    taskbar_invalidate();

    dbg_printf("[solitaire] entering wait loop\n");
    while (!app_closing) {
        uint32_t nv = ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        (void)nv;
    }
    dbg_printf("[solitaire] exited wait loop\n");

    wm_destroy_window(hwnd);
    taskbar_invalidate();

    return 0;
}
