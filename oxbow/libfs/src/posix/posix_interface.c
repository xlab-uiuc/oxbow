// File-system system calls.
// Mostly argument checking, since we don't trust
// user code, and calls into file.c and fs.c.
//
#define _GNU_SOURCE
#include <sys/stat.h>
#include "log.h"
#include <fcntl.h>
#include <stdio.h>
#include <errno.h>
#include <unistd.h>
#include <syscall.h>
#include <sys/mman.h>
#include <libsyscall_intercept_hook_point.h>

#include "global.h"
#include "oxbow_debug.h"
#include "posix_interface.h"
#include "fs.h"
#include "profile_libfs.h"
#include "msg.h"

#define LOG_BUFFER_SIZE 256
static inline void set_errno_from_negative(long ret)
{
	if (ret < 0)
		errno = syscall_error_code(ret);
}

void print_flags(int flags)
{
	char buffer[LOG_BUFFER_SIZE] = "open flags: ";
	size_t len = strlen(buffer);

	if ((flags & O_ACCMODE) == O_RDONLY)
		len += snprintf(buffer + len, LOG_BUFFER_SIZE - len,
				"O_RDONLY ");
	if ((flags & O_ACCMODE) == O_WRONLY)
		len += snprintf(buffer + len, LOG_BUFFER_SIZE - len,
				"O_WRONLY ");
	if ((flags & O_ACCMODE) == O_RDWR)
		len += snprintf(buffer + len, LOG_BUFFER_SIZE - len, "O_RDWR ");
	if (flags & O_CREAT)
		len += snprintf(buffer + len, LOG_BUFFER_SIZE - len,
				"O_CREAT ");
	if (flags & O_EXCL)
		len += snprintf(buffer + len, LOG_BUFFER_SIZE - len, "O_EXCL ");
	if (flags & O_NOCTTY)
		len += snprintf(buffer + len, LOG_BUFFER_SIZE - len,
				"O_NOCTTY ");
	if (flags & O_TRUNC)
		len += snprintf(buffer + len, LOG_BUFFER_SIZE - len,
				"O_TRUNC ");
	if (flags & O_APPEND)
		len += snprintf(buffer + len, LOG_BUFFER_SIZE - len,
				"O_APPEND ");
	if (flags & O_NONBLOCK)
		len += snprintf(buffer + len, LOG_BUFFER_SIZE - len,
				"O_NONBLOCK ");
	if (flags & O_SYNC)
		len += snprintf(buffer + len, LOG_BUFFER_SIZE - len, "O_SYNC ");
	if (flags & O_ASYNC)
		len += snprintf(buffer + len, LOG_BUFFER_SIZE - len,
				"O_ASYNC ");
	if (flags & O_LARGEFILE)
		len += snprintf(buffer + len, LOG_BUFFER_SIZE - len,
				"O_LARGEFILE ");
	if (flags & O_DIRECTORY)
		len += snprintf(buffer + len, LOG_BUFFER_SIZE - len,
				"O_DIRECTORY ");
	if (flags & O_NOFOLLOW)
		len += snprintf(buffer + len, LOG_BUFFER_SIZE - len,
				"O_NOFOLLOW ");
	if (flags & O_CLOEXEC)
		len += snprintf(buffer + len, LOG_BUFFER_SIZE - len,
				"O_CLOEXEC ");
	if (flags & O_DIRECT)
		len += snprintf(buffer + len, LOG_BUFFER_SIZE - len,
				"O_DIRECT ");
	if (flags & O_NOATIME)
		len += snprintf(buffer + len, LOG_BUFFER_SIZE - len,
				"O_NOATIME ");
	if (flags & O_PATH)
		len += snprintf(buffer + len, LOG_BUFFER_SIZE - len, "O_PATH ");
	if (flags & O_DSYNC)
		len += snprintf(buffer + len, LOG_BUFFER_SIZE - len,
				"O_DSYNC ");

	log_info("[%s] %s", __func__, buffer);
}

const char *get_file_type(umode_t mode)
{
	if (S_ISREG(mode))
		return "Regular File";
	if (S_ISDIR(mode))
		return "Directory";
	if (S_ISCHR(mode))
		return "Character Device";
	if (S_ISBLK(mode))
		return "Block Device";
	if (S_ISFIFO(mode))
		return "FIFO/Pipe";
	if (S_ISLNK(mode))
		return "Symbolic Link";
	if (S_ISSOCK(mode))
		return "Socket";
	return "Unknown";
}

