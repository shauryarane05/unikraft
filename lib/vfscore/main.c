/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * Copyright (c) 2005-2007, Kohsuke Ohtani
 * Copyright (C) 2013 Cloudius Systems, Ltd.
 * Copyright (c) 2019, NEC Europe Ltd., NEC Corporation.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. Neither the name of the author nor the names of any co-contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#define _GNU_SOURCE

#include <sys/statvfs.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <limits.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include <fcntl.h>
#include <vfscore/prex.h>
#include <vfscore/vnode.h>
#include "vfs.h"
#include <sys/file.h>
#include <stdarg.h>
#include <utime.h>
#include <vfscore/file.h>
#include <vfscore/mount.h>
#include <vfscore/fs.h>
#include <uk/print.h>
#include <uk/errptr.h>
#include <uk/ctors.h>
#include <uk/trace.h>
#include <uk/syscall.h>
#include <uk/essentials.h>

#ifdef DEBUG_VFS
int	vfs_debug = VFSDB_FLAGS;
#endif

/* This macro is for defining an alias of the 64bit version of a
 * syscall to the regular one. We only do this when libc-style
 * syscall wrappers are requested to be created.
 * NOTE: When using LFS64(), remember to undefine possible rename
 *       macros created by included libc headers (e.g., <fcntl.h>):
 *       ```
 *       #ifdef openat64
 *       #undef openat64
 *       #endif
 *
 *       LFS64(openat)
 *       ```
 */
