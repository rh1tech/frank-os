/*
 * FRANK OS
 * Copyright (c) 2025 Mikhail Matveev <xtreme@rh1.tech>
 * https://rh1.tech
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef SHELL_H
#define SHELL_H

#include "terminal.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Start the built-in shell on a terminal.
 * Spawns a FreeRTOS task that reads input from the terminal and
 * executes commands (ELF apps from SD card or built-in commands).
 * Returns true on success (task created). */
bool shell_start(terminal_t *term);

#ifdef __cplusplus
}
#endif

#endif /* SHELL_H */
