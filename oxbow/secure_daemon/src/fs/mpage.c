#include "fs/fs.h"
#include "io/sd_bio.h"
#include "io_dispatcher.h"
#include "oxbow_debug.h"
#include "profile_secure_daemon.h"
#include <stddef.h>
#include <time.h>
#include <sys/ioctl.h>
#include <stdatomic.h>

PF_TL_EVT(e002rca_ioctl_ra_end);
PF_TL_EVT(e004_ioctl_read_end);
PF_TL_EVT(bb1_ra_cache_consume);

PF_EVT(RA_KNL_REQ);
PF_EVT(RA_USER_REQ);

#define OXBOW_RA_STATS

#ifdef OXBOW_RA_STATS
/* Forward-declare user-level RA cache counters so that they are visible
 * to helper functions defined below.
 */
static atomic_ullong g_ra_cache_insert;
static atomic_ullong g_ra_cache_overwrite;
static atomic_ullong g_ra_cache_evict;
static atomic_ullong g_ra_cache_hit;
#endif

/* Max pages per readahead group for a single RA_END / BIO. */
#define MPAGE_RA_GRP_PAGES 32 // cat /sys/block/nvme1n1/queue/max_sectors_kb  shows 128 (32 pages)

#ifdef OXBOW_USER_READAHEAD
/* Max pages per user-level readahead BIO. */
#define MPAGE_USER_RA_GRP_PAGES 64 // Need to be the same as BIO_MAX_VECS (sd_bio.h)
#endif

/* =========================
 * User-level readahead cache helpers
 * ========================= */

int inode_ra_cache_insert(struct inode *inode, pgoff_t idx, const void *src)
{
	unsigned int slots, victim, i;
	char *base;
	unsigned int log_slot = 0;
	pgoff_t log_old_idx = 0;
	int log_type = 0; /* 0:none, 1:overwrite, 2:insert, 3:evict */

	if (!inode || !inode->ra_cache_buf || !inode->ra_cache_index ||
	    !inode->ra_cache_valid || inode->ra_cache_nr_slots == 0 ||
	    !src)
		return -1;

	pthread_spin_lock(&inode->ra_cache_lock);

	slots = inode->ra_cache_nr_slots;
	base = (char *)inode->ra_cache_buf;
	victim = slots; /* sentinel: no victim chosen yet */

	/* First pass: look for existing slot or a free slot. */
	for (i = 0; i < slots; i++) {
		if (inode->ra_cache_valid[i] &&
		    inode->ra_cache_index[i] == idx) {
			/* Overwrite existing cached page. */
			memcpy(base + (size_t)i * (size_t)PAGE_SIZE,
			       src, (size_t)PAGE_SIZE);
			log_type = 1;
			log_slot = i;
			goto out_unlock;
		}

		if (!inode->ra_cache_valid[i] && victim == slots)
			victim = i;
	}

	/* If no free slot, pick a victim in round-robin fashion. */
	if (victim == slots) {
		/* Evict an older entry. */
		victim = inode->ra_cache_cursor++ % slots;
		log_type = 3;
		log_slot = victim;
		log_old_idx = inode->ra_cache_index[victim];
	} else {
		/* Use a free slot. */
		log_type = 2;
		log_slot = victim;
	}

	inode->ra_cache_index[victim] = idx;
	memcpy(base + (size_t)victim * (size_t)PAGE_SIZE,
	       src, (size_t)PAGE_SIZE);
	inode->ra_cache_valid[victim] = 1;

out_unlock:
	pthread_spin_unlock(&inode->ra_cache_lock);

	(void)log_slot;
	(void)log_old_idx;

#ifdef OXBOW_RA_STATS
	/* Count cache operations for aggregated RA statistics. */
	if (log_type == 1) {
		atomic_fetch_add_explicit(&g_ra_cache_overwrite,
					  1ULL,
					  memory_order_relaxed);
	} else if (log_type == 2) {
		atomic_fetch_add_explicit(&g_ra_cache_insert,
					  1ULL,
					  memory_order_relaxed);
	} else if (log_type == 3) {
		atomic_fetch_add_explicit(&g_ra_cache_evict,
					  1ULL,
					  memory_order_relaxed);
	}
#endif

	return 0;
}

int inode_ra_cache_consume(struct inode *inode, pgoff_t idx)
{
	unsigned int slots, i;
	char *base;
	int hit = 0;
	unsigned int hit_slot = 0;

	if (!inode || !inode->ra_cache_buf || !inode->ra_cache_index ||
	    !inode->ra_cache_valid || inode->ra_cache_nr_slots == 0 ||
	    !inode->data)
		return 0;

	pthread_spin_lock(&inode->ra_cache_lock);

	slots = inode->ra_cache_nr_slots;
	base = (char *)inode->ra_cache_buf;

	for (i = 0; i < slots; i++) {
		if (!inode->ra_cache_valid[i])
			continue;
		if (inode->ra_cache_index[i] != idx)
			continue;

		/* Copy cached page into SHM-backed inode->data and mark
		 * it as consumed.
		 */
		memcpy((char *)inode->data + (size_t)idx * (size_t)PAGE_SIZE,
		       base + (size_t)i * (size_t)PAGE_SIZE,
		       (size_t)PAGE_SIZE);
		inode->ra_cache_valid[i] = 0;
		hit = 1;
		hit_slot = i;
		break;
	}

	pthread_spin_unlock(&inode->ra_cache_lock);

	if (hit) {
		(void)hit_slot;
#ifdef OXBOW_RA_STATS
		atomic_fetch_add_explicit(&g_ra_cache_hit,
					  1ULL,
					  memory_order_relaxed);
#endif
		/* Now raise SHM uptodate bit so that future READPAGE/RA
		 * can observe this page as up-to-date.
		 */
		mark_uptodate_in_shm(inode, (size_t)idx);
		return 1;
	}

	return 0;
}

