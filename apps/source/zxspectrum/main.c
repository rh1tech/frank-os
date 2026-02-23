/*
 * FRANK OS — ZX Spectrum 48K Emulator (standalone ELF app)
 *
 * Uses Fayzullin Z80 core with ARM Thumb-2 assembly dispatcher for
 * performance on RP2350. Replaces the chips z80.h per-T-state emulation.
 *
 * MEMORY MODEL:
 * FRANK OS ELF loader places .data and .bss in PSRAM, which does NOT
 * support writes via normal ARM store instructions on RP2350.  All mutable
 * state is therefore heap-allocated (pvPortMalloc returns SRAM) and accessed
 * through a pointer held in ARM register r9 (compiled with -ffixed-r9).
 * The paint callback runs on the compositor task (different r9), so it
 * retrieves state from window->user_data instead.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "m-os-api.h"
#include "frankos-app.h"

/*
 * m-os-api.h defines:
 *   #define switch DO_NOT_USE_SWITCH
 *   #define inline __force_inline
 * These break headers which use switch statements and inline functions.
 */
#undef switch
#undef inline
#undef __force_inline

/* Override assert for freestanding environment */
#define CHIPS_ASSERT(c) ((void)(c))

/* Fayzullin Z80 core — need Z80 type for app_globals_t */
#include "Z80.h"
#include "z80_arm.h"

/* ======================================================================
 * App globals struct — ALL mutable state lives here, heap-allocated.
 * Z80 MUST be the first member so z80_arm.S can use r9 directly as
 * the Z80 struct pointer.
 * ====================================================================== */

/* Forward-declare zx_t — full definition comes from zx.h later */
struct zx_t_fwd;

typedef struct {
    Z80 cpu;                        /*  0: Z80 CPU state (MUST BE FIRST) */
    uint8_t *rom;                   /* ROM pointer (const image, no copy) */
    uint8_t *ram[3];                /* RAM bank pointers (16KB each) */
    struct zx_t_fwd *zx;           /* Emulator state (zx_t, heap-allocated) */
    volatile bool vram_dirty;       /* VRAM dirty flag */
    void *app_task;                 /* FreeRTOS task handle */
    int32_t app_hwnd;               /* Window handle (hwnd_t) */
    volatile bool closing;          /* Close requested */
    bool tap_loaded;                /* TAP file loaded */
    uint8_t prev_modifiers;         /* Previous keyboard modifiers */
    FIL *tap_file;                  /* FatFS file handle (heap-allocated) */
    volatile bool reset_requested;  /* Deferred reset from WM task */
    volatile bool tap_path_ready;   /* Deferred TAP load from WM task */
    char tap_path[256];             /* Path for deferred TAP load */
} app_globals_t;

/* Register r9 holds the app_globals_t pointer.  Compiled with -ffixed-r9
 * so GCC never uses r9 for anything else.  FreeRTOS saves/restores r9
 * on context switch, making it effectively task-local. */
register app_globals_t *G asm("r9");

/* Macros so that zx.h inline functions (included below with CHIPS_IMPL)
 * can use zx_cpu / ZX_ROM / ZX_RAM transparently. */
#define zx_cpu   (G->cpu)
#define ZX_ROM   (G->rom)
#define ZX_RAM   (G->ram)

/* Pull in the emulator implementation.
 * Only need: chips_common (types) → kbd → clk → zx
 * Z80 CPU is now in Z80.c + z80_arm.S (separate compilation units) */
#define CHIPS_IMPL
#pragma GCC optimize("O2")
#include "chips_common.h"
#include "kbd.h"
#include "clk.h"
#include "zx.h"
#pragma GCC optimize("Oz")
#include "zx-roms.h"
#pragma GCC optimize("O2")

/* Now that zx_t is fully defined, cast the forward pointer */
#define ZX   ((zx_t *)G->zx)

