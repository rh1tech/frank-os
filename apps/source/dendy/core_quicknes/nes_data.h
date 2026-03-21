/*
 * QuickNES - NES emulator core
 * Original author: Shay Green (blargg)
 * https://github.com/libretro/QuickNES_Core
 * SPDX-License-Identifier: LGPL-2.1-or-later
 *
 * Fork maintained as part of MurmNES by Mikhail Matveev.
 * https://rh1.tech | https://github.com/rh1tech/murmnes
 */

#ifndef NES_DATA_H
#define NES_DATA_H

#include "blargg_common.h"
#include "apu_state.h"

typedef long nes_tag_t;

#if 'ABCD' == '\101\102\103\104'
#define FOUR_CHAR( c ) (\
	((c) / '\1\0\0\0' % 0x100 * 0x01000000L) +\
	((c) / '\0\1\0\0' % 0x100 * 0x00010000L) +\
	((c) / '\0\0\1\0' % 0x100 * 0x00000100L) +\
	((c) / '\0\0\0\1' % 0x100 * 0x00000001L)\
)
#else
#if 'ABCD' == 0x41424344
#define FOUR_CHAR( c ) c
#else
#define FOUR_CHAR( c ) (\
	((c) / 0x01000000 % 0x100 * 0x00000001) +\
	((c) / 0x00010000 % 0x100 * 0x00000100) +\
	((c) / 0x00000100 % 0x100 * 0x00010000) +\
	((c) / 0x00000001 % 0x100 * 0x01000000)\
)
#endif
#endif

// Binary format of save state blocks. All multi-uint8_t values are stored in little-endian.

nes_tag_t const state_file_tag = FOUR_CHAR('NESS');


// Name of cartridge file in 8-bit characters (UTF-8 preferred) with ".nes" etc *removed*,
// no NUL termination. Yes: "Castlevania (U)". No: "Strider (U).nes".
nes_tag_t const cart_name_tag = FOUR_CHAR('romn');

// CRC-32 of cartridge's PRG and CHR data combined
nes_tag_t const cart_checksum_tag = FOUR_CHAR('csum');

struct nes_block_t
{
	uint32_t tag; // ** stored in big-endian
	uint32_t size;
	
	void swap();
};
BOOST_STATIC_ASSERT( sizeof (nes_block_t) == 8 );

unsigned long const group_begin_size = 0xffffffff; // group block has this size
nes_tag_t const group_end_tag = FOUR_CHAR('gend'); // group end block has this tag

struct nes_state_t
{
	uint16_t timestamp; // CPU clocks * 15 (for NTSC)
	uint8_t pal;
	uint8_t unused [1];
	uint32_t frame_count; // number of frames emulated since power-up
	
	enum { tag = FOUR_CHAR('TIME') };
	void swap();
};
BOOST_STATIC_ASSERT( sizeof (nes_state_t) == 8 );

struct joypad_state_t
{
	uint32_t joypad_latches [2]; // joypad 1 & 2 shift registers
	uint8_t w4016;             // strobe
	uint8_t unused [3];
	
	enum { tag = FOUR_CHAR('CTRL') };
	void swap();
};
BOOST_STATIC_ASSERT( sizeof (joypad_state_t) == 12 );

// Increase this (and let me know) if your mapper requires more state. This only
// sets the size of the in-memory buffer; it doesn't affect the file format at all.
unsigned const max_mapper_state_size = 512;   //was 256, needed more for VRC7 audio state
struct mapper_state_t
{
	int size;
	union {
		double align;
		uint8_t data [max_mapper_state_size];
	};
	
	void write( const void* p, unsigned long s );
	int read( void* p, unsigned long s ) const;
};

struct cpu_state_t
{
	uint16_t pc;
	uint8_t s;
	uint8_t p;
	uint8_t a;
	uint8_t x;
	uint8_t y;
	uint8_t unused [1];
	
	enum { tag = FOUR_CHAR('CPUR') };
	void swap();
};
BOOST_STATIC_ASSERT( sizeof (cpu_state_t) == 8 );

struct ppu_state_t
{
	uint8_t w2000;                 // control
	uint8_t w2001;                 // control
	uint8_t r2002;                 // status
	uint8_t w2003;                 // sprite ram addr
	uint8_t r2007;                 // vram read buffer
	uint8_t second_write;          // next write to $2005/$2006 is second since last $2002 read
	uint16_t vram_addr;  // loopy_v
	uint16_t vram_temp;  // loopy_t
	uint8_t pixel_x;               // fine-scroll (0-7)
	uint8_t unused;
	uint8_t palette [0x20];        // entries $10, $14, $18, $1c should be ignored
	uint16_t decay_low;
	uint16_t decay_high;
	uint8_t open_bus;
	uint8_t unused2[3];
	
	enum { tag = FOUR_CHAR('PPUR') };
	void swap();
};
BOOST_STATIC_ASSERT( sizeof (ppu_state_t) == 20 + 0x20 );

struct mmc1_state_t
{
	uint8_t regs [4]; // current registers (5 bits each)
	uint8_t bit;      // number of bits in buffer (0 to 4)
	uint8_t buf;      // currently buffered bits (new bits added to bottom)
};
BOOST_STATIC_ASSERT( sizeof (mmc1_state_t) == 6 );

struct mmc3_state_t
{
	uint8_t banks [8]; // last writes to $8001 indexed by (mode & 7)
	uint8_t mode;      // $8000
	uint8_t mirror;    // $a000
	uint8_t sram_mode; // $a001
	uint8_t irq_ctr;   // internal counter
	uint8_t irq_latch; // $c000
	uint8_t irq_enabled;// last write was to 0) $e000, 1) $e001
	uint8_t irq_flag;
};
BOOST_STATIC_ASSERT( sizeof (mmc3_state_t) == 15 );

#endif
