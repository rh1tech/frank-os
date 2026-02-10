/*
 * PS/2 Keyboard Scancode Decoder
 *
 * Reads raw PS/2 scancode set 2 bytes from the unified ps2 driver,
 * converts them to HID key codes, and produces key events with ASCII.
 *
 * Ported from ps2kbd_mrmltr (C++) to plain C.
 */

#include "keyboard.h"
#include "terminal.h"
#include "ps2.h"
#include "FreeRTOS.h"
#include "task.h"
#include <string.h>
#include <stdio.h>

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

static void mos2_feed_scancode(uint8_t byte); /* defined below */

void keyboard_poll(void) {
    int byte;
    int limit = 32; // don't spin forever
    while (limit-- > 0 && (byte = ps2_kbd_get_byte()) >= 0) {
        /* Feed to MOS2 scancode handler (PS/2 set 2 → XT conversion) */
        mos2_feed_scancode((uint8_t)byte);
        /* Feed to FRANK OS windowing system (PS/2 set 2 → HID) */
        process_scancode((uint8_t)byte);
    }
}

bool keyboard_get_event(key_event_t *ev) {
    if (kq_head == kq_tail) return false;
    *ev = key_queue[kq_tail];
    kq_tail = (kq_tail + 1) % KEY_QUEUE_SIZE;
    return true;
}

/*==========================================================================
 * MOS2 keyboard API — scancode handler + stdin waiter notification
 *
 * MOS2 apps install a custom scancode handler via set_scancode_handler().
 * They then call kbd_add_stdin_waiter() and block on ulTaskNotifyTake().
 * When a key arrives, the handler processes the XT scancode, sets __c,
 * and we notify all waiting tasks via xTaskNotifyGive().
 *=========================================================================*/

/* Per-terminal MOS2 handler/waiter operations.
 * All state is stored in the calling task's terminal_t (via TLS). */

void set_scancode_handler(scancode_handler_t h) {
    terminal_t *t = terminal_get_active();
    if (t) t->mos2_scancode_handler = h;
    printf("[set_scancode_handler] %p -> term %p\n", h, t);
}
void set_cp866_handler(cp866_handler_t h) {
    terminal_t *t = terminal_get_active();
    if (t) t->mos2_cp866_handler = h;
}
scancode_handler_t get_scancode_handler(void) {
    terminal_t *t = terminal_get_active();
    scancode_handler_t h = t ? t->mos2_scancode_handler : NULL;
    printf("[get_scancode_handler] -> %p (term %p)\n", h, t);
    return h;
}
cp866_handler_t get_cp866_handler(void) {
    terminal_t *t = terminal_get_active();
    return t ? t->mos2_cp866_handler : NULL;
}
void kbd_set_stdin_owner(long pid) { (void)pid; }

/* Stdin waiters — per-terminal, stored in terminal_t */
void kbd_add_stdin_waiter(void *th) {
    terminal_t *t = terminal_get_active();
    if (!t) return;
    TaskHandle_t h = (TaskHandle_t)th;
    for (int i = 0; i < t->mos2_num_stdin_waiters; i++) {
        if (t->mos2_stdin_waiters[i] == h) return; /* already registered */
    }
    if (t->mos2_num_stdin_waiters < MAX_STDIN_WAITERS) {
        t->mos2_stdin_waiters[t->mos2_num_stdin_waiters++] = h;
    }
}

void kbd_remove_stdin_waiter(void *th) {
    terminal_t *t = terminal_get_active();
    if (!t) return;
    TaskHandle_t h = (TaskHandle_t)th;
    for (int i = 0; i < t->mos2_num_stdin_waiters; i++) {
        if (t->mos2_stdin_waiters[i] == h) {
            t->mos2_stdin_waiters[i] = t->mos2_stdin_waiters[--t->mos2_num_stdin_waiters];
            return;
        }
    }
}

/*==========================================================================
 * MOS2 default scancode handler
 *
 * This is a port of MOS2's handleScancode().  It converts XT scancodes
 * to CP866 characters, sets the global __c, and notifies stdin waiters.
 * The app's custom scancode_handler is called at the end, after the
 * default processing — matching MOS2's architecture.
 *=========================================================================*/

