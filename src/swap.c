/*
 * FRANK OS — App Suspension with PSRAM Swapping
 * Copyright (c) 2025 Mikhail Matveev <xtreme@rh1.tech>
 * https://rh1.tech
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "swap.h"
#include "window.h"
#include "window_event.h"
#include "cmd.h"
#include "taskbar.h"
#include "../drivers/psram/psram.h"
#include <string.h>
#include <stdio.h>

/*==========================================================================
 * Shared SRAM stack — all foreground app tasks use this single block.
 * Only one runs at a time; on switch, the 8KB is saved to PSRAM.
 *=========================================================================*/

static StackType_t shared_stack[SWAP_STACK_WORDS] __attribute__((aligned(8)));

/* True when the shared stack is actively in use by a running task.
 * Prevents a second foreground task from corrupting the first. */
static volatile bool shared_stack_busy;

bool swap_is_stack_busy(void) {
    return shared_stack_busy;
}

void swap_stack_acquire(void) {
    shared_stack_busy = true;
}

void swap_stack_release(void) {
    shared_stack_busy = false;
}

/*==========================================================================
 * Swap table — one entry per registered app window
 *=========================================================================*/

typedef struct {
    hwnd_t        hwnd;
    TaskHandle_t  task;
    StackType_t  *psram_save;   /* 8KB PSRAM buffer for saved stack */
    bool          suspended;
    bool          background;   /* never suspend (e.g. FrankAmp) */
} swap_entry_t;

#define SWAP_MAX_APPS  WM_MAX_WINDOWS
static swap_entry_t swap_table[SWAP_MAX_APPS];
static hwnd_t       active_fg = HWND_NULL;

/* Back-stack: when app exits, auto-resume the previous one */
#define SWAP_HISTORY_MAX 8
static hwnd_t  fg_history[SWAP_HISTORY_MAX];
static uint8_t fg_history_count = 0;

/* Pending background task — set before the app creates its window,
 * so swap_register() can auto-mark it as background. */
static TaskHandle_t pending_bg_task = NULL;

/* Deferred resume — set by swap_resume_previous() (called from the
 * exiting app task which is still on the shared stack).  The actual
 * memcpy + vTaskResume is done by swap_process_deferred() in the
 * compositor task, which runs on its own stack. */
static volatile hwnd_t deferred_resume_hwnd = HWND_NULL;

/*==========================================================================
 * Internal helpers
 *=========================================================================*/

static swap_entry_t *find_entry(hwnd_t hwnd) {
    if (hwnd == HWND_NULL) return NULL;
    for (int i = 0; i < SWAP_MAX_APPS; i++) {
        if (swap_table[i].hwnd == hwnd && swap_table[i].task != NULL)
            return &swap_table[i];
    }
    return NULL;
}

static swap_entry_t *find_entry_by_task(TaskHandle_t task) {
    if (!task) return NULL;
    for (int i = 0; i < SWAP_MAX_APPS; i++) {
        if (swap_table[i].task == task)
            return &swap_table[i];
    }
    return NULL;
}

static void history_push(hwnd_t hwnd) {
    if (hwnd == HWND_NULL) return;
    /* Remove duplicates first */
    for (int i = 0; i < fg_history_count; i++) {
        if (fg_history[i] == hwnd) {
            for (int j = i; j < fg_history_count - 1; j++)
                fg_history[j] = fg_history[j + 1];
            fg_history_count--;
            break;
        }
    }
    /* Push to top */
    if (fg_history_count >= SWAP_HISTORY_MAX) {
        /* Shift down — discard oldest */
        for (int i = 0; i < SWAP_HISTORY_MAX - 1; i++)
            fg_history[i] = fg_history[i + 1];
        fg_history_count = SWAP_HISTORY_MAX - 1;
    }
    fg_history[fg_history_count++] = hwnd;
}

static void history_remove(hwnd_t hwnd) {
    for (int i = 0; i < fg_history_count; i++) {
        if (fg_history[i] == hwnd) {
            for (int j = i; j < fg_history_count - 1; j++)
                fg_history[j] = fg_history[j + 1];
            fg_history_count--;
            return;
        }
    }
}

/*==========================================================================
 * Public API
 *=========================================================================*/

