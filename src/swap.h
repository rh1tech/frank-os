/*
 * FRANK OS — App Suspension with PSRAM Swapping
 * Copyright (c) 2025 Mikhail Matveev <xtreme@rh1.tech>
 * https://rh1.tech
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef SWAP_H
#define SWAP_H

#include <stdint.h>
#include <stdbool.h>
#include "window.h"
#include "FreeRTOS.h"
#include "task.h"

/* Shared SRAM stack size for app tasks */
#define SWAP_STACK_WORDS 2048
#define SWAP_STACK_BYTES (SWAP_STACK_WORDS * sizeof(StackType_t))  /* 8192 */

/* Initialize the swap manager — call once at boot before taskbar_init() */
void swap_init(void);

/* Register an app window + task for swap management.
 * Allocates an 8KB PSRAM save buffer for stack swapping. */
void swap_register(hwnd_t hwnd, TaskHandle_t task);

/* Unregister by hwnd — frees PSRAM save buffer */
void swap_unregister(hwnd_t hwnd);

/* Unregister by task handle (for cleanup when task exits) */
void swap_unregister_by_task(TaskHandle_t task);

/* Suspend an app: vTaskSuspend → memcpy stack→PSRAM → set WF_SUSPENDED */
void swap_suspend(hwnd_t hwnd);

/* Resume an app: memcpy PSRAM→stack → clear WF_SUSPENDED → vTaskResume */
void swap_resume(hwnd_t hwnd);

/* Suspend old foreground + resume target (single call for focus switch) */
void swap_switch_to(hwnd_t hwnd);

/* Query whether an app is suspended */
bool swap_is_suspended(hwnd_t hwnd);

/* Query whether an app is a background app (never suspended) */
bool swap_is_background(hwnd_t hwnd);

/* Mark an app as background (never suspended, e.g. FrankAmp) */
void swap_set_background(hwnd_t hwnd);
void swap_set_background_by_task(TaskHandle_t task);

/* Mark a task as pending-background.  When swap_register() is called
 * with this task handle, the entry is automatically marked background.
 * Used before the app creates its window. */
void swap_set_pending_background(TaskHandle_t task);

/* Get the current active foreground hwnd */
hwnd_t swap_get_foreground(void);

/* Get the previous foreground hwnd (for auto-resume on exit) */
hwnd_t swap_get_previous(void);

/* Request deferred resume of previous app from back-stack.
 * Safe to call from an exiting app task (still on the shared stack).
 * The actual memcpy + vTaskResume happens in swap_process_deferred(),
 * called by the compositor task. */
void swap_resume_previous(void);

/* Process any pending deferred resume.  Call from compositor_task
 * each frame.  Returns true if a resume was performed. */
bool swap_process_deferred(void);

/* Cancel any pending deferred resume.  Call before launching a new
 * app to prevent the deferred resume from overwriting the shared
 * stack while the new task is running on it. */
void swap_cancel_deferred(void);

/* Force-close a suspended app (delete task, destroy window, free ctx) */
void swap_force_close(hwnd_t hwnd);

/* Check whether a task already has a registered swap entry.
 * Returns true if found (used to prevent double-registration of
 * child windows created by the same task). */
bool swap_find_by_task(TaskHandle_t task);

/* Return the task handle registered for a swap entry (NULL if none) */
TaskHandle_t swap_get_task(hwnd_t hwnd);

/* Return pointer to the shared SRAM stack (for task creation) */
StackType_t *swap_get_shared_stack(void);

/* Return the stack size in words */
uint32_t swap_get_stack_words(void);

/* Shared stack usage tracking — prevents two concurrent foreground tasks
 * from using the same stack buffer (e.g. two terminal shells each running
 * an app).  When the stack is busy, callers should allocate a separate
 * stack (e.g. via PSRAM or heap). */
bool swap_is_stack_busy(void);
void swap_stack_acquire(void);
void swap_stack_release(void);

#endif /* SWAP_H */
