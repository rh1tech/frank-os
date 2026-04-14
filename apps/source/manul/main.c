/*
 * Manul — Text Web Browser for FRANK OS
 * Copyright (c) 2026 Mikhail Matveev <xtreme@rh1.tech>
 * https://rh1.tech
 *
 * Lynx-inspired text browser with GUI toolbar, clickable links,
 * and keyboard navigation.  Based on frank-lynx rendering engine.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "manul.h"
#include "lang.h"

/* App-local translations */
enum { AL_NAVIGATE, AL_BACK, AL_FORWARD, AL_STOP, AL_RELOAD, AL_NEXT_LINK, AL_PREV_LINK, AL_ABOUT, AL_COUNT };
static const char *al_en[] = {
    [AL_NAVIGATE]  = "Navigate",
    [AL_BACK]      = "Back     Alt+<",
    [AL_FORWARD]   = "Forward  Alt+>",
    [AL_STOP]      = "Stop       Esc",
    [AL_RELOAD]    = "Reload      F5",
    [AL_NEXT_LINK] = "Next link  Tab",
    [AL_PREV_LINK] = "Prev link S+Tb",
    [AL_ABOUT]     = "About Manul",
};
static const char *al_ru[] = {
    [AL_NAVIGATE]  = "\xD0\x9D\xD0\xB0\xD0\xB2\xD0\xB8\xD0\xB3\xD0\xB0\xD1\x86\xD0\xB8\xD1\x8F",
    [AL_BACK]      = "\xD0\x9D\xD0\xB0\xD0\xB7\xD0\xB0\xD0\xB4    Alt+<",
    [AL_FORWARD]   = "\xD0\x92\xD0\xBF\xD0\xB5\xD1\x80\xD1\x91\xD0\xB4   Alt+>",
    [AL_STOP]      = "\xD0\xA1\xD1\x82\xD0\xBE\xD0\xBF       Esc",
    [AL_RELOAD]    = "\xD0\x9E\xD0\xB1\xD0\xBD\xD0\xBE\xD0\xB2\xD0\xB8\xD1\x82\xD1\x8C    F5",
    [AL_NEXT_LINK] = "\xD0\xA1\xD0\xBB\xD0\xB5\xD0\xB4. \xD1\x81\xD1\x81\xD1\x8B\xD0\xBB\xD0\xBA\xD0\xB0  Tab",
    [AL_PREV_LINK] = "\xD0\x9F\xD1\x80\xD0\xB5\xD0\xB4. \xD1\x81\xD1\x81\xD1\x8B\xD0\xBB\xD0\xBA\xD0\xB0 S+Tb",
    [AL_ABOUT]     = "\xD0\x9E Manul",
};
static const char *AL(int id) { return lang_get() == LANG_RU ? al_ru[id] : al_en[id]; }

#include "html.h"
#include "render.h"
#include "http.h"
#include "url.h"
#include "font.h"

/* FatFS not needed but included by m-os-api.h */
#include "m-os-api-ff.h"

/*==========================================================================
 * Constants
 *=========================================================================*/

/* Toolbar layout */
#define TOOLBAR_H       24
#define BTN_W           22
#define BTN_H           20
#define BTN_Y            2
#define BTN_GAP          2
#define ADDR_Y           ((TOOLBAR_H - FONT_UI_HEIGHT) / 2)
#define ADDR_H          FONT_UI_HEIGHT
#define GO_W            28

/* Content area starts below toolbar */
#define CONTENT_Y       TOOLBAR_H

/* Status bar at the bottom */
#define STATUS_H        (FONT_UI_HEIGHT + 4)

/* Content padding */
#define CONTENT_PAD     4

/* Menu command IDs */
#define CMD_BACK        100
#define CMD_FORWARD     101
#define CMD_STOP        102
#define CMD_REFRESH     103
#define CMD_GO          104
#define CMD_ADDRESS     105

#define CMD_EXIT        200
#define CMD_ABOUT       300

#define CMD_NAV_BACK    400
#define CMD_NAV_FWD     401
#define CMD_NAV_STOP    402
#define CMD_NAV_RELOAD  403
#define CMD_NAV_HOME    404
#define CMD_NAV_GOTO    405

#define CMD_NEXT_LINK   500
#define CMD_PREV_LINK   501

/* Browser modes */
#define MODE_BROWSING   0
#define MODE_URL_INPUT  1
#define MODE_LOADING    2

/* History */
#define HISTORY_SIZE    16

/* HID scancodes */
#define SC_TAB          0x2B
#define SC_ENTER        0x28
#define SC_ESCAPE       0x29
#define SC_BACKSPACE    0x2A
#define SC_DELETE       0x4C
#define SC_HOME         0x4A
#define SC_END          0x4D
#define SC_PGUP         0x4B
#define SC_PGDN         0x4E
#define SC_UP           0x52
#define SC_DOWN         0x51
#define SC_LEFT         0x50
#define SC_RIGHT        0x4F
#define SC_F1           0x3A
#define SC_F5           0x3E

/*==========================================================================
 * History entry
 *=========================================================================*/

typedef struct {
    char     url[512];
    uint16_t scroll_pos;
    int16_t  selected_link;
} history_entry_t;

/*==========================================================================
 * App state
 *=========================================================================*/

typedef struct {
    hwnd_t       hwnd;
    uint8_t      mode;

    /* Page */
    render_page_t   page;
    render_ctx_t    render_ctx;
    html_parser_t   html_parser;
    char            current_url[512];

    /* Scroll & links */
    int32_t      scroll_y;       /* pixel offset */
    int16_t      selected_link;  /* -1 = none */
    int16_t      hover_link;     /* -1 = none, link under mouse cursor */
    int16_t      content_w;      /* client-derived */
    int16_t      content_h;

    /* Address bar */
    textarea_t   addr_ta;
    char         addr_buf[512];
    TimerHandle_t blink_timer;

    /* Toolbar button state */
    int8_t       btn_pressed;    /* -1=none, 0..4=which button */

    /* Scrollbar */
    scrollbar_t  vscroll;

    /* History */
    history_entry_t history[HISTORY_SIZE];
    uint8_t      history_pos;
    uint8_t      history_count;
    /* Forward stack — when we go back, forward entries are available */
    history_entry_t forward[HISTORY_SIZE];
    uint8_t      forward_count;

    /* Status bar */
    char         status_text[80];
    uint8_t      loading_dots;     /* 0-3 for animation */

    /* Deferred navigation — event handler stores URL here,
     * main loop performs the blocking network call. */
    char         pending_url[512];
    bool         nav_pending;

    /* Receive buffer — netcard data callback copies here,
     * main loop processes it (avoids heavy work in callback). */
    uint8_t     *recv_ring;         /* circular buffer in PSRAM */
    volatile uint32_t recv_wr;      /* write position (callback) */
    volatile uint32_t recv_rd;      /* read position (main loop) */
    volatile bool     recv_done;    /* set by close/done callback */

    /* Paint optimization — skip expensive content repaint on cursor blink */
    bool         content_dirty;

    /* State flags */
    bool         closing;
    bool         wifi_ok;
    bool         skip_history;   /* set by back/forward to prevent push */
} browser_t;

