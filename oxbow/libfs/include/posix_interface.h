#ifndef _POSIX_INTERFACE_H_
#define _POSIX_INTERFACE_H_

#include <sys/stat.h>
#include <sys/types.h>
#include "types.h"

long libfs_posix_open(const char *path, int flags, umode_t mode);
int libfs_posix_openat(int dfd, const char *filename, int flags, umode_t mode);
int libfs_posix_access(const char *pathname, int mode);
int libfs_posix_creat(const char *path, umode_t mode);
ssize_t libfs_posix_read(int fd, char *buf, size_t count);
ssize_t libfs_posix_pread64(int fd, char *buf, size_t count, loff_t off);
ssize_t libfs_posix_write(int fd, const char *buf, size_t count);
ssize_t libfs_posix_pwrite64(int fd, const char *buf, size_t count, loff_t pos);
int libfs_posix_lseek(int fd, int64_t offset, int origin);
int libfs_posix_mkdir(const char *path, umode_t mode);
int libfs_posix_rmdir(const char *path);
int libfs_posix_close(int fd);
int libfs_posix_stat(const char *filename, struct stat *stat_buf);
int libfs_posix_fstat(int fd, struct stat *stat_buf);
int libfs_posix_newfstatat(int dirfd, const char *pathname,
			   struct stat *statbuf, int flags);
int libfs_posix_fallocate(int fd, off_t offset, off_t len);
int libfs_posix_unlink(const char *path);
int libfs_posix_truncate(const char *filename, off_t length);
int libfs_posix_ftruncate(int fd, off_t length);
int libfs_posix_rename(const char *oldname, const char *newname);
int libfs_posix_fsync(int fd);
int libfs_posix_sync(void);
void *libfs_posix_mmap(void *addr, size_t length, int prot, int flags, int fd,
		       off_t offset);
ssize_t libfs_posix_getdents(int fd, struct linux_dirent *buf, size_t nbytes);
ssize_t libfs_posix_getdents64(int fd, struct linux_dirent64 *buf, size_t nbytes);
int libfs_posix_fcntl(int fd, int cmd, void *arg);
// int libfs_posix_execve(const char *pathname, char *const *argv,
// 		       char *const *envp);

#endif
