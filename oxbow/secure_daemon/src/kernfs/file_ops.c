/* Required before any header that may pull <sched.h> so cpu_set_t,
 * CPU_ZERO/CPU_SET, and pthread_setaffinity_np become visible (used by
 * the D-2 read_worker pinning helper below). */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#define __USE_GNU
#include <sched.h>

#include "fs.h"
#include "io/sd_bio.h"
#include "io/nvme.h"
#include "io_dispatcher.h"
#include "kernfs.h"
#include <semaphore.h>
#include <stdatomic.h>
#include <stdint.h>
#include <sys/epoll.h>
#include <sys/poll.h>
#include <sys/ioctl.h>
#include <sys/fcntl.h>
#include <sys/mman.h>
#include <sys/eventfd.h>
#include <sys/prctl.h>
#include <pthread.h>
#include <unistd.h>
#include "linux/diropsfd.h"
#include "thpool.h"

#include "common/time_stat.h"
#include "common/cpu_pinning.h"
#include "config.h"
#include "global.h"
#include "oxbow_debug.h"
#include "profile_secure_daemon.h"

#ifdef OXBOW_RD_INLINE_SUBMIT
/* Defined in src/io_dispatcher.c. Read by iod_submit_bio() to decide
 * whether the call site is inside a D-2 read_worker. */
extern __thread struct bio *tls_inline_batch[INLINE_BATCH_MAX];
extern __thread int tls_inline_batch_n;
extern __thread int tls_inline_batch_active;

/* Local helper mirroring thpool.c's static pinning_cpu(): pin the
 * current thread to a single CPU. We duplicate the few-line helper
 * instead of exposing the thpool internal so we don't widen its API. */
static void rd_worker_pin_cpu(int cpu)
{
	cpu_set_t set;
	pthread_t self = pthread_self();

	if (cpu < 0)
		return;

	CPU_ZERO(&set);
	CPU_SET(cpu, &set);
	pthread_setaffinity_np(self, sizeof(set), &set);
}
#endif

// worker managing states, who gonna be free or not

// Profiling
atomic_int g_file_worker_count;
atomic_int g_file_worker_exit_count;
atomic_int g_file_mmap_count;
atomic_int g_file_munmap_count;
threadpool g_file_worker_thpool;
threadpool g_file_worker_init_thpool;

// File epoll dispatcher (shared) replacing per-file threads
#define FILE_EPOLL_MAX_EVENTS 256
static int g_file_epoll_fd = -1;
static int g_file_eventfd = -1; // wake/shutdown notifier
static pthread_mutex_t g_file_epoll_mutex = PTHREAD_MUTEX_INITIALIZER;

#ifndef OXBOW_RD_INLINE_SUBMIT
static void *file_epoll_loop(void *arg);
static void file_epoll_loop_runner(void *arg);
#endif
static int file_dispatcher_init(void);
static int file_dispatcher_register(struct inode *inode);
static void process_inode_events(struct inode *inode, uint32_t events);

#ifdef OXBOW_IPC_MSG_RING
static inline void msg_ring_fail_fast(struct inode *inode, const char *op)
{
	oxb_error("msg_ring fail-fast: %s ino(%lu)", op, inode->i_ino);
	panic("msg_ring fail-fast");
}

static inline void trace_msg_ring_rx(const struct inode *inode,
				     const struct oxbow_msg *msg,
				     uint32_t head, uint32_t tail)
{
#ifndef PRINT_OXBOW_TRACE
	(void)inode;
	(void)head;
	(void)tail;
#endif

	switch (msg->event) {
	case ILLUFS_FILEWORKER_READ:
		oxb_trace("[IPC_RING_RX] ino=%lu seq=%u ev=READ idx=%llu folio=0x%llx head=%u tail=%u",
			 inode->i_ino, head + 1,
			 (unsigned long long)msg->arg.read.idx,
			 (unsigned long long)msg->arg.read.folio,
			 head, tail);
		break;
	case ILLUFS_FILEWORKER_RA:
		oxb_trace("[IPC_RING_RX] ino=%lu seq=%u ev=RA idx=%llu nr_pages=%u ractl=0x%llx head=%u tail=%u",
			 inode->i_ino, head + 1,
			 (unsigned long long)msg->arg.readahead.idx,
			 msg->arg.readahead.nr_pages,
			 (unsigned long long)msg->arg.readahead.ractl,
			 head, tail);
		break;
	case ILLUFS_FILEWORKER_NOTIFYOPEN:
		oxb_trace("[IPC_RING_RX] ino=%lu seq=%u ev=NOTIFY_OPEN pid=%d fd=%d head=%u tail=%u",
			 inode->i_ino, head + 1,
			 msg->arg.notify_open.pid, msg->arg.notify_open.fd,
			 head, tail);
		break;
	default:
		oxb_trace("[IPC_RING_RX] ino=%lu seq=%u ev=%u head=%u tail=%u",
			 inode->i_ino, head + 1, msg->event, head, tail);
		break;
	}
}

