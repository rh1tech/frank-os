/*
 * Originally from Murmulator OS 2 by DnCraptor
 * https://github.com/DnCraptor/murmulator-os2
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <time.h>
#include <hardware/clocks.h>
#include <hardware/flash.h>
#include <pico/multicore.h>
#include <pico/stdlib.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "FreeRTOS.h"
#include "task.h"
#include "sys_table.h"
#include "graphics.h"
#include "window.h"
#include "window_event.h"
#include "window_draw.h"
#include "menu.h"
#include "dialog.h"
#include "taskbar.h"
#include "filemanager.h"
#include "ff.h"
#include "hooks.h"
#include "portable.h"
#include "timers.h" // TODO
#include "ps2.h"
#include "app.h"
#include "cmd.h"
#include "psram_spi.h"
#include "math-wrapper.h"
#include "ram_page.h"
#include "overclock.h"
#include "hardfault.h"
#include "keyboard.h"
#include "usb.h"
#include "nespad.h"
#include "sound.h"
#include <math.h>

#include "sys/fcntl.h"
#include "sys/ioctl.h"
#include "sys/stat.h"
#include "unistd.h"
#include "dirent.h"
#include "errno.h"
#include "poll.h"

#include "__stdio.h"
#include "__stdlib.h"
#include "__getopt.h"
#include "__libgen.h"
#include "spawn.h"
#include "sys/wait.h"
#include "signal.h"

#include "fts.h"

// TODO: think about it
//extern int __cxa_pure_virtual();

FATFS* get_mount_fs(); // only one FS is supported foe now

// to cleanup BOOTA memory region on the MOS flashing
///unsigned long __in_boota() __aligned(4096) cleanup_boota[] = { 0 };

/* Defensive qsort wrapper: validate comparison function pointer before calling.
 * Catches out-of-bounds function-table lookups (e.g. stale .mc.sav with
 * incompatible struct layout producing a wild sort_type index). */
static void safe_qsort(void *base, size_t nmemb, size_t size,
                        int (*compar)(const void *, const void *)) {
    uintptr_t p = (uintptr_t)(void *)compar;
    /* Valid code lives in flash (0x10000000) or SRAM (0x20000000) */
    bool ok = (p >= 0x10000000u && p < 0x10400000u)
           || (p >= 0x20000000u && p < 0x20080000u);
    if (!ok) {
        printf("[safe_qsort] BAD cmp=%p — skipping sort\n", (void *)compar);
        return;
    }
    qsort(base, nmemb, size, compar);
}

