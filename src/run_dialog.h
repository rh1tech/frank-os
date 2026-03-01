/*
 * FRANK OS — Start > Run dialog
 * Copyright (c) 2025 Mikhail Matveev <xtreme@rh1.tech>
 * https://rh1.tech
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef RUN_DIALOG_H
#define RUN_DIALOG_H

#include <stdbool.h>

/* Open the Run dialog (Start → Run).
 * If the dialog is already open, brings it to the front. */
void run_dialog_open(void);

/* True if the Run dialog window is currently open. */
bool run_dialog_is_open(void);

/* Draw the history dropdown as a screen-coordinate overlay.
 * Called from wm_composite() after all windows are painted. */
void run_dialog_draw_dropdown(void);

/* Poll for deferred actions (browse result, error dialog dismiss).
 * Call from the compositor task loop alongside startmenu_check_pending(). */
void run_dialog_check_pending(void);

#endif /* RUN_DIALOG_H */
