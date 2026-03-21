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

// Jaleco_JF11 

class Mapper140 : public Nes_Mapper {
public:
	Mapper140()
	{
		register_state( &regs, 1 );
	}

	virtual void reset_state()
	{
		intercept_writes( 0x6000, 1 );
	}

	virtual void apply_mapping()
	{
		write_intercepted(0, 0x6000, regs );
	}

	bool write_intercepted( nes_time_t time, nes_addr_t addr, int data )
	{
		if ( addr < 0x6000 || addr > 0x7FFF )
			return false;

		regs = data;
		set_prg_bank( 0x8000, bank_32k, data >> 4);
		set_chr_bank( 0, bank_8k, data );

		return true;
	}

	virtual void write( nes_time_t, nes_addr_t addr, int data ) { }

	uint8_t regs;
};
