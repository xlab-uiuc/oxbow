/* Local IO */
#include "buffer_head.h"
#include "global.h"
#include "config.h"
#include "io/nvme.h"
#include "io_dispatcher.h"
#include "profile_secure_daemon.h"
#include <pthread.h>
#include <stdatomic.h>
#include <stddef.h>
#include <string.h>
#include <time.h>
#include "thpool.h"
#include "oxbow_debug.h"
#include "common/oxbow.h"
#include "common/time_stat.h"
#include "common/ring_buffer_mpmc.h"

#ifdef OXBOW_RD_INLINE_SUBMIT
/*
 * Thread-local batch used by D-2 read_workers in process_inode_events()
 * to accumulate BIOs from a single inode's mpage_readahead/readpage call
 * chain, then submit them to the worker's own SPDK qpair in one shot via
 * nvme_submit_bio_inline().
 *
 * iod_submit_bio(REQ_OP_READ) routes here when tls_inline_batch_active is
 * non-zero. Outside of read_worker context the batch is unused and
 * iod_submit_bio(REQ_OP_READ) panics (use iod_submit_bio_general()
 * instead, which routes to the legacy ring + pump path).
 */
__thread struct bio *tls_inline_batch[INLINE_BATCH_MAX];
__thread int tls_inline_batch_n;
__thread int tls_inline_batch_active;
#endif

/* Short spin optimization for reducing wakeups was removed due to
 * potential starvation under heavy load. Always wake a worker for now.
 */

PF_TL_EVT(e002f_rd_enqueue);

struct thpool_ *iod_workers;
struct ring_buffer_mpmc g_rd_bio_ring_buf;

#ifdef OXBOW_TRACK_TPUT
/* Real-time queue depth for read BIO ring buffer. */
static rt_q_stat g_q_rd_bio_ring;
#endif

int init_io_dispatcher(void)
{
	struct nvme_config nvme_conf;
	int se_thread_nr;
	int total_qpair_nr;
	int ret;

	log_info("Initializing IO Dispatcher");

	if (g_sd_conf.pcie_nvme_nr_vfs <= 0) {
		log_error("pcie_nvme_addr is empty or unparseable. Set a "
			  "comma-separated (or space-separated) list of PCIe BDFs, e.g. "
			  "'0000:d8:00.1,0000:d8:00.2'.");
		return -1;
	}

	set_nvme_config_list(&nvme_conf, g_sd_conf.pcie_nvme_addr_list,
			     g_sd_conf.pcie_nvme_nr_vfs);

	se_thread_nr = g_sd_conf.storage_engine_thread_num;
#ifdef OXBOW_RD_INLINE_SUBMIT
	/* Reserve trailing tids for D-2 read_workers (initialized in
	 * src/kernfs/file_ops.c). They take their own tls_tid and qpair
	 * but are not part of the iod_workers thpool. */
	total_qpair_nr = se_thread_nr + g_sd_conf.read_worker_thread_num;
#else
	total_qpair_nr = se_thread_nr;
#endif
	iod_workers = nvme_init(&nvme_conf, se_thread_nr, total_qpair_nr);
	if (!iod_workers) {
		log_error("Storage engine init failed.");
		return -1;
	}

	// Initialize a global bio ring buffer.
	ret = ring_buffer_mpmc_create(&g_rd_bio_ring_buf, 2 << 20); // 2M
	if (ret < 0) {
		log_error("Failed to initialize bio ring buffer.");
		return -1;
	}

#ifdef OXBOW_TRACK_TPUT
	init_rt_q_stat(&g_q_rd_bio_ring, "rd_bio_ring");
#endif

	/* Dedicated long-lived read workers are removed.
	 * Read BIOs are now drained by short-lived budgeted pump jobs,
	 * scheduled on-demand when new read work is enqueued.
	 *
	 * In OXBOW_RD_INLINE_SUBMIT mode the pump path remains compiled
	 * but is used only by iod_submit_bio_general() (e.g.,
	 * sync_device.c admin debug). Production reads bypass it.
	 */
	nvme_init_rd_workers(se_thread_nr);

	log_info("[%s] iod_workers(%d), total qpairs(%d)", __func__,
		 se_thread_nr, total_qpair_nr);
	return 0;
}

