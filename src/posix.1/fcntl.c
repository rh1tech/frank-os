/*
 * Originally from Murmulator OS 2 by DnCraptor
 * https://github.com/DnCraptor/murmulator-os2
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <FreeRTOS.h>
#include <task.h>

#include "sys/fcntl.h"
#include "sys/stat.h"
#include "errno.h"
#include "unistd.h"
#include "dirent.h"

#include "ff.h"
#include "../../api/m-os-api-c-array.h"

#include <time.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include "sys_table.h"
#include "cmd.h" // cmd_ctx_t* get_cmd_ctx(); char* copy_str(const char* s); // cmd.h

mode_t __umask(mode_t mask) {
    cmd_ctx_t* ctx = get_cmd_ctx();
    mode_t prev = ctx->umask;
    ctx->umask = mask & 0777;
    return prev;
}

// from libc (may be required to have own one?)
char* __realpath(const char *restrict filename, char *restrict resolved);
char* __realpathat(int dfd, const char *restrict filename, char *restrict resolved, int at);

// context-aware free (removes from pallocs tracking list, then frees)
extern void __free(void*);

// easy and fast FNV-1a (32-bit)
uint32_t __in_hfa() get_hash(const char *pathname) {
    uint32_t h = 2166136261u;
    while (*pathname) {
        h ^= (unsigned char)*pathname++;
        h *= 16777619u;
    }
    return h;
}

static int __in_hfa() is_leap_year(int year) {
    return (year % 4 == 0 && (year % 100 != 0 || year % 400 == 0));
}

static const int days_in_month[12] = { 31,28,31,30,31,30,31,31,30,31,30,31 };

// TODO: optimize it
time_t __in_hfa() simple_mktime(struct tm *t) {
    int year = t->tm_year + 1900;
    int mon = t->tm_mon;   // 0-11
    int day = t->tm_mday;  // 1-31
    int i;
    time_t seconds = 0;

    // add seconds for years since 1970
    for (i = 1970; i < year; ++i) {
        seconds += 365 * 24*3600;
        if (is_leap_year(i)) seconds += 24*3600;
    }

    // add seconds for months this year
    for (i = 0; i < mon; ++i) {
        seconds += days_in_month[i] * 24*3600;
        if (i == 1 && is_leap_year(year)) seconds += 24*3600; // Feb in leap year
    }

    // add seconds for days
    seconds += (day-1) * 24*3600;

    // add hours, minutes, seconds
    seconds += t->tm_hour * 3600;
    seconds += t->tm_min  * 60;
    seconds += t->tm_sec;

    return seconds;
}

static time_t __in_hfa() fatfs_to_time_t(WORD fdate, WORD ftime) {
    struct tm t;
    // FAT date: bits 15–9 year since 1980, 8–5 month, 4–0 day
    t.tm_year = ((fdate >> 9) & 0x7F) + 80; // years since 1900
    t.tm_mon  = ((fdate >> 5) & 0x0F) - 1;  // months 0–11
    t.tm_mday = fdate & 0x1F;

    // FAT time: bits 15–11 hour, 10–5 min, 4–0 sec/2
    t.tm_hour = (ftime >> 11) & 0x1F;
    t.tm_min  = (ftime >> 5) & 0x3F;
    t.tm_sec  = (ftime & 0x1F) * 2;

    t.tm_isdst = 0; // no DST info in FAT
    return simple_mktime(&t); // mktime(&t);
}

static posix_link_t* posix_links = 0;
static size_t posix_links_cnt = 0;

static posix_link_t* __in_hfa() posix_add_link(
    uint32_t hash,
    const char* path, 
    char type, 
    uint32_t ohash, // means mode for 'O' case
    const char* opath, 
    bool allocated
) {
    // goutf("[posix_add_link] %c %s [%o]\n", type, path, ohash);
    posix_link_t* lnk = (posix_link_t*)pvPortMalloc((posix_links_cnt + 1) * sizeof(posix_link_t));
    if (!lnk) return lnk;
    if (posix_links) {
        memcpy(lnk, posix_links, posix_links_cnt * sizeof(posix_link_t));
        vPortFree(posix_links);
        posix_links = lnk;
        lnk += posix_links_cnt;
    } else {
        posix_links = lnk;
    }
    ++posix_links_cnt;
    lnk->type = type;
    lnk->hash = hash;
    lnk->fname = allocated ? path : copy_str(path);
    if (type == 'H') {
        lnk->hlink.ohash = ohash;
        lnk->hlink.ofname = allocated ? opath : copy_str(opath);
    } else {
        lnk->desc.mode = type == 'S' ? (S_IFLNK | 0777) : ohash;
        lnk->desc.owner = 0; // no group/owner support for now
    }
    return lnk;
}

static FRESULT __in_hfa() extfs_flush() {
    FIL* pf = (FIL*)pvPortMalloc(sizeof(FIL));
    if (!pf) { errno = ENOMEM; return -1; }
    FRESULT r = FR_OK;
    vTaskSuspendAll();
    posix_link_t* lnk = posix_links;
    if (!lnk) {
        goto ok;
    }
    r = f_open(pf, "/.extfs", FA_CREATE_ALWAYS | FA_WRITE);
    if (r != FR_OK) goto ex;
    for (uint32_t i = 0; i < posix_links_cnt; ++i, ++lnk) {
        UINT bw;
        r = f_write(pf, &lnk->type, 1, &bw);
        if (r != FR_OK) goto ex;
        r = f_write(pf, &lnk->hash, sizeof(lnk->hash), &bw);
        if (r != FR_OK) goto ex;
        uint16_t sz = (uint16_t)strlen(lnk->fname);
        r = f_write(pf, &sz, sizeof(sz), &bw);
        if (r != FR_OK) goto ex;
        r = f_write(pf, lnk->fname, sz, &bw);
        if (r != FR_OK) goto ex;
        if (lnk->type == 'H') {
            r = f_write(pf, &lnk->hlink.ohash, sizeof(lnk->hlink.ohash), &bw);
            if (r != FR_OK) goto ex;
            sz = (uint16_t)strlen(lnk->hlink.ofname);
            r = f_write(pf, &sz, sizeof(sz), &bw);
            if (r != FR_OK) goto ex;
            r = f_write(pf, lnk->hlink.ofname, sz, &bw);
            if (r != FR_OK) goto ex;
        } else {
            r = f_write(pf, &lnk->desc.mode, sizeof(lnk->desc.mode), &bw);
            if (r != FR_OK) goto ex;
        }
    }
ex:
    f_close(pf);
ok:
    vPortFree(pf);
	xTaskResumeAll();
    return r;
}

posix_link_t* __in_hfa() lookup_exact(uint32_t hash, const char* path) {
    posix_link_t* lnk = posix_links;
    if (!lnk) return 0;
    for (uint32_t i = 0; i < posix_links_cnt; ++i, ++lnk) {
        if (lnk->hash == hash && strcmp(path, lnk->fname) == 0) {
            return lnk;
        }
    }
    return 0;
}

static posix_link_t* __in_hfa() lookup_by_orig(uint32_t ohash, const char* opath) {
    posix_link_t* lnk = posix_links;
    if (!lnk) return 0;
    for (uint32_t i = 0; i < posix_links_cnt; ++i, ++lnk) {
        if (lnk->type == 'H' && lnk->hlink.ofname && lnk->hlink.ohash == ohash && strcmp(opath, lnk->hlink.ofname) == 0) {
            return lnk;
        }
    }
    return 0;
}

static void __in_hfa() replace_orig(const char* opath, uint32_t ohash, posix_link_t* rename_to) {
    posix_link_t* lnk = posix_links;
    if (!lnk) return;
    for (uint32_t i = 0; i < posix_links_cnt; ++i, ++lnk) {
        if (lnk->type == 'H' && lnk->hlink.ofname && lnk->hlink.ohash == ohash && strcmp(opath, lnk->hlink.ofname) == 0) {
            lnk->hlink.ohash = rename_to->hash;
            vPortFree(lnk->hlink.ofname);
            lnk->hlink.ofname = copy_str(rename_to->fname);
        }
    }
    return;
}

static uint32_t __in_hfa() posix_unlink(const char* path, uint32_t hash, posix_link_t** rename_to) {
    posix_link_t* lnk = posix_links;
    if (!lnk) return 0; // nothing
    for (uint32_t i = 0; i < posix_links_cnt; ++i, ++lnk) {
        if (lnk->hash == hash && strcmp(path, lnk->fname) == 0) {
            uint32_t omode = S_IFREG | 0777;
            if (lnk->type == 'O' && rename_to) { // original removement (additional handling is required)
                omode = lnk->desc.mode;
                *rename_to = lookup_by_orig(hash, path);
                if (*rename_to) {
                    (*rename_to)->type = 'O';
                    replace_orig(hash, path, *rename_to);
                }
            }
            vPortFree(lnk->fname);
            if (lnk->hlink.ofname) vPortFree(lnk->hlink.ofname);
            if (posix_links_cnt - i > 1) {
                memmove(lnk, lnk + 1, sizeof(posix_link_t) * (posix_links_cnt - i - 1));
            }
            --posix_links_cnt;
            return omode; // flush is required
        }
    }
    return 0; // nothing
}

static bool __in_hfa() is_symlink(const char* path, uint32_t hash) {
    vTaskSuspendAll();
    posix_link_t* lnk = posix_links;
    if (!lnk) goto err; // nothing
    for (uint32_t i = 0; i < posix_links_cnt; ++i, ++lnk) {
        if (lnk->hash == hash && strcmp(path, lnk->fname) == 0) {
            char type = lnk->type;
        	xTaskResumeAll();
            return type == 'S';
        }
    }
err:
	xTaskResumeAll();
    return false; // nothing
}

static FRESULT __in_hfa() append_to_extfs(posix_link_t* lnk) {
    // goutf("[append_to_extfs] %c %s [%o]\n", lnk->type, lnk->fname, lnk->desc.mode);
    FIL* pf = (FIL*)pvPortMalloc(sizeof(FIL));
    if (!pf) { return FR_DISK_ERR; } // ??
    FRESULT r = f_open(pf, "/.extfs", FA_OPEN_ALWAYS | FA_WRITE);
    if (r != FR_OK) goto ex;
    r = f_lseek(pf, f_size(pf));
    if (r != FR_OK) goto err;
    UINT bw;
    r = f_write(pf, &lnk->type, 1, &bw);
    if (r != FR_OK) goto err;
    r = f_write(pf, &lnk->hash, sizeof(lnk->hash), &bw);
    if (r != FR_OK) goto err;
    uint16_t sz = (uint16_t)strlen(lnk->fname);
    r = f_write(pf, &sz, sizeof(sz), &bw);
    if (r != FR_OK) goto err;
    r = f_write(pf, lnk->fname, sz, &bw);
    if (r != FR_OK) goto err;
    r = f_write(pf, &lnk->desc.mode, sizeof(lnk->desc.mode), &bw);
    if (r != FR_OK) goto err;
    if (lnk->type == 'H') {
        sz = (uint16_t)strlen(lnk->hlink.ofname);
        r = f_write(pf, &sz, sizeof(sz), &bw);
        if (r != FR_OK) goto err;
        r = f_write(pf, lnk->hlink.ofname, sz, &bw);
        if (r != FR_OK) goto err;
    }
err:
    f_close(pf);
ex:
    vPortFree(pf);
    return r;
}

static FRESULT __in_hfa() extfs_add_link(
    const char* path,
    uint32_t hash,
    char type,
    uint32_t ohash, // means mode for 'O' type
    const char *opath
) {
    vTaskSuspendAll();
    FRESULT r;
    posix_link_t* lnk = posix_add_link(hash, path, type, ohash, opath, false);
    if (!lnk) { r = FR_DISK_ERR; goto ex; }
    r = append_to_extfs(lnk);
ex:
    xTaskResumeAll();
    return r;
}

static FRESULT __in_hfa() extfs_add_hlink(
    const char *path,
    uint32_t hash,
    const char *opath,
    uint32_t ohash,
    uint32_t omode
) {
    FRESULT r;
    vTaskSuspendAll();
    bool found_orig = false;
    posix_link_t* lnk = posix_links;
    for (uint32_t i = 0; i < posix_links_cnt; ++i, ++lnk) {
        if (lnk->hash == ohash && strcmp(opath, lnk->fname) == 0) {
            if (lnk->type != 'O') {
                r = FR_EXIST;
                goto ex;
            }
            found_orig = true;
            break;
        }
    }
    if (!found_orig) {
        r = extfs_add_link(opath, ohash, 'O', omode, 0);
        if (FR_OK != r) {
            goto ex;
        }
    }
    r = extfs_add_link(path, hash, 'H', ohash, opath);
ex:
	xTaskResumeAll();
    return r;
}

typedef struct FDESC_s {
    FIL* fp;
    unsigned int flags;
    char* path;
} FDESC;

void* __in_hfa() alloc_file(void) {
    FDESC* d = (FDESC*)pvPortCalloc(1, sizeof(FDESC));
    if (!d) return NULL;
    d->fp = (FIL*)pvPortCalloc(1, sizeof(FIL));
    if (!d->fp) {
        vPortFree(d);
        return NULL;
    }
    return d;
}

void __in_hfa() dealloc_file(void* p) {
    if (!p) return;
    FDESC* d = (FDESC*)p;
    if ((intptr_t)d->fp > STDERR_FILENO) vPortFree(d->fp);
    if (d->path) vPortFree(d->path);
    vPortFree(p);
}

void* __in_hfa() alloc_dir(void) {
    return pvPortCalloc(1, sizeof(DIR));
}

void __in_hfa() dealloc_dir(void* p) {
    if (!p) return;
    if (((DIR*)p)->dirent) {
        vPortFree(((DIR*)p)->dirent);
    }
    if (((DIR*)p)->dirname) {
        vPortFree(((DIR*)p)->dirname);
    }
    vPortFree(p);
}

void __in_hfa() init_pfiles(cmd_ctx_t* ctx) {
    static volatile bool posix_links_initialized = 0;
    if (!posix_links_initialized) {
        FIL* pf = (FIL*)pvPortMalloc(sizeof(FIL));
        if (!pf) { errno = ENOMEM; return; }
        posix_links_initialized = 1;
        vTaskSuspendAll();
        FRESULT r = f_open(pf, "/.extfs", FA_READ);
        if (r == FR_OK) {
            UINT br;
            char type;
            uint32_t hash;
            uint16_t strsize;
            while(!f_eof(pf)) {
                if (f_read(pf, &type, 1, &br) != FR_OK || br != 1) break;
                if (f_read(pf, &hash, sizeof(hash), &br) != FR_OK || br != sizeof(hash)) break;
                if (f_read(pf, &strsize, sizeof(strsize), &br) != FR_OK || br != sizeof(strsize) || strsize < 2) break;
                char* buf = pvPortMalloc(strsize + 1);
                if (!buf) break;
                buf[strsize] = 0;
                if (f_read(pf, buf, strsize, &br) != FR_OK || strsize != br) {
                    vPortFree(buf);
                    break;
                }
                uint32_t ohash = 0; // mode for 'O' case
                if (f_read(pf, &ohash, sizeof(ohash), &br) != FR_OK || br != sizeof(ohash)) goto brk2;
                char* obuf = 0;
                if (type == 'H') {
                    if (f_read(pf, &strsize, sizeof(strsize), &br) != FR_OK || br != sizeof(strsize) || strsize < 2) goto brk1;
                    obuf = (char*)pvPortMalloc(strsize + 1);
                    if (obuf) {
                        obuf[strsize] = 0;
                        if (f_read(pf, obuf, strsize, &br) != FR_OK || strsize != br) {
                            brk1: vPortFree(obuf);
                            brk2: vPortFree(buf);
                            break;
                        }
                    }
                }
                posix_add_link(hash, buf, type, ohash, obuf, true);
            }
            f_close(pf);
        }
        vPortFree(pf);
    	xTaskResumeAll();
    }
    if (!ctx || ctx->pfiles) return;
    ctx->pfiles = new_array_v(alloc_file, dealloc_file, NULL);
    ctx->pdirs = new_array_v(alloc_dir, dealloc_dir, NULL);

    // W/A for predefined file descriptors:
    FDESC* d;
    d = (FDESC*)pvPortCalloc(1, sizeof(FDESC));
    d->fp = (void*)STDIN_FILENO;
    array_push_back(ctx->pfiles, d); // 0 - stdin
    d = (FDESC*)pvPortCalloc(1, sizeof(FDESC));
    d->fp = (void*)STDOUT_FILENO;
    array_push_back(ctx->pfiles, d); // 1 - stdout
    d = (FDESC*)pvPortCalloc(1, sizeof(FDESC));
    d->fp = (void*)STDERR_FILENO;
    array_push_back(ctx->pfiles, d); // 2 - stderr
}

void __in_hfa() cleanup_pfiles(cmd_ctx_t* ctx) {
    if (!ctx || !ctx->pfiles) return;
    for (size_t i = 0; i < ctx->pfiles->size; ++i) {
        FDESC* fd = (FDESC*)array_get_at(ctx->pfiles, i);
        if (!fd) continue;
        FIL* fp = fd->fp;
        if ((intptr_t)fp > STDERR_FILENO) {
            if (fp->pending_descriptors > 0) {
                fp->pending_descriptors--;
                continue;
            }
            if (fp->obj.fs != 0) {
                f_close(fp);
            }
            vPortFree(fp);
        }
        vPortFree(fd);
    }
    vPortFree(ctx->pfiles->p);
    vPortFree(ctx->pfiles);
    ctx->pfiles = 0;
    if (ctx->pdirs) {
        for (size_t i = 0; i < ctx->pdirs->size; ++i) {
            if (ctx->pdirs->p[i]) {
                DIR *pd = (DIR *)ctx->pdirs->p[i];
                dealloc_dir(pd);
            }
        }
        vPortFree(ctx->pdirs->p);
        vPortFree(ctx->pdirs);
        ctx->pdirs = 0;
    }
}

static BYTE __in_hfa() map_flags_to_ff_mode(int flags) {
    BYTE mode = 0;
    if (flags & O_RDWR) {
        mode |= FA_READ | FA_WRITE;
    } else if (flags & O_WRONLY) {
        mode |= FA_WRITE;
    } else { // O_RDONLY
        mode |= FA_READ;
    }
    if (flags & O_CREAT) {
        if (flags & O_EXCL) {
            mode |= FA_CREATE_NEW;      // fail if exists
        } else if (flags & O_TRUNC) {
            mode |= FA_CREATE_ALWAYS;   // create or truncate
        } else {
            mode |= FA_OPEN_ALWAYS;     // create if not exists
        }
    } else if (flags & O_TRUNC) {
        // truncate existing file without creating
        mode |= FA_CREATE_ALWAYS;
    }
    if (flags & O_APPEND) {
        mode |= FA_OPEN_APPEND; // FatFs append flag
    }
    return mode;
}

static int __in_hfa() map_ff_fresult_to_errno(FRESULT fr) {
    switch(fr) {
        case FR_OK:
            return 0;          // ok
        case FR_DISK_ERR:
        case FR_INT_ERR:
        case FR_NOT_READY:
            return EIO;        // Input/output error
        case FR_NO_FILE:
        case FR_NO_PATH:
            return ENOENT;     // File or path not found
        case FR_INVALID_NAME:
            return ENOTDIR;    // Invalid path component
        case FR_DENIED:
        case FR_WRITE_PROTECTED:
            return EACCES;     // Permission denied
        case FR_EXIST:
            return EEXIST;     // File exists
        case FR_TOO_MANY_OPEN_FILES:
            return EMFILE;     // Process limit reached
        case FR_NOT_ENABLED:
        case FR_INVALID_OBJECT:
        case FR_TIMEOUT:
        case FR_LOCKED:
        case FR_NOT_ENOUGH_CORE:
      //  case FR_TOO_MANY_OPEN_FILES: // FatFs specific
            return EINVAL;     // Generic invalid argument
        default:
            return EIO;        // I/O error
    }
}

inline static bool is_closed_desc(const FDESC* fd) {
    if (fd && !fd->fp) return true;
    return fd && (intptr_t)fd->fp > STDERR_FILENO && fd->fp->obj.fs == 0;
}

static FDESC* __in_hfa() array_lookup_first_closed(array_t* arr, size_t* pn) {
    for (size_t i = 3; i < arr->size; ++i) {
        FDESC* fd = (FDESC*)array_get_at(arr, i);
        if (!fd) continue; // placeholder, just skip it
        if (!fd->fp) {
            fd->fp = (FIL*)pvPortCalloc(1, sizeof(FIL));
            *pn = i;
            return fd;
        }
        if (is_closed_desc(fd)) {
            *pn = i;
            return fd;
        }
    }
    return NULL;
}

inline static uint32_t __in_hfa() fatfs_mode_to_posix(BYTE fattrib) {
    uint32_t posix_mode = S_IXUSR | S_IXGRP | S_IXOTH | S_IRUSR | S_IRGRP | S_IROTH;
    if (fattrib & AM_DIR) {
        posix_mode |= S_IFDIR;
    } else {
        posix_mode |= S_IFREG;
    }
    if (!(fattrib & AM_RDO)) {
        posix_mode |= S_IWUSR | S_IWGRP | S_IWOTH;
    }
    return posix_mode;
}
/**
 * openat() — open a file relative to a directory file descriptor
 *
 * Parameters:
 *   dfd   – directory file descriptor (use AT_FDCWD for current directory)
 *   path  – pathname of the file to open
 *   flags – file status flags and access modes (see below)
 *   mode  – permissions to use if a new file is created
 *
 * flags (choose one):
 *   O_RDONLY – open for reading only
 *   O_WRONLY – open for writing only
 *   O_RDWR – open for both reading and writing
 * 
 * Additional flag options (combine with access mode using bitwise OR):
 *   O_CREAT – create the file if it does not exist (requires 'mode')
 *   O_EXCL – with O_CREAT, fail if the file already exists
 *   O_TRUNC – truncate file to zero length if it already exists
 *   O_APPEND – append writes to the end of file
 *   O_NONBLOCK – non-blocking I/O
 *   O_DIRECTORY – fail if the path is not a directory
 *   O_NOFOLLOW – do not follow symbolic links
 *   O_CLOEXEC – set close-on-exec (FD_CLOEXEC) on new descriptor
 *   O_SYNC – write operations wait for completion of file integrity updates
 *   O_DSYNC – write operations wait for data integrity completion
 *   O_TMPFILE – create an unnamed temporary file in the given directory (requires O_RDWR or O_WRONLY)
 * 
 * mode bits (used only with O_CREAT to define new file permissions):
 *   S_IRUSR – read permission for owner
 *   S_IWUSR – write permission for owner
 *   S_IXUSR – execute/search permission for owner
 *   S_IRGRP – read permission for group
 *   S_IWGRP – write permission for group
 *   S_IXGRP – execute/search permission for group
 *   S_IROTH – read permission for others
 *   S_IWOTH – write permission for others
 *   S_IXOTH – execute/search permission for others
 * 
 * Returns:
 *   On success: a new file descriptor (non-negative)
 *   On error:  -1 and errno is set appropriately
 */
