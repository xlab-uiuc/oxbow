#include "diropsfd.h"
#include <linux/limits.h>
#include <semaphore.h>
#include <stdatomic.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <pthread.h>

#include "thpool.h"
#include "oxbow_debug.h"
#include "fs.h"
#include "kernfs.h"
#include "linux/diropsfd.h"
#include "profile_secure_daemon.h"

PF_TL_EVT(fm_poll);
PF_TL_EVT(fm_read);
PF_TL_EVT(fm_revive);
PF_TL_EVT(fm_fdmap);

atomic_int g_dir_worker_count;
atomic_int g_dir_worker_exit_count;
threadpool g_dir_worker_thpool;
threadpool g_dir_worker_init_thpool;
bool manager_is_working = false;

// Shared epoll dispatcher for directory workers
#define DIR_EPOLL_MAX_EVENTS 256
static int g_dir_epoll_fd = -1;
static int g_dir_eventfd = -1; // wake/shutdown notifier
static pthread_mutex_t g_dir_epoll_mutex = PTHREAD_MUTEX_INITIALIZER;

static void *dir_epoll_loop(void *arg);
static int dir_dispatcher_init(void);
static int dir_dispatcher_register(struct inode *dir);
static int dir_dispatcher_deregister(struct inode *dir);

static void dir_epoll_loop_runner(void *arg)
{
	(void)arg;
	(void)dir_epoll_loop(NULL);
}

/* forward declarations for internal handlers used before their definitions */
static int kreq_drop_pagecache(struct inode *root);
static int kreq_mount(struct inode *inode, kaddr_t kret);
static int kreq_create(struct inode *dir, mode_t mode, kaddr_t kret,
		       kaddr_t ino_p, char *name);
static int kreq_lookup(struct inode *dir, kaddr_t kret, kaddr_t ino_p,
		       char *name);
static int kreq_readdir(struct inode *dir, kaddr_t k_names, kaddr_t k_inos,
			kaddr_t k_nr, unsigned long pos);
static int kreq_unlink(struct inode *dir, char *name, kaddr_t kret);
static int kreq_rename_new(struct inode *dir, char *name, kaddr_t kret,
			   unsigned long new_ino);
static int kreq_rename_old(struct inode *dir, char *name, kaddr_t kret,
			   unsigned long ino);
void clear_dir_dofd(struct inode *inode);

static int dir_dispatcher_init(void)
{
	int epfd, efd;
	struct epoll_event ev = { 0 };

	epfd = epoll_create1(EPOLL_CLOEXEC);
	if (epfd < 0) {
		perror("epoll_create1(dir)");
		return -1;
	}

	efd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
	if (efd < 0) {
		perror("eventfd(dir)");
		close(epfd);
		return -1;
	}

	ev.events = EPOLLIN;
	ev.data.ptr = NULL; // sentinel for eventfd
	if (epoll_ctl(epfd, EPOLL_CTL_ADD, efd, &ev) < 0) {
		perror("epoll_ctl ADD eventfd(dir)");
		close(efd);
		close(epfd);
		return -1;
	}

	g_dir_epoll_fd = epfd;
	g_dir_eventfd = efd;

	for (int i = 0; i < OXBOW_DIRWORKER_NR; i++)
		thpool_add_work(g_dir_worker_thpool, dir_epoll_loop_runner,
				NULL);

	return 0;
}

static int dir_dispatcher_register(struct inode *dir)
{
	struct epoll_event ev = { 0 };
	int ret;

	iget(dir);

	ev.events = EPOLLIN | EPOLLERR | EPOLLHUP | EPOLLRDHUP | EPOLLET |
		    EPOLLONESHOT;
	ev.data.ptr = (void *)dir;

	pthread_mutex_lock(&g_dir_epoll_mutex);
	ret = epoll_ctl(g_dir_epoll_fd, EPOLL_CTL_ADD, dir->dofd, &ev);
	pthread_mutex_unlock(&g_dir_epoll_mutex);

	if (ret < 0) {
		perror("epoll_ctl ADD dir fd");
		iput(dir);
		return -1;
	}

	/* mark as registered */
	inode_lock(dir);
	dir->i_state |= I_HAS_WORKER;
	inode_unlock(dir);

	atomic_fetch_add(&g_dir_worker_count, 1);

	return 0;
}