void exit_io_dispatcher(void)
{
	oxb_info("Exiting IO Dispatcher.");

	nvme_destroy_rd_workers();
	ring_buffer_mpmc_destroy(&g_rd_bio_ring_buf);

	// iod_workers is destroyed in msg.c by destroying rpc_libfs_handler_thpool.
}

// /**
//  * @brief Add bvec to bio. If there is no empty slots in the bio,
//  * its caller should allocate a new bio and call this function again.
//  *
//  * @param bio Where bvec is added.
//  * @param bvec
//  * @return int 0 on success. -1 on error (bio is full).
//  */
// static int iod_add_bvec_to_bio(struct bio *bio, struct bio_vec *bvec)
// {
// 	// If BIO is Full. New one is allocated by its caller.
// 	if (bio->bi_vcnt == BIO_MAX_VECS)
// 		return -1;

// 	// Add bvecs to bio by copying bvecs.
// 	memcpy(&bio->bi_io_vec[bio->bi_vcnt], bvec, sizeof(struct bio_vec));

// 	// Update bvec_cnt.
// 	bio->bi_vcnt++;

// 	return 0;
// }

// // FIXME: This function is errorneous. It does not check the block number of the
// // new bvec. It can make a bio that has non-contiguous blocks which is incorrect
// // by the definition of the bio, "A group of IO requests on to contiguous blocks."
// /**
//  * @brief Add bio vector (bvec) to bio list.
//  *
//  * @param bl
//  * @param bvec
//  * @param blk_no
//  * @return int Return 0 on success.
//  */
// static int add_bvec_to_bl(struct bio_list *bl, struct bio_vec *bvec,
// 			  baddr_t blk_no)
// {
// 	struct bio *bio, *new_bio;
// 	int ret;

// 	if (bio_list_empty(bl)) {
// 		bio = alloc_bio(blk_no);
// 		bio_list_add(bl, bio);
// 	} else {
// 		// Get the last entry that may not be filled with bio vectors yet.
// 		bio = bio_list_peek_tail(bl);
// 	}

// 	ret = iod_add_bvec_to_bio(bio, bvec);

// 	if (ret < 0) {
// 		//Alloc new bio.
// 		new_bio = alloc_bio(blk_no);
// 		bio_list_add(bl, new_bio);

// 		// Retry.
// 		ret = iod_add_bvec_to_bio(new_bio, bvec);
// 	}

// 	return ret;
// }

/**
 * @brief Adding a new I/O request to bio list as a bio vector (bvec).
 * The requested buffer should be mapped to contiguous blocks.
 * 
 * @param bl
 * @param buf buffer for I/O
 * @param blk_no 
 * @param len size in byte
 * @return int 0 on success.
 */
// int iod_add_io_req_to_bl(struct bio_list *bl, char *buf, baddr_t blk_no,
// 			 size_t len)
// {
// 	struct bio_vec bvec;
// 	int ret;

// 	bvec.bv_buf = buf;
// 	bvec.bv_len = len;

// 	ret = add_bvec_to_bl(bl, &bvec, blk_no);

// 	if (ret < 0)
// 		log_error("Adding bio vector to bio list failed.");

// 	return ret;
// }

// static struct bio_list *build_bio_list(struct bio *bio, struct mb_map *mb_map,
// 				       uint64_t mb_map_nr)
// {
// 	const size_t sequence_max_io = g_max_nvme_max_io_size;
// 	int sequence_left = nvme_get_seq_max();
// 	char *prev_end, *cur_buf;
// 	uint64_t i, cur_cnt, blk_cnt, max_blk;
// 	baddr_t start_blk, cur_blk, last_blk;
// 	struct bio_vec bvec;

// 	cur_buf = prev_end = NULL;
// 	max_blk = g_nvme_qpair_buf_max_blk_nr;
// 	start_blk = mb_map[0].block_num;

// 	last_blk = blk_cnt = 0;
// 	for (i = 0; i < mb_map_nr; i++) {
// 		cur_blk = mb_map[i].block_num;
// 		cur_buf = mb_map[i].bv_buf;
// 		cur_cnt = mb_map[i].n_blks;

