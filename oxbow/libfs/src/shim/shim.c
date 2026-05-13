#include <fcntl.h>
#include <libsyscall_intercept_hook_point.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <syscall.h>
#include <syslog.h>
#include <errno.h>

#include "posix_interface.h"
#include "global.h"
#include "oxbow.h"
#include "oxbow_libfs.h"

#define PATH_BUF_SIZE 4095
#define syscall_trace(...)

static inline int valid_fd(int fd)
{
#ifdef CHECK_FD
	if (fd >= g_fd_start)
		return 1;
	else
		return 0;
#else
	return 1;
#endif
}

static inline int get_oxbow_fd(int fd)
{
#ifdef CHECK_FD
	if (fd >= g_fd_start)
		return fd - g_fd_start;
	else
		return fd;
#else
	return fd;
#endif
}

static int collapse_name(const char *input, char *_output)
{
	char *output = _output;

	while (1) {
		/* Detect a . or .. component */
		if (input[0] == '.') {
			if (input[1] == '.' && input[2] == '/') {
				/* A .. component */
				if (output == _output)
					return -1;
				input += 2;
				while (*(++input) == '/')
					;
				while (--output != _output &&
				       *(output - 1) != '/')
					;
				continue;
			} else if (input[1] == '/') {
				/* A . component */
				input += 1;
				while (*(++input) == '/')
					;
				continue;
			}
		}

		/* Copy from here up until the first char of the next component */
		while (1) {
			*output++ = *input++;
			if (*input == '/') {
				*output++ = '/';
				/* Consume any extraneous separators */
				while (*(++input) == '/')
					;
				break;
			} else if (*input == 0) {
				*output = 0;
				return output - _output;
			}
		}
	}
}

// Check whether the input path starts with designated prefix.
static inline int check_prefix(const char *filename)
{
#ifdef CHECK_PREFIX
	char path_buf[PATH_BUF_SIZE];

	memset(path_buf, 0, PATH_BUF_SIZE);
	collapse_name(filename, path_buf);

	if (strncmp(path_buf, OXBOW_PREFIX, 5) == 0)
		return 1;

	return 0;
#else
	return 1;
#endif
}

static int shim_do_open(const char *filename, int flags, umode_t mode,
			long *result)
{
	long ret;

	if (!check_prefix(filename))
		return 1;

	ret = libfs_posix_open(filename, flags, mode);

	if (!valid_fd(ret))
		printf("incorrect fd %ld: file %s\n", ret, filename);

	syscall_trace(__func__, ret, 3, filename, flags, mode);

	if (ret == -1 && errno > 0)
		*result = -errno;
	else
		*result = ret;

	return 0;
}

static int shim_do_openat(int dfd, const char *filename, int flags,
			  umode_t mode, long *result)
{
	long ret;

	if (!check_prefix(filename))
		return 1;

	if (dfd == AT_FDCWD)
		ret = libfs_posix_open((char *)filename, flags, mode);
	else
		ret = libfs_posix_openat(dfd, (char *)filename, flags, mode);

	if (!valid_fd(ret))
		printf("incorrect fd %ld: file %s\n", ret, filename);

	syscall_trace(__func__, ret, 4, filename, dfd, flags, mode);

	if (ret == -1 && errno > 0)
		*result = -errno;
	else
		*result = ret;

	return 0;
}

int shim_do_creat(char *filename, umode_t mode, long *result)
{
	long ret;

	if (!check_prefix(filename))
		return 1;

	ret = libfs_posix_creat(filename, mode);

	if (!valid_fd(ret))
		printf("incorrect fd %ld\n", ret);

	syscall_trace(__func__, ret, 2, filename, mode);

	if (ret == -1 && errno > 0)
		*result = -errno;
	else
		*result = ret;

	return 0;
}

static int shim_do_read(int fd, void *buf, size_t count, size_t *result)
{
	ssize_t ret;

	if (valid_fd(fd)) {
		ret = libfs_posix_read(get_oxbow_fd(fd), buf, count);
		syscall_trace(__func__, ret, 3, fd, buf, count);

		if (ret == -1 && errno > 0)
			*result = (size_t)-errno;
		else
			*result = (size_t)ret;

		return 0;
	} else {
		return 1;
	}
}

