/* open file */
#include "fs.h"
#include "log.h"
#include "oxbow_debug.h"
#include <errno.h>
#include <pthread.h>
#include <libsyscall_intercept_hook_point.h>
#include <stdatomic.h>
#include <stdio.h>
#include <syscall.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include "msg.h"
#include "oxbow_kernel.h"
#include "profile_libfs.h"
#include "common/dirty_mgmt.h"
#include <time.h>
#include <unistd.h>

static struct open_file_table g_fd_table = { 0 };

atomic_int g_shm_counter;
struct timespec micro_block = { .tv_sec = 0, .tv_nsec = 1000 };

void init_libfs_fdt(void)
{
	int i;
	// memset(g_fd_table.open_files, 0,
	//        sizeof(struct lfs_file *) * OXBOW_MAX_OPEN_FILE);

	for (i = 0; i < OXBOW_MAX_OPEN_FILE; i++) {
		atomic_init(&g_fd_table.open_files[i].ref_cnt, 0);
		pthread_spin_init(&g_fd_table.open_files[i].f_lock,
				  PTHREAD_PROCESS_PRIVATE);
	}
	pthread_spin_init(&g_fd_table.lock, PTHREAD_PROCESS_PRIVATE);
	atomic_init(&g_shm_counter, 0);
	return;
}

void init_libfs_fdt_key_map(void *shm, int index)
{
	if (g_fd_table.fd_key_map != NULL) {
		libfs_info("This process already set p(%d), skip it", getpid());
		return;
	}
	g_fd_table.fd_key_map = shm;
	g_fd_table.index_from_daemon = index;
	l_debug("fd_key_map: 0x%lx index(%d)", shm, index);
}

int get_daemon_file_key(int fd)
{
	// int closed = -2;

	// if (atomic_compare_exchange_strong(&g_fd_table.fd_key_map[fd], &closed,
	// 				   -1))
	// 	return -1;
	// else

	return atomic_load(&g_fd_table.fd_key_map[fd]);
}

void close_daemon_file_key(struct lfs_inode *li, int fd)
{
	bool wait_for_close = false;

	pthread_spin_lock(&li->li_lock);
	if (li->li_state & LFS_INODE_INIT) {
		wait_for_close = true;
		li->li_state &= ~LFS_INODE_INIT;
	}
	pthread_spin_unlock(&li->li_lock);
	if (!wait_for_close)
		return;

	spin_dbg("[%s] fd:%d closed wait for it", __func__, fd);
	while (atomic_load(&g_fd_table.fd_key_map[fd]) == -1)
		;
	atomic_store(&g_fd_table.fd_key_map[fd], -1);
	spin_dbg("[%s] fd:%d closed done", __func__, fd);
}

/**
 * @brief Allocate new file object when file is opened by application
 *
 * @return struct lfs_file* 
 */
int attach_lfs_file(int fd, const char *path, int flags)
{
	struct lfs_inode *inode;
	struct lfs_file *f;
	bool skip = true;

	f = &g_fd_table.open_files[fd];
	if (f->fd || f->data || f->f_inode)
		log_warn("file(%d) not closed well...", fd);

	/* get libfs inode (if not exist then allocate new one) */
	inode = get_lfs_inode(path);
	if (!inode) {
		log_error("get_lfs_inode fail");
		return -1;
	}

	pthread_spin_lock(&inode->li_lock);
	if (inode->li_state & LFS_INODE_FREE) {
		// todo: counter?
		inode->li_state |= LFS_INODE_INIT;
		skip = false;
	}
	pthread_spin_unlock(&inode->li_lock);
	if (skip)
		goto skip;

	// Notify daemon we opened this file, daemon will attach metadata asychronously
	l_debug("pid(%d) fd(%d)", getpid(), fd);
	if (syscall_no_intercept(SYS_ioctl, fd, ILLUFS_IOCTL_NOTIFY,
				 ((unsigned long)getpid() << 32) | fd)) {
		log_error("ioctl fail");
		return -1;
	}

skip:
	f->fd = fd;
	f->pos = 0;
	f->data = NULL;
	f->flags = flags;
	f->f_inode = inode;

	return 0;
}

/**
 * @brief Get file struct, it must be initiated before this function called.
 *
 * @return struct lfs_file*
 */
struct lfs_file *get_lfs_file(int fd)
{
	struct lfs_file *f;

	if ((fd < 0) | (fd > OXBOW_MAX_OPEN_FILE)) {
		oxb_error("(fd: %d) over open file or overflow", fd);
		return NULL;
	}

	f = &g_fd_table.open_files[fd];
	if (!f->fd)
		return NULL;

	// atomic_fetch_add(&f->ref_cnt, 1);
	return f;
}

void put_lfs_file(struct lfs_file *f)
{
	atomic_fetch_sub(&f->ref_cnt, 1);
}