// 		/* memory and disk layout both contiguous */
// 		if ((cur_blk == last_blk && cur_buf == prev_end) || i == 0) {
// 			blk_cnt += cur_cnt;
// 			last_blk = cur_blk + cur_cnt;
// 			prev_end = cur_buf + cur_cnt * OXBOW_BLOCK_SIZE;
// 			continue;
// 		}

// 		/* not contiguous, next bio struct */
// 		while (blk_cnt >= max_blk) {
// 			bvec.bv_len = max_blk * OXBOW_BLOCK_SIZE;
// 			if (iod_add_bvec_to_bio(bio, &bvec) < 0) {
// 				bio->bi_next = alloc_bio(start_blk);
// 				bio = bio->bi_next;
// 				iod_add_bvec_to_bio(bio, &bvec);
// 			}
// 			start_blk += max_blk;
// 			blk_cnt -= max_blk;
// 			bvec.bv_buf += max_blk * OXBOW_BLOCK_SIZE;
// 		}
// 		if (blk_cnt > 0) {
// 			bvec.bv_len = blk_cnt * OXBOW_BLOCK_SIZE;
// 			if (iod_add_bvec_to_bio(bio, &bvec) < 0) {
// 				bio->bi_next = alloc_bio(start_blk);
// 				bio = bio->bi_next;
// 				iod_add_bvec_to_bio(bio, &bvec);
// 			}
// 			bvec.bv_buf = cur_buf;
// 		}

// 		/* count next contiguous bio */
// 		start_blk = cur_blk;
// 		blk_cnt = cur_cnt;
// 		last_blk = cur_blk + cur_cnt;
// 		prev_end = cur_buf + cur_cnt * PAGE_SIZE;
// 	}

// 	while (blk_cnt >= max_blk) {
// 		bvec.bv_len = max_blk * OXBOW_BLOCK_SIZE;
// 		if (iod_add_bvec_to_bio(bio, &bvec) < 0) {
// 			bio->bi_next = alloc_bio(start_blk);
// 			bio = bio->bi_next;
// 			iod_add_bvec_to_bio(bio, &bvec);
// 		}
// 		start_blk += max_blk;
// 		blk_cnt -= max_blk;
// 		bvec.bv_buf += max_blk * OXBOW_BLOCK_SIZE;
// 	}
// 	if (blk_cnt > 0) {
// 		bvec.bv_len = blk_cnt * OXBOW_BLOCK_SIZE;
// 		if (iod_add_bvec_to_bio(bio, &bvec) < 0) {
// 			bio->bi_next = alloc_bio(start_blk);
// 			bio = bio->bi_next;
// 			iod_add_bvec_to_bio(bio, &bvec);
// 		}
// 	}

// 	return bl;
// }

/**
 * @brief This function constructs bio lists based on the mb_mapping and
 * performs I/O. It merges mb_mappings if possible. 'Merging' means:
 * 	1) If blocks are contiguous, those blocks can be described with one bio.
 * 	2) Within a bio, if pages are contiguous, those pages can be described with
 *	one bvec.
 * 
 * @param bl 
 * @param mb_mapping 
 * @param blk_cnt 
 */
// void submit_mb_maps(struct mb_map *mb_map, uint64_t mb_map_nr)
// {
// 	d_debug("[%s]", __func__);

// 	// bl = build_bio_list(mb_map, mb_map_nr);
// 	// if (!bl) {
// 	// 	oxb_error("iod_write_mb_maps_fail");
// 	// 	return;
// 	// }

// 	d_debug("[%s] done", __func__);
// }

// static int compare_mb_map(const void *a, const void *b)
// {
// 	struct mb_map arg1;
// 	struct mb_map arg2;

// 	arg1 = *(const struct mb_map *)a;
// 	arg2 = *(const struct mb_map *)b;

// 	if (arg1.block_num < arg2.block_num)
// 		return -1;
// 	if (arg1.block_num > arg2.block_num)
// 		return 1;
// 	return 0;
// }

// void iod_sort_mb_maps(struct mb_map *mb_mapping, uint64_t mb_nr)
// {
// 	qsort(mb_mapping, mb_nr, sizeof(struct mb_map), compare_mb_map);
// }

