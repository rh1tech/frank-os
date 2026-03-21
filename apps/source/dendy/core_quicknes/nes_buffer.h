/*
 * QuickNES - NES emulator core
 * Original author: Shay Green (blargg)
 * https://github.com/libretro/QuickNES_Core
 * SPDX-License-Identifier: LGPL-2.1-or-later
 *
 * Fork maintained as part of MurmNES by Mikhail Matveev.
 * https://rh1.tech | https://github.com/rh1tech/murmnes
 */

#ifndef NES_BUFFER_H
#define NES_BUFFER_H

#include "multi_buffer.h"
class Nes_Apu;

class Nes_Nonlinearizer {
private:
	enum { table_bits = 11 };
	enum { table_size = 1 << table_bits };
	int16_t table [table_size];
	Nes_Apu* apu;
	long accum;
	long prev;
	
	long extra_accum;
	long extra_prev;
public:
	Nes_Nonlinearizer();
	bool enabled;
	void clear();
	void set_apu( Nes_Apu* a ) { apu = a; }
	Nes_Apu* enable( bool, Blip_Buffer* tnd );
	long make_nonlinear( Blip_Buffer& buf, long count );
	void SaveAudioBufferState();
	void RestoreAudioBufferState();
};

class Nes_Buffer : public Multi_Buffer {
public:
	Nes_Buffer();
	~Nes_Buffer();
	
	// Setup APU for use with buffer, including setting its output to this buffer.
	// If you're using Nes_Emu, this is automatically called for you.
	void set_apu( Nes_Apu* apu ) { nonlin.set_apu( apu ); }
	
	// Enable/disable non-linear output
	void enable_nonlinearity( bool = true );
	
	// Blip_Buffer to output other sound chips to
	Blip_Buffer* buffer() { return &buf; }
	
	// See Multi_Buffer.h
	const char *set_sample_rate( long rate, int msec = blip_default_length );

	void clock_rate( long );
	void bass_freq( int );
	void clear();
	channel_t channel( int );
	void end_frame( blip_time_t, bool unused = true );
	long samples_avail() const;
	long read_samples( blip_sample_t*, long );
	
private:
	Blip_Buffer buf;
	Blip_Buffer tnd;
	Nes_Nonlinearizer nonlin;
	friend Multi_Buffer* set_apu( Nes_Buffer*, Nes_Apu* );
public:
	virtual void SaveAudioBufferState();
	virtual void RestoreAudioBufferState();
};

#endif
