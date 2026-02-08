/*
 * PS/2 Keyboard Scancode Decoder
 *
 * Reads raw PS/2 scancode set 2 bytes from the unified ps2 driver,
 * converts them to HID key codes, and produces key events with ASCII.
 *
 * Ported from ps2kbd_mrmltr (C++) to plain C.
 */

#include "keyboard.h"
#include "ps2.h"
#include <string.h>

//=============================================================================
// HID Key Codes (subset used by the scancode tables)
//=============================================================================

#define HID_KEY_NONE           0x00
#define HID_KEY_A              0x04
#define HID_KEY_Z              0x1D
#define HID_KEY_1              0x1E
#define HID_KEY_0              0x27
#define HID_KEY_ENTER          0x28
#define HID_KEY_ESCAPE         0x29
#define HID_KEY_BACKSPACE      0x2A
#define HID_KEY_TAB            0x2B
#define HID_KEY_SPACE          0x2C
#define HID_KEY_MINUS          0x2D
#define HID_KEY_EQUAL          0x2E
#define HID_KEY_BRACKET_LEFT   0x2F
#define HID_KEY_BRACKET_RIGHT  0x30
#define HID_KEY_BACKSLASH      0x31
#define HID_KEY_EUROPE_1       0x32
#define HID_KEY_SEMICOLON      0x33
#define HID_KEY_APOSTROPHE     0x34
#define HID_KEY_GRAVE          0x35
#define HID_KEY_COMMA          0x36
#define HID_KEY_PERIOD         0x37
#define HID_KEY_SLASH          0x38
#define HID_KEY_CAPS_LOCK      0x39
#define HID_KEY_F1             0x3A
#define HID_KEY_F2             0x3B
#define HID_KEY_F3             0x3C
#define HID_KEY_F4             0x3D
#define HID_KEY_F5             0x3E
#define HID_KEY_F6             0x3F
#define HID_KEY_F7             0x40
#define HID_KEY_F8             0x41
#define HID_KEY_F9             0x42
#define HID_KEY_F10            0x43
#define HID_KEY_F11            0x44
#define HID_KEY_F12            0x45
#define HID_KEY_PRINT_SCREEN   0x46
#define HID_KEY_SCROLL_LOCK    0x47
#define HID_KEY_PAUSE          0x48
#define HID_KEY_INSERT         0x49
#define HID_KEY_HOME           0x4A
#define HID_KEY_PAGE_UP        0x4B
#define HID_KEY_DELETE         0x4C
#define HID_KEY_END            0x4D
#define HID_KEY_PAGE_DOWN      0x4E
#define HID_KEY_ARROW_RIGHT    0x4F
#define HID_KEY_ARROW_LEFT     0x50
#define HID_KEY_ARROW_DOWN     0x51
#define HID_KEY_ARROW_UP       0x52
#define HID_KEY_NUM_LOCK       0x53
#define HID_KEY_KEYPAD_DIVIDE  0x54
#define HID_KEY_KEYPAD_MULTIPLY 0x55
#define HID_KEY_KEYPAD_SUBTRACT 0x56
#define HID_KEY_KEYPAD_ADD     0x57
#define HID_KEY_KEYPAD_ENTER   0x58
#define HID_KEY_KEYPAD_1       0x59
#define HID_KEY_KEYPAD_2       0x5A
#define HID_KEY_KEYPAD_3       0x5B
#define HID_KEY_KEYPAD_4       0x5C
#define HID_KEY_KEYPAD_5       0x5D
#define HID_KEY_KEYPAD_6       0x5E
#define HID_KEY_KEYPAD_7       0x5F
#define HID_KEY_KEYPAD_8       0x60
#define HID_KEY_KEYPAD_9       0x61
#define HID_KEY_KEYPAD_0       0x62
#define HID_KEY_KEYPAD_DECIMAL 0x63
#define HID_KEY_EUROPE_2       0x64
#define HID_KEY_KEYPAD_EQUAL   0x67

#define HID_KEY_CONTROL_LEFT   0xE0
#define HID_KEY_SHIFT_LEFT     0xE1
#define HID_KEY_ALT_LEFT       0xE2
#define HID_KEY_GUI_LEFT       0xE3
#define HID_KEY_CONTROL_RIGHT  0xE4
#define HID_KEY_SHIFT_RIGHT    0xE5
#define HID_KEY_ALT_RIGHT      0xE6
#define HID_KEY_GUI_RIGHT      0xE7

//=============================================================================
// PS/2 Scancode Set 2 → HID lookup tables
//=============================================================================

