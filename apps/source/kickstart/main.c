/*
 * FRANK OS — Kickstart (standalone ELF app)
 * UF2 firmware launcher with XML metadata display
 * Copyright (c) 2026 Mikhail Matveev <xtreme@rh1.tech>
 * https://rh1.tech
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "m-os-api.h"
#include "frankos-app.h"
#include "m-os-api-ff.h"
#include "lang.h"

/* App-local translations */
enum { AL_LAUNCH_FW, AL_FLASH_FW, AL_ABOUT, AL_COUNT };
static const char *al_en[] = {
    [AL_LAUNCH_FW] = "Launch Firmware",
    [AL_FLASH_FW]  = "Flash Firmware",
    [AL_ABOUT]     = "About Kickstart",
};
static const char *al_ru[] = {
    [AL_LAUNCH_FW] = "\xD0\x97\xD0\xB0\xD0\xBF\xD1\x83\xD1\x81\xD0\xBA \xD0\xBF\xD1\x80\xD0\xBE\xD1\x88\xD0\xB8\xD0\xB2\xD0\xBA\xD0\xB8",
    [AL_FLASH_FW]  = "\xD0\x9F\xD1\x80\xD0\xBE\xD1\x88\xD0\xB8\xD1\x82\xD1\x8C",
    [AL_ABOUT]     = "\xD0\x9E Kickstart",
};
static const char *AL(int id) { return lang_get() == LANG_RU ? al_ru[id] : al_en[id]; }

/* ========================================================================
 * Local string helpers (not provided by the ELF runtime)
 * ======================================================================== */

static const char *local_strrchr(const char *s, int c) {
    const char *last = NULL;
    while (*s) {
        if (*s == (char)c) last = s;
        s++;
    }
    if (c == '\0') return s;
    return last;
}

static const char *local_strstr(const char *hay, const char *needle) {
    if (!needle[0]) return hay;
    int nlen = 0;
    while (needle[nlen]) nlen++;
    while (*hay) {
        int i = 0;
        while (i < nlen && hay[i] == needle[i]) i++;
        if (i == nlen) return hay;
        hay++;
    }
    return NULL;
}

/* ========================================================================
 * Constants
 * ======================================================================== */

#define BASE_DIR        "/kickstart"
#define MAX_ENTRIES     32
#define MAX_PATH_LEN   128
#define MAX_NAME_LEN    48
#define MAX_TITLE_LEN   64
#define MAX_AUTHOR_LEN  64
#define MAX_VER_LEN     32
#define MAX_WEB_LEN    128
#define MAX_DESC_LEN   512
#define MAX_CTRL_LEN   128
#define XML_BUF_SIZE  1024

#define LIST_W         140   /* default left pane width */
#define ROW_H           14
#define BTN_H           22
#define BTN_GAP          6
#define DETAIL_PAD       6

/* Menu command IDs */
#define CMD_UNLOAD     100
#define CMD_EXIT       101
#define CMD_ABOUT      200

/* USB HID key codes */
#define KEY_ENTER   0x28
#define KEY_UP      0x52
#define KEY_DOWN    0x51
#define KEY_ESC     0x29

/* ========================================================================
 * Firmware entry
 * ======================================================================== */

typedef struct {
    char filename[MAX_NAME_LEN];      /* e.g. "game.uf2" */
    char basename[MAX_NAME_LEN];      /* e.g. "game" */
    char title[MAX_TITLE_LEN];
    char version[MAX_VER_LEN];
    char author[MAX_AUTHOR_LEN];
    char website[MAX_WEB_LEN];
    char description[MAX_DESC_LEN];
    char controls[MAX_CTRL_LEN];
    bool has_xml;
} fw_entry_t;

/* ========================================================================
 * App state
 * ======================================================================== */

typedef struct {
    hwnd_t      hwnd;
    fw_entry_t  entries[MAX_ENTRIES];
    int         count;
    int         selected;
    int         list_scroll;
    scrollbar_t list_sb;
    scrollbar_t detail_sb;
    int         detail_scroll;
    int         detail_total_lines;
    bool        flash_pending;
    int         flash_index;
    char        last_flashed[MAX_NAME_LEN]; /* filename of currently flashed fw */
} kickstart_t;

static kickstart_t ks;
static void *app_task;
static volatile bool app_closing;

static bool is_already_flashed(int index) {
    if (index < 0 || index >= ks.count) return false;
    return ks.last_flashed[0] &&
           strcmp(ks.entries[index].filename, ks.last_flashed) == 0;
}

/* ========================================================================
 * XML metadata extraction
 * ======================================================================== */

static void xml_extract(const char *xml, const char *tag,
                        char *out, int max_len) {
    char open[32], close[32];
    snprintf(open, sizeof(open), "<%s>", tag);
    snprintf(close, sizeof(close), "</%s>", tag);

    const char *s = local_strstr(xml, open);
    if (!s) { out[0] = '\0'; return; }
    s += strlen(open);

    const char *e = local_strstr(s, close);
    if (!e) { out[0] = '\0'; return; }

    int len = (int)(e - s);
    if (len >= max_len) len = max_len - 1;
    memcpy(out, s, len);
    out[len] = '\0';
}