#define RECV_RING_SIZE  (256 * 1024)   /* 256KB in PSRAM — GitHub pages can be 150KB+ */

static browser_t br;
static void *app_task;

/*==========================================================================
 * Forward declarations
 *=========================================================================*/

static void br_navigate(const char *url);
static void br_go_back(void);
static void br_go_forward(void);
static void br_stop(void);
static void br_refresh(void);
static void br_do_go(void);
static void br_history_push(void);
static void br_select_link(int16_t idx);
static void br_follow_link(void);
static int16_t br_link_at_pixel(int16_t mx, int16_t my);
static void br_show_welcome(void);
static void br_setup_menu(void);
static void br_update_title(void);
static void br_update_scrollbar(void);
static int32_t br_max_scroll(void);

/* HTTP callbacks */
static void on_body_chunk(const uint8_t *data, uint16_t len, void *ctx);
static void on_done(void *ctx);

/*==========================================================================
 * Menu setup
 *=========================================================================*/

static void br_setup_menu(void) {
    menu_bar_t bar;
    memset(&bar, 0, sizeof(bar));
    bar.menu_count = 3;

    /* File menu */
    menu_def_t *file = &bar.menus[0];
    strncpy(file->title, L(STR_FILE), sizeof(file->title) - 1);
    file->accel_key = 0x09; /* HID 'F' */
    file->item_count = 1;
    strncpy(file->items[0].text, L(STR_FM_EXIT), sizeof(file->items[0].text) - 1);
    file->items[0].command_id = CMD_EXIT;

    /* Navigate menu */
    menu_def_t *nav = &bar.menus[1];
    strncpy(nav->title, AL(AL_NAVIGATE), sizeof(nav->title) - 1);
    nav->accel_key = 0x11; /* HID 'N' */
    nav->item_count = 7;
    strncpy(nav->items[0].text, AL(AL_BACK), sizeof(nav->items[0].text) - 1);
    nav->items[0].command_id = CMD_NAV_BACK;
    strncpy(nav->items[1].text, AL(AL_FORWARD), sizeof(nav->items[1].text) - 1);
    nav->items[1].command_id = CMD_NAV_FWD;
    strncpy(nav->items[2].text, AL(AL_STOP), sizeof(nav->items[2].text) - 1);
    nav->items[2].command_id = CMD_NAV_STOP;
    strncpy(nav->items[3].text, AL(AL_RELOAD), sizeof(nav->items[3].text) - 1);
    nav->items[3].command_id = CMD_NAV_RELOAD;
    nav->items[3].accel_key = SC_F5;
    nav->items[4].flags = MIF_SEPARATOR;
    strncpy(nav->items[5].text, AL(AL_NEXT_LINK), sizeof(nav->items[5].text) - 1);
    nav->items[5].command_id = CMD_NEXT_LINK;
    strncpy(nav->items[6].text, AL(AL_PREV_LINK), sizeof(nav->items[6].text) - 1);
    nav->items[6].command_id = CMD_PREV_LINK;

    /* Help menu */
    menu_def_t *help = &bar.menus[2];
    strncpy(help->title, L(STR_HELP), sizeof(help->title) - 1);
    help->accel_key = 0x0B; /* HID 'H' */
    help->item_count = 1;
    strncpy(help->items[0].text, L(STR_FM_ABOUT_MENU), sizeof(help->items[0].text) - 1);
    help->items[0].command_id = CMD_ABOUT;
    help->items[0].accel_key = SC_F1;

    menu_set(br.hwnd, &bar);
}

/*==========================================================================
 * Title bar
 *=========================================================================*/

static void br_update_title(void) {
    window_t *win = wm_get_window(br.hwnd);
    if (!win) return;

    char title[24];
    int n = 0;
    if (br.mode == MODE_LOADING) {
        const char *s = "Loading - Manul";
        while (*s && n < 23) title[n++] = *s++;
    } else if (br.page.title[0]) {
        const char *s = br.page.title;
        while (*s && n < 12) title[n++] = *s++;
        const char *suf = " - Manul";
        const char *q = suf;
        while (*q && n < 23) title[n++] = *q++;
    } else {
        const char *s = "Manul";
        while (*s && n < 23) title[n++] = *s++;
    }
    title[n] = '\0';
    memcpy(win->title, title, n + 1);
    win->flags |= WF_DIRTY | WF_FRAME_DIRTY;
    wm_mark_dirty();
    taskbar_invalidate();
}

/*==========================================================================
 * Scrollbar
 *=========================================================================*/

static int32_t br_total_content_height(void) {
    int32_t h = 0;
    uint16_t i;
    for (i = 0; i < br.page.num_lines; i++) {
        const render_line_t *line = render_get_line(&br.page, i);
        if (line && line->heading > 0)
            h += HFONT_H;
        else
            h += LFONT_H;
    }
    return h;
}

static int32_t br_max_scroll(void) {
    int32_t total_h = br_total_content_height();
    int32_t max = total_h - br.content_h;
    if (max < 0) max = 0;
    return max;
}

static void br_update_scrollbar(void) {
    int32_t total_h = br_total_content_height();
    /* Hide scrollbar when content fits */
    br.vscroll.visible = (total_h > br.content_h);
    scrollbar_set_range(&br.vscroll, total_h, br.content_h);
    scrollbar_set_pos(&br.vscroll, br.scroll_y);
}

/*==========================================================================
 * History
 *=========================================================================*/

static void br_history_push(void) {
    if (br.current_url[0] == '\0') return;

    history_entry_t *e = &br.history[br.history_pos % HISTORY_SIZE];
    strncpy(e->url, br.current_url, sizeof(e->url) - 1);
    e->url[sizeof(e->url) - 1] = '\0';
    e->scroll_pos = (uint16_t)(br.scroll_y / LFONT_H);
    e->selected_link = br.selected_link;

    br.history_pos = (br.history_pos + 1) % HISTORY_SIZE;
    if (br.history_count < HISTORY_SIZE)
        br.history_count++;

    /* Navigating forward clears the forward stack */
    br.forward_count = 0;
}