int __in_hfa() __openat(int dfd, const char* _path, int flags, mode_t mode) {
    if (flags & O_DIRECTORY) {
        DIR* pd = __opendirat(dfd, _path);
        if (!pd) {
            // errno already poipulated in __opendirat
            return -1;
        }
        return (int)(intptr_t)pd;  // dirfd
    }
    if (!_path) {
        errno = ENOTDIR;
        return -1;
    }
    // TODO: /dev/... , /proc/...
    cmd_ctx_t* ctx = get_cmd_ctx();
    init_pfiles(ctx);
    int realpath_flags = (flags & O_NOFOLLOW) ? AT_SYMLINK_NOFOLLOW : AT_SYMLINK_FOLLOW;
    char* path = __realpathat(dfd, _path, 0, realpath_flags);
    if (!path) return -1; // errno from __realpathat
    size_t n;
    FDESC* fd = array_lookup_first_closed(ctx->pfiles, &n);
    if (!fd) {
        fd = (FDESC*)alloc_file();
        if (!fd) { __free(path); errno = ENOMEM; return -1; }
        n = array_push_back(ctx->pfiles, fd);
        if (n == 0) {
            dealloc_file(fd);
            __free(path);
            errno = ENOMEM;
            return -1;
        }
    }
    FIL*  pf = fd->fp;
    pf->pending_descriptors = 0;
    if (flags & O_CREAT) mode &= ~ctx->umask;
    BYTE ff_mode = map_flags_to_ff_mode(flags);
    FRESULT fr = f_open(pf, path, ff_mode);
    if (fr != FR_OK) {
        __free(path);
        errno = map_ff_fresult_to_errno(fr);
        return -1;
    }
    FILINFO fno;
    fr = f_stat(path, &fno);
    if (fr != FR_OK) {
        f_close(pf);
        __free(path);
        errno = map_ff_fresult_to_errno(fr);
        return -1;
    }
    if (fd->path) __free(fd->path);
    fd->path = path;
    pf->ctime = fatfs_to_time_t(fno.fdate, fno.ftime);
    uint32_t hash = get_hash(path);
    posix_link_t* lnk = lookup_exact(hash, path);
    pf->mode = lnk && lnk->type != 'H' ? lnk->desc.mode : (flags & O_CREAT ? (mode | S_IFREG) : fatfs_mode_to_posix(fno.fattrib));
    if (!lnk) {
        vTaskSuspendAll();
        lnk = posix_add_link(hash, path, 'O', pf->mode, 0, false);
        if (!lnk) {
            xTaskResumeAll();
            f_close(pf);
            errno = ENOMEM;
            return -1;
        }
        FRESULT fr = append_to_extfs(lnk); // TODO:
        xTaskResumeAll();
    }
    errno = 0;
    return (int)n;
}

