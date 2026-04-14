/*
 * FRANK OS Application API
 * Copyright (c) 2026 Mikhail Matveev <xtreme@rh1.tech>
 * https://rh1.tech
 *
 * Standalone ELF apps include this header to access FRANK OS GUI services
 * through the MOS2 sys_table at 0x10FFF000.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef FRANKOS_APP_H
#define FRANKOS_APP_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ========================================================================
 * sys_table base (inherited from m-os-api.h)
 * ======================================================================== */

#ifndef M_OS_API_SYS_TABLE_BASE
#define M_OS_API_SYS_TABLE_BASE ((void*)(0x10000000ul + (16 << 20) - (4 << 10)))
static const unsigned long * const _sys_table_ptrs =
    (const unsigned long * const)M_OS_API_SYS_TABLE_BASE;
#endif

/* ========================================================================
 * Geometry types
 * ======================================================================== */

typedef struct { int16_t x, y, w, h; } rect_t;
typedef struct { int16_t x, y; } point_t;

/* ========================================================================
 * Color constants (CGA/EGA 16-color palette)
 * ======================================================================== */

#define COLOR_BLACK          0
#define COLOR_BLUE           1
#define COLOR_GREEN          2
#define COLOR_CYAN           3
#define COLOR_RED            4
#define COLOR_MAGENTA        5
#define COLOR_BROWN          6
#define COLOR_LIGHT_GRAY     7
#define COLOR_DARK_GRAY      8
#define COLOR_LIGHT_BLUE     9
#define COLOR_LIGHT_GREEN   10
#define COLOR_LIGHT_CYAN    11
#define COLOR_LIGHT_RED     12
#define COLOR_LIGHT_MAGENTA 13
#define COLOR_YELLOW        14
#define COLOR_WHITE         15

/* ========================================================================
 * Application flags (returned by __app_flags export)
 * ======================================================================== */

#define APPFLAG_BACKGROUND  (1u << 0)   /* app keeps running when not focused */
#define APPFLAG_SINGLETON   (1u << 1)   /* only one instance allowed */

/* ========================================================================
 * Window handle
 * ======================================================================== */

typedef uint8_t hwnd_t;
#define HWND_NULL    0
#define WM_MAX_WINDOWS 16

/* ========================================================================
 * Window flags
 * ======================================================================== */

#define WF_ALIVE     (1u << 0)
#define WF_VISIBLE   (1u << 1)
#define WF_FOCUSED   (1u << 2)
#define WF_CLOSABLE  (1u << 3)
#define WF_RESIZABLE (1u << 4)
#define WF_MOVABLE   (1u << 5)
#define WF_BORDER    (1u << 6)
#define WF_DIRTY       (1u << 7)
#define WF_MENUBAR     (1u << 8)
#define WF_FRAME_DIRTY (1u << 9)  /* decorations need repaint */
#define WF_FULLSCREENABLE (1u << 11) /* window supports fullscreen toggle */

/* ========================================================================
 * Window style presets
 * ======================================================================== */

#define WSTYLE_DEFAULT   (WF_CLOSABLE | WF_RESIZABLE | WF_MOVABLE | WF_BORDER)
#define WSTYLE_DIALOG    (WF_CLOSABLE | WF_MOVABLE | WF_BORDER)
#define WSTYLE_POPUP     0

/* ========================================================================
 * Window event types (WM_* message IDs)
 * ======================================================================== */

#define WM_NULL          0
#define WM_CREATE        1
#define WM_DESTROY       2
#define WM_CLOSE         3
#define WM_PAINT         4
#define WM_SETFOCUS      5
#define WM_KILLFOCUS     6
#define WM_MOVE          7
#define WM_SIZE          8
#define WM_MINIMIZE      9
#define WM_MAXIMIZE     10
#define WM_RESTORE      11
#define WM_KEYDOWN      12
#define WM_KEYUP        13
#define WM_CHAR         14
#define WM_MOUSEMOVE    15
#define WM_LBUTTONDOWN  16
#define WM_LBUTTONUP    17
#define WM_RBUTTONDOWN  18
#define WM_RBUTTONUP    19
#define WM_TIMER        20
#define WM_COMMAND      21
#define WM_DROPFILES    22  /* file opened via association; file_path in event */

/* Keyboard modifier flags */
#define KMOD_SHIFT  (1u << 0)
#define KMOD_CTRL   (1u << 1)
#define KMOD_ALT    (1u << 2)

/* ========================================================================
 * Window event structure
 * ======================================================================== */

typedef struct window_event {
    uint8_t type;
    uint8_t _pad;
    union {
        struct { uint8_t scancode; uint8_t modifiers; } key;
        struct { char ch; uint8_t modifiers; } charev;
        struct { int16_t x; int16_t y; uint8_t buttons; uint8_t modifiers; } mouse;
        struct { int16_t x; int16_t y; } move;
        struct { int16_t w; int16_t h; } size;
        struct { uint16_t id; } command;
        struct { uint16_t timer_id; } timer;
        struct { const char *file_path; } dropfiles; /* WM_DROPFILES */
    };
} window_event_t;

/* ========================================================================
 * Forward declarations and typedefs
 * ======================================================================== */

typedef struct window window_t;
/* Event handler callback. Return true if the event was consumed (handled),
 * false to indicate the event was not handled (reserved for future default
 * processing). The handler is called from the compositor task context. */
typedef bool (*event_handler_t)(hwnd_t hwnd, const window_event_t *event);
/* Paint handler callback. Called when the window needs repainting.
 * Use wd_begin()/wd_end() to bracket drawing operations. */