static inline int msg_ring_try_dequeue(struct oxbow_inode_msg_ring *ring,
				       struct oxbow_msg *out_msg,
				       uint32_t *out_head,
				       uint32_t *out_tail)
{
	struct oxbow_inode_msg_slot *slot;
	uint32_t pos, seq;
	int32_t dif;

	if (!ring || !out_msg)
		return -EINVAL;
	if (!ring->capacity ||
	    (ring->capacity & (ring->capacity - 1)) != 0 ||
	    ring->mask != (ring->capacity - 1))
		return -EINVAL;

	for (;;) {
		pos = __atomic_load_n(&ring->head, __ATOMIC_RELAXED);
		slot = &ring->slots[pos & ring->mask];
		seq = __atomic_load_n(&slot->seq, __ATOMIC_ACQUIRE);
		/* Signed distance keeps comparisons valid across u32 wrap-around. */
		dif = (int32_t)seq - (int32_t)(pos + 1);
		if (dif == 0) {
			if (__atomic_compare_exchange_n(
				    &ring->head, &pos, pos + 1, false,
				    __ATOMIC_RELAXED, __ATOMIC_RELAXED))
				break;
			continue;
		}
		if (dif < 0)
			return -EAGAIN;
	}

	*out_msg = slot->msg;
	__atomic_store_n(&slot->seq, pos + ring->capacity, __ATOMIC_RELEASE);
	if (out_head)
		*out_head = pos;
	if (out_tail)
		*out_tail = __atomic_load_n(&ring->tail, __ATOMIC_ACQUIRE);
	return 0;
}
#endif

int init_file_workers(void)
{
	atomic_init(&g_file_worker_count, 0);
	atomic_init(&g_file_worker_exit_count, 0);
	atomic_init(&g_file_mmap_count, 0);
	atomic_init(&g_file_munmap_count, 0);

#ifndef OXBOW_RD_INLINE_SUBMIT
	/* Legacy mode: file_worker thpool runs file_epoll_loop on N
	 * threads. In OXBOW_RD_INLINE_SUBMIT mode this pool is replaced
	 * by a dedicated read_worker pool with its own SPDK qpairs (see
	 * start_read_workers()). */
	g_file_worker_thpool =
		thpool_init(OXBOW_FILEWORKER_NR, "oxb_f_worker");
	if (!g_file_worker_thpool) {
		oxb_error("thpool_init failed");
		return -1;
	}
#else
	g_file_worker_thpool = NULL;
#endif

	/* Always created: 1-thread pool for one-shot per-inode init work
	 * (file_worker_init_fn). Unrelated to the read I/O path. */
	g_file_worker_init_thpool =
		thpool_init(1, "oxb_init_f_worker");
	if (!g_file_worker_init_thpool) {
		oxb_error("thpool_init failed");
		return -1;
	}

	return 0;
}

int start_file_dispatcher(void)
{
	if (file_dispatcher_init() < 0) {
		oxb_error("file dispatcher init failed");
		return -1;
	}

#ifdef OXBOW_RD_INLINE_SUBMIT
	if (start_read_workers() < 0) {
		oxb_error("start_read_workers failed");
		return -1;
	}
#endif
	return 0;
}

static int worker_do_mmap(struct inode *inode)
{
	void *data;

	/* Guard against concurrent mmap on the same inode */
	inode_lock(inode);
	if (inode->data) {
		inode_unlock(inode);
		return 0; // already mapped
	}

	data = mmap(0, OXBOW_MAX_FILE_SIZE, PROT_WRITE | PROT_READ, MAP_SHARED,
		    inode->fd, 0);
	if (data == MAP_FAILED) {
		inode_unlock(inode);
		oxb_error("mmap failed. errno:%s counter %d", strerror(errno),
			  atomic_load(&g_file_mmap_count));
		LOG_GLOBAL_WORKER_STATS();
		return -1;
	}
	atomic_fetch_add(&g_file_mmap_count, 1);
	inode->data = data;
	inode_unlock(inode);
	return 0;
}

/**
 * @brief translate multiple page read from kernel
 * 
 * @param __k_args (arguments from kernel)
 */
static void do_readahead(struct inode *inode, void *__k_rac, bool need)
{
	struct readahead_control rac;
	struct oxbow_ra_ctrl *k_rac;

	PF_TL_START(b_evt_readahead);

	rac.inode = inode;

	k_rac = __k_rac;
	rac.kernel_ractl = k_rac->ractl;
	rac.index = k_rac->index;
	rac.nr_pages = k_rac->nr_pages;
	rac.need = need;

	d_trace("[%s] inode(%lu) index(%lu) nr_pages(%u)", __func__,
		inode->i_ino, rac.index, rac.nr_pages);

	/*
	 * Debug-level log for each KERNEL readahead request handed over
	 * from the kernel to secure_daemon.  This can be re-enabled by
	 * turning on PRINT_DAEMON_DEBUG when deep RA tracing is needed.
	 */
	d_debug("[DAEMON_RA_REQ] inode(%lu) index(%lu) nr_pages=%u need=%d ractl=%p",
		inode->i_ino,
		(unsigned long)rac.index,
		(unsigned int)rac.nr_pages,
		(int)rac.need,
		(void *)rac.kernel_ractl);

	check_page_in_shm(inode, rac.index);

	if ((rac.index / NR_PAGES_IN_PEB) !=
	    ((rac.index + rac.nr_pages) / NR_PAGES_IN_PEB))
		check_page_in_shm(inode, rac.index + rac.nr_pages);

	BUG_ON(!inode->i_mapping->a_ops->readahead, "no ra");
	if (!(inode->i_state & I_HAS_WORKER)) {
		oxb_error("inode(%lu) no have worker", inode->i_ino);
		return;
	}

	if (inode->data == NULL) {
		int ret;
		ret = worker_do_mmap(inode);
		if (ret < 0) {
			oxb_error("failed to mmap ino(%lu)", inode->i_ino);
			mpage_end_io_failed(rac.index, rac.nr_pages, &rac);
			goto out;
		}
		if (ret == 1) {
			log_warn("file closed but readahead? ino(%lu)",
				 inode->i_ino);
			mpage_end_io_failed(rac.index, rac.nr_pages, &rac);
			goto out;
		}
	}

	inode->i_mapping->a_ops->readahead(&rac);

out:
	PF_TL_END(b_evt_readahead);
}