static int dir_dispatcher_deregister(struct inode *dir)
{
	int ret;
	inode_lock(dir);
	if (dir->i_state & I_CLOSING) {
		inode_unlock(dir);
		return 0;
	}
	dir->i_state |= I_CLOSING;
	int active = !!(dir->i_state & I_IO_ACTIVE);
	inode_unlock(dir);

	pthread_mutex_lock(&g_dir_epoll_mutex);
	ret = epoll_ctl(g_dir_epoll_fd, EPOLL_CTL_DEL, dir->dofd, NULL);
	pthread_mutex_unlock(&g_dir_epoll_mutex);
	if (ret < 0) {
		perror("epoll_ctl DEL dir fd");
	}

	/* clear registration bit */
	inode_lock(dir);
	dir->i_state &= ~I_HAS_WORKER;
	inode_unlock(dir);

	if (active)
		return 0; // cleanup deferred to epoll thread

	clear_dir_dofd(dir);
	atomic_fetch_add(&g_dir_worker_exit_count, 1);
	return 0;
}

static void *dir_epoll_loop(void *arg)
{
	struct epoll_event events[DIR_EPOLL_MAX_EVENTS];
	(void)arg;

	for (;;) {
		int n = epoll_wait(g_dir_epoll_fd, events, DIR_EPOLL_MAX_EVENTS,
				   -1);
		if (n < 0) {
			if (errno == EINTR)
				continue;
			perror("epoll_wait(dir)");
			break;
		}

		for (int i = 0; i < n; i++) {
			struct epoll_event *ev = &events[i];
			bool need_rearm = false;

			if (ev->data.ptr == NULL) {
				uint64_t v;
				if (read(g_dir_eventfd, &v, sizeof(v)) < 0) {
					// ignore
					// TODO: control epoll.
					oxb_warn("Nothing to do with eventfd.");
				}
				continue;
			}

			struct inode *di = (struct inode *)ev->data.ptr;

			if (ev->events & EPOLLIN) {
				inode_lock(di);
				di->i_state |= I_IO_ACTIVE;
				inode_unlock(di);
				need_rearm = true;
				for (;;) {
					struct dirops_msg msg;
					ssize_t r = read(di->dofd, &msg,
							 sizeof(msg));
					if (r == 0)
						break;
					if (r < 0) {
						if (errno == EAGAIN ||
						    errno == EWOULDBLOCK)
							break;
						perror("read(dir)");
						oxb_error(
							"Epoll read error inode(%lu)",
							di->i_ino);
						break;
					}
					d_debug("[%s] dir(%lu) get event(%s) read %lu",
						__func__, di->i_ino,
						GET_ILLUFS_EVENT_NAME(
							msg.event),
						r);

					switch (msg.event) {
					case ILLUFS_DROP_PGCACHE:
						kreq_drop_pagecache(di);
						break;
					case ILLUFS_MOUNT:
						kreq_mount(di, msg.ret);
						break;
					case ILLUFS_DIROP_CREATE:
						if (kreq_create(
							    di, msg.reserved2,
							    msg.ret,
							    msg.arg.create.ino,
							    msg.arg.create.name))
							log_error(
								"k_event_create fail");
						break;
					case ILLUFS_DIROP_LOOKUP:
						if (kreq_lookup(
							    di, msg.ret,
							    msg.arg.lookup.ino,
							    msg.arg.lookup.name))
							log_error(
								"kreq_lookup fail");
						break;
					case ILLUFS_DIROP_UNLINK:
						if (kreq_unlink(
							    di,
							    msg.arg.unlink.name,
							    msg.arg.unlink.ret))
							log_error(
								"kreq_unlink fail");
						break;
					case ILLUFS_DIROP_READDIR:
						if (kreq_readdir(
							    di,
							    msg.arg.readdir
								    .names,
							    msg.arg.readdir.inos,
							    msg.arg.readdir.nr,
							    msg.arg.readdir.pos))
							log_error(
								"k_event_readdir fail");
						break;
					case ILLUFS_RENAME_NEW:
						if (kreq_rename_new(
							    di,
							    msg.arg.rename_new
								    .name,
							    msg.arg.rename_new
								    .ret,
							    msg.arg.rename_new
								    .ino))
							log_error(
								"kreq_rename_new fail");
						break;
					case ILLUFS_RENAME_OLD:
						if (kreq_rename_old(
							    di,
							    msg.arg.rename_old
								    .name,
							    msg.arg.rename_old
								    .ret,
							    msg.arg.rename_old
								    .ino))
							log_error(
								"kreq_rename_new fail");
						break;
					case ILLUFS_DIROPS_NOTIFYDIROPEN:
						register_file_proc_fdmap(
							di,
							msg.arg.notify_open.fd,
							msg.arg.notify_open.pid);
						break;
					default:
						log_error("unintend case %d",
							  msg.event);
						break;
					}
				}
			} else if (ev->events & EPOLLOUT) {
				oxb_warn(
					"EPOLLOUT is actually used. Deregistering dir from epoll.");
				dir_dispatcher_deregister(di);
				need_rearm = false;
			} else if (ev->events &
				   (EPOLLHUP | EPOLLERR | EPOLLRDHUP)) {
				oxb_error(
					"Unknown event: EPOLLHUP | EPOLLERR | EPOLLRDHUP dir(%lu)",
					di->i_ino);
				panic("Unknown epoll event type.");
				// dir_dispatcher_deregister(di);
				// need_rearm = false;
			}

			// closing path
			{
				bool closing_now;
				inode_lock(di);
				closing_now = (di->i_state & I_CLOSING);
				inode_unlock(di);
				if (closing_now) {
					struct epoll_event dummy = { 0 };
					pthread_mutex_lock(&g_dir_epoll_mutex);
					if (epoll_ctl(g_dir_epoll_fd,
						      EPOLL_CTL_DEL, di->dofd,
						      &dummy) < 0) {
						// ignore
					}
					pthread_mutex_unlock(
						&g_dir_epoll_mutex);
					inode_lock(di);
					di->i_state &= ~I_IO_ACTIVE;
					inode_unlock(di);
					clear_dir_dofd(di);
					continue;
				}
			}

			// EPOLLONESHOT rearm if still registered
			if (need_rearm) {
				struct epoll_event rev = { 0 };
				rev.events = EPOLLIN | EPOLLERR | EPOLLHUP |
					     EPOLLRDHUP | EPOLLET |
					     EPOLLONESHOT;
				rev.data.ptr = (void *)di;
				pthread_mutex_lock(&g_dir_epoll_mutex);
				if (epoll_ctl(g_dir_epoll_fd, EPOLL_CTL_MOD,
					      di->dofd, &rev) < 0)
					perror("epoll_ctl MOD dir fd");
				pthread_mutex_unlock(&g_dir_epoll_mutex);
			}
			inode_lock(di);
			di->i_state &= ~I_IO_ACTIVE;
			inode_unlock(di);
		}
	}
	return NULL;
}