void swap_init(void) {
    memset(swap_table, 0, sizeof(swap_table));
    /* Fill shared stack with FreeRTOS watermark pattern */
    for (int i = 0; i < SWAP_STACK_WORDS; i++)
        shared_stack[i] = 0xa5a5a5a5;
    active_fg = HWND_NULL;
    fg_history_count = 0;
}

void swap_register(hwnd_t hwnd, TaskHandle_t task) {
    if (hwnd == HWND_NULL || !task) return;

    /* Don't double-register */
    if (find_entry(hwnd)) return;

    /* Find free slot */
    for (int i = 0; i < SWAP_MAX_APPS; i++) {
        if (swap_table[i].task == NULL) {
            swap_table[i].hwnd = hwnd;
            swap_table[i].task = task;
            swap_table[i].suspended = false;
            swap_table[i].background = false;
            /* Allocate 8KB PSRAM save buffer */
            swap_table[i].psram_save = (StackType_t *)psram_alloc(SWAP_STACK_BYTES);
            if (swap_table[i].psram_save)
                memset(swap_table[i].psram_save, 0xa5, SWAP_STACK_BYTES);
            /* Auto-mark background if this task was pre-flagged */
            if (pending_bg_task == task) {
                swap_table[i].background = true;
                pending_bg_task = NULL;
            }
            return;
        }
    }
}

void swap_unregister(hwnd_t hwnd) {
    swap_entry_t *e = find_entry(hwnd);
    if (!e) return;
    history_remove(hwnd);
    if (active_fg == hwnd)
        active_fg = HWND_NULL;
    if (e->psram_save) {
        psram_free(e->psram_save);
        e->psram_save = NULL;
    }
    memset(e, 0, sizeof(*e));
}

void swap_unregister_by_task(TaskHandle_t task) {
    swap_entry_t *e = find_entry_by_task(task);
    if (!e) return;
    history_remove(e->hwnd);
    if (active_fg == e->hwnd)
        active_fg = HWND_NULL;
    if (e->psram_save) {
        psram_free(e->psram_save);
        e->psram_save = NULL;
    }
    memset(e, 0, sizeof(*e));
}

void swap_suspend(hwnd_t hwnd) {
    swap_entry_t *e = find_entry(hwnd);
    if (!e || e->suspended || e->background) return;
    if (!e->psram_save) return;  /* no save buffer — can't suspend */

    /* 1. Suspend task FIRST (stack must be stable before copy) */
    vTaskSuspend(e->task);

    /* 2. Save the shared stack to PSRAM */
    memcpy(e->psram_save, shared_stack, SWAP_STACK_BYTES);

    /* 3. Set suspended flag on the window */
    e->suspended = true;
    shared_stack_busy = false;  /* stack saved to PSRAM — available now */
    window_t *win = wm_get_window(hwnd);
    if (win)
        win->flags |= WF_SUSPENDED;
}

void swap_resume(hwnd_t hwnd) {
    swap_entry_t *e = find_entry(hwnd);
    if (!e || !e->suspended) return;
    if (!e->psram_save) return;

    shared_stack_busy = true;  /* about to restore and run on it */
    /* 1. Restore stack from PSRAM BEFORE resuming */
    memcpy(shared_stack, e->psram_save, SWAP_STACK_BYTES);

    /* 2. Clear suspended flag */
    e->suspended = false;
    window_t *win = wm_get_window(hwnd);
    if (win)
        win->flags &= ~WF_SUSPENDED;

    /* 3. Resume task */
    vTaskResume(e->task);
}

void swap_switch_to(hwnd_t hwnd) {
    if (hwnd == active_fg) return;

    /* If the target is a non-registered window (dialog, child window),
     * don't change the foreground.  This prevents suspending the active
     * app when the user clicks on its own dialog (e.g. file save dialog,
     * find dialog, message box). */
    if (hwnd != HWND_NULL && !find_entry(hwnd)) return;

    /* Cancel any pending deferred resume.  If an app just exited and
     * set deferred_resume_hwnd, but we're now explicitly switching to a
     * new foreground (e.g. launching a new app), the deferred resume
     * must NOT fire — it would memcpy over the shared stack while the
     * new app task is already running on it, corrupting its state. */
    deferred_resume_hwnd = HWND_NULL;

    /* Suspend current foreground (if registered and not background) */
    if (active_fg != HWND_NULL) {
        swap_entry_t *old = find_entry(active_fg);
        if (old && !old->background && !old->suspended) {
            history_push(active_fg);
            swap_suspend(active_fg);
        }
    }

    /* Resume target (if registered and suspended) */
    if (hwnd != HWND_NULL) {
        swap_entry_t *target = find_entry(hwnd);
        if (target && target->suspended) {
            swap_resume(hwnd);
        }
    }

    active_fg = hwnd;
    taskbar_invalidate();
}

