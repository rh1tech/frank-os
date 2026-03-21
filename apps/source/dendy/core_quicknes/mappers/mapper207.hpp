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

struct taito_x1005_state_t
{
	uint8_t preg [ 3 ];
	uint8_t creg [ 6 ];
	uint8_t nametable [ 2 ];
};

BOOST_STATIC_ASSERT( sizeof (taito_x1005_state_t) == 11 );

// TaitoX1005

class Mapper207 : public Nes_Mapper, taito_x1005_state_t {
public:
	Mapper207()
	{
		taito_x1005_state_t *state = this;
		register_state( state, sizeof *state );
	}

	virtual void reset_state()
	{ }

	virtual void apply_mapping()
	{
		int i;
		intercept_writes( 0x7EF0, 1 );
		for ( i = 0; i < 3; i++ )
			set_prg_bank( 0x8000 + ( i << 13 ), bank_8k, preg [ i ] );
		for ( i = 0; i < 2; i++ )
			set_chr_bank( 0x0000 + ( i << 11 ), bank_2k, creg [ i ] >> 1);
		for ( i = 0; i < 4; i++ )
			set_chr_bank( 0x1000 + ( i << 10 ), bank_1k, creg [ 2 + i ] );
		mirror_manual( nametable [ 0 ], nametable [ 0 ], nametable [ 1 ], nametable [ 1 ] );
	}

	virtual bool write_intercepted( nes_time_t, nes_addr_t addr, int data )
	{
		if ( addr < 0x7EF0 || addr > 0x7EFF )
			return false;
		
		if ( ( addr & 0x0F ) < 6 )
		{
			creg [ addr & 0x07 ] = data;
			if ( ( addr & 0x0F ) < 2 )
			{
				nametable [ addr & 0x01 ] = data >> 7;
				mirror_manual( nametable [ 0 ], nametable [ 0 ], nametable [ 1 ], nametable [ 1 ] );
				set_chr_bank( ( addr << 11 ) & 0x800, bank_2k, creg [ addr & 0x01 ] >> 1 );
				return true;
			}

			set_chr_bank( 0x1000 | ( ( addr - 0x7EF2 ) << 10 ), bank_1k, creg [ addr & 0x07 ] );
			return true;
		}
		
		addr = ( addr - 0x7EFA ) >> 1;
		preg [ addr ] = data;
		set_prg_bank( 0x8000 | ( addr << 13 ), bank_8k, preg [ addr ] );
		return true;
	}

	virtual void write( nes_time_t, nes_addr_t addr, int data ) { }
};