static void br_go_back(void) {
    if (br.history_count == 0) return;

    /* Push current to forward stack */
    if (br.current_url[0] && br.forward_count < HISTORY_SIZE) {
        history_entry_t *fe = &br.forward[br.forward_count++];
        strncpy(fe->url, br.current_url, sizeof(fe->url) - 1);
        fe->url[sizeof(fe->url) - 1] = '\0';
        fe->scroll_pos = (uint16_t)(br.scroll_y / LFONT_H);
        fe->selected_link = br.selected_link;
    }

    if (br.history_pos == 0)
        br.history_pos = HISTORY_SIZE - 1;
    else
        br.history_pos--;
    br.history_count--;

    history_entry_t *e = &br.history[br.history_pos % HISTORY_SIZE];
    br.skip_history = true;
    br_navigate(e->url);
}

static void br_go_forward(void) {
    if (br.forward_count == 0) return;

    br.forward_count--;
    history_entry_t *fe = &br.forward[br.forward_count];

    /* Push current to back history (without clearing forward) */
    if (br.current_url[0]) {
        history_entry_t *he = &br.history[br.history_pos % HISTORY_SIZE];
        strncpy(he->url, br.current_url, sizeof(he->url) - 1);
        he->url[sizeof(he->url) - 1] = '\0';
        he->scroll_pos = (uint16_t)(br.scroll_y / LFONT_H);
        he->selected_link = br.selected_link;
        br.history_pos = (br.history_pos + 1) % HISTORY_SIZE;
        if (br.history_count < HISTORY_SIZE)
            br.history_count++;
    }

    /* Navigate without clearing forward stack */
    uint8_t saved_fwd = br.forward_count;
    br.skip_history = true;
    br_navigate(fe->url);
    br.forward_count = saved_fwd;
}

/*==========================================================================
 * Navigation
 *=========================================================================*/

/* Request navigation — stores the URL and wakes the main loop.
 * The actual blocking network call happens on the app task. */
static void br_navigate(const char *url) {
    strncpy(br.pending_url, url, sizeof(br.pending_url) - 1);
    br.pending_url[sizeof(br.pending_url) - 1] = '\0';
    br.nav_pending = true;

    /* Prepare UI immediately */
    if (!br.skip_history)
        br_history_push();
    br.skip_history = false;
    strncpy(br.current_url, url, sizeof(br.current_url) - 1);
    br.current_url[sizeof(br.current_url) - 1] = '\0';
    br.mode = MODE_LOADING;
    br.scroll_y = 0;
    br.selected_link = -1;
    br.loading_dots = 0;
    br.recv_wr = 0;
    br.recv_rd = 0;
    br.recv_done = false;
    strncpy(br.status_text, "Loading", sizeof(br.status_text) - 1);

    render_clear(&br.page);
    render_init(&br.page);
    render_ctx_init(&br.render_ctx, &br.page);
    html_parser_init(&br.html_parser);

    textarea_set_text(&br.addr_ta, br.current_url, strlen(br.current_url));
    br_update_scrollbar();
    br_update_title();
    br.content_dirty = true;
    wm_invalidate(br.hwnd);

    /* Wake the main loop to perform the blocking HTTP call */
    xTaskNotifyGive(app_task);
}

/* Actually perform the HTTP request — called from the main loop task. */
static void br_do_navigate(void) {
    if (!netcard_wifi_connected()) {
        br.wifi_ok = false;
        br.mode = MODE_BROWSING;
        strncpy(br.status_text, "No WiFi connection",
                sizeof(br.status_text) - 1);
        br.content_dirty = true;
        br_update_title();
        wm_invalidate(br.hwnd);
        return;
    }
    br.wifi_ok = true;

    if (!http_get(br.current_url, on_body_chunk, on_done, 0)) {
        br.mode = MODE_BROWSING;
        strncpy(br.status_text, "Failed to start request",
                sizeof(br.status_text) - 1);
        br.content_dirty = true;
        br_update_title();
        wm_invalidate(br.hwnd);
    }
}

static void br_stop(void) {
    if (br.mode == MODE_LOADING) {
        http_abort();
        br.mode = MODE_BROWSING;
        html_parser_finish(&br.html_parser,
                           (html_token_cb_t)render_process_token,
                           &br.render_ctx);
        render_flush(&br.render_ctx);
        br_update_scrollbar();
        br_update_title();
        wm_invalidate(br.hwnd);
    }
}

static void br_refresh(void) {
    if (br.current_url[0])
        br_navigate(br.current_url);
}

static void br_do_go(void) {
    const char *text = textarea_get_text(&br.addr_ta);
    if (!text || !text[0]) return;

    char url[512];
    strncpy(url, text, sizeof(url) - 1);
    url[sizeof(url) - 1] = '\0';

    /* Add http:// if no scheme */
    if (strncmp(url, "http://", 7) != 0 && strncmp(url, "https://", 8) != 0) {
        char tmp[512];
        snprintf(tmp, sizeof(tmp), "http://%s", url);
        strncpy(url, tmp, sizeof(url) - 1);
        url[sizeof(url) - 1] = '\0';
    }

    br.mode = MODE_BROWSING;
    br_navigate(url);
}

/*==========================================================================
 * Link handling
 *=========================================================================*/

static void br_select_link(int16_t idx) {
    if (br.page.num_links == 0) {
        br.selected_link = -1;
        return;
    }
    if (idx < 0) idx = 0;
    if (idx >= (int16_t)br.page.num_links)
        idx = (int16_t)br.page.num_links - 1;

    br.selected_link = idx;

    /* Scroll to make the link visible */
    const render_link_t *link = render_get_link(&br.page, (uint16_t)idx);
    if (link) {
        int32_t link_py = (int32_t)link->start_line * LFONT_H;
        if (link_py < br.scroll_y)
            br.scroll_y = link_py;
        else if (link_py + LFONT_H > br.scroll_y + br.content_h)
            br.scroll_y = link_py + LFONT_H - br.content_h;

        int32_t max = br_max_scroll();
        if (br.scroll_y > max) br.scroll_y = max;
        if (br.scroll_y < 0) br.scroll_y = 0;
        br_update_scrollbar();
    }
    br.content_dirty = true;
    wm_invalidate(br.hwnd);
}

static void br_follow_link(void) {
    if (br.selected_link < 0 || br.selected_link >= (int16_t)br.page.num_links)
        return;

    const render_link_t *link = render_get_link(&br.page,
                                                (uint16_t)br.selected_link);
    if (!link) return;

    /* Resolve relative URLs */
    if (br.current_url[0]) {
        url_t base, resolved;
        char resolved_str[512];
        if (url_parse(br.current_url, &base)) {
            url_resolve(&base, link->url, &resolved);
            url_to_string(&resolved, resolved_str, sizeof(resolved_str));
            br_navigate(resolved_str);
            return;
        }
    }
    br_navigate(link->url);
}

