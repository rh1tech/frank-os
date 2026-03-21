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

// Sunsoft2b

class Mapper089 : public Nes_Mapper {
public:
	Mapper089()
	{
		register_state( &regs, 1 );
	}

	virtual void reset_state()
	{}

	virtual void apply_mapping()
	{
		set_prg_bank( 0xC000, bank_16k, last_bank );
		write( 0, 0x8000, regs );
	}

	virtual void write( nes_time_t, nes_addr_t addr, int data )
	{
		regs = handle_bus_conflict( addr, data );

		set_chr_bank( 0x0000, bank_8k, ( ( data >> 4 ) & 0x08 ) | ( data & 0x07 ) );
		set_prg_bank( 0x8000, bank_16k, ( data >> 4 ) & 0x07 );
		mirror_single( ( data >> 3 ) & 1 );
	}

	uint8_t regs;
};