void print_mode(umode_t mode)
{
	printf("File Type: %s\n", get_file_type(mode));
	printf("  Owner: %s%s%s\n", (mode & S_IRUSR) ? "read " : "",
	       (mode & S_IWUSR) ? "write " : "",
	       (mode & S_IXUSR) ? "execute" : "");

	printf("  Group: %s%s%s\n", (mode & S_IRGRP) ? "read " : "",
	       (mode & S_IWGRP) ? "write " : "",
	       (mode & S_IXGRP) ? "execute" : "");

	printf("  Others: %s%s%s\n", (mode & S_IROTH) ? "read " : "",
	       (mode & S_IWOTH) ? "write " : "",
	       (mode & S_IXOTH) ? "execute" : "");
}

/** 
 * @note: posix_debug with arguments make the error before main function.
 *	      printf is fine, so that use printf in libfs_posix_open
 */
long libfs_posix_open(const char *path, int flags, umode_t mode)
{
	long fd;
	long ret;
	int err_code;

	// PF_TL_START(evt_posix_open);
	posix_trace("[%s] path: %s, flags: 0x%x, mode: 0%o | (tid:%d)", __func__,
		    path, flags, mode, get_tid());
	// leveldb case test
	// if (strcmp(path, "/oxbow/1") == 0) {
	// 	print_flags(flags);
	// }
	// print_mode(mode);

	// WRONLY make mmap failed (in leveldb)
	if ((flags & O_ACCMODE) == O_WRONLY) {
		flags &= ~O_WRONLY;
		flags |= O_RDWR;
	}

	/* mkdir context */
	if (S_ISDIR(mode) && (flags & O_CREAT)) {
		struct lfs_inode *inode;
		// mode &= ~S_IFDIR;
		ret = syscall_no_intercept(SYS_mkdir, path, mode);
		if (ret < 0) {
			set_errno_from_negative(ret);
			err_code = syscall_error_code(ret);
			if (err_code != EEXIST) {
				oxb_warn("mkdir failed errno(%d) %s", err_code,
					 path);
			}
			posix_trace("DONE: [%s] path:%s ret:%d | (tid:%d)",
				    __func__, path, -1, get_tid());
			return -1;
		}

		posix_trace("SYS_mkdir path:%s --> ret(%ld) | (tid:%d)", path,
			    ret, get_tid());

		inode = get_lfs_inode(path);
		if (!inode) {
			log_error("get_lfs_inode fail");
			errno = ENOENT;
			posix_trace("DONE: [%s] path:%s ret:%d | (tid:%d)",
				    __func__, path, -1, get_tid());
			return -1;
		}
		inode->mode = mode; // must set S_IFDIR here!
		// put_lfs_inode(inode); <--- this will free inode (TODO)

		posix_trace("DONE: [%s] path:%s ret:%ld | (tid:%d)", __func__,
			    path, ret, get_tid());
		return ret; // mkdir just return 0 or -1 (no fd)
	}
	/* Get file descriptor from kernel, support regular file and directory
	 * This not directly notify daemon file be opened */
	else {
		// PF_TL_START(evt_libfs_open);
		fd = syscall_no_intercept(SYS_open, path, flags, mode);
		if (fd == -ENOENT) {
			set_errno_from_negative(fd);
			posix_trace("SYS_open ENOENT: %s", path);
			posix_trace("DONE: [%s] path:%s ret:%d | (tid:%d)",
				    __func__, path, -1, get_tid());
			return -1;
		} else if (fd < 0) {
			oxb_error("SYS_open failed ret=%d path=%s | (tid: %d)",
				  fd, path, get_tid());
			set_errno_from_negative(fd);
			posix_trace("DONE: [%s] path:%s ret:%d | (tid:%d)",
				    __func__, path, -1, get_tid());
			return -1;
		}
		// PF_TL_END(evt_libfs_open);
		posix_trace("SYS_open path:%s --> fd(%d) | (tid:%d)", path, fd,
			    get_tid());
	}

	/* Get libfs file struct according to file descriptor */
	ret = attach_lfs_file(fd, path, flags);
	if (ret < 0) {
		errno = EINVAL;
		oxb_error("attach_lfs_file failed ret=%ld", ret);
		goto err;
	}

	posix_trace("DONE: [%s] ops(%s) ret(%ld) | (tid:%d)\n", __func__,
		    flags & O_CREAT ? "create" : "open", fd + g_fd_start,
		    get_tid());

	// TMP: for debug.
	// struct lfs_file *f = get_lfs_file(fd);
	// struct lfs_inode *inode = f->f_inode;
	// if (!inode->shm_header)
	// 	init_shared_file_metadata(f);
	// oxb_warn("[%s] path(%s) inode(%lu) fd:%d p: 0x%lx", __func__,
	// 	    inode->path, inode->shm_header->i_ino, f->fd,
	// 	    inode->shm_header->daemon_inode_va);

	// PF_TL_END(evt_posix_open);
	return fd + g_fd_start;

err:
	syscall_no_intercept(SYS_close, fd);
	posix_trace("DONE: [%s] path:%s ret:%d | (tid:%d)", __func__, path, -1,
		    get_tid());
	return -1;
}

