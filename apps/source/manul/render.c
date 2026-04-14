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

#include "manul.h"
#include "html.h"
#include "render.h"

/* ========================================================================
 * Color scheme
 * ======================================================================== */

#define FG_NORMAL   0    /* black */
#define FG_BOLD     0    /* black (bold rendered by caller) */
#define FG_LINK     1    /* blue */
#define BG_DEFAULT  15   /* white */

/* ========================================================================
 * UTF-8 to Windows-1251 conversion
 * ======================================================================== */

static const uint8_t utf8_to_win1251_80[] = {
    /* U+0400..U+040F => Win1251 0x80..0x8F */
    0xC0, 0xC1, 0xC2, 0xC3, 0xC4, 0xC5, 0xC6, 0xC7,
    0xC8, 0xC9, 0xCA, 0xCB, 0xCC, 0xCD, 0xCE, 0xCF
};

static uint8_t decode_utf8_char(const char *s, int *advance) {
    uint8_t b0 = (uint8_t)s[0];
    if (b0 < 0x80) {
        *advance = 1;
        return b0;
    }
    if ((b0 & 0xE0) == 0xC0) {
        uint8_t b1 = (uint8_t)s[1];
        if ((b1 & 0xC0) != 0x80) { *advance = 1; return '?'; }
        uint16_t cp = ((uint16_t)(b0 & 0x1F) << 6) | (b1 & 0x3F);
        *advance = 2;
        /* Cyrillic U+0410..U+044F => Win1251 0xC0..0xFF */
        if (cp >= 0x0410 && cp <= 0x044F)
            return (uint8_t)(cp - 0x0410 + 0xC0);
        /* U+0401 (Yo) => 0xA8, U+0451 (yo) => 0xB8 */
        if (cp == 0x0401) return 0xA8;
        if (cp == 0x0451) return 0xB8;
        /* U+0400..U+040F */
        if (cp >= 0x0400 && cp <= 0x040F)
            return utf8_to_win1251_80[cp - 0x0400];
        /* U+2010..U+2015 dashes */
        if (cp >= 0x00A0 && cp <= 0x00BF) return (uint8_t)(cp & 0xFF);
        /* Non-breaking space */
        if (cp == 0x00A0) return ' ';
        /* Soft hyphen */
        if (cp == 0x00AD) return '-';
        return '?';
    }
    if ((b0 & 0xF0) == 0xE0) {
        uint8_t b1 = (uint8_t)s[1];
        uint8_t b2 = (uint8_t)s[2];
        if ((b1 & 0xC0) != 0x80 || (b2 & 0xC0) != 0x80) { *advance = 1; return '?'; }
        uint16_t cp = ((uint16_t)(b0 & 0x0F) << 12) | ((uint16_t)(b1 & 0x3F) << 6) | (b2 & 0x3F);
        *advance = 3;
        /* Common typographic chars */
        if (cp == 0x2013) return '-';  /* en dash */
        if (cp == 0x2014) return '-';  /* em dash */
        if (cp == 0x2018 || cp == 0x2019) return '\''; /* smart quotes */
        if (cp == 0x201C || cp == 0x201D) return '"';   /* smart double quotes */
        if (cp == 0x2022) return 0x95;  /* bullet • */
        if (cp == 0x2026) return '.';  /* ellipsis -> single dot */
        if (cp == 0x00AB || cp == 0x00BB) return '"';   /* guillemets */
        /* U+0450..U+045F */
        if (cp >= 0x0450 && cp <= 0x045F)
            return (uint8_t)(cp - 0x0450 + 0x90);
        return '?';
    }
    if ((b0 & 0xF8) == 0xF0) {
        *advance = 4;
        return '?';
    }
    *advance = 1;
    return '?';
}

/* ========================================================================
 * HTML entity decoder (inline, small subset)
 * ======================================================================== */