/* __c is now per-terminal (terminal_t.mos2_c), but we keep this extern
 * for backward compatibility — it aliases the focused terminal's mos2_c.
 * Defined in mos2_stubs.c. */
extern volatile int __c;

/* MOS2 character codes used by apps (mc checks for these) */
#define CHAR_CODE_BS    8
#define CHAR_CODE_UP    17
#define CHAR_CODE_DOWN  18
#define CHAR_CODE_ENTER '\n'
#define CHAR_CODE_TAB   '\t'
#define CHAR_CODE_ESC   0x1B

#define PS2_LED_NUM_LOCK 0x02

/* MOS2 keyboard state */
static kbd_state_t ks = { 0 };

kbd_state_t *get_kbd_state(void) { return &ks; }

/*--- XT scancode → CP866 lookup tables (86 entries each) ---*/

static const char scan_code_2_cp866_a[] = {
     0 ,  0 , '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', '-', '=',  0 ,'\t',
    'q', 'w', 'e', 'r', 't', 'y', 'u', 'i', 'o', 'p', '[', ']','\n',  0 , 'a', 's',
    'd', 'f', 'g', 'h', 'j', 'k', 'l', ';','\'', '`',  0 ,'\\', 'z', 'x', 'c', 'v',
    'b', 'n', 'm', ',', '.', '/',  0 , '*',  0 , ' ',  0 ,  0 ,  0 ,  0 ,  0 ,  0 ,
     0 ,  0 ,  0 ,  0 ,  0 ,  0 ,  0 , '7', '8', '9', '-', '4', '5', '6', '+', '1',
    '2', '3', '0', '.', '/',  0
};
static const char scan_code_2_cp866_A[] = {
     0 ,  0 , '!', '@', '#', '$', '%', '^', '&', '*', '(', ')', '_', '+',  0 ,'\t',
    'Q', 'W', 'E', 'R', 'T', 'Y', 'U', 'I', 'O', 'P', '{', '}','\n',  0 , 'A', 'S',
    'D', 'F', 'G', 'H', 'J', 'K', 'L', ':', '"', '~',  0 , '|', 'Z', 'X', 'C', 'V',
    'B', 'N', 'M', '<', '>', '?',  0 , '*',  0 , ' ',  0 ,  0 ,  0 ,  0 ,  0 ,  0 ,
     0 ,  0 ,  0 ,  0 ,  0 ,  0 ,  0 , '7', '8', '9', '-', '4', '5', '6', '+', '1',
    '2', '3', '0', '.', '/',  0
};
static const char scan_code_2_cp866_aCL[] = {
     0 ,  0 , '!', '@', '#', '$', '%', '^', '&', '*', '(', ')', '_', '+',  0 ,'\t',
    'q', 'w', 'e', 'r', 't', 'y', 'u', 'i', 'o', 'p', '[', ']','\n',  0 , 'a', 's',
    'd', 'f', 'g', 'h', 'j', 'k', 'l', ':', '"', '~',  0 , '|', 'z', 'x', 'c', 'v',
    'b', 'n', 'm', '<', '>', '?',  0 , '*',  0 , ' ',  0 ,  0 ,  0 ,  0 ,  0 ,  0 ,
     0 ,  0 ,  0 ,  0 ,  0 ,  0 ,  0 , '7', '8', '9', '-', '4', '5', '6', '+', '1',
    '2', '3', '0', '.', '/',  0
};
static const char scan_code_2_cp866_ACL[] = {
     0 ,  0 , '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', '-', '=',  0 ,'\t',
    'Q', 'W', 'E', 'R', 'T', 'Y', 'U', 'I', 'O', 'P', '{', '}','\n',  0 , 'A', 'S',
    'D', 'F', 'G', 'H', 'J', 'K', 'L', ';','\'', '`',  0 ,'\\', 'Z', 'X', 'C', 'V',
    'B', 'N', 'M', ',', '.', '/',  0 , '*',  0 , ' ',  0 ,  0 ,  0 ,  0 ,  0 ,  0 ,
     0 ,  0 ,  0 ,  0 ,  0 ,  0 ,  0 , '7', '8', '9', '-', '4', '5', '6', '+', '1',
    '2', '3', '0', '.', '/',  0
};
static const char scan_code_2_cp866_ra[] = {
     0 ,  0 , '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', '-', '=',  0 ,'\t',
   0xA9,0xE6,0xE3,0xAA,0xA5,0xAD,0xA3,0xE8,0xE9,0xA7,0xE5,0xEA,'\n',  0 ,0xE4,0xEB,
   0xA2,0xA0,0xAF,0xE0,0xAE,0xAB,0xA4,0xA6,0xED,0xF1,  0 ,'\\',0xEF,0xE7,0xE1,0xAC,
   0xA8,0xE2,0xEC,0xA1,0xEE, ',',  0 , '*',  0 , ' ',  0 ,  0 ,  0 ,  0 ,  0 ,  0 ,
     0 ,  0 ,  0 ,  0 ,  0 ,  0 ,  0 , '7', '8', '9', '-', '4', '5', '6', '+', '1',
    '2', '3', '0', '.', '/',  0
};
static const char scan_code_2_cp866_rA[] = {
     0 ,  0 , '!', '"',0xFC, ';', '%', ':', '?', '*', '(', ')', '_', '+',  0 ,'\t',
   0x89,0x96,0x93,0x8A,0x85,0x8D,0x83,0x98,0x99,0x87,0x95,0x9A,'\n',  0 ,0x94,0x9B,
   0x82,0x80,0x8F,0x90,0x8E,0x8B,0x84,0x86,0x9D,0xF0,  0 , '/',0x9F,0x97,0x91,0x8C,
   0x88,0x92,0x9C,0x81,0x9E, '.',  0 , '*',  0 , ' ',  0 ,  0 ,  0 ,  0 ,  0 ,  0 ,
     0 ,  0 ,  0 ,  0 ,  0 ,  0 ,  0 , '7', '8', '9', '-', '4', '5', '6', '+', '1',
    '2', '3', '0', '.', '/',  0
};
static const char scan_code_2_cp866_raCL[] = {
     0 ,  0 , '!', '"',0xFC, ';', '%', ':', '?', '*', '(', ')', '_', '+',  0 ,'\t',
   0xA9,0xE6,0xE3,0xAA,0xA5,0xAD,0xA3,0xE8,0xE9,0xA7,0xE5,0xEA,'\n',  0 ,0xE4,0xEB,
   0xA2,0xA0,0xAF,0xE0,0xAE,0xAB,0xA4,0xA6,0xED,0xF1,  0 , '/',0xEF,0xE7,0xE1,0xAC,
   0xA8,0xE2,0xEC,0xA1,0xEE, ',',  0 , '*',  0 , ' ',  0 ,  0 ,  0 ,  0 ,  0 ,  0 ,
     0 ,  0 ,  0 ,  0 ,  0 ,  0 ,  0 , '7', '8', '9', '-', '4', '5', '6', '+', '1',
    '2', '3', '0', '.', '/',  0
};
static const char scan_code_2_cp866_rACL[] = {
     0 ,  0 , '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', '-', '=',  0 ,'\t',
   0x89,0x96,0x93,0x8A,0x85,0x8D,0x83,0x98,0x99,0x87,0x95,0x9A,'\n',  0 ,0x94,0x9B,
   0x82,0x80,0x8F,0x90,0x8E,0x8B,0x84,0x86,0x9D,0xF0,  0 ,'\\',0x9F,0x97,0x91,0x8C,
   0x88,0x92,0x9C,0x81,0x9E, '.',  0 , '*',  0 , ' ',  0 ,  0 ,  0 ,  0 ,  0 ,  0 ,
     0 ,  0 ,  0 ,  0 ,  0 ,  0 ,  0 , '7', '8', '9', '-', '4', '5', '6', '+', '1',
    '2', '3', '0', '.', '/',  0
};