static int shim_do_pread64(int fd, void *buf, size_t count, loff_t off,
			   size_t *result)
{
	ssize_t ret;

	if (valid_fd(fd)) {
		ret = libfs_posix_pread64(get_oxbow_fd(fd), buf, count, off);
		syscall_trace(__func__, ret, 4, fd, buf, count, off);

		if (ret == -1 && errno > 0)
			*result = (size_t)-errno;
		else
			*result = (size_t)ret;

		return 0;
	} else {
		return 1;
	}
}

static int shim_do_write(int fd, const void *buf, size_t count, size_t *result)
{
	ssize_t ret;

	if (valid_fd(fd)) {
		ret = libfs_posix_write(get_oxbow_fd(fd), buf, count);
		syscall_trace(__func__, ret, 3, fd, buf, count);

		if (ret == -1 && errno > 0)
			*result = (size_t)-errno;
		else
			*result = (size_t)ret;

		return 0;
	} else {
		return 1;
	}
}

static int shim_do_pwrite64(int fd, const void *buf, size_t count, loff_t pos,
			    size_t *result)
{
	ssize_t ret;

	if (valid_fd(fd)) {
		ret = libfs_posix_pwrite64(get_oxbow_fd(fd), buf, count, pos);
		syscall_trace(__func__, ret, 4, fd, buf, count, off);

		if (ret == -1 && errno > 0)
			*result = (size_t)-errno;
		else
			*result = (size_t)ret;

		return 0;
	} else {
		return 1;
	}
}

int shim_do_close(int fd, int *result)
{
	int ret;

	if (valid_fd(fd)) {
		ret = libfs_posix_close(get_oxbow_fd(fd));
		syscall_trace(__func__, ret, 1, fd);

		if (ret == -1 && errno > 0)
			*result = -errno;
		else
			*result = ret;

		return 0;
	} else {
		return 1;
	}
}

int shim_do_lseek(int fd, off_t offset, int origin, int *result)
{
	int ret;

	if (valid_fd(fd)) {
		ret = libfs_posix_lseek(get_oxbow_fd(fd), offset, origin);
		syscall_trace(__func__, ret, 3, fd, offset, origin);

		if (ret == -1 && errno > 0)
			*result = -errno;
		else
			*result = ret;

		return 0;
	} else {
		return 1;
	}
}

int shim_do_mkdir(void *path, mode_t mode, int *result)
{
	int ret;

	if (!check_prefix(path))
		return 1;

	ret = libfs_posix_mkdir(path, mode);
	syscall_trace(__func__, ret, 2, path, mode);

	if (ret == -1 && errno > 0)
		*result = -errno;
	else
		*result = ret;

	return 0;
}

int shim_do_rmdir(const char *path, int *result)
{
	int ret;

	if (!check_prefix(path))
		return 1;

	ret = libfs_posix_rmdir((char *)path);
	if (ret == -1 && errno > 0)
		*result = -errno;
	else
		*result = ret;

	return 0;
}

int shim_do_rename(char *oldname, char *newname, int *result)
{
	int ret;

	if (!check_prefix(oldname)) // Do we need to check newname too?
		return 1;

	ret = libfs_posix_rename(oldname, newname);
	syscall_trace(__func__, ret, 2, oldname, newname);

	if (ret == -1 && errno > 0)
		*result = -errno;
	else
		*result = ret;

	return 0;
}

int shim_do_fallocate(int fd, int mode, off_t offset, off_t len, int *result)
{
	int ret;

	if (valid_fd(fd)) {
		ret = libfs_posix_fallocate(get_oxbow_fd(fd), offset, len);
		syscall_trace(__func__, ret, 4, fd, mode, offset, len);

		if (ret == -1 && errno > 0)
			*result = -errno;
		else
			*result = ret;

		return 0;
	} else {
		return 1;
	}
}

int shim_do_stat(const char *filename, struct stat *statbuf, int *result)
{
	int ret;

	if (!check_prefix(filename))
		return 1;

	ret = libfs_posix_stat(filename, statbuf);
	syscall_trace(__func__, ret, 2, filename, statbuf);

	if (ret < 0)
		*result = -errno;
	else
		*result = ret;

	return 0;
}