// Page 0: normal scancodes (0x00 - 0x83)
static const uint8_t sc2_page0[] = {
    /* 00 */ HID_KEY_NONE,
    /* 01 */ HID_KEY_F9,
    /* 02 */ 0x00,
    /* 03 */ HID_KEY_F5,
    /* 04 */ HID_KEY_F3,
    /* 05 */ HID_KEY_F1,
    /* 06 */ HID_KEY_F2,
    /* 07 */ HID_KEY_F12,
    /* 08 */ 0x68, // F13
    /* 09 */ HID_KEY_F10,
    /* 0A */ HID_KEY_F8,
    /* 0B */ HID_KEY_F6,
    /* 0C */ HID_KEY_F4,
    /* 0D */ HID_KEY_TAB,
    /* 0E */ HID_KEY_GRAVE,
    /* 0F */ HID_KEY_KEYPAD_EQUAL,
    /* 10 */ 0x69, // F14
    /* 11 */ HID_KEY_ALT_LEFT,
    /* 12 */ HID_KEY_SHIFT_LEFT,
    /* 13 */ 0x00,
    /* 14 */ HID_KEY_CONTROL_LEFT,
    /* 15 */ 0x14, // Q
    /* 16 */ HID_KEY_1,
    /* 17 */ 0x00,
    /* 18 */ 0x6A, // F15
    /* 19 */ 0x00,
    /* 1A */ 0x1D, // Z
    /* 1B */ 0x16, // S
    /* 1C */ 0x04, // A
    /* 1D */ 0x1A, // W
    /* 1E */ 0x1F, // 2
    /* 1F */ 0x00,
    /* 20 */ 0x6B, // F16
    /* 21 */ 0x06, // C
    /* 22 */ 0x1B, // X
    /* 23 */ 0x07, // D
    /* 24 */ 0x08, // E
    /* 25 */ 0x21, // 4
    /* 26 */ 0x20, // 3
    /* 27 */ 0x00,
    /* 28 */ 0x6C, // F17
    /* 29 */ HID_KEY_SPACE,
    /* 2A */ 0x19, // V
    /* 2B */ 0x09, // F
    /* 2C */ 0x17, // T
    /* 2D */ 0x15, // R
    /* 2E */ 0x22, // 5
    /* 2F */ 0x00,
    /* 30 */ 0x6D, // F18
    /* 31 */ 0x11, // N
    /* 32 */ 0x05, // B
    /* 33 */ 0x0B, // H
    /* 34 */ 0x0A, // G
    /* 35 */ 0x1C, // Y
    /* 36 */ 0x23, // 6
    /* 37 */ 0x00,
    /* 38 */ 0x6E, // F19
    /* 39 */ 0x00,
    /* 3A */ 0x10, // M
    /* 3B */ 0x0D, // J
    /* 3C */ 0x18, // U
    /* 3D */ 0x24, // 7
    /* 3E */ 0x25, // 8
    /* 3F */ 0x00,
    /* 40 */ 0x6F, // F20
    /* 41 */ HID_KEY_COMMA,
    /* 42 */ 0x0E, // K
    /* 43 */ 0x0C, // I
    /* 44 */ 0x12, // O
    /* 45 */ HID_KEY_0,
    /* 46 */ 0x26, // 9
    /* 47 */ 0x00,
    /* 48 */ 0x70, // F21
    /* 49 */ HID_KEY_PERIOD,
    /* 4A */ HID_KEY_SLASH,
    /* 4B */ 0x0F, // L
    /* 4C */ HID_KEY_SEMICOLON,
    /* 4D */ 0x13, // P
    /* 4E */ HID_KEY_MINUS,
    /* 4F */ 0x00,
    /* 50 */ 0x71, // F22
    /* 51 */ 0x00,
    /* 52 */ HID_KEY_APOSTROPHE,
    /* 53 */ 0x00,
    /* 54 */ HID_KEY_BRACKET_LEFT,
    /* 55 */ HID_KEY_EQUAL,
    /* 56 */ 0x00,
    /* 57 */ 0x72, // F23
    /* 58 */ HID_KEY_CAPS_LOCK,
    /* 59 */ HID_KEY_SHIFT_RIGHT,
    /* 5A */ HID_KEY_ENTER,
    /* 5B */ HID_KEY_BRACKET_RIGHT,
    /* 5C */ 0x00,
    /* 5D */ HID_KEY_EUROPE_1,
    /* 5E */ 0x00,
    /* 5F */ 0x73, // F24
    /* 60 */ 0x00,
    /* 61 */ HID_KEY_EUROPE_2,
    /* 62 */ 0x00,
    /* 63 */ 0x00,
    /* 64 */ 0x00,
    /* 65 */ 0x00,
    /* 66 */ HID_KEY_BACKSPACE,
    /* 67 */ 0x00,
    /* 68 */ 0x00,
    /* 69 */ HID_KEY_KEYPAD_1,
    /* 6A */ 0x00,
    /* 6B */ HID_KEY_KEYPAD_4,
    /* 6C */ HID_KEY_KEYPAD_7,
    /* 6D */ 0x00,
    /* 6E */ 0x00,
    /* 6F */ 0x00,
    /* 70 */ HID_KEY_KEYPAD_0,
    /* 71 */ HID_KEY_KEYPAD_DECIMAL,
    /* 72 */ HID_KEY_KEYPAD_2,
    /* 73 */ HID_KEY_KEYPAD_5,
    /* 74 */ HID_KEY_KEYPAD_6,
    /* 75 */ HID_KEY_KEYPAD_8,
    /* 76 */ HID_KEY_ESCAPE,
    /* 77 */ HID_KEY_NUM_LOCK,
    /* 78 */ HID_KEY_F11,
    /* 79 */ HID_KEY_KEYPAD_ADD,
    /* 7A */ HID_KEY_KEYPAD_3,
    /* 7B */ HID_KEY_KEYPAD_SUBTRACT,
    /* 7C */ HID_KEY_KEYPAD_MULTIPLY,
    /* 7D */ HID_KEY_KEYPAD_9,
    /* 7E */ HID_KEY_SCROLL_LOCK,
    /* 7F */ 0x00,
    /* 80 */ 0x00,
    /* 81 */ 0x00,
    /* 82 */ 0x00,
    /* 83 */ HID_KEY_F7,
};

