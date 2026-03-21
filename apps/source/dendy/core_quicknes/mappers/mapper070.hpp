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

// Mapper_74x161x162x32

template < int mapperId >
class Mapper_74x161x162x32 : public Nes_Mapper {
public:
	Mapper_74x161x162x32()
	{
		register_state( &bank, 1 );
	}

	virtual void reset_state()
	{
		if ( mapperId == 86 )
			bank = ~0;
	}

	virtual void apply_mapping()
	{
		if ( mapperId == 152 ) write( 0, 0, bank );
		if ( mapperId == 70 ) write( 0, 0, bank );
		if ( mapperId == 86 )
		{
			intercept_writes( 0x6000, 1 );
			write_intercepted( 0, 0x6000, bank );
		}
	}

	virtual bool write_intercepted( nes_time_t, nes_addr_t addr, int data )
	{
		if ( addr != 0x6000 ) return false;
		if ( mapperId == 152 ) return false;
		if ( mapperId == 70 ) return false;

		bank = data;
		set_prg_bank( 0x8000, bank_32k, ( bank >> 4 ) & 0x03 );
		set_chr_bank( 0x0000, bank_8k, ( ( bank >> 4 ) & 0x04 ) | ( bank & 0x03 ) );

		return true;
	}

	virtual void write( nes_time_t, nes_addr_t addr, int data )
	{
		if ( mapperId == 86) return;

		bank = handle_bus_conflict (addr, data );
		set_prg_bank( 0x8000, bank_16k, ( bank >> 4 ) & 0x07 );
		set_chr_bank( 0x0000, bank_8k, bank & 0x0F );
		mirror_single( ( bank >> 7) & 0x01 );
	}

	uint8_t bank;
};

typedef Mapper_74x161x162x32<70> Mapper070;