/**
 * @brief translate single page read from kernel
 * 
 * @param __k_args (arguments from kernel)
 */
static void do_readpage(struct inode *inode, void *__k_args)
{
	struct read_control rc;
	struct oxbow_read_folio *k_args;

	PF_TL_START(b_evt_readpage);

	rc.inode = inode;

	k_args = __k_args;

	rc.folio = k_args->folio;
	rc.index = k_args->index;

	/*
	 * Debug-level log for each READPAGE request handed over from the kernel.
	 * Combined with kernel-side READ_FOLIO_REQ / READ_END_OK, this can be
	 * used to trace stuck handshakes when daemon debug logs are enabled.
	 */
	d_debug("[DAEMON_READPAGE] inode(%lu) index(%lu) folio=%p",
		inode->i_ino, rc.index, (void *)rc.folio);

	check_page_in_shm(inode, rc.index);

	if (check_uptodate_in_shm(inode, rc.index)) {
		end_io_no_bio(inode->fd, rc.folio);
		return;
	}

	BUG_ON(!inode->i_mapping->a_ops->readpage, "no read");
	BUG_ON(!(inode->i_state & I_HAS_WORKER), "no worker");

	if (inode->data == NULL) {
		int ret;
		ret = worker_do_mmap(inode);
		if (ret < 0) {
			oxb_error("failed to mmap ino(%lu)", inode->i_ino);
			end_io_no_bio(inode->fd, rc.folio);
			return;
		}
		if (ret == 1) {
			log_warn("file closed but readahead? ino(%lu)",
				 inode->i_ino);
			end_io_no_bio(inode->fd, rc.folio);
			return;
		}
	}

	inode->i_mapping->a_ops->readpage(&rc);

	PF_TL_END(b_evt_readpage);
}

// static int handle_async_remains(struct inode *inode)
// {
// 	struct oxbow_msg msg;
// 	struct pollfd pollfd;
// 	int ret, nready;

// 	/* Check async tasks remains */
// 	pollfd.fd = inode->fd;
// 	do {
// 		nready = poll(&pollfd, 1, 0);
// 		if (nready < 0) {
// 			perror("poll");
// 			pthread_exit(NULL);
// 		}

// 		d_debug("[%s] inode(%lu) nready %d", __func__, inode->i_ino,
// 			pollfd.revents);

// 		ret = read(pollfd.fd, &msg, sizeof(msg));
// 		if (ret < 0 && errno == EAGAIN) // nothing to do!
// 			break;
// 		if (ret < 0) {
// 			perror("read");
// 			oxb_error("ret %d", ret);
// 			return -1;
// 		}

// 		switch (msg.event) {
// 		case ILLUFS_FILEWORKER_RA:
// 			do_readahead(inode, &msg.arg.readahead, false);
// 			break;
// 		case ILLUFS_FILEWORKER_NOTIFYOPEN:
// 			register_file_proc_fdmap(inode, msg.arg.notify_open.fd,
// 						 msg.arg.notify_open.pid);
// 			break;
// 		case ILLUFS_FILEWORKER_READ:
// 		case ILLUFS_DAEMON_CLOSE:
// 		default:
// 			oxb_error("Must not reached here case %d", msg.event);
// 			// BUG_ON(1, "bug");
// 			break;
// 		}
// 	} while (ret != 0);

// 	return 0;
// }

static void exit_file_worker(struct inode *inode)
{
	/*
	 * Normal path:
	 *   - shm_header is initialized and shared_inode_lock() is safe.
	 *
	 * Defensive path:
	 *   - If shm_header is NULL for some reason (e.g., race or init failure),
	 *     avoid BUG_ON in shared_inode_lock() by falling back to inode_lock().
	 *   - We still clear I_HAS_WORKER and drop our reference via iput().
	 */
	if (!inode->shm_header) {
		inode_lock(inode);
		inode->i_state &= ~I_HAS_WORKER;
		inode_unlock(inode);
		iput(inode);
		return;
	}

	shared_inode_lock(inode);
	inode->i_state &= ~I_HAS_WORKER;
	shared_inode_unlock(inode);
	iput(inode);
}