int __in_hfa() __close(int fildes) {
    cmd_ctx_t* ctx = get_cmd_ctx();
    init_pfiles(ctx);
    FDESC* fd = (FDESC*)array_get_at(ctx->pfiles, fildes);
    if (fd == 0 || is_closed_desc(fd)) {
        goto e;
    }
    FIL* fp = fd->fp;
    if ((intptr_t)fp <= STDERR_FILENO) {  // just ignore close request for std descriptors
        errno = 0;
        return 0;
    }
    if (fp->pending_descriptors) {
        --fp->pending_descriptors;
        fd->fp = 0; // it was a pointer to other FIL in other descriptor, so cleanup it, to do not use by FDESC in 2 places
        fd->flags = 0;
        fd->path = 0;
        errno = 0;
        return 0;
    }
    FRESULT fr = f_close(fp);
    errno = map_ff_fresult_to_errno(fr);
    return errno == 0 ? 0 : -1;
e:
    errno = EBADF;
    return -1;
}

int __in_hfa() __fstatat(int dfd, const char *_path, struct stat *buf, int flags) {
    if (!buf || !_path) {
        errno = EFAULT;
        return -1;
    }
    cmd_ctx_t* ctx = get_cmd_ctx();
    init_pfiles(ctx);
    // TODO: devices...
    char* path = __realpathat(dfd, _path, 0, flags & AT_SYMLINK_NOFOLLOW);
    if (!path) {
        return -1;
    }
    if (strcmp(path, "/") == 0) {
        memset(buf, 0, sizeof(struct stat));
        buf->st_mode  = S_IFDIR | 0755;
        buf->st_nlink = 1;
        __free(path);
        errno = 0;
        return 0;
    }
    FILINFO fno;
    FRESULT fr = f_stat(path, &fno);
    if (fr != FR_OK) {
        __free(path);
        errno = map_ff_fresult_to_errno(fr);
        return -1;
    }
    posix_link_t* lnk = lookup_exact(get_hash(path), path);
    __free(path);
    buf->st_dev   = 0;                    // no device differentiation in FAT
    buf->st_ino   = 0;                    // FAT has no inode numbers
    buf->st_mode  = lnk && lnk->type != 'H' ? lnk->desc.mode : fatfs_mode_to_posix(fno.fattrib);
    buf->st_nlink = 1;                    // FAT does not support hard links
    buf->st_uid   = 0;                    // no UID support for now
    buf->st_gid   = 0;                    // no GID support for now
    buf->st_rdev  = 0;                    // no special device info for now
    buf->st_size  = fno.fsize;            // file size in bytes
    // timestamps (FatFs has date/time in local format, and no .extfs support for now)
    // convert FAT timestamps to time_t
    buf->st_mtime = fatfs_to_time_t(fno.fdate, fno.ftime);
    buf->st_atime = buf->st_mtime;        // FAT does not track last access reliably
    buf->st_ctime = buf->st_mtime;
    errno = 0;
    return 0;
}