/* Find link at pixel position (client coords, in content area).
 * Walks lines with variable height (headings are taller). */
static int16_t br_link_at_pixel(int16_t mx, int16_t my) {
    if (my < CONTENT_Y + CONTENT_PAD) return -1;
    if (mx < CONTENT_PAD) return -1;

    int32_t target_y = br.scroll_y + (my - CONTENT_Y - CONTENT_PAD);
    int32_t py = 0;
    uint16_t li;

    for (li = 0; li < br.page.num_lines; li++) {
        const render_line_t *line = render_get_line(&br.page, li);
        if (!line) return -1;
        int16_t lh = (line->heading > 0) ? HFONT_H : LFONT_H;
        if (target_y >= py && target_y < py + lh) {
            /* Found the line */
            int16_t char_w = (line->heading > 0) ? HFONT_W : LFONT_W;
            int32_t col_idx = (mx - CONTENT_PAD) / char_w;
            if (col_idx < 0 || col_idx >= RENDER_MAX_COLS)
                return -1;
            int8_t lid = line->cells[col_idx].link_id;
            if (lid < 0) return -1;
            return (int16_t)lid;
        }
        py += lh;
    }
    return -1;
}

/*==========================================================================
 * HTTP callbacks — lightweight: just copy data to ring buffer.
 * Heavy HTML parsing happens on the main loop.
 *=========================================================================*/

static void on_body_chunk(const uint8_t *data, uint16_t len, void *ctx) {
    (void)ctx;
    /* Copy into ring buffer — drop data if full (better than blocking).
     * Do NOT call xTaskNotifyGive here — that would preempt the netcard
     * task (priority 1) in favor of the app task (higher priority),
     * causing the PIO UART FIFO to overflow and lose bytes.
     * The main loop polls every 50ms via ulTaskNotifyTake timeout. */
    uint16_t i;
    for (i = 0; i < len; i++) {
        uint32_t next_wr = (br.recv_wr + 1) % RECV_RING_SIZE;
        if (next_wr == br.recv_rd)
            break;  /* buffer full, drop remaining */
        br.recv_ring[br.recv_wr] = data[i];
        br.recv_wr = next_wr;
    }
}

static void on_done(void *ctx) {
    (void)ctx;
    br.recv_done = true;
    /* Wake main loop to finalize */
    xTaskNotifyGive(app_task);
}

/* Process buffered data on the main loop task (safe for malloc/rendering) */
static void br_process_recv(void) {
    /* Drain ring buffer in chunks */
    uint8_t chunk[256];
    while (br.recv_rd != br.recv_wr) {
        uint16_t n = 0;
        while (n < sizeof(chunk) && br.recv_rd != br.recv_wr) {
            chunk[n++] = br.recv_ring[br.recv_rd];
            br.recv_rd = (br.recv_rd + 1) % RECV_RING_SIZE;
        }
        if (n > 0) {
            html_parser_feed(&br.html_parser, chunk, n,
                             (html_token_cb_t)render_process_token,
                             &br.render_ctx);
        }
    }

    /* Check if transfer is complete */
    if (br.recv_done) {
        br.recv_done = false;
        br.mode = MODE_BROWSING;

        const http_response_t *resp = http_get_response();
        int status_code = resp ? resp->status_code : 0;

        /* Always finalize the HTML parser — we may have partial content */
        html_parser_finish(&br.html_parser,
                           (html_token_cb_t)render_process_token,
                           &br.render_ctx);
        render_flush(&br.render_ctx);

        if (br.page.num_links > 0 && br.selected_link < 0)
            br.selected_link = 0;

        if (status_code >= 200 && status_code < 400) {
            snprintf(br.status_text, sizeof(br.status_text),
                     "Done - %u lines, %u links",
                     br.page.num_lines, br.page.num_links);
        } else if (status_code == 0 && br.page.num_lines <= 1) {
            strncpy(br.status_text, "Error: connection failed",
                    sizeof(br.status_text) - 1);
        } else if (status_code == 0) {
            /* Got some content despite status 0 (e.g. timeout) */
            snprintf(br.status_text, sizeof(br.status_text),
                     "Done - %u lines, %u links",
                     br.page.num_lines, br.page.num_links);
        } else {
            snprintf(br.status_text, sizeof(br.status_text),
                     "HTTP error %d", status_code);
        }

        br_update_scrollbar();
        br_update_title();
        br.content_dirty = true;
        wm_invalidate(br.hwnd);
    }
}

/*==========================================================================
 * Welcome page
 *=========================================================================*/

static void br_show_welcome(void) {
    render_clear(&br.page);
    render_init(&br.page);
    render_ctx_init(&br.render_ctx, &br.page);

    static const char *html =
        "<h1>Welcome to Manul</h1>"
        "<p>A text web browser for FRANK OS.</p>"
        "<p></p>"
        "<p><b>Quick Start:</b></p>"
        "<p>  Click the address bar or press Ctrl+L, type a URL,</p>"
        "<p>  then press Enter or click Go.</p>"
        "<p></p>"
        "<p><b>Navigation:</b></p>"
        "<p>  Tab / Shift+Tab  - Next / previous link</p>"
        "<p>  Enter            - Follow selected link</p>"
        "<p>  Backspace        - Go back</p>"
        "<p>  Up/Down          - Scroll</p>"
        "<p>  PgUp/PgDn        - Scroll one page</p>"
        "<p>  Ctrl+L           - Focus address bar</p>"
        "<p>  F5               - Reload</p>"
        "<p>  Esc              - Stop loading</p>"
        "<p></p>"
        "<p>You can also click links with the mouse.</p>";

    html_parser_init(&br.html_parser);
    html_parser_feed(&br.html_parser, (const uint8_t *)html, strlen(html),
                     (html_token_cb_t)render_process_token, &br.render_ctx);
    html_parser_finish(&br.html_parser,
                       (html_token_cb_t)render_process_token, &br.render_ctx);
    render_flush(&br.render_ctx);

    strncpy(br.page.title, "Welcome", sizeof(br.page.title) - 1);
    br.current_url[0] = '\0';
    br.scroll_y = 0;
    br.selected_link = -1;

    br_update_scrollbar();
    br_update_title();
}

/*==========================================================================
 * Blink timer
 *=========================================================================*/

