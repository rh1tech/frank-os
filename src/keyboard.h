#ifndef KEYBOARD_H
#define KEYBOARD_H

#include <stdint.h>
#include <stdbool.h>
#include "ff.h"

#ifdef __cplusplus
extern "C" {
#endif

// Key event
typedef struct {
    uint8_t hid_code;   // HID usage code
    uint8_t ascii;      // ASCII translation (0 if non-printable)
    uint8_t modifiers;  // Active modifier mask at time of event
    bool    pressed;    // true = press, false = release
} key_event_t;

// Modifier masks (match HID spec, granular left/right)
#define KBD_MOD_LCTRL   (1 << 0)
#define KBD_MOD_LSHIFT  (1 << 1)
#define KBD_MOD_LALT    (1 << 2)
#define KBD_MOD_LGUI    (1 << 3)
#define KBD_MOD_RCTRL   (1 << 4)
#define KBD_MOD_RSHIFT  (1 << 5)
#define KBD_MOD_RALT    (1 << 6)
#define KBD_MOD_RGUI    (1 << 7)

#define KBD_MOD_SHIFT   (KBD_MOD_LSHIFT | KBD_MOD_RSHIFT)
#define KBD_MOD_CTRL    (KBD_MOD_LCTRL  | KBD_MOD_RCTRL)
#define KBD_MOD_ALT     (KBD_MOD_LALT   | KBD_MOD_RALT)

// Poll PS/2 keyboard for new scancodes. Call frequently from main loop.
void keyboard_poll(void);

// Get next key event. Returns true if an event was available.
bool keyboard_get_event(key_event_t *ev);

// MOS2 keyboard state (modifier tracking, current scancode)
typedef struct kbd_state {
    bool bCtrlPressed;
    bool bAltPressed;
    bool bDelPressed;
    bool bLeftShift;
    bool bRightShift;
    bool bRus;
    bool bCapsLock;
    bool bTabPressed;
    bool bPlusPressed;
    bool bMinusPressed;
    uint32_t input;
} kbd_state_t;

kbd_state_t *get_kbd_state(void);

// MOS2 keyboard API compatibility stubs
typedef bool (*scancode_handler_t)(const uint32_t);
typedef bool (*cp866_handler_t)(const char, uint32_t);
void set_scancode_handler(scancode_handler_t h);
void set_cp866_handler(cp866_handler_t h);
scancode_handler_t get_scancode_handler(void);
cp866_handler_t get_cp866_handler(void);
void kbd_set_stdin_owner(long pid);

// MOS2 stdin waiter notification (keyboard.c)
void kbd_add_stdin_waiter(void *th);
void kbd_remove_stdin_waiter(void *th);

// MOS2 console I/O (mos2_stubs.c — reads __c set by scancode handler)
char __getch(void);
int __getc(FIL*);
char getch_now(void);

// MOS2 LED status (stub — no PS/2 LEDs on FRANK OS)
uint8_t get_leds_stat(void);

#ifdef __cplusplus
}
#endif

#endif // KEYBOARD_H