int __in_hfa() __access(const char *pathname, int mode)
{
    if (!pathname) {
        errno = EFAULT;
        return -1;
    }

    /* Допустимы только R_OK/W_OK/X_OK; F_OK == 0 и в маске не участвует */
    if (mode & ~(R_OK | W_OK | X_OK)) {
        errno = EINVAL;
        return -1;
    }

    struct stat st;
    if (__stat(pathname, &st) < 0) {
        /* errno уже выставлен __stat/__fstatat */
        return -1;
    }

    /* F_OK (mode == 0) — существование уже проверили через __stat */
    if ((mode & (R_OK | W_OK | X_OK)) == 0) {
        errno = 0;
        return 0;
    }

    cmd_ctx_t *ctx = get_cmd_ctx();
    uid_t uid = ctx->uid;   /* real uid */
    gid_t gid = ctx->gid;   /* real gid */

    mode_t m = st.st_mode;

    /* uid == 0: читать/писать всегда можно, исполнять — если есть хоть один x-бит */
    if (uid == 0) {
        if (mode & X_OK) {
            if (!(m & (S_IXUSR | S_IXGRP | S_IXOTH))) {
                errno = EACCES;
                return -1;
            }
        }
        errno = 0;
        return 0;
    }

    mode_t rmask, wmask, xmask;

    if (uid == st.st_uid) {
        rmask = S_IRUSR;  wmask = S_IWUSR;  xmask = S_IXUSR;
    } else if (gid == st.st_gid) {
        rmask = S_IRGRP;  wmask = S_IWGRP;  xmask = S_IXGRP;
    } else {
        rmask = S_IROTH;  wmask = S_IWOTH;  xmask = S_IXOTH;
    }

    if ((mode & R_OK) && !(m & rmask)) {
        errno = EACCES;
        return -1;
    }
    if ((mode & W_OK) && !(m & wmask)) {
        errno = EACCES;
        return -1;
    }
    if ((mode & X_OK) && !(m & xmask)) {
        errno = EACCES;
        return -1;
    }

    errno = 0;
    return 0;
}

int __in_hfa() __stat(const char* _path, struct stat *buf) {
    return __fstatat(AT_FDCWD, _path, buf, AT_SYMLINK_FOLLOW);
}