int libfs_posix_openat(int dfd, const char *filename, int flags, umode_t mode)
{
	struct lfs_file *f;
	char *dfd_fullpath;
	char fullpath[PATH_MAX];
	size_t dfd_len, filename_len;
	int ret = -1;

	posix_trace("[%s] dfd(%d) filename:%s flags:0x%x mode:0%o | (tid:%d)",
		    __func__, dfd, filename, flags, mode, get_tid());
	log_warn("[%s] not debugged yet", __func__);

	f = get_lfs_file(dfd);
	if (f == NULL) {
		errno = ENOENT;
		return -1;
	}

	if (!f->f_inode) {
		oxb_error("dfd inode is NULL");
		errno = ENOENT;
		ret = -1;
		goto put_file;
	}

	dfd_fullpath = f->f_inode->path;
	posix_trace("[%s] dfd(%d) fullpath:%s", __func__, dfd, dfd_fullpath);

	dfd_len = strlen(dfd_fullpath);
	filename_len = strlen(filename);

	// Check if combined path length exceeds PATH_MAX
	if (dfd_len + filename_len + 1 >= PATH_MAX) {
		errno = ENAMETOOLONG;
		ret = -1;
		goto put_file;
	}

	// Combine the directory full path and relative pathname
	memcpy(fullpath, dfd_fullpath, dfd_len);
	if (dfd_fullpath[dfd_len - 1] != '/') {
		fullpath[dfd_len++] = '/'; // Add '/' if not already present
	}
	memcpy(fullpath + dfd_len, filename, filename_len);
	fullpath[dfd_len + filename_len] = '\0'; // Null-terminate the full path

	oxb_debug("fullpath: %s", fullpath);

	ret = libfs_posix_open(fullpath, flags, mode);

put_file:
	put_lfs_file(f);
	posix_trace("DONE: [%s] dfd(%d) filename:%s ret:%d | (tid:%d)", __func__,
		    dfd, filename, ret, get_tid());
	return ret;
}

// FIXME: Is hooking necessary?
int libfs_posix_access(const char *pathname, int mode)
{
	int ret;

	posix_trace("[%s] path: %s | (tid:%d)", __func__, pathname, get_tid());

	ret = syscall_no_intercept(SYS_access, pathname, mode);
	if (ret < 0) {
		set_errno_from_negative(ret);
		ret = -1;
	}

	posix_trace("DONE: [%s] ret: %d | (tid:%d)\n", __func__, ret,
		    get_tid());

	return ret;
}

int libfs_posix_creat(const char *path, umode_t mode)
{
	int ret;

	posix_trace("[%s] path: %s, mode: 0%o | (tid:%d)", __func__, path, mode,
		    get_tid());

	ret = libfs_posix_open(path, O_CREAT | O_RDWR, mode);

	posix_trace("DONE: [%s] path: %s ret:%d | (tid:%d)", __func__, path,
		    ret, get_tid());

	return ret;
}

