/*
 * Manul — Text Web Browser for FRANK OS
 * Render engine: converts HTML tokens to a grid of styled cells.
 * Ported from the frank-lynx render engine.
 *
 * Copyright (c) 2026 Mikhail Matveev <xtreme@rh1.tech>
 * https://rh1.tech
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef RENDER_H
#define RENDER_H

#include <stdint.h>
#include <stdbool.h>

#define RENDER_MAX_COLS     80
#define RENDER_MAX_LINKS    256
#define RENDER_MAX_LINES    480

#define RATTR_NORMAL    0x00
#define RATTR_UNDERLINE 0x01
#define RATTR_BOLD      0x04
#define RATTR_REVERSE   0x08

typedef struct {
    char    ch;
    uint8_t attr;
    uint8_t color;
    int8_t  link_id;
} render_cell_t;

#define RCELL_FG(c)  ((c).color & 0x0F)
#define RCELL_BG(c)  (((c).color >> 4) & 0x0F)
#define RCELL_COLOR(fg, bg)  (((bg) << 4) | ((fg) & 0x0F))

typedef struct {
    render_cell_t cells[RENDER_MAX_COLS];
    uint8_t len;
    uint8_t heading;    /* 0=normal, 1-6=heading level (2x render) */
    uint8_t _pad[2];
} render_line_t;

typedef struct {
    char     url[128];
    uint16_t start_line;
    uint8_t  start_col;
    uint16_t end_line;
    uint8_t  end_col;
} render_link_t;

typedef struct {
    render_line_t *lines;
    uint16_t num_lines;
    uint16_t capacity;
    render_link_t *links;
    uint16_t num_links;
    char title[128];
} render_page_t;

typedef struct {
    render_page_t *page;
    uint8_t col;
    uint8_t indent;
    bool bold;
    bool underline;
    bool preformatted;
    bool in_anchor;
    int16_t current_link_id;
    uint8_t heading_level;
    uint8_t list_depth;
    bool ordered_list[8];
    uint16_t list_counter[8];
    uint8_t blockquote_depth;
    bool last_was_space;
    bool at_line_start;
    bool pending_paragraph;
    bool pending_br;
    char pending_bullet[8];  /* deferred bullet text, emitted on first content */
    uint8_t pending_bullet_len;
    bool in_head;
    bool in_title;
    bool suppress_output;
} render_ctx_t;

void render_init(render_page_t *page);
void render_clear(render_page_t *page);
void render_ctx_init(render_ctx_t *ctx, render_page_t *page);
void render_process_token(const void *token, void *ctx);
void render_flush(render_ctx_t *ctx);
const render_line_t *render_get_line(const render_page_t *page, uint16_t idx);
const render_link_t *render_get_link(const render_page_t *page, uint16_t idx);
int16_t render_find_next_link(const render_page_t *page, int16_t current, int direction);

#endif
