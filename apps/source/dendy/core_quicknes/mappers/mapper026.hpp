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

// Konami VRC6 mapper

// Nes_Emu 0.7.0. http://www.slack.net/~ant/

#include "nes_mapper.h"

typedef Mapper_Vrc6<3> Mapper026;