/* ======================================================================
 * Fayzullin Z80 callbacks
 * ====================================================================== */

/* Tape trap address in ROM (LD-BYTES entry point) */
#define ZX_TAPE_TRAP_ADDR 0x0556

/* Memory read callback — bank-select for non-contiguous RAM */
byte RdZ80(word Addr) {
    if (Addr < 0x4000) {
        if (Addr == ZX_TAPE_TRAP_ADDR) return 0x76; /* HALT for tape trap */
        return G->rom[Addr];
    }
    uint16_t ra = Addr - 0x4000;
    return G->ram[ra >> 14][ra & 0x3FFF];
}

/* Memory write callback — bank-select for non-contiguous RAM */
void WrZ80(word Addr, byte Value) {
    if (Addr < 0x4000) return; /* ROM — ignore writes */
    uint16_t ra = Addr - 0x4000;
    uint8_t *bank = G->ram[ra >> 14];
    uint16_t off = ra & 0x3FFF;
    if (ra < 0x1B00) {
        if (bank[off] != Value)
            G->vram_dirty = true;
    }
    bank[off] = Value;
}

/* I/O read callback */
byte InZ80(word Port) {
    zx_t *sys = ZX;
    if ((Port & 1) == 0) {
        /* Spectrum ULA (...............0) */
        uint8_t data = (1<<7)|(1<<5);
        if (sys->last_fe_out & (1<<3|1<<4)) {
            data |= (1<<6);
        }
        uint16_t column_mask = (~(Port >> 8)) & 0x00FF;
        const uint16_t kbd_lines = kbd_test_lines(&sys->kbd, column_mask);
        data |= (~kbd_lines) & 0x1F;
        return data;
    }
    if ((Port & 0xE0) == 0) {
        /* Kempston Joystick (........000.....) */
        return sys->kbd_joymask | sys->joy_joymask;
    }
    return 0xFF;
}

/* I/O write callback */
void OutZ80(word Port, byte Value) {
    zx_t *sys = ZX;
    if ((Port & 1) == 0) {
        /* Spectrum ULA */
        sys->border_color = Value & 7;
        sys->last_fe_out = Value;
        sys->beeper_state = 0 != (Value & (1<<4));
    }
}

/* Periodic interrupt check — unused, we manage interrupts externally */
word LoopZ80(Z80 *R) {
    (void)R;
    return INT_NONE;
}

/* ED FE patch — no-op */
void PatchZ80(Z80 *R) {
    (void)R;
}

/* Menu command IDs */
#define CMD_LOAD_TAP  1
#define CMD_RESET     2
#define CMD_EXIT      3

static void handle_tape_trap(void *ud);
static void load_tap_file(const char *path);

/* Border: 32 px left/right, 24 px top/bottom → 320×240 client area */
#define BORDER_H  32
#define BORDER_V  24
#define CLIENT_W  (256 + 2 * BORDER_H)   /* 320 */
#define CLIENT_H  (192 + 2 * BORDER_V)   /* 240 */

/* ======================================================================
 * 16×16 icon — diagonal rainbow stripes in ZX Spectrum bright colours
 *
 * 2-pixel-wide / stripes cycling through:
 *   Classic ZX Spectrum 4-colour rainbow: Red, Yellow, Green, Cyan
 *   3-pixel-wide diagonal (/) stripes on black background.
 *   d = row + col;  black where d < 9 or d >= 21.
 * ====================================================================== */