static void load_metadata(fw_entry_t *entry) {
    char path[MAX_PATH_LEN];
    snprintf(path, sizeof(path), BASE_DIR "/%s.xml", entry->basename);

    FIL f;
    if (f_open(&f, path, FA_READ) != FR_OK) {
        entry->has_xml = false;
        return;
    }

    char buf[XML_BUF_SIZE];
    UINT br;
    if (f_read(&f, buf, XML_BUF_SIZE - 1, &br) != FR_OK) {
        f_close(&f);
        entry->has_xml = false;
        return;
    }
    buf[br] = '\0';
    f_close(&f);

    xml_extract(buf, "title",       entry->title,       MAX_TITLE_LEN);
    xml_extract(buf, "version",     entry->version,     MAX_VER_LEN);
    xml_extract(buf, "author",      entry->author,      MAX_AUTHOR_LEN);
    xml_extract(buf, "website",     entry->website,     MAX_WEB_LEN);
    xml_extract(buf, "description", entry->description, MAX_DESC_LEN);
    xml_extract(buf, "controls",    entry->controls,    MAX_CTRL_LEN);

    /* Use title from XML if available, fallback to basename */
    if (entry->title[0] == '\0')
        strncpy(entry->title, entry->basename, MAX_TITLE_LEN - 1);

    entry->has_xml = true;
}

/* ========================================================================
 * Firmware scanning
 * ======================================================================== */

static void scan_firmware(void) {
    ks.count = 0;
    ks.selected = 0;
    ks.list_scroll = 0;
    ks.detail_scroll = 0;

    DIR dir;
    FILINFO fno;
    if (f_opendir(&dir, BASE_DIR) != FR_OK) return;

    while (ks.count < MAX_ENTRIES) {
        if (f_readdir(&dir, &fno) != FR_OK || fno.fname[0] == '\0')
            break;
        if (fno.fattrib & AM_DIR) continue;

        /* Accept .uf2 and .m2p2 files */
        const char *dot = local_strrchr(fno.fname, '.');
        if (!dot) continue;
        if (strcmp(dot, ".uf2") != 0 && strcmp(dot, ".m2p2") != 0) continue;

        fw_entry_t *e = &ks.entries[ks.count];
        strncpy(e->filename, fno.fname, MAX_NAME_LEN - 1);
        e->filename[MAX_NAME_LEN - 1] = '\0';

        /* basename = filename without extension */
        int base_len = (int)(dot - fno.fname);
        if (base_len >= MAX_NAME_LEN) base_len = MAX_NAME_LEN - 1;
        memcpy(e->basename, fno.fname, base_len);
        e->basename[base_len] = '\0';

        /* Default title = basename */
        strncpy(e->title, e->basename, MAX_TITLE_LEN - 1);
        e->title[MAX_TITLE_LEN - 1] = '\0';

        /* Clear metadata */
        e->version[0] = '\0';
        e->author[0] = '\0';
        e->website[0] = '\0';
        e->description[0] = '\0';
        e->controls[0] = '\0';
        e->has_xml = false;

        load_metadata(e);
        ks.count++;
    }
    f_closedir(&dir);

    /* Sort alphabetically by title (case-insensitive insertion sort) */
    for (int i = 1; i < ks.count; i++) {
        fw_entry_t tmp;
        memcpy(&tmp, &ks.entries[i], sizeof(fw_entry_t));
        const char *key = tmp.title[0] ? tmp.title : tmp.basename;
        int j = i - 1;
        while (j >= 0) {
            const char *ej = ks.entries[j].title[0]
                           ? ks.entries[j].title : ks.entries[j].basename;
            /* case-insensitive compare */
            const char *a = ej;
            const char *b = key;
            int cmp = 0;
            while (*a && *b) {
                char ca = *a; if (ca >= 'a' && ca <= 'z') ca -= 32;
                char cb = *b; if (cb >= 'a' && cb <= 'z') cb -= 32;
                if (ca != cb) { cmp = ca - cb; break; }
                a++; b++;
            }
            if (cmp == 0) cmp = (unsigned char)*a - (unsigned char)*b;
            if (cmp <= 0) break;
            memcpy(&ks.entries[j + 1], &ks.entries[j], sizeof(fw_entry_t));
            j--;
        }
        memcpy(&ks.entries[j + 1], &tmp, sizeof(fw_entry_t));
    }

    /* Read last-flashed firmware from .last file and pre-select it */
    ks.last_flashed[0] = '\0';
    {
        FIL lf;
        if (ks.count > 0 &&
            f_open(&lf, BASE_DIR "/.last", FA_READ) == FR_OK) {
            char last_name[MAX_NAME_LEN];
            UINT br;
            last_name[0] = '\0';
            f_read(&lf, last_name, sizeof(last_name) - 1, &br);
            f_close(&lf);
            last_name[br] = '\0';
            /* Strip trailing whitespace/newline */
            while (br > 0 && (last_name[br-1] == '\n' ||
                              last_name[br-1] == '\r' ||
                              last_name[br-1] == ' ')) {
                last_name[--br] = '\0';
            }
            for (int i = 0; i < ks.count; i++) {
                if (strcmp(ks.entries[i].filename, last_name) == 0) {
                    ks.selected = i;
                    strncpy(ks.last_flashed, last_name,
                            MAX_NAME_LEN - 1);
                    break;
                }
            }
        }
    }
}