static int decode_entity(const char *s, char *out) {
    /* Returns number of source bytes consumed, writes 1 char to *out */
    if (s[0] != '&') { *out = s[0]; return 1; }
    const char *p = s + 1;
    char name[12];
    int ni = 0;
    while (*p && *p != ';' && ni < 10) {
        name[ni++] = *p++;
    }
    name[ni] = '\0';
    int consumed = (int)(p - s);
    if (*p == ';') consumed++;

    if (name[0] == '#') {
        long val = 0;
        if (name[1] == 'x' || name[1] == 'X')
            val = manul_strtol(name + 2, 0, 16);
        else
            val = manul_strtol(name + 1, 0, 10);
        if (val > 0 && val < 128) { *out = (char)val; return consumed; }
        *out = '?';
        return consumed;
    }

    if (manul_strcasecmp(name, "amp") == 0)  { *out = '&'; return consumed; }
    if (manul_strcasecmp(name, "lt") == 0)   { *out = '<'; return consumed; }
    if (manul_strcasecmp(name, "gt") == 0)   { *out = '>'; return consumed; }
    if (manul_strcasecmp(name, "quot") == 0) { *out = '"'; return consumed; }
    if (manul_strcasecmp(name, "apos") == 0) { *out = '\''; return consumed; }
    if (manul_strcasecmp(name, "nbsp") == 0) { *out = ' '; return consumed; }
    if (manul_strcasecmp(name, "copy") == 0) { *out = '('; return consumed; }
    if (manul_strcasecmp(name, "reg") == 0)  { *out = '('; return consumed; }
    if (manul_strcasecmp(name, "mdash") == 0){ *out = '-'; return consumed; }
    if (manul_strcasecmp(name, "ndash") == 0){ *out = '-'; return consumed; }
    if (manul_strcasecmp(name, "laquo") == 0){ *out = '"'; return consumed; }
    if (manul_strcasecmp(name, "raquo") == 0){ *out = '"'; return consumed; }
    if (manul_strcasecmp(name, "bull") == 0) { *out = '*'; return consumed; }
    if (manul_strcasecmp(name, "hellip") == 0){ *out = '.'; return consumed; }

    /* Unknown entity: output '?' */
    *out = '?';
    return consumed;
}

/* ========================================================================
 * Internal helpers
 * ======================================================================== */

static render_line_t *ensure_line(render_page_t *page) {
    if (page->num_lines >= page->capacity) {
        if (page->num_lines >= RENDER_MAX_LINES)
            return &page->lines[page->num_lines - 1];
        uint16_t newcap = page->capacity + 64;
        if (newcap > RENDER_MAX_LINES) newcap = RENDER_MAX_LINES;
        render_line_t *nl = (render_line_t *)psram_alloc(newcap * sizeof(render_line_t));
        if (!nl) nl = (render_line_t *)malloc(newcap * sizeof(render_line_t));
        if (!nl) return &page->lines[page->num_lines - 1];
        memcpy(nl, page->lines, page->num_lines * sizeof(render_line_t));
        free(page->lines);
        page->lines = nl;
        page->capacity = newcap;
    }
    render_line_t *line = &page->lines[page->num_lines];
    __memset(line, 0, sizeof(render_line_t));
    /* link_id must be -1 (no link), not 0 (which is a valid link index) */
    {
        int ci;
        for (ci = 0; ci < RENDER_MAX_COLS; ci++)
            line->cells[ci].link_id = -1;
    }
    page->num_lines++;
    return line;
}

static render_line_t *current_line(render_ctx_t *ctx) {
    if (ctx->page->num_lines == 0)
        return ensure_line(ctx->page);
    return &ctx->page->lines[ctx->page->num_lines - 1];
}

static void new_line(render_ctx_t *ctx) {
    /* Collapse multiple blank lines: if the current line is blank
     * AND the previous line is also blank, don't add another. */
    if (ctx->page->num_lines >= 2) {
        render_line_t *cur = &ctx->page->lines[ctx->page->num_lines - 1];
        render_line_t *prev = &ctx->page->lines[ctx->page->num_lines - 2];
        if (cur->len == 0 && prev->len == 0)
            return;  /* already have a blank line, skip */
    }
    ensure_line(ctx->page);
    ctx->col = ctx->indent;
    ctx->last_was_space = true;
    ctx->at_line_start = true;
}

static uint8_t make_attr(const render_ctx_t *ctx) {
    uint8_t a = RATTR_NORMAL;
    if (ctx->bold || ctx->heading_level > 0) a |= RATTR_BOLD;
    if (ctx->underline || ctx->in_anchor) a |= RATTR_UNDERLINE;
    return a;
}

static uint8_t make_color(const render_ctx_t *ctx) {
    uint8_t fg = FG_NORMAL;
    if (ctx->in_anchor)
        fg = FG_LINK;
    else if (ctx->bold || ctx->heading_level > 0)
        fg = FG_BOLD;
    return RCELL_COLOR(fg, BG_DEFAULT);
}