typedef void (*paint_handler_t)(hwnd_t hwnd);

/* ========================================================================
 * Window structure (must match firmware layout exactly)
 * ======================================================================== */

struct window {
    uint16_t         flags;
    uint8_t          state;
    rect_t           frame;
    rect_t           restore_rect;
    uint8_t          bg_color;
    uint8_t          z_order;
    char             title[24];
    event_handler_t  event_handler;
    paint_handler_t  paint_handler;
    void            *user_data;
};

/* ========================================================================
 * Menu types
 * ======================================================================== */

#define MENU_MAX_ITEMS    8
#define MENU_MAX_MENUS    5
#define MIF_SEPARATOR    (1u << 0)
#define MIF_DISABLED     (1u << 1)

typedef struct {
    char     text[24];      /* UTF-8: up to ~11 Cyrillic chars + shortcut */
    uint16_t command_id;
    uint8_t  flags;
    uint8_t  accel_key;
} menu_item_t;

typedef struct {
    char        title[16];  /* UTF-8: up to ~7 Cyrillic chars */
    uint8_t     accel_key;
    uint8_t     item_count;
    menu_item_t items[MENU_MAX_ITEMS];
} menu_def_t;

typedef struct {
    uint8_t    menu_count;
    menu_def_t menus[MENU_MAX_MENUS];
} menu_bar_t;

/* ========================================================================
 * Dialog constants
 * ======================================================================== */

#define DLG_ICON_NONE       0
#define DLG_ICON_INFO       1
#define DLG_ICON_WARNING    2
#define DLG_ICON_ERROR      3

#define DLG_BTN_OK          (1u << 0)
#define DLG_BTN_CANCEL      (1u << 1)
#define DLG_BTN_YES         (1u << 2)
#define DLG_BTN_NO          (1u << 3)

/* Dialog result IDs (posted as WM_COMMAND to parent) */
#define DLG_RESULT_OK       0xFF01
#define DLG_RESULT_CANCEL   0xFF02
#define DLG_RESULT_YES      0xFF03
#define DLG_RESULT_NO       0xFF04
#define DLG_RESULT_INPUT    0xFF10
#define DLG_RESULT_FILE     0xFF20
#define DLG_RESULT_FILE_SAVE  0xFF21
#define DLG_RESULT_FIND_NEXT  0xFF31
#define DLG_RESULT_REPLACE    0xFF32
#define DLG_RESULT_REPLACE_ALL 0xFF33

/* ========================================================================
 * Theme constants
 * ======================================================================== */

#define THEME_BUTTON_FACE    COLOR_LIGHT_GRAY
#define THEME_TITLE_HEIGHT   20
#define THEME_MENU_HEIGHT    20
#define THEME_BORDER_WIDTH    4

/* ========================================================================
 * Display / taskbar constants
 * ======================================================================== */

/* Default desktop resolution */
#define DISPLAY_WIDTH   640
#define DISPLAY_HEIGHT  480
#define TASKBAR_HEIGHT   28

/* Video mode constants */
#define VIDEO_MODE_640x480x16   0   /* Desktop: 640x480, 4bpp, 16 colors   */
#define VIDEO_MODE_320x240x256  1   /* Fullscreen: 320x240, 8bpp, 256 colors */

/* ========================================================================
 * Font constants
 * ======================================================================== */

#define FONT_UI_WIDTH    6
#define FONT_UI_HEIGHT  12

/* ========================================================================
 * FreeRTOS timer types
 * ======================================================================== */

typedef void *TimerHandle_t;
typedef void (*TimerCallbackFunction_t)(TimerHandle_t xTimer);

#ifndef pdMS_TO_TICKS
#define pdMS_TO_TICKS(xTimeInMs) ((uint32_t)((xTimeInMs) * configTICK_RATE_HZ / 1000))
#endif

#ifndef configTICK_RATE_HZ
#define configTICK_RATE_HZ 1000
#endif

#ifndef pdTRUE
#define pdTRUE  1
#endif

#ifndef pdFALSE
#define pdFALSE 0
#endif

#ifndef portMAX_DELAY
#define portMAX_DELAY 0xFFFFFFFFUL
#endif

/* ========================================================================
 * Inline sys_table wrappers — FRANK OS GUI API (indices 404–433)
 * ======================================================================== */

/* 404: wm_create_window */
static inline hwnd_t wm_create_window(int16_t x, int16_t y, int16_t w, int16_t h,
                                        const char *title, uint16_t style,
                                        event_handler_t event_cb,
                                        paint_handler_t paint_cb) {
    typedef hwnd_t (*fn_t)(int16_t, int16_t, int16_t, int16_t,
                           const char*, uint16_t, event_handler_t, paint_handler_t);
    return ((fn_t)_sys_table_ptrs[404])(x, y, w, h, title, style, event_cb, paint_cb);
}

/* 405: wm_destroy_window */
static inline void wm_destroy_window(hwnd_t hwnd) {
    typedef void (*fn_t)(hwnd_t);
    ((fn_t)_sys_table_ptrs[405])(hwnd);
}

/* 406: wm_show_window */
static inline void wm_show_window(hwnd_t hwnd) {
    typedef void (*fn_t)(hwnd_t);
    ((fn_t)_sys_table_ptrs[406])(hwnd);
}

/* 407: wm_set_focus */
static inline void wm_set_focus(hwnd_t hwnd) {
    typedef void (*fn_t)(hwnd_t);
    ((fn_t)_sys_table_ptrs[407])(hwnd);
}