/* ========================================================================
 * Detail text rendering helpers
 * ======================================================================== */

/* Count lines needed to word-wrap a string into a given pixel width */
static int count_wrapped_lines(const char *text, int max_chars) {
    if (!text || text[0] == '\0') return 0;

    int lines = 0;
    const char *p = text;
    while (*p) {
        /* Find end of current line segment */
        int col = 0;
        const char *line_start = p;
        const char *last_space = NULL;

        while (*p && *p != '\n') {
            if (*p == ' ') last_space = p;
            col++;
            if (col >= max_chars) {
                if (last_space && last_space > line_start) {
                    p = last_space + 1;
                } else {
                    /* no space found, hard break */
                }
                break;
            }
            p++;
        }
        lines++;
        if (*p == '\n') p++;
    }
    return lines;
}

/* Draw word-wrapped text, returning number of lines drawn.
 * skip = lines to skip (for scrolling), max_lines = max to draw */
static int draw_wrapped_text(const char *text, int16_t x, int16_t y,
                             int max_chars, uint8_t fg, uint8_t bg,
                             int skip, int max_lines) {
    if (!text || text[0] == '\0') return 0;

    int line = 0;
    int drawn = 0;
    const char *p = text;

    while (*p && drawn < max_lines) {
        char linebuf[80];
        int col = 0;
        const char *line_start = p;
        const char *last_space = NULL;
        int last_space_col = 0;

        while (*p && *p != '\n') {
            if (*p == ' ') { last_space = p; last_space_col = col; }
            col++;
            if (col >= max_chars) {
                if (last_space && last_space > line_start) {
                    col = last_space_col;
                    p = last_space + 1;
                }
                break;
            }
            p++;
        }

        if (line >= skip) {
            int cpy = col;
            if (cpy >= (int)sizeof(linebuf)) cpy = (int)sizeof(linebuf) - 1;
            memcpy(linebuf, line_start, cpy);
            linebuf[cpy] = '\0';
            wd_text_ui(x, y + drawn * ROW_H, linebuf, fg, bg);
            drawn++;
        }

        line++;
        if (*p == '\n') p++;
    }
    return line;  /* total lines including skipped */
}

/* ========================================================================
 * Layout helpers
 * ======================================================================== */

static void get_layout(rect_t cr, int16_t *list_w, int16_t *detail_x,
                       int16_t *detail_w, int16_t *btn_y,
                       int16_t *list_rows, int16_t *detail_rows) {
    *list_w = LIST_W;
    if (*list_w > cr.w / 2) *list_w = cr.w / 2;
    *detail_x = *list_w + 2;
    *detail_w = cr.w - *detail_x;

    /* Flash button at bottom of list pane */
    *btn_y = cr.h - BTN_H - 4;
    *list_rows = (*btn_y - 2) / ROW_H;
    if (*list_rows < 1) *list_rows = 1;

    *detail_rows = (cr.h - 4) / ROW_H;
    if (*detail_rows < 1) *detail_rows = 1;
}

/* ========================================================================
 * Paint handler
 * ======================================================================== */