int init_dir_workers(void)
{
	atomic_init(&g_dir_worker_count, 0);
	atomic_init(&g_dir_worker_exit_count, 0);

	g_dir_worker_thpool = thpool_init(OXBOW_DIRWORKER_NR, "oxb_d_worker");
	if (!g_dir_worker_thpool) {
		oxb_error("thpool_init failed");
		return -1;
	}

	g_dir_worker_init_thpool = thpool_init(1, "oxb_init_d_worker");
	if (!g_dir_worker_init_thpool) {
		oxb_error("thpool_init failed");
		return -1;
	}

	if (dir_dispatcher_init() < 0) {
		oxb_error("dir dispatcher init failed");
		return -1;
	}
	return 0;
}

int g_mount = false;

int dir_inode_lock(struct inode *inode)
{
	return pthread_rwlock_wrlock(&inode->i_dir_lock);
}

int dir_inode_rdlock(struct inode *inode)
{
	return pthread_rwlock_rdlock(&inode->i_dir_lock);
}

int dir_inode_unlock(struct inode *inode)
{
	return pthread_rwlock_unlock(&inode->i_dir_lock);
}

/**
 * @brief kernel got drop_pagecache request, for dropping cache
 *	 daemon must release page cache of the file, even it is closed.
 */
static int kreq_drop_pagecache(struct inode *root)
{
	struct illufs_dirops_ret ret = { 0 };
	// struct super_block *sb = root->i_sb;
	// struct inode *inode = NULL;

	oxb_info("drop_pagecache not implemented yet");
	// pthread_spin_lock(&sb->s_inode_list_lock);
	// list_for_each_entry (inode, &sb->s_inodes, i_sb_list) {
	// 	if (!(inode->i_state & I_HAS_WORKER) &&
	// 	    S_ISREG(inode->i_mode)) {
	// 		list_del_init(&inode->i_sb_list);
	// 		release_inode(inode);
	// 	}
	// }
	// pthread_spin_unlock(&sb->s_inode_list_lock);

	ret.kret = 0;
	if (ioctl(root->dofd, ILLUFS_DIROPS_RET, &ret) < 0) {
		oxb_error("Failed to wake up kernel inode(%lu)", root->i_ino);
		return -1;
	}
	return 0;
}

