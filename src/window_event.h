#ifndef WINDOW_EVENT_H
#define WINDOW_EVENT_H

#include <stdint.h>
#include <stdbool.h>
#include "window.h"

/*==========================================================================
 * Event types (WM_* message IDs)
 *=========================================================================*/

#define WM_NULL          0

/* Lifecycle */
#define WM_CREATE        1
#define WM_DESTROY       2
#define WM_CLOSE         3

/* Paint */
#define WM_PAINT         4

/* Focus */
#define WM_SETFOCUS      5
#define WM_KILLFOCUS     6

/* Geometry */
#define WM_MOVE          7
#define WM_SIZE          8

/* State */
#define WM_MINIMIZE      9
#define WM_MAXIMIZE     10
#define WM_RESTORE      11

/* Keyboard */
#define WM_KEYDOWN      12
#define WM_KEYUP        13
#define WM_CHAR         14

/* Mouse */
#define WM_MOUSEMOVE    15
#define WM_LBUTTONDOWN  16
#define WM_LBUTTONUP    17
#define WM_RBUTTONDOWN  18
#define WM_RBUTTONUP    19

/* Misc */
#define WM_TIMER        20
#define WM_COMMAND      21

/*==========================================================================
 * Keyboard modifier flags
 *=========================================================================*/

#define KMOD_SHIFT  (1u << 0)
#define KMOD_CTRL   (1u << 1)
#define KMOD_ALT    (1u << 2)

/*==========================================================================
 * Common PS/2 scancodes (Set 1, make codes)
 *=========================================================================*/

#define KEY_ESC       0x01
#define KEY_1         0x02
#define KEY_2         0x03
#define KEY_3         0x04
#define KEY_4         0x05
#define KEY_5         0x06
#define KEY_6         0x07
#define KEY_7         0x08
#define KEY_8         0x09
#define KEY_9         0x0A
#define KEY_0         0x0B
#define KEY_MINUS     0x0C
#define KEY_EQUAL     0x0D
#define KEY_BACKSPACE 0x0E
#define KEY_TAB       0x0F
#define KEY_Q         0x10
#define KEY_W         0x11
#define KEY_E         0x12
#define KEY_R         0x13
#define KEY_T         0x14
#define KEY_Y         0x15
#define KEY_U         0x16
#define KEY_I         0x17
#define KEY_O         0x18
#define KEY_P         0x19
#define KEY_LBRACKET  0x1A
#define KEY_RBRACKET  0x1B
#define KEY_ENTER     0x1C
#define KEY_LCTRL     0x1D
#define KEY_A         0x1E
#define KEY_S         0x1F
#define KEY_D         0x20
#define KEY_F         0x21
#define KEY_G         0x22
#define KEY_H         0x23
#define KEY_J         0x24
#define KEY_K         0x25
#define KEY_L         0x26
#define KEY_SEMICOLON 0x27
#define KEY_QUOTE     0x28
#define KEY_BACKTICK  0x29
#define KEY_LSHIFT    0x2A
#define KEY_BACKSLASH 0x2B
#define KEY_Z         0x2C
#define KEY_X         0x2D
#define KEY_C         0x2E
#define KEY_V         0x2F
#define KEY_B         0x30
#define KEY_N         0x31
#define KEY_M         0x32
#define KEY_COMMA     0x33
#define KEY_DOT       0x34
#define KEY_SLASH     0x35
#define KEY_RSHIFT    0x36
#define KEY_LALT      0x38
#define KEY_SPACE     0x39
#define KEY_CAPSLOCK  0x3A
#define KEY_F1        0x3B
#define KEY_F2        0x3C
#define KEY_F3        0x3D
#define KEY_F4        0x3E
#define KEY_F5        0x3F
#define KEY_F6        0x40
#define KEY_F7        0x41
#define KEY_F8        0x42
#define KEY_F9        0x43
#define KEY_F10       0x44
#define KEY_F11       0x57
#define KEY_F12       0x58