int shim_do_lstat(const char *filename, struct stat *statbuf, int *result)
{
	int ret;

	if (!check_prefix(filename))
		return 1;

	// Symlink does not implemented yet
	// so stat and lstat is identical now.
	ret = libfs_posix_stat(filename, statbuf);
	syscall_trace(__func__, ret, 2, filename, statbuf);

	if (ret < 0)
		*result = -errno;
	else
		*result = ret;

	return 0;
}

int shim_do_fstat(int fd, struct stat *statbuf, int *result)
{
	int ret;

	if (valid_fd(fd)) {
		ret = libfs_posix_fstat(get_oxbow_fd(fd), statbuf);
		syscall_trace(__func__, ret, 2, fd, statbuf);

		if (ret < 0)
			*result = -errno;
		else
			*result = ret;

		return 0;
	} else {
		return 1;
	}
}

int shim_do_newfstatat(int dirfd, const char *pathname, struct stat *statbuf,
	       int flags, int *result)
{
	int ret;
	int libfs_dirfd;

	/* Accept AT_FDCWD as-is; otherwise validate and translate oxbow fds */
	if (dirfd == AT_FDCWD && check_prefix(pathname))
		libfs_dirfd = AT_FDCWD;
	else if (valid_fd(dirfd))
		libfs_dirfd = get_oxbow_fd(dirfd);
	else
		return 1;

	ret = libfs_posix_newfstatat(libfs_dirfd, pathname, statbuf, flags);

	syscall_trace(__func__, ret, 4, dirfd, pathname, statbuf, flags);

	if (ret < 0)
		*result = -errno;
	else
		*result = ret;

	return 0;

}

int shim_do_truncate(const char *filename, off_t length, int *result)
{
	int ret;

	if (!check_prefix(filename))
		return 1;

	ret = libfs_posix_truncate(filename, length);
	syscall_trace(__func__, ret, 2, filename, length);

	if (ret == -1 && errno > 0)
		*result = -errno;
	else
		*result = ret;

	return 0;
}

int shim_do_ftruncate(int fd, off_t length, int *result)
{
	int ret;

	if (valid_fd(fd)) {
		ret = libfs_posix_ftruncate(get_oxbow_fd(fd), length);
		syscall_trace(__func__, ret, 2, fd, length);

		if (ret == -1 && errno > 0)
			*result = -errno;
		else
			*result = ret;

		return 0;
	} else {
		return 1;
	}
}

int shim_do_unlink(const char *path, int *result)
{
	int ret;

	if (!check_prefix(path))
		return 1;

	ret = libfs_posix_unlink(path);
	syscall_trace(__func__, ret, 1, path);
	if (ret == -1 && errno > 0)
		*result = -errno;
	else
		*result = ret;

	return 0;
}

int shim_do_symlink(const char *target, const char *linkpath, int *result)
{
	int ret;

	if (!check_prefix(target))
		return 1;

	printf("%s\n", target);
	printf("symlink: do not support yet\n");
	exit(-1);
}

int shim_do_access(const char *pathname, int mode, int *result)
{
	int ret;

	if (!check_prefix(pathname))
		return 1;

	ret = libfs_posix_access((char *)pathname, mode);
	syscall_trace(__func__, ret, 2, pathname, mode);

	if (ret == -1 && errno > 0)
		*result = -errno;
	else
		*result = ret;

	return 0;
}

int shim_do_fsync(int fd, int *result)
{
	int ret;

	if (valid_fd(fd)) {
		ret = libfs_posix_fsync(get_oxbow_fd(fd));
		syscall_trace(__func__, ret, 1, fd);
		if (ret == -1 && errno > 0)
			*result = -errno;
		else
			*result = ret;

		return 0;
	} else {
		return 1;
	}
}

int shim_do_fdatasync(int fd, int *result)
{
	int ret;

	if (valid_fd(fd)) {
		ret = libfs_posix_fsync(get_oxbow_fd(fd));
		syscall_trace(__func__, 0, 1, fd);

		if (ret == -1 && errno > 0)
			*result = -errno;
		else
			*result = ret;

		return 0;
	} else {
		return 1;
	}
}

int shim_do_sync(int *result)
{
	int ret;

	ret = libfs_posix_sync();
	if (ret == -1 && errno > 0)
		*result = -errno;
	else
		*result = ret;

	return 0;
}