static int kreq_mount(struct inode *inode, kaddr_t kret)
{
	struct illufs_dirops_ret ret = { 0 };

	ret.kret = kret;
	ret.uret = 0;

	if (ioctl(inode->dofd, ILLUFS_DIROPS_RET, &ret) < 0) {
		oxb_error("Filesystem mounted but kernel not wake up");
		return -1;
	}
	oxb_info("Filesystem mounted success inode(%lu)", inode->i_ino);
	return 0;
}

/**
 * @brief Directory inode operation create.
 *
 * @param dir (directory inode)
 * @param mode
 * @param ino_p (inode pointer)
 * @param name (file name)
 * @return int (success on 0)
 */
static int kreq_create(struct inode *dir, mode_t mode, kaddr_t kret,
		       kaddr_t ino_p, char *name)
{
	struct illufs_ino_get ino_ret = { 0 };
	struct inode *new = NULL;

	fs_debug("[%s] name(dir(%d)/%s) start", __func__, dir->i_ino, name);
	/* kernel give this name to daemon */

	ino_ret.kret = kret;
	ino_ret.ino_kaddr = ino_p;

	if (!dir->i_op->create) {
		log_error("no create");
		goto err_uret;
	}

	oxb_debug("kreq_create: parent name(dir(%lu)/%s) mode(0%o)", dir->i_ino,
		  name, mode);

	dir_inode_lock(dir);
	if (dir->i_state & I_USE_JOURNAL)
		inode_add_journal(dir);

	if (dir->i_op->create(dir, &new, mode, name)) {
		log_error("create failed");
		dir_inode_unlock(dir);
		goto err_create;
	}
	dir_inode_unlock(dir);

	oxb_debug("kreq_create: created new inode(%lu) type(%s)", new->i_ino,
		  S_ISDIR(mode) ? "dir" : (S_ISREG(mode) ? "reg" : "other"));

	illufs_set_auth(new);

	// one for us, one for journaling (mark_inode_dirty)
	if (new && atomic_load(&new->i_count) > 2)
		oxb_error("inode(%lu) already in use", new->i_ino);

	/* wake up kernel thread */
	// log_debug("[%s] f(%s) ino(%lu)", __func__, name, new->i_ino);
	ino_ret.ino = new->i_ino;
	ino_ret.ino_kaddr = ino_p;
	ino_ret.uret = 0;
	ino_ret.kret = kret;

	if (ioctl(dir->dofd, ILLUFS_INOGET, &ino_ret))
		log_error("wake up failed");

	/* If new file is directory make directory worker */
	if (S_ISDIR(mode) && request_dir_init(new))
		goto err;
	else if (S_ISREG(mode) && request_file_init(new)) {
		oxb_error("[%s] init fail", __func__);
		goto err;
	}

	return 0;

err_create:
	ino_ret.ino = 0; // zero means create failed
err_uret:
	ino_ret.uret = -1;
	ioctl(dir->dofd, ILLUFS_INOGET, &ino_ret);
err:
	return -1;
}

/**
 * @brief Handling lookup (dir operation) request from kernel.
 *
 * @param dir (directory inode)
 * @param name (file name)
 * @param ino_p (inode pointer)
 * @return int (success on 0)
 */