static void put_char(render_ctx_t *ctx, char ch) {
    if (ctx->suppress_output) return;
    if (ctx->page->num_lines >= RENDER_MAX_LINES && ctx->col >= RENDER_MAX_COLS)
        return;

    /* Emit deferred bullet when real content arrives.
     * Temporarily clear anchor state so bullet/space don't
     * inherit the link_id from the <a> tag that follows. */
    if (ctx->pending_bullet_len > 0 && ch != ' ') {
        bool saved_anchor = ctx->in_anchor;
        int16_t saved_lid = ctx->current_link_id;
        ctx->in_anchor = false;
        ctx->current_link_id = -1;

        uint8_t bi;
        uint8_t saved_len = ctx->pending_bullet_len;
        ctx->pending_bullet_len = 0;
        for (bi = 0; bi < saved_len; bi++)
            put_char(ctx, ctx->pending_bullet[bi]);

        ctx->in_anchor = saved_anchor;
        ctx->current_link_id = saved_lid;
    }

    render_line_t *line = current_line(ctx);

    if (ctx->col >= RENDER_MAX_COLS) {
        new_line(ctx);
        line = current_line(ctx);
        /* Skip leading space after wrap */
        if (ch == ' ') return;
    }

    render_cell_t *cell = &line->cells[ctx->col];
    cell->ch = ch;
    cell->attr = make_attr(ctx);
    cell->color = make_color(ctx);
    cell->link_id = ctx->in_anchor ? (int8_t)ctx->current_link_id : -1;

    ctx->col++;
    if (ctx->col > line->len)
        line->len = ctx->col;
    if (ctx->heading_level > 0)
        line->heading = ctx->heading_level;

    ctx->last_was_space = (ch == ' ');
    ctx->at_line_start = false;
}

static void put_string(render_ctx_t *ctx, const char *s) {
    while (*s) {
        put_char(ctx, *s++);
    }
}

static void emit_block_gap(render_ctx_t *ctx) {
    /* Discard any pending bullet from an empty <li> */
    ctx->pending_bullet_len = 0;
    /* Ensure a blank line between block elements */
    if (ctx->page->num_lines > 0 && ctx->col > ctx->indent) {
        new_line(ctx);
    }
    if (ctx->page->num_lines > 1) {
        render_line_t *prev = &ctx->page->lines[ctx->page->num_lines - 2];
        if (prev->len > 0) {
            new_line(ctx);
        }
    }
    ctx->pending_paragraph = false;
    ctx->pending_br = false;
}

static void flush_pending(render_ctx_t *ctx) {
    if (ctx->pending_paragraph) {
        emit_block_gap(ctx);
    } else if (ctx->pending_br) {
        new_line(ctx);
        ctx->pending_br = false;
    }
}

static void update_indent(render_ctx_t *ctx) {
    uint8_t ind = 0;
    ind += ctx->blockquote_depth * 2;
    /* First list level has no indent (bullet provides hierarchy).
     * Nested lists indent by 2 chars per additional level. */
    if (ctx->list_depth > 1)
        ind += (ctx->list_depth - 1) * 2;
    if (ind > 20) ind = 20;
    ctx->indent = ind;
}

static int16_t add_link(render_ctx_t *ctx, const char *url) {
    render_page_t *pg = ctx->page;
    if (pg->num_links >= RENDER_MAX_LINKS) return -1;
    if (!pg->links) return -1;

    render_link_t *lnk = &pg->links[pg->num_links];
    __memset(lnk, 0, sizeof(render_link_t));
    strncpy(lnk->url, url, sizeof(lnk->url) - 1);
    lnk->url[sizeof(lnk->url) - 1] = '\0';
    lnk->start_line = (ctx->page->num_lines > 0) ? ctx->page->num_lines - 1 : 0;
    lnk->start_col = ctx->col;

    int16_t id = (int16_t)pg->num_links;
    pg->num_links++;
    return id;
}

static void close_link(render_ctx_t *ctx) {
    if (ctx->current_link_id >= 0 && ctx->current_link_id < ctx->page->num_links) {
        render_link_t *lnk = &ctx->page->links[ctx->current_link_id];
        lnk->end_line = (ctx->page->num_lines > 0) ? ctx->page->num_lines - 1 : 0;
        lnk->end_col = ctx->col;
    }
    ctx->in_anchor = false;
    ctx->current_link_id = -1;
    /* Add space after link so adjacent links don't run together */
    if (!ctx->at_line_start && ctx->col > 0)
        put_char(ctx, ' ');
}

