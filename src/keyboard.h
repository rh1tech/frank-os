#ifndef KEYBOARD_H
#define KEYBOARD_H

#include <stdint.h>
#include <stdbool.h>

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

#ifdef __cplusplus
}
#endif

#endif // KEYBOARD_H