int __in_hfa() __fstat(int fildes, struct stat *buf) {
    if (!buf) {
        errno = EFAULT;
        return -1;
    }
    if (fildes < 0) {
        goto e;
    }
    memset(buf, 0, sizeof(struct stat));
    cmd_ctx_t* ctx = get_cmd_ctx();
    init_pfiles(ctx);
    FDESC* fd = (FDESC*)array_get_at(ctx->pfiles, fildes);
    if (fd == 0 || is_closed_desc(fd)) {
        goto e;
    }
    FIL* fp = fd->fp;
    if ((intptr_t)fp == STDIN_FILENO) { // stdin
        buf->st_mode  = S_IFCHR | S_IRUSR | S_IRGRP | S_IROTH;
        goto ok;
    }
    if ((intptr_t)fp <= STDERR_FILENO) { // stdout/stderr
        buf->st_mode  = S_IFCHR | S_IWUSR | S_IWGRP | S_IWOTH;
        goto ok;
    }
    buf->st_dev   = 0;                    // no device differentiation in FAT
    buf->st_ino   = 0;                    // FAT has no inode numbers
    buf->st_mode  = fp->mode;
    buf->st_nlink = 1;                    // FAT does not support hard links
    buf->st_uid   = 0;                    // no UID/GID in FAT
    buf->st_gid   = 0;
    buf->st_rdev  = 0;                    // no special device info
    buf->st_size  = f_size(fp);           // file size in bytes
    // timestamps (FatFs has date/time in local format)
    // convert FAT timestamps to time_t
    buf->st_mtime = fp->ctime;
    buf->st_ctime = fp->ctime;
    buf->st_atime = fp->ctime;
ok:
    errno = 0;
    return 0;
e:
    errno = EBADF;
    return -1;
}

int __in_hfa() __lstat(const char *_path, struct stat *buf) {
    if (!buf || !_path) {
        errno = EFAULT;
        return -1;
    }
    cmd_ctx_t* ctx = get_cmd_ctx();
    init_pfiles(ctx);
    char* path = __realpathat(AT_FDCWD, _path, 0, AT_SYMLINK_NOFOLLOW);
    if (!path) {
        // __realpathat has one error codes set
        return -1;
    }
    FILINFO fno;
    FRESULT fr = f_stat(path, &fno);
    if (fr != FR_OK) {
        __free(path);
        errno = map_ff_fresult_to_errno(fr);
        return -1;
    }
    posix_link_t* lnk = lookup_exact(get_hash(path), path);
    buf->st_dev   = 0;                    // no device differentiation in FAT
    buf->st_ino   = 0;                    // FAT has no inode numbers
    buf->st_nlink = 1;                    // FAT does not support hard links
    buf->st_uid   = 0;                    // no UID/GID in FAT
    buf->st_gid   = 0;
    buf->st_rdev  = 0;                    // no special device info
    buf->st_ctime = fatfs_to_time_t(fno.fdate, fno.ftime); // own symlink
    if (!lnk) {
        buf->st_mode = ((fno.fattrib & AM_DIR) ? S_IFDIR : S_IFREG) | 0777;
    } else {
        switch (lnk->type) {
            case 'O': buf->st_mode = lnk->desc.mode; break; // real object (original)
            case 'H': buf->st_mode = S_IFLNK | 0777; break; // hlink (W/A should not be there, because recolved by __realpathat)
            case 'S': { // slink
                buf->st_mode = lnk->desc.mode;
                struct stat obuf;
                if (__stat(path, &obuf) < 0) {
                    __free(path);
                    return -1;
                }
                buf->st_size = obuf.st_size;
                buf->st_mtime = obuf.st_mtime;
                buf->st_atime = obuf.st_atime;
                goto ex;
            }
        }
    }
    buf->st_size  = fno.fsize;            // file size in bytes
    // timestamps (FatFs has date/time in local format)
    buf->st_mtime = buf->st_ctime;
    buf->st_atime = buf->st_mtime;
ex:
    __free(path);
    errno = 0;
    return 0;
}

int __in_hfa() __read(int fildes, void *buf, size_t count) {
    if (!buf) {
        errno = EFAULT;
        return -1;
    }
    if (fildes < 0) {
        e:
        errno = EBADF;
        return -1;
    }
    cmd_ctx_t* ctx = get_cmd_ctx();
    init_pfiles(ctx);
    FDESC* fd = (FDESC*)array_get_at(ctx->pfiles, fildes);
    if (fd == 0 || is_closed_desc(fd)) {
        goto e;
    }
    FIL* fp = fd->fp;
    if ((intptr_t)fp == STDIN_FILENO) {
        char *p = (char*)buf;
        size_t n = 0;
        while (n < count) {
            char c = fd->flags & O_NONBLOCK ? getch_now() : __getch();
            if (c == 0 && (fd->flags & O_NONBLOCK)) {
                if (n > 0) return n;
                errno = EAGAIN;
                return -1;
            }
            p[n++] = c;
            if (n == count) break;
            if (c == '\n' || c == '\r') {
                break;
            }
        }
        return n;
    }

    if (!S_ISREG(fp->mode)) {
        errno = S_ISDIR(fp->mode) ? EISDIR : EINVAL;
        return -1;
    }
    
    if ( fp->mode & (S_IRUSR | S_IRGRP | S_IROTH) ) {
        UINT br;
        FRESULT fr = f_read(fp, buf, count, &br);
        if (fr != FR_OK) {
            errno = map_ff_fresult_to_errno(fr);
            return -1;
        }
        errno = 0;
        return br;
    }
    errno = EACCES;
    return -1;
}

int __in_hfa() __readv(int fd, const struct iovec *iov, int iovcnt) {
    int res = 0;
    for (int i = 0; i < iovcnt; ++i, ++iov) {
        if (iov->iov_len == 0) continue;
        int sz = __read(fd, iov->iov_base, iov->iov_len);
        if (sz < 0) {
            return res > 0 ? res : -1;
        }
        if (sz == 0) break;
        res += sz;
        if (sz < iov->iov_len) break;
    }
    return res;
}

int __in_hfa() __write(int fildes, const void *buf, size_t count) {
    if (!buf) {
        errno = EFAULT;
        return -1;
    }
    if (fildes < 0) {
e:
        errno = EBADF;
        return -1;
    }
    cmd_ctx_t* ctx = get_cmd_ctx();
    init_pfiles(ctx);
    FDESC* fd = (FDESC*)array_get_at(ctx->pfiles, fildes);
    if (fd == 0 || is_closed_desc(fd)) {
        goto e;
    }
    FIL* fp = fd->fp;
    if ((intptr_t)fp == STDIN_FILENO) {
        goto nperm;
    }
    if ((intptr_t)fp <= STDERR_FILENO) {
		// TODO: tune up tty
		char* b = pvPortMalloc(count + 1);
		if (!b) { errno = ENOMEM; return -1; }
		memcpy(b, buf, count);
		b[count] = 0;
		gouta(b);
		vPortFree(b);
        errno = 0;
		return count;
    }
    if (!S_ISREG(fp->mode)) {
        errno = S_ISDIR(fp->mode) ? EISDIR : EINVAL;
        return -1;
    }
    if ( fp->mode & (S_IWUSR | S_IWGRP | S_IWOTH) ) {
        UINT br;
    /// TODO: support devices
        FRESULT fr = f_write(fp, buf, count, &br);
        if (fr != FR_OK) {
            errno = map_ff_fresult_to_errno(fr);
            return -1;
        }
        errno = 0;
        return br;
    }
nperm:
    errno = EACCES;
    return -1;
}

int __in_hfa() __writev(int fd, const struct iovec *iov, int iovcnt) {
    int res = 0;
    for (int i = 0; i < iovcnt; ++i, ++iov) {
        if (iov->iov_len == 0) continue;
        int sz = __write(fd, iov->iov_base, iov->iov_len);
        if (sz < 0) {
            return res > 0 ? res : -1;
        }
        if (sz == 0) break;
        res += sz;
        if (sz < iov->iov_len) break;
    }
    return res;
}