#ifdef OXBOW_RA_STATS
/* Global counters to track how many kernel readpage/readahead and
 * user-level readahead operations are issued. These are for debugging and
 * performance analysis only and have no functional effect.
 *
 * - g_kern_readpage_pages: number of pages read via kernel readpage path.
 * - g_kern_ra_windows:     number of kernel readahead windows.
 * - g_kern_ra_pages:       total pages covered by kernel readahead windows.
 * - g_user_ra_windows:     number of user-level readahead windows.
 * - g_user_ra_pages_planned: total pages planned to be prefetched by
 *                            user-level readahead.
 * - g_user_ra_pages_completed: total pages that actually completed via
 *                              user-level readahead BIOs.
 *
 * User-level readahead cache statistics (see forward declarations at top):
 * - g_ra_cache_insert:     number of cache insert operations into empty slot.
 * - g_ra_cache_overwrite:  number of overwrites of an existing valid entry.
 * - g_ra_cache_evict:      number of evictions due to lack of free slots.
 * - g_ra_cache_hit:        number of successful cache hits on consume.
 */
static atomic_ullong g_kern_readpage_pages;
static atomic_ullong g_kern_ra_windows;
static atomic_ullong g_kern_ra_pages;
static atomic_ullong g_user_ra_windows;
static atomic_ullong g_user_ra_pages_planned;
static atomic_ullong g_user_ra_pages_completed;

void mpage_ra_stats_dump(void)
{
	unsigned long long rp;
	unsigned long long kra_win;
	unsigned long long kra_pages;
	unsigned long long ura_win;
	unsigned long long ura_planned;
	unsigned long long ura_done;
	unsigned long long cache_ins;
	unsigned long long cache_ovw;
	unsigned long long cache_evict;
	unsigned long long cache_hit;

	rp = atomic_load_explicit(&g_kern_readpage_pages, memory_order_relaxed);
	kra_win = atomic_load_explicit(&g_kern_ra_windows, memory_order_relaxed);
	kra_pages = atomic_load_explicit(&g_kern_ra_pages, memory_order_relaxed);
	ura_win = atomic_load_explicit(&g_user_ra_windows, memory_order_relaxed);
	ura_planned = atomic_load_explicit(&g_user_ra_pages_planned,
					   memory_order_relaxed);
	ura_done = atomic_load_explicit(&g_user_ra_pages_completed,
					memory_order_relaxed);
	cache_ins = atomic_load_explicit(&g_ra_cache_insert,
					 memory_order_relaxed);
	cache_ovw = atomic_load_explicit(&g_ra_cache_overwrite,
					 memory_order_relaxed);
	cache_evict = atomic_load_explicit(&g_ra_cache_evict,
					   memory_order_relaxed);
	cache_hit = atomic_load_explicit(&g_ra_cache_hit,
					 memory_order_relaxed);

	mpage_info("==== RA_STATS Summary ====");
	mpage_info("READPAGE: total_pages=%llu", rp);
	mpage_info("KERNEL_RA: windows=%llu pages=%llu", kra_win, kra_pages);
	mpage_info("USER_RA: windows=%llu planned_pages=%llu completed_pages=%llu",
		   ura_win, ura_planned, ura_done);
	mpage_info("USER_RA_CACHE: insert=%llu overwrite=%llu evict=%llu hit=%llu",
		   cache_ins, cache_ovw, cache_evict, cache_hit);
}
#else
void mpage_ra_stats_dump(void)
{
}
#endif

#ifdef OXBOW_USER_READAHEAD
/* Forward declaration for user-level readahead BIO completion. */
static void mpage_user_ra_end_io(void *args);

/* User-level readahead helper.
 *
 * When enabled, this function plans and issues additional read BIOs from
 * user space beyond the kernel-provided readahead window.  It does *not*
 * interact with the kernel's RA_END handshake:
 *
 *   - Kernel readahead (mpage_readahead) still owns ILLUFS_IOCTL_RA_END
 *     and associated waiters via mpage_end_io().
 *   - User-level readahead only prefetches data into SHM backed by
 *     inode->data so that future READPAGE/RA from the kernel can skip
 *     real I/O via check_uptodate_in_shm().
 *
 * This keeps the kernel/daemon READ/RA handshake simple while allowing
 * aggressive speculative prefetching entirely on the daemon side.
 */