ssize_t libfs_posix_read(int fd, char *buf, size_t count)
{
	struct lfs_file *f;
	loff_t pos;
	ssize_t ret;

	posix_trace("[%s] fd: %d, count=%lu | (tid:%d)", __func__, fd, count,
		    get_tid());
	rw_debug("[tid:%d] read(fd: %d, count=%lu)", get_tid(), fd, count);

	f = get_lfs_file(fd);
	if (f == NULL) {
		errno = ENOENT;
		posix_trace("DONE: [%s] fd: %d ret:%d | (tid:%d)", __func__, fd,
			    -1, get_tid());
		return -1;
	}

	/* [TODO] update pos mechansim */
	pos = f->pos;
	ret = libfs_read(f, buf, count, &pos);
	if (ret > 0)
		f->pos = pos;
	else if (ret == 0)
		oxb_debug("nothing to read (EOF): path=%s pos=%lu",
			 f->f_inode->path, pos);
	else
		oxb_error("read failed: path=%s pos=%lu ret=%d",
			  f->f_inode->path, pos, ret);

	put_lfs_file(f);

	if (ret < 0) {
		errno = -ret;
		posix_trace("DONE: [%s] fd: %d ret:%d | (tid:%d)", __func__, fd,
			    -1, get_tid());
		return -1;
	}

	posix_trace("DONE: [%s] fd: %d ret:%zd | (tid:%d)", __func__, fd, ret,
		    get_tid());
	rw_debug("[tid:%d] read(fd: %d) ret %d", get_tid(), fd, ret);
	return ret;
}

ssize_t libfs_posix_pread64(int fd, char *buf, size_t count, loff_t pos)
{
	struct lfs_file *f;
	ssize_t ret;

	posix_trace("[%s] fd: %d, count=%lu, pos=%ld | (tid:%d)", __func__,
		    fd, count, pos, get_tid());
	rw_debug("[%s] (fd: %d, count=%lu) | (tid:%d)", __func__, fd, count,
		 get_tid());

	f = get_lfs_file(fd);
	if (f == NULL) {
		errno = ENOENT;
		posix_trace("DONE: [%s] fd: %d ret:%d | (tid:%d)", __func__, fd,
			    -1, get_tid());
		return -1;
	}

	ret = libfs_read(f, buf, count, &pos);

	if (ret < 0)
		oxb_error("pread64 failed: ret %d", ret);

	put_lfs_file(f);

	if (ret < 0) {
		errno = -ret;
		posix_trace("DONE: [%s] fd: %d ret:%d | (tid:%d)", __func__, fd,
			    -1, get_tid());
		return -1;
	}
	posix_trace("DONE: [%s] fd: %d ret:%zd | (tid:%d)", __func__, fd, ret,
		    get_tid());
	rw_debug("DONE: [%s] fd: %d, count=%lu, pos=%ld, ret:%zd | (tid:%d)\n",
		 __func__, fd, count, pos, ret, get_tid());
	return ret;
}

ssize_t libfs_posix_write(int fd, const char *buf, size_t count)
{
	struct lfs_file *f;
	loff_t pos;
	ssize_t ret;

	posix_trace("[%s] fd: %d, count=%lu | (tid:%d)", __func__, fd, count,
		    get_tid());

	f = get_lfs_file(fd);
	if (f == NULL)
	{
		posix_trace("DONE: [%s] fd: %d ret:%d | (tid:%d)", __func__, fd,
			    -ENOENT, get_tid());
		return -ENOENT;
	}

	if (f->pos + count > OXBOW_MAX_FILE_SIZE) {
		log_error("over oxbow file max size");
		errno = EFBIG;
		posix_trace("DONE: [%s] fd: %d ret:%d | (tid:%d)", __func__, fd,
			    -1, get_tid());
		return -1;
	}

	/* [TODO] update pos mechansim */
	pos = f->pos;
	ret = libfs_write(f, buf, count, &pos);
	if (ret > 0)
		f->pos = pos;
	else
		oxb_error("write failed: ret %d", ret);

	// put_lfs_file(f);

	if (ret < 0) {
		errno = -ret;
		posix_trace("DONE: [%s] fd: %d ret:%d | (tid:%d)", __func__, fd,
			    -1, get_tid());
		return -1;
	}

	posix_trace("DONE: [%s] fd: %d ret:%zd | (tid:%d)", __func__, fd, ret,
		    get_tid());
	rw_debug("DONE: [%s] fd: %d, count=%lu | (tid %d) \n", __func__, fd,
		 count, get_tid());
	return ret;
}

