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

// NROM-128 4-in-1 multicart

class Mapper060 : public Nes_Mapper {
public:
	Mapper060()
	{
		last_game = 2;
		register_state( &game_sel, 1 );
	}

	virtual void reset_state()
	{
		game_sel = last_game;
		game_sel++;
		game_sel &= 3;
	}

	virtual void apply_mapping()
	{
		set_prg_bank ( 0x8000, bank_16k, game_sel );
		set_prg_bank ( 0xC000, bank_16k, game_sel );
		set_chr_bank ( 0, bank_8k, game_sel );
		last_game = game_sel;
	}

	virtual void write( nes_time_t, nes_addr_t addr, int data ) { }

	uint8_t game_sel, last_game;
};
