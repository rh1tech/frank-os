/*
 * QuickNES - NES emulator core
 * Original author: Shay Green (blargg)
 * https://github.com/libretro/QuickNES_Core
 * SPDX-License-Identifier: LGPL-2.1-or-later
 *
 * Fork maintained as part of MurmNES by Mikhail Matveev.
 * https://rh1.tech | https://github.com/rh1tech/murmnes
 */

#ifndef APU_STATE_H
#define APU_STATE_H

#include <stdint.h>
#include "blargg_common.h"

struct apu_state_t
{
	typedef uint8_t env_t [3];
	/*struct env_t {
		uint8_t delay;
		uint8_t env;
		uint8_t written;
	};*/
	
	struct apu_t {
		uint8_t w40xx [0x14]; // $4000-$4013
		uint8_t w4015; // enables
		uint8_t w4017; // mode
		uint16_t frame_delay;
		uint8_t frame_step;
		uint8_t irq_flag;
	} apu;
	
	struct square_t {
		uint16_t delay;
		env_t env;
		uint8_t length_counter;
		uint8_t phase;
		uint8_t swp_delay;
		uint8_t swp_reset;
		uint8_t unused2 [1];
	};
	
	square_t square1;
	square_t square2;
	
	struct triangle_t {
		uint16_t delay;
		uint8_t length_counter;
		uint8_t phase;
		uint8_t linear_counter;
		uint8_t linear_mode;
	} triangle;
	
	struct noise_t {
		uint16_t delay;
		env_t env;
		uint8_t length_counter;
		uint16_t shift_reg;
	} noise;
	
	struct dmc_t {
		uint16_t delay;
		uint16_t remain;
		uint16_t addr;
		uint8_t buf;
		uint8_t bits_remain;
		uint8_t bits;
		uint8_t buf_full;
		uint8_t silence;
		uint8_t irq_flag;
	} dmc;
	
	//uint8_t length_counters [4];
	
	enum { tag = 0x41505552 }; // 'APUR'
	void swap();
};
BOOST_STATIC_ASSERT( sizeof (apu_state_t) == 72 );

#endif