/* Alt+numpad decimal entry — convert "123" → char(123) */
static char tricode2c(char tricode[4], size_t s) {
    unsigned int r = 0;
    for (int pos = s - 1; pos >= 0; pos--) {
        int bs = s - 1 - pos;
        unsigned int dShift = bs == 2 ? 100 : (bs == 1 ? 10 : 1);
        r += (tricode[pos] - '0') * dShift;
    }
    tricode[0] = 0;
    return (char)(r & 0xFF);
}

/*
 * Default MOS2 scancode handler — ported from MOS2 keyboard.c handleScancode().
 *
 * Takes an XT scancode, tracks modifier state, converts to CP866 character,
 * sets __c, notifies stdin waiters, then calls the app's custom scancode_handler.
 */
static void default_mos2_handler(uint32_t ps2scancode) {
    static char tricode[4] = {0};
    size_t s;
    char c = 0;

    /* Suppress MOS2 character output when Win (GUI) key is held.
     * Win+key combos are handled by input_task and should not leak
     * characters into MOS2 apps. */
    if (modifiers & (KBD_MOD_LGUI | KBD_MOD_RGUI))
        return;

    /* Find the focused terminal to route MOS2 input to */
    hwnd_t focus = wm_get_focus();
    terminal_t *ft = (focus != HWND_NULL) ? terminal_from_hwnd(focus) : NULL;

    ks.input = ps2scancode;

    /* Arrow keys (when NumLock is off) → character codes */
    if ((ps2scancode == 0xE048 || ps2scancode == 0x48) &&
        !(get_leds_stat() & PS2_LED_NUM_LOCK)) {
        ks.input = 0;
        if (ft && ft->mos2_cp866_handler) ft->mos2_cp866_handler(CHAR_CODE_UP, ps2scancode);
        if (ft) { ft->mos2_c = CHAR_CODE_UP; terminal_notify_stdin_ready(ft); }
        __c = CHAR_CODE_UP;
        goto ex;
    }
    if ((ps2scancode == 0xE050 || ps2scancode == 0x50) &&
        !(get_leds_stat() & PS2_LED_NUM_LOCK)) {
        ks.input = 0;
        if (ft && ft->mos2_cp866_handler) ft->mos2_cp866_handler(CHAR_CODE_DOWN, ps2scancode);
        if (ft) { ft->mos2_c = CHAR_CODE_DOWN; terminal_notify_stdin_ready(ft); }
        __c = CHAR_CODE_DOWN;
        goto ex;
    }

    /* Numpad Enter */
    if (ps2scancode == 0xE09C) {
        if (ft && ft->mos2_cp866_handler) ft->mos2_cp866_handler(CHAR_CODE_ENTER, ps2scancode);
        if (ft) { ft->mos2_c = CHAR_CODE_ENTER; terminal_notify_stdin_ready(ft); }
        __c = CHAR_CODE_ENTER;
        goto ex;
    }

    switch ((uint8_t)(ks.input & 0xFF)) {
    case 0x81: /* ESC release */
        if (ft && ft->mos2_cp866_handler) ft->mos2_cp866_handler(CHAR_CODE_ESC, ps2scancode);
        if (ft) { ft->mos2_c = CHAR_CODE_ESC; terminal_notify_stdin_ready(ft); }
        __c = CHAR_CODE_ESC;
        goto ex;
    case 0x1D: /* Ctrl press */
        ks.bCtrlPressed = true;
        if (ks.bRightShift || ks.bLeftShift) ks.bRus = !ks.bRus;
        break;
    case 0x9D: /* Ctrl release */
        ks.bCtrlPressed = false;
        break;
    case 0x38: /* Alt press */
        ks.bAltPressed = true;
        break;
    case 0xB8: /* Alt release — finalize tricode if any */
        ks.bAltPressed = false;
        s = strlen(tricode);
        if (s) {
            c = tricode2c(tricode, s);
            if (ft) { ft->mos2_c = c; terminal_notify_stdin_ready(ft); }
            __c = c;
            goto ex;
        }
        break;
    case 0x53: /* Del press */
        ks.bDelPressed = true;
        break;
    case 0xD3: /* Del release */
        ks.bDelPressed = false;
        break;
    case 0x2A: /* Left Shift press */
        ks.bLeftShift = true;
        if (ks.bCtrlPressed) ks.bRus = !ks.bRus;
        break;
    case 0xAA: /* Left Shift release */
        ks.bLeftShift = false;
        break;
    case 0x36: /* Right Shift press */
        ks.bRightShift = true;
        if (ks.bCtrlPressed) ks.bRus = !ks.bRus;
        break;
    case 0xB6: /* Right Shift release */
        ks.bRightShift = false;
        break;
    case 0x3A: /* CapsLock toggle */
        ks.bCapsLock = !ks.bCapsLock;
        break;
    case 0x0E: /* Backspace */
        if (ft && ft->mos2_cp866_handler) ft->mos2_cp866_handler(CHAR_CODE_BS, ps2scancode);
        if (ft) { ft->mos2_c = CHAR_CODE_BS; terminal_notify_stdin_ready(ft); }
        __c = CHAR_CODE_BS;
        goto ex;
    case 0x0F: /* Tab press */
        ks.bTabPressed = true;
        break;
    case 0x8F: /* Tab release */
        ks.bTabPressed = false;
        break;
    case 0x0C: /* - press */
    case 0x4A: /* numpad - press */
        ks.bMinusPressed = true;
        break;
    case 0x8C: /* - release */
    case 0xCA: /* numpad - release */
        ks.bMinusPressed = false;
        break;
    case 0x0D: /* += press */
    case 0x4E: /* numpad + press */
        ks.bPlusPressed = true;
        break;
    case 0x8D: /* += release */
    case 0xCE: /* numpad + release */
        ks.bPlusPressed = false;
        break;
    default:
        break;
    }

    /* Character lookup — only for make codes with index < 86 */
    if (c || ks.input < 86) {
        if (!c) {
            const char *table;
            if (ks.bRus) {
                if (!ks.bCapsLock)
                    table = (ks.bRightShift || ks.bLeftShift) ? scan_code_2_cp866_rA : scan_code_2_cp866_ra;
                else
                    table = (ks.bRightShift || ks.bLeftShift) ? scan_code_2_cp866_raCL : scan_code_2_cp866_rACL;
            } else {
                if (!ks.bCapsLock)
                    table = (ks.bRightShift || ks.bLeftShift) ? scan_code_2_cp866_A : scan_code_2_cp866_a;
                else
                    table = (ks.bRightShift || ks.bLeftShift) ? scan_code_2_cp866_aCL : scan_code_2_cp866_ACL;
            }
            c = table[ks.input];
        }
        if (c) {
            /* Alt+numpad decimal entry */
            if (ks.bAltPressed && c >= '0' && c <= '9') {
                s = strlen(tricode);
                if (s == 3) {
                    c = tricode2c(tricode, s);
                } else {
                    tricode[s++] = c;
                    tricode[s] = 0;
                    goto ex;
                }
            }
        }
        if (ft && ft->mos2_cp866_handler) ft->mos2_cp866_handler(c, ps2scancode);
        if (ks.bCtrlPressed && ps2scancode == 0x2E) {
            /* Ctrl+C → EOF */
            if (ft) ft->mos2_c = -1;
            __c = -1;
        } else {
            if (ft) ft->mos2_c = c;
            __c = c;
        }
        if (ft) terminal_notify_stdin_ready(ft);
    }

ex:
    if (ft && ft->mos2_scancode_handler) {
        ft->mos2_scancode_handler(ps2scancode);
    }
}