static void paint(hwnd_t hwnd) {
    wd_begin(hwnd);

    rect_t cr = wm_get_client_rect(hwnd);
    int16_t lw, dx, dw, by, lr, dr;
    get_layout(cr, &lw, &dx, &dw, &by, &lr, &dr);

    /* ---- Left pane: firmware list ---- */
    int16_t list_content_w = lw - SCROLLBAR_WIDTH;
    int list_chars = list_content_w / FONT_UI_WIDTH;
    if (list_chars < 1) list_chars = 1;

    for (int row = 0; row < lr; row++) {
        int idx = ks.list_scroll + row;
        int16_t ry = 2 + row * ROW_H;

        if (idx >= ks.count) {
            wd_fill_rect(0, ry, list_content_w, ROW_H, COLOR_WHITE);
            continue;
        }

        fw_entry_t *e = &ks.entries[idx];
        uint8_t bg = (idx == ks.selected) ? COLOR_BLUE : COLOR_WHITE;
        uint8_t fg = (idx == ks.selected) ? COLOR_WHITE : COLOR_BLACK;

        wd_fill_rect(0, ry, list_content_w, ROW_H, bg);

        /* Truncate with "..." if too long */
        char disp[48];
        const char *name = e->title[0] ? e->title : e->basename;
        int nlen = strlen(name);
        if (nlen > list_chars && list_chars > 3) {
            int cpy = list_chars - 3;
            if (cpy >= (int)sizeof(disp) - 4) cpy = (int)sizeof(disp) - 4;
            memcpy(disp, name, cpy);
            disp[cpy] = '.'; disp[cpy+1] = '.'; disp[cpy+2] = '.';
            disp[cpy+3] = '\0';
        } else {
            int cpy = nlen;
            if (cpy >= (int)sizeof(disp)) cpy = (int)sizeof(disp) - 1;
            memcpy(disp, name, cpy);
            disp[cpy] = '\0';
        }

        wd_text_ui(2, ry + 1, disp, fg, bg);
    }

    /* List scrollbar */
    int16_t list_h = by - 4 - 2;  /* from y=2 to button area */
    ks.list_sb.x = list_content_w;
    ks.list_sb.y = 2;
    ks.list_sb.w = SCROLLBAR_WIDTH;
    ks.list_sb.h = list_h;
    if (ks.count > lr) {
        ks.list_sb.visible = true;
        scrollbar_set_range(&ks.list_sb, ks.count, lr);
        scrollbar_set_pos(&ks.list_sb, ks.list_scroll);
    } else {
        ks.list_sb.visible = false;
        wd_fill_rect(list_content_w, 2, SCROLLBAR_WIDTH,
                     list_h, COLOR_WHITE);
    }
    scrollbar_paint(&ks.list_sb);

    /* Separator line */
    wd_vline(lw, 0, cr.h, COLOR_DARK_GRAY);
    wd_vline(lw + 1, 0, cr.h, COLOR_WHITE);

    /* ---- Flash button at bottom ---- */
    int16_t btn_w = lw - BTN_GAP * 2;
    if (btn_w < 40) btn_w = 40;
    bool have_sel = (ks.selected >= 0 && ks.selected < ks.count);

    wd_fill_rect(0, by - 4, lw, 4, THEME_BUTTON_FACE);
    wd_bevel_rect(BTN_GAP, by, btn_w, BTN_H,
                  COLOR_WHITE, COLOR_DARK_GRAY, THEME_BUTTON_FACE);
    {
        const char *lbl = is_already_flashed(ks.selected) ? "Launch" : "Flash";
        int tw = strlen(lbl) * FONT_UI_WIDTH;
        int16_t tx = BTN_GAP + (btn_w - tw) / 2;
        int16_t ty = by + (BTN_H - FONT_UI_HEIGHT) / 2;
        wd_text_ui(tx, ty, lbl,
                   have_sel ? COLOR_BLACK : COLOR_DARK_GRAY,
                   THEME_BUTTON_FACE);
    }
    wd_fill_rect(0, by + BTN_H, lw, cr.h - by - BTN_H, THEME_BUTTON_FACE);

    /* ---- Right pane: detail view ---- */
    int16_t detail_content_w = dw - SCROLLBAR_WIDTH - DETAIL_PAD * 2;
    if (detail_content_w < 20) detail_content_w = 20;
    int detail_chars = detail_content_w / FONT_UI_WIDTH;
    if (detail_chars < 1) detail_chars = 1;

    /* Clear only the content area, not the scrollbar column */
    wd_fill_rect(dx, 0, dw - SCROLLBAR_WIDTH, cr.h, THEME_BUTTON_FACE);

    if (ks.selected >= 0 && ks.selected < ks.count) {
        fw_entry_t *e = &ks.entries[ks.selected];
        int16_t tx = dx + DETAIL_PAD;
        int line = 0;
        int skip = ks.detail_scroll;

        /* Title */
        {
            const char *t = e->title[0] ? e->title : e->basename;
            int tl = count_wrapped_lines(t, detail_chars);
            if (line + tl > skip && line < skip + dr) {
                int s = (skip > line) ? skip - line : 0;
                int ml = dr - (line - skip > 0 ? line - skip : 0);
                draw_wrapped_text(t, tx, 4 + (line - skip + s) * ROW_H,
                                  detail_chars, COLOR_BLACK,
                                  THEME_BUTTON_FACE, s, ml);
            }
            line += tl;
        }
        line++;  /* blank after title */

        /* Version */
        if (e->version[0]) {
            if (line >= skip && line < skip + dr) {
                char tmp[80];
                snprintf(tmp, sizeof(tmp), "Version: %s", e->version);
                wd_text_ui(tx, 4 + (line - skip) * ROW_H, tmp,
                           COLOR_BLACK, THEME_BUTTON_FACE);
            }
            line++;
        }

        /* Author */
        if (e->author[0]) {
            if (line >= skip && line < skip + dr) {
                char tmp[80];
                snprintf(tmp, sizeof(tmp), "Author: %s", e->author);
                wd_text_ui(tx, 4 + (line - skip) * ROW_H, tmp,
                           COLOR_BLACK, THEME_BUTTON_FACE);
            }
            line++;
        }

        /* Website */
        if (e->website[0]) {
            if (line >= skip && line < skip + dr) {
                char tmp[80];
                snprintf(tmp, sizeof(tmp), "Web: %s", e->website);
                int s = (skip > line) ? skip - line : 0;
                int ml = dr - (line > skip ? line - skip : 0);
                draw_wrapped_text(tmp, tx, 4 + (line - skip + s) * ROW_H,
                                  detail_chars, COLOR_BLUE,
                                  THEME_BUTTON_FACE, s, ml);
            }
            line += count_wrapped_lines(e->website, detail_chars);
        }

        /* Blank separator before description/controls */
        if (e->description[0] || e->controls[0])
            line++;

        /* Description */
        if (e->description[0]) {
            if (line >= skip && line < skip + dr) {
                wd_text_ui(tx, 4 + (line - skip) * ROW_H,
                           "Description:", COLOR_DARK_GRAY,
                           THEME_BUTTON_FACE);
            }
            line++;
            {
                int dl = count_wrapped_lines(e->description, detail_chars);
                if (line + dl > skip && line < skip + dr) {
                    int s = (skip > line) ? skip - line : 0;
                    int ml = dr - (line > skip ? line - skip : 0);
                    draw_wrapped_text(e->description, tx,
                                      4 + (line - skip + s) * ROW_H,
                                      detail_chars, COLOR_BLACK,
                                      THEME_BUTTON_FACE, s, ml);
                }
                line += dl;
            }
        }

        /* Controls */
        if (e->controls[0]) {
            line++;  /* gap before controls */
            if (line >= skip && line < skip + dr) {
                wd_text_ui(tx, 4 + (line - skip) * ROW_H,
                           "Controls:", COLOR_DARK_GRAY,
                           THEME_BUTTON_FACE);
            }
            line++;
            {
                int cl = count_wrapped_lines(e->controls, detail_chars);
                if (line + cl > skip && line < skip + dr) {
                    int s = (skip > line) ? skip - line : 0;
                    int ml = dr - (line > skip ? line - skip : 0);
                    draw_wrapped_text(e->controls, tx,
                                      4 + (line - skip + s) * ROW_H,
                                      detail_chars, COLOR_BLACK,
                                      THEME_BUTTON_FACE, s, ml);
                }
                line += cl;
            }
        }

        /* File name */
        line++;
        if (line >= skip && line < skip + dr) {
            char tmp[80];
            snprintf(tmp, sizeof(tmp), "File: %s", e->filename);
            wd_text_ui(tx, 4 + (line - skip) * ROW_H, tmp,
                       COLOR_DARK_GRAY, THEME_BUTTON_FACE);
        }
        line++;

        ks.detail_total_lines = line;
    } else {
        ks.detail_total_lines = 0;
        if (ks.count == 0) {
            wd_text_ui(dx + DETAIL_PAD, 4,
                       "No firmware found.", COLOR_DARK_GRAY,
                       THEME_BUTTON_FACE);
            wd_text_ui(dx + DETAIL_PAD, 4 + ROW_H,
                       "Place .uf2 files in", COLOR_DARK_GRAY,
                       THEME_BUTTON_FACE);
            wd_text_ui(dx + DETAIL_PAD, 4 + ROW_H * 2,
                       BASE_DIR "/", COLOR_DARK_GRAY,
                       THEME_BUTTON_FACE);
            wd_text_ui(dx + DETAIL_PAD, 4 + ROW_H * 4,
                       "Optional: add .xml files", COLOR_DARK_GRAY,
                       THEME_BUTTON_FACE);
            wd_text_ui(dx + DETAIL_PAD, 4 + ROW_H * 5,
                       "with metadata (title,", COLOR_DARK_GRAY,
                       THEME_BUTTON_FACE);
            wd_text_ui(dx + DETAIL_PAD, 4 + ROW_H * 6,
                       "author, description).", COLOR_DARK_GRAY,
                       THEME_BUTTON_FACE);
        }
    }

    /* Detail scrollbar */
    ks.detail_sb.x = cr.w - SCROLLBAR_WIDTH;
    ks.detail_sb.y = 0;
    ks.detail_sb.w = SCROLLBAR_WIDTH;
    ks.detail_sb.h = cr.h;
    if (ks.detail_total_lines > dr) {
        ks.detail_sb.visible = true;
        scrollbar_set_range(&ks.detail_sb, ks.detail_total_lines, dr);
        scrollbar_set_pos(&ks.detail_sb, ks.detail_scroll);
    } else {
        ks.detail_sb.visible = false;
        wd_fill_rect(cr.w - SCROLLBAR_WIDTH, 0,
                     SCROLLBAR_WIDTH, cr.h, THEME_BUTTON_FACE);
    }
    scrollbar_paint(&ks.detail_sb);

    wd_end();
}