/* 408: wm_get_window */
static inline window_t *wm_get_window(hwnd_t hwnd) {
    typedef window_t *(*fn_t)(hwnd_t);
    return ((fn_t)_sys_table_ptrs[408])(hwnd);
}

/* 409: wm_set_window_rect */
static inline void wm_set_window_rect(hwnd_t hwnd, int16_t x, int16_t y,
                                        int16_t w, int16_t h) {
    typedef void (*fn_t)(hwnd_t, int16_t, int16_t, int16_t, int16_t);
    ((fn_t)_sys_table_ptrs[409])(hwnd, x, y, w, h);
}

/* 410: wm_invalidate */
static inline void wm_invalidate(hwnd_t hwnd) {
    typedef void (*fn_t)(hwnd_t);
    ((fn_t)_sys_table_ptrs[410])(hwnd);
}

/* 411: wm_post_event */
static inline bool wm_post_event(hwnd_t hwnd, const window_event_t *event) {
    typedef bool (*fn_t)(hwnd_t, const window_event_t*);
    return ((fn_t)_sys_table_ptrs[411])(hwnd, event);
}

/* 412: wm_get_client_rect */
static inline rect_t wm_get_client_rect(hwnd_t hwnd) {
    typedef rect_t (*fn_t)(hwnd_t);
    return ((fn_t)_sys_table_ptrs[412])(hwnd);
}

/* 413: wd_begin */
static inline void wd_begin(hwnd_t hwnd) {
    typedef void (*fn_t)(hwnd_t);
    ((fn_t)_sys_table_ptrs[413])(hwnd);
}

/* 414: wd_end */
static inline void wd_end(void) {
    typedef void (*fn_t)(void);
    ((fn_t)_sys_table_ptrs[414])();
}

/* 415: wd_pixel */
static inline void wd_pixel(int16_t x, int16_t y, uint8_t color) {
    typedef void (*fn_t)(int16_t, int16_t, uint8_t);
    ((fn_t)_sys_table_ptrs[415])(x, y, color);
}

/* 416: wd_hline */
static inline void wd_hline(int16_t x, int16_t y, int16_t w, uint8_t color) {
    typedef void (*fn_t)(int16_t, int16_t, int16_t, uint8_t);
    ((fn_t)_sys_table_ptrs[416])(x, y, w, color);
}

/* 417: wd_vline */
static inline void wd_vline(int16_t x, int16_t y, int16_t h, uint8_t color) {
    typedef void (*fn_t)(int16_t, int16_t, int16_t, uint8_t);
    ((fn_t)_sys_table_ptrs[417])(x, y, h, color);
}

/* 418: wd_fill_rect */
static inline void wd_fill_rect(int16_t x, int16_t y, int16_t w, int16_t h,
                                  uint8_t color) {
    typedef void (*fn_t)(int16_t, int16_t, int16_t, int16_t, uint8_t);
    ((fn_t)_sys_table_ptrs[418])(x, y, w, h, color);
}

/* 419: wd_clear */
static inline void wd_clear(uint8_t color) {
    typedef void (*fn_t)(uint8_t);
    ((fn_t)_sys_table_ptrs[419])(color);
}

/* 420: wd_rect */
static inline void wd_rect(int16_t x, int16_t y, int16_t w, int16_t h,
                             uint8_t color) {
    typedef void (*fn_t)(int16_t, int16_t, int16_t, int16_t, uint8_t);
    ((fn_t)_sys_table_ptrs[420])(x, y, w, h, color);
}

/* 421: wd_bevel_rect */
static inline void wd_bevel_rect(int16_t x, int16_t y, int16_t w, int16_t h,
                                   uint8_t light, uint8_t dark, uint8_t face) {
    typedef void (*fn_t)(int16_t, int16_t, int16_t, int16_t,
                         uint8_t, uint8_t, uint8_t);
    ((fn_t)_sys_table_ptrs[421])(x, y, w, h, light, dark, face);
}

/* 422: wd_char_ui */
static inline void wd_char_ui(int16_t x, int16_t y, char c,
                                uint8_t fg, uint8_t bg) {
    typedef void (*fn_t)(int16_t, int16_t, char, uint8_t, uint8_t);
    ((fn_t)_sys_table_ptrs[422])(x, y, c, fg, bg);
}

/* 423: wd_text_ui */
static inline void wd_text_ui(int16_t x, int16_t y, const char *str,
                                uint8_t fg, uint8_t bg) {
    typedef void (*fn_t)(int16_t, int16_t, const char*, uint8_t, uint8_t);
    ((fn_t)_sys_table_ptrs[423])(x, y, str, fg, bg);
}

/* 424: menu_set */
static inline void menu_set(hwnd_t hwnd, const menu_bar_t *bar) {
    typedef void (*fn_t)(hwnd_t, const menu_bar_t*);
    ((fn_t)_sys_table_ptrs[424])(hwnd, bar);
}

/* 425: dialog_show */
static inline hwnd_t dialog_show(hwnd_t parent, const char *title,
                                   const char *text, uint8_t icon,
                                   uint8_t buttons) {
    typedef hwnd_t (*fn_t)(hwnd_t, const char*, const char*, uint8_t, uint8_t);
    return ((fn_t)_sys_table_ptrs[425])(parent, title, text, icon, buttons);
}

