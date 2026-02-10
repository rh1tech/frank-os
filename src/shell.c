#include "shell.h"
#include "terminal.h"
#include "app.h"
#include "cmd.h"
#include "elf32.h"
#include "FreeRTOS.h"
#include "task.h"
#include "sdcard_init.h"
#include "ff.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#define SHELL_MAX_LINE  256
#define SHELL_MAX_ARGS  16

static int next_shell_id = 0;

/* Recursively remove a directory and all its contents (FatFS) */
static void rm_rf(const char *path) {
    DIR dir;
    FILINFO fno;
    if (f_opendir(&dir, path) != FR_OK) return;
    char child[64];
    while (f_readdir(&dir, &fno) == FR_OK && fno.fname[0]) {
        snprintf(child, sizeof(child), "%s/%s", path, fno.fname);
        if (fno.fattrib & AM_DIR)
            rm_rf(child);
        else
            f_unlink(child);
    }
    f_closedir(&dir);
    f_unlink(path);
}

/* Helper: get this shell's terminal via TLS */
static inline terminal_t *my_term(void) {
    return terminal_get_active();
}

/*==========================================================================
 * Built-in commands
 *=========================================================================*/

static void cmd_help(int argc, char **argv) {
    (void)argc; (void)argv;
    terminal_t *t = my_term();
    terminal_puts(t, "Built-in commands:\n");
    terminal_puts(t, "  ls [dir]   - list files\n");
    terminal_puts(t, "  cd <dir>   - change directory\n");
    terminal_puts(t, "  pwd        - print working directory\n");
    terminal_puts(t, "  clear      - clear screen\n");
    terminal_puts(t, "  free       - show heap info\n");
    terminal_puts(t, "  mount      - retry SD card mount\n");
    terminal_puts(t, "  help       - this message\n");
    terminal_puts(t, "  reboot     - reboot system\n");
    terminal_puts(t, "\nOther commands run as ELF apps from SD card.\n");
}

static void cmd_clear(int argc, char **argv) {
    (void)argc; (void)argv;
    terminal_clear(my_term(), 0 /* COLOR_BLACK */);
}

static void cmd_free_cmd(int argc, char **argv) {
    (void)argc; (void)argv;
    terminal_t *t = my_term();
    size_t free_sz = xPortGetFreeHeapSize();
    size_t total = configTOTAL_HEAP_SIZE;
    terminal_printf(t, "Heap: %u / %u bytes free (%u%% used)\n",
                    (unsigned)free_sz, (unsigned)total,
                    (unsigned)((total - free_sz) * 100 / total));
}

static void cmd_ls(int argc, char **argv) {
    terminal_t *t = my_term();
    if (!sdcard_is_mounted()) {
        terminal_puts(t, "No SD card\n");
        return;
    }
    const char *path;
    cmd_ctx_t *ctx = get_cmd_ctx();
    if (argc > 1) {
        path = argv[1];
    } else {
        path = get_ctx_var(ctx, "CD");
        if (!path) path = "/";
    }
    DIR dir;
    FILINFO fno;
    FRESULT fr = f_opendir(&dir, path);
    if (fr != FR_OK) {
        terminal_printf(t, "Cannot open '%s' (%d)\n", path, fr);
        return;
    }
    int count = 0;
    for (;;) {
        fr = f_readdir(&dir, &fno);
        if (fr != FR_OK) {
            terminal_printf(t, "(readdir err %d)\n", fr);
            break;
        }
        if (fno.fname[0] == 0) break;
        if (fno.fattrib & AM_DIR) {
            terminal_printf(t, "  [%s]\n", fno.fname);
        } else {
            terminal_printf(t, "  %-20s %lu\n", fno.fname, (unsigned long)fno.fsize);
        }
        count++;
    }
    f_closedir(&dir);
    terminal_printf(t, "%d item(s)\n", count);
}

static void cmd_cd(int argc, char **argv) {
    terminal_t *t = my_term();
    if (argc < 2) {
        terminal_puts(t, "Usage: cd <directory>\n");
        return;
    }
    if (!sdcard_is_mounted()) {
        terminal_puts(t, "No SD card\n");
        return;
    }
    DIR dir;
    FRESULT fr = f_opendir(&dir, argv[1]);
    if (fr != FR_OK) {
        terminal_printf(t, "Cannot open '%s' (%d)\n", argv[1], fr);
        return;
    }
    f_closedir(&dir);
    cmd_ctx_t *ctx = get_cmd_ctx();
    set_ctx_var(ctx, "CD", argv[1]);
}