void bh_end_io(void *args)
{
	struct bio *bio;
	struct buffer_head *bh;

	bio = args;
	bh = bio->bi_private;

	// IO error should be checked here.
	clear_buffer_new(bh);
	set_buffer_uptodate(bh);
	unlock_buffer(bh);

	free(bio);
}

static struct bio *alloc_bio_for_bh(blk_opf_t opf, struct buffer_head *bh)
{
	struct bio *bio;

	bio = alloc_bio(1, opf);
	if (!bio) {
		oxb_error("bio alloc fail");
		return NULL;
	}

	bio->bi_start = bh->b_blocknr;
	bio->bi_io_vec->bv_buf = bh->b_data;
	bio->bi_vcnt = 1;
	bio->bi_vtotal = 1;
	bio->bi_private = bh;
	bio->end_io = bh_end_io;

	return bio;
}

/**  
 * @brief If IO delegated to worker thread (iod_workers)
 *        In our case it is related to read IO
 */

static void iod_submit_bh(blk_opf_t opf, struct bio *bio)
{
	if ((opf & REQ_OP_MASK) == REQ_OP_WRITE)
		thpool_add_work(iod_workers, nvme_wr_submit_bh, bio);

	else if ((opf & REQ_OP_MASK) == REQ_OP_READ)
		thpool_add_work(iod_workers, nvme_rd_submit_bh, bio);
	else
		oxb_error("Not supported yet %d", opf & REQ_OP_MASK);
}

/**  
 * @brief If IO handled by caller thread (caller is iod_workers)
 *        In our case it is related to staging (fsync)
 */

void submit_bh(blk_opf_t opf, struct buffer_head *bh)
{
	struct bio *bio;

	bh_debug("[%s/tid:%d] opf%d lba:%lu", __func__, tls_tid, opf,
		 bh->b_blocknr);

	bio = alloc_bio_for_bh(opf, bh);
	if (!bio) {
		oxb_error("bio alloc fail");
		return;
	}

	// If this context is iod_workers, handle IO directly
	if (tls_ioworker) {
		if ((opf & REQ_OP_MASK) == REQ_OP_WRITE)
			nvme_wr_submit_bh(bio);
		else if ((opf & REQ_OP_MASK) == REQ_OP_READ)
			nvme_rd_submit_bh(bio);
		else
			oxb_error("Not supported yet %d", opf & REQ_OP_MASK);
	} else
		iod_submit_bh(opf, bio);
}

// // This function should be used in the io worker thread.
// void submit_bio(blk_opf_t opf, struct bio *bio)
// {
// 	stg_debug("[%s] %lu", __func__, bio->bi_start);
// 	BUG_ON(!tls_ioworker, "Not iod_workers");

// 	if ((opf & REQ_OP_MASK) == REQ_OP_WRITE)
// 		nvme_wr_submit_bio(bio);
// 	else if ((opf & REQ_OP_MASK) == REQ_OP_READ)
// 		nvme_rd_submit_bio(bio);
// 	else
// 		oxb_error("Not supported yet %d", opf & REQ_OP_MASK);
// }

/**
 * @brief Enqueue bio to ring buffer.
 * 
 * @param bio 
 */
void iod_enqueue_bio(struct bio *bio)
{
	uint32_t cnt = 0;

	// oxb_warn("[Enqueue] tls_tid=%d bio=%p", tls_tid, bio);

	PF_TL_START(e002f_rd_enqueue);
	while (ring_buffer_mpmc_try_enqueue(&g_rd_bio_ring_buf, bio) != 0) {
		oxbow_cpu_relax();
		cnt++;

		if (cnt >= 100000 && cnt % 100000 == 0)
			oxb_warn("ring_buffer_mpmc_try_enqueue failed %d times",
				 cnt);
	}
#ifdef OXBOW_TRACK_TPUT
	/* Sample current ring occupancy. Tag=0 (no specific inode context). */
	check_rt_q(&g_q_rd_bio_ring,
		   (uint32_t)ring_buffer_mpmc_count(&g_rd_bio_ring_buf), 0);
#endif
	PF_TL_END(e002f_rd_enqueue);
}