/* 426: taskbar_invalidate */
static inline void taskbar_invalidate(void) {
    typedef void (*fn_t)(void);
    ((fn_t)_sys_table_ptrs[426])();
}

/* 427: xTimerCreate */
static inline TimerHandle_t xTimerCreate(const char *name, uint32_t period,
                                           uint32_t autoReload, void *pvTimerID,
                                           TimerCallbackFunction_t callback) {
    typedef TimerHandle_t (*fn_t)(const char*, uint32_t, uint32_t, void*,
                                   TimerCallbackFunction_t);
    return ((fn_t)_sys_table_ptrs[427])(name, period, autoReload, pvTimerID, callback);
}

/* 428: xTimerGenericCommandFromTask — underlying function for timer macros.
 * FreeRTOS command IDs: START=1, STOP=3, DELETE=5 */
#define _tmrCOMMAND_START           1
#define _tmrCOMMAND_STOP            3
#define _tmrCOMMAND_CHANGE_PERIOD   4
#define _tmrCOMMAND_DELETE          5

/* xTaskGetTickCount is at sys_table index 17 */
static inline uint32_t _app_xTaskGetTickCount(void) {
    typedef uint32_t (*fn_t)(void);
    return ((fn_t)_sys_table_ptrs[17])();
}
#define xTaskGetTickCount _app_xTaskGetTickCount

static inline int32_t xTimerStart(TimerHandle_t xTimer, uint32_t xTicksToWait) {
    typedef int32_t (*fn_t)(TimerHandle_t, int32_t, uint32_t, int32_t*, uint32_t);
    return ((fn_t)_sys_table_ptrs[428])(xTimer, _tmrCOMMAND_START,
                                         _app_xTaskGetTickCount(), 0, xTicksToWait);
}

static inline int32_t xTimerStop(TimerHandle_t xTimer, uint32_t xTicksToWait) {
    typedef int32_t (*fn_t)(TimerHandle_t, int32_t, uint32_t, int32_t*, uint32_t);
    return ((fn_t)_sys_table_ptrs[428])(xTimer, _tmrCOMMAND_STOP,
                                         0, 0, xTicksToWait);
}

static inline int32_t xTimerChangePeriod(TimerHandle_t xTimer,
                                          uint32_t xNewPeriodTicks,
                                          uint32_t xTicksToWait) {
    typedef int32_t (*fn_t)(TimerHandle_t, int32_t, uint32_t, int32_t*, uint32_t);
    return ((fn_t)_sys_table_ptrs[428])(xTimer, _tmrCOMMAND_CHANGE_PERIOD,
                                         xNewPeriodTicks, 0, xTicksToWait);
}

static inline int32_t xTimerDelete(TimerHandle_t xTimer, uint32_t xTicksToWait) {
    typedef int32_t (*fn_t)(TimerHandle_t, int32_t, uint32_t, int32_t*, uint32_t);
    return ((fn_t)_sys_table_ptrs[428])(xTimer, _tmrCOMMAND_DELETE,
                                         0, 0, xTicksToWait);
}

/* 429: pvTimerGetTimerID */
static inline void *pvTimerGetTimerID(TimerHandle_t xTimer) {
    typedef void *(*fn_t)(TimerHandle_t);
    return ((fn_t)_sys_table_ptrs[429])(xTimer);
}

/* 430: xTaskGenericNotify — underlying function for xTaskNotifyGive.
 * eIncrement = 3 (eNotifyAction enum value) */
#define _eIncrement 3

static inline int32_t xTaskNotifyGive(void *xTaskToNotify) {
    typedef int32_t (*fn_t)(void*, uint32_t, uint32_t, int32_t, uint32_t*);
    return ((fn_t)_sys_table_ptrs[430])(xTaskToNotify, 0, 0, _eIncrement, 0);
}

/* 431: ulTaskGenericNotifyTake */
static inline uint32_t ulTaskNotifyTake(int32_t xClearCountOnExit,
                                          uint32_t xTicksToWait) {
    typedef uint32_t (*fn_t)(uint32_t, int32_t, uint32_t);
    return ((fn_t)_sys_table_ptrs[431])(0, xClearCountOnExit, xTicksToWait);
}

/* ========================================================================
 * File dialog API (indices 439–440)
 * ======================================================================== */

/* 439: file_dialog_open */
static inline hwnd_t file_dialog_open(hwnd_t parent, const char *title,
                                       const char *initial_path,
                                       const char *filter_ext) {
    typedef hwnd_t (*fn_t)(hwnd_t, const char*, const char*, const char*);
    return ((fn_t)_sys_table_ptrs[439])(parent, title, initial_path, filter_ext);
}

/* 440: file_dialog_get_path */
static inline const char *file_dialog_get_path(void) {
    typedef const char *(*fn_t)(void);
    return ((fn_t)_sys_table_ptrs[440])();
}

/* 475: file_dialog_save */
static inline hwnd_t file_dialog_save(hwnd_t parent, const char *title,
                                        const char *initial_path,
                                        const char *filter_ext,
                                        const char *initial_name) {
    typedef hwnd_t (*fn_t)(hwnd_t, const char*, const char*, const char*,
                           const char*);
    return ((fn_t)_sys_table_ptrs[475])(parent, title, initial_path,
                                         filter_ext, initial_name);
}

/* 441: wd_button — standard Win95-style push button */
static inline void wd_button(int16_t x, int16_t y, int16_t w, int16_t h,
                              const char *label, bool focused, bool pressed) {
    typedef void (*fn_t)(int16_t, int16_t, int16_t, int16_t,
                         const char*, bool, bool);
    ((fn_t)_sys_table_ptrs[441])(x, y, w, h, label, focused, pressed);
}