ssize_t libfs_posix_pwrite64(int fd, const char *buf, size_t count, loff_t pos)
{
	struct lfs_file *f;
	ssize_t ret;

	posix_trace("[%s] fd: %d, count=%lu, pos=%ld | (tid:%d)", __func__,
		    fd, count, pos, get_tid());
	rw_debug("[%s] fd: %d, count=%lu pos=%ld| tid:%d", __func__, fd, count,
		 pos, get_tid());

	f = get_lfs_file(fd);
	if (f == NULL)
	{
		posix_trace("DONE: [%s] fd: %d ret:%d | (tid:%d)", __func__, fd,
			    -ENOENT, get_tid());
		return -ENOENT;
	}

	if (pos + count > OXBOW_MAX_FILE_SIZE) {
		log_error("over oxbow file max size");
		errno = EFBIG;
		posix_trace("DONE: [%s] fd: %d ret:%d | (tid:%d)", __func__, fd,
			    -1, get_tid());
		return -1;
	}

	ret = libfs_write(f, buf, count, &pos);

	if (ret < 0)
		oxb_error("pwrite64 failed: ret %d", ret);

	put_lfs_file(f);

	if (ret < 0) {
		errno = -ret;
		posix_trace("DONE: [%s] fd: %d ret:%d | (tid:%d)", __func__, fd,
			    -1, get_tid());
		return -1;
	}
	posix_trace("DONE: [%s] fd: %d ret:%zd | (tid:%d)", __func__, fd, ret,
		    get_tid());
	rw_debug("DONE: [%s] fd: %d, count=%lu, pos=%ld, ret:%zd | (tid:%d)\n",
		 __func__, fd, count, pos, ret, get_tid());
	return ret;
}

int libfs_posix_lseek(int fd, int64_t offset, int origin)
{
	struct lfs_file *f;
	int ret = 0;

	posix_trace("[%s] fd: %d, offset: %ld, origin: %d | (tid:%d)", __func__,
		    fd, (long)offset, origin, get_tid());

	// posix_debug("[%s] fd(%d) ofs(%ld) %s", __func__, fd, offset,
	// 	    origin == SEEK_SET	 ? "SEEK_SET" :
	// 	    (origin == SEEK_CUR) ? "SEEK_CUR" :
	// 				   "SEEK_END");

	f = get_lfs_file(fd);
	if (f == NULL) {
		errno = ENOENT;
		posix_trace("DONE: [%s] fd: %d ret:%d | (tid:%d)", __func__, fd,
			    -1, get_tid());
		return -1;
	}

	switch (origin) {
	case SEEK_SET:
		f->pos = offset;
		break;
	case SEEK_CUR:
		f->pos += offset;
		break;
	case SEEK_END:
		f->pos = get_oxbow_isize(f);
		break;
	default:
		ret = -EINVAL;
		break;
	}

	put_lfs_file(f);

	if (ret < 0) {
		errno = -ret;
		posix_trace("DONE: [%s] fd: %d ret:%d | (tid:%d)", __func__, fd,
			    -1, get_tid());
		return -1;
	}

	posix_trace("DONE: [%s] fd: %d ret:%lld | (tid:%d)", __func__, fd,
		    (long long)f->pos, get_tid());
	return f->pos;
}

int libfs_posix_close(int fd)
{
	struct lfs_file *f;
	int ret;

	ret = -1;

	// PF_TL_START(close_a);

	posix_trace("[%s] fd: %d | (tid:%d)", __func__, fd, get_tid());

	f = get_lfs_file(fd);
	if (f == NULL) {
		errno = ENOENT;
		posix_trace("DONE: [%s] fd:%d ret:%d | tid(%d)\n", __func__, fd,
			    -1, get_tid());
		return -1;
	}

	close_daemon_file_key(f->f_inode, fd);

	// decrease inode reference and reclaim shared segments on last close
	put_lfs_inode(f->f_inode);
	f->f_inode = NULL;

	if (f->flags & O_DIRECTORY)
		goto skip_munmap;

	if (f->data)
		f->data = NULL;

skip_munmap:
	/* Prevent kernel give same fd be erased by this */
	// free_lfs_file(f)
	// PF_TL_START(close_ab_sysclose);
	ret = syscall_no_intercept(SYS_close, fd);
	if (ret < 0) {
		set_errno_from_negative(ret);
		ret = -1;
		log_error("close error");
		goto ret;
	}
	// PF_TL_END(close_ab_sysclose);

	f->state = 0;
	f->pos = 0;
	f->fd = 0;
	ret = 0; // on success
	// PF_TL_END(close_a);
ret:
	posix_trace("DONE: [%s] ret: %d | (tid:%d)\n", __func__, ret,
		    get_tid());
	return ret;
}