/* ========================================================================
 * Tag classification helpers
 * ======================================================================== */

static bool is_block_tag(const char *tag) {
    if (manul_strcasecmp(tag, "p") == 0) return true;
    if (manul_strcasecmp(tag, "div") == 0) return true;
    if (manul_strcasecmp(tag, "section") == 0) return true;
    if (manul_strcasecmp(tag, "article") == 0) return true;
    if (manul_strcasecmp(tag, "aside") == 0) return true;
    if (manul_strcasecmp(tag, "header") == 0) return true;
    if (manul_strcasecmp(tag, "footer") == 0) return true;
    if (manul_strcasecmp(tag, "nav") == 0) return true;
    if (manul_strcasecmp(tag, "main") == 0) return true;
    if (manul_strcasecmp(tag, "figure") == 0) return true;
    if (manul_strcasecmp(tag, "figcaption") == 0) return true;
    if (manul_strcasecmp(tag, "details") == 0) return true;
    if (manul_strcasecmp(tag, "summary") == 0) return true;
    if (manul_strcasecmp(tag, "dd") == 0) return true;
    if (manul_strcasecmp(tag, "dt") == 0) return true;
    if (manul_strcasecmp(tag, "dl") == 0) return true;
    return false;
}

static bool is_heading_tag(const char *tag) {
    if (tag[0] != 'h' && tag[0] != 'H') return false;
    if (tag[1] >= '1' && tag[1] <= '6' && tag[2] == '\0') return true;
    return false;
}

static int heading_level(const char *tag) {
    if (is_heading_tag(tag)) return tag[1] - '0';
    return 0;
}

/* ========================================================================
 * Token attribute finder (case-insensitive)
 * ======================================================================== */

static const char *find_attr(const html_token_t *tok, const char *name) {
    if (manul_strcasecmp(name, "href") == 0 && tok->attr_href[0])
        return tok->attr_href;
    if (manul_strcasecmp(name, "alt") == 0 && tok->attr_alt[0])
        return tok->attr_alt;
    if (manul_strcasecmp(name, "type") == 0 && tok->attr_type[0])
        return tok->attr_type;
    if (manul_strcasecmp(name, "name") == 0 && tok->attr_name[0])
        return tok->attr_name;
    if (manul_strcasecmp(name, "value") == 0 && tok->attr_value[0])
        return tok->attr_value;
    if (manul_strcasecmp(name, "action") == 0 && tok->attr_action[0])
        return tok->attr_action;
    if (manul_strcasecmp(name, "src") == 0 && tok->attr_src[0])
        return tok->attr_src;
    return 0;
}

/* ========================================================================
 * Text emission with word-wrap and UTF-8 decoding
 * ======================================================================== */

static void emit_text(render_ctx_t *ctx, const char *text, uint16_t len) {
    if (ctx->suppress_output) return;

    const char *p = text;
    const char *end = text + len;

    while (p < end) {
        if (*p == '&') {
            char decoded;
            int consumed = decode_entity(p, &decoded);
            p += consumed;
            if (decoded == ' ') {
                if (!ctx->preformatted) {
                    if (!ctx->last_was_space && !ctx->at_line_start)
                        put_char(ctx, ' ');
                } else {
                    put_char(ctx, ' ');
                }
            } else {
                flush_pending(ctx);
                put_char(ctx, decoded);
            }
            continue;
        }

        if (ctx->preformatted) {
            if (*p == '\n') {
                new_line(ctx);
                p++;
                continue;
            }
            if (*p == '\t') {
                int spaces = 8 - (ctx->col % 8);
                int si;
                for (si = 0; si < spaces; si++) put_char(ctx, ' ');
                p++;
                continue;
            }
            int adv = 1;
            uint8_t ch = decode_utf8_char(p, &adv);
            put_char(ctx, (char)ch);
            p += adv;
            continue;
        }

        /* Collapse whitespace in normal mode */
        if (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') {
            if (!ctx->last_was_space && !ctx->at_line_start) {
                put_char(ctx, ' ');
            }
            p++;
            continue;
        }

        /* Word-wrap: scan ahead to find word length */
        const char *word_start = p;
        int word_len = 0;
        while (p < end && *p != ' ' && *p != '\t' && *p != '\n' && *p != '\r' && *p != '&') {
            int adv = 1;
            decode_utf8_char(p, &adv);
            word_len++;
            p += adv;
        }

        /* If the word would overflow, wrap to next line */
        if (ctx->col + word_len > RENDER_MAX_COLS && ctx->col > ctx->indent) {
            new_line(ctx);
        }

        flush_pending(ctx);

        /* Emit the word */
        const char *wp = word_start;
        while (wp < p) {
            int adv = 1;
            uint8_t ch = decode_utf8_char(wp, &adv);
            put_char(ctx, (char)ch);
            wp += adv;
        }
    }
}

