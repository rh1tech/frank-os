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

struct vrc3_state_t
{
	bool     irq_enable;
	bool     irq_awk;
	uint16_t irq_latch;
	uint16_t irq_counter;

	uint8_t  irq_pending;
	uint16_t next_time;
};

// VRC3

class Mapper073 : public Nes_Mapper, vrc3_state_t {
public:
	Mapper073()
	{
		vrc3_state_t * state = this;
		register_state( state, sizeof * state );
	}

	void reset_state()
	{
	}

	void apply_mapping()
	{
		enable_sram();
		mirror_vert();
	}

	virtual void run_until( nes_time_t end_time )
	{
		if ( irq_enable )
		{
			long counter = irq_counter + ( end_time - next_time );

			if ( counter > 0xFFFF )
			{
				irq_pending = true;
				irq_enable  = irq_awk;
				irq_counter = irq_latch;
			}
			else
				irq_counter = counter;
		}

		next_time = end_time;
	}

	virtual void end_frame( nes_time_t end_time )
	{
		if ( end_time > next_time )
			run_until( end_time );

		next_time -= end_time;
	}

	virtual nes_time_t next_irq( nes_time_t present )
	{
		if ( irq_pending )
			return present;

		if ( !irq_enable )
			return no_irq;

		return 0x10000 - irq_counter + next_time;
	}

	void write_irq_counter( int shift, int data )
	{
		irq_latch &= ~( 0xF << shift );
		irq_latch |= data << shift;
	}

	void write( nes_time_t time, nes_addr_t addr, int data )
	{
		data &= 0xF;

		switch ( addr >> 12 )
		{
			case 0xF: set_prg_bank( 0x8000, bank_16k, data ); break;
			case 0x8: write_irq_counter(  0, data ); break;
			case 0x9: write_irq_counter(  4, data ); break;
			case 0xA: write_irq_counter(  8, data ); break;
			case 0xB: write_irq_counter( 12, data ); break;
			case 0xC:
				irq_pending = false;
				irq_awk     = data & 1;
				irq_enable  = data & 2;

				if ( irq_enable )
					irq_counter = irq_latch;

				break;
			case 0xD:
				irq_pending = false;
				irq_enable  = irq_awk;
				break;
		}

		irq_changed();
	}
};

// void register_vrc3_mapper();
// void register_vrc3_mapper()
// {
// 	register_mapper< Mapper073> ( 73 );
// }