int libfs_posix_mkdir(const char *path, umode_t mode)
{
	int ret = -1;

	posix_trace("[%s] path: %s | (tid:%d)", __func__, path, get_tid());
	// print_mode(mode);

	ret = libfs_posix_open(path, O_CREAT | O_RDWR, mode | S_IFDIR);
	if (ret == g_fd_start)
		ret = 0;

	// Errno and return value are set in libfs_posix_open.

	posix_trace("[DONE:%s] path: %s --> ret(%d) | (tid:%d)\n", __func__,
		    path, ret, get_tid());
	return ret;
}

int libfs_posix_rmdir(const char *path)
{
	int ret = -1;

	posix_trace("[tid:%d] rmdir path: %s", get_tid(), path);

	ret = libfs_posix_unlink(path);

	// Errno and return value are set in libfs_posix_unlink.

	posix_trace("DONE: [%s] path: %s --> ret(%d) | (tid:%d)\n", __func__,
		    path, ret, get_tid());
	return ret;
}

int libfs_posix_stat(const char *filename, struct stat *stat_buf)
{
	int fd, ret;

	posix_trace("[%s] path(%s) | (tid:%d)", __func__, filename, get_tid());

	fd = libfs_posix_open(filename, O_RDONLY, 0);
	if (fd < 0) {
		oxb_error("failed to open file");

		// Errno and return value are set in libfs_posix_open.
		return fd;
	}

	fd -= g_fd_start;

	ret = libfs_posix_fstat(fd, stat_buf);

	if (libfs_posix_close(fd) < 0) {
		oxb_error("close failed", __func__);
		// Errno and return value are set in libfs_posix_close.

		return -1;
	}

	posix_trace("DONE: [%s] ret: %d | (tid: %d)", __func__, ret, get_tid());
	return ret;
}

void print_stat_struct(const struct stat *stat_buf)
{
	if (!stat_buf) {
		printf("stat_buf is NULL\n");
		return;
	}

	posix_debug("=== stat info ===\n");
	posix_debug("Device ID       : %ld\n", (long)stat_buf->st_dev);
	posix_debug("Inode number    : %ld\n", (long)stat_buf->st_ino);
	posix_debug("Mode (file type): %o\n", stat_buf->st_mode);
	posix_debug("Link count      : %ld\n", (long)stat_buf->st_nlink);
	posix_debug("UID             : %d\n", stat_buf->st_uid);
	posix_debug("GID             : %d\n", stat_buf->st_gid);
	posix_debug("Special device  : %ld\n", (long)stat_buf->st_rdev);
	posix_debug("File size       : %ld bytes\n", (long)stat_buf->st_size);
	posix_debug("Block size      : %ld\n", (long)stat_buf->st_blksize);
	posix_debug("Blocks allocated: %ld\n", (long)stat_buf->st_blocks);

	// Time information.
	posix_debug("Last access     : %s", ctime(&stat_buf->st_atime));
	posix_debug("Last modified   : %s", ctime(&stat_buf->st_mtime));
	posix_debug("Last status chg : %s", ctime(&stat_buf->st_ctime));
	posix_debug("=================\n");
}

int libfs_posix_fstat(int fd, struct stat *stat_buf)
{
	struct lfs_file *f;
	int ret;

	posix_trace("[%s] fd:%d | tid(%d)", __func__, fd, get_tid());

	f = get_lfs_file(fd);
	if (f == NULL) {
		errno = ENOENT;
		posix_trace("DONE: [%s] fd(%d) ret:%d | tid(%d)\n", __func__, fd,
			    -1, get_tid());
		return -1;
	}

	ret = libfs_file_fstat(f, stat_buf);
	if (ret < 0) {
		errno = -ret; // Only ENOENT is returned.
		ret = -1;
	} else {
		print_stat_struct(stat_buf);
	}

	put_lfs_file(f);

	posix_trace("DONE: [%s] fd:%d ret:%d | tid(%d)\n", __func__, fd, ret,
		    get_tid());
	return ret;
}