int __in_hfa() __dup(int oldfd) {
    if (oldfd < 0) {
        goto e;
    }
    cmd_ctx_t* ctx = get_cmd_ctx();
    init_pfiles(ctx);
    FDESC* fd = (FDESC*)array_get_at(ctx->pfiles, oldfd);
    if (fd == 0 || is_closed_desc(fd)) {
        goto e;
    }
    FIL* fp = fd->fp;
    char* orig_path = fd->path;
    fd = (FDESC*)pvPortCalloc(1, sizeof(FDESC));
    if (!fd) { errno = ENOMEM; return -1; }
    fd->fp = fp;
    fd->flags = 0;
    fd->path = orig_path;
    int res = array_push_back(ctx->pfiles, fd);
    if (!res) {
        vPortFree(fd);
        errno = ENOMEM;
        return -1;
    }
    ++fp->pending_descriptors;
    errno = 0;
    return res;
e:
    errno = EBADF;
    return -1;
}

int __in_hfa() __dup3(int oldfd, int newfd, int flags) {
    if (oldfd < 0 || newfd < 0) goto e;
    if (oldfd == newfd) {
        errno = EINVAL;  // POSIX требует EINVAL при dup3(oldfd == newfd)
        return -1;
    }
    cmd_ctx_t* ctx = get_cmd_ctx();
    init_pfiles(ctx);
    FDESC* fd0 = (FDESC*)array_get_at(ctx->pfiles, oldfd);
    if (fd0 == 0 || is_closed_desc(fd0)) goto e;
    FDESC* fd1 = (FDESC*)array_get_at(ctx->pfiles, newfd);
    if (fd1 == 0) {
        if (array_resize(ctx->pfiles, newfd + 1) < 0) {
            errno = ENOMEM;
            return -1;
        }
        fd1 = (FDESC*)array_get_at(ctx->pfiles, newfd);
        if (fd1 == 0) {
            errno = ENOMEM;
            return -1;
        }
    } else {
        __close(newfd);
    }
    FIL* fp0 = fd0->fp;
    FIL* fp1 = fd1->fp;
    if ((intptr_t)fp1 > STDERR_FILENO && !fp1->pending_descriptors) { // not STD and not in use by other descriptors, so we can remove it
        vPortFree(fd1->fp);
    }
    fd1->fp = fp0;
    fd1->flags = 0;
    fd1->path = fd0->path;
    ctx->pfiles->p[newfd] = fd1;
    fp0->pending_descriptors++;
    errno = 0;
    return newfd;
e:
    errno = EBADF;
    return -1;
}

int __in_hfa() __dup2(int oldfd, int newfd)
{
    if (oldfd < 0 || newfd < 0) {
        errno = EBADF;
        return -1;
    }
    if (oldfd == newfd) {
        errno = 0;
        return newfd;
    }
    return __dup3(oldfd, newfd, 0);
}

/*
 * Minimal POSIX.1-like fcntl() implementation.
 *
 * Supported commands:
 *   F_GETFD   - get descriptor flags (e.g., FD_CLOEXEC)
 *   F_SETFD   - set descriptor flags
 *   F_GETFL   - get file status flags (e.g., O_APPEND, O_NONBLOCK)
 *   F_SETFL   - set file status flags
 *
 * Unimplemented commands will return -1 and set errno = EINVAL.
 */
int __in_hfa() __fcntl(int fd, int cmd, uintptr_t flags) {
    if (fd < 0) goto e;
    cmd_ctx_t* ctx = get_cmd_ctx();
    init_pfiles(ctx);
    FDESC* fdesc = (FDESC*)array_get_at(ctx->pfiles, fd);
    if (!fdesc || is_closed_desc(fdesc)) goto e;
    int ret;
    switch (cmd) {
        case F_GETFD:
            ret = fdesc->flags & FD_CLOEXEC;
            break;
        case F_SETFD:
            fdesc->flags = (fdesc->flags & ~FD_CLOEXEC) | (flags & FD_CLOEXEC);
            ret = 0;
            break;
        case F_GETFL:
            ret = fdesc->flags;
            break;
        case F_SETFL:
            /* Only allow O_APPEND, O_NONBLOCK, etc. — silently ignore unsupported bits */
            fdesc->flags = (fdesc->flags & ~(O_APPEND | O_NONBLOCK)) | (flags & (O_APPEND | O_NONBLOCK));
            ret = 0;
            break;
        default:
            errno = EINVAL;
            return -1;
    }
    errno = 0;
    return ret;
e:
    errno = EBADF;
    return -1;
}

int __in_hfa() __llseek(unsigned int fd,
             unsigned long offset_high,
             unsigned long offset_low,
             off_t *result,
             unsigned int whence
) {
    if (!result) { errno = EFAULT; return -1; }
    off_t offset = offset_low | ((off_t)offset_high << 32);
    if (fd < 0) goto e;
    cmd_ctx_t* ctx = get_cmd_ctx();
    init_pfiles(ctx);
    FDESC* fdesc = (FDESC*)array_get_at(ctx->pfiles, fd);
    if (!fdesc || is_closed_desc(fdesc)) goto e;
    FIL* fp = fdesc->fp;
    // Standard descriptors cannot be seeked
    if ((intptr_t)fp <= STDERR_FILENO) {
        errno = ESPIPE; // Illegal seek
        return -1;
    }
    int64_t new_pos;
    switch (whence) {
        case SEEK_SET:
            new_pos = offset;
            break;
        case SEEK_CUR:
            new_pos = f_tell(fp) + offset;
            break;
        case SEEK_END:
            new_pos = f_size(fp) + offset;
            break;
        default:
            errno = EINVAL;
            return -1;
    }
    if (new_pos < 0) {
        errno = EINVAL;
        return -1;
    }
    FRESULT fr = f_lseek(fp, new_pos);
    if (fr != FR_OK) {
        errno = map_ff_fresult_to_errno(fr);
        return -1;
    }

    errno = 0;
    *result = f_tell(fp);
    return 0;
e:
    errno = EBADF;
    return -1;
}

long __in_hfa() __lseek_p(int fd, long offset, int whence) {
    if (fd < 0) goto e;
    cmd_ctx_t* ctx = get_cmd_ctx();
    init_pfiles(ctx);
    FDESC* fdesc = (FDESC*)array_get_at(ctx->pfiles, fd);
    if (!fdesc || is_closed_desc(fdesc)) goto e;
    FIL* fp = fdesc->fp;
    int64_t new_pos;
    // Standard descriptors cannot be seeked
    if ((intptr_t)fp <= STDERR_FILENO) {
        errno = ESPIPE; // Illegal seek
        return -1;
    }
    switch (whence) {
        case SEEK_SET:
            new_pos = offset;
            break;
        case SEEK_CUR:
            new_pos = f_tell(fp) + offset;
            break;
        case SEEK_END:
            new_pos = f_size(fp) + offset;
            break;
        default:
            errno = EINVAL;
            return -1;
    }
    if (new_pos < 0) {
        errno = EINVAL;
        return -1;
    }
    FRESULT fr = f_lseek(fp, new_pos);
    if (fr != FR_OK) {
        errno = map_ff_fresult_to_errno(fr);
        return -1;
    }
    errno = 0;
    return f_tell(fp);
e:
    errno = EBADF;
    return -1;
}