static const uint8_t zx_icon_16x16[256] = {
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x0C, 0x0C, 0x0C, 0x0E, 0x0E, 0x0E, 0x0A,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x0C, 0x0C, 0x0C, 0x0E, 0x0E, 0x0E, 0x0A, 0x0A,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x0C, 0x0C, 0x0C, 0x0E, 0x0E, 0x0E, 0x0A, 0x0A, 0x0A,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x0C, 0x0C, 0x0C, 0x0E, 0x0E, 0x0E, 0x0A, 0x0A, 0x0A, 0x0B,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x0C, 0x0C, 0x0C, 0x0E, 0x0E, 0x0E, 0x0A, 0x0A, 0x0A, 0x0B, 0x0B,
    0x00, 0x00, 0x00, 0x00, 0x0C, 0x0C, 0x0C, 0x0E, 0x0E, 0x0E, 0x0A, 0x0A, 0x0A, 0x0B, 0x0B, 0x0B,
    0x00, 0x00, 0x00, 0x0C, 0x0C, 0x0C, 0x0E, 0x0E, 0x0E, 0x0A, 0x0A, 0x0A, 0x0B, 0x0B, 0x0B, 0x00,
    0x00, 0x00, 0x0C, 0x0C, 0x0C, 0x0E, 0x0E, 0x0E, 0x0A, 0x0A, 0x0A, 0x0B, 0x0B, 0x0B, 0x00, 0x00,
    0x00, 0x0C, 0x0C, 0x0C, 0x0E, 0x0E, 0x0E, 0x0A, 0x0A, 0x0A, 0x0B, 0x0B, 0x0B, 0x00, 0x00, 0x00,
    0x0C, 0x0C, 0x0C, 0x0E, 0x0E, 0x0E, 0x0A, 0x0A, 0x0A, 0x0B, 0x0B, 0x0B, 0x00, 0x00, 0x00, 0x00,
    0x0C, 0x0C, 0x0E, 0x0E, 0x0E, 0x0A, 0x0A, 0x0A, 0x0B, 0x0B, 0x0B, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x0C, 0x0E, 0x0E, 0x0E, 0x0A, 0x0A, 0x0A, 0x0B, 0x0B, 0x0B, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x0E, 0x0E, 0x0E, 0x0A, 0x0A, 0x0A, 0x0B, 0x0B, 0x0B, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x0E, 0x0E, 0x0A, 0x0A, 0x0A, 0x0B, 0x0B, 0x0B, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x0E, 0x0A, 0x0A, 0x0A, 0x0B, 0x0B, 0x0B, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x0A, 0x0A, 0x0A, 0x0B, 0x0B, 0x0B, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
};

/* ======================================================================
 * Colour mapping:  ZX 16-colour → CGA 16-colour
 * ====================================================================== */

static const uint8_t zx_to_cga[16] = {
    COLOR_BLACK,          /*  0  Black         */
    COLOR_BLUE,           /*  1  Blue          */
    COLOR_RED,            /*  2  Red           */
    COLOR_MAGENTA,        /*  3  Magenta       */
    COLOR_GREEN,          /*  4  Green         */
    COLOR_CYAN,           /*  5  Cyan          */
    COLOR_BROWN,          /*  6  Yellow (dim)  */
    COLOR_LIGHT_GRAY,     /*  7  White  (dim)  */
    COLOR_BLACK,          /*  8  Bright Black  */
    COLOR_LIGHT_BLUE,     /*  9  Bright Blue   */
    COLOR_LIGHT_RED,      /* 10  Bright Red    */
    COLOR_LIGHT_MAGENTA,  /* 11  Bright Magenta*/
    COLOR_LIGHT_GREEN,    /* 12  Bright Green  */
    COLOR_LIGHT_CYAN,     /* 13  Bright Cyan   */
    COLOR_YELLOW,         /* 14  Bright Yellow */
    COLOR_WHITE,          /* 15  Bright White  */
};

/* ======================================================================
 * Paint handler — 1× native resolution (256×192) + border
 *
 * Runs on the compositor task (different from main), so r9 does NOT
 * hold our app_globals_t pointer.  Retrieve it from window->user_data.
 * ====================================================================== */