/* 442: wd_fb_ptr — direct framebuffer pointer for fast rendering.
 * Returns pointer to framebuffer byte at client (cx,cy).
 * cx must be even (pair-encoded byte boundary).
 * Sets *stride to FB_STRIDE (320). Returns NULL if no active context. */
static inline uint8_t *wd_fb_ptr(int16_t cx, int16_t cy, int16_t *stride) {
    typedef uint8_t *(*fn_t)(int16_t, int16_t, int16_t*);
    return ((fn_t)_sys_table_ptrs[442])(cx, cy, stride);
}

/* 501: wd_get_clip_size — returns clipped client area dimensions (pixels).
 * After wd_begin(), these reflect how much of the client area is visible.
 * Apps using wd_fb_ptr() must limit writes to these bounds. */
static inline void wd_get_clip_size(int16_t *w, int16_t *h) {
    typedef void (*fn_t)(int16_t*, int16_t*);
    ((fn_t)_sys_table_ptrs[501])(w, h);
}

/* ========================================================================
 * Clipboard API (indices 451–454)
 * ======================================================================== */

/* 451: clipboard_set_text */
static inline bool clipboard_set_text(const char *text, uint16_t len) {
    typedef bool (*fn_t)(const char*, uint16_t);
    return ((fn_t)_sys_table_ptrs[451])(text, len);
}

/* 452: clipboard_get_text */
static inline const char *clipboard_get_text(void) {
    typedef const char *(*fn_t)(void);
    return ((fn_t)_sys_table_ptrs[452])();
}

/* 453: clipboard_get_length */
static inline uint16_t clipboard_get_length(void) {
    typedef uint16_t (*fn_t)(void);
    return ((fn_t)_sys_table_ptrs[453])();
}

/* 454: clipboard_clear */
static inline void clipboard_clear(void) {
    typedef void (*fn_t)(void);
    ((fn_t)_sys_table_ptrs[454])();
}

/* ========================================================================
 * Scrollbar control (indices 455–459)
 * ======================================================================== */

#define SCROLLBAR_WIDTH  16

typedef struct {
    int16_t  x, y, w, h;
    bool     horizontal;
    bool     visible;
    int32_t  range;
    int32_t  page;
    int32_t  pos;
    bool     dragging;
    int16_t  drag_offset;
} scrollbar_t;

/* 455: scrollbar_init */
static inline void scrollbar_init(scrollbar_t *sb, bool horizontal) {
    typedef void (*fn_t)(scrollbar_t*, bool);
    ((fn_t)_sys_table_ptrs[455])(sb, horizontal);
}

/* 456: scrollbar_set_range */
static inline void scrollbar_set_range(scrollbar_t *sb, int32_t range,
                                         int32_t page) {
    typedef void (*fn_t)(scrollbar_t*, int32_t, int32_t);
    ((fn_t)_sys_table_ptrs[456])(sb, range, page);
}

/* 457: scrollbar_set_pos */
static inline void scrollbar_set_pos(scrollbar_t *sb, int32_t pos) {
    typedef void (*fn_t)(scrollbar_t*, int32_t);
    ((fn_t)_sys_table_ptrs[457])(sb, pos);
}

/* 458: scrollbar_paint */
static inline void scrollbar_paint(scrollbar_t *sb) {
    typedef void (*fn_t)(scrollbar_t*);
    ((fn_t)_sys_table_ptrs[458])(sb);
}

/* 459: scrollbar_event */
static inline bool scrollbar_event(scrollbar_t *sb,
                                     const window_event_t *event,
                                     int32_t *new_pos) {
    typedef bool (*fn_t)(scrollbar_t*, const window_event_t*, int32_t*);
    return ((fn_t)_sys_table_ptrs[459])(sb, event, new_pos);
}

/* ========================================================================
 * Textarea control (indices 460–474)
 * ======================================================================== */

#define TEXTAREA_MAX_SIZE  32768

typedef struct {
    char    *buf;
    int32_t  buf_size;
    int32_t  len;
    int32_t  cursor;
    int32_t  sel_anchor;
    bool     cursor_visible;
    int16_t  rect_x, rect_y;
    int16_t  rect_w, rect_h;
    int32_t  scroll_x;
    int32_t  scroll_y;
    scrollbar_t  vscroll;
    scrollbar_t  hscroll;
    hwnd_t   hwnd;
    int32_t  total_lines;
    int32_t  max_line_width;
} textarea_t;

/* 460: textarea_init */
static inline void textarea_init(textarea_t *ta, char *buf, int32_t buf_size,
                                   hwnd_t hwnd) {
    typedef void (*fn_t)(textarea_t*, char*, int32_t, hwnd_t);
    ((fn_t)_sys_table_ptrs[460])(ta, buf, buf_size, hwnd);
}

/* 461: textarea_set_text */
static inline void textarea_set_text(textarea_t *ta, const char *text,
                                       int32_t len) {
    typedef void (*fn_t)(textarea_t*, const char*, int32_t);
    ((fn_t)_sys_table_ptrs[461])(ta, text, len);
}

/* 462: textarea_get_text */
static inline const char *textarea_get_text(textarea_t *ta) {
    typedef const char *(*fn_t)(textarea_t*);
    return ((fn_t)_sys_table_ptrs[462])(ta);
}