int libfs_posix_newfstatat(int dfd, const char *pathname, struct stat *statbuf,
			   int flags)
{
	int fd, ret;

	posix_trace("[%s] dfd(%d) path(%s) flags:%x start | tid(%d)", __func__,
		    dfd, pathname, flags, get_tid());

	/* AT_EMPTY_PATH: act on dfd itself only when pathname == "". */
	if ((flags & AT_EMPTY_PATH) && dfd > 0 && pathname &&
	    pathname[0] == '\0')
		// Errno and return value are set in libfs_posix_fstat.
		return libfs_posix_fstat(dfd, statbuf);

	/* We emulate newfstatat() via open + fstat().
	 * Thanks to illufs always mapping regular files as writable internally,
	 * we can safely use O_RDONLY here for both files and directories.
	 */
	if (dfd == AT_FDCWD)
		fd = libfs_posix_open(
			pathname,
			O_RDONLY | ((flags & AT_SYMLINK_NOFOLLOW) ? O_NOFOLLOW :
								    0),
			0);
	else
		fd = libfs_posix_openat(
			dfd, pathname,
			O_RDONLY | ((flags & AT_SYMLINK_NOFOLLOW) ? O_NOFOLLOW :
								    0),
			0);
	if (fd < 0) {
		posix_trace("Failed to open file.");
		// Errno and return value are set in libfs_posix_open/openat.
		return fd;
	}

	fd -= g_fd_start;

	ret = libfs_posix_fstat(fd, statbuf);

	if (libfs_posix_close(fd) < 0) {
		oxb_error("close failed", __func__);
		ret = -1;
	}

	posix_trace("DONE: [%s] ret: %d | (tid: %d)", __func__, ret, get_tid());
	return ret;
}

int libfs_posix_fallocate(int fd, off_t offset, off_t len)
{
	posix_trace("[%s] fd: %d, offset: %ld, len: %ld | (tid:%d)", __func__,
		    fd, offset, len, get_tid());
	// TODO
	oxb_error("%s not implemented", __func__);
	errno = ENOSYS;
	posix_trace("DONE: [%s] fd: %d ret:%d | (tid:%d)", __func__, fd, -1,
		    get_tid());
	return -1;
}

void *libfs_posix_mmap(void *addr, size_t length, int prot, int flags, int fd,
		       off_t offset)
{
	void *ret;

	if (!(flags & MAP_ANONYMOUS || fd == -1)) {
		oxb_error("filebacked memory not implemented");
		posix_trace(
			"DONE: [%s] addr:%p length:%zu prot:%d flags:0x%x fd:%d offset:%ld ret:%p | (tid:%d)",
			__func__, addr, length, prot, flags, fd, (long)offset,
			MAP_FAILED, get_tid());
		return MAP_FAILED;
	}
	posix_trace(
		"[%s] addr:%p length:%zu prot:%d flags:0x%x fd:%d offset:%ld | (tid:%d)",
		__func__, addr, length, prot, flags, fd, (long)offset,
		get_tid());

	ret = (void *)syscall_no_intercept(SYS_mmap, addr, length, prot, flags,
					   fd, offset);
	if ((long)ret < 0) {
		set_errno_from_negative((long)ret);
		ret = (void *)-1;
	}

	posix_trace("DONE: [%s] mmap: %p | (tid:%d)\n", __func__, ret,
		    get_tid());

	return ret;
}

int libfs_posix_unlink(const char *path)
{
	int ret = -1;

	posix_trace("[%s] path: %s | (tid:%d)", __func__, path, get_tid());

	// Remove from cache if it exists.
	remove_lfs_inode(path);

	ret = syscall_no_intercept(SYS_unlink, path);
	if (ret < 0) {
		set_errno_from_negative(ret);
		ret = -1;
	}

	posix_trace("DONE: [%s] ret:%d | (tid:%d)\n", __func__, ret, get_tid());

	return ret;
}

int libfs_posix_truncate(const char *filename, off_t length)
{
	posix_trace("[%s] path: %s, length: %ld | (tid:%d)", __func__, filename,
		    length, get_tid());
	// TODO
	oxb_error("%s not implemented", __func__);
	errno = ENOSYS;
	posix_trace("DONE: [%s] path: %s ret:%d | (tid:%d)", __func__, filename,
		    -1, get_tid());
	return -1;
}

