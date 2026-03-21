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

// https://www.nesdev.org/wiki/INES_Mapper240

class Mapper240 : public Nes_Mapper {
public:
	Mapper240()
	{
		register_state( &regs, 1 );
	}

	virtual void reset_state()
	{
	}

	virtual void apply_mapping()
	{
		enable_sram();
		intercept_writes( 0x4020, 1 );
		write_intercepted( 0, 0x4120, regs );
	}

	virtual void write( nes_time_t, nes_addr_t, int data )
	{ }

	virtual bool write_intercepted( nes_time_t, nes_addr_t addr, int data )
	{
		if ( addr < 0x4020 || addr > 0x5FFF )
			return false;

		regs = data;
		set_chr_bank( 0x0000, bank_8k, data & 0x0F );
		set_prg_bank( 0x8000, bank_32k, data >> 4 );

		return true;
	}

	uint8_t regs;
};