static void zx_paint(hwnd_t hwnd) {
    window_t *win = wm_get_window(hwnd);
    if (!win || !win->user_data) return;
    app_globals_t *g = (app_globals_t *)win->user_data;
    zx_t *sys = (zx_t *)g->zx;

    wd_begin(hwnd);

    uint8_t border = zx_to_cga[sys->border_color & 7];

    /* Draw border — four rectangles around the 256×192 bitmap area */
    wd_fill_rect(0, 0, CLIENT_W, BORDER_V, border);                       /* top */
    wd_fill_rect(0, BORDER_V + 192, CLIENT_W, BORDER_V, border);          /* bottom */
    wd_fill_rect(0, BORDER_V, BORDER_H, 192, border);                     /* left */
    wd_fill_rect(BORDER_H + 256, BORDER_V, BORDER_H, 192, border);        /* right */

    /* Direct framebuffer access — bypass wd_hline overhead entirely.
     * Each ZX bitmap byte (8 mono pixels) expands to 4 framebuffer bytes
     * via a 4-entry LUT indexed by bit pairs. */
    int16_t stride;
    uint8_t *fb_base = wd_fb_ptr(BORDER_H, BORDER_V, &stride);
    uint8_t *vmem = sys->ram[0];
    bool flash_swap = (sys->blink_counter & 0x10) != 0;

    for (int y = 0; y < 192; y++) {
        int addr = ((y & 0xC0) << 5) | ((y & 0x07) << 8) | ((y & 0x38) << 2);
        int attr_row = 0x1800 + ((y >> 3) << 5);
        uint8_t *dst = fb_base + y * stride;

        for (int col = 0; col < 32; col++) {
            uint8_t byte = vmem[addr + col];
            uint8_t attr = vmem[attr_row + col];

            uint8_t ink   = attr & 0x07;
            uint8_t paper = (attr >> 3) & 0x07;
            uint8_t bright = (attr & 0x40) ? 8 : 0;
            if ((attr & 0x80) && flash_swap) {
                uint8_t tmp = ink; ink = paper; paper = tmp;
            }
            uint8_t fg = zx_to_cga[ink + bright];
            uint8_t bg = zx_to_cga[paper + bright];

            uint8_t lut[4] = {
                (bg << 4) | bg, (bg << 4) | fg,
                (fg << 4) | bg, (fg << 4) | fg
            };
            *dst++ = lut[(byte >> 6) & 3];
            *dst++ = lut[(byte >> 4) & 3];
            *dst++ = lut[(byte >> 2) & 3];
            *dst++ = lut[(byte >> 0) & 3];
        }
    }
    wd_end();
}

/* ======================================================================
 * Keyboard mapping:  HID scancode → ZX key code
 * ====================================================================== */

static int hid_to_zx(uint8_t scancode) {
    if (scancode >= 0x04 && scancode <= 0x1D)     /* a-z */
        return 'a' + (scancode - 0x04);
    if (scancode >= 0x1E && scancode <= 0x26)      /* 1-9 */
        return '1' + (scancode - 0x1E);
    if (scancode == 0x27) return '0';
    if (scancode == 0x28) return 0x0D;             /* Enter */
    if (scancode == 0x2C) return ' ';              /* Space */
    if (scancode == 0x2A) return 0x0C;             /* Backspace → Delete */
    if (scancode == 0x50) return 0x08;             /* Left */
    if (scancode == 0x4F) return 0x09;             /* Right */
    if (scancode == 0x51) return 0x0A;             /* Down */
    if (scancode == 0x52) return 0x0B;             /* Up */
    return -1;
}

static void handle_modifiers(app_globals_t *g, uint8_t mods) {
    zx_t *sys = (zx_t *)g->zx;
    uint8_t changed = mods ^ g->prev_modifiers;
    g->prev_modifiers = mods;

    if (changed & KMOD_SHIFT) {
        if (mods & KMOD_SHIFT) zx_key_down(sys, 0x00);
        else                   zx_key_up(sys, 0x00);
    }
    if (changed & KMOD_CTRL) {
        if (mods & KMOD_CTRL)  zx_key_down(sys, 0x0F);
        else                   zx_key_up(sys, 0x0F);
    }
}