/* ========================================================================
 * Flash firmware
 * ======================================================================== */

static void do_flash(int index) {
    if (index < 0 || index >= ks.count) return;

    /* Already flashed — just reboot into it */
    if (ks.last_flashed[0] &&
        strcmp(ks.entries[index].filename, ks.last_flashed) == 0) {
        system_reboot_to_firmware();
        return;  /* never reached */
    }

    /* Save last-flashed filename before flashing (won't return on success) */
    {
        FIL lf;
        if (f_open(&lf, BASE_DIR "/.last",
                   FA_WRITE | FA_CREATE_ALWAYS) == FR_OK) {
            UINT bw;
            f_write(&lf, ks.entries[index].filename,
                    strlen(ks.entries[index].filename), &bw);
            f_close(&lf);
        }
    }

    char path[MAX_PATH_LEN];
    snprintf(path, sizeof(path), BASE_DIR "/%s",
             ks.entries[index].filename);

    load_firmware(path);
    /* If load_firmware returns, it failed — just invalidate */
    wm_invalidate(ks.hwnd);
}

/* ========================================================================
 * Ensure selected item is visible in list
 * ======================================================================== */

static void ensure_visible(int list_rows) {
    if (ks.selected < ks.list_scroll)
        ks.list_scroll = ks.selected;
    else if (ks.selected >= ks.list_scroll + list_rows)
        ks.list_scroll = ks.selected - list_rows + 1;
    if (ks.list_scroll < 0) ks.list_scroll = 0;
}