/* ========================================================================
 * Handle open tag
 * ======================================================================== */

static void handle_open_tag(render_ctx_t *ctx, const html_token_t *tok) {
    const char *tag = tok->tag;

    /* <head> suppresses output */
    if (manul_strcasecmp(tag, "head") == 0) {
        ctx->in_head = true;
        ctx->suppress_output = true;
        return;
    }
    /* <title> inside head */
    if (manul_strcasecmp(tag, "title") == 0) {
        ctx->in_title = true;
        return;
    }
    /* <body> re-enables output */
    if (manul_strcasecmp(tag, "body") == 0) {
        ctx->in_head = false;
        ctx->suppress_output = false;
        return;
    }
    /* <script>, <style> suppress */
    if (manul_strcasecmp(tag, "script") == 0 || manul_strcasecmp(tag, "style") == 0) {
        ctx->suppress_output = true;
        return;
    }

    if (ctx->suppress_output && !ctx->in_title) return;

    /* Headings */
    if (is_heading_tag(tag)) {
        emit_block_gap(ctx);
        ctx->heading_level = (uint8_t)heading_level(tag);
        ctx->bold = true;
        return;
    }

    /* Paragraph */
    if (manul_strcasecmp(tag, "p") == 0) {
        ctx->pending_paragraph = true;
        return;
    }

    /* Line break */
    if (manul_strcasecmp(tag, "br") == 0) {
        new_line(ctx);
        return;
    }

    /* Horizontal rule */
    if (manul_strcasecmp(tag, "hr") == 0) {
        new_line(ctx);
        {
            int hi;
            for (hi = 0; hi < RENDER_MAX_COLS; hi++) put_char(ctx, '-');
        }
        new_line(ctx);
        return;
    }

    /* Preformatted */
    if (manul_strcasecmp(tag, "pre") == 0) {
        emit_block_gap(ctx);
        ctx->preformatted = true;
        return;
    }
    if (manul_strcasecmp(tag, "code") == 0 || manul_strcasecmp(tag, "tt") == 0) {
        /* Inline code - no special handling in text mode */
        return;
    }

    /* Anchor */
    if (manul_strcasecmp(tag, "a") == 0) {
        const char *href = find_attr(tok, "href");
        if (href && href[0]) {
            int16_t lid = add_link(ctx, href);
            if (lid >= 0) {
                ctx->in_anchor = true;
                ctx->current_link_id = lid;
            }
        }
        return;
    }

    /* Bold / strong */
    if (manul_strcasecmp(tag, "b") == 0 || manul_strcasecmp(tag, "strong") == 0) {
        ctx->bold = true;
        return;
    }

    /* Italic / emphasis - show as underline in text mode */
    if (manul_strcasecmp(tag, "i") == 0 || manul_strcasecmp(tag, "em") == 0) {
        ctx->underline = true;
        return;
    }

    /* Underline */
    if (manul_strcasecmp(tag, "u") == 0) {
        ctx->underline = true;
        return;
    }

    /* Unordered list */
    if (manul_strcasecmp(tag, "ul") == 0) {
        emit_block_gap(ctx);
        if (ctx->list_depth < 8) {
            ctx->ordered_list[ctx->list_depth] = false;
            ctx->list_counter[ctx->list_depth] = 0;
        }
        ctx->list_depth++;
        update_indent(ctx);
        return;
    }

    /* Ordered list */
    if (manul_strcasecmp(tag, "ol") == 0) {
        emit_block_gap(ctx);
        if (ctx->list_depth < 8) {
            ctx->ordered_list[ctx->list_depth] = true;
            ctx->list_counter[ctx->list_depth] = 0;
        }
        ctx->list_depth++;
        update_indent(ctx);
        return;
    }

    /* List item */
    if (manul_strcasecmp(tag, "li") == 0) {
        new_line(ctx);
        /* Defer bullet — only emit when actual content follows.
         * This hides empty list items (e.g. <li><img alt=""></li>). */
        ctx->pending_bullet_len = 0;
        {
            uint8_t si;
            for (si = 0; si < ctx->indent && ctx->pending_bullet_len < 7; si++)
                ctx->pending_bullet[ctx->pending_bullet_len++] = ' ';
        }
        if (ctx->list_depth > 0) {
            uint8_t li_depth = ctx->list_depth - 1;
            if (li_depth < 8 && ctx->ordered_list[li_depth]) {
                ctx->list_counter[li_depth]++;
                char nbuf[8];
                snprintf(nbuf, sizeof(nbuf), "%u.", (unsigned)ctx->list_counter[li_depth]);
                const char *np = nbuf;
                while (*np && ctx->pending_bullet_len < 7)
                    ctx->pending_bullet[ctx->pending_bullet_len++] = *np++;
            } else {
                if (ctx->pending_bullet_len < 7)
                    ctx->pending_bullet[ctx->pending_bullet_len++] = (char)0x95;
            }
            if (ctx->pending_bullet_len < 7)
                ctx->pending_bullet[ctx->pending_bullet_len++] = ' ';
        } else {
            if (ctx->pending_bullet_len < 7)
                ctx->pending_bullet[ctx->pending_bullet_len++] = (char)0x95;
            if (ctx->pending_bullet_len < 7)
                ctx->pending_bullet[ctx->pending_bullet_len++] = ' ';
        }
        return;
    }

    /* Blockquote */
    if (manul_strcasecmp(tag, "blockquote") == 0) {
        emit_block_gap(ctx);
        ctx->blockquote_depth++;
        update_indent(ctx);
        return;
    }

    /* Table elements */
    if (manul_strcasecmp(tag, "table") == 0) {
        emit_block_gap(ctx);
        return;
    }
    if (manul_strcasecmp(tag, "tr") == 0) {
        new_line(ctx);
        return;
    }
    if (manul_strcasecmp(tag, "td") == 0 || manul_strcasecmp(tag, "th") == 0) {
        if (!ctx->at_line_start) {
            put_string(ctx, " | ");
        }
        if (manul_strcasecmp(tag, "th") == 0) ctx->bold = true;
        return;
    }

    /* Image - show [alt] only if alt has meaningful text */
    if (manul_strcasecmp(tag, "img") == 0) {
        const char *alt = find_attr(tok, "alt");
        if (alt) {
            /* Check if alt has any alphanumeric content */
            const char *p = alt;
            bool has_alnum = false;
            while (*p && !has_alnum) {
                uint8_t c = (uint8_t)*p;
                if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                    (c >= '0' && c <= '9') || c >= 0xC0)  /* Cyrillic in UTF-8 */
                    has_alnum = true;
                p++;
            }
            if (has_alnum) {
                put_char(ctx, '[');
                p = alt;
                while (*p) {
                    int adv = 1;
                    uint8_t ch = decode_utf8_char(p, &adv);
                    put_char(ctx, (char)ch);
                    p += adv;
                }
                put_char(ctx, ']');
            }
            /* Empty or non-alnum alt: skip entirely */
        }
        /* No alt attribute: skip entirely (don't show [IMAGE]) */
        return;
    }

    /* Form elements */
    if (manul_strcasecmp(tag, "form") == 0) {
        emit_block_gap(ctx);
        const char *action = find_attr(tok, "action");
        if (action && action[0]) {
            put_string(ctx, "[FORM: ");
            put_string(ctx, action);
            put_char(ctx, ']');
            new_line(ctx);
        }
        return;
    }
    if (manul_strcasecmp(tag, "input") == 0) {
        const char *type = find_attr(tok, "type");
        const char *name = find_attr(tok, "name");
        const char *value = find_attr(tok, "value");
        put_char(ctx, '[');
        if (type && type[0]) {
            if (manul_strcasecmp(type, "submit") == 0) {
                put_char(ctx, '<');
                put_string(ctx, value ? value : "Submit");
                put_char(ctx, '>');
            } else if (manul_strcasecmp(type, "hidden") == 0) {
                /* Skip hidden inputs */
                return;
            } else {
                put_string(ctx, type);
                if (name && name[0]) { put_char(ctx, ':'); put_string(ctx, name); }
            }
        } else {
            put_string(ctx, "input");
            if (name && name[0]) { put_char(ctx, ':'); put_string(ctx, name); }
        }
        put_char(ctx, ']');
        return;
    }
    if (manul_strcasecmp(tag, "textarea") == 0) {
        const char *name = find_attr(tok, "name");
        put_string(ctx, "[textarea");
        if (name && name[0]) { put_char(ctx, ':'); put_string(ctx, name); }
        put_char(ctx, ']');
        new_line(ctx);
        return;
    }
    if (manul_strcasecmp(tag, "select") == 0) {
        put_string(ctx, "[select]");
        return;
    }
    if (manul_strcasecmp(tag, "option") == 0) {
        /* Options inside select - minimal rendering */
        return;
    }
    if (manul_strcasecmp(tag, "button") == 0) {
        put_string(ctx, "[<");
        return;
    }
    if (manul_strcasecmp(tag, "label") == 0) {
        /* Labels - just flow text */
        return;
    }

    /* Generic block tags */
    if (is_block_tag(tag)) {
        ctx->pending_paragraph = true;
        return;
    }

    /* <div> with line break semantics */
    if (manul_strcasecmp(tag, "div") == 0) {
        ctx->pending_paragraph = true;
        return;
    }

    /* Ignore unknown tags */
}

