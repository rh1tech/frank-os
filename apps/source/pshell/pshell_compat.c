/*
 * pshell_compat.c — LittleFS → FatFs wrapper implementations for FRANK OS
 *
 * Copyright (c) 2026 Mikhail Matveev <xtreme@rh1.tech>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "pshell_compat.h"

/* ── Dummy globals (referenced by io.h users but never actually used) ── */
lfs_t fs_lfs;
struct lfs_config fs_cfg;

/* ── Heap boundaries (provide weak symbols for status_cmd) ──────────── */
/* These are normally provided by the linker script.
 * For FRANK OS ELF apps they're not meaningful, so stub them. */
char __heap_start __attribute__((weak));
char __heap_end   __attribute__((weak));

/* ── Forward declarations ──────────────────────────────────────────── */
static void make_xattr_path(char *out, const char *path, uint8_t type);

/* ── Helper: map LittleFS open flags to FatFs mode ─────────────────── */
static BYTE lfs_flags_to_fatfs(int flags) {
    BYTE mode = 0;
    if (flags & LFS_O_RDONLY)  mode |= FA_READ;
    if (flags & LFS_O_WRONLY)  mode |= FA_WRITE;
    if ((flags & LFS_O_CREAT) && (flags & LFS_O_TRUNC))
        mode |= FA_CREATE_ALWAYS;
    else if ((flags & LFS_O_CREAT) && (flags & LFS_O_EXCL))
        mode |= FA_CREATE_NEW;
    else if (flags & LFS_O_CREAT)
        mode |= FA_OPEN_ALWAYS;
    if (flags & LFS_O_APPEND)  mode |= FA_OPEN_APPEND;
    /* If only RDONLY with no other flags, just read */
    if (mode == 0) mode = FA_READ;
    return mode;
}

/* ── File operations ───────────────────────────────────────────────── */

int fs_file_open(lfs_file_t *file, const char *path, int flags) {
    BYTE mode = lfs_flags_to_fatfs(flags);
    FRESULT fr = f_open(file, path, mode);
    return (fr == FR_OK) ? LFS_ERR_OK : -(int)fr;
}

int fs_file_close(lfs_file_t *file) {
    FRESULT fr = f_close(file);
    return (fr == FR_OK) ? LFS_ERR_OK : -(int)fr;
}

int fs_file_read(lfs_file_t *file, void *buf, int size) {
    UINT br = 0;
    FRESULT fr = f_read(file, buf, (UINT)size, &br);
    if (fr != FR_OK) return -(int)fr;
    return (int)br;
}

int fs_file_write(lfs_file_t *file, const void *buf, int size) {
    UINT bw = 0;
    FRESULT fr = f_write(file, buf, (UINT)size, &bw);
    if (fr != FR_OK) return -(int)fr;
    return (int)bw;
}

int fs_file_seek(lfs_file_t *file, int off, int whence) {
    FSIZE_t target;
    if (whence == LFS_SEEK_SET) {
        target = (FSIZE_t)off;
    } else if (whence == LFS_SEEK_CUR) {
        target = f_tell(file) + off;
    } else { /* LFS_SEEK_END */
        target = f_size(file) + off;
    }
    FRESULT fr = f_lseek(file, target);
    if (fr != FR_OK) return -(int)fr;
    return (int)f_tell(file);
}

int fs_file_tell(lfs_file_t *file) {
    return (int)f_tell(file);
}

int fs_file_size(lfs_file_t *file) {
    return (int)f_size(file);
}

int fs_file_rewind(lfs_file_t *file) {
    FRESULT fr = f_lseek(file, 0);
    return (fr == FR_OK) ? LFS_ERR_OK : -(int)fr;
}

int fs_file_truncate(lfs_file_t *file, int size) {
    /* Seek to desired size, then truncate */
    FRESULT fr = f_lseek(file, (FSIZE_t)size);
    if (fr != FR_OK) return -(int)fr;
    fr = f_truncate(file);
    return (fr == FR_OK) ? LFS_ERR_OK : -(int)fr;
}

/* ── Directory operations ──────────────────────────────────────────── */

int fs_dir_open(lfs_dir_t *dir, const char *path) {
    FRESULT fr = f_opendir(dir, path);
    return (fr == FR_OK) ? LFS_ERR_OK : -(int)fr;
}

int fs_dir_close(lfs_dir_t *dir) {
    FRESULT fr = f_closedir(dir);
    return (fr == FR_OK) ? LFS_ERR_OK : -(int)fr;
}

int fs_dir_read(lfs_dir_t *dir, struct lfs_info *info) {
    FILINFO fno;
    FRESULT fr = f_readdir(dir, &fno);
    if (fr != FR_OK) return -(int)fr;
    if (fno.fname[0] == '\0') return 0; /* end of directory */

    info->type = (fno.fattrib & AM_DIR) ? LFS_TYPE_DIR : LFS_TYPE_REG;
    info->size = (uint32_t)fno.fsize;
    strncpy(info->name, fno.fname, sizeof(info->name) - 1);
    info->name[sizeof(info->name) - 1] = '\0';
    return 1; /* positive = entry available */
}

int fs_dir_rewind(lfs_dir_t *dir) {
    FRESULT fr = f_readdir(dir, 0); /* NULL rewinds */
    return (fr == FR_OK) ? LFS_ERR_OK : -(int)fr;
}

/* ── Metadata operations ───────────────────────────────────────────── */

