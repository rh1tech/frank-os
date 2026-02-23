#pragma once
/*
 * zx.h — ZX Spectrum 48K emulator
 *
 * Rewritten to use Fayzullin Z80 core + ARM ASM dispatcher instead of
 * chips z80.h per-T-state emulation.
 *
 * Original structure based on zx2040 by antirez / chips by floooh.
 */
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdalign.h>

#include "Z80.h"
#include "z80_arm.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ZX_SNAPSHOT_VERSION (0x0002)

#define ZX_FRAMEBUFFER_WIDTH (320/2)
#define ZX_FRAMEBUFFER_HEIGHT (256)
#define ZX_FRAMEBUFFER_SIZE_BYTES (ZX_FRAMEBUFFER_WIDTH * ZX_FRAMEBUFFER_HEIGHT)
#define ZX_DISPLAY_WIDTH (320)
#define ZX_DISPLAY_HEIGHT (256)

// ZX Spectrum models
typedef enum {
    ZX_TYPE_48K,
} zx_type_t;

// ZX Spectrum joystick types
typedef enum {
    ZX_JOYSTICKTYPE_NONE,
    ZX_JOYSTICKTYPE_KEMPSTON,
    ZX_JOYSTICKTYPE_SINCLAIR_1,
    ZX_JOYSTICKTYPE_SINCLAIR_2,
} zx_joystick_type_t;

// joystick mask bits
#define ZX_JOYSTICK_RIGHT   (1<<0)
#define ZX_JOYSTICK_LEFT    (1<<1)
#define ZX_JOYSTICK_DOWN    (1<<2)
#define ZX_JOYSTICK_UP      (1<<3)
#define ZX_JOYSTICK_BTN     (1<<4)

// config parameters for zx_init()
typedef struct {
    zx_type_t type;
    zx_joystick_type_t joystick_type;
    struct {
        chips_range_t zx48k;
    } roms;
} zx_desc_t;

// ZX emulator state
typedef struct {
    zx_type_t type;
    zx_joystick_type_t joystick_type;
    bool memory_paging_disabled;
    uint8_t kbd_joymask;
    uint8_t joy_joymask;
    uint32_t tick_count;
    uint8_t last_mem_config;
    uint8_t last_fe_out;
    uint8_t blink_counter;
    uint8_t border_color;
    int frame_scan_lines;
    int top_border_scanlines;
    int scanline_period;
    int scanline_counter;
    int scanline_y;

    int beeper_state;
    int16_t *beeper_buf;         /* PCM buffer (1152 stereo frames, heap) */
    int      beeper_buf_pos;     /* write position in stereo frames */
    bool     beeper_audio;       /* I2S audio active */

    int int_counter;
    uint32_t display_ram_bank;
    kbd_t kbd;
    uint64_t freq_hz;
    bool valid;
    void (*tape_trap)(void *ud);
    void *tape_trap_ud;
    uint8_t *ram[3];   /* Allocated separately — avoids SRAM code/heap overlap */
} zx_t;

// zx_cpu, ZX_ROM, ZX_RAM must be defined by the including module
// before #include "zx.h" with CHIPS_IMPL.  Typically these are macros
// that dereference a heap-allocated app_globals_t via register r9.

void zx_init(zx_t* sys, const zx_desc_t* desc);
void zx_discard(zx_t* sys);
void zx_reset(zx_t* sys);
chips_display_info_t zx_display_info(zx_t* sys);
uint32_t zx_exec(zx_t* sys, uint32_t micro_seconds);
void zx_key_down(zx_t* sys, int key_code);
void zx_key_up(zx_t* sys, int key_code);
void zx_set_joystick_type(zx_t* sys, zx_joystick_type_t type);
zx_joystick_type_t zx_joystick_type(zx_t* sys);
void zx_joystick(zx_t* sys, uint8_t mask);
bool zx_quickload(zx_t* sys, chips_range_t data);

#ifdef __cplusplus
}
#endif

/*-- IMPLEMENTATION ----------------------------------------------------------*/
#ifdef CHIPS_IMPL
#include <string.h>
#ifndef CHIPS_ASSERT
    #include <assert.h>
    #define CHIPS_ASSERT(c) assert(c)
#endif

static void _zx_init_keyboard_matrix(zx_t* sys);

#define _ZX_DEFAULT(val,def) (((val) != 0) ? (val) : (def))
#define _ZX_48K_FREQUENCY (3500000)

/* Tape trap address in ROM (LD-BYTES entry point) */
#define ZX_TAPE_TRAP_ADDR 0x0556