static void blink_cb(TimerHandle_t t) {
    (void)t;

    if (br.mode == MODE_URL_INPUT) {
        br.addr_ta.cursor_visible = !br.addr_ta.cursor_visible;
        wm_invalidate(br.hwnd);
    }

    /* Animate loading dots in status bar */
    if (br.mode == MODE_LOADING) {
        br.loading_dots = (br.loading_dots + 1) % 4;
        wm_invalidate(br.hwnd);
    }
}

/*==========================================================================
 * Paint handler
 *=========================================================================*/

static void br_paint_toolbar(void) {
    int16_t cw, ch;
    wd_get_clip_size(&cw, &ch);

    /* Toolbar background */
    wd_fill_rect(0, 0, cw, TOOLBAR_H, THEME_BUTTON_FACE);

    /* Buttons: B, F, S, R — disabled when action unavailable */
    const char *labels[] = { "B", "F", "S", "R" };
    bool enabled[4];
    enabled[0] = (br.history_count > 0);               /* Back */
    enabled[1] = (br.forward_count > 0);               /* Forward */
    enabled[2] = (br.mode == MODE_LOADING);             /* Stop */
    enabled[3] = (br.mode != MODE_LOADING &&            /* Refresh */
                  br.current_url[0] != '\0');

    int16_t bx = BTN_GAP;
    int i;
    for (i = 0; i < 4; i++) {
        bool pressed = (br.btn_pressed == i) && enabled[i];
        wd_button(bx, BTN_Y, BTN_W, BTN_H, labels[i], false, pressed);
        if (!enabled[i]) {
            /* Overdraw label as grayed out */
            int16_t tx = bx + (BTN_W - FONT_UI_WIDTH) / 2;
            int16_t ty = BTN_Y + (BTN_H - FONT_UI_HEIGHT) / 2;
            wd_text_ui(tx, ty, labels[i], COLOR_DARK_GRAY, THEME_BUTTON_FACE);
        }
        bx += BTN_W + BTN_GAP;
    }

    /* Address bar: sunken rect + textarea */
    int16_t addr_x = bx + 2;
    int16_t go_x = cw - GO_W - BTN_GAP;
    int16_t addr_w = go_x - addr_x - BTN_GAP;

    wd_bevel_rect(addr_x - 2, ADDR_Y - 2, addr_w + 4, ADDR_H + 4,
                  COLOR_DARK_GRAY, COLOR_WHITE, COLOR_WHITE);
    textarea_paint(&br.addr_ta);

    /* Go button */
    bool go_pressed = (br.btn_pressed == 4);
    wd_button(go_x, BTN_Y, GO_W, BTN_H, "Go", false, go_pressed);

    /* Separator line below toolbar */
    wd_hline(0, TOOLBAR_H - 1, cw, COLOR_DARK_GRAY);
}

/* Draw a 6x12 character using leggie font (all chars, ASCII + Cyrillic) */
static void draw_char(int16_t px, int16_t py, uint8_t ch,
                      uint8_t fg, uint8_t bg, bool bold) {
    int r, b;
    for (r = 0; r < LFONT_H; r++) {
        uint8_t bits = cfont_row(ch, (uint8_t)r, bold);
        for (b = 0; b < LFONT_W; b++) {
            uint8_t color = (bits & 0x80) ? fg : bg;
            wd_pixel(px + b, py + r, color);
            bits <<= 1;
        }
    }
}

/* Draw a heading character using leggie 9x18 bold */
static void draw_heading_char(int16_t px, int16_t py, uint8_t ch,
                              uint8_t fg, uint8_t bg) {
    int r, b;
    for (r = 0; r < HFONT_H; r++) {
        uint16_t bits = cfont_heading_row(ch, (uint8_t)r);
        for (b = 0; b < HFONT_W; b++) {
            uint8_t color = (bits & 0x8000) ? fg : bg;
            wd_pixel(px + b, py + r, color);
            bits <<= 1;
        }
    }
}

/* Line height depends on heading level */
static int16_t line_height(const render_line_t *line) {
    if (line->heading > 0)
        return HFONT_H;        /* 18px for headings */
    return LFONT_H;             /* 12px normal */
}

static void br_paint_content(void) {
    int16_t cw_total, ch_total;
    wd_get_clip_size(&cw_total, &ch_total);

    int16_t region_x = 0;
    int16_t region_y = CONTENT_Y;
    int16_t region_w = br.vscroll.visible ? (cw_total - SCROLLBAR_WIDTH) : cw_total;
    int16_t region_h = br.content_h;

    /* Background — white */
    wd_fill_rect(region_x, region_y, region_w, region_h, COLOR_WHITE);

    int16_t tx = region_x + CONTENT_PAD;
    int16_t ty = region_y + CONTENT_PAD;
    int16_t tw = region_w - 2 * CONTENT_PAD;
    int16_t th = region_h - 2 * CONTENT_PAD;
    if (tw < 0) tw = 0;
    if (th < 0) th = 0;

    /* Walk lines, accumulating pixel Y from variable-height lines */
    int32_t pixel_y = -br.scroll_y;  /* relative to ty */
    uint16_t li;

    for (li = 0; li < br.page.num_lines; li++) {
        const render_line_t *line = render_get_line(&br.page, li);
        if (!line) break;

        int16_t lh = line_height(line);
        int16_t py = (int16_t)(ty + pixel_y);

        /* Skip lines above viewport */
        if (py + lh <= ty) {
            pixel_y += lh;
            continue;
        }
        /* Stop if below viewport */
        if (py >= ty + th) break;

        bool is_heading = (line->heading > 0);
        int16_t char_w = is_heading ? HFONT_W : LFONT_W;

        int32_t col;
        for (col = 0; col < (int32_t)line->len; col++) {
            int16_t px = (int16_t)(tx + col * char_w);
            if (px >= tx + tw) break;

            const render_cell_t *cell = &line->cells[col];
            uint8_t fg, bg;

            if (cell->link_id >= 0 &&
                cell->link_id == br.selected_link) {
                /* Selected link (Tab): reverse video */
                fg = COLOR_WHITE;
                bg = COLOR_BLUE;
            } else if (cell->link_id >= 0 &&
                       cell->link_id == br.hover_link) {
                /* Hovered link (mouse): highlighted background */
                fg = COLOR_BLUE;
                bg = COLOR_LIGHT_CYAN;
            } else if (cell->link_id >= 0) {
                /* Normal link: blue underlined */
                fg = COLOR_BLUE;
                bg = COLOR_WHITE;
            } else {
                fg = COLOR_BLACK;
                bg = COLOR_WHITE;
            }

            uint8_t ch = (uint8_t)cell->ch;
            bool bold = (cell->attr & RATTR_BOLD) != 0;

            if (is_heading) {
                draw_heading_char(px, py, ch, fg, bg);
            } else {
                draw_char(px, py, ch, fg, bg, bold);
            }

            /* Draw underline for links */
            if (cell->link_id >= 0) {
                int16_t uy = py + LFONT_H - 1;
                int ub;
                for (ub = 0; ub < LFONT_W; ub++)
                    wd_pixel(px + ub, uy, fg);
            }
        }

        pixel_y += lh;
    }
}