/* ========================================================================
 * Event handler
 * ======================================================================== */

static void show_flash_dialog(hwnd_t hwnd, const char *name) {
    static char dlg_text[256];
    if (is_already_flashed(ks.selected)) {
        snprintf(dlg_text, sizeof(dlg_text),
                 "Launch \"%s\"?\n\n"
                 "Already flashed, will reboot.", name);
        ks.flash_pending = true;
        ks.flash_index = ks.selected;
        dialog_show(hwnd, AL(AL_LAUNCH_FW), dlg_text,
                    DLG_ICON_INFO, DLG_BTN_OK | DLG_BTN_CANCEL);
    } else {
        snprintf(dlg_text, sizeof(dlg_text),
                 "Flash \"%s\"?\n\n"
                 "Screen will turn off during\n"
                 "flashing. Please wait.", name);
        ks.flash_pending = true;
        ks.flash_index = ks.selected;
        dialog_show(hwnd, AL(AL_FLASH_FW), dlg_text,
                    DLG_ICON_WARNING, DLG_BTN_OK | DLG_BTN_CANCEL);
    }
}

static bool event_handler(hwnd_t hwnd, const window_event_t *event) {
    rect_t cr = wm_get_client_rect(hwnd);
    int16_t lw, dx, dw, by, lr, dr;
    get_layout(cr, &lw, &dx, &dw, &by, &lr, &dr);

    uint8_t type = event->type;

    if (type == WM_CLOSE) {
        app_closing = true;
        xTaskNotifyGive(app_task);
        return true;
    }

    if (type == WM_SIZE) {
        ks.detail_scroll = 0;
        wm_invalidate(hwnd);
        return true;
    }

    if (type == WM_COMMAND) {
        uint16_t id = event->command.id;
        if (id == DLG_RESULT_OK && ks.flash_pending) {
            ks.flash_pending = false;
            do_flash(ks.flash_index);
            return true;
        }
        if (id == DLG_RESULT_CANCEL) {
            ks.flash_pending = false;
            return true;
        }
        if (id == CMD_UNLOAD) {
            /* Delete .last file so all firmware re-flashes fresh */
            f_unlink(BASE_DIR "/.last");
            ks.last_flashed[0] = '\0';
            wm_invalidate(hwnd);
            return true;
        }
        if (id == CMD_EXIT) {
            app_closing = true;
            xTaskNotifyGive(app_task);
            return true;
        }
        if (id == CMD_ABOUT) {
            dialog_show(hwnd, AL(AL_ABOUT),
                        "Kickstart\n\nFRANK OS v" FRANK_VERSION_STR
                        "\nUF2 Firmware Launcher\n"
                        "(c) 2026 Mikhail Matveev\n"
                        "<xtreme@rh1.tech>\n"
                        "github.com/rh1tech/frank-os",
                        DLG_ICON_INFO, DLG_BTN_OK);
            return true;
        }
        return false;
    }

    if (type == WM_KEYDOWN) {
        uint8_t sc = event->key.scancode;

        if (sc == KEY_UP && ks.count > 0) {
            if (ks.selected > 0) {
                ks.selected--;
                ks.detail_scroll = 0;
                ensure_visible(lr);
                wm_invalidate(hwnd);
            }
            return true;
        }
        if (sc == KEY_DOWN && ks.count > 0) {
            if (ks.selected < ks.count - 1) {
                ks.selected++;
                ks.detail_scroll = 0;
                ensure_visible(lr);
                wm_invalidate(hwnd);
            }
            return true;
        }
        if (sc == KEY_ENTER && ks.count > 0) {
            fw_entry_t *e = &ks.entries[ks.selected];
            show_flash_dialog(hwnd, e->title[0] ? e->title : e->basename);
            return true;
        }
        if (sc == KEY_ESC) {
            app_closing = true;
            xTaskNotifyGive(app_task);
            return true;
        }
        if (sc == 0x3A) { /* F1 */
            window_event_t ce = {0}; ce.type = WM_COMMAND; ce.command.id = CMD_ABOUT;
            wm_post_event(hwnd, &ce); return true;
        }
        return false;
    }

    if (type == WM_LBUTTONDOWN) {
        int16_t mx = event->mouse.x;
        int16_t my = event->mouse.y;

        /* Check list area click */
        if (mx < lw - SCROLLBAR_WIDTH && my >= 2 && my < 2 + lr * ROW_H) {
            int row = (my - 2) / ROW_H;
            int idx = ks.list_scroll + row;
            if (idx < ks.count && idx != ks.selected) {
                ks.selected = idx;
                ks.detail_scroll = 0;
                wm_invalidate(hwnd);
            }
            return true;
        }

        /* Check Flash button */
        {
            int16_t btn_w = lw - BTN_GAP * 2;
            if (btn_w < 40) btn_w = 40;

            if (mx >= BTN_GAP && mx < BTN_GAP + btn_w &&
                my >= by && my < by + BTN_H &&
                ks.selected >= 0 && ks.selected < ks.count) {
                fw_entry_t *e = &ks.entries[ks.selected];
                show_flash_dialog(hwnd,
                                  e->title[0] ? e->title : e->basename);
                return true;
            }
        }

        /* List scrollbar */
        {
            int32_t new_pos;
            if (scrollbar_event(&ks.list_sb, event, &new_pos)) {
                ks.list_scroll = new_pos;
                wm_invalidate(hwnd);
                return true;
            }
        }
        /* Detail scrollbar */
        {
            int32_t new_pos;
            if (scrollbar_event(&ks.detail_sb, event, &new_pos)) {
                ks.detail_scroll = new_pos;
                wm_invalidate(hwnd);
                return true;
            }
        }
        return false;
    }

    if (type == WM_LBUTTONUP || type == WM_MOUSEMOVE) {
        int32_t new_pos;
        if (scrollbar_event(&ks.list_sb, event, &new_pos)) {
            ks.list_scroll = new_pos;
            wm_invalidate(hwnd);
            return true;
        }
        if (scrollbar_event(&ks.detail_sb, event, &new_pos)) {
            ks.detail_scroll = new_pos;
            wm_invalidate(hwnd);
            return true;
        }
        return false;
    }

    if (type == WM_DROPFILES) {
        if (event->dropfiles.file_path) {
            const char *dot = local_strrchr(event->dropfiles.file_path, '.');
            if (dot && (strcmp(dot, ".uf2") == 0 ||
                        strcmp(dot, ".m2p2") == 0)) {
                show_flash_dialog(hwnd, event->dropfiles.file_path);
            }
        }
        return true;
    }

    return false;
}