void zx_init(zx_t* sys, const zx_desc_t* desc) {
    CHIPS_ASSERT(sys && desc);

    /* Save heap pointers across memset (allocated externally) */
    uint8_t *saved_ram[3] = { sys->ram[0], sys->ram[1], sys->ram[2] };
    int16_t *saved_beeper_buf = sys->beeper_buf;
    bool     saved_beeper_audio = sys->beeper_audio;
    memset(sys, 0, sizeof(zx_t));
    sys->ram[0] = saved_ram[0];
    sys->ram[1] = saved_ram[1];
    sys->ram[2] = saved_ram[2];
    sys->beeper_buf = saved_beeper_buf;
    sys->beeper_audio = saved_beeper_audio;
    sys->valid = true;
    sys->type = desc->type;
    sys->joystick_type = desc->joystick_type;
    sys->freq_hz = _ZX_48K_FREQUENCY;

    sys->border_color = 0;
    CHIPS_ASSERT(desc->roms.zx48k.ptr && (desc->roms.zx48k.size == 0x4000));
    sys->display_ram_bank = 0;
    sys->frame_scan_lines = 312;
    sys->top_border_scanlines = 64;
    sys->scanline_period = 224;
    sys->scanline_counter = sys->scanline_period;

    /* Set up global memory pointers for Fayzullin callbacks */
    ZX_ROM = (uint8_t *)desc->roms.zx48k.ptr;
    ZX_RAM[0] = sys->ram[0];
    ZX_RAM[1] = sys->ram[1];
    ZX_RAM[2] = sys->ram[2];

    /* Initialize Fayzullin Z80 */
    ResetZ80(&zx_cpu);
    zx_cpu.TrapBadOps = 0;
    zx_cpu.IPeriod = sys->scanline_period;
    zx_cpu.ICount = sys->scanline_period;

    _zx_init_keyboard_matrix(sys);

    /* Zero RAM banks */
    for (int i = 0; i < 3; i++) {
        if (sys->ram[i]) memset(sys->ram[i], 0, 0x4000);
    }

    /* Audio: reset buffer position (beeper_buf preserved across init) */
    sys->beeper_buf_pos = 0;
}

void zx_discard(zx_t* sys) {
    CHIPS_ASSERT(sys && sys->valid);
    sys->valid = false;
}

void zx_reset(zx_t* sys) {
    CHIPS_ASSERT(sys && sys->valid);
    ResetZ80(&zx_cpu);
    zx_cpu.TrapBadOps = 0;
    sys->memory_paging_disabled = false;
    sys->kbd_joymask = 0;
    sys->joy_joymask = 0;
    sys->last_fe_out = 0;
    sys->scanline_counter = sys->scanline_period;
    sys->scanline_y = 0;
    sys->blink_counter = 0;
    sys->display_ram_bank = 0;

    /* Re-setup memory pointers (ZX_ROM unchanged — points to const ROM image) */
    ZX_RAM[0] = sys->ram[0];
    ZX_RAM[1] = sys->ram[1];
    ZX_RAM[2] = sys->ram[2];
}

uint32_t zx_exec(zx_t* sys, uint32_t micro_seconds) {
    CHIPS_ASSERT(sys && sys->valid);
    uint32_t num_ticks = clk_us_to_ticks(sys->freq_hz, micro_seconds);
    uint32_t remaining = num_ticks;

    while (remaining > 0) {
        /* Run up to next scanline boundary */
        uint32_t batch = remaining;
        if (batch > (uint32_t)sys->scanline_counter)
            batch = sys->scanline_counter;

        int ret = z80_arm_exec(batch);
        uint32_t ran = (ret < 0) ? batch + (uint32_t)(-ret) : batch - ret;

        sys->scanline_counter -= ran;
        remaining -= (ran < remaining) ? ran : remaining;

        /* Tape trap: check BEFORE delivering interrupts, because
         * IntZ80 clears IFF_HALT and advances PC past the HALT. */
        if ((zx_cpu.IFF & IFF_HALT) && zx_cpu.PC.W == ZX_TAPE_TRAP_ADDR) {
            if (sys->tape_trap) {
                zx_cpu.IFF &= ~IFF_HALT;
                zx_cpu.PC.W++; /* Advance past HALT */
                sys->tape_trap(sys->tape_trap_ud);
            }
            /* else: no handler — leave Z80 halted; vblank interrupt
             * will eventually wake it and ROM error handling runs. */
        }

        /* Scanline boundary → sample beeper + check vblank */
        if (sys->scanline_counter <= 0) {
            sys->scanline_counter += sys->scanline_period;

            /* Sample beeper for I2S audio (one sample per scanline = 15625 Hz).
             * Stop at 1152 — caller drains the buffer via pcm_write. */
            if (sys->beeper_buf && sys->beeper_buf_pos < 1152) {
                int16_t s = sys->beeper_state ? 4096 : -4096;
                int pos = sys->beeper_buf_pos;
                sys->beeper_buf[pos * 2]     = s;
                sys->beeper_buf[pos * 2 + 1] = s;
                sys->beeper_buf_pos = pos + 1;
            }

            if (++sys->scanline_y >= sys->frame_scan_lines) {
                sys->scanline_y = 0;
                sys->blink_counter++;
                IntZ80(&zx_cpu, INT_RST38);
            }
        }
    }

    kbd_update(&sys->kbd, micro_seconds);
    return num_ticks;
}

