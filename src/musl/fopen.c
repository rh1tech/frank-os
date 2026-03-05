#include "internal/stdio_impl.h"
#include "internal/__stdio.h"
#include "internal/__stdlib.h"
#include "sys/fcntl.h"
#include "sys/ioctl.h"
#include "sys_table.h"
#include <string.h>
#include <errno.h>

FILE* __libc() __ofl_add(FILE *f)
{
	FILE **head = __ofl_lock();
	f->next = *head;
	if (*head) {
		(*head)->prev = f;
	}
	*head = f;
	__ofl_unlock();
	return f;
}

FILE* __libc() __fdopen(int fd, const char *mode)
{
	FILE *f;
	struct winsize wsz;

	/* Check for valid initial mode character */
	if (!strchr("rwa", *mode)) {
		errno = EINVAL;
		return 0;
	}

	/* Allocate FILE+buffer or fail */
	if (!(f=malloc(sizeof *f + UNGET + BUFSIZ))) return 0;

	/* Zero-fill only the struct, not the buffer */
	memset(f, 0, sizeof *f);

	/* Impose mode restrictions */
	if (!strchr(mode, '+')) f->flags = (*mode == 'r') ? F_NOWR : F_NORD;

	/* Apply close-on-exec flag */
	if (strchr(mode, 'e')) __fcntl(fd, F_SETFD, FD_CLOEXEC);

	/* Set append mode on fd if opened for append */
	if (*mode == 'a') {
		int flags = __fcntl(fd, F_GETFL, 0);
		if (!(flags & O_APPEND))
			__fcntl(fd, F_SETFL, flags | O_APPEND);
		f->flags |= F_APP;
	}

	f->fd = fd;
	f->buf = (unsigned char *)f + sizeof *f + UNGET;
	f->buf_size = BUFSIZ;

	/* Activate line buffered mode for terminals */
	f->lbf = EOF;
	if (!(f->flags & F_NOWR) && !__ioctl(fd, TIOCGWINSZ, &wsz))
		f->lbf = '\n';

	/* Initialize op ptrs. No problem if some are unneeded. */
	f->read = __stdio_read;
	f->write = __stdio_write;
	f->seek = __stdio_seek;
	f->close = __stdio_close;

	///if (!libc.threaded)
		f->lock = -1;

	/* Add new FILE to open file list */
	return __ofl_add(f);
}

weak_alias(__fdopen, fdopen);

int __libc() __fmodeflags(const char *mode)
{
	int flags;
	if (strchr(mode, '+')) flags = O_RDWR;
	else if (*mode == 'r') flags = O_RDONLY;
	else flags = O_WRONLY;
	if (strchr(mode, 'x')) flags |= O_EXCL;
	if (strchr(mode, 'e')) flags |= O_CLOEXEC;
	if (*mode != 'r') flags |= O_CREAT;
	if (*mode == 'w') flags |= O_TRUNC;
	if (*mode == 'a') flags |= O_APPEND;
	return flags;
}

extern int printf(const char *restrict, ...);
extern size_t xPortGetFreeHeapSize(void);

FILE* __libc() __fopen(const char *restrict filename, const char *restrict mode)
{
	FILE *f;
	int fd;
	int flags;
	/* Check for valid initial mode character */
	if (!strchr("rwa", *mode)) {
		errno = EINVAL;
		return 0;
	}

	/* Compute the flags to pass to open() */
	flags = __fmodeflags(mode);

	fd = __openat(AT_FDCWD, filename, flags, 0666);
	if (fd < 0) return 0;
	if (flags & O_CLOEXEC) {
		__fcntl(fd, F_SETFD, FD_CLOEXEC);
	}
	f = __fdopen(fd, mode);
	if (f) {
		return f;
	}

	__close(fd);
	return 0;
}

long __libc() __syscall_ret(unsigned long r)
{
	if (r > -4096UL) {
		errno = -r;
		return -1;
	}
	return r;
}

/* The basic idea of this implementation is to open a new FILE,
 * hack the necessary parts of the new FILE into the old one, then
 * close the new FILE. */

/* Locking IS necessary because another thread may provably hold the
 * lock, via flockfile or otherwise, when freopen is called, and in that
 * case, freopen cannot act until the lock is released. */

FILE* __libc() __freopen(const char *restrict filename, const char *restrict mode, FILE *restrict f)
{
	int fl = __fmodeflags(mode);
	FILE *f2;

	FLOCK(f);

	__fflush(f);

	if (!filename) {
		if (fl & O_CLOEXEC) {
			__fcntl(f->fd, F_SETFD, FD_CLOEXEC);
		}
		fl &= ~(O_CREAT|O_EXCL|O_CLOEXEC);
		if (__fcntl(f->fd, F_SETFL, fl) < 0) {
			goto fail;
		}
	} else {
		f2 = __fopen(filename, mode);
		if (!f2) goto fail;
		if (f2->fd == f->fd) f2->fd = -1; /* avoid closing in fclose */
		else {
			if (__dup3(f2->fd, f->fd, fl & O_CLOEXEC) < 0) {
				goto fail2;
			}
		}

		f->flags = (f->flags & F_PERM) | f2->flags;
		f->read = f2->read;
		f->write = f2->write;
		f->seek = f2->seek;
		f->close = f2->close;

		__fclose(f2);
	}

	f->mode = 0;
	f->locale = 0;
	FUNLOCK(f);
	return f;

fail2:
	__fclose(f2);
fail:
	__fclose(f);
	return NULL;
}

int __libc() __fileno(FILE* f) {
	if (!f) {
		errno = EINVAL;
		return -1;
	}
	return f->fd;
}