static void mpage_user_readahead(struct inode *inode, pgoff_t kernel_index,
				 unsigned int kernel_nr)
{
	pgoff_t user_start, user_end;
	pgoff_t pg;
	pgoff_t last_pg;

	if (!inode || !inode->i_op || !inode->i_op->get_blocks)
		return;

	/* Compute user-level readahead window:
	 *   [user_start, user_end)
	 * starting *after* the kernel RA window.
	 *
	 * To avoid re-reading the same pages multiple times across successive
	 * kernel RA windows, we clamp user_start to the highest index already
	 * prefetched by user-level RA for this inode (ra_user_prefetch_idx).
	 * This ensures that each page is prefetched at most once by user-level
	 * RA in the forward direction.
	 */
	last_pg = (pgoff_t)SIZE_TO_BLK_NR(inode->i_size);
	if (kernel_index >= last_pg)
		return;

	/* Start just beyond the kernel window. */
	user_start = kernel_index + (pgoff_t)kernel_nr;
	if (user_start < inode->ra_user_prefetch_idx)
		user_start = inode->ra_user_prefetch_idx;
	if (user_start >= last_pg)
		return;

	user_end = kernel_index +
		   (pgoff_t)kernel_nr * OXBOW_USER_RA_WINDOW_MULT;
	if (user_end > last_pg)
		user_end = last_pg;

	if (user_end <= user_start)
		return;

	/* Record highest planned prefetch index so future calls do not
	 * re-issue user-level RA for pages below this boundary.
	 */
	inode->ra_user_prefetch_idx = user_end;

#ifdef OXBOW_RA_STATS
	{
		pgoff_t planned = user_end - user_start;

		atomic_fetch_add_explicit(&g_user_ra_windows, 1ULL,
					  memory_order_relaxed);
		atomic_fetch_add_explicit(
			&g_user_ra_pages_planned,
			(unsigned long long)planned,
			memory_order_relaxed);

		mpage_dbg("[RA_STATS] USER_RA inode=%lu kernel_index=%lu kernel_nr=%u user_start=%lu user_end=%lu planned=%lu",
			  inode->i_ino,
			  (unsigned long)kernel_index,
			  (unsigned int)kernel_nr,
			  (unsigned long)user_start,
			  (unsigned long)user_end,
			  (unsigned long)planned);
	}
#endif

	/*
	 * Plan log for user-level readahead window.
	 * Downgraded to debug level so that normal performance runs are not
	 * flooded; can be re-enabled via PRINT_MPAGE_DEBUG.
	 */
	mpage_dbg("[USER_RA_PLAN] inode=%lu kernel_index=%lu kernel_nr=%u user_start=%lu user_end=%lu",
		  inode->i_ino,
		  (unsigned long)kernel_index,
		  (unsigned int)kernel_nr,
		  (unsigned long)user_start,
		  (unsigned long)user_end);

	/*
	 * Issue grouped BIOs for pages in [user_start, user_end) that are not
	 * yet uptodate in SHM.  Pages are grouped in chunks of up to
	 * MPAGE_USER_RA_GRP_PAGES, mirroring the kernel readahead grouping
	 * policy while allowing a larger user-level RA granularity.
	 */
	for (pg = user_start; pg < user_end; ) {
		struct buffer_extent be = (struct buffer_extent){ 0 };
		struct bio *bio;
		pgoff_t max_pg_in_be;
		unsigned int remaining;
		unsigned int grp;
		unsigned int io_nr;
		unsigned int i;
		int all_uptodate = 1;
		int ret;

		remaining = (unsigned int)(user_end - pg);
		grp = remaining > MPAGE_USER_RA_GRP_PAGES ?
			MPAGE_USER_RA_GRP_PAGES : remaining;

		/* Fast path: entire group already uptodate in SHM. */
		for (i = 0; i < grp; i++) {
			if (!check_uptodate_in_shm(
				    inode, (size_t)(pg + (pgoff_t)i))) {
				all_uptodate = 0;
				break;
			}
		}

		if (all_uptodate) {
			pg += (pgoff_t)grp;
			continue;
		}

		/* Sanity check against file size. */
		if (SIZE_TO_BLK_NR(inode->i_size) < (size_t)pg + 1)
			break;

		/*
		 * Resolve physical extent for the first page in this group and
		 * cap the BIO so that it does not cross the extent boundary.
		 */
		inode_idx_lock_shared(inode);
		ret = inode->i_op->get_blocks(inode, &be, (sector_t)pg, 0);
		inode_idx_unlock(inode);
		if (ret) {
			oxb_error("[USER_RA] get_blocks failed inode=%lu index=%lu",
				  inode->i_ino, (unsigned long)pg);
			pg += (pgoff_t)grp;
			continue;
		}

		max_pg_in_be = (pgoff_t)be.iblock + (pgoff_t)be.nr;
		io_nr = grp;
		if (pg + (pgoff_t)io_nr > max_pg_in_be)
			io_nr = (unsigned int)(max_pg_in_be - pg);

		if (io_nr == 0) {
			pg += (pgoff_t)grp;
			continue;
		}

		bio = alloc_bio((unsigned short)io_nr, REQ_OP_READ);
		if (!bio) {
			oxb_error("[USER_RA] failed to alloc bio for inode=%lu index=%lu",
				  inode->i_ino, (unsigned long)pg);
			break;
		}

		bio->daemon_fd = inode->fd;
		bio->page_index = (size_t)pg;
		bio->bi_start = be.lba +
				(baddr_t)((pgoff_t)pg - (pgoff_t)be.iblock);
		bio->bi_vcnt = (unsigned short)io_nr;
		bio->bi_vtotal = (unsigned short)io_nr;
		bio->bi_io_vec->bv_buf =
			(char *)inode->data + PAGE_SIZE * (size_t)pg;
		/* Track owning inode for stats/logging in end_io. */
		bio->bi_private = inode;
		/* Mark this BIO as originating from user-level readahead so
		 * that NVMe layer can selectively apply no-op behaviour.
		 */
		bio->flags |= BIO_FLAG_USER_RA;
		bio->end_io = mpage_user_ra_end_io;

		mpage_dbg("[USER_RA_SUBMIT] inode=%lu start=%lu nr_pages=%u baddr=%lu bio=%p",
			  inode->i_ino,
			  (unsigned long)pg,
			  (unsigned int)io_nr,
			  (unsigned long)bio->bi_start,
			  bio);

		iod_submit_bio(REQ_OP_READ, bio);

		pg += (pgoff_t)io_nr;

		PF_TRACK_TPUT(RA_USER_REQ, io_nr);
	}
}
#endif