void zx_key_down(zx_t* sys, int key_code) {
    CHIPS_ASSERT(sys && sys->valid);
    switch (sys->joystick_type) {
        case ZX_JOYSTICKTYPE_NONE:
            kbd_key_down(&sys->kbd, key_code);
            break;
        case ZX_JOYSTICKTYPE_KEMPSTON:
            switch (key_code) {
                case 0xFF:  sys->kbd_joymask |= ZX_JOYSTICK_BTN; break;
                case 0xFE:  sys->kbd_joymask |= ZX_JOYSTICK_LEFT; break;
                case 0xFD:  sys->kbd_joymask |= ZX_JOYSTICK_RIGHT; break;
                case 0xFC:  sys->kbd_joymask |= ZX_JOYSTICK_DOWN; break;
                case 0xFB:  sys->kbd_joymask |= ZX_JOYSTICK_UP; break;
                default:    kbd_key_down(&sys->kbd, key_code); break;
            }
            break;
        case ZX_JOYSTICKTYPE_SINCLAIR_1:
            switch (key_code) {
                case 0xFF:  key_code = '5'; break;
                case 0xFE:  key_code = '1'; break;
                case 0xFD:  key_code = '2'; break;
                case 0xFC:  key_code = '3'; break;
                case 0xFB:  key_code = '4'; break;
                default: break;
            }
            kbd_key_down(&sys->kbd, key_code);
            break;
        case ZX_JOYSTICKTYPE_SINCLAIR_2:
            switch (key_code) {
                case 0xFF:  key_code = '0'; break;
                case 0xFE:  key_code = '6'; break;
                case 0xFD:  key_code = '7'; break;
                case 0xFC:  key_code = '8'; break;
                case 0xFB:  key_code = '9'; break;
                default: break;
            }
            kbd_key_down(&sys->kbd, key_code);
            break;
    }
}

void zx_key_up(zx_t* sys, int key_code) {
    CHIPS_ASSERT(sys && sys->valid);
    switch (sys->joystick_type) {
        case ZX_JOYSTICKTYPE_NONE:
            kbd_key_up(&sys->kbd, key_code);
            break;
        case ZX_JOYSTICKTYPE_KEMPSTON:
            switch (key_code) {
                case 0xFF:  sys->kbd_joymask &= ~ZX_JOYSTICK_BTN; break;
                case 0xFE:  sys->kbd_joymask &= ~ZX_JOYSTICK_LEFT; break;
                case 0xFD:  sys->kbd_joymask &= ~ZX_JOYSTICK_RIGHT; break;
                case 0xFC:  sys->kbd_joymask &= ~ZX_JOYSTICK_DOWN; break;
                case 0xFB:  sys->kbd_joymask &= ~ZX_JOYSTICK_UP; break;
                default:    kbd_key_up(&sys->kbd, key_code); break;
            }
            break;
        case ZX_JOYSTICKTYPE_SINCLAIR_1:
            switch (key_code) {
                case 0xFF:  key_code = '5'; break;
                case 0xFE:  key_code = '1'; break;
                case 0xFD:  key_code = '2'; break;
                case 0xFC:  key_code = '3'; break;
                case 0xFB:  key_code = '4'; break;
                default: break;
            }
            kbd_key_up(&sys->kbd, key_code);
            break;
        case ZX_JOYSTICKTYPE_SINCLAIR_2:
            switch (key_code) {
                case 0xFF:  key_code = '0'; break;
                case 0xFE:  key_code = '6'; break;
                case 0xFD:  key_code = '7'; break;
                case 0xFC:  key_code = '8'; break;
                case 0xFB:  key_code = '9'; break;
                default: break;
            }
            kbd_key_up(&sys->kbd, key_code);
            break;
    }
}