static int init_file(struct inode *inode)
{
	int ret = 0;

	// This is first registering file fd to epoll dispatcher.
	// It might be called from lookup or creat in kernel function
	PF_TL_START(open1_syscall_getfd);
	inode->fd = syscall(__NR_diropfsfd, inode->i_ino);
	if (inode->fd < 0) {
		oxb_error("inode(%lu) errno %d", inode->i_ino, strerror(errno));
		return -1;
	}

	PF_TL_END(open1_syscall_getfd);

	if (!inode->data) {
		ret = worker_do_mmap(inode);
		if (ret < 0) {
			oxb_error("failed to mmap ino(%lu)", inode->i_ino);
			close(inode->fd);
			panic("failed to mmap.");
			return -1;
		}
	}

	// Ensure non-blocking for epoll-driven reads (for multi-threading)
	int flags = fcntl(inode->fd, F_GETFL, 0);
	if (flags >= 0)
		fcntl(inode->fd, F_SETFL, flags | O_NONBLOCK);

	d_debug("[%s] ino(%lu) fd(%d)", __func__, inode->i_ino, inode->fd);

	PF_TL_START(open2_init_shm);
	if (init_shm_shared_state(inode)) {
		close(inode->fd);
		return -1;
	}
	PF_TL_END(open2_init_shm);

	inode->i_state |= (I_SET_ONCE | I_HAS_WORKER);

#ifdef OXBOW_IPC_MSG_RING
	{
		void *ring_mem;

		ring_mem = mmap(NULL, 4096, PROT_READ | PROT_WRITE,
				MAP_SHARED, inode->fd,
				(off_t)OXBOW_MSG_RING_MMAP_PGOFF * 4096);
		if (ring_mem != MAP_FAILED) {
			inode->msg_ring =
				(struct oxbow_inode_msg_ring *)ring_mem;
			if (!inode->msg_ring->capacity ||
			    (inode->msg_ring->capacity &
			     (inode->msg_ring->capacity - 1)) != 0 ||
			    inode->msg_ring->mask !=
				    (inode->msg_ring->capacity - 1)) {
				oxb_warn(
					"msg_ring header invalid ino(%lu), fallback to legacy path",
					inode->i_ino);
				munmap(inode->msg_ring, PAGE_SIZE);
				inode->msg_ring = NULL;
			} else {
				oxb_info("msg_ring mmap OK ino(%lu) cap=%u",
					 inode->i_ino,
					 inode->msg_ring->capacity);
			}
		} else {
			inode->msg_ring = NULL;
			if (errno == EINVAL || errno == ENODEV ||
			    errno == EPERM) {
				static int fallback_once_logged;
				if (!fallback_once_logged) {
					fallback_once_logged = 1;
					oxb_warn(
						"[IPC_MODE_MISMATCH] msg_ring mmap unsupported, fallback to legacy path (errno=%d: %s)",
						errno, strerror(errno));
				}
			} else {
				oxb_error("msg_ring mmap FAILED ino(%lu) errno=%d (%s)",
					  inode->i_ino, errno,
					  strerror(errno));
				close(inode->fd);
				inode->fd = -1;
				return -1;
			}
		}
	}
#else
	inode->msg_ring = NULL;
#endif

	d_debug("[%s] done", __func__);
	return 0; // on success
}

// ==================== Shared file epoll dispatcher ====================

#define EPOLL_MSG_BATCH 32

static inline void dispatch_epoll_msg(struct inode *inode,
				      const struct oxbow_msg *msg)
{
	switch (msg->event) {
	case ILLUFS_FILEWORKER_RA:
		d_trace("inode(%lu) readahead", inode->i_ino);
		do_readahead(inode, &msg->arg.readahead, true);
		break;
	case ILLUFS_FILEWORKER_READ:
		d_trace("inode(%lu) read", inode->i_ino);
		do_readpage(inode, &msg->arg.read);
		break;
	case ILLUFS_FILEWORKER_NOTIFYOPEN:
	{
		static int notify_open_first;
		if (!notify_open_first) {
			notify_open_first = 1;
#ifdef OXBOW_IPC_MSG_RING
			oxb_info("[NOTIFY_OPEN_RECV] ino=%lu pid=%d fd=%d mode=msg_ring",
				 inode->i_ino, msg->arg.notify_open.pid,
				 msg->arg.notify_open.fd);
#else
			oxb_info("[NOTIFY_OPEN_RECV] ino=%lu pid=%d fd=%d mode=legacy",
				 inode->i_ino, msg->arg.notify_open.pid,
				 msg->arg.notify_open.fd);
#endif
		}
		register_file_proc_fdmap(inode, msg->arg.notify_open.fd,
					 msg->arg.notify_open.pid);
		break;
	}
	default:
		oxb_error("inode(%lu) got unknown case %d",
			  inode->i_ino, msg->event);
		break;
	}
}

static inline void drain_legacy_fd_messages(struct inode *inode)
{
	for (;;) {
		struct oxbow_msg msgs[EPOLL_MSG_BATCH];
		ssize_t bytes;
		int nmsg, j;

		bytes = read(inode->fd, msgs, sizeof(msgs));
		if (bytes <= 0) {
			if (bytes < 0 &&
			    (errno == EAGAIN || errno == EWOULDBLOCK))
				break;
			if (bytes == 0)
				break;
			perror("read");
			break;
		}
		nmsg = bytes / sizeof(struct oxbow_msg);
		for (j = 0; j < nmsg; j++)
			dispatch_epoll_msg(inode, &msgs[j]);
	}
}

/*
 * Drain pending kernel-side messages for one inode and process them
 * (do_readahead / do_readpage / register_file_proc_fdmap), then handle
 * close cleanup. Used by both the legacy file_worker thpool entry
 * (file_epoll_loop) and the D-2 read_worker entry (read_worker_loop).
 *
 * The caller is responsible for ensuring that BIOs produced inside this
 * call go to the right submission path:
 *   - file_worker mode: tls_inline_batch_active=0 -> ring + pump path
 *     via iod_submit_bio()/nvme_schedule_read_pump().
 *   - read_worker mode: tls_inline_batch_active=1 -> inline collection
 *     in tls_inline_batch[] then nvme_submit_bio_inline() at the end of
 *     this batch (wrapper handles the flush).
 */