#if UK_LIBC_SYSCALLS
#define LFS64(x)				\
	__alias(x, x##64)
#else
#define LFS64(x)
#endif /* !UK_LIBC_SYSCALLS */

static mode_t global_umask = S_IWGRP | S_IWOTH;

static inline int libc_error(int err)
{
    errno = err;
    return -1;
}

// In BSD's internal implementation of read() and write() code, for example
// sosend_generic(), a partial read or write returns both an EWOULDBLOCK error
// *and* a non-zero number of written bytes. In that case, we need to zero the
// error, so the system call appear a successful partial read/write.
// In FreeBSD, dofilewrite() and dofileread() (sys_generic.c) do this too.
static inline int has_error(int error, int bytes)
{
	/* TODO: OSv checks also for ERESTART */
	return error && (
		(bytes == 0) ||
		(error != EWOULDBLOCK && error != EINTR));
}

static inline mode_t apply_umask(mode_t mode)
{
	return mode & ~uk_load_n(&global_umask);
}

UK_TRACEPOINT(trace_vfs_open, "\"%s\" %#x 0%0o", const char*, int, mode_t);
UK_TRACEPOINT(trace_vfs_open_ret, "%d", int);
UK_TRACEPOINT(trace_vfs_open_err, "%d", int);

struct task *main_task;	/* we only have a single process */

UK_LLSYSCALL_R_DEFINE(int, open, const char*, pathname, int, flags,
		      mode_t, mode)
{
	trace_vfs_open(pathname, flags, mode);

	struct task *t = main_task;
	char path[PATH_MAX];
	struct vfscore_file *fp;
	int fd, error;
	int acc;

	acc = 0;
	switch (flags & O_ACCMODE) {
	case O_RDONLY:
		acc = VREAD;
		break;
	case O_WRONLY:
		acc = VWRITE;
		break;
	case O_RDWR:
		acc = VREAD | VWRITE;
		break;
	}

	error = task_conv(t, pathname, acc, path);
	if (error)
		goto out_error;

	mode = apply_umask(mode);
	error = sys_open(path, flags, mode, &fp);
	if (error)
		goto out_error;

	error = fdalloc(fp, &fd);
	if (error)
		goto out_fput;
	fdrop(fp);
	trace_vfs_open_ret(fd);
	return fd;

	out_fput:
	fdrop(fp);
	out_error:
	trace_vfs_open_err(error);
	return -error;
}

#if UK_LIBC_SYSCALLS
int open(const char *pathname, int flags, ...)
{
	mode_t mode = 0;

	if (flags & O_CREAT) {
		va_list ap;

		va_start(ap, flags);
		mode = va_arg(ap, mode_t);
		va_end(ap);
	}

	return uk_syscall_e_open((long int)pathname, flags, mode);
}

#ifdef open64
#undef open64
#endif

LFS64(open);
#endif /* UK_LIBC_SYSCALLS */

UK_LLSYSCALL_R_DEFINE(int, openat, int, dirfd, const char *, pathname,
		      int, flags, int, mode)
{
	if (pathname[0] == '/' || dirfd == AT_FDCWD) {
		return uk_syscall_do_open((long int)pathname, flags, mode);
	}

	struct vfscore_file *fp;
	int error = fget(dirfd, &fp);
	if (error)
		return -error;

	struct vnode *vp = fp->f_dentry->d_vnode;
	vn_lock(vp);

	char p[PATH_MAX];

	/* build absolute path */
	strlcpy(p, fp->f_dentry->d_mount->m_path, PATH_MAX);
	strlcat(p, fp->f_dentry->d_path, PATH_MAX);
	strlcat(p, "/", PATH_MAX);
	strlcat(p, pathname, PATH_MAX);

	vn_unlock(vp);
	fdrop(fp);

	error = uk_syscall_do_open((long int)p, flags, mode);

	return error;
}

#if UK_LIBC_SYSCALLS
int openat(int dirfd, const char *pathname, int flags, ...)
{
	mode_t mode = 0;

	if (flags & O_CREAT) {
		va_list ap;

		va_start(ap, flags);
		mode = va_arg(ap, mode_t);
		va_end(ap);
	}

	return uk_syscall_e_openat(dirfd, (long) pathname, flags, mode);
}

#ifdef openat64
#undef openat64
#endif

LFS64(openat);
#endif /* UK_LIBC_SYSCALLS */

UK_SYSCALL_R_DEFINE(int, creat, const char*, pathname, mode_t, mode)
{
	return uk_syscall_do_open((long int)pathname,
				  O_CREAT | O_WRONLY | O_TRUNC,
				  mode);
}

#ifdef creat64
#undef creat64
#endif

LFS64(creat);


UK_TRACEPOINT(trace_vfs_mknod, "\"%s\" 0%0o %#x", const char*, mode_t, dev_t);
UK_TRACEPOINT(trace_vfs_mknod_ret, "");
UK_TRACEPOINT(trace_vfs_mknod_err, "%d", int);

static int __xmknod_helper(int ver __maybe_unused, const char *pathname,
			   mode_t mode, dev_t *dev __maybe_unused)
{
	UK_ASSERT(ver == 0); // On x86-64 Linux, _MKNOD_VER_LINUX is 0.
	struct task *t = main_task;
	char path[PATH_MAX];
	int error;

	trace_vfs_mknod(pathname, mode, *dev);
	if ((error = task_conv(t, pathname, VWRITE, path)) != 0)
		goto out_error;

	error = sys_mknod(path, mode);
	if (error)
		goto out_error;

	trace_vfs_mknod_ret();
	return 0;

	out_error:
	trace_vfs_mknod_err(error);
	return -error;
}

#if UK_LIBC_SYSCALLS
int __xmknod(int ver, const char *pathname,
		mode_t mode, dev_t *dev __unused)
{
	return __xmknod_helper(ver, pathname, mode, dev);
}
#endif /* UK_LIBC_SYSCALLS */

UK_SYSCALL_R_DEFINE(int, mknod, const char*, pathname, mode_t, mode, dev_t, dev)
{
	return __xmknod_helper(0, pathname, mode, &dev);
}

/**
 * Return:
 * = 0, Success and the nr of bytes read is returned in bytes parameter.
 * < 0, error code.
 */
static ssize_t do_preadv(struct vfscore_file *fp, const struct iovec *iov,
			 int iovcnt, off_t offset, ssize_t *bytes)
{
	size_t cnt;
	int error;

	UK_ASSERT(fp && iov);

	/* Otherwise, try to read the file. */
	error = sys_read(fp, iov, iovcnt, offset, &cnt);

	if (has_error(error, cnt))
		goto out_error;

	*bytes = cnt;
	return 0;

out_error:
	return -error;
}

UK_TRACEPOINT(trace_vfs_preadv, "%p %#x %d %#x", void *, const struct iovec*,
	      int, off_t);
UK_TRACEPOINT(trace_vfs_preadv_ret, "%#x", ssize_t);
UK_TRACEPOINT(trace_vfs_preadv_err, "%d", int);

ssize_t vfscore_preadv(struct vfscore_file *fp, const struct iovec *iov,
		       int iovcnt, off_t offset)
{
	ssize_t bytes;
	int error;

	trace_vfs_preadv(fp, iov, iovcnt, offset);

	/* Check if the file is indeed seekable. */
	if (fp->f_vfs_flags & UK_VFSCORE_NOPOS) {
		error = -ESPIPE;
		goto out_error_fdrop;
	}
	/* Check if the file has not already been read and that
	 * is not a character device.
	 */
	else if (fp->f_offset < 0 &&
		(fp->f_dentry == NULL ||
		 fp->f_dentry->d_vnode->v_type != VCHR)) {
		error = -EINVAL;
		goto out_error_fdrop;
	}

	/* Otherwise, try to read the file. */
	error = do_preadv(fp, iov, iovcnt, offset, &bytes);

out_error_fdrop:
	fdrop(fp);

	if (error < 0)
		goto out_error;

	trace_vfs_preadv_ret(bytes);
	return bytes;

out_error:
	trace_vfs_preadv_err(error);
	return error;
}

#ifdef preadv64
#undef preadv64
#endif

UK_TRACEPOINT(trace_vfs_pread, "%p %#x %#x %#x", void *, void*, size_t, off_t);
UK_TRACEPOINT(trace_vfs_pread_ret, "%#x", ssize_t);
UK_TRACEPOINT(trace_vfs_pread_err, "%d", int);

/*
 * Some libc's define some macros that remove the 64 suffix
 * from some system call function names (e.g., <unistd.h>, <fcntl.h>).
 * We need to undefine them here so that our system call
 * registration does not fail in such a case.
 */
#ifdef pread64
#undef pread64
#endif

ssize_t vfscore_pread64(struct vfscore_file *fp,
			void *buf, size_t count, off_t offset)
{
	trace_vfs_pread(fp, buf, count, offset);
	struct iovec iov = {
			.iov_base	= buf,
			.iov_len	= count,
	};
	ssize_t bytes;

	bytes = vfscore_preadv(fp, &iov, 1, offset);
	if (bytes < 0)
		trace_vfs_pread_err(bytes);
	else
		trace_vfs_pread_ret(bytes);
	return bytes;
}

UK_TRACEPOINT(trace_vfs_readv, "%p %#x %#x", void *, const struct iovec*,
	      int);
UK_TRACEPOINT(trace_vfs_readv_ret, "%#x", ssize_t);
UK_TRACEPOINT(trace_vfs_readv_err, "%d", int);

ssize_t vfscore_readv(struct vfscore_file *fp,
		      const struct iovec *iov, int iovcnt)
{
	ssize_t bytes;
	int error;

	trace_vfs_readv(fp, iov, iovcnt);

	/* Check if the file has not already been read and that is
	 * not a character device.
	 */
	if (fp->f_offset < 0 &&
	   (fp->f_dentry == NULL ||
	    fp->f_dentry->d_vnode->v_type != VCHR)) {
		error = -EINVAL;
		goto out_error_fdrop;
	}

	/* Otherwise, try to read the file. */
	error = do_preadv(fp, iov, iovcnt, -1, &bytes);

out_error_fdrop:
	fdrop(fp);

	if (error < 0)
		goto out_error;

	trace_vfs_readv_ret(bytes);
	return bytes;

out_error:
	trace_vfs_readv_err(error);
	return error;
}

UK_TRACEPOINT(trace_vfs_read, "%p %#x %d", void *, void *, int);
UK_TRACEPOINT(trace_vfs_read_ret, "%#x", ssize_t);
UK_TRACEPOINT(trace_vfs_read_err, "%d", int);

ssize_t vfscore_read(struct vfscore_file *fp, void *buf, size_t count)
{
	ssize_t bytes;

	UK_ASSERT(buf);

	struct iovec iov = {
			.iov_base	= buf,
			.iov_len	= count,
	};

	trace_vfs_read(fp, buf, count);

	bytes = vfscore_readv(fp, &iov, 1);
	if (bytes < 0)
		trace_vfs_read_err(bytes);
	else
		trace_vfs_read_ret(bytes);
	return bytes;
}

static int do_pwritev(struct vfscore_file *fp, const struct iovec *iov,
		      int iovcnt, off_t offset, ssize_t *bytes)
{
	int error;
	size_t cnt;

	UK_ASSERT(bytes);

	/* Otherwise, try to read the file. */
	error = sys_write(fp, iov, iovcnt, offset, &cnt);

	if (has_error(error, cnt))
		goto out_error;

	*bytes = cnt;
	return 0;

out_error:
	*bytes = -1;
	return -error;
}

UK_TRACEPOINT(trace_vfs_pwritev, "%p %#x %#x %#x", void *, const struct iovec*,
	      int, off_t);
UK_TRACEPOINT(trace_vfs_pwritev_ret, "%#x", ssize_t);
UK_TRACEPOINT(trace_vfs_pwritev_err, "%d", int);

ssize_t vfscore_pwritev(struct vfscore_file *fp, const struct iovec *iov,
			int iovcnt, off_t offset)
{
	ssize_t bytes;
	int error;

	trace_vfs_pwritev(fp, iov, iovcnt, offset);

	/* Check if the file is indeed seekable. */
	if (fp->f_vfs_flags & UK_VFSCORE_NOPOS) {
		error = -ESPIPE;
		goto out_error_fdrop;
	}
	/* Check if the file has not already been written to and that it is
	 * not a character device.
	 */
	else if (fp->f_offset < 0 &&
		(fp->f_dentry == NULL ||
		 fp->f_dentry->d_vnode->v_type != VCHR)) {
		error = -EINVAL;
		goto out_error_fdrop;
	}

	/* Otherwise, try to read the file. */
	error = do_pwritev(fp, iov, iovcnt, offset, &bytes);

out_error_fdrop:
	fdrop(fp);

	if (error < 0)
		goto out_error;

	trace_vfs_pwritev_ret(bytes);
	return bytes;

out_error:
	trace_vfs_pwritev_err(error);
	return error;
}

#ifdef pwritev64
#undef pwritev64
#endif

UK_TRACEPOINT(trace_vfs_pwrite, "%p %#x %#x %#x", void *, const void*, size_t,
	      off_t);
UK_TRACEPOINT(trace_vfs_pwrite_ret, "%#x", ssize_t);
UK_TRACEPOINT(trace_vfs_pwrite_err, "%d", int);

/*
 * Some libc's define some macros that remove the 64 suffix
 * from some system call function names (e.g., <unistd.h>, <fcntl.h>).
 * We need to undefine them here so that our system call
 * registration does not fail in such a case.
 */
#ifdef pwrite64
#undef pwrite64
#endif

ssize_t vfscore_pwrite64(struct vfscore_file *fp, const void *buf,
			 size_t count, off_t offset)
{
	trace_vfs_pwrite(fp, buf, count, offset);
	struct iovec iov = {
			.iov_base	= (void *)buf,
			.iov_len	= count,
	};
	ssize_t bytes;

	bytes = vfscore_pwritev(fp, &iov, 1, offset);
	if (bytes < 0)
		trace_vfs_pwrite_err(bytes);
	else
		trace_vfs_pwrite_ret(bytes);
	return bytes;
}

UK_TRACEPOINT(trace_vfs_writev, "%p %#x %d", void *, const struct iovec*, int);
UK_TRACEPOINT(trace_vfs_writev_ret, "%#x", ssize_t);
UK_TRACEPOINT(trace_vfs_writev_err, "%d", int);

ssize_t vfscore_writev(struct vfscore_file *fp,
		       const struct iovec *vec, int vlen)
{
	ssize_t bytes;
	int error;

	trace_vfs_writev(fp, vec, vlen);

	/* Check if the file has not already been written to and
	 * that it is not a character device.
	 */
	if (fp->f_offset < 0 &&
	   (fp->f_dentry == NULL ||
	    fp->f_dentry->d_vnode->v_type != VCHR)) {
		error = -EINVAL;
		goto out_error_fdrop;
	}

	/* Otherwise, try to read the file. */
	error = do_pwritev(fp, vec, vlen, -1, &bytes);

out_error_fdrop:
	fdrop(fp);

	if (error < 0)
		goto out_error;

	trace_vfs_pwritev_ret(bytes);
	return bytes;

out_error:
	trace_vfs_pwritev_err(error);
	return error;
}

UK_TRACEPOINT(trace_vfs_write, "%p %#x %#x", void *, const void *, size_t);
UK_TRACEPOINT(trace_vfs_write_ret, "%#x", ssize_t);
UK_TRACEPOINT(trace_vfs_write_err, "%d", int);

ssize_t vfscore_write(struct vfscore_file *fp, const void *buf, size_t count)
{
	ssize_t bytes;

	UK_ASSERT(buf);

	struct iovec iov = {
			.iov_base	= (void *)buf,
			.iov_len	= count,
	};
	trace_vfs_write(fp, buf, count);
	bytes = vfscore_writev(fp, &iov, 1);
	if (bytes < 0)
		trace_vfs_write_err(errno);
	else
		trace_vfs_write_ret(bytes);
	return bytes;
}

UK_TRACEPOINT(trace_vfs_stat, "\"%s\" %#x", const char*, struct stat*);
UK_TRACEPOINT(trace_vfs_stat_ret, "");
UK_TRACEPOINT(trace_vfs_stat_err, "%d", int);

UK_SYSCALL_R_DEFINE(int, stat, const char*, pathname, struct stat*, st)
{
	struct task *t = main_task;
	char path[PATH_MAX];
	int error;

	trace_vfs_stat(pathname, st);

	if (pathname == NULL || st == NULL) {
		return -EFAULT;
	}

	error = task_conv(t, pathname, 0, path);
	if (error) {
		goto out_error;
	}

	error = sys_stat(path, st);
	if (error) {
		goto out_error;
	}
	
	trace_vfs_stat_ret();
	return 0;

out_error:
	trace_vfs_stat_err(error);
	return -error;
}

#ifdef stat64
#undef stat64
#endif

LFS64(stat);

UK_TRACEPOINT(trace_vfs_lstat, "pathname=%s, stat=%#x", const char*,
	      struct stat*);
UK_TRACEPOINT(trace_vfs_lstat_ret, "");
UK_TRACEPOINT(trace_vfs_lstat_err, "errno=%d", int);

UK_SYSCALL_R_DEFINE(int, lstat, const char*, pathname, struct stat*, st)
{
	struct task *t = main_task;
	char path[PATH_MAX];
	int error;

	trace_vfs_lstat(pathname, st);

	if (pathname == NULL || st == NULL) {
		return -EFAULT;
	}

	error = task_conv(t, pathname, 0, path);
	if (error) {
		goto out_error;
	}

	error = sys_lstat(path, st);
	if (error) {
		goto out_error;
	}

	trace_vfs_lstat_ret();
	return 0;

out_error:
	trace_vfs_lstat_err(error);
	return -error;
}

#ifdef lstat64
#undef lstat64
#endif

LFS64(lstat);

/* The fstat syscall is no longer implemented here; need to declare */
long uk_syscall_do_fstat(long dirfd, long st);

UK_TRACEPOINT(trace_vfs_statfs, "\"%s\" %#x", const char*, struct statfs*);
UK_TRACEPOINT(trace_vfs_statfs_ret, "");
UK_TRACEPOINT(trace_vfs_statfs_err, "%d", int);

int __statfs(const char *pathname, struct statfs *buf)
{
	trace_vfs_statfs(pathname, buf);
	struct task *t = main_task;
	char path[PATH_MAX];
	int error;

	error = task_conv(t, pathname, 0, path);
	if (error)
		goto out_errno;

	error = sys_statfs(path, buf);
	if (error)
		goto out_errno;
	trace_vfs_statfs_ret();
	return 0;

out_errno:
	trace_vfs_statfs_err(error);
	return -error;
}

UK_SYSCALL_R_DEFINE(int, statfs, const char*, pathname, struct statfs*, buf)
{
	return __statfs(pathname, buf);
}

#ifdef statfs64
#undef statfs64
#endif

LFS64(statfs);

UK_TRACEPOINT(trace_vfs_fstatfs, "\"%s\" %#x", int, struct statfs*);
UK_TRACEPOINT(trace_vfs_fstatfs_ret, "");
UK_TRACEPOINT(trace_vfs_fstatfs_err, "%d", int);

int __fstatfs(int fd, struct statfs *buf)
{
	struct vfscore_file *fp;
	int error;

	trace_vfs_fstatfs(fd, buf);
	error = fget(fd, &fp);
	if (error)
		goto out_errno;

	error = sys_fstatfs(fp, buf);
	fdrop(fp);

	if (error)
		goto out_errno;
	trace_vfs_fstatfs_ret();
	return 0;

out_errno:
	trace_vfs_fstatfs_err(error);
	return -error;
}

UK_SYSCALL_R_DEFINE(int, fstatfs, int, fd, struct statfs*, buf)
{
	return __fstatfs(fd, buf);
}

#ifdef fstatfs64
#undef fstatfs64
#endif

LFS64(fstatfs);

#if UK_LIBC_SYSCALLS
static int
statfs_to_statvfs(struct statvfs *dst, struct statfs *src)
{
	dst->f_bsize = src->f_bsize;
	dst->f_frsize = src->f_bsize;
	dst->f_blocks = src->f_blocks;
	dst->f_bfree = src->f_bfree;
	dst->f_bavail = src->f_bavail;
	dst->f_files = src->f_files;
	dst->f_ffree = src->f_ffree;
	dst->f_favail = 0;
	dst->f_fsid = src->f_fsid.__val[0];
	dst->f_flag = src->f_flags;
	dst->f_namemax = src->f_namelen;
	return 0;
}

int
statvfs(const char *pathname, struct statvfs *buf)
{
	struct statfs st;

	if (__statfs(pathname, &st) < 0)
		return -1;
	return statfs_to_statvfs(buf, &st);
}

#ifdef statvfs64
#undef statvfs64
#endif

LFS64(statvfs);

int
fstatvfs(int fd, struct statvfs *buf)
{
	struct statfs st;

	if (__fstatfs(fd, &st) < 0)
		return -1;
	return statfs_to_statvfs(buf, &st);
}

#ifdef fstatvfs64
#undef fstatvfs64
#endif

LFS64(fstatvfs);
#endif /* UK_LIBC_SYSCALLS */


UK_TRACEPOINT(trace_vfs_getcwd, "%#x %zu", char*, size_t);
UK_TRACEPOINT(trace_vfs_getcwd_ret, "%zd", ssize_t);
UK_TRACEPOINT(trace_vfs_getcwd_err, "%d", int);

UK_SYSCALL_R_DEFINE(ssize_t, getcwd, char*, path, size_t, size)
{
	trace_vfs_getcwd(path, size);
	struct task *t = main_task;
	size_t len;
	int error;

	if (!path) {
		error = EFAULT;
		goto out_error;
	}
	if (!size) {
		error = EINVAL;
		goto out_error;
	}

	len = strlen(t->t_cwd) + 1;

	if (size < len) {
		error = ERANGE;
		goto out_error;
	}

	memcpy(path, t->t_cwd, len);
	trace_vfs_getcwd_ret(len);
	return len;

out_error:
	trace_vfs_getcwd_err(error);
	return -error;
}


/*
 * The file control system call.
 */
#define SETFL (O_APPEND | O_ASYNC | O_DIRECT | O_NOATIME | O_NONBLOCK)

UK_TRACEPOINT(trace_vfs_fcntl, "%p %d %#x", void *, int, int);
UK_TRACEPOINT(trace_vfs_fcntl_ret, "\"%s\"", int);
UK_TRACEPOINT(trace_vfs_fcntl_err, "%d", int);

int vfscore_fcntl(struct vfscore_file *fp, unsigned int cmd, unsigned long arg)
{
	int ret = 0, error = 0;
	int tmp, oldf;

	trace_vfs_fcntl(fp, cmd, arg);

	// An important note about our handling of FD_CLOEXEC / O_CLOEXEC:
	// close-on-exec shouldn't have been a file flag (fp->f_flags) - it is a
	// file descriptor flag, meaning that that two dup()ed file descriptors
	// could have different values for FD_CLOEXEC. Our current implementation
	// *wrongly* makes close-on-exec an f_flag (using the bit O_CLOEXEC).
	// There is little practical difference, though, because this flag is
	// ignored in OSv anyway, as it doesn't support exec().
	switch (cmd) {
	case F_GETFD:
		ret = (fp->f_flags & O_CLOEXEC) ? FD_CLOEXEC : 0;
		break;
	case F_SETFD:
		FD_LOCK(fp);
		fp->f_flags = (fp->f_flags & ~O_CLOEXEC) |
				((arg & FD_CLOEXEC) ? O_CLOEXEC : 0);
		FD_UNLOCK(fp);
		break;
	case F_GETFL:
		// As explained above, the O_CLOEXEC should have been in f_flags,
		// and shouldn't be returned. Linux always returns 0100000 ("the
		// flag formerly known as O_LARGEFILE) so let's do it too.
		ret = (vfscore_oflags(fp->f_flags) & ~O_CLOEXEC) | 0100000;
		break;
	case F_SETFL:
		FD_LOCK(fp);
		oldf = fp->f_flags;
		tmp  = vfscore_oflags(fp->f_flags) & ~SETFL;
		fp->f_flags = vfscore_fflags(tmp | (arg & SETFL));

		/* Sync nonblocking/async state with file flags
		 * To make sure that the actual state of the underlying file
		 * is in sync with the configured flag, we need to perform the
		 * ioctl while holding the lock. This is not optimal since we
		 * hold the lock for the duration of potentially expensive
		 * operations. It would be better to just set the flag here and
		 * make sure that all components directly consume this flag
		 * instead of synching it to other places.
		 */
		tmp = fp->f_flags & FNONBLOCK;
		if ((tmp ^ oldf) & FNONBLOCK) {
			error = vfs_ioctl(fp, FIONBIO, &tmp);
			if (unlikely(error)) {
				fp->f_flags = oldf;
				FD_UNLOCK(fp);
				break;
			}
		}

		tmp = fp->f_flags & FASYNC;
		if ((tmp ^ oldf) & FASYNC) {
			error = vfs_ioctl(fp, FIOASYNC, &tmp);
			if (unlikely(error)) {
				tmp = oldf & FNONBLOCK;
				if ((tmp ^ fp->f_flags) & FNONBLOCK)
					(void)vfs_ioctl(fp, FIONBIO, &tmp);

				fp->f_flags = oldf;
				FD_UNLOCK(fp);
				break;
			}
		}

		FD_UNLOCK(fp);
		break;
	case F_SETLK:
		uk_pr_warn_once("fcntl(F_SETLK) stubbed\n");
		break;
	case F_GETLK:
		uk_pr_warn_once("fcntl(F_GETLK) stubbed. Always unlocked\n");

		struct flock *flk = (struct flock *)arg;

		if (unlikely(!flk)) {
			error = EFAULT;
			goto out_errno;
		}

		/* For now, stubbing as always unlocked seems to be fine. We
		 * are, after all, a unikernel.
		 */
		flk->l_type = F_UNLCK;

		break;
	case F_SETLKW:
		uk_pr_warn_once("fcntl(F_SETLKW) stubbed\n");
		break;
	case F_SETOWN:
		uk_pr_warn_once("fcntl(F_SETOWN) stubbed\n");
		break;
	default:
		uk_pr_err("unsupported fcntl cmd %#x\n", cmd);
		error = EINVAL;
	}

	if (error)
		goto out_errno;
	trace_vfs_fcntl_ret(ret);
	return ret;

out_errno:
	trace_vfs_fcntl_err(error);
	return -error;
}

UK_TRACEPOINT(trace_vfs_access, "\"%s\" 0%0o", const char*, int);
UK_TRACEPOINT(trace_vfs_access_ret, "");
UK_TRACEPOINT(trace_vfs_access_err, "%d", int);

/*
 * Check permission for file access
 */
UK_SYSCALL_R_DEFINE(int, access, const char*, pathname, int, mode)
{
	trace_vfs_access(pathname, mode);
	struct task *t = main_task;
	char path[PATH_MAX];
	int acc, error = 0;

	acc = 0;
	if (mode & R_OK)
		acc |= VREAD;
	if (mode & W_OK)
		acc |= VWRITE;

	if ((error = task_conv(t, pathname, acc, path)) != 0)
		goto out_error;

	error = sys_access(path, mode);
	if (error)
		goto out_error;
	trace_vfs_access_ret();
	return 0;

	out_error:
	trace_vfs_access_err(error);
	return -error;
}

UK_SYSCALL_R_DEFINE(int, faccessat, int, dirfd, const char*, pathname, int, mode, int, flags)
{
	char tmp[PATH_MAX];
	const char *p;
	struct vfscore_file *fp;
	int error;

	if (pathname[0] == '/' ||
	    dirfd == AT_FDCWD) {	/* Full path inside pathname */
		p = pathname;
	} else { /* Create Relative Path */
		error = fget(dirfd, &fp);
		if (error)
			goto out_error;

		struct vnode *vp = fp->f_dentry->d_vnode;

		vn_lock(vp);

		/* build absolute path */
		strlcpy(tmp, fp->f_dentry->d_mount->m_path, PATH_MAX);
		strlcat(tmp, fp->f_dentry->d_path, PATH_MAX);
		strlcat(tmp, "/", PATH_MAX);
		strlcat(tmp, pathname, PATH_MAX);

		vn_unlock(vp);
		fdrop(fp);

		p = tmp;
	}

	if (flags & AT_SYMLINK_NOFOLLOW) {
		struct stat st;

		error = uk_syscall_do_lstat((long)p, (long)&st);
		/* Check if the file is an actual symlink */
		if (error == 0 && S_ISLNK(st.st_mode))
			UK_CRASH("UNIMPLEMENTED: faccessat() with AT_SYMLINK_NOFOLLOW\n");
	}

	error = uk_syscall_do_access((long)p, (long)mode);

out_error:
	return error;
}

int euidaccess(const char *pathname, int mode)
{
	return uk_syscall_do_access((long) pathname, (long) mode);
}

__weak_alias(euidaccess,eaccess);

#if 0
/*
 * Return if specified file is a tty
 */
int isatty(int fd)
{
	struct vfscore_file *fp;
	int istty = 0;

	trace_vfs_isatty(fd);
	fileref f(fileref_from_fd(fd));
	if (!f) {
		errno = EBADF;
		trace_vfs_isatty_err(errno);
		return -1;
	}

	fp = f.get();
	if (dynamic_cast<tty_file*>(fp) ||
		(fp->f_dentry && fp->f_dentry->d_vnode->v_flags & VISTTY)) {
		istty = 1;
	}

	trace_vfs_isatty_ret(istty);
	return istty;
}
#endif

UK_TRACEPOINT(trace_vfs_truncate, "\"%s\" %#x", const char*, off_t);
UK_TRACEPOINT(trace_vfs_truncate_ret, "");
UK_TRACEPOINT(trace_vfs_truncate_err, "%d", int);

UK_SYSCALL_R_DEFINE(int, truncate, const char*, pathname, off_t, length)
{
	trace_vfs_truncate(pathname, length);
	struct task *t = main_task;
	char path[PATH_MAX];
	int error;

	error = ENOENT;
	if (pathname == NULL)
		goto out_error;

	if ((error = task_conv(t, pathname, VWRITE, path)) != 0)
		goto out_error;

	error = sys_truncate(path, length);
	if (error)
		goto out_error;

	trace_vfs_truncate_ret();
	return 0;

	out_error:
	trace_vfs_truncate_err(error);
	return -error;
}

#ifdef truncate64
#undef truncate64
#endif

LFS64(truncate);

UK_SYSCALL_DEFINE(ssize_t, readlink, const char *, pathname, char *, buf, size_t, bufsize)
{
	struct task *t = main_task;
	char path[PATH_MAX];
	int error;
	ssize_t size;

	error = -EINVAL;
	if (bufsize <= 0)
		goto out_errno;

	error = ENOENT;
	if (pathname == NULL)
		goto out_errno;
	error = task_conv(t, pathname, VWRITE, path);
	if (error)
		goto out_errno;

	size  = 0;
	error = sys_readlink(path, buf, bufsize, &size);

	if (error != 0)
		goto out_errno;

	return size;
	out_errno:
	errno = error;
	return -1;
}

UK_TRACEPOINT(trace_vfs_utimes, "\"%s\"");
UK_TRACEPOINT(trace_vfs_utimes_ret, "");
UK_TRACEPOINT(trace_vfs_utimes_err, "%d", int);

int futimes(int fd, const struct timeval *times)
{
    return futimesat(fd, NULL, times);
}

UK_SYSCALL_DEFINE(int, futimesat, int, dirfd, const char*, pathname, const struct timeval*, times)
{
	struct stat st;
	struct vfscore_file *fp;
	int error;
	char *absolute_path;

	if ((pathname && pathname[0] == '/') || dirfd == AT_FDCWD)
		return utimes(pathname, times);

	// Note: if pathname == NULL, futimesat operates on dirfd itself, and in
	// that case it doesn't have to be a directory.
	if (pathname) {
		error = fstat(dirfd, &st);
		if (error) {
			error = errno;
			goto out_errno;
		}

		if (!S_ISDIR(st.st_mode)){
			error = ENOTDIR;
			goto out_errno;
		}
	}

	error = fget(dirfd, &fp);
	if (error)
		goto out_errno;

	/* build absolute path */
	absolute_path = (char*)malloc(PATH_MAX);
	if (!absolute_path) {
		fdrop(fp);
		error = EFAULT;
		goto out_errno;
	}

	strlcpy(absolute_path, fp->f_dentry->d_mount->m_path, PATH_MAX);
	strlcat(absolute_path, fp->f_dentry->d_path, PATH_MAX);

	if (pathname) {
		strlcat(absolute_path, "/", PATH_MAX);
		strlcat(absolute_path, pathname, PATH_MAX);
	}

	error = utimes(absolute_path, times);
	free(absolute_path);

	fdrop(fp);

	if (error)
		goto out_errno;
	return 0;

out_errno:
	errno = error;
	return -1;
}

UK_TRACEPOINT(trace_vfs_utimensat, "\"%s\"");
UK_TRACEPOINT(trace_vfs_utimensat_ret, "");
UK_TRACEPOINT(trace_vfs_utimensat_err, "%d", int);

UK_SYSCALL_R_DEFINE(int, utimensat, int, dirfd, const char*, pathname, const struct timespec*, times, int, flags)
{
	int error;

	trace_vfs_utimensat(pathname);

	error = sys_utimensat(dirfd, pathname, times, flags);

	if (error) {
		trace_vfs_utimensat_err(error);
		return -error;
	}

	trace_vfs_utimensat_ret();
	return 0;
}

#if UK_LIBC_SYSCALLS
UK_TRACEPOINT(trace_vfs_futimens, "%d");
UK_TRACEPOINT(trace_vfs_futimens_ret, "");
UK_TRACEPOINT(trace_vfs_futimens_err, "%d", int);

int futimens(int fd, const struct timespec *times)
{
	trace_vfs_futimens(fd);

	int error = sys_futimens(fd, times);
	if (error) {
		trace_vfs_futimens_err(error);
		errno = error;
		return -1;
	}

	trace_vfs_futimens_ret();
	return 0;
}
#endif /* UK_LIBC_SYSCALLS */

static int do_utimes(const char *pathname, const struct timeval *times, int flags)
{
	struct task *t = main_task;
	char path[PATH_MAX];
	int error;

	trace_vfs_utimes(pathname);

	error = task_conv(t, pathname, 0, path);
	if (error) {
		goto out_error;
	}

	error = sys_utimes(path, times, flags);
	if (error) {
		goto out_error;
	}

	trace_vfs_utimes_ret();
	return 0;

	out_error:
	trace_vfs_utimes_err(error);
	return -error;
}

UK_SYSCALL_R_DEFINE(int, utimes, const char*, pathname,
	const struct timeval*, times)
{
	return do_utimes(pathname, times, 0);
}

int lutimes(const char *pathname, const struct timeval *times)
{
	return do_utimes(pathname, times, AT_SYMLINK_NOFOLLOW);
}

UK_SYSCALL_R_DEFINE(int, utime, const char *, pathname,
		    const struct utimbuf *, t)
{
	if (t) {
		struct timeval times[2];
		times[0].tv_sec = t->actime;
		times[0].tv_usec = 0;
		times[1].tv_sec = t->modtime;
		times[1].tv_usec = 0;
		return uk_syscall_do_utimes((long) pathname, (long) times);
	} else {
		return uk_syscall_do_utimes((long) pathname, (long) NULL);
	}
}

UK_TRACEPOINT(trace_vfs_chmod, "\"%s\" 0%0o", const char*, mode_t);
UK_TRACEPOINT(trace_vfs_chmod_ret, "");
UK_TRACEPOINT(trace_vfs_chmod_err, "%d", int);

UK_SYSCALL_R_DEFINE(int, chmod, const char*, pathname, mode_t, mode)
{
	trace_vfs_chmod(pathname, mode);
	struct task *t = main_task;
	char path[PATH_MAX];
	int error = ENOENT;
	if (pathname == NULL)
		goto out_error;
	if ((error = task_conv(t, pathname, VWRITE, path)) != 0)
		goto out_error;
	error = sys_chmod(path, mode & UK_ALLPERMS);
	if (error)
		goto out_error;
	trace_vfs_chmod_ret();
	return 0;

out_error:
	trace_vfs_chmod_err(error);
	return -error;
}

UK_SYSCALL_R_DEFINE(int, chown, const char*, path, uid_t, owner, gid_t, group)
{
	UK_WARN_STUBBED();
	return 0;
}

UK_SYSCALL_R_DEFINE(int, lchown, const char*, path, uid_t, owner, gid_t, group)
{
	UK_WARN_STUBBED();
	return 0;
}


#if 0
ssize_t sendfile(int out_fd, int in_fd, off_t *_offset, size_t count)
{
	struct vfscore_file *in_fp;
	struct vfscore_file *out_fp;
	fileref in_f{fileref_from_fd(in_fd)};
	fileref out_f{fileref_from_fd(out_fd)};

	if (!in_f || !out_f) {
		return libc_error(EBADF);
	}

	in_fp = in_f.get();
	out_fp = out_f.get();

	if (!in_fp->f_dentry) {
		return libc_error(EBADF);
	}

	if (!(in_fp->f_flags & UK_FREAD))
		return libc_error(EBADF);

	if (out_fp->f_type & DTYPE_VNODE) {
		if (!out_fp->f_dentry)
			return libc_error(EBADF);
		else if (!(out_fp->f_flags & UK_FWRITE))
			return libc_error(EBADF);
	}

	off_t offset ;

	if (_offset != nullptr) {
		offset = *_offset;
	} else {
		/* if _offset is nullptr, we need to read from the present position of in_fd */
		offset = lseek(in_fd, 0, SEEK_CUR);
	}

	// Constrain count to the extent of the file...
	struct stat st;
	if (fstat(in_fd, &st) < 0) {
		return -1;
	} else {
		if (offset >= st.st_size) {
			return 0;
		} else if ((offset + count) >= st.st_size) {
			count = st.st_size - offset;
			if (count == 0) {
				return 0;
			}
		}
	}

	size_t bytes_to_mmap = count + (offset % mmu::page_size);
	off_t offset_for_mmap =  align_down(offset, (off_t)mmu::page_size);

	char *src = static_cast<char *>(mmap(nullptr, bytes_to_mmap, PROT_READ, MAP_SHARED, in_fd, offset_for_mmap));

	if (src == MAP_FAILED) {
		return -1;
	}

	int ret = write(out_fd, src + (offset % PAGESIZE), count);

	if (ret < 0) {
		return libc_error(errno);
	} else if(_offset == nullptr) {
		lseek(in_fd, ret, SEEK_CUR);
	} else {
		*_offset += ret;
	}

	assert(munmap(src, count) == 0);

	return ret;
}

#undef sendfile64

LFS64(sendfile);
#endif

#if UK_LIBC_SYSCALLS
int posix_fadvise(int fd __unused, off_t offset __unused, off_t len __unused,
		int advice)
{
	switch (advice) {
	case POSIX_FADV_NORMAL:
	case POSIX_FADV_SEQUENTIAL:
	case POSIX_FADV_RANDOM:
	case POSIX_FADV_NOREUSE:
	case POSIX_FADV_WILLNEED:
	case POSIX_FADV_DONTNEED:
		return 0;
	default:
		return EINVAL;
	}
}

#ifdef posix_fadvise64
#undef posix_fadvise64
#endif

LFS64(posix_fadvise);
#endif /* UK_LIBC_SYSCALLS */

UK_SYSCALL_R_DEFINE(mode_t, umask, mode_t, newmask)
{
	return uk_exchange_n(&global_umask, newmask);
}

int
fs_noop(void)
{
	return 0;
}

UK_SYSCALL_R_DEFINE(int, chroot, const char*, path)
{
	UK_WARN_STUBBED();
	return -ENOSYS;
}

static struct task _main_task_impl;
static void vfscore_init(void)
{
	memset(&_main_task_impl, 0, sizeof(_main_task_impl));
	strcpy(_main_task_impl.t_cwd, "/");
	main_task = &_main_task_impl;

	vnode_init();
	lookup_init();
}

UK_CTOR_PRIO(vfscore_init, 1);

int stat(const char *path, struct stat *st) {
    struct vnode *vp;
    int error;

    UK_SYSCALL_RET1(stat, path, st);

    if (path == NULL || st == NULL) {
        return -EFAULT;
    }

    if (path[0] == '\0') {
        return -ENOENT;
    }

    error = vfscore_lookup(path, 0, &vp);
    if (error) {
        return error;
    }

    error = vfscore_vn_getattr(vp, st);
    vfscore_vput(vp);
    return error;
}

int lstat(const char *path, struct stat *st) {
    struct vnode *vp;
    int error;

    UK_SYSCALL_RET1(lstat, path, st);

    if (path == NULL || st == NULL) {
        return -EFAULT;
    }

    if (path[0] == '\0') {
        return -ENOENT;
    }

    error = vfscore_lookup(path, LOOKUP_NOFOLLOW, &vp);
    if (error) {
        return error;
    }

    error = vfscore_vn_getattr(vp, st);
    vfscore_vput(vp);
    return error;
}

int fstatat(int dirfd, const char *path, struct stat *st, int flags) {
    struct vnode *vp;
    int error;

    UK_SYSCALL_RET1(fstatat, dirfd, path, st, flags);

    if (path == NULL || st == NULL) {
        return -EFAULT;
    }

    if (path[0] == '\0') {
        return -ENOENT;
    }

    error = vfscore_lookupat(dirfd, path, flags, &vp);
    if (error) {
        return error;
    }

    error = vfscore_vn_getattr(vp, st);
    vfscore_vput(vp);
    return error;
}

UK_SYSCALL_R_DEFINE(int, fstatat, int, dirfd, const char*, path,
				struct stat*, st, int, flags)
{
    int error;

    if (!path || !st)
        return -EFAULT;
    
    if (path[0] == '/' || dirfd == AT_FDCWD) {
        if (flags & AT_SYMLINK_NOFOLLOW)
            return uk_syscall_do_lstat((long) path, (long) st);
        else
            return uk_syscall_do_stat((long) path, (long) st);
    }
    
    // If AT_EMPTY_PATH and pathname is an empty string, fstatat() operates on
    // dirfd itself, and in that case it doesn't have to be a directory.
    if ((flags & AT_EMPTY_PATH) && !path[0]) {
        return uk_syscall_do_fstat((long) dirfd, (long) st);
    }

    struct vfscore_file *fp;
    error = fget(dirfd, &fp);
    if (error)
        return -error;

    struct vnode *vp = fp->f_dentry->d_vnode;
    vn_lock(vp);

    char p[PATH_MAX];
    /* build absolute path */
    strlcpy(p, fp->f_dentry->d_mount->m_path, PATH_MAX);
    strlcat(p, fp->f_dentry->d_path, PATH_MAX);
    strlcat(p, "/", PATH_MAX);
    strlcat(p, path, PATH_MAX);

    vn_unlock(vp);
    fdrop(fp);

    if (flags & AT_SYMLINK_NOFOLLOW)
        error = uk_syscall_do_lstat((long) p, (long) st);
    else
        error = uk_syscall_do_stat((long) p, (long) st);

    return error;
}

#ifdef fstatat64
#undef fstatat64
#endif

LFS64(fstatat);

UK_SYSCALL_R_DEFINE(int, newfstatat, int, dirfd, const char*, path,
                    struct stat*, st, int, flags)
{
    return uk_syscall_do_fstatat((long) dirfd, (long) path, (long) st, (long) flags);
}