#ifdef OXBOW_TRACK_TPUT
/* Track how many readahead pages are in-flight (not yet RA_END'ed back
 * to the kernel) per-inode. This approximates the readahead window
 * queue depth for each file separately.
 */
static rt_q_stat g_q_ra_inflight_pages;

static inline void mpage_ra_inflight_add(struct inode *inode, uint32_t nr)
{
	static int inited;

	if (!inited) {
		init_rt_q_stat(&g_q_ra_inflight_pages, "ra_pages_inflight");
		inited = 1;
	}

	if (!inode)
		return;

	atomic_fetch_add_explicit(&inode->ra_inflight_pages, nr,
				  memory_order_relaxed);
	/* Best-effort depth sampling; exact atomic load is unnecessary
	 * for profiling-only statistics.
	 */
	check_rt_q(&g_q_ra_inflight_pages, inode->ra_inflight_pages,
		   (uint64_t)inode->i_ino);
}

static inline void mpage_ra_inflight_sub(struct inode *inode, uint32_t nr)
{
	if (!inode)
		return;

	atomic_fetch_sub_explicit(&inode->ra_inflight_pages, nr,
				  memory_order_relaxed);
	/* Best-effort depth sampling; exact atomic load is unnecessary
	 * for profiling-only statistics.
	 */
	check_rt_q(&g_q_ra_inflight_pages, inode->ra_inflight_pages,
		   (uint64_t)inode->i_ino);
}
#endif

/**
 * @brief Handle end_io, unlock folio in the kernel and free bio.
 *        will be called by polling thread through callback function
 * 		  (at the end of last page io done)
 */
void mpage_end_io(void *args)
{
	struct bio *bio;
	struct illufs_ioctl_ra_end end_io;
	struct inode *inode = NULL;

	bio = args;
	inode = (struct inode *)bio->bi_private;

	end_io.ractl = (__u64)bio->ractl;
	end_io.index = bio->page_index;
	end_io.nr_pages = bio->bi_vtotal;
	end_io.success = 1;

	nvme_debug("[%s] bio(%lu) nr(%lu) end", __func__, end_io.index,
		   end_io.nr_pages);

	/*
	 * RA_END path for kernel readahead BIOs.
	 * If we ever see a [DAEMON_RA_REQ] in secure_daemon without a
	 * matching RA_END_OK/illufs side completion, it means either:
	 *   - this function was never called, or
	 *   - the ioctl() below failed.
	 */
	/* Debug-only RA_END trace; keep disabled for normal performance runs. */
	mpage_dbg("[RA_END_SEND] type=normal inode=%lu index=%lu nr=%u ractl=%p success=%d bio=%p",
		  inode ? inode->i_ino : 0,
		  (unsigned long)end_io.index,
		  (unsigned int)end_io.nr_pages,
		  (void *)end_io.ractl,
		  (int)end_io.success,
		  bio);

	PF_TL_START(e002rca_ioctl_ra_end);
	if (ioctl(bio->daemon_fd, ILLUFS_IOCTL_RA_END, &end_io) < 0)
		oxb_error("[%s] fail", __func__);
	PF_TL_END(e002rca_ioctl_ra_end);

#ifdef OXBOW_TRACK_TPUT
	/* Pages in this BIO are now fully reported back to the kernel. */
	mpage_ra_inflight_sub(inode, (uint32_t)bio->bi_vtotal);
#endif

	free(bio->bi_io_vec);
	free(bio);

	nvme_debug("[%s] bio(%lu) nr(%lu) done", __func__, bio->page_index,
		   bio->bi_vcnt);
}

void single_end_io(void *args)
{
	struct bio *bio = args;

	mpage_dbg("[READ_END_SEND] index=%lu folio=%p bio=%p vcnt=%lu",
		  (unsigned long)bio->page_index,
		  (void *)bio->folio, bio,
		  (unsigned long)bio->bi_vcnt);

	PF_TL_START(e004_ioctl_read_end);
	if (ioctl(bio->daemon_fd, ILLUFS_IOCTL_READ_END, (__u64)bio->folio))
		oxb_error("fail to wake");
	PF_TL_END(e004_ioctl_read_end);

	free(bio->bi_io_vec);
	free(bio);

	nvme_debug("[%s] bio(%lu) nr(%lu) done", __func__, bio->page_index,
		   bio->bi_vcnt);
}