void zx_set_joystick_type(zx_t* sys, zx_joystick_type_t type) {
    CHIPS_ASSERT(sys && sys->valid);
    sys->joystick_type = type;
}

zx_joystick_type_t zx_joystick_type(zx_t* sys) {
    CHIPS_ASSERT(sys && sys->valid);
    return sys->joystick_type;
}

void zx_joystick(zx_t* sys, uint8_t mask) {
    CHIPS_ASSERT(sys && sys->valid);
    if (sys->joystick_type == ZX_JOYSTICKTYPE_SINCLAIR_1) {
        if (mask & ZX_JOYSTICK_BTN)   { kbd_key_down(&sys->kbd, '5'); }
        else                          { kbd_key_up(&sys->kbd, '5'); }
        if (mask & ZX_JOYSTICK_LEFT)  { kbd_key_down(&sys->kbd, '1'); }
        else                          { kbd_key_up(&sys->kbd, '1'); }
        if (mask & ZX_JOYSTICK_RIGHT) { kbd_key_down(&sys->kbd, '2'); }
        else                          { kbd_key_up(&sys->kbd, '2'); }
        if (mask & ZX_JOYSTICK_DOWN)  { kbd_key_down(&sys->kbd, '3'); }
        else                          { kbd_key_up(&sys->kbd, '3'); }
        if (mask & ZX_JOYSTICK_UP)    { kbd_key_down(&sys->kbd, '4'); }
        else                          { kbd_key_up(&sys->kbd, '4'); }
    }
    else if (sys->joystick_type == ZX_JOYSTICKTYPE_SINCLAIR_2) {
        if (mask & ZX_JOYSTICK_BTN)   { kbd_key_down(&sys->kbd, '0'); }
        else                          { kbd_key_up(&sys->kbd, '0'); }
        if (mask & ZX_JOYSTICK_LEFT)  { kbd_key_down(&sys->kbd, '6'); }
        else                          { kbd_key_up(&sys->kbd, '6'); }
        if (mask & ZX_JOYSTICK_RIGHT) { kbd_key_down(&sys->kbd, '7'); }
        else                          { kbd_key_up(&sys->kbd, '7'); }
        if (mask & ZX_JOYSTICK_DOWN)  { kbd_key_down(&sys->kbd, '8'); }
        else                          { kbd_key_up(&sys->kbd, '8'); }
        if (mask & ZX_JOYSTICK_UP)    { kbd_key_down(&sys->kbd, '9'); }
        else                          { kbd_key_up(&sys->kbd, '9'); }
    }
    else {
        sys->joy_joymask = mask;
    }
}

static void _zx_init_keyboard_matrix(zx_t* sys) {
    kbd_init(&sys->kbd, 1);
    kbd_register_modifier(&sys->kbd, 0, 0, 0);
    kbd_register_modifier(&sys->kbd, 1, 7, 1);
    const char* keymap =
        " zxcv"
        "asdfg"
        "qwert"
        "12345"
        "09876"
        "poiuy"
        " lkjh"
        "  mnb"
        " ZXCV"
        "ASDFG"
        "QWERT"
        "     "
        "     "
        "POIUY"
        " LKJH"
        "  MNB"
        " : ?/"
        "     "
        "   <>"
        "!@#$%"
        "_)('&"
        "\";   "
        " =+-^"
        "  .,*";
    for (int layer = 0; layer < 3; layer++) {
        for (int column = 0; column < 8; column++) {
            for (int line = 0; line < 5; line++) {
                const uint8_t c = keymap[layer*40 + column*5 + line];
                if (c != 0x20) {
                    kbd_register_key(&sys->kbd, c, column, line, (layer>0) ? (1<<(layer-1)) : 0);
                }
            }
        }
    }
    kbd_register_key(&sys->kbd, ' ', 7, 0, 0);
    kbd_register_key(&sys->kbd, 0x0F, 7, 1, 0);
    kbd_register_key(&sys->kbd, 0x08, 3, 4, 1);
    kbd_register_key(&sys->kbd, 0x0A, 4, 4, 1);
    kbd_register_key(&sys->kbd, 0x0B, 4, 3, 1);
    kbd_register_key(&sys->kbd, 0x09, 4, 2, 1);
    kbd_register_key(&sys->kbd, 0x07, 3, 0, 1);
    kbd_register_key(&sys->kbd, 0x0C, 4, 0, 1);
    kbd_register_key(&sys->kbd, 0x0D, 6, 0, 0);
}

