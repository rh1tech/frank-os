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

#include <cstring>

#include "nes_mapper.h"
#include "blargg_source.h"

// MMC2 

class Mapper009: public Nes_Mapper
{
	uint8_t regs[6]; // A,B,C,D,E,F

	void mirror(uint8_t val)
	{
		if (val & 1)
			mirror_horiz();
		else
			mirror_vert();
	}

public:
	Mapper009()
	{
		register_state(regs, sizeof(regs));
	}

	virtual void reset_state()
	{
		std::memset(regs, 0, sizeof(regs));
	}

	virtual void apply_mapping()
	{
		mirror(regs[5]);
		set_prg_bank(0x8000, bank_8k, regs[0]);
		set_prg_bank(0xa000, bank_8k, 13);
		set_prg_bank(0xc000, bank_8k, 14);
		set_prg_bank(0xe000, bank_8k, 15);

		set_chr_bank(0x0000, bank_4k, regs[1]);
		set_chr_bank(0x1000, bank_4k, regs[3]);

		set_chr_bank_ex(0x0000, bank_4k, regs[2]);
		set_chr_bank_ex(0x1000, bank_4k, regs[4]);
	}

	virtual void write(nes_time_t, nes_addr_t addr, int data)
	{
		switch (addr >> 12)
		{
		case 0xa: regs[0] = data; set_prg_bank(0x8000, bank_8k, data); break;
		case 0xb: regs[1] = data; set_chr_bank(0x0000, bank_4k, data); break;
		case 0xc: regs[2] = data; set_chr_bank_ex(0x0000, bank_4k, data); break;
		case 0xd: regs[3] = data; set_chr_bank(0x1000, bank_4k, data); break;
		case 0xe: regs[4] = data; set_chr_bank_ex(0x1000, bank_4k, data); break;
		case 0xf: regs[5] = data; mirror(data); break;
		}
	}
};