unsigned long __in_systable() __aligned(4096) sys_table_ptrs[] = {
    // task.h
    xTaskCreate, // 0
    vTaskDelete, // 1
    vTaskDelay,  // 2
    xTaskDelayUntil, // 3
    xTaskAbortDelay, // 4
    uxTaskPriorityGet, // 5
    uxTaskPriorityGetFromISR, // 6
    uxTaskBasePriorityGet, // 7
    uxTaskBasePriorityGetFromISR, // 8
    eTaskGetState, // 9
    vTaskGetInfo, // 10
    vTaskPrioritySet, // 11
    vTaskSuspend, // 12
    vTaskResume, // 13
    xTaskResumeFromISR, // 14
    vTaskSuspendAll, // 15
    xTaskResumeAll, // 16
    xTaskGetTickCount, // 17
    xTaskGetTickCountFromISR, // 18
    uxTaskGetNumberOfTasks, // 19
    pcTaskGetName, // 20
    xTaskGetHandle, // 21
    uxTaskGetStackHighWaterMark, // 22
    vTaskSetThreadLocalStoragePointer, // 23
    pvTaskGetThreadLocalStoragePointer, // 24
    // TODO: hooks support?
    getApplicationMallocFailedHookPtr, // 25
    setApplicationMallocFailedHookPtr, // 26
    getApplicationStackOverflowHookPtr, // 27
    setApplicationStackOverflowHookPtr, // 28
    // #include <stdio.h>
    snprintf, // 29
    // graphics.h
    draw_text, // 30 
    draw_window, // 31
    // 
    __malloc, // 32
    __free, // 33
    //
    graphics_set_mode, // 34
    graphics_lock_buffer, // was graphics_set_buffer 35
    graphics_set_offset, // 36
    0, // graphics_set_palette, // 37
    graphics_set_buffer, // 38
    graphics_set_bgcolor, // 39
    0, // graphics_set_flashmode, // 40
    goutf, // 41
    graphics_set_con_pos, // 42
    graphics_set_con_color, // 43
    clrScr, // 44
    gbackspace, // 45

    f_open, // 46
    f_close, // 47
    f_write, // 48
    f_read, // 49
    f_stat, // 50
    f_lseek, // 51
    f_truncate, // 52
    f_sync, // 53
    f_opendir, // 54
    f_closedir, // 55
    f_readdir, // 56
    f_mkdir, // 57
    f_unlink, // 58
    f_rename, // 59
    strcpy, // 60
    f_getfree, // 61
    //
    strlen, // 62
    strncpy, // 63
    get_leds_stat, // 64
    //
    load_firmware, // 65
    run_app, // 66
    //
    vsnprintf, // 67
    //
    get_stdout, // 68
    get_stderr, // 69
    //
    fgoutf, // 70
    //
    psram_size, // 71
    psram_cleanup, // 72
    write8psram, // 73
    write16psram, // 74
    write32psram, // 75
    read8psram, // 76
    read16psram, // 77
    read32psram, // 78
    //
    __u32u32u32_div, // 79
    __u32u32u32_rem, // 80
    __fff_div, // 81
    __fff_mul, // 82
    __ffu32_mul, // 83
    __ddd_div, // 84
    __ddd_mul, // 85
    __ddu32_mul, // 86
    __ddf_mul, // 87
    __ffu32_div, // 88
    __ddu32_div, // 89
    //
    swap_size, // 90
    swap_base_size, // 91
    swap_base, // 92
    ram_page_read, // 93
    ram_page_read16, // 94
    ram_page_read32, // 95
    ram_page_write, // 96
    ram_page_write16, // 97
    ram_page_write32, // 98
    //
    get_cmd_startup_ctx, // 99
    //
    atoi, // 100
    //
    overclocking, // 101
    overclocking_ex, // 102
    get_overclocking_khz, // 103
    set_overclocking, // 104
    set_sys_clock_pll, // 105
    check_sys_clock_khz, // 106
    //
    next_token, // 107
    //
    strcmp, // 108
    strncmp, // 109
    //
    vPortGetHeapStats, // 110
    get_cpu_ram_size, // 111
    get_cpu_flash_size, // 112
    //
    get_mount_fs, // 113
    f_getfree32, // 114
    //
    get_scancode_handler, // 115
    set_scancode_handler, // 116
    get_cp866_handler, // 117
    set_cp866_handler, // 118
    gbackspace, // 119
    //
    is_new_app, // 120
    run_new_app, // 121
    //
    __getch, // 122
    __putc, // 123
    //
    cleanup_bootb_ctx, // 124
    load_app, // 125
    exec, // 126
    //
    gouta, // 127
    //
    exists, // 128
    concat, // 129
    concat2, // 130
    //
    flash_block, // 131
    get_heap_total, // 132
    swap_pages, // 133
    swap_pages_base, // 134
    swap_page_size, // 135
    //
    xTaskGetCurrentTaskHandle, // 136
    __copy_str, // 137
    get_cmd_ctx, // 138
    cleanup_ctx, // 139
    get_ctx_var, // 140
    set_ctx_var, // 141
    memset, // 142
    clone_ctx, // 143
    remove_ctx, // 144
    //
    xPortGetFreeHeapSize, // 145
    //
    get_console_width, // 146
    get_console_height, // 147
    //
    __getc, // 148
    f_eof, // 149
    f_getc, // 150
    f_open_pipe, // 151
    //
    get_buffer, // 152
    get_buffer_size, // 153
    get_screen_bitness, // 154
    cleanup_graphics, // 155
    install_graphics_driver, // 156
    get_console_bitness, // 157
    get_screen_width, // 158
    get_screen_height, // 159
    get_graphics_driver, // 160
    is_buffer_text, // 161
    graphics_get_mode, // 162
    graphics_is_mode_text, // 163
    set_vga_dma_handler_impl, // 164 (TODO: organize)
    set_vga_clkdiv, // 165
    __calloc, // 166
    memcpy, // 167
    vga_dma_channel_set_read_addr, // 168
    //
    safe_qsort, // 169
    strnlen,  // 170
    flash_do_cmd, // 171
    flash_range_erase, // 172
    flash_range_program, // 173
    flash_get_unique_id, // 174
    multicore_lockout_start_blocking, // 175
    multicore_lockout_end_blocking, // 176
    get_cpu_flash_jedec_id, // 177
    psram_id, // 178
    //
    init_pico_usb_drive, // 179 (better use usb_driver)
    pico_usb_drive_heartbeat, // 180 (better use usb_driver)
    tud_msc_ejected, // 181
    set_tud_msc_ejected, // 182 (better use usb_driver)
    //
    show_logo, // 183
    getch_now, // 184
    //
    usb_driver, // 185
    set_cursor_color, // 186
    //
    nespad_stat, // 187
    graphics_get_default_mode, // 188
    //
    graphics_get_font_table, // 189
    graphics_get_font_width, // 190
    graphics_get_font_height, // 191
    graphics_set_font, // 192
    graphics_set_ext_font, // 193
    //
    blimp, // 194
    graphics_con_x, // 195
    graphics_con_y, // 196
    //
    pcm_setup, // 197
    pcm_cleanup, // 198
    pcm_set_buffer, // 199
    // v.0.2.4
    __trunc, // 200
    __floor, // 201
    __pow, // 202
    __sqrt, // 203
    __sin, // 204
    __cos, // 205
    __tan, // 206
    __atan, // 207
    __log, // 208
    __exp, // 209
    __aeabi_fmul, // 210
    __aeabi_i2f, // 211
    __aeabi_fadd, // 212
    __aeabi_fsub, // 213
    __aeabi_fdiv, // 214
    __aeabi_fcmpge, // 215
    __aeabi_idivmod, // 216
    __aeabi_idiv, // 217
    __aeabi_f2d, // 218
    __aeabi_d2f, // 219
    __aeabi_f2iz, // 220
    __aeabi_fcmplt, // 221
    __aeabi_dsub, // 222
    __aeabi_d2iz, // 223
    __aeabi_fcmpeq, // 224
    __aeabi_fcmpun, // 225
    __aeabi_fcmpgt, // 226
    __aeabi_dcmpge, // 227
    __aeabi_uidiv, // 228
    __aeabi_ui2f, // 229
    __aeabi_f2uiz, // 230
    __aeabi_fcmple, // 231
    memmove, // 232
    // API v.20
    cmd_tab, // 233
    history_steps, // 234
    cmd_enter_helper, // 235
    set_usb_detached_handler, // 236
    op_console, // 237
    f_read_str, // 238
    draw_label, // 239
    draw_box, // 240
    draw_panel, // 241
    draw_button, // 242
    uxTaskGetSystemState, // 243
    kill, // 244
    // API v.21
    __aeabi_dmul, // 245
    __aeabi_ddiv, // 246
    __aeabi_dadd, // 247
    __aeabi_i2d, // 248
    __aeabi_dcmpeq, // 249
    __aeabi_ui2d, // 250
    __aeabi_dcmplt, // 251
    // API v.22
    strcat, // 252
    memcmp, // 253
    reboot_me, // 254
    // API v.23
    free_app_flash, // 255
    __aeabi_d2uiz, // 256
    // API v.24
    powf, // 257
    // API v.25
    __clzsi2, // 258
    __aeabi_lmul, // 259
    // API v.26
    gpio_put, // 260
    time, // 261
    time_us_32, // 262
    time_us_64, // 263
    __aeabi_uldivmod, // 264
    // API v.27 POSIX.1 (Base)
    __openat, // 265
    __close, // 266
    __fstatat, // 267
    __fstat, // 268
    __lstat, // 269
    __read, // 270
    __write, // 271
    __fcntl, // 272
    __dup, // 273
    __dup2, // 274
    __lseek_p,// 275
    __errno_location, // 276
    // missed gcc math
    __aeabi_dcmpun, // 277
    __aeabi_llsr, // 278
    __aeabi_ldivmod, // 279
    __aeabi_l2d, // 280
    __aeabi_ul2d, // 281
    __aeabi_d2lz, // 282
    __aeabi_d2ulz, // 283
    __aeabi_l2f, // 284
    __aeabi_ul2f, // 285
    __aeabi_f2lz, // 286
    __aeabi_f2ulz, // 287
    __muldc3, // 288
    __divdc3, // 289
    __mulsc3, // 290
    __divsc3, // 291
    __ctzsi2, // 292
    __popcountsi2, // 293
    __powisf2, // 294
    __powidf2, // 295
    __aeabi_llsl, // 296
    __aeabi_lasr, // 297
    __aeabi_lcmp, // 298
    // POSIX
    __poll, // 299
    __ioctl, // 300
    __writev, // 301
    __readlinkat, // 302
    __realloc, // 303
    __readv, // 304
    __llseek, // 305
    __unlinkat, // 306
    __renameat, // 307
    // libc
    __vfscanf, // 308
    __vsnprintf, // 309
    __vfprintf, // 310
    __fwritex, // 311
    __fwrite, // 312
    __fopen, // 313
    __fclose, // 314
    __fflush, // 315
    __fread, // 316
    __fputc, // 317
    __rewind, // 318
    __fseek,  // 319
    __fgetc, // 320
    __ungetc, // 321
    __fgets, // 322
    __fseeko,  // 323
    __ftello, // 324
    __fgetpos, // 325
    __fsetpos, // 326
    __feof, // 327
    __ferror, // 328
    __clearerr, // 329
    __freopen, // 330
    __stdin, // 331
    __stdout, // 332
    __stderr, // 333
    __dup3, // 334
    __fputs, // 335
    __vsscanf, // 336
    __perror, // 337
    __setvbuf, // 338
    // POSIZ
    __linkat, // 339
    __symlinkat, // 340
    __realpath, // 341
    __tmpnam, // 342
    __tmpfile, // 343
    // misc
    __getopt, // 344
    &__optarg, // 345
    &__optind, // 346
    &__opterr, // 347
    &__optopt, // 348
    &__optreset,  // 349
    // libc
    __exit, // 350
    __dirname, // 351
    __basename, // 352
    strerror, // 353
    __getprogname, // 354
    __fileno, // 355
    // posix
    __mkdirat, // 356
    // Open BSD
    fts_open, // 357
    fts_close, // 358
    fts_children, // 359
    fts_read, // 360
    fts_set, // 361
    // posix
    __opendir, // 362 TODO: replace usage by __opendirat 373
    __closedir, // 363
    __readdir, // 364
    __rewinddir, // 365
    0, // 366 TODO: replace it
    // driver
    register_driver, // 367
    drivers_info, // 368
    // posix
    __umask, // 369
    __fchdir, // 370
    __dirfd, // 371
    __chdir, // 372
    __opendirat, // 373
    __fchmodat, // 374
    __fchmod, // 375
    __fork, // 376
    __execve, // 377
    __posix_spawn, // 378
    __getpid, // 379
    __waitpid, // 380
    __getppid, // 381
    __setsid, // 382
    __getsid, // 383
    __getgid, // 384
    __setgid, // 385
    __getegid, // 386
    __getuid, // 387
    __geteuid, // 388
    __setuid, // 389
    __seteuid, // 390
    __setegid, // 391
    __getpgid, // 392
    __setpgid, // 393
    __tcgetpgrp, // 394
    __tcsetpgrp, // 395
    __posix_spawnp, // 396
    __getenv, // 397
    __access, // 398
    __kill, // 399
    __signal, // 400
    __raise, // 401
    __sigprocmask, // 402
    __getcwd, // 403
    // FRANK OS GUI API
    wm_create_window, // 404
    wm_destroy_window, // 405
    wm_show_window, // 406
    wm_set_focus, // 407
    wm_get_window, // 408
    wm_set_window_rect, // 409
    wm_invalidate, // 410
    wm_post_event, // 411
    wm_get_client_rect, // 412
    wd_begin, // 413
    wd_end, // 414
    wd_pixel, // 415
    wd_hline, // 416
    wd_vline, // 417
    wd_fill_rect, // 418
    wd_clear, // 419
    wd_rect, // 420
    wd_bevel_rect, // 421
    wd_char_ui, // 422
    wd_text_ui, // 423
    menu_set, // 424
    dialog_show, // 425
    taskbar_invalidate, // 426
    xTimerCreate, // 427
    xTimerGenericCommandFromTask, // 428
    pvTimerGetTimerID, // 429
    xTaskGenericNotify, // 430
    ulTaskGenericNotifyTake, // 431
    wm_set_pending_icon, // 432
    wd_icon_32, // 433
    wd_icon_16, // 434
    menu_popup_show, // 435
    dialog_input_show, // 436
    dialog_input_get_text, // 437
    // TODO:
    0
};