#ifdef OXBOW_USER_READAHEAD
/* Completion callback for user-level readahead BIOs.
 *
 * These BIOs are not tied to any kernel readahead control structure
 * (ractl), so they do not issue ILLUFS_IOCTL_RA_END.  They simply
 * populate SHM-backed pages and update user-level RA statistics.
 */
static void mpage_user_ra_end_io(void *args)
{
	struct bio *bio = args;
	struct inode *inode = (struct inode *)bio->bi_private;

	(void)inode;

#ifdef OXBOW_RA_STATS
	/* Count pages that actually completed via user-level readahead. */
	atomic_fetch_add_explicit(
		&g_user_ra_pages_completed,
		(unsigned long long)bio->bi_vtotal,
		memory_order_relaxed);
#endif

	/* Debug-level completion trace; can be re-enabled via PRINT_MPAGE_DEBUG. */
	mpage_dbg("[USER_RA_END] inode=%lu start=%lu nr=%u bio=%p",
		  (unsigned long)(inode ? inode->i_ino : 0),
		  (unsigned long)bio->page_index,
		  (unsigned int)bio->bi_vtotal,
		  bio);

	free(bio->bi_io_vec);
	free(bio);
}
#endif

void end_io_no_bio(int dfd, u64 folio)
{
	PF_TL_START(e004_ioctl_read_end);
	if (ioctl(dfd, ILLUFS_IOCTL_READ_END, folio))
		oxb_error("fail to wake");
	PF_TL_END(e004_ioctl_read_end);
}

/**
 * @brief submit bio to io dispatcher.
 * 
 */
static void mpage_bio_submit(blk_opf_t opf, struct bio *bio)
{
	/* Log BIOs constructed in mpage before submitting to IO dispatcher. */
	mpage_dbg("[BIO_SUBMIT] op=%u page_index=%lu baddr=%lu nr_pages=%u",
		 (unsigned int)(opf & REQ_OP_MASK),
		 bio->page_index, bio->bi_start, bio->bi_vtotal);

	/*
	 * Debug-level log for kernel readahead BIO submission.
	 * This mirrors [READPAGE_SUBMIT] for single-page reads so that we
	 * can correlate full RA paths when PRINT_MPAGE_DEBUG is enabled.
	 */
	mpage_dbg("[KERNEL_RA_SUBMIT] op=%u page_index=%lu baddr=%lu nr_pages=%u bio=%p ractl=%p",
		  (unsigned int)(opf & REQ_OP_MASK),
		  (unsigned long)bio->page_index,
		  (unsigned long)bio->bi_start,
		  (unsigned int)bio->bi_vtotal,
		  bio,
		  (void *)bio->ractl);

#ifdef OXBOW_NOOP_MPAGE_BIO_SUBMIT
	// Pass mpage bio submit.
#else
	bio->bi_vcnt = bio->bi_vtotal;
	bio->end_io = mpage_end_io;
	iod_submit_bio(opf, bio);
#endif
}

static void single_bio_submit(blk_opf_t opf, struct bio *bio)
{
	/* Log single-page BIOs (random read path). */
	mpage_dbg("[READPAGE_SUBMIT] op=%u page_index=%lu folio=%p baddr=%lu nr_pages=%u bio=%p",
		  (unsigned int)(opf & REQ_OP_MASK),
		  (unsigned long)bio->page_index,
		  (void *)bio->folio,
		  (unsigned long)bio->bi_start,
		  (unsigned int)bio->bi_vtotal,
		  bio);

	bio->end_io = single_end_io;
	iod_submit_bio(opf, bio);
}

int mpage_block_alloc(struct inode *inode, size_t idx)
{
	struct buffer_extent be;
	int ret;
	BUG_ON(inode->i_op->get_blocks == NULL, "no get block");

	inode_idx_lock(inode);
	ret = inode->i_op->get_blocks(inode, &be, idx, 1);
	inode_idx_unlock(inode);
	return ret;
}

static void init_mpage_bio(struct bio *bio, size_t pg_idx,
			   struct readahead_control *rac, baddr_t start)
{
	bio->daemon_fd = rac->inode->fd;
	bio->page_index = pg_idx;
	bio->ractl = rac->kernel_ractl;
	bio->bi_start = start;
	bio->bi_io_vec->bv_buf =
		(char *)(rac->inode->data) + pg_idx * PAGE_SIZE;
	bio->bi_private = rac->inode;
}

/**
 * @brief read single page from disk, may be called on random read.
 */
