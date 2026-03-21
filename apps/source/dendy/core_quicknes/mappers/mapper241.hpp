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
 
// https://www.nesdev.org/wiki/INES_Mapper241

class Mapper241 : public Nes_Mapper {
public:
	Mapper241()
	{
		register_state( &bank, 1 );
	}

	virtual void reset_state()
	{ }

	virtual void apply_mapping()
	{
		enable_sram();
		write( 0, 0, bank );
	}

	virtual void write( nes_time_t, nes_addr_t, int data )
	{
		bank = data;
		set_prg_bank( 0x8000, bank_32k, bank );
	}

	uint8_t bank;
};