/* ========================================================================
 * Handle close tag
 * ======================================================================== */

static void handle_close_tag(render_ctx_t *ctx, const html_token_t *tok) {
    const char *tag = tok->tag;

    if (manul_strcasecmp(tag, "head") == 0) {
        ctx->in_head = false;
        ctx->suppress_output = false;
        return;
    }
    if (manul_strcasecmp(tag, "title") == 0) {
        ctx->in_title = false;
        return;
    }
    if (manul_strcasecmp(tag, "script") == 0 || manul_strcasecmp(tag, "style") == 0) {
        if (!ctx->in_head) ctx->suppress_output = false;
        return;
    }

    if (ctx->suppress_output) return;

    /* Headings */
    if (is_heading_tag(tag)) {
        ctx->heading_level = 0;
        ctx->bold = false;
        new_line(ctx);
        return;
    }

    /* Paragraph */
    if (manul_strcasecmp(tag, "p") == 0) {
        ctx->pending_paragraph = true;
        return;
    }

    /* Preformatted */
    if (manul_strcasecmp(tag, "pre") == 0) {
        ctx->preformatted = false;
        new_line(ctx);
        return;
    }

    /* Anchor */
    if (manul_strcasecmp(tag, "a") == 0) {
        close_link(ctx);
        return;
    }

    /* Bold / strong */
    if (manul_strcasecmp(tag, "b") == 0 || manul_strcasecmp(tag, "strong") == 0) {
        ctx->bold = false;
        return;
    }

    /* Italic / emphasis */
    if (manul_strcasecmp(tag, "i") == 0 || manul_strcasecmp(tag, "em") == 0) {
        ctx->underline = false;
        return;
    }

    /* Underline */
    if (manul_strcasecmp(tag, "u") == 0) {
        ctx->underline = false;
        return;
    }

    /* Lists */
    if (manul_strcasecmp(tag, "ul") == 0 || manul_strcasecmp(tag, "ol") == 0) {
        if (ctx->list_depth > 0) ctx->list_depth--;
        update_indent(ctx);
        new_line(ctx);
        return;
    }

    if (manul_strcasecmp(tag, "li") == 0) {
        /* No special action needed */
        return;
    }

    /* Blockquote */
    if (manul_strcasecmp(tag, "blockquote") == 0) {
        if (ctx->blockquote_depth > 0) ctx->blockquote_depth--;
        update_indent(ctx);
        new_line(ctx);
        return;
    }

    /* Table elements */
    if (manul_strcasecmp(tag, "table") == 0) {
        new_line(ctx);
        return;
    }
    if (manul_strcasecmp(tag, "th") == 0) {
        ctx->bold = false;
        return;
    }
    if (manul_strcasecmp(tag, "td") == 0) {
        return;
    }
    if (manul_strcasecmp(tag, "tr") == 0) {
        return;
    }

    /* Form */
    if (manul_strcasecmp(tag, "form") == 0) {
        new_line(ctx);
        return;
    }
    if (manul_strcasecmp(tag, "button") == 0) {
        put_string(ctx, ">]");
        return;
    }

    /* Generic block tags */
    if (is_block_tag(tag)) {
        ctx->pending_paragraph = true;
        return;
    }
    if (manul_strcasecmp(tag, "div") == 0) {
        ctx->pending_paragraph = true;
        return;
    }
}