static void process_inode_events(struct inode *inode, uint32_t events)
{
	if (events & EPOLLIN) {
		inode_lock(inode);
		inode->i_state |= I_IO_ACTIVE;
		inode_unlock(inode);

#ifdef OXBOW_IPC_MSG_RING
		{
			struct oxbow_inode_msg_ring *ring = inode->msg_ring;
			struct oxbow_msg msg;
			static int ring_read_first;
			bool use_legacy_fallback = false;

			if (!ring)
				use_legacy_fallback = true;
			else if (!ring->capacity ||
				 (ring->capacity &
				  (ring->capacity - 1)) != 0 ||
				 ring->mask != (ring->capacity - 1))
				use_legacy_fallback = true;

			if (use_legacy_fallback) {
				static int fallback_first_logged;
				if (!fallback_first_logged) {
					fallback_first_logged = 1;
					oxb_warn(
						"[IPC_MODE_MISMATCH] msg_ring unavailable/invalid, fallback to legacy read path");
				}
				drain_legacy_fd_messages(inode);
				goto done_epollin;
			}

			if (!ring_read_first) {
				uint32_t head = __atomic_load_n(
					&ring->head, __ATOMIC_ACQUIRE);
				uint32_t tail = __atomic_load_n(
					&ring->tail, __ATOMIC_ACQUIRE);
				ring_read_first = 1;
				oxb_warn(
					"[MSG_RING_READ] ino=%lu head=%u tail=%u cap=%u",
					inode->i_ino, head, tail,
					ring->capacity);
			}

			for (;;) {
				uint32_t head_pos = 0;
				uint32_t tail_pos = 0;
				int dqret = msg_ring_try_dequeue(
					ring, &msg, &head_pos, &tail_pos);

				if (dqret == -EAGAIN)
					break;
				if (dqret < 0)
					msg_ring_fail_fast(
						inode,
						"ring dequeue error");
				trace_msg_ring_rx(inode, &msg, head_pos,
						   tail_pos);
				dispatch_epoll_msg(inode, &msg);
			}
		}
#else
		drain_legacy_fd_messages(inode);
#endif
#ifdef OXBOW_IPC_MSG_RING
done_epollin:;
#endif
	} else if (events & (EPOLLHUP | EPOLLERR | EPOLLRDHUP)) {
		oxb_error("EPOLLHUP|EPOLLERR|EPOLLRDHUP ino(%lu)",
			  inode->i_ino);
		panic("Unknown epoll event type.");
	}

	/*
	 * Close path: use test-and-clear on I_CLOSING so only one thread
	 * performs cleanup even without EPOLLONESHOT.
	 */
	{
		bool do_close = false;

		inode_lock(inode);
		if (inode->i_state & I_CLOSING) {
			inode->i_state &= ~I_CLOSING;
			do_close = true;
		}
		inode->i_state &= ~I_IO_ACTIVE;
		inode_unlock(inode);

		if (do_close) {
			pthread_mutex_lock(&g_file_epoll_mutex);
			epoll_ctl(g_file_epoll_fd, EPOLL_CTL_DEL,
				  inode->fd, NULL);
			pthread_mutex_unlock(&g_file_epoll_mutex);
			exit_file_worker(inode);
		}
	}
}

#ifndef OXBOW_RD_INLINE_SUBMIT
static void *file_epoll_loop(void *arg)
{
	struct epoll_event events[FILE_EPOLL_MAX_EVENTS];
	(void)arg;

	for (;;) {
		int n = epoll_wait(g_file_epoll_fd, events,
				   FILE_EPOLL_MAX_EVENTS, -1);
		if (n < 0) {
			if (errno == EINTR)
				continue;
			perror("epoll_wait");
			break;
		}

		for (int i = 0; i < n; i++) {
			struct epoll_event *ev = &events[i];
			static int epoll_first_logged;

			if (ev->data.ptr == NULL) {
				uint64_t v;

				(void)read(g_file_eventfd, &v, sizeof(v));
				continue;
			}

			struct inode *inode = (struct inode *)ev->data.ptr;

			if (!epoll_first_logged) {
				epoll_first_logged = 1;
				oxb_warn("[EPOLL_EVENT] first event: "
					 "ino=%lu events=0x%x msg_ring=%p",
					 inode->i_ino, ev->events,
					 (void *)inode->msg_ring);
			}

			process_inode_events(inode, ev->events);
		}
	}

	return NULL;
}

static void file_epoll_loop_runner(void *arg)
{
	(void)arg;
	(void)file_epoll_loop(NULL);
}
#endif /* !OXBOW_RD_INLINE_SUBMIT */

#ifdef OXBOW_RD_INLINE_SUBMIT
/* ============================================================
 *   Read_worker pool: dedicated pthreads that own SPDK
 *   qpairs (tid se..se+read_worker_thread_num-1) and run the full
 *   "kernel msg -> mpage -> SPDK submit + poll" pipeline inline,
 *   bypassing the read pump for production reads.
 *
 *   Pool size is runtime-configurable via `read_worker_thread_num`
 *   in secure_daemon_conf.sh (see g_sd_conf.read_worker_thread_num).
 *   The pthread_t array is sized statically to OXBOW_RD_WORKER_NR_MAX.
 * ============================================================ */

static pthread_t g_read_worker_pthreads[OXBOW_RD_WORKER_NR_MAX];
static atomic_int g_read_worker_stop = ATOMIC_VAR_INIT(0);
static atomic_int g_read_worker_started = ATOMIC_VAR_INIT(0);