/*=== FILE LOADING ===========================================================*/

typedef struct {
    uint8_t A, F;
    uint8_t C, B;
    uint8_t L, H;
    uint8_t PC_l, PC_h;
    uint8_t SP_l, SP_h;
    uint8_t I, R;
    uint8_t flags0;
    uint8_t E, D;
    uint8_t C_, B_;
    uint8_t E_, D_;
    uint8_t L_, H_;
    uint8_t A_, F_;
    uint8_t IY_l, IY_h;
    uint8_t IX_l, IX_h;
    uint8_t EI;
    uint8_t IFF2;
    uint8_t flags1;
} _zx_z80_header;

typedef struct {
    uint8_t len_l;
    uint8_t len_h;
    uint8_t PC_l, PC_h;
    uint8_t hw_mode;
    uint8_t out_7ffd;
    uint8_t rom1;
    uint8_t flags;
    uint8_t out_fffd;
    uint8_t tlow_l;
    uint8_t tlow_h;
    uint8_t spectator_flags;
    uint8_t mgt_rom_paged;
    uint8_t multiface_rom_paged;
    uint8_t rom_0000_1fff;
    uint8_t rom_2000_3fff;
    uint8_t joy_mapping[10];
    uint8_t kbd_mapping[10];
    uint8_t mgt_type;
    uint8_t disciple_button_state;
    uint8_t disciple_flags;
    uint8_t out_1ffd;
} _zx_z80_ext_header;

typedef struct {
    uint8_t len_l;
    uint8_t len_h;
    uint8_t page_nr;
} _zx_z80_page_header;

static bool _zx_overflow(const uint8_t* ptr, intptr_t num_bytes, const uint8_t* end_ptr) {
    return (ptr + num_bytes) > end_ptr;
}