int __in_hfa() __unlinkat(int dfd, const char* _pathname, int flags) {
    cmd_ctx_t* ctx = get_cmd_ctx();
    init_pfiles(ctx);
	char* pathname = __realpathat(dfd, _pathname, 0, flags | AT_HLINK_NOFOLLOW);
	if (!pathname) { return -1; }
    FRESULT fr;
    FILINFO info;
    fr = f_stat(pathname, &info);
    if (fr != FR_OK) {
        errno = map_ff_fresult_to_errno(fr);
inval:
        __free(pathname);
        return -1;
    }
    if (!(flags & AT_REMOVEANY)) { // W/A to support unlink in libc remove style
        if (flags & AT_REMOVEDIR) {
            if (!(info.fattrib & AM_DIR)) {
                errno = ENOTDIR;
                goto inval;
            }
        } else {
            if (info.fattrib & AM_DIR) {
                errno = EISDIR;
                goto inval;
            }
        }
    }
    vTaskSuspendAll();
    uint32_t hash = get_hash(pathname);
    posix_link_t* rename_to = 0;
    uint32_t omode = posix_unlink(pathname, hash, &rename_to);
    if (omode) {
        if (rename_to) {
            f_unlink(rename_to->fname);
            fr = f_rename(pathname, rename_to->fname); if (fr != FR_OK) { goto err; }
            // new original, so cleanup 'H' related fields
            vPortFree(rename_to->hlink.ofname);
            rename_to->desc.mode = omode;
            rename_to->desc.owner = 0;
            goto ok;
        }
    }
    fr = f_unlink(pathname);
    if (fr != FR_OK) {
err:
    	xTaskResumeAll();
        __free(pathname);
        errno = map_ff_fresult_to_errno(fr);
        return -1;
    }
ok:
    extfs_flush();
	xTaskResumeAll();
    __free(pathname);
    errno = 0;
    return 0;
}

int __in_hfa() __renameat(int dfd1, const char * f1, int dfd2, const char * f2) {
    if (!f1 || !f2) {
        errno = EFAULT;
        return -1;
    }
    cmd_ctx_t* ctx = get_cmd_ctx();
    init_pfiles(ctx);
    vTaskSuspendAll();
    char* path = __realpathat(dfd1, f1, 0, AT_SYMLINK_NOFOLLOW);
	if (!path) { return -1; }
    uint32_t hash = get_hash(path);
    posix_link_t* lnk = lookup_exact(hash, path);
    char* path2 = __realpathat(dfd2, f2, 0, AT_SYMLINK_NOFOLLOW);
	if (!path2) { __free(path); return -1; }
    /// TODO: POSIX rename() допускает переименование каталогов (если f2 указывает на существующий каталог, то поведение зависит от того, пуст ли он).
    FRESULT fr = f_rename(path, path2);
    if (fr != FR_OK) {
        __free(path);
        __free(path2);
        errno = map_ff_fresult_to_errno(fr);
    	xTaskResumeAll();
        return -1;
    }
    if (lnk) {
        vPortFree(lnk->fname);
        lnk->fname = path2;
        lnk->hash = get_hash(path2);
        if (lnk->type == 'O')
            replace_orig(path, hash, lnk);
        extfs_flush();
    } else {
        __free(path2);
    }
    __free(path);
	xTaskResumeAll();
    errno = 0;
    return 0;
}

int __in_hfa() __linkat(int fde, const char* _existing, int fdn, const char* _new, int flag) {
    if (!_existing || ! _new) {
        errno = EFAULT;
        return -1;
    }
    cmd_ctx_t* ctx = get_cmd_ctx();
    init_pfiles(ctx);
	char* existing = __realpathat(fde, _existing, 0, flag);
	if (!existing) { return -1; }
    uint32_t ohash = get_hash(existing);
    posix_link_t* lnk = lookup_exact(ohash, existing);
    uint32_t omode;
    if (!lnk) {
        FILINFO* fno = (FILINFO*)pvPortMalloc(sizeof(FILINFO));
        if (!fno) {
            __free(existing);
            errno = ENOMEM;
            return -1;
        }
        FRESULT fr = f_stat(existing, fno);
        if (fr != FR_OK) {
            vPortFree(fno);
            __free(existing);
            errno = map_ff_fresult_to_errno(fr);
            return -1;
        }
        if (fno->fattrib & AM_DIR) {
            vPortFree(fno);
            __free(existing);
            errno = EPERM; // Operation not permitted for directories
            return -1;
        }
        omode = fatfs_mode_to_posix(fno->fattrib);
        vPortFree(fno);
    } else {
        omode = (lnk->type != 'H') ? lnk->desc.mode : (S_IFLNK | 0777);
    }
    char* new = __realpathat(fdn, _new, 0, AT_SYMLINK_FOLLOW);
	if (!new) { __free(existing); return -1; }
    uint32_t hash = get_hash(new);
    FIL* pf = (FIL*)pvPortMalloc(sizeof(FIL));
    if (!pf) { __free(existing); __free(new); errno = ENOMEM; return -1; }
    FRESULT fr = f_open(pf, new, FA_WRITE | FA_CREATE_NEW);
    if (fr != FR_OK) {
        __free(existing);
        __free(new);
        vPortFree(pf);
        errno = map_ff_fresult_to_errno(fr);
        return -1;
    }
    UINT bw;
    f_write(pf, "H", 1, &bw);
    fr = f_write(pf, existing, strlen(existing) + 1, &bw);
    f_close(pf);
    vPortFree(pf);
    if (fr != FR_OK) {
        __free(existing);
        __free(new);
        errno = map_ff_fresult_to_errno(fr);
        return -1;
    }
    fr = extfs_add_hlink(new, hash, existing, ohash, omode);
    __free(existing);
    __free(new);
    if (fr != FR_OK) {
        errno = map_ff_fresult_to_errno(fr);
        return -1;
    }
    errno = 0;
    return 0;
}
int __in_hfa() __symlinkat(const char *existing, int fd, const char* _new) {
    if (!existing || !_new) {
        errno = EFAULT;
        return -1;
    }
    cmd_ctx_t* ctx = get_cmd_ctx();
    init_pfiles(ctx);
	char* new = __realpathat(fd, _new, 0, AT_SYMLINK_NOFOLLOW);
	if (!new) { return -1; }
    uint32_t hash = get_hash(new);
    FIL* pf = (FIL*)pvPortMalloc(sizeof(FIL));
    if (!pf) { __free(new); errno = ENOMEM; return -1; }
    FRESULT fr = f_open(pf, new, FA_WRITE | FA_CREATE_NEW);
    if (fr != FR_OK) {
        __free(new);
        vPortFree(pf);
        errno = map_ff_fresult_to_errno(fr);
        return -1;
    }
    UINT bw;
    f_write(pf, "S", 1, &bw);
    fr = f_write(pf, existing, strlen(existing) + 1, &bw);
    f_close(pf);
    vPortFree(pf);
    if (fr != FR_OK) {
        __free(new);
        errno = map_ff_fresult_to_errno(fr);
        return -1;
    }
    fr = extfs_add_link(new, hash, 'S', S_IFLNK | 0777, 0);
    __free(new);
    if (fr != FR_OK) {
        errno = map_ff_fresult_to_errno(fr);
        return -1;
    }
    errno = 0;
    return 0;
}

long __in_hfa() __readlinkat(int fd, const char *restrict _path, char *restrict buf, size_t bufsize) {
    cmd_ctx_t* ctx = get_cmd_ctx();
    init_pfiles(ctx);
	char* path = __realpathat(fd, _path, 0, AT_SYMLINK_FOLLOW);
	if (!path) { return -1; }
    if (!is_symlink(get_hash(path), path)) {
        __free(path);
        errno = EINVAL;
        return -1;
    }
    int res = __readlinkat_internal(path, buf, bufsize);
    __free(path);
    return res;
}

long __in_hfa() __readlinkat_internal(const char *restrict path, char *restrict buf, size_t bufsize) {
    FIL* pf = (FIL*)pvPortMalloc(sizeof(FIL));
    if (!pf) { __free(path); errno = ENOMEM; return -1; }
    FRESULT fr = f_open(pf, path, FA_READ);
    if (fr != FR_OK) {
        vPortFree(pf);
        errno = map_ff_fresult_to_errno(fr);
        return -1;
    }
    UINT br;
    fr = f_read(pf, buf, bufsize, &br);
    f_close(pf);
    vPortFree(pf);
    if (fr != FR_OK) {
        errno = map_ff_fresult_to_errno(fr);
        return -1;
    }
    if (br < 2 || buf[0] != 'S') {
        errno = EINVAL;
        return -1;
    }
    long res = strlen(buf + 1);
    memmove(buf, buf + 1, res);
    errno = 0;
    return res;
}