/* ========================================================================
 * Menu definition
 * ======================================================================== */

static void setup_menu(hwnd_t hwnd) {
    menu_bar_t bar;
    memset(&bar, 0, sizeof(bar));
    bar.menu_count = 2;

    menu_def_t *file = &bar.menus[0];
    strncpy(file->title, L(STR_FILE), sizeof(file->title) - 1);
    file->accel_key = 0x09; /* HID 'F' — Alt+F */
    file->item_count = 2;
    strncpy(file->items[0].text, "Unload firmware", sizeof(file->items[0].text) - 1);
    file->items[0].command_id = CMD_UNLOAD;
    strncpy(file->items[1].text, L(STR_FM_EXIT), sizeof(file->items[1].text) - 1);
    file->items[1].command_id = CMD_EXIT;

    menu_def_t *help = &bar.menus[1];
    strncpy(help->title, L(STR_HELP), sizeof(help->title) - 1);
    help->accel_key = 0x0B; /* HID 'H' — Alt+H */
    help->item_count = 1;
    strncpy(help->items[0].text, L(STR_FM_ABOUT_MENU), sizeof(help->items[0].text) - 1);
    help->items[0].command_id = CMD_ABOUT;
    help->items[0].accel_key = 0x3A;

    menu_set(hwnd, &bar);
}

/* ========================================================================
 * Entry point
 * ======================================================================== */

/* ========================================================================
 * Direct UF2 launch (invoked from Navigator with argv[1] = path)
 * ======================================================================== */