static void cmd_pwd(int argc, char **argv) {
    (void)argc; (void)argv;
    cmd_ctx_t *ctx = get_cmd_ctx();
    char *cd = get_ctx_var(ctx, "CD");
    terminal_printf(my_term(), "%s\n", cd ? cd : "/");
}

static void cmd_mount(int argc, char **argv) {
    (void)argc; (void)argv;
    terminal_t *t = my_term();
    if (sdcard_is_mounted()) {
        terminal_puts(t, "SD card already mounted\n");
        return;
    }
    terminal_puts(t, "Mounting SD card...\n");
    if (sdcard_mount()) {
        terminal_puts(t, "SD card mounted OK\n");
    } else {
        terminal_printf(t, "Mount failed (err %d)\n",
                        (int)sdcard_last_error());
    }
}

static void cmd_reboot(int argc, char **argv) {
    (void)argc; (void)argv;
    terminal_puts(my_term(), "Rebooting...\n");
    reboot_me();
}

/*==========================================================================
 * Line reading — reads from terminal character by character
 *=========================================================================*/

static int shell_readline(terminal_t *t, char *buf, int maxlen) {
    int pos = 0;
    for (;;) {
        int ch = terminal_getch(t);
        if (ch < 0) continue;

        if (ch == '\n' || ch == '\r') {
            terminal_putc(t, '\n');
            buf[pos] = 0;
            return pos;
        } else if (ch == '\b' || ch == 0x7F) {
            if (pos > 0) {
                pos--;
                terminal_putc(t, '\b');
            }
        } else if (ch >= 0x20 && pos < maxlen - 1) {
            buf[pos++] = (char)ch;
            terminal_putc(t, (char)ch);
        }
    }
}

/*==========================================================================
 * Command line parsing
 *=========================================================================*/

static int shell_parse(char *line, char **argv, int max_args) {
    int argc = 0;
    char *p = line;
    while (*p && argc < max_args) {
        /* Skip whitespace */
        while (*p == ' ' || *p == '\t') p++;
        if (*p == 0) break;

        /* Handle quoted strings */
        if (*p == '"') {
            p++;
            argv[argc++] = p;
            while (*p && *p != '"') p++;
            if (*p == '"') *p++ = 0;
        } else {
            argv[argc++] = p;
            while (*p && *p != ' ' && *p != '\t') p++;
            if (*p) *p++ = 0;
        }
    }
    return argc;
}

/*==========================================================================
 * Silent ELF magic check — returns true if file starts with ELF magic.
 * Used in chain loop to skip non-ELF files without printing errors.
 *=========================================================================*/

static bool is_elf_file(const char *path) {
    FIL f;
    if (f_open(&f, path, FA_READ) != FR_OK) return false;
    uint32_t magic = 0;
    UINT rb;
    f_read(&f, &magic, 4, &rb);
    f_close(&f);
    return rb == 4 && magic == ELF_MAGIC;
}

/*==========================================================================
 * Run ELF app from SD card using MOS2 pipeline
 *=========================================================================*/