/* 463: textarea_get_length */
static inline int32_t textarea_get_length(textarea_t *ta) {
    typedef int32_t (*fn_t)(textarea_t*);
    return ((fn_t)_sys_table_ptrs[463])(ta);
}

/* 464: textarea_set_rect */
static inline void textarea_set_rect(textarea_t *ta, int16_t x, int16_t y,
                                       int16_t w, int16_t h) {
    typedef void (*fn_t)(textarea_t*, int16_t, int16_t, int16_t, int16_t);
    ((fn_t)_sys_table_ptrs[464])(ta, x, y, w, h);
}

/* 465: textarea_paint */
static inline void textarea_paint(textarea_t *ta) {
    typedef void (*fn_t)(textarea_t*);
    ((fn_t)_sys_table_ptrs[465])(ta);
}

/* 466: textarea_event */
static inline bool textarea_event(textarea_t *ta,
                                    const window_event_t *event) {
    typedef bool (*fn_t)(textarea_t*, const window_event_t*);
    return ((fn_t)_sys_table_ptrs[466])(ta, event);
}

/* 467: textarea_cut */
static inline void textarea_cut(textarea_t *ta) {
    typedef void (*fn_t)(textarea_t*);
    ((fn_t)_sys_table_ptrs[467])(ta);
}

/* 468: textarea_copy */
static inline void textarea_copy(textarea_t *ta) {
    typedef void (*fn_t)(textarea_t*);
    ((fn_t)_sys_table_ptrs[468])(ta);
}

/* 469: textarea_paste */
static inline void textarea_paste(textarea_t *ta) {
    typedef void (*fn_t)(textarea_t*);
    ((fn_t)_sys_table_ptrs[469])(ta);
}

/* 470: textarea_select_all */
static inline void textarea_select_all(textarea_t *ta) {
    typedef void (*fn_t)(textarea_t*);
    ((fn_t)_sys_table_ptrs[470])(ta);
}

/* 471: textarea_find */
static inline bool textarea_find(textarea_t *ta, const char *needle,
                                   bool case_sensitive, bool forward) {
    typedef bool (*fn_t)(textarea_t*, const char*, bool, bool);
    return ((fn_t)_sys_table_ptrs[471])(ta, needle, case_sensitive, forward);
}

/* 472: textarea_replace */
static inline bool textarea_replace(textarea_t *ta, const char *needle,
                                      const char *replacement,
                                      bool case_sensitive) {
    typedef bool (*fn_t)(textarea_t*, const char*, const char*, bool);
    return ((fn_t)_sys_table_ptrs[472])(ta, needle, replacement, case_sensitive);
}

/* 473: textarea_replace_all */
static inline int textarea_replace_all(textarea_t *ta, const char *needle,
                                         const char *replacement,
                                         bool case_sensitive) {
    typedef int (*fn_t)(textarea_t*, const char*, const char*, bool);
    return ((fn_t)_sys_table_ptrs[473])(ta, needle, replacement, case_sensitive);
}

/* 474: textarea_blink */
static inline void textarea_blink(textarea_t *ta) {
    typedef void (*fn_t)(textarea_t*);
    ((fn_t)_sys_table_ptrs[474])(ta);
}

/* ========================================================================
 * Find/Replace dialog API (indices 476–481)
 * ======================================================================== */

/* 476: find_dialog_show */
static inline hwnd_t find_dialog_show(hwnd_t parent) {
    typedef hwnd_t (*fn_t)(hwnd_t);
    return ((fn_t)_sys_table_ptrs[476])(parent);
}

/* 477: replace_dialog_show */
static inline hwnd_t replace_dialog_show(hwnd_t parent) {
    typedef hwnd_t (*fn_t)(hwnd_t);
    return ((fn_t)_sys_table_ptrs[477])(parent);
}

/* 478: find_dialog_get_text */
static inline const char *find_dialog_get_text(void) {
    typedef const char *(*fn_t)(void);
    return ((fn_t)_sys_table_ptrs[478])();
}

/* 479: find_dialog_get_replace_text */
static inline const char *find_dialog_get_replace_text(void) {
    typedef const char *(*fn_t)(void);
    return ((fn_t)_sys_table_ptrs[479])();
}

/* 480: find_dialog_case_sensitive */
static inline bool find_dialog_case_sensitive(void) {
    typedef bool (*fn_t)(void);
    return ((fn_t)_sys_table_ptrs[480])();
}

/* 481: find_dialog_close */
static inline void find_dialog_close(void) {
    typedef void (*fn_t)(void);
    ((fn_t)_sys_table_ptrs[481])();
}

/* 482: wm_mark_dirty — trigger compositor loop without marking any
 * window for repaint.  Useful for apps that do direct buffer writes
 * and need the event loop to keep dispatching mouse events. */
static inline void wm_mark_dirty(void) {
    typedef void (*fn_t)(void);
    ((fn_t)_sys_table_ptrs[482])();
}

/* ========================================================================
 * File association API (indices 494–500)
 * ======================================================================== */

#define FA_MAX_APPS        16
#define FA_MAX_EXTS        8
#define FA_EXT_LEN         8
#define FA_NAME_LEN        20
#define FA_PATH_LEN        32
#define FA_ICON_SIZE       256

typedef struct {
    char     name[FA_NAME_LEN];
    char     path[FA_PATH_LEN];
    uint8_t  icon[FA_ICON_SIZE];
    bool     has_icon;
    char     exts[FA_MAX_EXTS][FA_EXT_LEN];
    uint8_t  ext_count;
} fa_app_t;