/*
 * Drain a partially full inline batch when capacity is reached mid-round.
 * Called from process_inode_events callers (and from iod_submit_bio's
 * spillover path inside the BIO append loop). Keeps the active flag set
 * so subsequent per-inode dispatch keeps appending to the batch.
 */
static inline void inline_batch_flush(void)
{
	if (tls_inline_batch_n > 0) {
		nvme_submit_bio_inline(tls_inline_batch,
				       tls_inline_batch_n);
		tls_inline_batch_n = 0;
	}
}

/*
 * Process one inode's events while the caller's inline batch is active.
 * The batch is shared across all inodes processed in the same epoll
 * round (see read_worker_loop), so this helper does NOT reset/flush it.
 * The caller must set up tls_inline_batch_{active,n} before the first
 * call and flush + clear active after the last call.
 *
 * Per-epoll-round batching is critical at high app concurrency (32+
 * apps): a single read_worker can be handed multiple inode events per
 * epoll_wait return; merging their BIOs into one nvme_submit_bio_inline
 * call lets per-qpair queue depth approach nvme_target_qd instead of
 * being capped by the 1-2 BIO/inode ratio of typical kernel readahead
 * events.
 */
static void inode_msg_handler_inline(struct inode *inode, uint32_t events)
{
	process_inode_events(inode, events);
}

static void *read_worker_loop(void *arg)
{
	int my_idx = (int)(intptr_t)arg;
	int my_tid = g_sd_conf.storage_engine_thread_num + my_idx;
	int rd_nr = g_sd_conf.read_worker_thread_num;
	int total_qpair_nr =
		g_sd_conf.storage_engine_thread_num + rd_nr;
	int target_cpu;
	struct epoll_event events[FILE_EPOLL_MAX_EVENTS];
	char thread_name[16];

	/* Mirror spdk_thread_do() in c-thread-pool/thpool.c so this
	 * thread looks like a regular IO worker to per-tid SPDK code:
	 *   - tls_tid identifies the qpair we own in g_io_threads[].
	 *   - tls_ioworker = 1 gates SPDK access (per existing asserts).
	 *   - CPU pinning matches iod_workers' policy so DMA buffers
	 *     allocated with SPDK_ENV_SOCKET_ID_ANY land on the right
	 *     NUMA node.
	 */
	tls_tid = my_tid;
	tls_ioworker = 1;
	target_cpu = oxb_sd_pin_cpu_for_tid(my_tid, total_qpair_nr);
	rd_worker_pin_cpu(target_cpu);

	snprintf(thread_name, sizeof(thread_name), "oxb_rd_w_%d", my_idx);
	prctl(PR_SET_NAME, thread_name);

	oxb_info("[read_worker] idx=%d tid=%d cpu=%d started",
		 my_idx, my_tid, target_cpu);

	while (!atomic_load_explicit(&g_read_worker_stop,
				     memory_order_relaxed)) {
		int n = epoll_wait(g_file_epoll_fd, events,
				   FILE_EPOLL_MAX_EVENTS, -1);
		if (n < 0) {
			if (errno == EINTR)
				continue;
			perror("epoll_wait(read_worker)");
			break;
		}

		/* Simple fsync QoS (option a):
		 * While any fsync is inflight, throttle the read_worker
		 * pool to ~25 % of its size so writes have headroom.
		 * Workers above the active quota briefly yield instead
		 * of competing for the device. EPOLLET ensures the events
		 * we received are still ours to process; we just delay
		 * processing by sleeping briefly, then re-check stop and
		 * fsync_inflight on the next iteration.
		 *
		 * (event indices i still need to be processed before we
		 * yield, so we apply the throttle before draining the
		 * batch.)
		 */
		if (nvme_fsync_inflight_get() > 0) {
			int active_quota = (rd_nr + 3) / 4;

			if (active_quota < 1)
				active_quota = 1;
			if (my_idx >= active_quota) {
				usleep(100);
				/* fall through to process events; we still
				 * own them and dropping them would stall the
				 * inode's pipeline. */
			}
		}

		/* Per-epoll-round batching: collect BIOs from ALL inodes
		 * processed in this round into a single tls_inline_batch
		 * and submit them as one nvme_submit_bio_inline() call.
		 * This keeps the batch large enough to drive per-qpair
		 * queue depth toward nvme_target_qd at high concurrency,
		 * instead of paying a separate submit/poll cycle for each
		 * inode's 1-2 BIOs. */
		tls_inline_batch_n = 0;
		tls_inline_batch_active = 1;

		for (int i = 0; i < n; i++) {
			struct epoll_event *ev = &events[i];

			if (ev->data.ptr == NULL) {
				uint64_t v;
				ssize_t r;

				/* Drain any pending wakeup tokens.
				 * Errno EAGAIN is fine: another worker
				 * already consumed the counter. */
				r = read(g_file_eventfd, &v, sizeof(v));
				(void)r;
				continue;
			}

			struct inode *inode =
				(struct inode *)ev->data.ptr;
			inode_msg_handler_inline(inode, ev->events);
		}

		inline_batch_flush();
		tls_inline_batch_active = 0;
	}

	oxb_info("[read_worker] idx=%d tid=%d exiting", my_idx, my_tid);
	return NULL;
}