// Page 1: extended scancodes (after 0xE0 prefix)
static uint8_t sc2_page1_lookup(uint8_t code) {
    switch (code) {
        case 0x11: return HID_KEY_ALT_RIGHT;
        case 0x14: return HID_KEY_CONTROL_RIGHT;
        case 0x1F: return HID_KEY_GUI_LEFT;
        case 0x27: return HID_KEY_GUI_RIGHT;
        case 0x4A: return HID_KEY_KEYPAD_DIVIDE;
        case 0x5A: return HID_KEY_KEYPAD_ENTER;
        case 0x69: return HID_KEY_END;
        case 0x6B: return HID_KEY_ARROW_LEFT;
        case 0x6C: return HID_KEY_HOME;
        case 0x70: return HID_KEY_INSERT;
        case 0x71: return HID_KEY_DELETE;
        case 0x72: return HID_KEY_ARROW_DOWN;
        case 0x74: return HID_KEY_ARROW_RIGHT;
        case 0x75: return HID_KEY_ARROW_UP;
        case 0x7A: return HID_KEY_PAGE_DOWN;
        case 0x7C: return HID_KEY_PRINT_SCREEN;
        case 0x7D: return HID_KEY_PAGE_UP;
        default:   return HID_KEY_NONE;
    }
}

//=============================================================================
// HID → ASCII
//=============================================================================

static uint8_t hid_to_ascii(uint8_t code, bool shift) {
    // Letters a-z (HID 0x04-0x1D)
    if (code >= 0x04 && code <= 0x1D) {
        uint8_t c = 'a' + (code - 0x04);
        return shift ? (c - 32) : c;
    }
    // Numbers 1-9, 0 (HID 0x1E-0x27)
    if (code >= 0x1E && code <= 0x27) {
        if (shift) {
            const char shifted[] = "!@#$%^&*()";
            return (uint8_t)shifted[code - 0x1E];
        }
        if (code == 0x27) return '0';
        return '1' + (code - 0x1E);
    }
    switch (code) {
        case 0x28: return '\n';
        case 0x29: return 0x1B; // ESC
        case 0x2A: return '\b';
        case 0x2B: return '\t';
        case 0x2C: return ' ';
        case 0x2D: return shift ? '_' : '-';
        case 0x2E: return shift ? '+' : '=';
        case 0x2F: return shift ? '{' : '[';
        case 0x30: return shift ? '}' : ']';
        case 0x31: return shift ? '|' : '\\';
        case 0x33: return shift ? ':' : ';';
        case 0x34: return shift ? '"' : '\'';
        case 0x35: return shift ? '~' : '`';
        case 0x36: return shift ? '<' : ',';
        case 0x37: return shift ? '>' : '.';
        case 0x38: return shift ? '?' : '/';
        // Arrow keys
        case 0x4F: return 0x80; // Right
        case 0x50: return 0x81; // Left
        case 0x51: return 0x82; // Down
        case 0x52: return 0x83; // Up
        // Function keys F1-F12
        case 0x3A: return 0xF1;
        case 0x3B: return 0xF2;
        case 0x3C: return 0xF3;
        case 0x3D: return 0xF4;
        case 0x3E: return 0xF5;
        case 0x3F: return 0xF6;
        case 0x40: return 0xF7;
        case 0x41: return 0xF8;
        case 0x42: return 0xF9;
        case 0x43: return 0xFA;
        case 0x44: return 0xFB;
        case 0x45: return 0xFC;
        default:   return 0;
    }
}