static int kreq_lookup(struct inode *dir, kaddr_t kret, kaddr_t ino_p,
		       char *name)
{
	struct illufs_ino_get ino_ret = { 0 };
	struct inode *tar = NULL;
	int ret;

	/* kernel give this name to daemon */
	nvme_debug("[%s] name (%s)", __func__, name);

	ret = -1;
	if (!dir->i_op->lookup) {
		log_error("no lookup");
		goto ret;
	}

	dir_inode_rdlock(dir);
	tar = dir->i_op->lookup(dir, name);
	dir_inode_unlock(dir);

	/* set semaphore only if found inode and it is directory */
	if (tar) {
		illufs_set_auth(tar);
		ino_ret.ino = tar->i_ino;
	} else
		ino_ret.ino = 0;

	ino_ret.uret = 0;
	ino_ret.ino_kaddr = ino_p;
	ino_ret.kret = kret;

	if (tar && tar->i_count > 1)
		oxb_error("inode(%lu) already in use", tar->i_ino);

	/* wake up kernel thread */
	if (ioctl(dir->dofd, ILLUFS_INOGET, &ino_ret)) {
		log_error("wake up failed");
		goto ret;
	}

	if (tar) {
		if (S_ISDIR(tar->i_mode) && request_dir_init(tar)) {
			oxb_error("[%s] init fail", __func__);
			goto ret;
		} else if (S_ISREG(tar->i_mode) && request_file_init(tar)) {
			oxb_error("[%s] init fail", __func__);
			goto ret;
		}
	}
	/* on success */
	ret = 0;
	d_debug("[%s] dir(%lu) lookup(%s) done %s", __func__, dir->i_ino, name,
		tar ? "FOUND" : "NOT FOUND");

ret:
	return ret;
}

/* naive implementation of readdir, read directory in secure daemon
   Aggregate into cache and send kernel through ioctl, then free */
static int kreq_readdir(struct inode *dir, kaddr_t k_names, kaddr_t k_inos,
			kaddr_t k_nr, unsigned long pos)
{
	struct dofdio_readdir readdir_ioctl = { 0 };
	char *names = NULL;
	unsigned long *inos = NULL;
	int nr, name_len = 0;

	oxb_debug("[%s] tid=%ld dir(%lu) pos=%lu start", __func__,
		  (long)get_tid(), dir->i_ino, pos);

	assert(dir->i_fop->iterate_shared);

	nr = dir->i_fop->iterate_shared(dir, pos, &names, &name_len, &inos);
	if (nr < 0) {
		oxb_error("[%s] iterate_shared failed dir(%lu) pos=%lu nr=%d",
			  __func__, (long)get_tid(), dir->i_ino, pos, nr);
		return nr;
	}

	oxb_debug(
		"[%s] tid=%ld dir(%lu) pos=%lu nr=%d name_len=%d names=%s inos=%p",
		__func__, (long)get_tid(), dir->i_ino, pos, nr, name_len,
		names ? names : "NULL", inos);

	readdir_ioctl.names = k_names;
	readdir_ioctl.inos = k_inos;
	readdir_ioctl.nr = k_nr;

	readdir_ioctl.dentry_nr = nr;
	readdir_ioctl.u_inos = (__u64)inos;
	readdir_ioctl.u_names = (__u64)names;
	readdir_ioctl.names_len = name_len;

	if (ioctl(dir->dofd, DOFDIO_READDIR, &readdir_ioctl)) {
		log_error("[kreq_readdir] DOFDIO_READDIR ioctl failed dir(%lu) pos=%lu",
			  dir->i_ino, pos);
		return -ENOMEM;
	}

	oxb_debug("[kreq_readdir] dir(%lu) pos=%lu done dentry_nr=%u names_len=%u",
		 dir->i_ino, pos, readdir_ioctl.dentry_nr, readdir_ioctl.names_len);

	free(names);
	free(inos);

	return 0;
}