void increase_file_ref_cnt(struct lfs_file *f)
{
	atomic_fetch_add(&f->ref_cnt, 1);
}

/**
 * @brief Decrease file reference counter.
 * 
 * @param f 
 * @return uint32_t Return the reference counter value before decreasing it.
 */
uint32_t decrease_file_ref_cnt(struct lfs_file *f)
{
	uint32_t orig_cnt;
	orig_cnt = atomic_fetch_sub(&f->ref_cnt, 1);

	return orig_cnt;
}

int file_do_mmap(struct lfs_file *f)
{
	struct lfs_inode *inode = f->f_inode;
	void *data = NULL;
	long ret;
	int prot;
	bool new = false;

	pthread_spin_lock(&inode->li_lock);
	data = inode->data;
	pthread_spin_unlock(&inode->li_lock);

	if (data) {
		f->data = data;
		return 0;
	}

	if (S_ISDIR(inode->mode))
		return 0;

	if ((f->flags & O_ACCMODE) == O_RDONLY)
		prot = PROT_READ;
	else
		prot = PROT_WRITE | PROT_READ;

	/* Max size to avoid mremap overhead, it is okay since we manage file size in user */
	ret = syscall_no_intercept(SYS_mmap, 0, OXBOW_MAX_FILE_SIZE, prot,
				   MAP_SHARED, f->fd, 0);
	if (ret < 0) {
		int err = syscall_error_code(ret);

		if (err == 0)
			err = EINVAL;

		if (err == ENODEV) {
			/* mmap not supported on this file (likely directory).
			 * Mark inode as directory and set O_DIRECTORY to avoid
			 * further mmap attempts on this fd. */
			pthread_spin_lock(&inode->li_lock);
			if (!S_ISDIR(inode->mode))
				inode->mode |= S_IFDIR;
			pthread_spin_unlock(&inode->li_lock);

			f->flags |= O_DIRECTORY;

			errno = err;
			l_debug("mmap ENODEV fd(%d) (ret=%ld) treat as directory: file={ptr:%p, flags:0x%x, pos:%lld, state:%d, inode:%p, inode_path:%s, inode_state:0x%x}",
				f->fd, ret, (void *)f, f->flags,
				(long long)f->pos, f->state, (void *)inode,
				inode ? inode->path : "NULL",
				(unsigned int)(inode ? inode->li_state : 0));

			return -1;
		}

		errno = err;
		log_error(
			"mmap fail fd(%d) (ret=%ld, errno=%d) file={ptr:%p, flags:0x%x, pos:%lld, state:%d, inode:%p, inode_path:%s, inode_state:0x%x}",
			f->fd, ret, err, (void *)f, f->flags, (long long)f->pos,
			f->state, (void *)inode, inode ? inode->path : "NULL",
			(unsigned int)(inode ? inode->li_state : 0));
		perror("mmap");
		return -1;
	}
	data = (void *)ret;
	pthread_spin_lock(&inode->li_lock);
	if (inode->data)
		new = true;
	else
		inode->data = data;
	f->data = inode->data;
	pthread_spin_unlock(&inode->li_lock);

	if (new) {
		oxb_warn("Already mapped by other thread. Unmap new one.");
		syscall_no_intercept(SYS_munmap, data, OXBOW_MAX_FILE_SIZE);
	}

	return 0;
}

int init_shared_file_metadata(struct lfs_file *f)
{
	struct lfs_inode *inode = f->f_inode;
	char name[10];
	void *ptr;
	int daemon_fd = -1;
	int shm_fd, ret = -1;

	PF_TL_START(shm_connection);

	PF_TL_START(shma_loop);
	l_debug("[%s]", __func__);
retry:
	daemon_fd = get_daemon_file_key(f->fd);
	if (daemon_fd < 0) {
		if (f->data == NULL && !(f->flags & O_DIRECTORY)) {
			file_do_mmap(f);
			goto retry;
		}
		// nanosleep(&micro_block, NULL);
		// log_debug("[%s] fd(%d) not ready: %d", __func__, f->fd,
		// 	  daemon_fd);
		goto retry;
		// How can we get shm without knowing ino
		// else
		// 	msg_send_sd_getshm(f, &daemon_fd);
	}
	PF_TL_END(shma_loop);
	l_debug("[%s] we got %d", __func__, daemon_fd);
	// name will be a file descriptor of daemon
	sprintf(name, "ox_%d", daemon_fd);

	PF_TL_START(shmb_open);
	shm_fd = shm_open(name, O_RDWR, 0666);
	if (shm_fd == -1) {
		log_error("name(%s)", name);
		perror("shm_open failed");
		return ret;
	}
	PF_TL_END(shmb_open);

	PF_TL_START(shmc_mmap);
	ptr = mmap(0, SHM_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd, 0);
	if (ptr == MAP_FAILED) {
		perror("mmap failed");
		close(shm_fd);
		goto free;
	}
	PF_TL_END(shmc_mmap);

	pthread_spin_lock(&inode->li_lock);
	if (inode->li_state & LFS_INODE_OPEN) {
		/* initiation is already done by other thread */
		ret = 0;
		goto free;
	}
	inode->li_state |= LFS_INODE_OPEN;
	inode->li_state &= ~LFS_INODE_FREE;
	inode->shm_header = (struct shm_shared_state *)ptr;
	inode->shm_fd = shm_fd;
	ret = 0;

	pthread_spin_unlock(&inode->li_lock);
	l_debug("[%s] inode(%s) fd(%d) daemon(%d) ptr(0x%lx)", __func__,
		inode->path, f->fd, daemon_fd, (unsigned long)ptr);
	PF_TL_END(shm_connection);
	return ret;

free:
	pthread_spin_unlock(&inode->li_lock);
	close(shm_fd);
	munmap(ptr, SHM_SIZE);
	return ret;
}

