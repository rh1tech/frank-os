/*
 * QuickNES - NES emulator core
 * Original author: Shay Green (blargg)
 * https://github.com/libretro/QuickNES_Core
 * SPDX-License-Identifier: LGPL-2.1-or-later
 *
 * MMC5 rewritten with run_until() scanline simulation and proper
 * $5204 read support. Fixes Castlevania III (E) story scroll freeze.
 *
 * Fork maintained as part of MurmNES by Mikhail Matveev.
 * https://rh1.tech | https://github.com/rh1tech/murmnes
 */

#pragma once

#include "nes_mapper.h"
#include "nes_core.h"
#include <string.h>

/* Copyright (C) 2004-2006 Shay Green. This module is free software; you
can redistribute it and/or modify it under the terms of the GNU Lesser
General Public License as published by the Free Software Foundation; either
version 2.1 of the License, or (at your option) any later version. This
module is distributed in the hope that it will be useful, but WITHOUT ANY
WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
FOR A PARTICULAR PURPOSE. See the GNU Lesser General Public License for
more details. You should have received a copy of the GNU Lesser General
Public License along with this module; if not, write to the Free Software
Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307 USA */

#include "blargg_source.h"

/* Scanline timing constants (same as MMC3 in mapper004) */
static int const mmc5_ppu_overclock = 3;
/* MMC5 detects the first scanline at PPU cycle 4 of the first visible scanline. */
static nes_time_t const mmc5_first_scanline = 21 * 341 + 4;
static nes_time_t const mmc5_last_scanline = mmc5_first_scanline + 240 * 341;

struct mmc5_state_t
{
	enum { reg_count = 0x30 };
	uint8_t regs [0x30];
	uint8_t irq_enabled;
};
BOOST_STATIC_ASSERT( sizeof (mmc5_state_t) == 0x31 );

class Mapper005 : public Nes_Mapper, mmc5_state_t {
public:
	Mapper005()
	{
		mmc5_state_t* state = this;
		register_state( state, sizeof *state );
	}

	virtual void reset_state()
	{
		irq_scanline = 0;
		irq_pending = false;
		next_scanline_time = mmc5_first_scanline;
		current_scanline = 0;
		multiplier_a = 0;
		multiplier_b = 0;
		memset( exram, 0, sizeof exram );
		regs [0x00] = 2;
		regs [0x01] = 3;
		regs [0x14] = 0x7f;
		regs [0x15] = 0x7f;
		regs [0x16] = 0x7f;
		regs [0x17] = 0x7f;
	}

	virtual void read_state( mapper_state_t const& in )
	{
		Nes_Mapper::read_state( in );
		irq_pending = false;
		next_scanline_time = mmc5_first_scanline;
		current_scanline = 0;
	}

	enum { regs_addr = 0x5100 };

	void start_frame()
	{
		next_scanline_time = mmc5_first_scanline;
		current_scanline = 0;
	}

	/* Simulate scanlines up to the given PPU time.
	 * Check each scanline against irq_scanline. */
	void run_until( nes_time_t end_time )
	{
		if ( !ppu_enabled() )
			return;

		end_time *= mmc5_ppu_overclock;
		while ( next_scanline_time < end_time && next_scanline_time <= mmc5_last_scanline )
		{
			current_scanline++;
			/* MMC5 increments scanline counter and compares on subsequent scanlines */
			if ( irq_scanline && current_scanline == irq_scanline + 1 && !irq_pending )
			{
				irq_pending = true;
			}
			next_scanline_time += 341; /* Nes_Ppu::scanline_len */
		}
	}

	virtual nes_time_t next_irq( nes_time_t present )
	{
		run_until( present );

		if ( !(irq_enabled & 0x80) )
			return no_irq;

		if ( irq_pending )
			return 0;

		if ( !ppu_enabled() || !irq_scanline || irq_scanline >= 240 )
			return no_irq;

		/* Predict when the target scanline will be reached.
		 * IRQ triggers when current_scanline increments to irq_scanline + 1. */
		int remain = (irq_scanline + 1) - current_scanline - 1;
		if ( remain < 0 )
			return no_irq; /* already passed this frame */

		long time = remain * 341L + next_scanline_time;
		if ( time > mmc5_last_scanline )
			return no_irq;

		return time / mmc5_ppu_overclock + 1;
	}

	/* $5204 read: bit 7 = IRQ pending, bit 6 = in-frame.
	 * Reading acknowledges (clears pending). */
	virtual int read( nes_time_t time, nes_addr_t addr )
	{
		if ( addr == 0x5204 )
		{
			run_until( time );
			int result = 0;
			if ( current_scanline > 0 && current_scanline <= 240 )
				result |= 0x40;
			if ( irq_pending )
				result |= 0x80;
			irq_pending = false;
			irq_changed();
			return result;
		}
		if ( addr == 0x5205 )
			return (multiplier_a * multiplier_b) & 0xFF;
		if ( addr == 0x5206 )
			return (multiplier_a * multiplier_b) >> 8;
		/* ExRAM: $5C00-$5FFF — readable in modes 0, 1, 2 */
		if ( addr >= 0x5C00 && addr <= 0x5FFF )
			return exram [addr - 0x5C00];
		return -1;
	}

	virtual void end_frame( nes_time_t end_time )
	{
		run_until( end_time );
		start_frame();
	}