static void shell_run_elf(terminal_t *t, int argc, char **argv) {
    if (!sdcard_is_mounted()) {
        terminal_puts(t, "No SD card\n");
        return;
    }
    cmd_ctx_t *ctx = get_cmd_startup_ctx();
    extern array_t *pids;

    /* Save original command for COMSPEC-like re-run after app chains.
     * In MOS2, vCmdTask auto-reruns COMSPEC after any chained command
     * finishes (e.g. mc → mcview → back to mc). We replicate that here. */
    char *saved_cmd = copy_str(argv[0]);

    /* Propagate terminal to cmd_ctx so exec pipeline inherits it */
    ctx->term = t;

    /* Set up argc/argv */
    ctx->argc = argc;
    ctx->argv = (char **)pvPortMalloc((argc + 1) * sizeof(char *));
    if (!ctx->argv) {
        terminal_puts(t, "Out of memory\n");
        vPortFree(saved_cmd);
        return;
    }
    for (int i = 0; i < argc; i++) {
        ctx->argv[i] = copy_str(argv[i]);
    }
    ctx->argv[argc] = NULL;
    if (ctx->orig_cmd) vPortFree(ctx->orig_cmd);
    ctx->orig_cmd = copy_str(argv[0]);

    /* Check if file exists (searches CD, BASE, PATH env vars) */
    if (!exists(ctx)) {
        terminal_printf(t, "'%s' not found\n", argv[0]);
        cleanup_ctx(ctx);
        goto done;
    }

    /* Validate as ELF */
    if (!is_new_app(ctx)) {
        terminal_printf(t, "'%s' is not a valid app\n", argv[0]);
        ctx->stage = INVALIDATED;
        cleanup_ctx(ctx);
        goto done;
    }

    /* Load ELF sections into memory */
    if (!load_app(ctx)) {
        terminal_printf(t, "Failed to load '%s'\n", argv[0]);
        ctx->stage = INVALIDATED;
        cleanup_ctx(ctx);
        goto done;
    }

    /* Main exec loop with COMSPEC-like re-run.
     * When an app chains to another (e.g. mc → mcview via cmd_enter_helper),
     * the inner while loop handles the chain. After the chained app exits,
     * we re-run the original command — just like MOS2's vCmdTask reruns
     * COMSPEC after any command chain finishes. */
    for (;;) {
        printf("[shell] calling exec...\n");
        exec(ctx);
        printf("[shell] exec returned, stage=%d ret=%d\n", ctx->stage, ctx->ret_code);

        /* Handle chained commands (e.g. mc launching mcview).
         * ctx->stage == PREPARED means the app set up a follow-on command. */
        bool had_chain = false;
        while (ctx->stage == PREPARED) {
            had_chain = true;
            printf("[shell] chain: orig_cmd='%s'\n",
                   ctx->orig_cmd ? ctx->orig_cmd : "(null)");
            if (!exists(ctx))       { cleanup_ctx(ctx); break; }
            /* Silent ELF check: if the chained file is not an ELF (e.g. a
             * data file that mc tried to "open"), skip it without printing
             * the noisy "It is not an ELF file" message from is_new_app. */
            if (!is_elf_file(ctx->orig_cmd)) {
                printf("[shell] chain: '%s' is not ELF, skipping\n", ctx->orig_cmd);
                cleanup_ctx(ctx); break;
            }
            if (!is_new_app(ctx))   { cleanup_ctx(ctx); break; }
            if (!load_app(ctx))     { cleanup_ctx(ctx); break; }
            ctx->term = t;  /* re-propagate terminal after cleanup */
            exec(ctx);
        }

        /* Normal exit (no chaining) — return to shell prompt */
        if (!had_chain) break;

        /* Chain finished (e.g. mcview exited after mc launched it).
         * Re-run original command (COMSPEC mechanism from MOS2). */
        printf("[shell] chain ended, re-running '%s'\n", saved_cmd);

        /* Repair context after cleanup_ctx zeroed pids and pallocs */
        pids->p[ctx->pid] = ctx;
        vTaskSetThreadLocalStoragePointer(xTaskGetCurrentTaskHandle(), 0, ctx);

        /* Re-prepare context with original command */
        ctx->argc = 1;
        ctx->argv = (char **)pvPortMalloc(2 * sizeof(char *));
        ctx->argv[0] = copy_str(saved_cmd);
        ctx->argv[1] = NULL;
        if (ctx->orig_cmd) vPortFree(ctx->orig_cmd);
        ctx->orig_cmd = copy_str(saved_cmd);
        ctx->term = t;

        if (!exists(ctx))     { cleanup_ctx(ctx); break; }
        if (!is_new_app(ctx)) { cleanup_ctx(ctx); break; }
        if (!load_app(ctx))   { cleanup_ctx(ctx); break; }
        /* Loop back to exec */
    }

    if (ctx->ret_code != 0) {
        terminal_printf(t, "Exit code: %d\n", ctx->ret_code);
    }

done:
    /* Repair context for shell use — like MOS2's vCmdTask does after exec.
     * cleanup_ctx/exec may have zeroed pids->p[1], which must point to the
     * shell's ctx for subsequent MOS2 API calls to work. */
    pids->p[1] = ctx;
    vTaskSetThreadLocalStoragePointer(xTaskGetCurrentTaskHandle(), 0, ctx);
    vPortFree(saved_cmd);
    printf("[shell] shell_run_elf returning\n");
}

