/*
 * QuickNES - NES emulator core
 * Original author: Shay Green (blargg)
 * https://github.com/libretro/QuickNES_Core
 * SPDX-License-Identifier: LGPL-2.1-or-later
 *
 * Fork maintained as part of MurmNES by Mikhail Matveev.
 * https://rh1.tech | https://github.com/rh1tech/murmnes
 */

#ifndef NES_PPU_RENDERING_H
#define NES_PPU_RENDERING_H

#include "nes_ppu_impl.h"

class Nes_Ppu_Rendering : public Nes_Ppu_Impl {
	typedef Nes_Ppu_Impl base;
public:
	Nes_Ppu_Rendering();
	
	int sprite_limit;
	
	uint8_t* host_pixels;
	long host_row_bytes;
	
protected:
	
	long sprite_hit_found; // -1: sprite 0 didn't hit, 0: no hit so far, > 0: y * 341 + x
	void draw_background( int start, int count );
	void draw_sprites( int start, int count );
	
private:

	void draw_scanlines( int start, int count, uint8_t* pixels, long pitch, int mode );
	void draw_background_( int count );
	
	// destination for draw functions; avoids extra parameters
	uint8_t* scanline_pixels; 
	long scanline_row_bytes;
	
	// fill/copy
	void fill_background( int count );
	void clip_left( int count );
	void save_left( int count );
	void restore_left( int count );
	
	// sprites
	enum { max_sprites = 64 };
	uint8_t sprite_scanlines [image_height]; // number of sprites on each scanline
	void draw_sprites_( int start, int count );
	bool sprite_hit_possible( int scanline ) const;
	void check_sprite_hit( int begin, int end );
};

inline Nes_Ppu_Rendering::Nes_Ppu_Rendering()
{
	sprite_limit = 8;
	host_pixels = NULL;
}

inline void Nes_Ppu_Rendering::draw_sprites( int start, int count )
{
	draw_scanlines( start, count, host_pixels + host_row_bytes * start, host_row_bytes, 2 );
}

#endif