static void br_paint_status(void) {
    int16_t cw, ch;
    wd_get_clip_size(&cw, &ch);
    int16_t sy = ch - STATUS_H;

    /* Status bar background (button face, like toolbar) */
    wd_hline(0, sy, cw, COLOR_DARK_GRAY);
    wd_fill_rect(0, sy + 1, cw, STATUS_H - 1, THEME_BUTTON_FACE);

    /* Status text — vertically centered */
    int16_t text_y = sy + 1 + (STATUS_H - 1 - FONT_UI_HEIGHT) / 2;

    if (br.mode == MODE_LOADING) {
        /* Animated "Loading..." */
        char buf[80];
        const char *dots = &"..."[3 - br.loading_dots];
        snprintf(buf, sizeof(buf), "Loading%s", dots);
        wd_text_ui(4, text_y, buf, COLOR_BLACK, THEME_BUTTON_FACE);
    } else if (br.status_text[0]) {
        wd_text_ui(4, text_y, br.status_text, COLOR_BLACK, THEME_BUTTON_FACE);
    } else {
        const char *wifi = br.wifi_ok ? "WiFi connected" : "No WiFi";
        wd_text_ui(4, text_y, wifi, COLOR_DARK_GRAY, THEME_BUTTON_FACE);
    }
}

static void br_paint(hwnd_t hwnd) {
    wd_begin(hwnd);

    br_paint_toolbar();

    br_paint_content();
    if (br.vscroll.visible)
        scrollbar_paint(&br.vscroll);

    br_paint_status();

    wd_end();
}

/*==========================================================================
 * Geometry update
 *=========================================================================*/

static void br_update_geometry(int16_t w, int16_t h) {
    /* Address bar position */
    int16_t bx = BTN_GAP + 4 * (BTN_W + BTN_GAP) + 2;
    int16_t go_x = w - GO_W - BTN_GAP;
    int16_t addr_w = go_x - bx - BTN_GAP;
    textarea_set_rect(&br.addr_ta, bx, ADDR_Y, addr_w, ADDR_H);

    /* Content area (between toolbar and status bar) */
    br.content_h = h - CONTENT_Y - STATUS_H;
    if (br.content_h < 0) br.content_h = 0;

    /* Content width depends on scrollbar visibility — update after scrollbar */
    int32_t total_h = (int32_t)br.page.num_lines * FONT_UI_HEIGHT;
    bool need_scroll = (total_h > br.content_h);
    br.content_w = need_scroll ? (w - SCROLLBAR_WIDTH) : w;

    /* Scrollbar */
    br.vscroll.x = w - SCROLLBAR_WIDTH;
    br.vscroll.y = CONTENT_Y;
    br.vscroll.w = SCROLLBAR_WIDTH;
    br.vscroll.h = br.content_h;
    br_update_scrollbar();

    br.content_dirty = true;
}

/*==========================================================================
 * Event handler
 *=========================================================================*/