/* Arrow keys (extended — E0 prefix stripped) */
#define KEY_UP        0x48
#define KEY_LEFT      0x4B
#define KEY_RIGHT     0x4D
#define KEY_DOWN      0x50
#define KEY_HOME      0x47
#define KEY_END       0x4F
#define KEY_PAGEUP    0x49
#define KEY_PAGEDOWN  0x51
#define KEY_INSERT    0x52
#define KEY_DELETE    0x53

/*==========================================================================
 * Event structure (~12 bytes) — tagged union
 *=========================================================================*/

struct window_event {
    uint8_t type;       /* WM_* event type */
    uint8_t _pad;
    union {
        /* WM_KEYDOWN, WM_KEYUP */
        struct {
            uint8_t scancode;
            uint8_t modifiers;  /* KMOD_* flags */
        } key;

        /* WM_CHAR */
        struct {
            char ch;
            uint8_t modifiers;
        } charev;

        /* WM_MOUSEMOVE, WM_LBUTTONDOWN/UP, WM_RBUTTONDOWN/UP */
        struct {
            int16_t x;         /* client-relative */
            int16_t y;
            uint8_t buttons;   /* bit 0 = left, bit 1 = right */
            uint8_t modifiers;
        } mouse;

        /* WM_MOVE */
        struct {
            int16_t x;
            int16_t y;
        } move;

        /* WM_SIZE */
        struct {
            int16_t w;
            int16_t h;
        } size;

        /* WM_COMMAND */
        struct {
            uint16_t id;
        } command;

        /* WM_TIMER */
        struct {
            uint16_t timer_id;
        } timer;
    };
};

/* Size verification — only on 32-bit ARM target */
#if defined(__arm__) || defined(__thumb__)
_Static_assert(sizeof(window_event_t) <= 12, "window_event_t exceeds 12 bytes");
#endif

/*==========================================================================
 * Event queue size
 *=========================================================================*/

#define WM_EVENT_QUEUE_SIZE  128

/*==========================================================================
 * Event dispatch API
 *=========================================================================*/

/* Initialize the event subsystem (spinlock, queue).  Call once at startup
 * before any events are posted. */
void wm_event_init(void);

/* Post an event to a window's queue.
 * Returns true if the event was enqueued. */
bool wm_post_event(hwnd_t hwnd, const window_event_t *event);

/* Post an event to the focused window */
bool wm_post_event_focused(const window_event_t *event);

/* Dispatch all queued events (call from main loop) */
void wm_dispatch_events(void);

/* Process a raw mouse input event from the input task.
 * Handles window-manager-level behavior: hit-testing, focus changes,
 * title-bar dragging, close button.  Client-area events are forwarded
 * to the target window's event handler via the event queue.
 * type = WM_LBUTTONDOWN / WM_LBUTTONUP / WM_MOUSEMOVE / WM_RBUTTONDOWN / WM_RBUTTONUP
 * x,y  = screen coordinates */
void wm_handle_mouse_input(uint8_t type, int16_t x, int16_t y, uint8_t buttons);

/* Query active drag state for outline drawing.
 * Returns true if a drag is in progress, fills output params. */
bool wm_get_drag_outline(hwnd_t *hwnd, int16_t *dx, int16_t *dy);

/*==========================================================================
 * Input state queries
 *=========================================================================*/

/* Current mouse position in screen coordinates */
point_t wm_get_mouse_pos(void);

/* Set / get cursor position (atomic packed word, safe cross-task) */
void wm_set_cursor_pos(int16_t x, int16_t y);
void wm_get_cursor_pos(int16_t *x, int16_t *y);

/* Set / get live mouse button state */
void    wm_set_mouse_buttons(uint8_t buttons);
uint8_t wm_get_mouse_buttons(void);

/* Is a key currently held down? (by PS/2 scancode) */
bool wm_is_key_down(uint8_t scancode);

/* Current modifier state */
uint8_t wm_get_modifiers(void);

/* Compositor dirty flag — set when anything visual changed.
 * display_task checks this to avoid recompositing idle frames,
 * which eliminates SRAM bus pressure that starves the HDMI DMA. */
void wm_mark_dirty(void);
bool wm_needs_composite(void);

#endif /* WINDOW_EVENT_H */