int __in_hfa() __mkdirat(int dirfd, const char *pathname, mode_t mode) {
    cmd_ctx_t* ctx = get_cmd_ctx();
    init_pfiles(ctx);
	char* path = __realpathat(dirfd, pathname, 0, AT_SYMLINK_FOLLOW);
	if (!path) { return -1; }
    FRESULT fr = f_mkdir(path);
    if (fr != FR_OK) {
        __free(path);
        errno = map_ff_fresult_to_errno(fr);
        return -1;
    }
    uint32_t hash = get_hash(path);
    vTaskSuspendAll();
    mode &= ~ctx->umask;
    posix_link_t* lnk = posix_add_link(hash, path, 'O', (mode | S_IFDIR), 0, true);
    if (!lnk) {
        xTaskResumeAll();
        errno = ENOMEM;
        return -1;
    }
    fr = append_to_extfs(lnk); // todo
    xTaskResumeAll();
    errno = 0;
    return 0;
}

DIR* __in_hfa() __opendirat(int bfd, const char* _path) {
    if (!_path) {
        errno = EINVAL;
        return 0;
    }
    cmd_ctx_t* ctx = get_cmd_ctx();
    init_pfiles(ctx);
	char* path = __realpathat(bfd, _path, 0, AT_SYMLINK_FOLLOW);
    if (!path) {
        return 0;
    }
    DIR* pd = (DIR*)alloc_dir();
    if (!pd) {
        __free(path);
        errno = ENOMEM;
        return 0;
    }
    FRESULT fr = f_opendir(pd, path);
    if (fr != FR_OK) {
        vPortFree(pd);
        __free(path);
        errno = map_ff_fresult_to_errno(fr);
        return 0;
    }
    for (size_t i = 0; i < ctx->pdirs->size; ++i) {
        if (ctx->pdirs->p[i] == 0) {
            ctx->pdirs->p[i] = pd;
            goto ex;
        }
    }
    array_push_back(ctx->pdirs, pd);
ex:
    pd->dirname = path;
    errno = 0;
    return pd;
}

DIR* __in_hfa() __opendir(const char* _path) {
    return __opendirat(AT_FDCWD, _path);
}

int __in_hfa() __closedir(DIR* d) {
    if (!d) {
        errno = EINVAL;
        return -1;
    }
    cmd_ctx_t* ctx = get_cmd_ctx();
    init_pfiles(ctx);
    FRESULT fr = f_closedir(d);
    if (fr != FR_OK) {
        errno = map_ff_fresult_to_errno(fr);
        return -1;
    }
    for (size_t i = 0; i < ctx->pdirs->size; ++i) {
        if (ctx->pdirs->p[i] == d) {
            ctx->pdirs->p[i] = 0;
            break;
        }
    }
    dealloc_dir(d);
    errno = 0;
    return 0;
}

struct dirent* __in_hfa() __readdir(DIR* d) {
    cmd_ctx_t* ctx = get_cmd_ctx();
    if (!d || !ctx || !ctx->pdirs) {
        errno = EINVAL;
        return 0;
    }
    if (!d->dirent) {
        d->dirent = pvPortCalloc(1, sizeof(struct dirent));
        if (!d->dirent) {
            errno = ENOMEM;
            return 0;
        }
    }
    struct dirent* de = (struct dirent*)d->dirent;
    if (de->pos == 0) {
        de->d_name = ".";
        de->d_namlen = 1;
        de->pos++;
        return de;
    }
    if (de->pos == 1) {
        de->d_name = "..";
        de->d_namlen = 2;
        de->pos++;
        return de;
    }
    FRESULT fr = f_readdir(d, &de->ff_info);
    de->pos++;
    if (fr != FR_OK) {
        errno = map_ff_fresult_to_errno(fr);
        return 0;
    }
    if (de->ff_info.fname[0] == '\0') {
        return 0;
    }
    de->d_name = de->ff_info.fname;
    de->d_namlen = strlen(de->ff_info.fname);
    return de;
}

void __in_hfa() __rewinddir(DIR *d) {
    if (!d) {
        errno = EINVAL;
        return;
    }
    FRESULT fr = f_rewinddir(d);
    if (fr != FR_OK) {
        errno = map_ff_fresult_to_errno(fr);
        return;
    }
    if (d->dirent) {
        memset(d->dirent, 0, sizeof(struct dirent));
    }
    errno = 0;
}

int __fchdir(int d) {
    cmd_ctx_t* ctx = get_cmd_ctx();
    if (!d || !ctx || !ctx->pdirs) {
        errno = EINVAL;
        return -1;
    }
    for (size_t i = 0; i < ctx->pdirs->size; ++i) {
        if (ctx->pdirs->p[i] == (DIR*)d) {
            return __chdir(((DIR*)d)->dirname);
        }
    }
    errno = EBADF;
    return -1;
}

int __dirfd(DIR* pd) {
    return (intptr_t)pd;
}

char* get_dir(int dfd, char* buf, size_t size) {
    cmd_ctx_t* ctx = get_cmd_ctx();
    if (!dfd || !ctx || !ctx->pdirs) {
        errno = EINVAL;
        return 0;
    }
    for (size_t i = 0; i < ctx->pdirs->size; ++i) {
        if (ctx->pdirs->p[i] == (DIR*)dfd) {
            size_t len = strlen(((DIR*)dfd)->dirname) + 1;
            if (!buf) {
                buf = (char*)pvPortMalloc(len);
                if (!buf) { errno = ENOMEM; return 0; }
            } else if (len > size) {
                errno = ERANGE;
                return 0;
            }
            memcpy(buf, ((DIR*)dfd)->dirname, len);
            errno = 0;
            return buf;
        }
    }
    errno = EBADF;
    return 0;
}

int __fchmodat(int fd, const char* n, mode_t m, int fl)
{
    if (!n) {
        errno = EINVAL;
        return -1;
    }
    int realpath_flags = (fl & AT_SYMLINK_NOFOLLOW)
                         ? AT_SYMLINK_NOFOLLOW
                         : AT_SYMLINK_FOLLOW;
    char* path = __realpathat(fd, n, 0, realpath_flags);
    if (!path) {
        return -1; // errno already set
    }
    uint32_t h = get_hash(path);
    posix_link_t* lnk = lookup_exact(h, path);
    if (lnk) {
        lnk->desc.mode = m;
        FRESULT fr = extfs_flush();
        __free(path);
        if (fr != FR_OK) {
            errno = EIO;
            return -1;
        }
        errno = 0;
        return 0;
    }
    vTaskSuspendAll();
    lnk = posix_add_link(h, path, 'O', m, 0, true);
    if (!lnk) {
        xTaskResumeAll();
        __free(path);
        errno = ENOMEM;
        return -1;
    }
    FRESULT fr = append_to_extfs(lnk);
    xTaskResumeAll();
    if (fr != FR_OK) {
        errno = EIO;
        return -1;
    }
    errno = 0;
    return 0;
}

int __fchmod(int d, mode_t m)
{
    cmd_ctx_t* ctx = get_cmd_ctx();
    if (!ctx || !ctx->pfiles || !ctx->pdirs) {
        errno = EINVAL;
        return -1;
    }
    if ((size_t)d < ctx->pfiles->size) {
        FDESC* f = ctx->pfiles->p[d];
        if (f) {
            // stdin/stdout/stderr have path == NULL
            if (!f->path) {
                errno = EBADF;
                return -1;
            }
            return __fchmodat(AT_FDCWD, f->path, m, 0);
        }
    }
    for (size_t i = 0; i < ctx->pdirs->size; ++i) {
        DIR* pd = ctx->pdirs->p[i];
        if (!pd)
            continue;
        // dirfd = (int)(intptr_t)DIR*
        if ((int)(intptr_t)pd == d) {
            if (!pd->dirname) {
                errno = EBADF;
                return -1;
            }
            return __fchmodat(AT_FDCWD, pd->dirname, m, 0);
        }
    }
    errno = EBADF;
    return -1;
}