int libfs_posix_ftruncate(int fd, off_t length)
{
	posix_trace("[%s] fd: %d, length: %ld | (tid:%d)", __func__, fd, length,
		    get_tid());
	// TODO
	oxb_error("%s not implemented", __func__);
	errno = ENOSYS;
	posix_trace("DONE: [%s] fd: %d ret:%d | (tid:%d)", __func__, fd, -1,
		    get_tid());
	return -1;
}

int libfs_posix_rename(const char *oldpath, const char *newpath)
{
	int ret;

	posix_trace("[%s] (old:%s, new:%s)", __func__, oldpath, newpath);

	ret = rename_lfs_inode(oldpath, newpath);
	if (ret == 0) {
		errno = ENOENT;
		oxb_warn("ENOENT: failed to get inode");
		return -1;
	} else if (ret < 0) {
		oxb_error("failed to rename inode. ");
		errno = -ENOENT; // FIXME: Need to set proper error code.
		return -1;
	}

	ret = syscall_no_intercept(SYS_rename, oldpath, newpath);
	if (ret < 0) {
		set_errno_from_negative(ret);
		ret = -1;
	}

	posix_trace("DONE: [%s] ret:%d\n", __func__, ret);

	return ret;
}

// #define KERNEL_PATH_FSYNC
int libfs_posix_fsync(int fd)
{
	struct lfs_file *f;
	int ret;

	PF_TL_START(b_evt_fsync);

	posix_trace("[%s] fd: %d | (tid:%d)", __func__, fd, get_tid());

	f = get_lfs_file(fd);
	if (f == NULL) {
		errno = ENOENT;
		return -1;
	}

	ret = libfs_file_fsync(f);
	if (ret < 0) {
		errno = -ret;
		ret = -1;
	}

	put_lfs_file(f);

	PF_TL_END(b_evt_fsync);

	posix_trace("DONE: [%s] fd(%d) ret:%d | tid(%d) \n", __func__, fd, ret,
		    get_tid());

	return ret;
}

int libfs_posix_sync(void)
{
	posix_trace("[POSIX] sync() flush all dirty data");
	oxb_error("sync() not implemented");
	errno = ENOSYS;
	posix_trace("DONE: [%s] ret:%d | (tid:%d)", __func__, -1, get_tid());
	return -1;

	// int ret = libfs_sync();
	// if (ret < 0) {
	// 	errno = -ret;
	// 	ret = -1;
	// }
	// return ret;
}

ssize_t libfs_posix_getdents(int fd, struct linux_dirent *buf, size_t nbytes)
{
	posix_trace("[%s] fd(%d) nbytes(%lu) | (tid:%d)", __func__, fd, nbytes,
		    get_tid());
	(void)buf;
	oxb_error("getdents not implemented");
	errno = ENOSYS;
	posix_trace("DONE: [%s] fd(%d) ret:%d | (tid:%d)", __func__, fd, -1,
		    get_tid());
	return -1;
	// return (size_t)sizeof(struct linux_dirent);
}

ssize_t libfs_posix_getdents64(int fd, struct linux_dirent64 *buf, size_t nbytes)
{
	ssize_t ret;

	posix_trace("[%s] fd(%d) nbytes(%zu) | (tid:%d)", __func__, fd, nbytes,
		    get_tid());

	ret = syscall_no_intercept(SYS_getdents64, fd, buf, nbytes);
	if (ret < 0) {
		set_errno_from_negative(ret);
		ret = -1;
	}

	posix_trace("DONE: [%s] ret(%ld) | (tid:%d)\n", __func__, ret,
		    get_tid());

	return ret;
}

// FIXME: Is hooking necessary?
int libfs_posix_fcntl(int fd, int cmd, void *arg)
{
	int ret;

	posix_trace("[%s] fd(%d) cmd(%d) arg(%p)", __func__, fd, cmd, arg);

	ret = syscall_no_intercept(SYS_fcntl, fd, cmd, arg);
	if (ret < 0) {
		set_errno_from_negative(ret);
		ret = -1;
	}

	posix_trace("[DONE:%s] fd(%d) --> ret(%d)\n", __func__, fd, ret);

	return ret;
}

// int libfs_posix_execve(const char *pathname, char *const *argv,
// 		       char *const *envp)
// {
// 	// TODO
// 	oxb_error("%s not implemented", __func__);
// 	return -1;
// }