/* 494: file_assoc_scan */
static inline void file_assoc_scan(void) {
    typedef void (*fn_t)(void);
    ((fn_t)_sys_table_ptrs[494])();
}

/* 495: file_assoc_find */
static inline const fa_app_t *file_assoc_find(const char *ext) {
    typedef const fa_app_t *(*fn_t)(const char *);
    return ((fn_t)_sys_table_ptrs[495])(ext);
}

/* 496: file_assoc_find_all */
static inline int file_assoc_find_all(const char *ext,
                                       const fa_app_t **out, int max) {
    typedef int (*fn_t)(const char *, const fa_app_t **, int);
    return ((fn_t)_sys_table_ptrs[496])(ext, out, max);
}

/* 497: file_assoc_open */
static inline bool file_assoc_open(const char *file_path) {
    typedef bool (*fn_t)(const char *);
    return ((fn_t)_sys_table_ptrs[497])(file_path);
}

/* 498: file_assoc_open_with */
static inline bool file_assoc_open_with(const char *file_path,
                                         const char *app_path) {
    typedef bool (*fn_t)(const char *, const char *);
    return ((fn_t)_sys_table_ptrs[498])(file_path, app_path);
}

/* 499: file_assoc_get_apps */
static inline const fa_app_t *file_assoc_get_apps(int *count) {
    typedef const fa_app_t *(*fn_t)(int *);
    return ((fn_t)_sys_table_ptrs[499])(count);
}

/* 500: desktop_add_shortcut */
static inline bool desktop_add_shortcut(const char *path) {
    typedef bool (*fn_t)(const char *);
    return ((fn_t)_sys_table_ptrs[500])(path);
}

/* 502: wm_toggle_fullscreen — toggle fullscreen mode for a window.
 * Strips borders/menubar and expands to full screen; calling again restores. */
static inline void wm_toggle_fullscreen(hwnd_t hwnd) {
    typedef void (*fn_t)(hwnd_t);
    ((fn_t)_sys_table_ptrs[502])(hwnd);
}

/* 503: wm_is_fullscreen — returns true if window is in fullscreen mode */
static inline bool wm_is_fullscreen(hwnd_t hwnd) {
    typedef bool (*fn_t)(hwnd_t);
    return ((fn_t)_sys_table_ptrs[503])(hwnd);
}

/* ========================================================================
 * Video mode API (indices 515–521)
 * ======================================================================== */

/* 515: display_set_video_mode — switch video mode at runtime.
 * mode: VIDEO_MODE_640x480x16 or VIDEO_MODE_320x240x256.
 * Stops DVI, reconfigures, clears framebuffer, restarts DVI.
 * Returns 0 on success, negative on error.
 * WARNING: In 320x240x256 mode, the desktop/windowed UI is not available.
 *          Use this for fullscreen apps only. Restore 640x480x16 before exit. */
static inline int display_set_video_mode(uint8_t mode) {
    typedef int (*fn_t)(uint8_t);
    return ((fn_t)_sys_table_ptrs[515])(mode);
}

/* 516: display_get_video_mode — returns current VIDEO_MODE_* */
static inline uint8_t display_get_video_mode(void) {
    typedef uint8_t (*fn_t)(void);
    return ((fn_t)_sys_table_ptrs[516])();
}

/* 517: display_set_palette_entry — set one entry in the 256-color palette.
 * index: 0..255, rgb888: 0xRRGGBB.
 * Only meaningful in VIDEO_MODE_320x240x256. */
static inline void display_set_palette_entry(uint8_t index, uint32_t rgb888) {
    typedef void (*fn_t)(uint8_t, uint32_t);
    ((fn_t)_sys_table_ptrs[517])(index, rgb888);
}

/* 518-521: Runtime display dimensions (read via pointer dereference).
 * These reflect the currently active video mode. */
static inline uint16_t display_get_width(void) {
    return *(const uint16_t *)_sys_table_ptrs[518];
}
static inline uint16_t display_get_height(void) {
    return *(const uint16_t *)_sys_table_ptrs[519];
}
static inline uint16_t display_get_fb_stride(void) {
    return *(const uint16_t *)_sys_table_ptrs[520];
}
static inline uint8_t display_get_bpp(void) {
    return *(const uint8_t *)_sys_table_ptrs[521];
}

/* 522: display_get_framebuffer — direct pointer to the draw buffer.
 * In VIDEO_MODE_320x240x256: 320×240 bytes, 1 byte per pixel (palette index).
 * In VIDEO_MODE_640x480x16:  320×480 bytes, 4bpp packed (2 pixels per byte).
 * The pointer is dereferenced at call time (pointer-to-pointer in sys_table). */
static inline uint8_t *display_get_framebuffer(void) {
    return *(uint8_t **)_sys_table_ptrs[522];
}

/* 523: display_set_pixel — mode-aware pixel set with bounds check */
static inline void display_set_pixel(int x, int y, uint8_t color) {
    typedef void (*fn_t)(int, int, uint8_t);
    ((fn_t)_sys_table_ptrs[523])(x, y, color);
}

/* 524: display_clear — clear entire framebuffer to a color */
static inline void display_clear(uint8_t color) {
    typedef void (*fn_t)(uint8_t);
    ((fn_t)_sys_table_ptrs[524])(color);
}

/* 525: display_wait_vsync — block until next vertical blank */
static inline void display_wait_vsync(void) {
    typedef void (*fn_t)(void);
    ((fn_t)_sys_table_ptrs[525])();
}