/* Forward declarations — noinline prevents compiler pulling them into .text */
static bool handle_menu_command(hwnd_t hwnd, app_globals_t *g, int command_id)
    __attribute__((noinline));
static void setup_menu(hwnd_t hwnd)
    __attribute__((noinline));

/* ======================================================================
 * Event handler
 * ====================================================================== */

static bool zx_event(hwnd_t hwnd, const window_event_t *ev) {
    /* Event handler runs on the WM task — r9 is NOT our globals pointer.
     * Retrieve app state from window->user_data instead. */
    window_t *win = wm_get_window(hwnd);
    if (!win || !win->user_data) return false;
    app_globals_t *g = (app_globals_t *)win->user_data;
    zx_t *sys = (zx_t *)g->zx;

    if (ev->type == WM_COMMAND) {
        if (ev->command.id == DLG_RESULT_FILE) {
            /* Defer to main loop — load_tap_file uses G macros */
            const char *p = file_dialog_get_path();
            if (p) {
                strncpy(g->tap_path, p, sizeof(g->tap_path) - 1);
                g->tap_path[sizeof(g->tap_path) - 1] = '\0';
                g->tap_path_ready = true;
                xTaskNotifyGive(g->app_task);
            }
            return true;
        }
        if (ev->command.id == DLG_RESULT_CANCEL)
            return true;  /* user cancelled — ignore */
        return handle_menu_command(hwnd, g, ev->command.id);
    }

    if (ev->type == WM_KEYDOWN || ev->type == WM_KEYUP) {
        int zx_key = hid_to_zx(ev->key.scancode);
        if (zx_key >= 0) {
            if (ev->type == WM_KEYDOWN) zx_key_down(sys, zx_key);
            else                         zx_key_up(sys, zx_key);
        }
        handle_modifiers(g, ev->key.modifiers);
        return true;
    }

    if (ev->type == WM_CLOSE) {
        g->closing = true;
        xTaskNotifyGive(g->app_task);
        return true;
    }

    return false;
}


/* ======================================================================
 * Entry point
 * ====================================================================== */