int shim_do_fcntl(int fd, int cmd, void *arg, int *result)
{
	int ret;

	if (valid_fd(fd)) {
		ret = libfs_posix_fcntl(get_oxbow_fd(fd), cmd, arg);
		syscall_trace(__func__, ret, 3, fd, cmd, arg);

		if (ret == -1 && errno > 0)
			*result = -errno;
		else
			*result = ret;

		return 0;
	} else {
		return 1;
	}
}

int shim_do_mmap(void *addr, size_t length, int prot, int flags, int fd,
		 off_t offset, void **result)
{
	void *ret;

	if (valid_fd(fd)) {
		printf("filebacked memory(mmap): not implemented\n");
		*result = (void *)-1;
		return 0;
	}
	/* We have to handle anonymous mmap */
	else {
		// ret = libfs_posix_mmap(addr, length, prot, flags, fd, offset);
		// syscall_trace(__func__, ret, 6, addr, length, prot, flags, fd,
		// 	      offset);

		// *result = ret;
		return 1;
	}
}

int shim_do_munmap(void *addr, size_t length, int *result)
{
	return 1;
}

int shim_do_getdents(int fd, struct linux_dirent *buf, size_t count,
		     size_t *result)
{
	// TODO
	ssize_t ret;

	if (valid_fd(fd)) {
		ret = libfs_posix_getdents(get_oxbow_fd(fd), buf, count);

		syscall_trace(__func__, ret, 3, fd, buf, count);

		if (ret == -1 && errno > 0)
			*result = (size_t)-errno;
		else
			*result = (size_t)ret;

		return 0;
	} else {
		return 1;
	}
}

int shim_do_getdents64(int fd, struct linux_dirent64 *buf, size_t count,
		       size_t *result)
{
	// TODO
	ssize_t ret;

	if (valid_fd(fd)) {
		ret = libfs_posix_getdents64(get_oxbow_fd(fd), buf, count);

		syscall_trace(__func__, ret, 3, fd, buf, count);

		if (ret == -1 && errno > 0)
			*result = (size_t)-errno;
		else
			*result = (size_t)ret;

		return 0;
	} else {
		return 1;
	}
}

// int shim_do_execve(const char *pathname, char *const *argv, char *const *envp,
// 		   int *result)
// {
// 	int ret;

// 	ret = libfs_posix_execve(pathname, argv, envp);

// 	syscall_trace(__func__, ret, 3, pathname, argv, envp);

// 	*result = ret;
// 	return 0;
// }

/**
 * @brief DO NOT USE PRINTF HERE! IT WILL MAKE SEGFAULT. 
 * 
 */
