/*
 * QuickNES - NES emulator core
 * Original author: Shay Green (blargg)
 * https://github.com/libretro/QuickNES_Core
 * SPDX-License-Identifier: LGPL-2.1-or-later
 *
 * Fork maintained as part of MurmNES by Mikhail Matveev.
 * https://rh1.tech | https://github.com/rh1tech/murmnes
 */

#ifndef NES_UTIL_H
#define NES_UTIL_H

#include <stdint.h>
#include "blargg_common.h"

class Nes_Emu;
class Nes_Cart;

struct game_genie_patch_t
{
	unsigned addr;    // always 0x8000 or greater
	int change_to;
	int compare_with; // if -1, always change byte
	
	// Decode Game Genie code
	const char *decode( const char* in );
	
	// Apply patch to cartridge data. Might not work for some codes, since this really
	// requires emulator support. Returns number of bytes changed, where 0
	// means patch wasn't for that cartridge.
	int apply( Nes_Cart& ) const;
};

class Cheat_Value_Finder {
public:
	Cheat_Value_Finder();
	
	// Start scanning emulator's memory for values that are constantly changing.
	void start( Nes_Emu* );
	
	// Rescan memory and eliminate any changed bytes from later matching.
	// Should be called many times after begin_scan() and before begin_matching().
	void rescan();
	
	// Start search for any bytes which changed by difference between original and
	// changed values.
	void search( int original, int changed );
	
	// Get next match and return its delta from changed value (closer to 0
	// is more likely to be a match), or no_match if there are no more matches.
	// Optionally returns address of matched byte.
	enum { no_match = 0x100 };
	int next_match( int* addr = NULL );
	
	// Change current match to new value. Returns previous value.
	int change_value( int new_value );
	
private:
	Nes_Emu* emu;
	int original_value;
	int changed_value;
	int pos;
	enum { low_mem_size = 0x800 };
	uint8_t original [low_mem_size];
	uint8_t changed  [low_mem_size];
};

#endif