int start_read_workers(void)
{
	int started = atomic_exchange_explicit(&g_read_worker_started, 1,
					       memory_order_acq_rel);
	int i;
	int rd_nr = g_sd_conf.read_worker_thread_num;

	if (started) {
		oxb_warn("read_workers already started");
		return 0;
	}

	if (rd_nr <= 0) {
		oxb_error("read_worker_thread_num not set or <= 0 (%d). "
			  "Set it in secure_daemon_conf.sh.",
			  rd_nr);
		return -1;
	}
	if (rd_nr > OXBOW_RD_WORKER_NR_MAX) {
		oxb_warn("read_worker_thread_num=%d exceeds "
			 "OXBOW_RD_WORKER_NR_MAX=%d; clamping.",
			 rd_nr, OXBOW_RD_WORKER_NR_MAX);
		rd_nr = OXBOW_RD_WORKER_NR_MAX;
		g_sd_conf.read_worker_thread_num = rd_nr;
	}

	atomic_store_explicit(&g_read_worker_stop, 0, memory_order_relaxed);

	for (i = 0; i < rd_nr; i++) {
		int rc = pthread_create(&g_read_worker_pthreads[i], NULL,
					read_worker_loop,
					(void *)(intptr_t)i);
		if (rc != 0) {
			oxb_error("pthread_create read_worker %d failed: %d",
				  i, rc);
			return -1;
		}
	}

	oxb_info("[%s] %d read_workers started (tid %d..%d)", __func__,
		 rd_nr, g_sd_conf.storage_engine_thread_num,
		 g_sd_conf.storage_engine_thread_num + rd_nr - 1);
	return 0;
}

void stop_read_workers(void)
{
	int started = atomic_load_explicit(&g_read_worker_started,
					   memory_order_acquire);
	int i;

	if (!started)
		return;

	atomic_store_explicit(&g_read_worker_stop, 1, memory_order_release);

	/*
	 * Wake each read_worker individually.
	 *
	 * The shared eventfd is level-triggered without EPOLLEXCLUSIVE.
	 * Naively writing the counter once is unreliable for waking N
	 * workers because:
	 *   - The first reader drains the entire counter (eventfd is not
	 *     in EFD_SEMAPHORE mode), so subsequent waiters never
	 *     observe the ready state.
	 *   - A worker mid-processing (inside process_inode_events ->
	 *     mpage -> nvme_submit_bio_inline) is not in epoll_wait at
	 *     the time of the write and may re-enter epoll_wait after
	 *     processing if the stop flag was set just before its
	 *     while-condition check.
	 *
	 * Robust shutdown: loop with pthread_tryjoin_np, re-poking the
	 * eventfd each round. Each write transitions the counter from 0
	 * to 1 (after a previous reader drained it), giving the kernel
	 * a fresh ready edge to wake another waiter. The brief sleep
	 * between attempts gives the woken worker time to read+exit.
	 */
	for (i = 0; i < g_sd_conf.read_worker_thread_num; i++) {
		for (;;) {
			uint64_t v = 1;
			ssize_t wret;
			int rc;

			wret = write(g_file_eventfd, &v, sizeof(v));
			(void)wret;

			rc = pthread_tryjoin_np(g_read_worker_pthreads[i],
						NULL);
			if (rc == 0)
				break;
			if (rc != EBUSY) {
				oxb_warn("pthread_tryjoin_np(read_worker %d) "
					 "returned %d",
					 i, rc);
				break;
			}
			usleep(10000); /* 10 ms */
		}
	}

	atomic_store_explicit(&g_read_worker_started, 0,
			      memory_order_release);
}
#endif /* OXBOW_RD_INLINE_SUBMIT */

static int file_dispatcher_init(void)
{
	int epfd, efd;
	struct epoll_event ev = { 0 };

	epfd = epoll_create1(EPOLL_CLOEXEC);
	if (epfd < 0) {
		perror("epoll_create1");
		return -1;
	}

	efd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
	if (efd < 0) {
		perror("eventfd");
		close(epfd);
		return -1;
	}

	ev.events = EPOLLIN;
	ev.data.ptr = NULL;
	if (epoll_ctl(epfd, EPOLL_CTL_ADD, efd, &ev) < 0) {
		perror("epoll_ctl ADD eventfd");
		close(efd);
		close(epfd);
		return -1;
	}

	g_file_epoll_fd = epfd;
	g_file_eventfd = efd;

#ifndef OXBOW_RD_INLINE_SUBMIT
	/* Legacy file_worker mode: launch N file_epoll_loop instances on
	 * the file_worker thpool. The D-2 path uses dedicated read_worker
	 * pthreads instead, started in start_read_workers(). */
	for (int i = 0; i < OXBOW_FILEWORKER_NR; i++)
		thpool_add_work(g_file_worker_thpool, file_epoll_loop_runner,
				NULL);
#endif

	return 0;
}

/**
 * @brief Register g_file_epoll_fd to epoll. It can be used for 
 * 
 * @param inode 
 * @return int 
 */
static int file_dispatcher_register(struct inode *inode)
{
	struct epoll_event ev = { 0 };
	int ret;

	iget(inode); // hold reference while registered

	ev.events = EPOLLIN | EPOLLERR | EPOLLHUP | EPOLLRDHUP | EPOLLET;
	ev.data.ptr = (void *)inode;

	pthread_mutex_lock(&g_file_epoll_mutex);
	ret = epoll_ctl(g_file_epoll_fd, EPOLL_CTL_ADD, inode->fd, &ev);
	pthread_mutex_unlock(&g_file_epoll_mutex);

	if (ret < 0) {
		perror("epoll_ctl ADD file fd error.");
		iput(inode);
		return -1;
	}

	atomic_fetch_add(&g_file_worker_count, 1);

	return 0;
}