void iod_submit_bio(blk_opf_t opf, struct bio *bio)
{
	if ((opf & REQ_OP_MASK) == REQ_OP_WRITE) {
		thpool_add_work(iod_workers, nvme_wr_submit_bio, bio);

	} else if ((opf & REQ_OP_MASK) == REQ_OP_READ) {
#ifdef OXBOW_RD_INLINE_SUBMIT
		/* Strict invariant: production read path must run inside a
		 * D-2 read_worker (process_inode_events -> mpage), where
		 * tls_inline_batch_active is 1. Other call sites
		 * (sync_device.c admin debug, etc.) must use
		 * iod_submit_bio_general(), which routes to the legacy
		 * ring + pump path explicitly.
		 */
		if (!tls_inline_batch_active) {
			panic("iod_submit_bio(REQ_OP_READ) called outside read_worker context. "
			      "Use iod_submit_bio_general() for non-read_worker callers.");
		}
		if (tls_inline_batch_n >= INLINE_BATCH_MAX) {
			/* Spillover: drain the current batch eagerly so we
			 * have room for the new BIO. The per-epoll-round
			 * batching in read_worker_loop will continue
			 * appending to the now-empty batch and flush again
			 * at the end of the round. */
			nvme_submit_bio_inline(tls_inline_batch,
					       tls_inline_batch_n);
			tls_inline_batch_n = 0;
		}
		tls_inline_batch[tls_inline_batch_n++] = bio;
#else
		iod_enqueue_bio(bio);
		/* Schedule a budgeted read pump job on demand. */
		nvme_schedule_read_pump();
#endif
	} else
		oxb_error("Not supported yet %d", opf & REQ_OP_MASK);
}

/**
 * @brief Submit bio to io worker thread. For synchronization, the caller should
 * set the end_io callback to the bio. For example, one can use semaphore to
 * make caller thread wait until the bio is done.
 *
 * Note: When OXBOW_RD_INLINE_SUBMIT is on, this routes read BIOs through
 * the legacy ring + pump path because callers (e.g., sync_device.c admin
 * signal handler) run outside a D-2 read_worker. Production reads should
 * NOT use this path.
 *
 * @param bio
 * @param is_read
 */
void iod_submit_bio_general(struct bio *bio, bool is_read)
{
	if (is_read) {
#ifdef OXBOW_RD_INLINE_SUBMIT
		/* Visibility for unintended fallback callers. The ring +
		 * pump path is intentionally kept for sync_device.c admin
		 * debug (SIGRTMIN+2 dump/load) but should never be hit by
		 * production hot paths.
		 *
		 * Rate limit to avoid log floods when sync_device.c emits
		 * many BIOs in one dump/load round (one per BIO call).
		 *
		 * Atomics are not necessary: a few races just produce
		 * one extra warn line on first call from each thread, which
		 * is the desired behavior anyway. */
		static atomic_ullong fb_count = ATOMIC_VAR_INIT(0);
		static atomic_long fb_last_log_sec = ATOMIC_VAR_INIT(0);
		unsigned long long n;
		long now_sec, last_sec;

		n = atomic_fetch_add_explicit(&fb_count, 1,
					      memory_order_relaxed) + 1;
		now_sec = (long)time(NULL);
		last_sec = atomic_load_explicit(&fb_last_log_sec,
						memory_order_relaxed);

		if (n == 1 || now_sec - last_sec >= 1) {
			atomic_store_explicit(&fb_last_log_sec, now_sec,
					      memory_order_relaxed);
			oxb_warn(
				"iod_submit_bio_general(READ) using ring+pump fallback (count=%llu). "
				"Production reads should go through read_worker (D-2 inline). "
				"Expected callers: sync_device.c admin debug (SIGRTMIN+2) only.",
				n);
		}

		/* sync_device.c et al.: not in read_worker context, so use
		 * the ring + pump fallback path which runs on iod_workers
		 * (tid 0..se-1). */
		iod_enqueue_bio(bio);
		nvme_schedule_read_pump();
#else
		iod_submit_bio(REQ_OP_READ, bio);
#endif
	} else {
		iod_submit_bio(REQ_OP_WRITE, bio);
	}
}