static int hook(long syscall_number, long arg0, long arg1, long arg2, long arg3,
		long arg4, long arg5, long *result)
{
	ALL_UNUSED(arg4, arg5);

	/* [note] return 1 will call normal route for syscall
		if program dead before launch main, suspect the return value */

	switch (syscall_number) {
	case SYS_open:
		return shim_do_open((char *)arg0, (int)arg1, (umode_t)arg2,
				    (long *)result);
	case SYS_openat:
		return shim_do_openat((int)arg0, (const char *)arg1, (int)arg2,
				      (umode_t)arg3, (long *)result);
	case SYS_openat2:
		return shim_do_openat((int)arg0, (const char *)arg1, (int)arg2,
				      (umode_t)arg3, (long *)result);
	case SYS_creat:
		return shim_do_creat((char *)arg0, (umode_t)arg1,
				     (long *)result);
	case SYS_read:
		return shim_do_read((int)arg0, (void *)arg1, (size_t)arg2,
				    (size_t *)result);
	case SYS_pread64:
		return shim_do_pread64((int)arg0, (void *)arg1, (size_t)arg2,
				       (loff_t)arg3, (size_t *)result);
	case SYS_write:
		return shim_do_write((int)arg0, (void *)arg1, (size_t)arg2,
				     (size_t *)result);
	case SYS_pwrite64:
		return shim_do_pwrite64((int)arg0, (void *)arg1, (size_t)arg2,
					(loff_t)arg3, (size_t *)result);
	case SYS_close:
		return shim_do_close((int)arg0, (int *)result);
	case SYS_lseek:
		return shim_do_lseek((int)arg0, (off_t)arg1, (int)arg2,
				     (int *)result);
	case SYS_mkdir:
		return shim_do_mkdir((void *)arg0, (mode_t)arg1, (int *)result);
	case SYS_rmdir:
		return shim_do_rmdir((const char *)arg0, (int *)result);
	case SYS_rename:
		return shim_do_rename((char *)arg0, (char *)arg1,
				      (int *)result);
	case SYS_fallocate:
		return shim_do_fallocate((int)arg0, (int)arg1, (off_t)arg2,
					 (off_t)arg3, (int *)result);
	case SYS_stat:
		return shim_do_stat((const char *)arg0, (struct stat *)arg1,
				    (int *)result);
#ifdef SYS_stat64
	case SYS_stat64:
		return shim_do_stat((const char *)arg0, (struct stat *)arg1,
				    (int *)result);
#endif
	case SYS_lstat:
		return shim_do_lstat((const char *)arg0, (struct stat *)arg1,
				     (int *)result);
#ifdef SYS_lstat64
	case SYS_lstat64:
		return shim_do_lstat((const char *)arg0, (struct stat *)arg1,
				     (int *)result);
#endif
	case SYS_fstat:
		return shim_do_fstat((int)arg0, (struct stat *)arg1,
				     (int *)result);
#ifdef SYS_fstat64
	case SYS_fstat64:
		return shim_do_fstat((int)arg0, (struct stat *)arg1,
				     (int *)result);
#endif
	case SYS_newfstatat:
		return shim_do_newfstatat((int)arg0, (const char *)arg1,
					  (struct stat *)arg2, (int)arg3,
					  (int *)result);
#ifdef SYS_fstatat64
	case SYS_fstatat64:
		return shim_do_newfstatat((int)arg0, (const char *)arg1,
					  (struct stat *)arg2, (int)arg3,
					  (int *)result);
#endif
	case SYS_truncate:
		return shim_do_truncate((const char *)arg0, (off_t)arg1,
					(int *)result);
	case SYS_ftruncate:
		return shim_do_ftruncate((int)arg0, (off_t)arg1, (int *)result);
	case SYS_unlink:
		return shim_do_unlink((const char *)arg0, (int *)result);
	case SYS_symlink:
		return shim_do_symlink((const char *)arg0, (const char *)arg1,
				       (int *)result);
	case SYS_access:
		return shim_do_access((const char *)arg0, (int)arg1,
				      (int *)result);
	case SYS_fsync:
		return shim_do_fsync((int)arg0, (int *)result);
	case SYS_fdatasync:
		return shim_do_fdatasync((int)arg0, (int *)result);
	case SYS_sync:
		return shim_do_sync((int *)result);
	case SYS_fcntl:
		return shim_do_fcntl((int)arg0, (int)arg1, (void *)arg2,
				     (int *)result);
	case SYS_mmap:
		return shim_do_mmap((void *)arg0, (size_t)arg1, (int)arg2,
				    (int)arg3, (int)arg4, (off_t)arg5,
				    (void **)result);
	case SYS_munmap:
		return shim_do_munmap((void *)arg0, (size_t)arg1,
				      (int *)result);
	case SYS_getdents:
		return shim_do_getdents((int)arg0, (struct linux_dirent *)arg1,
					(size_t)arg2, (size_t *)result);
	case SYS_getdents64:
		return shim_do_getdents64((int)arg0,
					  (struct linux_dirent64 *)arg1,
					  (size_t)arg2, (size_t *)result);
	// case SYS_execve:
	// 	return shim_do_execve((const char *)arg0, (char *)arg1,
	// 			      (char *)arg2, (int *)result);
	default:
		return 1;
	}
}

static __attribute__((constructor)) void init(void)
{
#if 0
	const char *disable_init;

	disable_init = getenv("MLFS_DISABLE_INIT");

	// FIXME: automatically calling init can cause issues with fork and also with
	// gdb Figure out a way to get around this
	if (!disable_init) {
		// initiate libfs
		init_libfs();
	}

	// Set up the callback function
	intercept_hook_point = hook;

	if (!disable_init) {
		// shutdown upon exit
		exit(-1);
		// atexit(shutdown_fs);
	}

#else
	// Set up the callback function
	intercept_hook_point = hook;
#endif
}