void mpage_readpage(struct read_control *rc)
{
	struct bio *bio;
	struct buffer_extent be = { 0 };
	struct inode *inode;
	int ret;

	PF_TL_START(ba_mpage_readpage);

	inode = rc->inode;

#ifdef OXBOW_RA_STATS
	/* Count kernel readpage calls as pages. */
	{
		atomic_fetch_add_explicit(
				&g_kern_readpage_pages,
				1ULL,
			memory_order_relaxed);

		mpage_dbg("[RA_STATS] READPAGE inode=%lu index=%lu",
			  inode->i_ino,
			  (unsigned long)rc->index);
	}
#endif

#ifdef OXBOW_NOOP_BIO_READ
	/*
	 * No-op mode:
	 * Immediately notify the kernel that this folio is ready without
	 * issuing any real I/O.
	 */
	end_io_no_bio(inode->fd, rc->folio);
	PF_TL_END(ba_mpage_readpage);
	return;
#endif

	BUG_ON(inode->i_op->get_blocks == NULL, "no get_blocks");

	bio = alloc_bio(1, REQ_OP_READ);
	if (!bio) {
		oxb_error("failed to alloc bio");
		end_io_no_bio(inode->fd, rc->folio);
		return;
	}

	bio->page_index = rc->index;
	bio->bi_vcnt = 1;
	bio->bi_vtotal = 1;
	bio->daemon_fd = inode->fd;
	bio->folio = rc->folio;
	/* Track owning inode so that NVMe completion can mark SHM pages
	 * uptodate for this READPAGE I/O as well.
	 */
	bio->bi_private = inode;

	if (SIZE_TO_BLK_NR(inode->i_size) < rc->index + 1) {
		oxb_warn("[%s] inconsistent size(%luKiB, %luMiB) index(%lu-%luKB)",
			 __func__, inode->i_size >> 10, inode->i_size >> 20,
			 rc->index, rc->index * 4);
		single_end_io(bio);
		return;
	}

	inode_idx_lock_shared(inode);

	PF_TL_START(baa_get_blocks);

	ret = inode->i_op->get_blocks(inode, &be, rc->index, 0);

	PF_TL_END(baa_get_blocks);

	inode_idx_unlock(inode);
	if (ret) {
		oxb_error("[%s] get_block failed. index(%lu) iblock(%lu)",
			  __func__, rc->index, SIZE_TO_BLK_NR(inode->i_size));
		end_io_no_bio(inode->fd, rc->folio);
		usleep(100);
		panic("[mpage_readpage] iget_blocks failed");
		goto free;
	}

	bio->bi_io_vec->bv_buf = (char *)inode->data + PAGE_SIZE * rc->index;
	bio->bi_start = rc->index - be.iblock + be.lba;

	mpage_dbg("[%s] inode=%lu index=%lu baddr=%lu", __func__, inode->i_ino, rc->index, bio->bi_start);

	single_bio_submit(REQ_OP_READ, bio);

	/* on success */
	mpage_dbg("[%s] done", __func__);

	PF_TL_END(ba_mpage_readpage);

	return;

free:
	free(bio->bi_io_vec);
	free(bio);
}

static void mpage_end_io_noread(pgoff_t start, int nr,
				struct readahead_control *rac)
{
	struct illufs_ioctl_ra_end end_io;

	mpage_dbg("[%s] bio(%lu) nr(%lu)", __func__, start, nr);
	end_io.ractl = rac->kernel_ractl;
	end_io.index = start;
	end_io.nr_pages = nr;
	end_io.success = 1;

	/* Debug-only RA_END trace for no-read completions. */
	mpage_dbg("[RA_END_SEND] type=noread inode=%lu index=%lu nr=%u ractl=%p success=%d",
		  rac->inode ? rac->inode->i_ino : 0,
		  (unsigned long)end_io.index,
		  (unsigned int)end_io.nr_pages,
		  (void *)end_io.ractl,
		  (int)end_io.success);

	PF_TL_START(e002rca_ioctl_ra_end);
	if (ioctl(rac->inode->fd, ILLUFS_IOCTL_RA_END, &end_io) < 0)
		oxb_error("[%s] fail", __func__);
	PF_TL_END(e002rca_ioctl_ra_end);
#ifdef OXBOW_TRACK_TPUT
	/* Pages [start, start+nr) are reported without actual I/O. */
	mpage_ra_inflight_sub(rac->inode, (uint32_t)nr);
#endif
}

void mpage_end_io_failed(pgoff_t start, int nr, struct readahead_control *rac)
{
	struct illufs_ioctl_ra_end end_io;

	d_debug("[%s] bio(%lu) nr(%lu)", __func__, start, nr);
	end_io.ractl = rac->kernel_ractl;
	end_io.index = start;
	end_io.nr_pages = nr;
	end_io.success = 0;

	/* Debug-only RA_END trace for failed completions. */
	mpage_dbg("[RA_END_SEND] type=failed inode=%lu index=%lu nr=%u ractl=%p success=%d",
		  rac->inode ? rac->inode->i_ino : 0,
		  (unsigned long)end_io.index,
		  (unsigned int)end_io.nr_pages,
		  (void *)end_io.ractl,
		  (int)end_io.success);

	if (ioctl(rac->inode->fd, ILLUFS_IOCTL_RA_END, &end_io) < 0)
		oxb_error("[%s] fail", __func__);
}

/**
 * @brief Read multiple blocks from disk, may be called on readahead.
 * 
 * @param rac 
 * @param get_block (filesystem specific function)
 */