/* 526: display_swap_buffers — swap front/back buffers (currently no-op) */
static inline void display_swap_buffers(void) {
    typedef void (*fn_t)(void);
    ((fn_t)_sys_table_ptrs[526])();
}

/* ========================================================================
 * Direct keyboard API for fullscreen apps (indices 527–528)
 *
 * In VIDEO_MODE_320x240x256 the windowed UI / compositor is suspended,
 * so WM_KEYDOWN / WM_KEYUP events are not dispatched.  Fullscreen apps
 * must poll the keyboard directly using these two functions.
 * ======================================================================== */

typedef struct {
    uint8_t hid_code;   /* HID usage code (same scancodes as WM_KEYDOWN) */
    uint8_t ascii;      /* ASCII translation (0 if non-printable)        */
    uint8_t modifiers;  /* Active modifier mask at time of event         */
    bool    pressed;    /* true = press, false = release                 */
} app_key_event_t;

/* 527: keyboard_poll — read pending PS/2 scancodes into the event queue.
 * Call this at least once per frame (~60 Hz) to avoid losing key events. */
static inline void keyboard_poll(void) {
    typedef void (*fn_t)(void);
    ((fn_t)_sys_table_ptrs[527])();
}

/* 528: keyboard_get_event — dequeue one key event.
 * Returns true if an event was available, false if queue is empty. */
static inline bool keyboard_get_event(app_key_event_t *ev) {
    typedef bool (*fn_t)(app_key_event_t *);
    return ((fn_t)_sys_table_ptrs[528])(ev);
}

/* 504: wm_find_window_by_title — find alive+visible window by title.
 * Returns HWND_NULL if none found. Used for singleton enforcement. */
static inline hwnd_t wm_find_window_by_title(const char *title) {
    typedef hwnd_t (*fn_t)(const char *);
    return ((fn_t)_sys_table_ptrs[504])(title);
}

/* 505: app_launch_deferred — request a deferred app launch.
 * Safe to call from ELF app context; the compositor loop performs the
 * actual launch_elf_app_with_file call on the next frame. */
static inline void app_launch_deferred(const char *app_path,
                                        const char *file_path) {
    typedef void (*fn_t)(const char *, const char *);
    ((fn_t)_sys_table_ptrs[505])(app_path, file_path);
}

/* ========================================================================
 * MIDI OPL FM synthesis API (indices 506–511)
 * ======================================================================== */

typedef struct midi_opl midi_opl_t;

/* 506: midi_opl_init */
static inline midi_opl_t *midi_opl_init(void) {
    typedef midi_opl_t *(*fn_t)(void);
    return ((fn_t)_sys_table_ptrs[506])();
}

/* 507: midi_opl_load */
static inline bool midi_opl_load(midi_opl_t *ctx, const char *filepath) {
    typedef bool (*fn_t)(midi_opl_t*, const char*);
    return ((fn_t)_sys_table_ptrs[507])(ctx, filepath);
}

/* 508: midi_opl_render */
static inline int midi_opl_render(midi_opl_t *ctx, int16_t *buf, int max_frames) {
    typedef int (*fn_t)(midi_opl_t*, int16_t*, int);
    return ((fn_t)_sys_table_ptrs[508])(ctx, buf, max_frames);
}

/* 509: midi_opl_playing */
static inline bool midi_opl_playing(midi_opl_t *ctx) {
    typedef bool (*fn_t)(midi_opl_t*);
    return ((fn_t)_sys_table_ptrs[509])(ctx);
}

/* 510: midi_opl_set_loop */
static inline void midi_opl_set_loop(midi_opl_t *ctx, bool loop) {
    typedef void (*fn_t)(midi_opl_t*, bool);
    ((fn_t)_sys_table_ptrs[510])(ctx, loop);
}

/* 511: midi_opl_free */
static inline void midi_opl_free(midi_opl_t *ctx) {
    typedef void (*fn_t)(midi_opl_t*);
    ((fn_t)_sys_table_ptrs[511])(ctx);
}

/* 554: wd_radio — Win95-style radio button */
static inline void wd_radio(int16_t x, int16_t y, const char *label,
                             bool selected) {
    typedef void (*fn_t)(int16_t, int16_t, const char*, bool);
    ((fn_t)_sys_table_ptrs[554])(x, y, label, selected);
}

/* Count UTF-8 characters (not bytes) for width calculations */
static inline int gfx_utf8_charcount(const char *str) {
    int count = 0;
    while (*str) {
        uint8_t b = (uint8_t)*str;
        if (b < 0x80)        str += 1;
        else if (b < 0xE0)   str += 2;
        else if (b < 0xF0)   str += 3;
        else                  str += 4;
        count++;
    }
    return count;
}

/* ========================================================================
 * Localization API (indices 551–553)
 * ======================================================================== */

#define LANG_EN  0
#define LANG_RU  1

/* 551: lang_get */
static inline uint8_t lang_get(void) {
    typedef uint8_t (*fn_t)(void);
    return ((fn_t)_sys_table_ptrs[551])();
}

/* 552: L — get localized string by ID */
static inline const char *L(int str_id) {
    typedef const char *(*fn_t)(int);
    return ((fn_t)_sys_table_ptrs[552])(str_id);
}

/* 553: lang_set */
static inline void lang_set(uint8_t lang) {
    typedef void (*fn_t)(uint8_t);
    ((fn_t)_sys_table_ptrs[553])(lang);
}

#ifdef __cplusplus
}
#endif

#endif /* FRANKOS_APP_H */