static bool br_event(hwnd_t hwnd, const window_event_t *ev) {
    (void)hwnd;

    /* ── WM_CLOSE ────────────────────────────────────────────────── */
    if (ev->type == WM_CLOSE) {
        if (br.mode == MODE_LOADING)
            http_abort();
        br.closing = true;
        wm_destroy_window(br.hwnd);
        xTaskNotifyGive(app_task);
        return true;
    }

    /* ── WM_SIZE ─────────────────────────────────────────────────── */
    else if (ev->type == WM_SIZE) {
        br_update_geometry(ev->size.w, ev->size.h);
        wm_invalidate(br.hwnd);
        return true;
    }

    /* ── WM_SETFOCUS ─────────────────────────────────────────────── */
    else if (ev->type == WM_SETFOCUS) {
        if (br.blink_timer) xTimerStart(br.blink_timer, 0);
        wm_invalidate(br.hwnd);
        return true;
    }

    /* ── WM_KILLFOCUS ────────────────────────────────────────────── */
    else if (ev->type == WM_KILLFOCUS) {
        if (br.blink_timer) xTimerStop(br.blink_timer, 0);
        br.addr_ta.cursor_visible = false;
        wm_invalidate(br.hwnd);
        return true;
    }

    /* ── WM_COMMAND ──────────────────────────────────────────────── */
    else if (ev->type == WM_COMMAND) {
        uint16_t cmd = ev->command.id;

        if (cmd == CMD_EXIT) {
            window_event_t ce;
            memset(&ce, 0, sizeof(ce));
            ce.type = WM_CLOSE;
            wm_post_event(br.hwnd, &ce);
            return true;
        }
        else if (cmd == CMD_NAV_BACK || cmd == CMD_BACK) {
            br_go_back();
            return true;
        }
        else if (cmd == CMD_NAV_FWD || cmd == CMD_FORWARD) {
            br_go_forward();
            return true;
        }
        else if (cmd == CMD_NAV_STOP || cmd == CMD_STOP) {
            br_stop();
            return true;
        }
        else if (cmd == CMD_NAV_RELOAD || cmd == CMD_REFRESH) {
            br_refresh();
            return true;
        }
        else if (cmd == CMD_GO || cmd == CMD_NAV_GOTO) {
            br_do_go();
            return true;
        }
        else if (cmd == CMD_NEXT_LINK) {
            if (br.page.num_links > 0) {
                int16_t next = br.selected_link + 1;
                if (next >= (int16_t)br.page.num_links) next = 0;
                br_select_link(next);
            }
            return true;
        }
        else if (cmd == CMD_PREV_LINK) {
            if (br.page.num_links > 0) {
                int16_t prev = br.selected_link - 1;
                if (prev < 0) prev = (int16_t)br.page.num_links - 1;
                br_select_link(prev);
            }
            return true;
        }
        else if (cmd == CMD_ABOUT) {
            dialog_show(br.hwnd, AL(AL_ABOUT),
                        "Manul Web Browser\n\nFRANK OS v" FRANK_VERSION_STR
                        "\n(c) 2026 Mikhail Matveev\n"
                        "<xtreme@rh1.tech>\n"
                        "github.com/rh1tech/frank-os",
                        DLG_ICON_INFO, DLG_BTN_OK);
            return true;
        }
        else if (cmd == DLG_RESULT_OK) {
            wm_invalidate(br.hwnd);
            return true;
        }
        return false;
    }

    /* ── WM_KEYDOWN ──────────────────────────────────────────────── */
    else if (ev->type == WM_KEYDOWN) {
        uint8_t sc = ev->key.scancode;
        uint8_t mod = ev->key.modifiers;
        bool ctrl = (mod & KMOD_CTRL) != 0;
        bool shift = (mod & KMOD_SHIFT) != 0;

        /* Ctrl+L — focus address bar */
        if (ctrl && sc == 0x0F) { /* HID 'l' */
            br.mode = MODE_URL_INPUT;
            textarea_select_all(&br.addr_ta);
            br.addr_ta.cursor_visible = true;
            wm_invalidate(br.hwnd);
            return true;
        }

        /* F5 — reload */
        if (sc == SC_F5) {
            br_refresh();
            return true;
        }

        /* F1 — about */
        if (sc == SC_F1) {
            window_event_t ce;
            memset(&ce, 0, sizeof(ce));
            ce.type = WM_COMMAND;
            ce.command.id = CMD_ABOUT;
            wm_post_event(br.hwnd, &ce);
            return true;
        }

        /* URL input mode */
        if (br.mode == MODE_URL_INPUT) {
            /* Enter — navigate */
            if (sc == SC_ENTER) {
                br_do_go();
                return true;
            }
            /* Escape — cancel */
            if (sc == SC_ESCAPE) {
                br.mode = MODE_BROWSING;
                br.addr_ta.cursor_visible = false;
                textarea_set_text(&br.addr_ta, br.current_url,
                                  strlen(br.current_url));
                wm_invalidate(br.hwnd);
                return true;
            }
            /* Forward to textarea */
            if (textarea_event(&br.addr_ta, ev)) {
                wm_invalidate(br.hwnd);
                return true;
            }
            return true;
        }

        /* Loading mode — Escape stops */
        if (br.mode == MODE_LOADING) {
            if (sc == SC_ESCAPE) {
                br_stop();
                return true;
            }
            return true;
        }

        /* Browsing mode */

        /* Tab / Shift+Tab — link navigation */
        if (sc == SC_TAB) {
            if (br.page.num_links > 0) {
                if (shift) {
                    int16_t prev = br.selected_link - 1;
                    if (prev < 0) prev = (int16_t)br.page.num_links - 1;
                    br_select_link(prev);
                } else {
                    int16_t next = br.selected_link + 1;
                    if (next >= (int16_t)br.page.num_links) next = 0;
                    br_select_link(next);
                }
            }
            return true;
        }

        /* Enter — follow link */
        if (sc == SC_ENTER) {
            br_follow_link();
            return true;
        }

        /* Backspace — go back */
        if (sc == SC_BACKSPACE) {
            br_go_back();
            return true;
        }

        /* Scroll: Up/Down */
        if (sc == SC_UP) {
            br.scroll_y -= LFONT_H;
            if (br.scroll_y < 0) br.scroll_y = 0;
            br_update_scrollbar();
            br.content_dirty = true;
            wm_invalidate(br.hwnd);
            return true;
        }
        if (sc == SC_DOWN) {
            br.scroll_y += LFONT_H;
            int32_t max = br_max_scroll();
            if (br.scroll_y > max) br.scroll_y = max;
            br_update_scrollbar();
            br.content_dirty = true;
            wm_invalidate(br.hwnd);
            return true;
        }

        /* Page Up/Down */
        if (sc == SC_PGUP) {
            br.scroll_y -= br.content_h;
            if (br.scroll_y < 0) br.scroll_y = 0;
            br_update_scrollbar();
            br.content_dirty = true;
            wm_invalidate(br.hwnd);
            return true;
        }
        if (sc == SC_PGDN) {
            br.scroll_y += br.content_h;
            int32_t max = br_max_scroll();
            if (br.scroll_y > max) br.scroll_y = max;
            br_update_scrollbar();
            br.content_dirty = true;
            wm_invalidate(br.hwnd);
            return true;
        }

        /* Home/End */
        if (sc == SC_HOME) {
            br.scroll_y = 0;
            br_update_scrollbar();
            br.content_dirty = true;
            wm_invalidate(br.hwnd);
            return true;
        }
        if (sc == SC_END) {
            br.scroll_y = br_max_scroll();
            br_update_scrollbar();
            br.content_dirty = true;
            wm_invalidate(br.hwnd);
            return true;
        }

        return false;
    }

    /* ── WM_CHAR ─────────────────────────────────────────────────── */
    else if (ev->type == WM_CHAR) {
        if (br.mode == MODE_URL_INPUT) {
            if (textarea_event(&br.addr_ta, ev)) {
                wm_invalidate(br.hwnd);
                return true;
            }
        }
        return true;
    }

    /* ── WM_LBUTTONDOWN ──────────────────────────────────────────── */
    else if (ev->type == WM_LBUTTONDOWN) {
        int16_t mx = ev->mouse.x;
        int16_t my = ev->mouse.y;

        /* Toolbar button clicks — only if enabled */
        if (my < TOOLBAR_H) {
            bool btn_en[4];
            btn_en[0] = (br.history_count > 0);
            btn_en[1] = (br.forward_count > 0);
            btn_en[2] = (br.mode == MODE_LOADING);
            btn_en[3] = (br.mode != MODE_LOADING && br.current_url[0] != '\0');

            int16_t bx = BTN_GAP;
            int i;
            for (i = 0; i < 4; i++) {
                if (mx >= bx && mx < bx + BTN_W &&
                    my >= BTN_Y && my < BTN_Y + BTN_H &&
                    btn_en[i]) {
                    br.btn_pressed = (int8_t)i;
                    wm_invalidate(br.hwnd);
                    return true;
                }
                bx += BTN_W + BTN_GAP;
            }

            /* Go button */
            int16_t cw, ch;
            wd_get_clip_size(&cw, &ch);
            int16_t go_x = cw - GO_W - BTN_GAP;
            if (mx >= go_x && mx < go_x + GO_W &&
                my >= BTN_Y && my < BTN_Y + BTN_H) {
                br.btn_pressed = 4;
                wm_invalidate(br.hwnd);
                return true;
            }

            /* Address bar click — enter URL mode */
            int16_t addr_x = BTN_GAP + 4 * (BTN_W + BTN_GAP) + 2;
            if (mx >= addr_x && mx < go_x - BTN_GAP) {
                br.mode = MODE_URL_INPUT;
                br.addr_ta.cursor_visible = true;
                /* Forward click to textarea for cursor positioning */
                textarea_event(&br.addr_ta, ev);
                wm_invalidate(br.hwnd);
                return true;
            }
            return true;
        }

        /* Content area — link click */
        if (my >= CONTENT_Y) {
            /* Check scrollbar first */
            int32_t new_pos;
            if (scrollbar_event(&br.vscroll, ev, &new_pos)) {
                br.scroll_y = new_pos;
                br.content_dirty = true;
                wm_invalidate(br.hwnd);
                return true;
            }

            int16_t lid = br_link_at_pixel(mx, my);
            if (lid >= 0) {
                br_select_link(lid);
                br_follow_link();
                return true;
            }
        }
        return true;
    }

    /* ── WM_LBUTTONUP ────────────────────────────────────────────── */
    else if (ev->type == WM_LBUTTONUP) {
        int8_t was = br.btn_pressed;
        br.btn_pressed = -1;

        /* Handle scrollbar drag release */
        int32_t new_pos;
        if (scrollbar_event(&br.vscroll, ev, &new_pos)) {
            br.scroll_y = new_pos;
            wm_invalidate(br.hwnd);
            return true;
        }

        if (was >= 0) {
            wm_invalidate(br.hwnd);
            if (was == 0) br_go_back();
            else if (was == 1) br_go_forward();
            else if (was == 2) br_stop();
            else if (was == 3) br_refresh();
            else if (was == 4) br_do_go();
        }

        if (br.mode == MODE_URL_INPUT)
            textarea_event(&br.addr_ta, ev);
        return true;
    }

    /* ── WM_MOUSEMOVE ────────────────────────────────────────────── */
    else if (ev->type == WM_MOUSEMOVE) {
        /* Handle scrollbar dragging */
        int32_t new_pos;
        if (scrollbar_event(&br.vscroll, ev, &new_pos)) {
            br.scroll_y = new_pos;
            wm_invalidate(br.hwnd);
            return true;
        }

        /* Track hover over links.
         * wm_post_event skips compositor_dirty for MOUSEMOVE, so
         * we need wm_force_full_repaint to wake the compositor. */
        int16_t old_hover = br.hover_link;
        br.hover_link = br_link_at_pixel(ev->mouse.x, ev->mouse.y);
        if (br.hover_link != old_hover) {
            window_t *w = wm_get_window(br.hwnd);
            if (w) w->flags |= WF_DIRTY;
            wm_mark_dirty();
        }

        if (br.mode == MODE_URL_INPUT)
            textarea_event(&br.addr_ta, ev);
        return true;
    }

    /* ── WM_TIMER ────────────────────────────────────────────────── */
    else if (ev->type == WM_TIMER) {
        /* Poll HTTP for deferred redirects */
        http_poll();
        return true;
    }

    return false;
}