	/* Recalculate all CHR bank mappings based on current CHR mode ($5101).
	 *
	 * Sprite banks ($5120-$5127) and BG banks ($5128-$512B) are
	 * mapped according to the bank size selected by the mode register:
	 *   Mode 0: 8KB  — $5127 for sprites, $512B for BG
	 *   Mode 1: 4KB  — $5123/$5127 for sprites, $512B for BG
	 *   Mode 2: 2KB  — $5121/$5123/$5125/$5127 for sprites, $5129/$512B for BG
	 *   Mode 3: 1KB  — $5120-$5127 for sprites, $5128-$512B for BG
	 */
	void update_chr_banks()
	{
		int mode = regs [0x01] & 3;

		switch ( mode )
		{
		case 0: /* 8KB */
			set_chr_bank( 0x0000, bank_8k, regs [0x27] );
			set_chr_bank_bg( 0x0000, bank_8k, regs [0x2b] );
			break;

		case 1: /* 4KB */
			set_chr_bank( 0x0000, bank_4k, regs [0x23] );
			set_chr_bank( 0x1000, bank_4k, regs [0x27] );
			set_chr_bank_bg( 0x0000, bank_4k, regs [0x2b] );
			set_chr_bank_bg( 0x1000, bank_4k, regs [0x2b] );
			break;

		case 2: /* 2KB */
			set_chr_bank( 0x0000, bank_2k, regs [0x21] );
			set_chr_bank( 0x0800, bank_2k, regs [0x23] );
			set_chr_bank( 0x1000, bank_2k, regs [0x25] );
			set_chr_bank( 0x1800, bank_2k, regs [0x27] );
			set_chr_bank_bg( 0x0000, bank_2k, regs [0x29] );
			set_chr_bank_bg( 0x0800, bank_2k, regs [0x2b] );
			set_chr_bank_bg( 0x1000, bank_2k, regs [0x29] );
			set_chr_bank_bg( 0x1800, bank_2k, regs [0x2b] );
			break;

		case 3: /* 1KB */
			for ( int i = 0; i < 4; i++ )
				set_chr_bank( i * 0x400, bank_1k, regs [0x20 + i] );
			for ( int i = 0; i < 4; i++ )
				set_chr_bank( 0x1000 + i * 0x400, bank_1k, regs [0x24 + i] );
			for ( int i = 0; i < 4; i++ ) {
				set_chr_bank_bg( i * 0x400, bank_1k, regs [0x28 + i] );
				set_chr_bank_bg( 0x1000 + i * 0x400, bank_1k, regs [0x28 + i] );
			}
			break;
		}
	}

	virtual bool write_intercepted( nes_time_t time, nes_addr_t addr, int data )
	{
		int reg = addr - regs_addr;
		if ( (unsigned) reg < reg_count )
		{
			regs [reg] = data;
			switch ( reg )
			{
			case 0x01: /* CHR mode — remap all CHR banks */
				update_chr_banks();
				break;

			case 0x05:
				mirror_manual( data & 3, data >> 2 & 3,
						data >> 4 & 3, data >> 6 & 3 );
				break;

			case 0x15:
				set_prg_bank( 0x8000, bank_16k, data >> 1 & 0x3f );
				break;

			case 0x16:
				set_prg_bank( 0xC000, bank_8k, data & 0x7f );
				break;

			case 0x17:
				set_prg_bank( 0xE000, bank_8k, data & 0x7f );
				break;

			case 0x20: case 0x21: case 0x22: case 0x23:
			case 0x24: case 0x25: case 0x26: case 0x27:
			case 0x28: case 0x29: case 0x2a: case 0x2b:
				update_chr_banks();
				break;
			}
		}
		else if ( addr == 0x5203 )
		{
			run_until( time );
			irq_scanline = data;
			irq_changed();
		}
		else if ( addr == 0x5204 )
		{
			run_until( time );
			irq_enabled = data;
			if ( !(data & 0x80) )
				irq_pending = false;
			irq_changed();
		}
		else if ( addr == 0x5205 )
		{
			multiplier_a = data;
		}
		else if ( addr == 0x5206 )
		{
			multiplier_b = data;
		}
		else if ( addr >= 0x5C00 && addr <= 0x5FFF )
		{
			/* ExRAM: writable in mode 2, cleared to 0 in mode 0/1 during rendering */
			exram [addr - 0x5C00] = data;
		}
		else
		{
			return false;
		}

		return true;
	}

	void apply_mapping()
	{
		static unsigned char prg_list [] = { 0x05, 0x15, 0x16, 0x17 };

		enable_sram();
		enable_mmc5_chr_split();
		for ( int i = 0; i < (int) sizeof prg_list; i++ )
			write_intercepted( 0, regs_addr + prg_list [i], regs [prg_list [i]] );
		update_chr_banks();
		intercept_writes( 0x5100, 0x200 );
		intercept_writes( 0x5C00, 0x400 ); /* ExRAM writes */
		intercept_reads( 0x5200, 0x100 );
		intercept_reads( 0x5C00, 0x400 );  /* ExRAM reads */
		start_frame();
	}

	virtual void write( nes_time_t, nes_addr_t, int ) { }

	uint8_t irq_scanline;
	bool irq_pending;
	nes_time_t next_scanline_time;
	int current_scanline;
	uint8_t multiplier_a;
	uint8_t multiplier_b;
	uint8_t exram [0x400]; /* 1KB ExRAM at $5C00-$5FFF */
};