//=============================================================================
// HID code → modifier bit (or 0 if not a modifier)
//=============================================================================

static uint8_t hid_to_mod_bit(uint8_t hid) {
    switch (hid) {
        case HID_KEY_CONTROL_LEFT:  return KBD_MOD_LCTRL;
        case HID_KEY_SHIFT_LEFT:    return KBD_MOD_LSHIFT;
        case HID_KEY_ALT_LEFT:      return KBD_MOD_LALT;
        case HID_KEY_GUI_LEFT:      return KBD_MOD_LGUI;
        case HID_KEY_CONTROL_RIGHT: return KBD_MOD_RCTRL;
        case HID_KEY_SHIFT_RIGHT:   return KBD_MOD_RSHIFT;
        case HID_KEY_ALT_RIGHT:     return KBD_MOD_RALT;
        case HID_KEY_GUI_RIGHT:     return KBD_MOD_RGUI;
        default: return 0;
    }
}

//=============================================================================
// Key event ring buffer
//=============================================================================

#define KEY_QUEUE_SIZE 32

static key_event_t key_queue[KEY_QUEUE_SIZE];
static uint8_t kq_head = 0;
static uint8_t kq_tail = 0;

static void enqueue_event(uint8_t hid, bool pressed, uint8_t mods) {
    uint8_t next = (kq_head + 1) % KEY_QUEUE_SIZE;
    if (next == kq_tail) return; // full — drop
    bool shift = (mods & KBD_MOD_SHIFT) != 0;
    key_queue[kq_head] = (key_event_t){
        .hid_code  = hid,
        .ascii     = hid_to_ascii(hid, shift),
        .modifiers = mods,
        .pressed   = pressed,
    };
    kq_head = next;
}

//=============================================================================
// Scancode state machine
//=============================================================================

static uint8_t modifiers = 0;          // current modifier mask
static bool    sc_extended = false;     // saw 0xE0 prefix
static bool    sc_release  = false;     // saw 0xF0 break code

static void process_scancode(uint8_t byte) {
    switch (byte) {
        case 0xAA: // BAT OK / self-test passed — ignore
        case 0xFC: // self-test failed — ignore
        case 0xFE: // resend request — ignore
        case 0xFA: // ACK — ignore
        case 0xEE: // echo response — ignore
            return;

        case 0xE0: // extended prefix
            sc_extended = true;
            return;

        case 0xE1: // pause/break start — eat the entire multi-byte sequence
            // Pause key sends E1 14 77 E1 F0 14 F0 77. We just ignore it.
            // The bytes will be consumed one-at-a-time and fall through as
            // unknown scancodes which produce HID_KEY_NONE → dropped.
            return;

        case 0xF0: // break (release) prefix
            sc_release = true;
            return;

        default:
            break;
    }

    // Translate scancode → HID key code
    uint8_t hid;
    if (sc_extended) {
        hid = sc2_page1_lookup(byte);
    } else {
        hid = (byte < sizeof(sc2_page0)) ? sc2_page0[byte] : HID_KEY_NONE;
    }

    bool is_release = sc_release;

    // Reset state for next scancode
    sc_extended = false;
    sc_release  = false;

    if (hid == HID_KEY_NONE) return;

    // Update modifier tracking
    uint8_t mod_bit = hid_to_mod_bit(hid);
    if (mod_bit) {
        if (is_release)
            modifiers &= ~mod_bit;
        else
            modifiers |= mod_bit;
    }

    enqueue_event(hid, !is_release, modifiers);
}

//=============================================================================
// Public API
//=============================================================================

void keyboard_poll(void) {
    int byte;
    int limit = 32; // don't spin forever
    while (limit-- > 0 && (byte = ps2_kbd_get_byte()) >= 0) {
        process_scancode((uint8_t)byte);
    }
}

bool keyboard_get_event(key_event_t *ev) {
    if (kq_head == kq_tail) return false;
    *ev = key_queue[kq_tail];
    kq_tail = (kq_tail + 1) % KEY_QUEUE_SIZE;
    return true;
}