bool zx_quickload(zx_t* sys, chips_range_t data) {
    CHIPS_ASSERT(data.ptr && (data.size > 0));
    uint8_t* ptr = data.ptr;
    const uint8_t* end_ptr = ptr + data.size;
    if (_zx_overflow(ptr, sizeof(_zx_z80_header), end_ptr)) {
        return false;
    }
    const _zx_z80_header* hdr = (const _zx_z80_header*) ptr;
    ptr += sizeof(_zx_z80_header);
    const _zx_z80_ext_header* ext_hdr = 0;
    uint16_t pc = (hdr->PC_h<<8 | hdr->PC_l) & 0xFFFF;
    const bool is_version1 = 0 != pc;
    if (!is_version1) {
        if (_zx_overflow(ptr, sizeof(_zx_z80_ext_header), end_ptr)) {
            return false;
        }
        ext_hdr = (_zx_z80_ext_header*) ptr;
        int ext_hdr_len = (ext_hdr->len_h<<8)|ext_hdr->len_l;
        ptr += 2 + ext_hdr_len;
        if (ext_hdr->hw_mode >= 3) {
            return false;
        }
    }
    const bool v1_compr = 0 != (hdr->flags0 & (1<<5));
    while (ptr < end_ptr) {
        int page_index = 0;
        int src_len = 0;
        if (is_version1) {
            src_len = data.size - sizeof(_zx_z80_header);
        }
        else {
            _zx_z80_page_header* phdr = (_zx_z80_page_header*) ptr;
            if (_zx_overflow(ptr, sizeof(_zx_z80_page_header), end_ptr)) {
                return false;
            }
            ptr += sizeof(_zx_z80_page_header);
            src_len = (phdr->len_h<<8 | phdr->len_l) & 0xFFFF;
            page_index = phdr->page_nr - 3;
            if ((sys->type == ZX_TYPE_48K) && (page_index == 5)) {
                page_index = 0;
            }
            if ((page_index < 0) || (page_index > 7)) {
                page_index = -1;
            }
        }
        uint8_t* dst_ptr;
        if (-1 == page_index) {
            dst_ptr = NULL;
        } else {
            dst_ptr = sys->ram[page_index];
        }
        if (0xFFFF == src_len) {
            return false;
        }
        else {
            int src_pos = 0;
            bool v1_done = false;
            uint8_t val[4];
            while ((src_pos < src_len) && !v1_done) {
                val[0] = ptr[src_pos];
                val[1] = ptr[src_pos+1];
                val[2] = ptr[src_pos+2];
                val[3] = ptr[src_pos+3];
                if (v1_compr && (0==val[0]) && (0xED==val[1]) && (0xED==val[2]) && (0==val[3])) {
                    v1_done = true;
                    src_pos += 4;
                }
                else if (0xED == val[0]) {
                    if (0xED == val[1]) {
                        uint8_t count = val[2];
                        CHIPS_ASSERT(0 != count);
                        uint8_t data = val[3];
                        src_pos += 4;
                        for (int i = 0; i < count; i++) {
                            if (dst_ptr) *dst_ptr++ = data;
                        }
                    }
                    else {
                        if (dst_ptr) *dst_ptr++ = val[0];
                        src_pos++;
                    }
                }
                else {
                    if (dst_ptr) *dst_ptr++ = val[0];
                    src_pos++;
                }
            }
            CHIPS_ASSERT(src_pos == src_len);
        }
        if (0xFFFF == src_len) {
            ptr += 0x4000;
        }
        else {
            ptr += src_len;
        }
    }

    /* Load registers into Fayzullin Z80 struct */
    ResetZ80(&zx_cpu);
    zx_cpu.AF.B.h = hdr->A;  zx_cpu.AF.B.l = hdr->F;
    zx_cpu.BC.B.h = hdr->B;  zx_cpu.BC.B.l = hdr->C;
    zx_cpu.DE.B.h = hdr->D;  zx_cpu.DE.B.l = hdr->E;
    zx_cpu.HL.B.h = hdr->H;  zx_cpu.HL.B.l = hdr->L;
    zx_cpu.IX.W = (hdr->IX_h<<8)|hdr->IX_l;
    zx_cpu.IY.W = (hdr->IY_h<<8)|hdr->IY_l;
    zx_cpu.AF1.W = (hdr->A_<<8)|hdr->F_;
    zx_cpu.BC1.W = (hdr->B_<<8)|hdr->C_;
    zx_cpu.DE1.W = (hdr->D_<<8)|hdr->E_;
    zx_cpu.HL1.W = (hdr->H_<<8)|hdr->L_;
    zx_cpu.SP.W = (hdr->SP_h<<8)|hdr->SP_l;
    zx_cpu.I = hdr->I;
    zx_cpu.R = (hdr->R & 0x7F) | ((hdr->flags0 & 1)<<7);

    /* Set interrupt mode flags */
    zx_cpu.IFF = 0;
    if (hdr->EI != 0)   zx_cpu.IFF |= IFF_1;
    if (hdr->IFF2 != 0) zx_cpu.IFF |= IFF_2;
    if (hdr->flags1 != 0xFF) {
        uint8_t im = hdr->flags1 & 3;
        if (im == 1)      zx_cpu.IFF |= IFF_IM1;
        else if (im == 2) zx_cpu.IFF |= IFF_IM2;
    } else {
        zx_cpu.IFF |= IFF_IM1;
    }

    if (ext_hdr) {
        zx_cpu.PC.W = (ext_hdr->PC_h<<8)|ext_hdr->PC_l;
    } else {
        zx_cpu.PC.W = (hdr->PC_h<<8)|hdr->PC_l;
    }
    zx_cpu.TrapBadOps = 0;

    sys->border_color = (hdr->flags0>>1) & 7;
    return true;
}

chips_display_info_t zx_display_info(zx_t* sys) {
    static const uint32_t palette[16] = {
        0xFF000000, 0xFFD70000, 0xFF0000D7, 0xFFD700D7,
        0xFF00D700, 0xFFD7D700, 0xFF00D7D7, 0xFFD7D7D7,
        0xFF000000, 0xFFFF0000, 0xFF0000FF, 0xFFFF00FF,
        0xFF00FF00, 0xFFFFFF00, 0xFF00FFFF, 0xFFFFFFFF,
    };
    const chips_display_info_t res = {
        .frame = {
            .dim = {
                .width = ZX_FRAMEBUFFER_WIDTH,
                .height = ZX_FRAMEBUFFER_HEIGHT,
            },
            .bytes_per_pixel = 1,
        },
        .screen = {
            .x = 0,
            .y = 0,
            .width = ZX_DISPLAY_WIDTH,
            .height = ZX_DISPLAY_HEIGHT,
        },
        .palette = {
            .ptr = (void*)palette,
            .size = sizeof(palette),
        }
    };
    CHIPS_ASSERT(((sys == 0) && (res.frame.buffer.ptr == 0)) || ((sys != 0) && (res.frame.buffer.ptr != 0)));
    return res;
}

#endif // CHIPS_IMPL
