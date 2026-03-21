/*
 * QuickNES - NES emulator core
 * Original author: Shay Green (blargg)
 * https://github.com/libretro/QuickNES_Core
 * SPDX-License-Identifier: LGPL-2.1-or-later
 *
 * Fork maintained as part of MurmNES by Mikhail Matveev.
 * https://rh1.tech | https://github.com/rh1tech/murmnes
 */

#include "nes_mapper.h"

#pragma once

template < bool multicart >
class Mapper_AveNina : public Nes_Mapper {
public:
	Mapper_AveNina()
	{
		register_state( &regs, 1 );
	}

	void write_regs();

	virtual void reset_state()
	{
		intercept_writes( 0x4000, 0x1000 );
		intercept_writes( 0x5000, 0x1000 );
	}

	virtual void apply_mapping()
	{
		write_intercepted( 0, 0x4100, regs );
	}

	virtual bool write_intercepted( nes_time_t, nes_addr_t addr , int data )
	{
		if ( addr < 0x4100 || addr > 0x5FFF )
			return false;

		 if ( addr & 0x100 )
			regs = data;

		write_regs();
		return true;
	}

	virtual void write( nes_time_t, nes_addr_t addr, int data )
	{
		if ( multicart == 0 &&
			( ( addr == 0x8000 ) || ( addr & 0xFCB0 ) == 0xFCB0 ) )
				set_chr_bank( 0, bank_8k, data & 0x07 );
	}

	uint8_t regs;
};

template < bool multicart >
void Mapper_AveNina< multicart >::write_regs()
{
	if ( multicart == 0 )
	{
		set_prg_bank ( 0x8000, bank_32k, ( regs >> 3 ) & 0x01 );
		set_chr_bank ( 0, bank_8k, regs & 0x07 );
	}
	else
	{
		set_prg_bank ( 0x8000, bank_32k, ( regs >> 3 ) & 0x07 );
		set_chr_bank ( 0x0000, bank_8k, ( ( regs >> 3 ) & 0x08 ) | ( regs & 0x07 ) );
		if ( regs & 0x80 ) mirror_vert();
		else mirror_horiz();
	}
}

typedef Mapper_AveNina<false> Mapper079;