static int kreq_unlink(struct inode *dir, char *name, kaddr_t kret)
{
	struct illufs_dirops_ret io_args = { 0 };
	struct inode *tar = NULL;
	int ret;

	ret = -1;
	io_args.kret = kret;

	d_debug("[%s] name(dir(%d)/%s) start", __func__, dir->i_ino, name);

	/* Pre-fetch target (if present) to allow proactive deregistration */
	if (dir->i_op->lookup)
		tar = dir->i_op->lookup(dir, name);

	if (!dir->i_op->unlink) {
		oxb_error("no unlink");
		io_args.uret = ret;
		if (ioctl(dir->dofd, ILLUFS_DIROPS_RET, &io_args))
			log_error("wake up failed");
		goto ret;
	}

	oxb_debug("unlink name(dir(%d)/%s) found target inode(%lu)", dir->i_ino,
		 name, tar ? tar->i_ino : 0);

	dir_inode_lock(dir);
	if (dir->i_state & I_USE_JOURNAL)
		inode_add_journal(dir);

	/* Proactively deregister file/dir workers if any */
	if (tar) {
		if (S_ISREG(tar->i_mode)) {
			file_dispatcher_deregister(tar);
		} else if (S_ISDIR(tar->i_mode)) {
			dir_dispatcher_deregister(tar);
		}
	}

	ret = dir->i_op->unlink(dir, name);

	io_args.uret = ret;
	if (ioctl(dir->dofd, ILLUFS_DIROPS_RET, &io_args)) {
		log_error("wake up failed");
		ret = -1;
	}

	dir_inode_unlock(dir);

ret:
	return ret;
}

static int kreq_rename_new(struct inode *dir, char *name, kaddr_t kret,
			   unsigned long new_ino)
{
	struct illufs_dirops_ret io_args = { 0 };
	int ret;

	ret = -1;
	io_args.kret = kret;

	d_debug("[%s] name(dir(%d)/%s) start", __func__, dir->i_ino, name);

	if (!dir->i_op->rename_new) {
		oxb_error("no rename_new");
		io_args.uret = ret;
		if (ioctl(dir->dofd, ILLUFS_DIROPS_RET, &io_args))
			log_error("wake up failed");
		goto ret;
	}

	dir_inode_lock(dir);
	if (dir->i_state & I_USE_JOURNAL)
		inode_add_journal(dir);

	ret = dir->i_op->rename_new(dir, name, new_ino);

	io_args.uret = ret;
	if (ioctl(dir->dofd, ILLUFS_DIROPS_RET, &io_args)) {
		log_error("wake up failed");
		ret = -1;
	}

	dir_inode_unlock(dir);

ret:
	return ret;
}

static int kreq_rename_old(struct inode *dir, char *name, kaddr_t kret,
			   unsigned long ino)
{
	struct illufs_dirops_ret io_args = { 0 };
	int ret;

	ret = -1;
	io_args.kret = kret;

	d_debug("[%s] name(dir(%d)/%s) start", __func__, dir->i_ino, name);

	if (!dir->i_op->rename_old) {
		oxb_error("no rename_old");
		io_args.uret = ret;
		if (ioctl(dir->dofd, ILLUFS_DIROPS_RET, &io_args))
			log_error("wake up failed");
		goto ret;
	}

	dir_inode_lock(dir);
	if (dir->i_state & I_USE_JOURNAL)
		inode_add_journal(dir);

	ret = dir->i_op->rename_old(dir, name, ino);

	io_args.uret = ret;
	if (ioctl(dir->dofd, ILLUFS_DIROPS_RET, &io_args)) {
		log_error("wake up failed");
		ret = -1;
	}

	dir_inode_unlock(dir);

ret:
	return ret;
}

int init_dir(struct inode *dir)
{
	struct illufs_dir_worker_reg args = { 0 };
	int dofd;

	args.ino = dir->i_ino;

	dofd = syscall(__NR_diropfsfd, 0);
	if (dofd == -1) {
		log_error("diropsfd failed");
		return -1;
	}
	dir->dofd = dofd;

	// FIXME: Is the ioctl necessary? Leave it for now as directory
	// operations are not important for the experiment.
	if (ioctl(dir->dofd, ILLUFS_DIR_WORKER_REGISTER, &args)) {
		log_error("ioctl(ILLUFS_DIR_WORKER_REGISTER) failed ino(%lu)",
			  dir->i_ino);
		close(dofd);
		return -1;
	}

	if (init_shm_shared_state(dir)) {
		log_error("init_shm_shared_state failed ino(%lu)", dir->i_ino);
		close(dofd);
		return -1;
	}

	return 0;
}

void clear_dir_dofd(struct inode *inode)
{
	if (inode->dofd && close(inode->dofd) < 0)
		perror("failed to close dirfd on evict_inode");
	inode->dofd = 0;
}