void mpage_readahead(struct readahead_control *rac)
{
	struct inode *inode;
	pgoff_t last_pg;
	pgoff_t first;
	unsigned int total;
	unsigned int usable;
	unsigned int leftover;
	get_blocks_t get_blocks;
	int ret = 0;

	PF_TL_START(bb_mpage_readahead);

	inode = rac->inode;

#ifdef OXBOW_DISABLE_KERNEL_RA
	/*
	 * Debug mode: logically disable kernel readahead without changing
	 * the kernel-side readahead logic.
	 *
	 * We immediately report RA_END with success=0 for the entire window
	 * so that:
	 *   - folios in this window are unlocked but NOT marked uptodate, and
	 *   - subsequent accesses fall back to illufs_read_folio() one page
	 *     at a time.
	 *
	 * This keeps the RA state machine alive (ra_cnt, ractl, etc.) while
	 * forcing all actual data I/O through the READPAGE/single-bio path.
	 */
	oxb_warn("[KERNEL_RA_DISABLED] inode=%lu index=%lu nr=%u",
		 inode->i_ino,
		 (unsigned long)rac->index,
		 (unsigned int)rac->nr_pages);

	mpage_end_io_failed(rac->index, rac->nr_pages, rac);
	PF_TL_END(bb_mpage_readahead);
	return;
#endif

#ifdef OXBOW_RA_STATS
	/* Count kernel readahead window and pages. */
	{
		atomic_fetch_add_explicit(
				&g_kern_ra_windows,
			1ULL,
			memory_order_relaxed);
		atomic_fetch_add_explicit(
			&g_kern_ra_pages,
			(unsigned long long)rac->nr_pages,
			memory_order_relaxed);

		mpage_dbg("[RA_STATS] KERNEL_RA inode=%lu index=%lu nr=%u",
			  inode->i_ino,
			  (unsigned long)rac->index,
			  (unsigned int)rac->nr_pages);
	}
#endif

#ifdef OXBOW_TRACK_TPUT
	/* rac->nr_pages pages will be eventually reported via RA_END.
	 * Track them as "in-flight readahead pages" per-inode. */
	mpage_ra_inflight_add(rac->inode, (uint32_t)rac->nr_pages);
	PF_TRACK_TPUT(RA_KNL_REQ, rac->nr_pages);
#endif

#ifdef OXBOW_NOOP_BIO_READ
	/*
	 * No-op mode:
	 * Tell the kernel that all pages in this readahead window are
	 * completed without touching storage.
	 */
	mpage_end_io_noread(rac->index, rac->nr_pages, rac);
	PF_TL_END(bb_mpage_readahead);
	return;
#endif

	BUG_ON(inode->i_op->get_blocks == NULL, "no get_blocks");
	get_blocks = inode->i_op->get_blocks;

	/* Log kernel readahead requests: index and number of pages. */
	mpage_dbg("[RA_REQ] inode=%lu index=%lu nr_pages=%u",
		 inode->i_ino, rac->index, rac->nr_pages);

	/* Appending case arrived here? maybe due to readahed? */
	if (SIZE_TO_BLK_NR(inode->i_size) < rac->index + 1 || !rac->need) {
		mpage_dbg("[%s] inconsistent size(%luKiB, %luMiB) index(%lu-%luKB)",
			 __func__, inode->i_size >> 10, inode->i_size >> 20,
			 rac->index, rac->index * 4);
		mpage_end_io_noread(rac->index, rac->nr_pages, rac);

		PF_TL_END(bb_mpage_readahead);
		return;
	}

	/*
	 * Readahead BIO grouping policy:
	 *
	 *   - Group pages in this window into chunks of up to
	 *     MPAGE_RA_GRP_PAGES pages.
	 *   - For each chunk:
	 *       * If all pages are already uptodate in SHM, report them via
	 *         mpage_end_io_noread() without issuing real I/O.
	 *       * Otherwise, issue one BIO covering up to
	 *         MPAGE_RA_GRP_PAGES contiguous pages and let mpage_end_io()
	 *         send a single RA_END for the group.
	 *
	 * This keeps RA_END granularity at roughly MPAGE_RA_GRP_PAGES pages
	 * while avoiding excessive per-page BIO overhead.
	 */
	first = rac->index;
	total = rac->nr_pages;
	last_pg = (pgoff_t)SIZE_TO_BLK_NR(inode->i_size);

	if (first >= last_pg) {
		/* Window starts beyond EOF: just report as no-read. */
		mpage_end_io_noread(first, (int)total, rac);
		PF_TL_END(bb_mpage_readahead);
		return;
	}

	/* Cap the usable window by file size; leftover (if any) is no-read. */
	if (first + (pgoff_t)total > last_pg) {
		usable = (unsigned int)(last_pg - first);
		leftover = total - usable;
	} else {
		usable = total;
		leftover = 0;
	}

	if (usable == 0) {
		if (leftover > 0)
			mpage_end_io_noread(first, (int)leftover, rac);
		PF_TL_END(bb_mpage_readahead);
		return;
	}

	/* Process usable pages in groups of up to MPAGE_RA_GRP_PAGES pages. */
	{
		pgoff_t pg = first;

		while ((unsigned int)(pg - first) < usable) {
			unsigned int remaining;
			unsigned int grp;
			unsigned int i;
			int all_uptodate = 1;

			remaining = (unsigned int)((first + (pgoff_t)usable) -
						   pg);
			grp = remaining > MPAGE_RA_GRP_PAGES ?
				MPAGE_RA_GRP_PAGES : remaining;

			/* First, try to satisfy this group's pages from the
			 * user-level readahead cache by copying cached data
			 * into SHM and marking them uptodate.
			 *
			 * When user-level RA is disabled (OXBOW_USER_READAHEAD
			 * not defined), no pages are ever inserted into this
			 * cache, so walking it on every kernel RA window is
			 * pure overhead.  Guard this block so that we only pay
			 * the cache lookup cost when user-level RA is actually
			 * in use.
			 *
			 * bb1_ra_cache_consume measures how much time is spent
			 * just walking the per-inode RA cache for this group.
			 */
#ifdef OXBOW_USER_READAHEAD
			PF_TL_START(bb1_ra_cache_consume);
			for (i = 0; i < grp; i++) {
				(void)inode_ra_cache_consume(
					inode, pg + (pgoff_t)i);
			}
			PF_TL_END(bb1_ra_cache_consume);
#endif

			/* Fast path: entire group already uptodate in SHM.
			 * bb2_ra_check_shm captures the cost of scanning SHM
			 * uptodate bits for this group, separate from cache
			 * lookup and BIO submission.
			 */
			PF_TL_START(bbb_ra_check_shm);
			for (i = 0; i < grp; i++) {
				if (!check_uptodate_in_shm(
					    inode, (size_t)(pg + (pgoff_t)i))) {
					all_uptodate = 0;
					break;
				}
			}
			PF_TL_END(bbb_ra_check_shm);

			if (all_uptodate) {
				mpage_end_io_noread(pg, (int)grp, rac);
				pg += (pgoff_t)grp;
				continue;
			}

			/* Issue a BIO for this group (up to 4 contiguous pages). */
			if (pg >= last_pg)
				break;

			{
				struct buffer_extent be = { 0 };
				struct bio *bio;
				pgoff_t max_pg_in_be;
				unsigned int io_nr = grp;

				/* End-to-end cost of translating this group
				 * into a BIO is tracked by bbc_ra_issue_bio.
				 *
				 * Inside this, we further break down:
				 *   - bbca_get_blocks_ra: metadata lookup
				 *   - bbcb_alloc_init_bio: alloc_bio() +
				 *       init_mpage_bio() + bookkeeping
				 *   - bbcc_submit_bio: mpage_bio_submit()
				 */
				PF_TL_START(bbc_ra_issue_bio);

				inode_idx_lock_shared(inode);
				PF_TL_START(bbca_get_blocks_ra);
				ret = get_blocks(inode, &be, (sector_t)pg, 0);
				PF_TL_END(bbca_get_blocks_ra);
				inode_idx_unlock(inode);
				if (ret) {
					oxb_error(
						"[mpage_readahead] get_blocks failed: start=%lu nr=%u",
						(unsigned long)pg, grp);
					mpage_end_io_failed(pg, (int)grp, rac);
					PF_TL_END(bbc_ra_issue_bio);
					PF_TL_END(bb_mpage_readahead);
					return;
				}

				/* Ensure the BIO does not cross extent boundary. */
				max_pg_in_be =
					(pgoff_t)be.iblock + (pgoff_t)be.nr;
				if (pg + (pgoff_t)io_nr > max_pg_in_be)
					io_nr = (unsigned int)(max_pg_in_be -
							       pg);

				PF_TL_START(bbcb_alloc_init_bio);
				bio = alloc_bio((unsigned short)io_nr,
						REQ_OP_READ);
				if (!bio) {
					oxb_error(
						"[mpage_readahead] failed to alloc bio for group: start=%lu nr=%u",
						(unsigned long)pg, io_nr);
					mpage_end_io_failed(pg, (int)io_nr, rac);
					PF_TL_END(bbcb_alloc_init_bio);
					PF_TL_END(bbc_ra_issue_bio);
					PF_TL_END(bb_mpage_readahead);
					return;
				}

				init_mpage_bio(
					bio, (size_t)pg, rac,
					be.lba +
						(baddr_t)((pgoff_t)pg -
							  (pgoff_t)be.iblock));
				bio->bi_vtotal = (unsigned short)io_nr;

				mpage_dbg(
					"[RA_GRP_SUBMIT] inode=%lu start=%lu nr_pages=%u baddr=%lu bio=%p",
					inode->i_ino,
					(unsigned long)pg,
					(unsigned int)io_nr,
					(unsigned long)bio->bi_start, bio);
				PF_TL_END(bbcb_alloc_init_bio);

				PF_TL_START(bbcc_submit_bio);
				mpage_bio_submit(REQ_OP_READ, bio);
				PF_TL_END(bbcc_submit_bio);

				PF_TL_END(bbc_ra_issue_bio);

				pg += (pgoff_t)io_nr;
			}
		}
	}

	/* Leftover pages beyond file size are reported as no-read. */
	if (leftover > 0) {
		pgoff_t left_start = first + (pgoff_t)usable;

		mpage_end_io_noread(left_start, (int)leftover, rac);
	}

	mpage_dbg("[%s] done", __func__);

#ifdef OXBOW_USER_READAHEAD
	/* Kick off user-level readahead for pages beyond this kernel RA window. */
	mpage_user_readahead(inode, rac->index, rac->nr_pages);
#endif

	PF_TL_END(bb_mpage_readahead);

#ifdef OXBOW_NOOP_MPAGE_BIO_SUBMIT
	/*
	 * No-op mode:
	 * Tell the kernel that all pages in this readahead window are
	 * completed without touching storage.
	 */
	mpage_end_io_noread(rac->index, rac->nr_pages, rac);
#endif
	return;
}