/*==========================================================================
 * Entry point
 *=========================================================================*/

int main(void) {
    memset(&br, 0, sizeof(br));
    br.btn_pressed = -1;
    br.selected_link = -1;
    br.hover_link = -1;
    app_task = xTaskGetCurrentTaskHandle();

    http_init();
    cfont_init();

    /* Allocate receive ring buffer in PSRAM (plenty of space) */
    br.recv_ring = (uint8_t *)psram_alloc(RECV_RING_SIZE);
    if (!br.recv_ring) {
        /* Fallback to SRAM if PSRAM unavailable */
        br.recv_ring = (uint8_t *)malloc(RECV_RING_SIZE);
    }
    if (!br.recv_ring) return 1;
    br.recv_wr = 0;
    br.recv_rd = 0;
    br.recv_done = false;

    /* Create window */
    int16_t win_w = 540 + 2 * THEME_BORDER_WIDTH;
    int16_t win_h = 400 + THEME_TITLE_HEIGHT + THEME_MENU_HEIGHT +
                    2 * THEME_BORDER_WIDTH;

    br.hwnd = wm_create_window(20, 10, win_w, win_h,
                                "Manul",
                                WSTYLE_DEFAULT | WF_MENUBAR,
                                br_event, br_paint);
    if (br.hwnd == HWND_NULL)
        return 1;

    br_setup_menu();

    /* Initialize address bar textarea */
    textarea_init(&br.addr_ta, br.addr_buf, sizeof(br.addr_buf), br.hwnd);
    br.addr_ta.cursor_visible = false;

    /* Initialize scrollbar */
    scrollbar_init(&br.vscroll, false);
    br.vscroll.visible = true;

    /* Set up geometry from initial client rect */
    rect_t cr = wm_get_client_rect(br.hwnd);
    br_update_geometry(cr.w, cr.h);

    /* Initialize render page */
    render_init(&br.page);

    /* Blink timer for address bar cursor + loading animation */
    br.blink_timer = xTimerCreate("mnblink", pdMS_TO_TICKS(500),
                                   pdTRUE, 0, blink_cb);
    if (br.blink_timer) xTimerStart(br.blink_timer, 0);

    /* Show welcome page */
    br.wifi_ok = netcard_wifi_connected();
    if (br.wifi_ok)
        strncpy(br.status_text, "WiFi connected", sizeof(br.status_text) - 1);
    else
        strncpy(br.status_text, "No WiFi", sizeof(br.status_text) - 1);
    br.content_dirty = true;
    br_show_welcome();

    /* Start with address bar focused so user can type immediately */
    br.mode = MODE_URL_INPUT;
    br.addr_ta.cursor_visible = true;

    wm_show_window(br.hwnd);
    wm_set_focus(br.hwnd);
    taskbar_invalidate();

    dbg_printf("[manul] started\n");

    /* Main loop — blocking work (HTTP) runs here, not in the event handler.
     * Received data is buffered by netcard callback and processed here. */
    while (!br.closing) {
        /* Short timeout so we check the ring buffer frequently,
         * but still yield CPU for the netcard task to read UART. */
        uint32_t nv = ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(10));
        (void)nv;

        /* Handle deferred navigation (blocking netcard calls) */
        if (br.nav_pending) {
            br.nav_pending = false;
            br_do_navigate();
        }

        /* Process any buffered HTTP body data */
        if (br.mode == MODE_LOADING || br.recv_rd != br.recv_wr || br.recv_done)
            br_process_recv();

        /* Poll HTTP for deferred redirects */
        http_poll();
    }

    /* Cleanup */
    if (br.blink_timer) {
        xTimerStop(br.blink_timer, 0);
        xTimerDelete(br.blink_timer, 0);
    }

    /* Render page buffers are in PSRAM — freed on app exit automatically */
    if (br.recv_ring) psram_free(br.recv_ring);

    dbg_printf("[manul] exited\n");
    return 0;
}