void dir_worker_init_fn(void *arg)
{
	struct inode *dir = (struct inode *)arg;
	int ret = 0;

	BUG_ON(!dir, "no dir");
	BUG_ON(!dir->i_mapping, "not prepared");

	ret = init_dir(dir);
	if (ret < 0) {
		oxb_error("failed to init dir ino(%lu)", dir->i_ino);
		panic("failed to init dir.");
		return;
	}

	ret = dir_dispatcher_register(dir);
	if (ret < 0){
		clear_dir_dofd(dir);
		oxb_error("failed to register dir ino(%lu)", dir->i_ino);
		panic("failed to register dir to epoll dispatcher.");
		return;
	}
}

int request_dir_init(struct inode *dir)
{
	BUG_ON(!dir, "no dir");
	d_debug("[%s] dir(%lu)", __func__, dir->i_ino);

	/* Check if already initialized and registered to epoll */
	inode_lock(dir);
	bool already = (dir->i_state & I_HAS_WORKER) && dir->dofd > 0;
	inode_unlock(dir);
	if (already) {
		d_debug("[%s] dir(%lu) already initialized (dofd=%d)", __func__,
			dir->i_ino, dir->dofd);
		return 0;
	}

	thpool_add_work(g_dir_worker_init_thpool, dir_worker_init_fn, (void *)dir);

	return 0;
}