int main(int argc, char **argv) {
    (void)argc;
    (void)argv;

    /* Allocate the globals struct on the SRAM heap and install in r9.
     * From this point on, all code in this task can access G-> fields. */
    app_globals_t *globals = (app_globals_t *)pvPortMalloc(sizeof(app_globals_t));
    if (!globals) { serial_printf("ZX: globals alloc failed\n"); return 1; }
    memset(globals, 0, sizeof(app_globals_t));
    G = globals;

    serial_printf("ZX: starting\n");

    G->app_task = xTaskGetCurrentTaskHandle();

    /* Allocate FIL struct separately to isolate from other state */
    FIL *tap_fil = (FIL *)pvPortMalloc(sizeof(FIL));
    if (!tap_fil) { serial_printf("ZX: FIL alloc failed\n"); return 1; }
    memset(tap_fil, 0, sizeof(FIL));
    G->tap_file = tap_fil;

    /* Allocate emulator state on heap */
    zx_t *zx = (zx_t *)pvPortMalloc(sizeof(zx_t));
    if (!zx) { serial_printf("ZX: zx_t alloc failed\n"); return 1; }
    G->zx = (struct zx_t_fwd *)zx;

    /* Allocate all 3 RAM banks as one contiguous 48KB block to avoid
     * heap fragmentation issues (60KB heap, 48KB needed for RAM). */
    uint8_t *ram_block = (uint8_t *)pvPortMalloc(0x4000 * 3);
    if (!ram_block) { serial_printf("ZX: ram alloc failed\n"); vPortFree(zx); return 1; }
    zx->ram[0] = ram_block;
    zx->ram[1] = ram_block + 0x4000;
    zx->ram[2] = ram_block + 0x8000;

    /* Allocate I2S beeper audio buffer (1152 stereo frames) */
    int16_t *beeper_buf = (int16_t *)pvPortMalloc(1152 * 2 * sizeof(int16_t));
    if (beeper_buf) {
        memset(beeper_buf, 0, 1152 * 2 * sizeof(int16_t));
        zx->beeper_buf = beeper_buf;
        pcm_init(15625, 2);
        zx->beeper_audio = true;
    }

    /* Initialise ZX Spectrum 48K emulator */
    zx_desc_t desc;
    memset(&desc, 0, sizeof(desc));
    desc.type = ZX_TYPE_48K;
    desc.joystick_type = ZX_JOYSTICKTYPE_NONE;
    desc.roms.zx48k.ptr = dump_amstrad_zx48k_bin;
    desc.roms.zx48k.size = sizeof(dump_amstrad_zx48k_bin);
    zx_init(zx, &desc);

    /* Window: 320×240 client area (256×192 + border) + menu bar */
    int16_t fw = CLIENT_W + 2 * THEME_BORDER_WIDTH;
    int16_t fh = CLIENT_H + THEME_TITLE_HEIGHT + THEME_MENU_HEIGHT
               + 2 * THEME_BORDER_WIDTH;
    int16_t x  = (DISPLAY_WIDTH  - fw) / 2;
    int16_t y  = (DISPLAY_HEIGHT - TASKBAR_HEIGHT - fh) / 2;
    if (y < 0) y = 0;

    /* Set icon before window creation — wm_create_window picks it up */
    ((void(*)(const uint8_t*))_sys_table_ptrs[432])(zx_icon_16x16);

    G->app_hwnd = wm_create_window(x, y, fw, fh, "ZX Spectrum",
                                    WSTYLE_DIALOG | WF_MENUBAR,
                                    zx_event, zx_paint);
    if (G->app_hwnd == HWND_NULL) {
        vPortFree(zx);
        return 1;
    }

    /* Store G pointer in window user_data for the paint callback
     * (which runs on the compositor task with a different r9). */
    window_t *win = wm_get_window(G->app_hwnd);
    if (win) win->user_data = G;

    setup_menu(G->app_hwnd);
    wm_show_window(G->app_hwnd);
    wm_set_focus(G->app_hwnd);
    taskbar_invalidate();

    serial_printf("ZX: running\n");

    /* Main emulation loop.
     * zx_exec() takes MICROSECONDS — 20000 µs = 20 ms = 1 ZX frame. */
    while (!G->closing) {
        /* Handle deferred actions from WM task */
        if (G->reset_requested) {
            G->reset_requested = false;
            zx_desc_t rdesc;
            memset(&rdesc, 0, sizeof(rdesc));
            rdesc.type = ZX_TYPE_48K;
            rdesc.joystick_type = ZX_JOYSTICKTYPE_NONE;
            rdesc.roms.zx48k.ptr = dump_amstrad_zx48k_bin;
            rdesc.roms.zx48k.size = sizeof(dump_amstrad_zx48k_bin);
            zx_init(zx, &rdesc);
            /* Re-arm tape trap if TAP file is still loaded */
            if (G->tap_loaded) {
                zx->tape_trap = handle_tape_trap;
                zx->tape_trap_ud = zx;
                f_lseek(G->tap_file, 0);  /* Rewind */
            }
            wm_invalidate(G->app_hwnd);
        }
        if (G->tap_path_ready) {
            G->tap_path_ready = false;
            load_tap_file(G->tap_path);
        }

        for (int burst = 0; burst < 8 && !G->closing; burst++) {
            zx_exec(zx, 2500);
            /* Drain beeper PCM buffer — pcm_write blocks until a DMA
             * buffer is free, which throttles us to real-time. */
            if (zx->beeper_buf_pos >= 1152) {
                pcm_write(zx->beeper_buf, 1152);
                zx->beeper_buf_pos = 0;
            }
        }

        /* Always invalidate — needed for FLASH attribute blinking
         * even when VRAM content hasn't changed */
        G->vram_dirty = false;
        wm_invalidate(G->app_hwnd);
        if (!zx->beeper_audio)
            vTaskDelay(pdMS_TO_TICKS(1));
    }

    wm_destroy_window(G->app_hwnd);
    taskbar_invalidate();
    if (G->tap_loaded) {
        f_close(G->tap_file);
        G->tap_loaded = false;
    }
    if (zx->beeper_audio) {
        pcm_cleanup();
    }
    zx->tape_trap = NULL;
    vPortFree(zx);
    return 0;
}