static char direct_uf2_path[MAX_PATH_LEN];

/* Check if the given full path matches the last-flashed firmware */
static bool direct_is_already_flashed(const char *path) {
    FIL lf;
    if (f_open(&lf, BASE_DIR "/.last", FA_READ) != FR_OK) return false;
    char last[MAX_NAME_LEN];
    UINT br;
    last[0] = '\0';
    f_read(&lf, last, sizeof(last) - 1, &br);
    f_close(&lf);
    last[br] = '\0';
    while (br > 0 && (last[br-1] == '\n' || last[br-1] == '\r'
                      || last[br-1] == ' '))
        last[--br] = '\0';
    if (!last[0]) return false;

    /* Compare basename of path with last-flashed filename */
    const char *base = local_strrchr(path, '/');
    base = base ? base + 1 : path;
    return strcmp(base, last) == 0;
}

static void direct_do_flash(void) {
    const char *path = direct_uf2_path;

    /* Already flashed — just reboot */
    if (direct_is_already_flashed(path)) {
        system_reboot_to_firmware();
        return; /* never reached */
    }

    /* Save last-flashed (basename only) */
    const char *base = local_strrchr(path, '/');
    base = base ? base + 1 : path;
    {
        FIL lf;
        if (f_open(&lf, BASE_DIR "/.last",
                   FA_WRITE | FA_CREATE_ALWAYS) == FR_OK) {
            UINT bw;
            f_write(&lf, base, strlen(base), &bw);
            f_close(&lf);
        }
    }

    load_firmware((char *)path);
    /* If we get here, flash failed */
    wm_invalidate(ks.hwnd);
}

static bool direct_event_handler(hwnd_t hwnd, const window_event_t *event) {
    if (event->type == WM_CLOSE) {
        app_closing = true;
        xTaskNotifyGive(app_task);
        return true;
    }
    if (event->type == WM_COMMAND) {
        uint16_t id = event->command.id;
        if (id == DLG_RESULT_OK) {
            direct_do_flash();
            return true;
        }
        if (id == DLG_RESULT_CANCEL) {
            app_closing = true;
            xTaskNotifyGive(app_task);
            return true;
        }
    }
    return false;
}

static void direct_paint(hwnd_t hwnd) {
    (void)hwnd;
}

static int run_direct(const char *path) {
    app_task = xTaskGetCurrentTaskHandle();
    app_closing = false;

    strncpy(direct_uf2_path, path, MAX_PATH_LEN - 1);
    direct_uf2_path[MAX_PATH_LEN - 1] = '\0';

    const char *name = local_strrchr(path, '/');
    name = name ? name + 1 : path;

    /* Create a hidden window just to receive dialog WM_COMMAND results */
    ks.hwnd = wm_create_window(-1, -1, 1, 1, "",
                                0, direct_event_handler, direct_paint);
    if (ks.hwnd == HWND_NULL) return 1;

    static char dlg_text[256];
    if (direct_is_already_flashed(path)) {
        snprintf(dlg_text, sizeof(dlg_text),
                 "Launch \"%s\"?\n\n"
                 "Already flashed, will reboot.", name);
        dialog_show(ks.hwnd, AL(AL_LAUNCH_FW), dlg_text,
                    DLG_ICON_INFO, DLG_BTN_OK | DLG_BTN_CANCEL);
    } else {
        snprintf(dlg_text, sizeof(dlg_text),
                 "Flash \"%s\"?\n\n"
                 "Screen will turn off during\n"
                 "flashing. Please wait.", name);
        dialog_show(ks.hwnd, AL(AL_FLASH_FW), dlg_text,
                    DLG_ICON_WARNING, DLG_BTN_OK | DLG_BTN_CANCEL);
    }

    while (!app_closing) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    }

    wm_destroy_window(ks.hwnd);
    return 0;
}

/* ========================================================================
 * Entry point
 * ======================================================================== */

int main(int argc, char **argv) {
    /* Direct UF2 launch from Navigator */
    if (argc >= 2 && argv[1] && argv[1][0]) {
        return run_direct(argv[1]);
    }

    app_task = xTaskGetCurrentTaskHandle();

    memset(&ks, 0, sizeof(ks));
    ks.selected = 0;
    ks.flash_index = -1;

    scrollbar_init(&ks.list_sb, false);
    scrollbar_init(&ks.detail_sb, false);

    /* Scan firmware before creating window */
    scan_firmware();

    ks.hwnd = wm_create_window(60, 40, 460, 320, "Kickstart",
                                WSTYLE_DEFAULT | WF_MENUBAR,
                                event_handler, paint);
    if (ks.hwnd == HWND_NULL) return 1;

    setup_menu(ks.hwnd);
    wm_show_window(ks.hwnd);
    wm_set_focus(ks.hwnd);

    while (!app_closing) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    }

    wm_destroy_window(ks.hwnd);
    return 0;
}