static int do_daemon_init(struct super_block *sb, int dofd)
{
	struct dofdio_daemon_init daemon_init = { 0 };
	void *addr;
	size_t addr_len;

	addr_len = sizeof(struct inode_auth) * sb->nr_inodes;
	addr_len = (addr_len + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
	addr = mmap(NULL, addr_len, PROT_WRITE | PROT_READ,
		    MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
	if (addr == (void *)-1) {
		log_error("mmap failed");
		return -1;
	}

	/* after do below registration, illusionFS can be mounted on kernel */
	daemon_init.filename_max = sb->filename_max;
	daemon_init.nr_inodes = sb->nr_inodes;
	daemon_init.max_subfiles = sb->max_subfiles;
	daemon_init.auth_mmap = (unsigned long)addr;
	// daemon_init.io_pages = 256; // TODO: make it configurable
	// daemon_init.ra_pages = 32;
	// daemon_init.io_pages = 256; // TODO: make it configurable
	// daemon_init.ra_pages = 64;
	// daemon_init.io_pages = 512; // TODO: make it configurable
	// daemon_init.ra_pages = 256;

	d_debug("illusionFS init nr_inodes %d, auth mmap %lx", sb->nr_inodes,
		addr);

#ifdef OXBOW_IPC_MSG_RING
	log_info("[IPC_MODE] daemon_init mode=msg_ring");
#else
	log_info("[IPC_MODE] daemon_init mode=legacy");
#endif

	if (ioctl(dofd, ILLUFS_DAEMON_INIT, &daemon_init) == -1) {
		log_error("ioctl-DOFDIO_DAEMON_INIT");
		munmap(addr, addr_len);
		return -1;
	}

	sb->auth_mmap = addr;
	sb->auth_mmap_len = addr_len;
	sb->oxbow_manager_fd = dofd;
	return 0;
}

/**
 * @brief Initiate root worker at init time.
 *
 * @param sb
 * @return int (0 on success)
 */
int init_root_worker(struct super_block *sb)
{
	struct inode *root = sb->s_root;

	dir_worker_init_fn(root);
	oxb_info("init_root_worker done root ino %d", root->i_ino);

	return 0;
}

void exit_manager(void)
{
	int dofd = g_super_block->oxbow_manager_fd;

	while (manager_is_working) {
		log_info("waiting for manager to exit");
		sleep(1);
	}

	if (ioctl(dofd, ILLUFS_DAEMON_UNINIT, NULL) == -1) {
		log_error("ioctl-UFFDIO_DAEMON_UNINIT");
	}

	if (close(dofd) < 0)
		perror("exit_root_worker close fail");

#ifdef THPOOL_PROFILE
	thpool_profile_print_summary(g_dir_worker_thpool);
	thpool_profile_print_summary(g_dir_worker_init_thpool);
#endif

	// TODO: Terminate gracefully.
	// thpool_wait(g_dir_worker_thpool);
	// thpool_destroy(g_dir_worker_thpool);
	// thpool_wait(g_dir_worker_init_thpool);
	// thpool_destroy(g_dir_worker_init_thpool);
}

static void evict(unsigned long ino)
{
	struct inode *inode;

	i_debug("[%s] ino(%lu)", __func__, ino);
	oxb_warn("[%s] FS Manager evicts ino(%lu). Does it called frequently?",
		 __func__, ino);

	/* Try to fetch existing in-memory inode; if absent, construct transient one */
	inode = ihold(ino);
	if (inode) {
		/* Hold a ref while performing free to avoid races */
		iget(inode);
	} else {
		inode = g_super_block->s_op->iget(g_super_block, ino);
		if (!inode) {
			oxb_error("inode not found");
			panic("inode not found.");
			return;
		}
	}

	// if (inode->i_nlink == 0) {
		/*
		 * Inode has no links; let iput_final handle on-disk free
		 * after removing it from the inode cache.
		 */
	// }

	/* Drop our ref; iput path will remove from lists and destroy */
	iput(inode);
	return;
}

/**
 * @brief handle event from directory inode operation.
 * (per-directory uffd handler)
 *
 * @param __args
 */
static void *oxbow_fs_manager(void *__args)
{
	struct pollfd pollfd;
	struct oxbow_msg msg;
	unsigned long arg = (unsigned long)__args;
	int nready, timeout, dofd;

	dofd = (int)arg;
	pollfd.fd = dofd;
	pollfd.events = POLLIN;
	timeout = 0;
	for (;;) {
		PF_TL_START(fm_poll);
		nready = poll(&pollfd, 1, timeout);
		if (nready == -1) {
			log_error("poll error");
			break;
		} else if (nready == 0) {
			PF_TL_END(fm_poll);
			manager_is_working = false;
			timeout = -1;
			continue;
		}
		manager_is_working = true;
		timeout = 0;
		PF_TL_END(fm_poll);

		// d_debug("[%s] nready(%d) re(%d)", __func__, nready,
		// 	pollfd.revents);
		if (pollfd.revents & POLLIN) {
			PF_TL_START(fm_read);
			if (read(dofd, &msg, sizeof(msg)) == -1) {
				perror("read fail");
				log_error("read error %d", errno);
				break;
			}
			PF_TL_END(fm_read);

			// log_info("[%s] manager get event(%s)", __func__,
			// 	 GET_ILLUFS_MANAGER_EVENT_NAME(msg.event));

			switch (msg.event) {
				// case ILLUFS_DROP_PGCACHE: // XXX
				// 	kreq_drop_pagecache(di);
				// 	break;
				// case ILLUFS_MOUNT: // XXX
				// 	kreq_mount(di, msg.ret);
				// 	break;
				// case ILLUFS_MANAGER_REVIVE:
				// 	PF_TL_START(fm_revive);
				// 	if (kreq_revive_file_worker(msg.arg.revive.ino))
				// 		log_error("kreq_revive fail");
				// 	PF_TL_END(fm_revive);
				// 	break;
				// case ILLUFS_MANAGER_NOTIFYOPEN:
				// 	PF_TL_START(fm_fdmap);
				// 	register_file_proc_fdmap(
				// 		msg.arg.notify_open.ino,
				// 		msg.arg.notify_open.fd,
				// 		msg.arg.notify_open.pid);
				// 	PF_TL_END(fm_fdmap);
				// 	break;
			case ILLUFS_MANAGER_EVICT_INODE:
				evict(msg.arg.evict_inode.ino);
				break;
			default:
				log_error("[%s] not used now %d", __func__,
					  msg.event);
				oxbow_assert(0);
				break;
			}
		}
	}

	close(dofd);
	pthread_exit(NULL);
}

int init_fs_manager(void)
{
	pthread_t thr;
	unsigned long dofd;
	int ret;

	dofd = syscall(__NR_diropfsfd, ULONG_MAX);
	if ((int)dofd == -1) {
		log_error("diropsfd failed");
		return -1;
	}

	if (do_daemon_init(g_super_block, dofd)) {
		log_error("root_do_daemon_init failed");
		close(dofd);
		return -1;
	}

	/* now kernel can init root_inode */
	illufs_set_auth(g_super_block->s_root);

	ret = pthread_create(&thr, NULL, oxbow_fs_manager, (void *)dofd);
	if (ret != 0) {
		log_error("pthread create");
		return -1;
	}

	log_info("[%s] done", __func__);
	return 0;
}