/* ======================================================================
 * Menu setup (in .text.startup — called once from main)
 * ====================================================================== */

static void setup_menu(hwnd_t hwnd) {
    menu_bar_t bar;
    memset(&bar, 0, sizeof(bar));
    bar.menu_count = 1;

    menu_def_t *file = &bar.menus[0];
    strncpy(file->title, "File", sizeof(file->title) - 1);
    file->accel_key = 0x09; /* HID 'F' — underlines F, enables Alt+F */
    file->item_count = 4;

    strncpy(file->items[0].text, "Load TAP...", 19);
    file->items[0].command_id = CMD_LOAD_TAP;

    strncpy(file->items[1].text, "Reset", 19);
    file->items[1].command_id = CMD_RESET;

    file->items[2].flags = MIF_SEPARATOR;

    strncpy(file->items[3].text, "Exit", 19);
    file->items[3].command_id = CMD_EXIT;

    menu_set(hwnd, &bar);
}

/* ======================================================================
 * Menu command handler (in .text.startup — called via noinline from zx_event)
 * ====================================================================== */

static bool handle_menu_command(hwnd_t hwnd, app_globals_t *g, int command_id) {
    if (command_id == CMD_EXIT) {
        window_event_t ce;
        memset(&ce, 0, sizeof(ce));
        ce.type = WM_CLOSE;
        wm_post_event(hwnd, &ce);
        return true;
    }
    if (command_id == CMD_RESET) {
        /* Defer to main loop — zx_init uses G macros that need r9 */
        g->reset_requested = true;
        xTaskNotifyGive(g->app_task);
        return true;
    }
    if (command_id == CMD_LOAD_TAP) {
        file_dialog_open(hwnd, "Load TAP", "/", ".tap");
        return true;
    }
    return false;
}

/* ======================================================================
 * TAP file loading
 * ====================================================================== */

static void load_tap_file(const char *path) {
    if (G->tap_loaded) {
        f_close(G->tap_file);
        G->tap_loaded = false;
        ZX->tape_trap = NULL;
    }

    FRESULT fr = f_open(G->tap_file, path, FA_READ);
    if (fr != FR_OK) {
        serial_printf("ZX: TAP open failed: %s (err %d)\n", path, fr);
        return;
    }

    G->tap_loaded = true;
    ZX->tape_trap = handle_tape_trap;
    ZX->tape_trap_ud = ZX;
    serial_printf("ZX: TAP loaded: %s\n", path);
}