/*==========================================================================
 * PS/2 Set 2 → XT (Set 1) scancode conversion
 *
 * MOS2 scancode handlers expect XT-format scancodes.
 * XT make codes are single bytes; release = make | 0x80.
 * Extended keys (E0 prefix) are passed as 0xE0XX.
 *=========================================================================*/

/* PS/2 Set 2 make code → XT Set 1 make code (indices 0x00–0x83) */
static const uint8_t ps2_to_xt[] = {
    0xFF, 0x43, 0x00, 0x3F, 0x3D, 0x3B, 0x3C, 0x58, /* 00-07 */
    0x64, 0x44, 0x42, 0x40, 0x3E, 0x0F, 0x29, 0x00, /* 08-0F */
    0x65, 0x38, 0x2A, 0x00, 0x1D, 0x10, 0x02, 0x00, /* 10-17 */
    0x66, 0x00, 0x2C, 0x1F, 0x1E, 0x11, 0x03, 0x00, /* 18-1F */
    0x67, 0x2E, 0x2D, 0x20, 0x12, 0x05, 0x04, 0x00, /* 20-27 */
    0x68, 0x39, 0x2F, 0x21, 0x14, 0x13, 0x06, 0x00, /* 28-2F */
    0x69, 0x31, 0x30, 0x23, 0x22, 0x15, 0x07, 0x00, /* 30-37 */
    0x6A, 0x00, 0x32, 0x24, 0x16, 0x08, 0x09, 0x00, /* 38-3F */
    0x6B, 0x33, 0x25, 0x17, 0x18, 0x0B, 0x0A, 0x00, /* 40-47 */
    0x6C, 0x34, 0x35, 0x26, 0x27, 0x19, 0x0C, 0x00, /* 48-4F */
    0x6D, 0x00, 0x28, 0x00, 0x1A, 0x0D, 0x00, 0x00, /* 50-57 */
    0x3A, 0x36, 0x1C, 0x1B, 0x00, 0x2B, 0x00, 0x00, /* 58-5F */
    0x00, 0x56, 0x00, 0x00, 0x00, 0x00, 0x0E, 0x00, /* 60-67 */
    0x00, 0x4F, 0x00, 0x4B, 0x47, 0x00, 0x00, 0x00, /* 68-6F */
    0x52, 0x53, 0x50, 0x4C, 0x4D, 0x48, 0x01, 0x45, /* 70-77 */
    0x57, 0x4E, 0x51, 0x4A, 0x37, 0x49, 0x46, 0x00, /* 78-7F */
    0x00, 0x00, 0x00, 0x41,                          /* 80-83 */
};