bool swap_is_suspended(hwnd_t hwnd) {
    swap_entry_t *e = find_entry(hwnd);
    return e ? e->suspended : false;
}

bool swap_is_background(hwnd_t hwnd) {
    swap_entry_t *e = find_entry(hwnd);
    return e ? e->background : false;
}

void swap_set_background(hwnd_t hwnd) {
    swap_entry_t *e = find_entry(hwnd);
    if (e) e->background = true;
}

void swap_set_background_by_task(TaskHandle_t task) {
    swap_entry_t *e = find_entry_by_task(task);
    if (e) e->background = true;
}

void swap_set_pending_background(TaskHandle_t task) {
    pending_bg_task = task;
}

hwnd_t swap_get_foreground(void) {
    return active_fg;
}

hwnd_t swap_get_previous(void) {
    if (fg_history_count == 0) return HWND_NULL;
    return fg_history[fg_history_count - 1];
}

void swap_resume_previous(void) {
    /* Find the most recent suspended app in the back-stack.
     * We can NOT call swap_resume() here because the caller (the exiting
     * app task) is still running on the shared stack — memcpy would
     * overwrite our own stack frame and hardfault.
     * Instead, set a deferred flag for the compositor to process. */
    while (fg_history_count > 0) {
        hwnd_t prev = fg_history[fg_history_count - 1];
        fg_history_count--;
        swap_entry_t *e = find_entry(prev);
        if (e && e->suspended) {
            deferred_resume_hwnd = prev;
            wm_mark_dirty();  /* wake compositor to process it */
            return;
        }
    }
    active_fg = HWND_NULL;
}

bool swap_process_deferred(void) {
    hwnd_t hwnd = deferred_resume_hwnd;
    if (hwnd == HWND_NULL) return false;
    deferred_resume_hwnd = HWND_NULL;

    swap_entry_t *e = find_entry(hwnd);
    if (e && e->suspended) {
        /* Now safe: compositor runs on its own stack, not shared_stack */
        swap_resume(hwnd);
        active_fg = hwnd;
        wm_set_focus(hwnd);
        taskbar_invalidate();
    }
    return true;
}

void swap_force_close(hwnd_t hwnd) {
    swap_entry_t *e = find_entry(hwnd);
    if (!e) return;

    TaskHandle_t task = e->task;

    /* 1. Get cmd_ctx_t BEFORE deleting the task */
    cmd_ctx_t *ctx = (cmd_ctx_t *)pvTaskGetThreadLocalStoragePointer(task, 0);

    /* 2. Delete the task */
    vTaskDelete(task);

    /* 3. Destroy the window */
    wm_destroy_window(hwnd);

    /* 4. Reset global input handlers */
    extern void set_usb_detached_handler(void*);
    extern void set_scancode_handler(void*);
    extern void set_cp866_handler(void*);
    set_usb_detached_handler(0);
    set_scancode_handler(0);
    set_cp866_handler(0);

    /* 5. Free context (sections, pallocs, etc.) */
    if (ctx) {
        void *mem = ctx->task_mem;
        remove_ctx(ctx);
        /* Defer-free the TCB memory (task is already deleted,
         * but the TCB block may still be referenced briefly) */
        if (mem) {
            extern void task_mem_defer_free(void *ptr);
            task_mem_defer_free(mem);
        }
    }

    /* 6. Unregister from swap table */
    swap_unregister(hwnd);
}

void swap_cancel_deferred(void) {
    deferred_resume_hwnd = HWND_NULL;
}

bool swap_find_by_task(TaskHandle_t task) {
    return find_entry_by_task(task) != NULL;
}

StackType_t *swap_get_shared_stack(void) {
    return shared_stack;
}

uint32_t swap_get_stack_words(void) {
    return SWAP_STACK_WORDS;
}