/* ========================================================================
 * Public API
 * ======================================================================== */

void render_init(render_page_t *page) {
    __memset(page, 0, sizeof(render_page_t));
    /* Pre-allocate full capacity in PSRAM to avoid repeated realloc */
    page->capacity = RENDER_MAX_LINES;
    page->lines = (render_line_t *)psram_alloc(page->capacity * sizeof(render_line_t));
    if (!page->lines) {
        /* Fallback: smaller SRAM allocation */
        page->capacity = 64;
        page->lines = (render_line_t *)malloc(page->capacity * sizeof(render_line_t));
    }
    if (page->lines) {
        __memset(page->lines, 0, page->capacity * sizeof(render_line_t));
    }
    if (!page->links) {
        page->links = (render_link_t *)psram_alloc(RENDER_MAX_LINKS * sizeof(render_link_t));
        if (!page->links)
            page->links = (render_link_t *)malloc(RENDER_MAX_LINKS * sizeof(render_link_t));
    }
    page->num_links = 0;
    page->num_lines = 0;
    page->title[0] = '\0';
}

void render_clear(render_page_t *page) {
    /* Keep allocated buffers (PSRAM) — just reset counters.
     * This avoids repeated alloc/free between page navigations. */
    page->num_lines = 0;
    page->num_links = 0;
    page->title[0] = '\0';
}