int fs_stat(const char *path, struct lfs_info *info) {
    FILINFO fno;
    FRESULT fr = f_stat(path, &fno);
    if (fr != FR_OK) return -(int)fr;

    info->type = (fno.fattrib & AM_DIR) ? LFS_TYPE_DIR : LFS_TYPE_REG;
    info->size = (uint32_t)fno.fsize;
    strncpy(info->name, fno.fname, sizeof(info->name) - 1);
    info->name[sizeof(info->name) - 1] = '\0';
    return LFS_ERR_OK;
}

int fs_remove(const char *path) {
    FRESULT fr = f_unlink(path);
    if (fr == FR_OK) {
        /* Clean up any sidecar attribute files */
        char xpath[260];
        make_xattr_path(xpath, path, 1);
        f_unlink(xpath); /* ignore errors — sidecar may not exist */
    }
    return (fr == FR_OK) ? LFS_ERR_OK : -(int)fr;
}

int fs_rename(const char *oldpath, const char *newpath) {
    FRESULT fr = f_rename(oldpath, newpath);
    if (fr == FR_OK) {
        /* Rename any sidecar attribute file too */
        char oldxa[260], newxa[260];
        make_xattr_path(oldxa, oldpath, 1);
        make_xattr_path(newxa, newpath, 1);
        f_rename(oldxa, newxa); /* ignore errors */
    }
    return (fr == FR_OK) ? LFS_ERR_OK : -(int)fr;
}

int fs_mkdir(const char *path) {
    FRESULT fr = f_mkdir(path);
    return (fr == FR_OK) ? LFS_ERR_OK : -(int)fr;
}

/* ── Filesystem lifecycle (no-ops — always mounted in FRANK OS) ────── */

int fs_mount(void)   { return LFS_ERR_OK; }
int fs_unmount(void) { return LFS_ERR_OK; }
int fs_load(void)    { return LFS_ERR_OK; }
int fs_unload(void)  { return LFS_ERR_OK; }
int fs_format(void)  { return LFS_ERR_OK; }
int fs_gc(void)      { return LFS_ERR_OK; }

/* ── Extended attributes via sidecar files ─────────────────────────── */
/* LittleFS supports per-file extended attributes; FatFs does not.
 * We emulate them with a tiny sidecar file: "<path>.xa<type>"
 * e.g. fs_setattr("/bin/hello", 1, "exe", 4) writes to "/bin/hello.xa1" */

static void make_xattr_path(char *out, const char *path, uint8_t type) {
    /* Build "<path>.xa<type>" — type is a single digit (0-9) */
    char *d = out;
    while (*path) *d++ = *path++;
    *d++ = '.'; *d++ = 'x'; *d++ = 'a';
    *d++ = '0' + (type % 10);
    *d = '\0';
}

int fs_getattr(const char *path, uint8_t type, void *buffer, uint32_t size) {
    char xpath[260];
    make_xattr_path(xpath, path, type);
    FIL f;
    if (f_open(&f, xpath, FA_READ) != FR_OK)
        return -1;
    UINT br = 0;
    f_read(&f, buffer, (UINT)size, &br);
    f_close(&f);
    return (int)br;
}

int fs_setattr(const char *path, uint8_t type, const void *buffer, uint32_t size) {
    char xpath[260];
    make_xattr_path(xpath, path, type);
    FIL f;
    if (f_open(&f, xpath, FA_WRITE | FA_CREATE_ALWAYS) != FR_OK)
        return -1;
    UINT bw = 0;
    f_write(&f, buffer, (UINT)size, &bw);
    f_close(&f);
    return (bw == size) ? LFS_ERR_OK : -1;
}

int fs_removeattr(const char *path, uint8_t type) {
    char xpath[260];
    make_xattr_path(xpath, path, type);
    f_unlink(xpath);
    return LFS_ERR_OK;
}

/* ── Filesystem statistics ─────────────────────────────────────────── */

int fs_fsstat(struct fs_fsstat_t *stat) {
    FATFS *fatfs = NULL;
    DWORD free_clust = 0;
    FRESULT fr = f_getfree("", &free_clust, &fatfs);
    if (fr != FR_OK || !fatfs) {
        stat->block_size  = 512;
        stat->block_count = 0;
        stat->blocks_used = 0;
        return -(int)fr;
    }
    stat->block_size  = fatfs->csize * FF_MAX_SS;
    stat->block_count = (uint32_t)(fatfs->n_fatent - 2);
    stat->blocks_used = stat->block_count - (uint32_t)free_clust;
    return LFS_ERR_OK;
}

int fs_fs_size(void) {
    struct fs_fsstat_t stat;
    if (fs_fsstat(&stat) < 0) return -1;
    return (int)(stat.blocks_used * stat.block_size);
}

/* ── printf implementation routed through VT100 ───────────────────── */

int vprintf(const char *fmt, va_list ap) {
    char buf[512];
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    for (int i = 0; i < n && buf[i]; i++)
        vt100_putc(buf[i]);
    return n;
}

int printf(const char *fmt, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    for (int i = 0; i < n && buf[i]; i++)
        vt100_putc(buf[i]);
    return n;
}

int putchar(int c) {
    vt100_putc((char)c);
    return c;
}

int puts(const char *s) {
    vt100_puts_nl(s);
    return 0;
}

int getchar(void) {
    return vt100_getch();
}

/* ── __wrap_printf / __wrap_sprintf (called by cc_printf.S) ────────── */
/* The Pico SDK uses --wrap=printf to redirect printf calls.
 * cc_printf.S explicitly calls __wrap_printf and __wrap_sprintf
 * for compiled C programs' printf/sprintf support. */

int __wrap_printf(const char *fmt, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    for (int i = 0; i < n && buf[i]; i++)
        vt100_putc(buf[i]);
    return n;
}

int __wrap_sprintf(char *str, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(str, 0x7FFF, fmt, ap);
    va_end(ap);
    return n;
}