/* Extended PS/2 Set 2 (after E0) → XT extended code */
static uint8_t ps2_ext_to_xt(uint8_t code) {
    switch (code) {
        case 0x11: return 0x38; /* Right Alt */
        case 0x14: return 0x1D; /* Right Ctrl */
        case 0x1F: return 0x5B; /* Left GUI */
        case 0x27: return 0x5C; /* Right GUI */
        case 0x4A: return 0x35; /* Keypad / */
        case 0x5A: return 0x1C; /* Keypad Enter */
        case 0x69: return 0x4F; /* End */
        case 0x6B: return 0x4B; /* Left Arrow */
        case 0x6C: return 0x47; /* Home */
        case 0x70: return 0x52; /* Insert */
        case 0x71: return 0x53; /* Delete */
        case 0x72: return 0x50; /* Down Arrow */
        case 0x74: return 0x4D; /* Right Arrow */
        case 0x75: return 0x48; /* Up Arrow */
        case 0x7A: return 0x51; /* Page Down */
        case 0x7D: return 0x49; /* Page Up */
        default:   return 0x00;
    }
}

/* State machine for PS/2 → XT conversion (separate from the HID state machine) */
static bool xt_extended = false;
static bool xt_release  = false;

static void mos2_feed_scancode(uint8_t byte) {
    /* Skip protocol bytes */
    if (byte == 0xAA || byte == 0xFC || byte == 0xFE ||
        byte == 0xFA || byte == 0xEE)
        return;

    if (byte == 0xE0) { xt_extended = true; return; }
    if (byte == 0xF0) { xt_release  = true; return; }
    if (byte == 0xE1) return; /* Pause key — ignore */

    /* Convert PS/2 set 2 → XT set 1 */
    uint32_t xt_code;
    if (xt_extended) {
        uint8_t xt = ps2_ext_to_xt(byte);
        if (xt == 0) { xt_extended = false; xt_release = false; return; }
        xt_code = 0xE000 | xt;
        if (xt_release) xt_code |= 0x80; /* release bit on low byte */
    } else {
        if (byte >= sizeof(ps2_to_xt)) { xt_release = false; return; }
        uint8_t xt = ps2_to_xt[byte];
        if (xt == 0 || xt == 0xFF) { xt_release = false; return; }
        xt_code = xt;
        if (xt_release) xt_code |= 0x80;
    }
    xt_extended = false;
    xt_release  = false;

    /* Run the default MOS2 handler (converts to char, sets __c,
     * then calls the app's custom scancode_handler at the end) */
    default_mos2_handler(xt_code);
}