int file_dispatcher_deregister(struct inode *inode)
{
	int ret;
	/* Mark closing; idempotent behavior */
	inode_lock(inode);
	if (inode->i_state & I_CLOSING) {
		inode_unlock(inode);
		return 0;
	}
	inode->i_state |= I_CLOSING;
	/* snapshot active state under lock */
	int active = !!(inode->i_state & I_IO_ACTIVE);
	inode_unlock(inode);

	pthread_mutex_lock(&g_file_epoll_mutex);
	ret = epoll_ctl(g_file_epoll_fd, EPOLL_CTL_DEL, inode->fd, NULL);
	pthread_mutex_unlock(&g_file_epoll_mutex);
	if (ret < 0) {
		if (errno != ENOENT && errno != EBADF) {
			oxb_warn("inode(%lu) epoll_ctl DEL file fd",
				 inode->i_ino);
			perror("epoll_ctl DEL file fd");
		}
	}

	/* If someone is actively handling this fd, defer cleanup to epoll thread */
	if (active) {
		return 0;
	}

	exit_file_worker(inode);
	atomic_fetch_add(&g_file_worker_exit_count, 1);
	return 0;
}

void file_worker_init_fn(void *arg)
{
	struct inode *inode = (struct inode *)arg;
	int ret = 0;

	BUG_ON(!inode, "no inode");
	BUG_ON(!inode->i_mapping, "not prepared");

	/* If inode is already closing/evicting/deleted, skip initialization. */
	inode_lock(inode);
	if (inode->i_state & (I_CLOSING | I_DELETED | I_EVICT)) {
		inode_unlock(inode);
		oxb_error("skip file init ino(%lu) state=0x%lx", inode->i_ino,
			  inode->i_state);
		return;
	}
	inode_unlock(inode);

	// Initialize and register this file with shared epoll dispatcher
	ret = init_file(inode);
	if (ret < 0) {
		oxb_error("failed to init file ino(%lu)", inode->i_ino);
		panic("failed to init file.");
		return;
	}

	ret = file_dispatcher_register(inode);
	if (ret < 0) {
		exit_file_worker(inode);
		oxb_error("failed to register file ino(%lu)", inode->i_ino);
		panic("failed to register file.");
		return;
	}
}

/**
 * @brief Request file initialization to the file worker thread. We avoid any
 * time-consuming operations here to make create or lookup return quickly.
 * 
 * @param inode 
 * @return int 
 */
int request_file_init(struct inode *inode)
{
	int ret = -1;
	bool set_once;

	d_debug("[%s] ino(%lu), inode count(%d)", __func__, inode->i_ino,
		inode->i_count);

	/* If this inode is closing/evicting/deleted, never reattach a worker. */
	inode_lock(inode);
	if (inode->i_state & (I_CLOSING | I_DELETED | I_EVICT)) {
		inode_unlock(inode);
		iput(inode);
		goto ret;
	}
	set_once = !!(inode->i_state & I_SET_ONCE);
	inode_unlock(inode);

	/* Inode should be initiated by lookup function, not at here */
	if (inode->i_state & I_NEW || !S_ISREG(inode->i_mode)) {
		oxb_error("inode(%d) state %d / mode %d", inode->i_ino,
			  inode->i_state, inode->i_mode);
		iput(inode);
		goto ret;
	}

	// log_debug("ino(%lu) start (%d)", inode->i_ino,
	// 	  inode->i_state & I_SET_ONCE);

	// it is already launched before
	if (set_once) {
		/*
		 * Fast path for inodes that were initialized once:
		 * - I_SET_ONCE guarantees shm/metadata were set up.
		 * - I_HAS_WORKER should remain true as long as the dispatcher
		 *   is registered; once we start closing (I_CLOSING), we bail
		 *   out above and never come here.
		 *
		 * Nothing to do here for now; keeping this branch explicit
		 * documents the intent and leaves room for future "revive"
		 * logic if needed.
		 */
		/*
		 * This inode was already initialized once:
		 * - I_SET_ONCE guarantees shm/metadata are ready.
		 * - A file worker should already be attached.
		 *
		 * From the caller's perspective this is a successful, fast-path
		 * no-op, so we must return 0 here to avoid spurious "init fail"
		 * errors on repeated lookups/opens of the same inode.
		 */
		ret = 0;
		goto ret;
	}

	/* Delegate initialization to the worker init thread */
	thpool_add_work(g_file_worker_init_thpool, file_worker_init_fn,
			(void *)inode);
	ret = 0;

ret:
	return ret;
}

void exit_file_workers(void) {
#ifdef OXBOW_RD_INLINE_SUBMIT
	stop_read_workers();
#endif

#ifdef THPOOL_PROFILE
	if (g_file_worker_thpool)
		thpool_profile_print_summary(g_file_worker_thpool);
	if (g_file_worker_init_thpool)
		thpool_profile_print_summary(g_file_worker_init_thpool);
#endif

	// TODO: Terminate gracefully.
	// thpool_wait(g_file_worker_thpool);
	// thpool_destroy(g_file_worker_thpool);
	// thpool_wait(g_file_worker_init_thpool);
	// thpool_destroy(g_file_worker_init_thpool);
}