void render_ctx_init(render_ctx_t *ctx, render_page_t *page) {
    __memset(ctx, 0, sizeof(render_ctx_t));
    ctx->page = page;
    ctx->col = 0;
    ctx->indent = 0;
    ctx->bold = false;
    ctx->underline = false;
    ctx->preformatted = false;
    ctx->in_anchor = false;
    ctx->current_link_id = -1;
    ctx->heading_level = 0;
    ctx->list_depth = 0;
    ctx->blockquote_depth = 0;
    ctx->last_was_space = true;
    ctx->at_line_start = true;
    ctx->pending_paragraph = false;
    ctx->pending_br = false;
    ctx->in_head = false;
    ctx->in_title = false;
    ctx->suppress_output = false;
}

void render_process_token(const void *token, void *user_ctx) {
    const html_token_t *tok = (const html_token_t *)token;
    render_ctx_t *ctx = (render_ctx_t *)user_ctx;

    if (tok->type == HTML_TOKEN_TEXT) {
        /* Title text capture */
        if (ctx->in_title && tok->text_len > 0) {
            /* Decode UTF-8 to Win1251 for the title */
            uint16_t cur = (uint16_t)strlen(ctx->page->title);
            const char *p = tok->text;
            const char *end = tok->text + tok->text_len;
            while (p < end && cur + 1 < sizeof(ctx->page->title)) {
                int adv = 1;
                uint8_t ch = decode_utf8_char(p, &adv);
                ctx->page->title[cur++] = (char)ch;
                p += adv;
            }
            ctx->page->title[cur] = '\0';
        }
        if (!ctx->suppress_output && tok->text_len > 0) {
            flush_pending(ctx);
            emit_text(ctx, tok->text, tok->text_len);
        }
    }
    else if (tok->type == HTML_TOKEN_TAG_OPEN) {
        handle_open_tag(ctx, tok);
    }
    else if (tok->type == HTML_TOKEN_TAG_CLOSE) {
        handle_close_tag(ctx, tok);
    }
    else if (tok->type == HTML_TOKEN_TAG_SELF_CLOSE) {
        /* Self-closing tags: treat like open for void elements */
        handle_open_tag(ctx, tok);
    }
}

void render_flush(render_ctx_t *ctx) {
    /* Close any open anchor */
    if (ctx->in_anchor)
        close_link(ctx);

    /* Ensure we have at least one line */
    if (ctx->page->num_lines == 0)
        ensure_line(ctx->page);
}

const render_line_t *render_get_line(const render_page_t *page, uint16_t idx) {
    if (idx >= page->num_lines) return 0;
    return &page->lines[idx];
}

const render_link_t *render_get_link(const render_page_t *page, uint16_t idx) {
    if (idx >= page->num_links) return 0;
    return &page->links[idx];
}

int16_t render_find_next_link(const render_page_t *page, int16_t current, int direction) {
    if (page->num_links == 0) return -1;

    if (current < 0) {
        /* No current link: return first or last */
        if (direction > 0) return 0;
        return (int16_t)(page->num_links - 1);
    }

    int16_t next = current + (int16_t)direction;
    if (next < 0) next = (int16_t)(page->num_links - 1);
    if (next >= (int16_t)page->num_links) next = 0;
    return next;
}