static void handle_tape_trap(void *ud) {
    zx_t *sys = (zx_t *)ud;

    /* Read Z80 registers set by ROM before calling LD-BYTES:
     *   A  = expected flag byte (0x00 header, 0xFF data)
     *   IX = destination address
     *   DE = expected data length (payload only)
     *   CF = 1: LOAD, 0: VERIFY */
    uint8_t  expected_flag = zx_cpu.AF.B.h;
    uint16_t dest          = zx_cpu.IX.W;
    uint16_t expected_len  = zx_cpu.DE.W;
    bool     is_load       = (zx_cpu.AF.B.l & C_FLAG) != 0;

    /* VERIFY mode: just pretend success */
    if (!is_load) {
        zx_cpu.AF.B.l |= C_FLAG;
        zx_cpu.PC.W = 0x05E2;
        return;
    }

    /* Read 2-byte block length (little-endian) from TAP file */
    uint8_t hdr[3];
    UINT br;
    FRESULT fr = f_read(G->tap_file, hdr, 2, &br);
    if (fr != FR_OK || br < 2) {
        /* EOF or read error — rewind for next LOAD attempt */
        serial_printf("ZX: TAP EOF/error, rewinding\n");
        f_lseek(G->tap_file, 0);
        zx_cpu.AF.B.l &= ~C_FLAG;
        zx_cpu.PC.W = 0x05E2;
        return;
    }

    uint16_t block_len = hdr[0] | ((uint16_t)hdr[1] << 8);
    if (block_len < 2) {
        /* Malformed block */
        zx_cpu.AF.B.l &= ~C_FLAG;
        zx_cpu.PC.W = 0x05E2;
        return;
    }

    /* Read 1-byte flag */
    fr = f_read(G->tap_file, &hdr[2], 1, &br);
    if (fr != FR_OK || br < 1) {
        zx_cpu.AF.B.l &= ~C_FLAG;
        zx_cpu.PC.W = 0x05E2;
        return;
    }
    uint8_t flag = hdr[2];

    /* Flag mismatch: skip this block, let ROM retry with next block */
    if (flag != expected_flag) {
        uint16_t remaining = block_len - 1;
        f_lseek(G->tap_file, f_tell(G->tap_file) + remaining);
        zx_cpu.AF.B.l &= ~C_FLAG;
        zx_cpu.PC.W = 0x05E2;
        return;
    }

    /* data_len = block_len - 2 (subtract flag byte and checksum byte) */
    uint16_t data_len = block_len - 2;
    uint16_t to_load = (expected_len < data_len) ? expected_len : data_len;

    /* Read and copy payload into Z80 RAM in 128-byte chunks */
    uint8_t xor_check = flag;
    uint8_t buf[128];
    uint16_t loaded = 0;

    while (loaded < to_load) {
        uint16_t chunk = to_load - loaded;
        if (chunk > sizeof(buf)) chunk = sizeof(buf);
        fr = f_read(G->tap_file, buf, chunk, &br);
        if (fr != FR_OK || br == 0) break;
        for (UINT i = 0; i < br; i++) {
            WrZ80(dest++, buf[i]);
            xor_check ^= buf[i];
            loaded++;
        }
    }

    /* If data_len > expected_len, read remaining bytes for checksum */
    if (data_len > expected_len) {
        uint16_t skip = data_len - expected_len;
        while (skip > 0) {
            uint16_t chunk = (skip > sizeof(buf)) ? sizeof(buf) : skip;
            fr = f_read(G->tap_file, buf, chunk, &br);
            if (fr != FR_OK || br == 0) break;
            for (UINT i = 0; i < br; i++)
                xor_check ^= buf[i];
            skip -= br;
        }
    }

    /* Read and verify checksum byte */
    uint8_t file_checksum;
    fr = f_read(G->tap_file, &file_checksum, 1, &br);
    if (fr == FR_OK && br == 1)
        xor_check ^= file_checksum;

    /* Update Z80 registers */
    zx_cpu.IX.W += to_load;
    zx_cpu.DE.W -= to_load;

    if (xor_check == 0) {
        /* Success */
        zx_cpu.AF.B.l |= C_FLAG;
        zx_cpu.AF1.W |= 0x40;  /* bit 6 of F' — ROM internal flag */
    } else {
        zx_cpu.AF.B.l &= ~C_FLAG;
    }

    zx_cpu.PC.W = 0x05E2;

    /* If at EOF, rewind for multi-load games */
    if (f_eof(G->tap_file))
        f_lseek(G->tap_file, 0);
}