int libfs_file_fsync(struct lfs_file *f)
{
	struct lfs_inode *inode;
	int ret;

	inode = f->f_inode;
	if (!inode)
		return -ENOENT;

	// if (S_ISDIR(inode->mode)) {
	// 	if (inode->ino == 0) {
	// 		log_error("ino is not set");
	// 		return -ENOENT;
	// 	}
	// 	// TODO:
	// 	return 0;
	// 	// return msg_send_sd_ino_fsync(inode->ino);
	// }

	PF_TL_START(b_evt_fsync_shm);
	if (!inode->shm_header)
		if (init_shared_file_metadata(f) < 0)
			return -EINVAL;
	PF_TL_END(b_evt_fsync_shm);

	// Clear dirty page counter.
	// NOTE: It is not precise. There can be many more dirty pages before
	// this request is handled by secure daemon.
	clear_process_dirty_counter();

	l_debug("[%s] path(%s) inode(%lu) fd:%d p: 0x%lx", __func__,
		    inode->path, inode->shm_header->i_ino, f->fd,
		    inode->shm_header->daemon_inode_va);

	PF_TL_START(b_evt_fsync_msg);
	ret = msg_send_sd_fsync(inode->shm_header->daemon_inode_va);
	PF_TL_END(b_evt_fsync_msg);

	return ret;
}

int libfs_sync(void)
{
	msg_send_sd_sync();

	return 0;
}

// int libfs_file_fstat(struct lfs_file *f, void *stat_buf)
// {
// 	struct lfs_inode *inode;

// 	inode = f->f_inode;
// 	if (!inode)
// 		return -ENOENT;

// 	if (S_ISDIR(inode->mode))
// 		return msg_send_sd_ino_fstat(inode->ino, stat_buf);

// 	BUG_ON(!inode->shm_header, "shm NULL");

// 	return msg_send_sd_fstat(inode->shm_header->daemon_inode_va, stat_buf);
// }

int libfs_file_fstat(struct lfs_file *f, struct stat *stat_buf)
{
	struct lfs_inode *inode;

	posix_debug("[%s] f(%s)", __func__, f->f_inode->path);
	inode = f->f_inode;
	if (!inode)
		return -ENOENT;

	if (!inode->shm_header)
		init_shared_file_metadata(f);

	// log_debug("[%s] path(%s) inode(%lu) p: 0x%lx", __func__, inode->path,
	// 	  inode->ino, inode->shm_header);
	stat_buf->st_ino = inode->shm_header->i_ino;
	stat_buf->st_nlink = inode->shm_header->i_nlink;
	stat_buf->st_mode = inode->shm_header->i_mode;
	stat_buf->st_uid = inode->shm_header->i_uid;
	stat_buf->st_gid = inode->shm_header->i_gid;
	stat_buf->st_size = get_oxbow_isize(f);
	stat_buf->st_blksize = OXBOW_BLOCK_SIZE;
	stat_buf->st_blocks = inode->shm_header->i_blocks;

	return 0;
}

int libfs_file_fallocate(struct lfs_file *f)
{
	struct lfs_inode *inode;

	inode = f->f_inode;
	if (!inode)
		return -ENOENT;

	if (f->flags & O_DIRECTORY) {
		log_error("directory falloc is not impleneted yet");
		return -EINVAL;
	}

	return msg_send_sd_fallocate(inode->shm_header->daemon_inode_va);
}

int libfs_file_ftruncate(struct lfs_file *f)
{
	struct lfs_inode *inode;

	inode = f->f_inode;
	if (!inode)
		return -ENOENT;

	if (f->flags & O_DIRECTORY) {
		log_error("directory ftrunc is not impleneted yet");
		return -EINVAL;
	}

	return msg_send_sd_ftruncate(inode->shm_header->daemon_inode_va);
}
