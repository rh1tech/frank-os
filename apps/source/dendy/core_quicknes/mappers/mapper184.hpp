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

// Sunsoft1

class Mapper184 : public Nes_Mapper {
public:
	Mapper184()
	{
		register_state( &regs, 1 );
	}

	virtual void reset_state()
	{}

	virtual void apply_mapping()
	{
		set_prg_bank( 0x8000, bank_32k, 0 );
		intercept_writes( 0x6000, 1 );
		write_intercepted( 0, 0x6000, regs );
	}

	virtual bool write_intercepted( nes_time_t, nes_addr_t addr, int data )
	{
		if ( addr != 0x6000 )
			return false;

		regs = data;
		set_chr_bank( 0x0000, bank_4k, data & 0x07 );
		set_chr_bank( 0x1000, bank_4k, ( data >> 4 ) & 0x07 );

		return true;
	}

	virtual void write( nes_time_t, nes_addr_t addr, int data )
	{}

	uint8_t regs;
};

