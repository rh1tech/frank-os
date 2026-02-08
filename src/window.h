#ifndef WINDOW_H
#define WINDOW_H

#include <stdint.h>
#include <stdbool.h>

/*==========================================================================
 * Geometry types
 *=========================================================================*/

typedef struct {
    int16_t x, y, w, h;
} rect_t;

typedef struct {
    int16_t x, y;
} point_t;

/*==========================================================================
 * Color constants (CGA/EGA 16-color palette indices)
 *=========================================================================*/

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

/*==========================================================================
 * Window handle
 *=========================================================================*/

/* 0 = null/invalid, 1-16 = valid window handles */
typedef uint8_t hwnd_t;

#define HWND_NULL    0
#define WM_MAX_WINDOWS 16

/*==========================================================================
 * Window flags (bitfield in window_t.flags)
 *=========================================================================*/

#define WF_ALIVE     (1u << 0)
#define WF_VISIBLE   (1u << 1)
#define WF_FOCUSED   (1u << 2)
#define WF_CLOSABLE  (1u << 3)
#define WF_RESIZABLE (1u << 4)
#define WF_MOVABLE   (1u << 5)
#define WF_BORDER    (1u << 6)
#define WF_DIRTY     (1u << 7)

/*==========================================================================
 * Window state
 *=========================================================================*/

#define WS_NORMAL    0
#define WS_MINIMIZED 1
#define WS_MAXIMIZED 2

/*==========================================================================
 * Window style presets (for wm_create_window)
 *=========================================================================*/

/* Standard overlapped window with title bar, border, all buttons */
#define WSTYLE_DEFAULT   (WF_CLOSABLE | WF_RESIZABLE | WF_MOVABLE | WF_BORDER)
/* Dialog: closable, movable, has border, not resizable */
#define WSTYLE_DIALOG    (WF_CLOSABLE | WF_MOVABLE | WF_BORDER)
/* Borderless popup */
#define WSTYLE_POPUP     0

/*==========================================================================
 * Forward declarations
 *=========================================================================*/

typedef struct window_event window_event_t; /* defined in window_event.h */
typedef struct window       window_t;

/* Event handler callback: return true if handled */
typedef bool (*event_handler_t)(hwnd_t hwnd, const window_event_t *event);

/* Paint handler callback: called during WM_PAINT */
typedef void (*paint_handler_t)(hwnd_t hwnd);

/*==========================================================================
 * Window structure (~52 bytes)
 *=========================================================================*/

struct window {
    uint8_t          flags;           /* WF_* bitfield */
    uint8_t          state;           /* WS_NORMAL / MINIMIZED / MAXIMIZED */
    rect_t           frame;           /* outer frame in screen coordinates */
    rect_t           restore_rect;    /* saved rect before maximize */
    uint8_t          bg_color;        /* client area background color */
    uint8_t          z_order;         /* position in z-stack (0=bottom) */
    char             title[24];       /* null-terminated title string */
    event_handler_t  event_handler;   /* event callback (may be NULL) */
    paint_handler_t  paint_handler;   /* paint callback (may be NULL) */
};

/* Size check — only meaningful on the 32-bit ARM target.
 * On 64-bit hosts (where clangd runs) pointers are 8 bytes. */
#if defined(__arm__) || defined(__thumb__)
_Static_assert(sizeof(window_t) <= 56, "window_t exceeds 56 bytes");
#endif

/*==========================================================================
 * Window Manager API
 *=========================================================================*/

/* Initialize the window manager — call once at startup */
void wm_init(void);

/* Create a new window. Returns HWND_NULL on failure.
 * x,y,w,h = outer frame. style = WF_* flags ORed together. */
hwnd_t wm_create_window(int16_t x, int16_t y, int16_t w, int16_t h,
                         const char *title, uint8_t style,
                         event_handler_t event_cb,
                         paint_handler_t paint_cb);

/* Destroy a window and free its slot */
void wm_destroy_window(hwnd_t hwnd);

/* Show / hide */
void wm_show_window(hwnd_t hwnd);
void wm_hide_window(hwnd_t hwnd);

/* State changes */
void wm_minimize_window(hwnd_t hwnd);
void wm_maximize_window(hwnd_t hwnd);
void wm_restore_window(hwnd_t hwnd);

/* Position / size */
void wm_move_window(hwnd_t hwnd, int16_t x, int16_t y);
void wm_resize_window(hwnd_t hwnd, int16_t w, int16_t h);
void wm_set_window_rect(hwnd_t hwnd, int16_t x, int16_t y,
                         int16_t w, int16_t h);

/* Focus management */
void   wm_set_focus(hwnd_t hwnd);
hwnd_t wm_get_focus(void);
void   wm_cycle_focus(void);   /* Alt+Tab behavior */

/* Query */
rect_t   wm_get_client_rect(hwnd_t hwnd);
window_t *wm_get_window(hwnd_t hwnd); /* NULL if invalid */

/* Invalidation — marks window for repaint */
void wm_invalidate(hwnd_t hwnd);

/* Set title string */
void wm_set_title(hwnd_t hwnd, const char *title);

/* Hit-test all visible windows top-to-bottom.
 * Returns the hwnd of the topmost window containing the point,
 * or HWND_NULL if the point is on the desktop. */
hwnd_t wm_window_at_point(int16_t x, int16_t y);

/* Compositor: repaint all visible windows back-to-front, then swap buffers */
void wm_composite(void);

#endif /* WINDOW_H */