/*==========================================================================
 * Shell task — main loop
 *=========================================================================*/

static void shell_task(void *pv) {
    terminal_t *t = (terminal_t *)pv;
    char line[SHELL_MAX_LINE];
    char *argv[SHELL_MAX_ARGS];

    /* Store terminal in this task's TLS so terminal_get_active() finds it */
    terminal_set_task_terminal(t);

    /* Allocate a per-shell cmd_ctx (each terminal needs its own) */
    const TaskHandle_t th = xTaskGetCurrentTaskHandle();
    extern array_t *pids;
    cmd_ctx_t *ctx = (cmd_ctx_t *)pvPortCalloc(1, sizeof(cmd_ctx_t));
    ctx->task = th;
    ctx->ppid = 0;
    ctx->term = t;
    vTaskSetThreadLocalStoragePointer(th, 0, ctx);

    /* Initialize pids array on first shell; assign unique pid for each */
    if (!pids) {
        pids = new_array_v(0, 0, 0);
        array_push_back(pids, 0);     /* index 0 unused */
    }
    int pid = (int)pids->size;
    array_push_back(pids, ctx);
    ctx->pid = pid;
    ctx->pgid = pid;
    ctx->sid = pid;

    /* Create per-shell temp directory */
    int shell_id = next_shell_id++;
    char tmpdir[24];
    snprintf(tmpdir, sizeof(tmpdir), "/tmp/%d", shell_id);
    f_mkdir(tmpdir);

    /* Set default env vars */
    set_ctx_var(ctx, "CD", "/");
    set_ctx_var(ctx, "BASE", "/");
    set_ctx_var(ctx, "PATH", "/mos2");
    set_ctx_var(ctx, "TEMP", tmpdir);

    /* Show welcome */
    terminal_puts(t, "MOS\n");
    if (sdcard_is_mounted()) {
        terminal_puts(t, "SD card: mounted\n");
    } else {
        terminal_printf(t, "SD card: not mounted (err %d)\n",
                        (int)sdcard_last_error());
    }
    terminal_puts(t, "Type 'help' for commands.\n\n");

    for (;;) {
        /* Check if terminal is closing */
        if (t->closing) break;

        /* Show prompt with current directory */
        char *cd = get_ctx_var(ctx, "CD");
        printf("[shell] prompt cd='%s'\n", cd ? cd : "(null)");
        terminal_printf(t, "%s> ", cd ? cd : "FOS");
        printf("[shell] waiting for input...\n");

        /* Read a line */
        int len = shell_readline(t, line, sizeof(line));
        if (len == 0) continue;

        /* Check if terminal is closing */
        if (t->closing) break;

        /* Parse into argv */
        int argc = shell_parse(line, argv, SHELL_MAX_ARGS);
        if (argc == 0) continue;

        /* Check built-in commands */
        if (strcmp(argv[0], "help") == 0) {
            cmd_help(argc, argv);
        } else if (strcmp(argv[0], "clear") == 0 || strcmp(argv[0], "cls") == 0) {
            cmd_clear(argc, argv);
        } else if (strcmp(argv[0], "free") == 0) {
            cmd_free_cmd(argc, argv);
        } else if (strcmp(argv[0], "ls") == 0 || strcmp(argv[0], "dir") == 0) {
            cmd_ls(argc, argv);
        } else if (strcmp(argv[0], "cd") == 0) {
            cmd_cd(argc, argv);
        } else if (strcmp(argv[0], "pwd") == 0) {
            cmd_pwd(argc, argv);
        } else if (strcmp(argv[0], "mount") == 0) {
            cmd_mount(argc, argv);
        } else if (strcmp(argv[0], "reboot") == 0) {
            cmd_reboot(argc, argv);
        } else {
            /* Try to run as ELF from SD card */
            shell_run_elf(t, argc, argv);
        }
    }

    /* Shell exiting — clean up per-shell temp directory and destroy terminal */
    printf("[shell] shell task exiting, cleaning up %s\n", tmpdir);
    rm_rf(tmpdir);
    terminal_destroy(t);
    vTaskDelete(NULL);
}

/*==========================================================================
 * Public API
 *=========================================================================*/

void shell_start(terminal_t *term) {
    xTaskCreate(shell_task, "shell", 4096, (void *)term, 1, NULL);
}
