/*
 * QuickNES - NES emulator core
 * Original author: Shay Green (blargg)
 * https://github.com/libretro/QuickNES_Core
 * SPDX-License-Identifier: LGPL-2.1-or-later
 *
 * Fork maintained as part of MurmNES by Mikhail Matveev.
 * https://rh1.tech | https://github.com/rh1tech/murmnes
 */

#pragma once

#include "nes_mapper.h"
 
// NTDEC's TC-112 mapper IC.

class Mapper193 : public Nes_Mapper {
public:
	Mapper193()
	{
		register_state( regs, sizeof regs );
	}

	virtual void reset_state()
	{ }

	virtual void apply_mapping()
	{
		for ( size_t i = 0; i < sizeof regs; i++ )
			write_intercepted( 0, 0x6000 + i, regs [ i ] );
		set_prg_bank( 0xA000, bank_8k, ~2 );
		set_prg_bank( 0xC000, bank_8k, ~1 );
		set_prg_bank( 0xE000, bank_8k, ~0 );
		intercept_writes( 0x6000, 0x03 );
	}

	virtual void write( nes_time_t, nes_addr_t addr, int data )
	{ }

	virtual bool write_intercepted( nes_time_t, nes_addr_t addr, int data )
	{
		if ( addr < 0x6000 || addr > 0x6003 )
			return false;

		regs [ addr & 0x03 ] = data;
		switch ( addr & 0x03 )
		{
		case 0: set_chr_bank( 0x0000, bank_4k, regs [ 0 ] >> 2 ); break;
		case 1: set_chr_bank( 0x1000, bank_2k, regs [ 1 ] >> 1 ); break;
		case 2: set_chr_bank( 0x1800, bank_2k, regs [ 2 ] >> 1 ); break;
		case 3: set_prg_bank( 0x8000, bank_8k, regs [ 3 ] ); break;
		}

		return true;
	}

	uint8_t regs [ 4 ];
};

