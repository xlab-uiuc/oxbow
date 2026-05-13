#include <pthread.h>
#include <sys/mman.h>
#include <stdint.h>
#include "fs/fs.h"
#include "buffer_head.h"
#include "io/sd_bio.h"
#include "io_dispatcher.h"
#include "kernfs.h"
#include "oxbow_debug.h"
#include "io/nvme.h"
#include "journal.h"
#include "profile_secure_daemon.h"
#include "thpool.h"
#include "msg.h"
#include "utils/jlock_profile.h"

// #define ENABLE_CKPT // Uncomment to disable sync checkpointing.
// #define ENABLE_ASYNC_CKPT // Uncomment to disable async checkpointing.
#ifdef ENABLE_ASYNC_CKPT
#define STG_CKPT_THRESHOLD_ASYNC 40 // Start async checkpointing if free space is less than 40%.
#endif

#ifdef DEBUG_PRINT_STG_TX
#include "utils/print_tx.h"
#endif

#include <sys/queue.h>

// Write desc block and commit block asynchronously.
#define ASYNC_DESC_COMMIT_WRITE

PF_TL_EVT(z08_stg_n_inodes);
PF_TL_EVT(z09_stg_idx_tags);
PF_TL_EVT(z10_stg_idx_nblks);
PF_TL_EVT(z11_stg_data_tags);
PF_TL_EVT(z12_stg_data_nblks);
PF_TL_EVT(z13_total_etag_nblks);

PF_EVT(SYNC_memcpy_data);
PF_EVT(SYNC_io_data);

static int sync_dirent(struct inode *inode)
{
	struct buffer_head *bh;

	stg_debug("[%s] inode(%lu)", __func__, inode->i_ino);

	pthread_spin_lock(&inode->i_mapping->private_lock);
	list_for_each_entry (bh, &inode->i_mapping->private_list, elem)
		if (buffer_dirty(bh))
			sync_dirty_buffer(bh);
	pthread_spin_unlock(&inode->i_mapping->private_lock);

	return 0;
}

static void unlock_pages_batch(struct inode *inode)
{
	struct inode_range_dirty *ird;
	bit_index_t start, cnt;
	uint32_t i;

	ird = inode->i_stg_dirty_start;

next_ird:
	for (i = 0; i < ird->count; i++) {
		start = ird->dirty_pages[i].start;
		cnt = ird->dirty_pages[i].nr;

		if (cnt)
			for (i = 0; i < cnt; i++)
				inode_unlock_pagebit(inode, start + i);
	}

	if (ird->next && ird->next->count) {
		ird = ird->next;
		goto next_ird;
	}
}

void sync_end_io(void *args)
{
	struct bio *bio = args;
	free(bio->bi_io_vec);
	free(bio);
}

// Synchronous I/O for now. It can be optimized if we pipeline IO submission and
// completion just like read IOs.
static void sync_bio_submit(struct bio *bio)
{
	bio->bi_vcnt = bio->bi_vtotal;
	bio->end_io = sync_end_io;

	nvme_wr_submit_bio(bio);
}

static int sync_file_data(struct inode *inode, struct buffer_extent *be)
{
	struct bio *bio = NULL;
	struct inode_range_dirty *ird;
	char *i_data = inode->data;
	bit_index_t start, cnt, tmp;
	get_blocks_t get_blocks;
	uint32_t i;
	int ret;

	stg_debug("[%s]", __func__);

	inode_gather_dirty_pagebits(inode, 0, -1);

	ird = inode->i_stg_dirty_start;

	// Decrement the dirty page count here. But no journaling doesn't need
	// dirty page counting.

	BUG_ON(inode->i_op->get_blocks == NULL, "no get_blocks");
	get_blocks = inode->i_op->get_blocks;

next_ird:
	for (i = 0; i < ird->count; i++) {
		start = ird->dirty_pages[i].start;
		cnt = ird->dirty_pages[i].nr;

		if (!cnt)
			continue;

		inode_idx_lock_shared(inode);
		ret = get_blocks(inode, be, start, 0);
		inode_idx_unlock(inode);
		if (ret) {
			oxb_error("ino(%lu) getblk fail", inode->i_ino);
			return ret;
		}

	retry:
		if (start + cnt <= be->iblock + be->nr) {
			while (cnt) {
				if (!bio) {
					bio = alloc_bio(BIO_MAX_VECS,
							REQ_OP_WRITE);
					if (!bio) {
						oxb_error("alloc_bio fail");
						oxbow_assert(0);
						return -1;
					}
					bio->bi_start =
						be->lba + start - be->iblock;
				}
				add_bvec(bio, i_data + PAGE_SIZE * start);
				if (bio_full(bio)) {
					sync_bio_submit(bio);
					bio = NULL;
				}
				cnt--;
				tmp--;
				start++;
			}
		} else if (start < be->iblock + be->nr) {
			tmp = be->iblock + be->nr - start;

			// as long as this is contiguous
			while (cnt && tmp) {
				if (!bio) {
					bio = alloc_bio(BIO_MAX_VECS,
							REQ_OP_WRITE);
					if (!bio) {
						oxb_error("alloc_bio fail");
						oxbow_assert(0);
						return -1;
					}
					bio->bi_start =
						be->lba + start - be->iblock;
				}
				add_bvec(bio, i_data + PAGE_SIZE * start);
				if (bio_full(bio)) {
					sync_bio_submit(bio);
					bio = NULL;
				}
				cnt--;
				tmp--;
				start++;
			}
		}
		if (cnt != 0) {
			BUG_ON((long)cnt < 0, "overflow");
			inode_idx_lock_shared(inode);
			ret = get_blocks(inode, be, start, 0);
			inode_idx_unlock(inode);
			if (ret) {
				oxb_error("ino(%lu) getblk fail", inode->i_ino);
				return ret;
			}
			goto retry;
		}

		ird->dirty_pages[i].start = 0;
		ird->dirty_pages[i].nr = 0;
	}

	if (ird->next && ird->next->count) {
		ird = ird->next;
		goto next_ird;
	}

	if (bio && bio->bi_vtotal)
		sync_bio_submit(bio);

	unlock_pages_batch(inode);

	free_ird_except_first(inode->i_stg_dirty_start);

	return 0;
}

static int sync_inode_metadata(struct inode *inode)
{
	struct buffer_head *bh;
	int ret;

	ret = -1;

	illufs_get_auth(inode);
	stg_debug("[%s] inode(%lu) auth from kernel mode %d, uid %d, gid %d",
		  __func__, inode->i_ino, inode->i_mode, inode->i_uid,
		  inode->i_gid);

	if (inode->i_state & I_DIRTY) {
		stg_debug("[%s] inode(%lu) flush", __func__, inode->i_ino);
		if (inode->i_sb->s_op->write_inode(inode) < 0) {
			log_error("inode write fail");
			return ret;
		}
	}

	/* File index(extent) block sync, only apply for sefs */
	pthread_spin_lock(&inode->i_mapping->private_lock);
	list_for_each_entry (bh, &inode->i_mapping->private_list, elem) {
		if (buffer_dirty(bh))
			sync_dirty_buffer(bh);
	}
	pthread_spin_unlock(&inode->i_mapping->private_lock);
	ret = 0;

	return ret;
}

// No journal path.
static int sync_file(struct inode *inode)
{
	struct buffer_extent be = { 0 };
	loff_t old;
	int ret = -1;

	stg_debug("[%s] %s", __func__,
		  S_ISREG(inode->i_mode) ? "Regular file" : "Directory");

	if (S_ISREG(inode->i_mode)) {
		/* [TODO] decrease critical path of i_data_sem */
		shared_inode_lock(inode);

		if (inode->i_size != get_recent_isize(inode)) {
			old = inode->i_size;
			inode->i_size = get_recent_isize(inode);
			stg_debug("[%s] inode(%lu) size changed: %ld --> %ld",
				  __func__, inode->i_ino, old, inode->i_size);
			illufs_set_i_size(inode);
			inode->i_state |= I_DIRTY;

			/* alloc block if we needed */
			if (inode_alloc_blocks(inode, &be, old))
				return -1;
		}

		ret = sync_file_data(inode, &be);
		if (ret < 0)
			oxb_error("failed to sync file data");

		// if (inode->i_state & I_DIRTY)
		ret |= sync_inode_metadata(inode);
		if (ret < 0)
			oxb_error("failed to sync metadata");

		shared_inode_unlock(inode);
	} else if (S_ISDIR(inode->i_mode)) {
		inode_lock(inode);

		ret = sync_dirent(inode);
		// if (inode->i_state & I_DIRTY)
		ret |= sync_inode_metadata(inode);

		inode_unlock(inode);
	} else
		oxb_error("not expected mode");

	return ret;
}

static int sync_all_inodes(struct super_block *sb)
{
	struct inode *inode;

	list_for_each_entry (inode, &sb->s_inodes, i_sb_list) {
		if (sync_file(inode) < 0)
			return -1;
	}
	return 0;
}

static int sync_all_fs_metadata(struct super_block *sb)
{
	struct buffer_head *bh;

	/* how to divide dirty buffer and not? */
	pthread_spin_lock(&sb->s_mapping.private_lock);
	list_for_each_entry (bh, &sb->s_mapping.private_list, elem) {
		if (buffer_dirty(bh))
			sync_dirty_buffer(bh);
	}
	pthread_spin_unlock(&sb->s_mapping.private_lock);

	bh = sb->s_bh;
	mark_buffer_dirty(sb->s_bh);
	sync_dirty_buffer(sb->s_bh);
	return 0;
}

static int sync_fs_nojournal(struct super_block *sb)
{
	/* Nojournal mode not contain dirty data after close */
	// log_info("not supporting sync_fs for now");
	// return 0;

	if (sync_all_inodes(sb) < 0) {
		oxb_error("inode fail");
		return -1;
	}

	return sync_all_fs_metadata(sb);
}

int sync_fs(void)
{
	struct super_block *sb = g_super_block;

	if (!sb->journal)
		return sync_fs_nojournal(sb);
	else
		return commit_journal(sb);
}

enum stx_req_type {
	STX_REQ_RECLAIM = 0,
	STX_REQ_TAIL_DATA,
	STX_REQ_DESC,
	STX_REQ_COMMIT,
};

static inline void stx_track_req_issue(stage_tx *stx, uint32_t req_nr,
				       enum stx_req_type type)
{
	if (!req_nr)
		return;

	stx->req_issued_total += req_nr;

	switch (type) {
	case STX_REQ_RECLAIM:
		stx->req_issued_reclaim += req_nr;
		break;
	case STX_REQ_TAIL_DATA:
		stx->req_issued_tail_data += req_nr;
		break;
	case STX_REQ_DESC:
		stx->req_issued_desc += req_nr;
		break;
	case STX_REQ_COMMIT:
		stx->req_issued_commit += req_nr;
		break;
	default:
		oxbow_assert(0);
	}
}

static inline void stx_track_req_complete(stage_tx *stx, uint32_t req_nr)
{
	if (!req_nr)
		return;

	stx->req_completed_total += req_nr;
	if (stx->req_completed_total > stx->req_issued_total) {
		oxb_error("stage req completion overflow: completed=%llu issued=%llu",
			  (unsigned long long)stx->req_completed_total,
			  (unsigned long long)stx->req_issued_total);
		oxbow_assert(0);
	}
}

static inline void stx_mark_slots_issued_prefix(stage_tx *stx,
						uint32_t issued_slots)
{
	uint32_t i;

	if (!issued_slots)
		return;

	if (issued_slots > stx->slot_capacity)
		issued_slots = stx->slot_capacity;

	for (i = 0; i < issued_slots; i++) {
		if (stx->slot_busy[i]) {
			oxb_error("slot %u is still busy before re-issue", i);
			oxbow_assert(0);
		}
		stx->slot_busy[i] = 1u;
		stx->slot_done[i] = 0u;
	}

	stx->busy_slots += issued_slots;
	oxbow_assert(stx->busy_slots <= stx->slot_capacity);
	stx->slot_head = issued_slots % stx->slot_capacity;
}

static inline void stx_reclaim_done_slots_inorder(stage_tx *stx);

#ifndef ASYNC_DESC_COMMIT_WRITE
static inline void stx_mark_slots_completed_prefix(stage_tx *stx,
						   uint32_t completed_slots)
{
	uint32_t i;

	if (!completed_slots)
		return;

	if (completed_slots > stx->slot_capacity)
		completed_slots = stx->slot_capacity;

	for (i = 0; i < completed_slots; i++) {
		if (!stx->slot_busy[i]) {
			oxb_error("slot %u completion forced while not busy", i);
			oxbow_assert(0);
		}
		stx->slot_done[i] = 1u;
	}

	stx_reclaim_done_slots_inorder(stx);
}
#endif

static inline void stx_reclaim_done_slots_inorder(stage_tx *stx)
{
	while (stx->busy_slots && stx->slot_busy[stx->slot_tail] &&
	       stx->slot_done[stx->slot_tail]) {
		stx->slot_busy[stx->slot_tail] = 0u;
		stx->slot_done[stx->slot_tail] = 0u;
		stx->slot_tail++;
		if (stx->slot_tail == stx->slot_capacity)
			stx->slot_tail = 0;
		stx->busy_slots--;
	}
}

static void stx_nvme_slot_complete(void *ctx, uint32_t slot_idx, int is_error)
{
	stage_tx *stx = ctx;

	if (!stx)
		return;

	if (slot_idx >= stx->slot_capacity) {
		oxb_error("invalid slot completion index: %u (cap=%u)", slot_idx,
			  stx->slot_capacity);
		stx->io_error = true;
		return;
	}

	if (!stx->slot_busy[slot_idx]) {
		oxb_error("slot %u completion arrived while not busy", slot_idx);
		stx->io_error = true;
		return;
	}

	stx->slot_done[slot_idx] = 1u;
	if (is_error)
		stx->io_error = true;
}

static inline uint32_t stx_poll_completions(stage_tx *stx,
					    uint32_t max_completions)
{
	uint32_t done;
	uint64_t outstanding;

	outstanding = stx->req_issued_total - stx->req_completed_total;
	if (!outstanding)
		return 0;

	if (!max_completions || max_completions > outstanding) {
		if (outstanding > UINT32_MAX)
			max_completions = UINT32_MAX;
		else
			max_completions = (uint32_t)outstanding;
	}

	done = nvme_poll_completions(max_completions);
	if (!done)
		return 0;

	if ((uint64_t)done > outstanding) {
		oxb_error("poll completion overflow: done=%u outstanding=%llu",
			  done, (unsigned long long)outstanding);
		oxbow_assert(0);
	}

	stx_track_req_complete(stx, done);
	stx_reclaim_done_slots_inorder(stx);
	return done;
}

static void stx_drain_until_slot_free(stage_tx *stx, uint32_t slot_idx)
{
	uint32_t spin_cnt = 0;

	oxbow_assert(slot_idx < stx->slot_capacity);

	while (stx->slot_busy[slot_idx]) {
		uint32_t done = stx_poll_completions(stx, 0);
		if (!done) {
			oxbow_cpu_relax();
			spin_cnt++;
			if (spin_cnt % 1000000 == 0)
				oxb_warn("waiting slot %u to be free (busy=%u issued=%llu completed=%llu)",
					 slot_idx, stx->busy_slots,
					 (unsigned long long)stx->req_issued_total,
					 (unsigned long long)stx->req_completed_total);
		} else {
			spin_cnt = 0;
		}

		if (stx->io_error) {
			oxb_error("I/O error while waiting slot %u", slot_idx);
			oxbow_assert(0);
		}
	}

	stx->slot_head = (slot_idx + 1) % stx->slot_capacity;
}

static void stx_drain_all_outstanding(stage_tx *stx)
{
	uint32_t spin_cnt = 0;

	while (stx->req_completed_total < stx->req_issued_total) {
		uint32_t done = stx_poll_completions(stx, 0);
		if (!done) {
			oxbow_cpu_relax();
			spin_cnt++;
			if (spin_cnt % 1000000 == 0)
				oxb_warn("waiting all outstanding I/O (issued=%llu completed=%llu busy_slots=%u)",
					 (unsigned long long)stx->req_issued_total,
					 (unsigned long long)stx->req_completed_total,
					 stx->busy_slots);
		} else {
			spin_cnt = 0;
		}

		if (stx->io_error) {
			oxb_error("I/O error while draining outstanding I/O");
			oxbow_assert(0);
		}
	}
}

/**
 * @brief allocate new stage transaction context object
 * 
 * @return stage_tx* 
 */
static stage_tx *new_stage_tx(struct inode *inode)
{
	stage_tx *new;

	new = calloc(1, sizeof *new);
	if (!new) {
		oxb_error("malloc fail");
		return NULL;
	}

	INIT_LIST_HEAD(&new->elem);

	// Assuming synchronous IO, the first buffer is always used.
	new->cur = nvme_get_seq_buffer(0);
	oxbow_assert(new->cur);
	// if (!new->cur) {
	// 	oxb_error("alloc fail");
	// 	free(new);
	// 	return NULL;
	// }

	new->desc_blk_buf = nvme_get_desc_blk_buffer();
	oxbow_assert(new->desc_blk_buf);
	// if (!new->desc_blk_buf) {
	// 	oxb_error("desc_blk_buf alloc fail");
	// 	free(new);
	// 	return NULL;
	// }

	new->commit_blk_buf = nvme_get_commit_blk_buffer();
	oxbow_assert(new->commit_blk_buf);
	// if (!new->commit_blk_buf) {
	// 	oxb_error("commit_blk_buf alloc fail");
	// 	free(new);
	// 	return NULL;
	// }

	new->seq_max = nvme_get_seq_max();
	if (new->seq_max <= 0) {
		oxb_error("invalid seq_max: %d", new->seq_max);
		free(new);
		return NULL;
	}
	new->nr_blocks = g_max_nvme_max_io_size / OXBOW_BLOCK_SIZE;
	new->j = inode->i_sb->journal;
	new->slot_capacity = (uint32_t)new->seq_max;
	new->slot_head = 0;
	new->slot_tail = 0;
	new->busy_slots = 0;
	new->slot_busy = calloc(new->slot_capacity, sizeof(*new->slot_busy));
	new->slot_done = calloc(new->slot_capacity, sizeof(*new->slot_done));
	if (!new->slot_busy || !new->slot_done) {
		oxb_error("failed to allocate stage slot tracker");
		free(new->slot_busy);
		free(new->slot_done);
		free(new);
		return NULL;
	}
	new->io_error = false;

	atomic_init(&new->is_completed, false);

	// We can skip it.
	// memset(&new->desc_blk_copy, 0, sizeof(struct stage_descriptor_block));

	stg_debug("new stage tx CREATED. inode=%lu tx=%p tx->j=%p cur=%p",
		  inode->i_ino, new, new->j, new->cur);
	return new;
}

// NOTE: The previous block should be filled before calling this function as
// data can be flushed when there is no more space in the sequence buffer.
/**
 * @brief Reserve a block in sequence buffer.
 * 
 * @param stx 
 * @return void* 
 */
static void *resv_blk(stage_tx *stx)
{
	void *ret;
	bool skip_first_blk;
	uint32_t issued_reqs;

	if (stx->nr_blocks == 0) {
		/* Each sequence contains max IO size for NVMe */
		/* In our model, it will be 32KB */
		sync_debug("no left blocks in seq_idx(%d)", stx->seq_idx);

		if (stx->seq_idx == stx->seq_max - 1) {
			/* If fsynced file is too big so that no more sequence buffers */
			/* Fix the LBA of this stage_Tx, and do I/O to reclaim buffer */
			if (stx->desc_blk_skipped)
				skip_first_blk = false;
			else { // first write.
				skip_first_blk = true;
				stx->desc_blk_skipped = true;
			}

			sync_debug("Do I/O to reclaim buffers tls_tid=%d desc_blk_skipped=%d",
				   tls_tid, stx->desc_blk_skipped);

			// NOTE: Following assertions ensure the staging area
			// has been reserved (in stage_fix_blocks() function).
			// There are unlikely cases (e.g., a large number of
			// etag blocks or index blocks) that could violate these
			// assertions. For now, we simply assume those cases do
			// not occur.
			oxbow_assert(stx->start); // stx->start should already be set.
			oxbow_assert(stx->stage_area_reserved);

			sync_debug("Async write %d blocks.", stx->nr_pending);

			issued_reqs = nvme_direct_write_async_track(
				stx->seq_max, stx->start + stx->nr_issued,
				stx->nr_pending, skip_first_blk,
				stx_nvme_slot_complete, stx);
			if (!issued_reqs)
				stx->io_error = true;
			oxbow_assert(issued_reqs > 0);
			stx_mark_slots_issued_prefix(stx, issued_reqs);
			stx_track_req_issue(stx, issued_reqs, STX_REQ_RECLAIM);
			PF_TRACK_TPUT(SYNC_io_data, stx->nr_pending);

			stx->seq_idx = 0;
			stx->nr_issued += stx->nr_pending;
			stx->nr_pending = 0;
		} else
			stx->seq_idx++;

		stx_drain_until_slot_free(stx, (uint32_t)stx->seq_idx);

		/* Get the next buffer's address */
		stx->cur = nvme_get_seq_buffer(stx->seq_idx);
		stx->nr_blocks = g_max_nvme_max_io_size / OXBOW_BLOCK_SIZE;
	}

	ret = stx->cur;

	oxbow_assert(ret);

	stx->cur += PAGE_SIZE;
	stx->nr_pending++;
	stx->nr_blocks--;

	sync_debug(
		"resv_blk: seq_idx=%d stx->cur=%p stx->nr_pending=%d stx->nr_blocks=%d",
		stx->seq_idx, ret, stx->nr_pending, stx->nr_blocks);


	return ret;
}

/**
 * @brief Copy metadata to spdk buffer.
 * 
 * @param stx 
 * @param src 
 */
static void copy_blk(stage_tx *stx, const void *src)
{
	void *dst = resv_blk(stx);
	memcpy(dst, src, PAGE_SIZE);
}

/**
 * @brief Copy data from page cache to spdk buffer.
 * 
 * @param inode 
 * @param stx 
 * @param iblock_start 
 * @param cnt 
 */
static void copy_data_blks(struct inode *inode, stage_tx *stx, pgoff_t iblock_start,
			   u32 cnt)
{
	void *dst, *start, *src;
	unsigned int i;

	start = (char *)inode->data + iblock_start * PAGE_SIZE;

	for (i = 0; i < cnt; i++) {
		dst = resv_blk(stx);
		src = (char *)start + i * PAGE_SIZE;

		stg_debug("[%s] inode(%lu) data=0x%lx dst=0x%lx src=0x%lx",
			  __func__, inode->i_ino, inode->data, dst,
			  src);

		// if (iblock_start + i >= 32765 && iblock_start + i <= 32767) {
		// 	stg_debug("[%s] handling iblock=%lu", __func__,
		// 		  iblock_start + i);
		// 	stg_debug("src:");
		// 	hex_dump(src, 64);
		// }

		memcpy(dst, src, PAGE_SIZE);

		PF_TRACK_TPUT(SYNC_memcpy_data, 1);

		// sync_debug("[%s] inode(%lu) done", __func__, inode->i_ino);
		inode_unlock_pagebit(inode, iblock_start + i);
	}
	stg_debug("[%s] inode(%lu) start: %ld, cnt: %d done", __func__,
		  inode->i_ino, iblock_start, cnt);
}

static int write_etags(struct journal_extent_tag_block *je, u64 start, u32 cnt)
{
	stg_debug("[%s] nr_tags: %u, max: %d", __func__, je->h.nr,
		  JOURNAL_ETAG_MAX);

	// if (je->h.nr > JOURNAL_ETAG_MAX) {
	// 	oxb_error("[%s] nr_tags: %u exceeds max: %d", __func__, je->h.nr,
	// 		  JOURNAL_ETAG_MAX);
	// }
	oxbow_assert(je->h.nr <= JOURNAL_ETAG_MAX);

	je->tags[je->h.nr].start = start;
	je->tags[je->h.nr].cnt = cnt;
	je->h.nr++;

	stg_debug("[%s] fill tag: start(%lu) cnt(%u) to stage's etag blk",
		  __func__, start, cnt);

	if (je->h.nr == JOURNAL_ETAG_MAX)
		return 1;

	return 0;
}

static int stx_write_desc_tag(struct stage_descriptor_block *sdb, u64 start,
			      u32 cnt)
{
	stg_debug("[%s] nr_tags: %u, max: %d", __func__, sdb->h.nr_tags,
		  STAGE_DESC_BLK_TAG_MAX);
	BUG_ON(sdb->h.nr_tags >= STAGE_DESC_BLK_TAG_MAX, "idx bug");

	sdb->tags[sdb->h.nr_tags].start = start;
	sdb->tags[sdb->h.nr_tags].cnt = cnt;
	sdb->h.nr_tags++;

	stg_debug("[%s] fill tag: start(%lu) cnt(%u) to stage's desc blk",
		  __func__, start, cnt);

	if (sdb->h.nr_tags == STAGE_DESC_BLK_TAG_MAX)
		return 1;

	return 0;
}

static void print_tag_block(struct journal_extent_tag_block *je)
{
	unsigned int i;

	for (i = 0; i < je->h.nr; i++)
		stg_info("tag[%d] start: %ld, cnt: %d", i, je->tags[i].start,
			  je->tags[i].cnt);
}

static void print_desctag_block(struct stage_descriptor_block *de)
{
	unsigned int i;

	for (i = 0; i < de->h.nr_tags; i++)
		stg_info("tag[%d] start: %ld, cnt: %d", i, de->tags[i].start,
			  de->tags[i].cnt);
}

/* Simple CRC32C shim; stub for now. */
// static uint32_t crc32c_buf(uint32_t seed, const void *buf, size_t len)
// {
// 	(void)buf; (void)len; (void)seed;
// 	return 0;
// }

/* Serialize IRD into stage buffers, one PAGE per resv_blk().
 * Returns number of frames (blocks) written. */
static uint32_t serialize_and_fill_ird_list(stage_tx *stx,
						    const struct inode_range_dirty *ird)
{
	const size_t hdr_sz = sizeof(struct ird_frame_header);
	const size_t ent_sz = sizeof(struct ird_frame_entry);
	const uint32_t ents_per_block = (uint32_t)((PAGE_SIZE - hdr_sz) / ent_sz);
	uint32_t written_blocks = 0;
	const struct inode_range_dirty *cur = ird;
	uint64_t prev_end = 0;
	bool first_in_frame = true;
	uint32_t ent_idx = 0;
	struct ird_frame_header *hdr = NULL;
	struct ird_frame_entry *ents = NULL;

	oxbow_assert(ents_per_block > 0);

	while (cur) {
		for (uint32_t i = 0; i < cur->count; i++) {
			uint64_t start = (uint64_t)cur->dirty_pages[i].start;
			uint64_t len = (uint64_t)cur->dirty_pages[i].nr;
			if (len == 0)
				continue;

			/* Start new frame if needed */
			if (first_in_frame) {
				void *blk = resv_blk(stx);
				oxbow_assert(blk);

				hdr = (struct ird_frame_header *)blk;
				hdr->base_pg = start;
				hdr->n_entries = 0;
				// hdr->flags = 0;
				// hdr->crc32 = 0;
				ents = (struct ird_frame_entry *)((char *)blk + hdr_sz);
				ent_idx = 0;
				prev_end = start; /* for first delta calc */
				first_in_frame = false;
				written_blocks++;
			}

			/* If frame is full, finalize and start a new one */
			if (ent_idx == ents_per_block) {
				hdr->n_entries = (uint16_t)ent_idx;
				// size_t ents_bytes = ent_sz * ent_idx;
				// hdr->crc32 = crc32c_buf(0, ents, ents_bytes);
				first_in_frame = true;
				--i; /* re-process current range in new frame */
				continue;
			}

			uint64_t delta = (start >= prev_end) ? (start - prev_end) : 0;
			ents[ent_idx].delta = (uint32_t)delta;
			ents[ent_idx].len = (uint32_t)len;
			ent_idx++;
			hdr->n_entries = (uint16_t)ent_idx;
			prev_end = start + len;
		}
		cur = cur->next;
	}

	/* Finalize last frame if open */
	// if (!first_in_frame) {
		// size_t ents_bytes = ent_sz * ent_idx;
		// hdr->crc32 = crc32c_buf(0, ents, ents_bytes);
	// }

	return written_blocks;
}

/**
 * @brief fill tags in stage transaction
 */
void stx_wrtag(stage_tx *stx, u64 start, u32 cnt)
{
	if (stx->ext_blk) {
		if (write_etags(stx->ext_blk, start, cnt)) {
			// TOCHECK: there can be an empty ext blk at the end. Is
			// it correct?

			stg_debug("tag block full");
			// print_tag_block(stx->ext_blk);
			stx->ext_blk = resv_blk(stx);
			stx->ext_blk->h.nr = 0;
			stx->desc_blk_copy.h.nr_etag_blks++;
			PF_TL_CNT(z13_total_etag_nblks, 1);
		}
	} else {
		if (stx_write_desc_tag(&stx->desc_blk_copy, start, cnt)) {
			// TOCHECK: there can be an empty ext blk at the end. Is
			// it correct?

			stg_debug("desc block full");
			// print_desctag_block(&stx->desc_blk_copy);
			stx->ext_blk = resv_blk(stx);
			stx->ext_blk->h.nr = 0;
			stx->desc_blk_copy.h.nr_etag_blks++;
			PF_TL_CNT(z13_total_etag_nblks, 1);
		}
	}
}

/**
 * @brief Inode is stored on stage area not with other inodes in same block.
 *        This is to avoid false-sharing problem and gain scalability on fsync.
 * 
 * @return int (success on 0)
 */
static int stage_inode(stage_tx *stx, struct inode *inode,
		       struct buffer_extent *be)
{
	loff_t old;

	// stg_debug("[BEFORE] inode=0x%lx inode->i_size: %ld, get_recent_isize(inode): %ld\n",
	// 	(unsigned long)inode, inode->i_size, get_recent_isize(inode));

	if (S_ISDIR(inode->i_mode))
		goto skip_balloc;

	if (inode->i_size != get_recent_isize(inode)) {
		old = inode->i_size;
		inode->i_size = get_recent_isize(inode);
		illufs_set_i_size(inode);
		inode->i_state |= I_DIRTY;

		PF_TL_START(af_____evt_inode_alloc);

		/* alloc block if we needed */
		if (inode_alloc_blocks(inode, be, old))
			return -1;

		PF_TL_END(af_____evt_inode_alloc);
	}

skip_balloc:
	/* stage inode itself and other related blocks */
	if (inode->i_sb->s_op->stage_inode(inode, &stx->desc_blk_copy.h))
		return -1;

	// stg_debug("[%s] tag:%d", __func__, stx->desc_blk_copy.h.nr_tags);
	// stg_debug("[AFTER] inode=0x%lx inode->i_size: %ld, get_recent_isize(inode): %ld\n",
	//        (unsigned long)inode, inode->i_size, get_recent_isize(inode));

	stg_debug("[%s] staging done: inode(%lu) i_size=%ld", __func__,
		  inode->i_ino, inode->i_size);

	return 0;
}

/**
 * @brief Add tags. Block allocation is done in this function.
 * 
 * @param stx 
 * @param inode 
 * @param be 
 * @param ird 
 */
static void stage_fill_tags(stage_tx *stx, struct inode *inode,
			    struct buffer_extent *be,
			    struct inode_range_dirty *ird)
{
	bit_index_t start, cnt, tmp;
	uint32_t i;
	int ret;

next_ird:
	for (i = 0; i < ird->count; i++) {
		start = ird->dirty_pages[i].start;
		cnt = ird->dirty_pages[i].nr;

		// stg_debug("[Tid%d] dirty pages: %ld~%ld", tls_tid, start,
		// 	  start + cnt - 1);

		if (!cnt)
			continue;

		inode_idx_lock_shared(inode);
		ret = inode->i_op->get_blocks(inode, be, start, 0);
		inode_idx_unlock(inode);
		if (ret) {
			oxb_error("ino(%lu) getblk fail", inode->i_ino);
			return;
		}

		stg_debug(
			"[%s] inode(%lu) start: %ld, cnt: %d be_i(%lu) be_nr(%lu) be_start(%lu)",
			__func__, inode->i_ino, start, cnt, be->iblock, be->nr,
			be->lba);

		/* block indexing may be not contiguous */
	retry:
		if (start + cnt <= be->iblock + be->nr) {
			stg_debug(
				"[%s]wrtag: start(%lu) baddr=%lu cnt=%u be->iblock=%lu",
				__func__, start, be->lba + start - be->iblock,
				cnt, be->iblock);
			stx_wrtag(stx, be->lba + start - be->iblock, cnt);

			PF_TL_CNT(z11_stg_data_tags, 1);
			PF_TL_CNT(z12_stg_data_nblks, cnt);

			cnt = 0;

		} else if (start < be->iblock + be->nr) {
			tmp = be->iblock + be->nr - start;
			stg_debug(
				"[%s]wrtag: start(%lu) baddr=%lu tmp=%u be->iblock=%lu",
				__func__, start, be->lba + start - be->iblock,
				tmp, be->iblock);
			stx_wrtag(stx, be->lba + start - be->iblock, tmp);

			PF_TL_CNT(z11_stg_data_tags, 1);
			PF_TL_CNT(z12_stg_data_nblks, tmp);

			cnt -= tmp;
			start += tmp;
		}
		if (cnt != 0) {
			BUG_ON((long)cnt < 0, "overflow");

			inode_idx_lock_shared(inode);
			ret = inode->i_op->get_blocks(inode, be, start, 0);
			inode_idx_unlock(inode);
			if (ret) {
				oxb_error("ino(%lu) getblk fail", inode->i_ino);
				return;
			}

			goto retry;
		}
	}

	if (ird->next && ird->next->count) {
		ird = ird->next;
		goto next_ird;
	}

	stg_debug("[tid%d,%s] inode(%lu) filling tags done", tls_tid, __func__,
		  inode->i_ino);
}

static void stage_fill_index_blk(stage_tx *stx, struct inode *inode)
{
	struct buffer_head *bh, *bh_tmp;
	pthread_spinlock_t *lock;
	struct address_space *mapping;
	struct list_head tmp;

	mapping = inode->i_mapping;
	lock = &mapping->private_lock;

	INIT_LIST_HEAD(&tmp);

	/* dirty blocks, mostly indexing block of file */
	pthread_spin_lock(lock);
	list_for_each_entry_safe (bh, bh_tmp, &mapping->private_list, elem) {
		list_del_init(&bh->elem);
		mapping->nr_entries--;
		bh->b_assoc_map = NULL;
		if (buffer_dirty(bh)) {
			list_add_tail(&bh->elem, &tmp);
			bh->b_assoc_map = mapping;

			pthread_spin_unlock(lock);

			lock_buffer(bh);
			copy_blk(stx, bh->b_data);
			stx_wrtag(stx, bh->b_blocknr, 1);

			PF_TL_CNT(z09_stg_idx_tags, 1);
			PF_TL_CNT(z10_stg_idx_nblks, 1);

			clear_buffer_dirty(bh);
			unlock_buffer(bh);

			stg_debug("bh(%lu) added", bh->b_blocknr);

			pthread_spin_lock(lock);
		} else // if block is in private list, it must be dirty
			oxb_warn("bh(%lu) not dirty", bh->b_blocknr);
	}

	while (!list_empty(&tmp)) {
		bh = list_entry(tmp.prev, struct buffer_head, elem);
		mapping = bh->b_assoc_map;
		list_del_init(&bh->elem);
		bh->b_assoc_map = NULL;
		if (buffer_dirty(bh)) {
			list_add_tail(&bh->elem, &mapping->private_list);
			bh->b_assoc_map = mapping;
			mapping->nr_entries++;
		}
		pthread_spin_unlock(lock);
		brelse(bh);
		pthread_spin_lock(lock);
	}

	pthread_spin_unlock(lock);

	stg_debug("[%s] inode(%lu) done", __func__, inode->i_ino);
}

/**
 * @brief Get the number of used blocks in the stage area.
 * In the circular buffer, used = head - tail (modulo stage_total).
 * NOTE: Caller must hold j->lock.
 *
 * @param j journal control context
 * @return u32 number of used blocks
 */
u32 get_stage_used_blks(struct journal_control_ctx *j)
{
	u32 base = j->stage_end - j->stage_total;
	u32 head = j->stage_start - base;
	u32 tail = j->stage_tail - base;

	if (head >= tail)
		return head - tail;
	else
		return j->stage_total - tail + head;
}

/**
 * @brief Free (reclaim) blocks in the stage area by advancing stage_tail.
 * Called when DevFS completes a checkpoint.
 *
 * @param j journal control context
 * @param nr_blks number of blocks to free
 */
void stage_free_blks(struct journal_control_ctx *j, u32 nr_blks)
{
	u32 base = j->stage_end - j->stage_total;

	oxb_info("Stage area freed: nr_blks=%u", nr_blks);

	jlock_acquire(&j->lock);

	j->stage_tail += nr_blks;

	/*
	 * If there is a gap (skipped blocks at the boundary) and the tail
	 * has reached it, skip the gap by jumping tail to base.
	 */
	if (j->stage_gap_start && j->stage_tail >= j->stage_gap_start) {
		/* Carry over any excess beyond the gap to the base region. */
		j->stage_tail = base + (j->stage_tail - j->stage_gap_start);
		j->stage_gap_start = 0;
	}

	/* Wrap tail to base if it reached stage_end (no-gap case). */
	if (j->stage_tail >= j->stage_end)
		j->stage_tail = base + (j->stage_tail - j->stage_end);

	pthread_spin_unlock(&j->lock);
}

/**
 * @brief Get the stage area usage percentage.
 * Returns the ratio of used blocks to total stage area as a percentage.
 * Accounts for blocks reclaimed via checkpoint (stage_tail).
 * NOTE: Caller must hold j->lock.
 *
 * @param j journal control context
 * @param used_blks number of used blocks (from get_stage_used_blks())
 * @return int usage percentage (0-100)
 */
int get_stage_usage_pct(struct journal_control_ctx *j, u32 used_blks)
{
	if (j->stage_total == 0)
		return 0;

	return (int)((u64)used_blks * 100 / j->stage_total);
}

/**
 * @brief Reserve a contiguous region in the stage area (circular buffer).
 * If the requested region would cross the stage_end boundary, the remaining
 * space at the end is skipped and the allocation wraps to the base of the
 * stage area, keeping the transaction contiguous on disk.
 * NOTE: Caller must hold j->lock.
 *
 * @param j journal control context (locked)
 * @param stx stage transaction (stx->start and stx->commit_baddr are set)
 * @param nr_blks total number of blocks to reserve (including commit block)
 */
static void stage_reserve_area(struct journal_control_ctx *j, stage_tx *stx,
			       u32 nr_blks)
{
	u32 base = j->stage_end - j->stage_total;
	u32 new_start;
	u32 free_blks, gap;

	/*
	 * Ensure enough free space for the allocation.  If the allocation
	 * would cross stage_end, a wrap-around will skip the remaining
	 * blocks (gap), so account for that waste as well.
	 * Recalculate both after re-acquiring the lock since other threads
	 * may have changed stage_start while the lock was released.
	 */
	free_blks = j->stage_total - get_stage_used_blks(j);
	gap = (j->stage_start + nr_blks > j->stage_end) ?
		      j->stage_end - j->stage_start :
		      0;

#ifdef ENABLE_CKPT
	while (nr_blks + gap > free_blks) {
		u32 deficit = nr_blks + gap - free_blks;
		pthread_spin_unlock(&j->lock);
		oxb_warn("Stage area full: requesting sync ckpt "
			 "(need=%u free=%u gap=%u deficit=%u)",
			 nr_blks, free_blks, gap, deficit);
		msg_send_devfs_stg_ckpt(j->stage_total, deficit, 1 /* sync */);
		jlock_acquire(&j->lock);
		free_blks = j->stage_total - get_stage_used_blks(j);
		gap = (j->stage_start + nr_blks > j->stage_end) ?
			      j->stage_end - j->stage_start :
			      0;
	}
#else
	if (nr_blks + gap > free_blks) {
		oxb_error("Stage area full. Sync ckpt is disabled.");
		panic("Stage area full. Sync ckpt is disabled.");
	}
#endif
	stx->start = j->stage_start;
	new_start = j->stage_start + nr_blks;

	/* If this transaction would cross the boundary, wrap to base. */
	if (new_start > j->stage_end) {
		oxb_warn("Stage area wrap-around: stage_start=%u -> base=%u "
			 "(skipped %u blocks at the end)",
			 j->stage_start, base, j->stage_end - j->stage_start);

		/* Record the gap so stage_tail can skip it later. */
		j->stage_gap_start = j->stage_start;

		stx->start = base;
		new_start = base + nr_blks;

		BUG_ON(new_start > j->stage_end,
		       "stage tx exceeds stage area size");
	}

	/*
	 * Check that the allocation does not overwrite un-checkpointed data.
	 * This can happen when head has wrapped but tail has not yet advanced.
	 */
	if (j->stage_start < j->stage_tail || stx->start < j->stage_start) {
		/* Head has wrapped or allocation wrapped to base. */
		BUG_ON(new_start > j->stage_tail,
		       "stage area overrun: would overwrite un-ckpt'd data");
	}

	j->stage_start = new_start;

	/* Wrap stage_start to base if it reached exactly stage_end. */
	if (j->stage_start == j->stage_end)
		j->stage_start = base;

	/* The commit block is the last block of the transaction. */
	stx->commit_baddr = stx->start + nr_blks - 1;
}

/**
 * @brief fix how many blocks are required for this staging
 * the number of blocks determined here, it must not change after this
 * NOTE: This function must be called after stage_fill_index_blk() and
 * stage_fill_tags() functions, because the number of blocks they require is
 * counted in stx->nr_pending.
 * 
 * @param stx 
 * @param inode 
 * journaling tx.
 */
void stage_fix_blocks(stage_tx *stx, struct inode *inode)
{
	struct journal_control_ctx *j = stx->j;
	u32 nr_dirty_blks;

	nr_dirty_blks = stx->nr_dirty_blks;

#if (STG_WAIT_MODE == ALWAYS_PERSIST)
	/* NOTE: We reserve staging area for the waiting dirty blocks
	 * atomically. Otherwise, other threads can use the staging area,
	 * which incurs interleaved staging area use by two different files.
	 * If we found that background journaling is completed after
	 * stage_fill_data for the new dirty data is returned, we can just skip
	 * those reserved area. (Of course, the space is not used efficiently
	 * with this design.)
	 */
	nr_dirty_blks += stx->nr_waiting_dirty_data_blks[0];
	nr_dirty_blks += stx->nr_waiting_dirty_ird_blks[0];
	nr_dirty_blks += stx->nr_waiting_dirty_data_blks[1];
	nr_dirty_blks += stx->nr_waiting_dirty_ird_blks[1];
#endif

	jlock_acquire(&j->lock);

	// Nothing is written yet.
	oxbow_assert(stx->nr_issued == 0);

	/* Reserve contiguous stage area (circular buffer). */
	stage_reserve_area(j, stx,
			   stx->nr_pending + nr_dirty_blks + 1 /* commit blk */);

	// tag, index, etc. blocks are counted here.
	stx->nr_other_blks = stx->nr_pending;

	sync_debug(
		"[FIX BLOCKS] (tls_tid=%d) reserved: %lu - %lu (commit_baddr=%lu)",
		tls_tid, stx->start, stx->commit_baddr, stx->commit_baddr);
	sync_debug(
		"Total number of blocks (%u) = nr_other_blks(%u) + nr_dirty_blks (%u)",
		stx->nr_other_blks + nr_dirty_blks, stx->nr_other_blks,
		nr_dirty_blks);
	sync_debug(
		"\tnr_dirty_blks (%u) = dirty_data(%u) + waiting_dirty_data (%u + %u) + waiting_dirty_ird (%u + %u)",
		nr_dirty_blks, stx->nr_dirty_blks,
		stx->nr_waiting_dirty_data_blks[0],
		stx->nr_waiting_dirty_data_blks[1],
		stx->nr_waiting_dirty_ird_blks[0],
		stx->nr_waiting_dirty_ird_blks[1]);

	pthread_spin_unlock(&j->lock);

	stx->stage_area_reserved = true;

#ifdef ENABLE_ASYNC_CKPT
	/* Proactively trigger async ckpt when free space is low. */
	u32 nr_used_blks;
	int stg_free_pct;
	u32 nr_blks_to_free;

	jlock_acquire(&j->lock);

	nr_used_blks = get_stage_used_blks(j);
	stg_free_pct = 100 - get_stage_usage_pct(j, nr_used_blks);
	pthread_spin_unlock(&j->lock);

	if (stg_free_pct < STG_CKPT_THRESHOLD_ASYNC) {
		if (!atomic_exchange(&j->stg_ckpt_in_flight, true)) {
			// OPTIMIZE: How many blocks do we request to checkpoint? (Policy)
			nr_blks_to_free = nr_used_blks / 2;

			oxb_info(
				"Async CKPT requested. Stage free space: %d%% (threshold: %d%%) nr_blks_to_free=%u",
				stg_free_pct, STG_CKPT_THRESHOLD_ASYNC,
				nr_blks_to_free);

			msg_send_devfs_stg_ckpt(j->stage_total,
						nr_blks_to_free,
						0 /* is_async */);
		}
	}
#endif

	stg_debug("[%s] inode(%lu) done", __func__, inode->i_ino);
}

static void stage_fill_data(stage_tx *stx, struct inode *inode,
			    struct inode_range_dirty *ird)
{
	bit_index_t start;
	uint32_t i;
	uint64_t cnt, processed, split, remains, to_process;
	uint64_t data_blks_cnt = 0;
	uint64_t nvme_max_blks = g_max_nvme_max_io_size / OXBOW_BLOCK_SIZE;

	BUG_ON(ird == NULL, "ird is NULL");

next_ird:
	for (i = 0; i < ird->count; i++) {
		start = ird->dirty_pages[i].start;
		cnt = ird->dirty_pages[i].nr;

		if (!cnt)
			continue;

		/* Splitting may be required as contiguous data can be larger than spdk buffer size */
		processed = split = 0;
		remains = cnt;

		while (remains > 0) {
			to_process = (stx->nr_blocks == 0) ?
					     min(remains, nvme_max_blks) :
					     min(remains, stx->nr_blocks);

			copy_data_blks(inode, stx, start + processed,
				       to_process);
			data_blks_cnt += to_process;
			processed += to_process;
			remains -= to_process;

			sync_debug(
				"[%s] cnt=%d, processed=%d, remains=%d, to_process=%d data_blks_cnt=%d, stx->nr_blocks=%d",
				__func__, cnt, processed, remains, to_process,
				data_blks_cnt, stx->nr_blocks);
		}

		oxbow_assert(cnt == processed);

		ird->dirty_pages[i].start = 0;
		ird->dirty_pages[i].nr = 0;
	}

	if (ird->next) {
		ird = ird->next;
		goto next_ird;
	}

	stx->data_blks_cnt += data_blks_cnt;

	stg_debug("[tid%d, %s] inode(%lu) done", tls_tid, __func__,
		  inode->i_ino);
}

void stage_waiting_dirty_data(stage_tx *stx, struct inode *inode,
			      struct inode_range_dirty *waiting_dirty_list,
			      uint32_t df_id)
{
	uint32_t nr_ird_blks;
	// oxb_warn("(tls_tid=%d) @@@@@@@@@@@@@@@@@@@ stage_waiting_dirty_data() called.", tls_tid);

	// For now, index blocks are not staged. (stage_fill_index_blk()).
	// Indexing blocks are re-constructed based on the data blocks.
	// Additionally, even if it is required to store the indexing block, it
	// doesn't impact the evaluation results as there are few index
	// blocks.

	// We don't need to fill tags. We instead store the ird list.
	// Tags can be constructed from the ird list on recovery.
	// Note that blocks are allocated while building journal tx - no block
	// allocation should happen here.

	// Serialize the waiting dirty IRD list compactly into stage area buffers.
	nr_ird_blks = serialize_and_fill_ird_list(stx, waiting_dirty_list);

	if (nr_ird_blks != stx->nr_waiting_dirty_ird_blks[df_id]) {
		oxb_error(
			"nr_ird_blks(%u) != stx->nr_waiting_dirty_ird_blks[%d](%u)",
			nr_ird_blks, df_id,
			stx->nr_waiting_dirty_ird_blks[df_id]);
	}
	oxbow_assert(nr_ird_blks == stx->nr_waiting_dirty_ird_blks[df_id]);

	// Persist the waiting dirty data blocks.
	stage_fill_data(stx, inode, waiting_dirty_list);
}

#if (STG_WAIT_MODE == ALWAYS_WAIT)
static inline void wait_for_txid_change(struct inode *inode, int df_id, int expected_txid)
{
	pthread_mutex_lock(&inode->i_journal_waiting_dirty_mutex[df_id]);
	while (expected_txid ==
	       atomic_load(&inode->i_journal_waiting_dirty_txid[df_id])) {
		pthread_cond_wait(&inode->i_journal_waiting_dirty_cond[df_id],
				  &inode->i_journal_waiting_dirty_mutex[df_id]);
	}
	pthread_mutex_unlock(&inode->i_journal_waiting_dirty_mutex[df_id]);
}
#endif

/**
 * @brief 
 * 
 * @param stx 
 * @param inode 
 * @param be 
 * @param stg_dirty_list Snapshot of i_stg_dirty_start.
 * @param waiting_dirty_list 
 * @param waiting_txid 
 */
static void stage_file_data(stage_tx *stx, struct inode *inode,
			    struct buffer_extent *be,
			    struct inode_range_dirty *stg_dirty_list,
			    struct inode_range_dirty *waiting_dirty_list[],
			    int waiting_txid[])
{
	int older_df_id = -1;
	int recent_df_id = -1;

	// [1] metadata
	PF_TL_START(an_____evt_stage_fill_index_blk);
	stage_fill_index_blk(stx, inode); // may be indexing blocks
	PF_TL_END(an_____evt_stage_fill_index_blk);

	PF_TL_START(ao_____evt_stage_fill_tags);
	// Block allocation is done in this function.
	stage_fill_tags(stx, inode, be, stg_dirty_list);
	PF_TL_END(ao_____evt_stage_fill_tags);

	PF_TL_START(ap_____evt_stage_fix_blocks);
	// Start block address of this staging is decided in this function.
	stage_fix_blocks(stx, inode);
	PF_TL_END(ap_____evt_stage_fix_blocks);

	// [2] data
	PF_TL_START(aq_____evt_stage_fill_data);
	stage_fill_data(stx, inode, stg_dirty_list);
	PF_TL_END(aq_____evt_stage_fill_data);

#if (STG_WAIT_MODE != NO_WRITE)

	// Decide order of two txs.
	if (waiting_txid[0] == -1 && waiting_txid[1] == -1)
		goto ret;
	else if (waiting_txid[0] == -1) {
		older_df_id = 1;
		goto older_tx;
	} else if (waiting_txid[1] == -1) {
		older_df_id = 0;
		goto older_tx;
	} else {
		if (waiting_txid[0] < waiting_txid[1]) {
			older_df_id = 0;
			recent_df_id = 1;
		} else {
			older_df_id = 1;
			recent_df_id = 0;
		}
	}
#endif

	////////////////////////////////////////////////////////////////////
	///////////////// POLICY FOR WAITING OR PERSISTING /////////////////
	////////////////////////////////////////////////////////////////////
#if (STG_WAIT_MODE == ALWAYS_WAIT)
	// Wait until both txs are completed (txid is updated).
	// Wait for recent_tx first while waiting for the older_tx to complete.
recent_tx:
	wait_for_txid_change(inode, recent_df_id, waiting_txid[recent_df_id]);
older_tx:
	wait_for_txid_change(inode, older_df_id, waiting_txid[older_df_id]);

#elif (STG_WAIT_MODE == ALWAYS_PERSIST)
	int cur_txid;
	stx->waiting_dirty_skipped[0] = false;
	stx->waiting_dirty_skipped[1] = false;

	// Handle recent_tx first while waiting for the older_tx to complete.
	// Check still background commit is ongoing.
recent_tx:
	// We know that the journal tx has been completed if stored txid is
	// different from the current txid. No need to stage the list.
	cur_txid = atomic_load(&inode->i_journal_waiting_dirty_txid[recent_df_id]);
	if (waiting_txid[recent_df_id] != cur_txid) {
		oxb_info(
			"(tls_tid=%d) RECENT_TX: waiting_txid(%d) != cur_txid(%d)",
			tls_tid, waiting_txid[recent_df_id], cur_txid);
		free_ird_snapshot(waiting_dirty_list[recent_df_id]);
		stx->waiting_dirty_skipped[recent_df_id] = true;
		goto older_tx;
	}

	stage_waiting_dirty_data(stx, inode, waiting_dirty_list[recent_df_id], recent_df_id);
	free_ird_snapshot(waiting_dirty_list[recent_df_id]);

older_tx:
	// tx finished.
	cur_txid = atomic_load(&inode->i_journal_waiting_dirty_txid[older_df_id]);
	if (waiting_txid[older_df_id] != cur_txid) {
		oxb_info(
			"(tls_tid=%d) OLDER_TX: waiting_txid(%d) != cur_txid(%d)",
			tls_tid, waiting_txid[older_df_id], cur_txid);
		free_ird_snapshot(waiting_dirty_list[older_df_id]);
		stx->waiting_dirty_skipped[older_df_id] = true;
		goto ret;
	}

	stage_waiting_dirty_data(stx, inode, waiting_dirty_list[older_df_id], older_df_id);
	free_ird_snapshot(waiting_dirty_list[older_df_id]);

#elif (STG_WAIT_MODE == ADAPTIVE_WAIT)
	// TODO: based on the size of waiting dirty data?

#elif (STG_WAIT_MODE == NO_WRITE)
	// NO WRITE. (Do not guarantee crash consistency).
	// do nothing.
#else
#error "Invalid STG_WAIT_MODE"
#endif

ret:
	return;
}

static void stage_fix_dir_blocks(stage_tx *stx, struct inode *dir, uint32_t n_blks)
{
	struct journal_control_ctx *j = stx->j;

	jlock_acquire(&j->lock);

	// Nothing is written yet.
	oxbow_assert(stx->nr_issued == 0);

	/* Reserve contiguous stage area (circular buffer). */
	stage_reserve_area(j, stx,
			   stx->nr_pending + n_blks + 1 /* commit blk */);

	pthread_spin_unlock(&j->lock);

	/* Directory path uses only descriptor, tag and directory blocks.
	 * Count them as "other" blocks for validation. */
	oxb_info("(tls_tid=%d) nr_other_blks=%u, nr_pending=%u n_blks=%u",
		 tls_tid, stx->nr_other_blks, stx->nr_pending, n_blks);

	// desc and directory blks.
	stx->nr_other_blks = stx->nr_pending + n_blks;

	stx->stage_area_reserved = true;
}

/**
 * @brief build stage transaction, based on inode/file state.
 *
 * @return int (success on 0)
 */
static int build_stage_dir(struct inode *dir, stage_tx *stx)
{
	struct buffer_head *bh;
	int ret = -1;
	uint32_t n_blks = 0;

	// TODO: Some features are not supported for directory because it does
	// not affect to the evaluations.
	// Here are some missing features for the complete implementation:
	//  - waiting dirty list management is not implemented for directory.
	//  - dir inode is not added to the j_file_sets.
	//
	oxb_warn("Staging directory is not fully supported.");
	// panic("Not supported: staging directory");

	inode_lock(dir);
	if (dir->i_state & I_DELETED) {
		inode_unlock(dir);
		// FIXME: Directory staging is not supported. Skip staging for deleted dir.
		return 0;
	}
	inode_unlock(dir);

	dir_inode_lock(dir);
	if (stage_inode(stx, dir, NULL)) {
		log_error("failed to stage dir inode");
		dir_inode_unlock(dir);
		goto ret;
	}

	pthread_spin_lock(&dir->i_mapping->private_lock);

	// Count # of blocks.
	list_for_each_entry (bh, &dir->i_mapping->private_list, elem) {
		if (buffer_dirty(bh))
			n_blks++;
	}

	// Reserve blocks in stage area.
	stage_fix_dir_blocks(stx, dir, n_blks);

	list_for_each_entry (bh, &dir->i_mapping->private_list, elem) {
		if (buffer_dirty(bh)) {
			copy_blk(stx, bh->b_data);
			stx_wrtag(stx, bh->b_blocknr, 1);
			clear_buffer_dirty(bh);
		}
	}

	pthread_spin_unlock(&dir->i_mapping->private_lock);

	// Set MRC tx id.
	jlock_acquire(&dir->i_sb->journal->lock);
	stx->desc_blk_copy.h.mrc_tx_id = dir->i_sb->journal->mrc_tx_id;
	pthread_spin_unlock(&dir->i_sb->journal->lock);

	check_and_add_journal_file(dir);

	list_add_tail(&stx->elem, &dir->i_stage_list);
	dir_inode_unlock(dir);

	ret = 0;
ret:
	return ret;
}

/**
 * @brief build stage transaction, based on inode/file state.
 *
 * @return int (success on 0)
 */
static int build_stage_tx(struct inode *inode, stage_tx *stx)
{
	struct buffer_extent be = { 0 };
	int ret = -1;
	int waiting_txid[2] = { -1, -1 };
	struct inode_range_dirty *stg_ird_snap = NULL; // Snapshot of i_stg_dirty_start.
	struct inode_range_dirty *waiting_dirty_list[2] = { NULL, NULL };

	// stg_debug("[%s] tag:%d", __func__, stx->desc_blk->h.nr_tags);
	/* Stage inode first, then file data based on inode state.
	 * Atomic file state modify, which means it is a critical path */
	PF_TL_START(ad____evt_wait_shi_lock_fg);
	shared_inode_lock(inode);
	PF_TL_END(ad____evt_wait_shi_lock_fg);

	PF_TL_START(ae____evt_stage_inode);
	if (stage_inode(stx, inode, &be)) {
		goto ret;
	}
	PF_TL_END(ae____evt_stage_inode);

	PF_TL_CNT(z08_stg_n_inodes, 1);

	PF_TL_START(ah____evt_inode_gather_dirty);
	inode_gather_dirty_pagebits(inode, 0, -1);
	PF_TL_END(ah____evt_inode_gather_dirty);

	/* Snapshot ird. */
	stg_ird_snap = snapshot_ird(inode->i_stg_dirty_start);

	/* Calculate the number of dirty blocks. */
	stx->nr_dirty_blks =
		inode_get_stg_dirty_blocks_nr(inode, stg_ird_snap);

	// Get the mrc_tx_id from the journal. (Make it close to dirty bit snapshot.)
	// NOTE: The stage tx can be recovered only when MRC tx is successfully
	// recovered.
	jlock_acquire(&inode->i_sb->journal->lock);
	stx->desc_blk_copy.h.mrc_tx_id = inode->i_sb->journal->mrc_tx_id;
	pthread_spin_unlock(&inode->i_sb->journal->lock);

	// Check if there is ongoing background journaling.
	// And store the txid for later use.
	for (int i = 0; i < 2; i++) {
		pthread_spin_lock(&inode->i_journal_waiting_dirty_lock[i]);

		waiting_txid[i] = atomic_load(&inode->i_journal_waiting_dirty_txid[i]);

		// Cut the list.
		//
		// (waiting_txid[i] != -1)
		//     --> there is ongoing background journaling.
		if (waiting_txid[i] != -1) {
			// The dirty waiting list has already been processed by
			// another fsync.
			if (is_dirty_waiting_list_empty(inode, i)) {
				// Regard as no ongoing background journaling.
				waiting_txid[i] = -1;
				goto next;
			}

#if (STG_WAIT_MODE != NO_WRITE)
			// Get snapshot of the list and clear the live list.
			waiting_dirty_list[i] = snapshot_ird(
				inode->i_journal_waiting_dirty_start[i]);

			// # of dirty data blocks.
			stx->nr_waiting_dirty_data_blks[i] =
				inode->i_journal_waiting_nr_dirty_blks[i];

			// # of blocks to store ird list.
			stx->nr_waiting_dirty_ird_blks[i] =
				inode->i_journal_waiting_nr_ird_blks[i];

			// Reset the blk counters as well.
			inode->i_journal_waiting_nr_dirty_blks[i] = 0;
			inode->i_journal_waiting_nr_ird_blks[i] = 0;
#endif

			// NOTE: The inode will be removed from the waiting file list
			// in the journal context. (Cf. i_journal_waiting_lists)
		}
	next:
		pthread_spin_unlock(&inode->i_journal_waiting_dirty_lock[i]);
	}

	inode_lock(inode);

	// Add to journal tx.
	// NOTE: We have to check the stage list separately from the dirty list
	// as there might not be any writes. (fsync only).
	check_and_add_journal_file(inode);

	// Add to the inode's stage list.
	//
	// NOTE: The journaling thread can put this stage tx into the journal tx's
	// stage trace block even though the stage tx is not committed yet.
	// During checkpointing and recovery, DevFS must check whether the
	// stage tx has been committed.
	list_add_tail(&stx->elem, &inode->i_stage_list);

	inode_unlock(inode);

	shared_inode_unlock(inode);

	/* Stage file data, several steps to do this work */
	PF_TL_START(am____evt_stage_file_data);
	stage_file_data(stx, inode, &be, stg_ird_snap, waiting_dirty_list,
			waiting_txid);
	PF_TL_END(am____evt_stage_file_data);

	// Free ird snapshot.
	free_ird_snapshot(stg_ird_snap);

#ifdef DEBUG_PRINT_STG_TX
	// print_stage_tx(stx, inode, ~0U);
	// print_stage_tx(stx, inode, DUMP_EXT4_EXTENT_BLOCKS|DUMP_EXT4_INODE_TABLE);
	print_stage_tx(stx, inode, DUMP_EXT4_EXTENT_BLOCKS);
#endif

	ret = 0;
ret:
	return ret;
}

/**
 * @brief the file stored into staging area as transaction, it contains metadata and data.
 *
 * @return int (success on 0)
 */
int stage_file(struct inode *inode)
{
	struct stage_commit_block *commit_blk;
	struct stage_descriptor_block *desc_blk;
	stage_tx *stx;
	int ret = -1;
	baddr_t desc_baddr;
	baddr_t commit_baddr; // For validation.
	uint32_t nr_waiting_dirty_ird_blks_written = 0;

	sync_debug("[TID:%d %s] inode(%lu)", tls_tid, __func__, inode->i_ino);
	PF_TL_START(ab__evt_sd_stg);

	inode_lock(inode);
	if (inode->i_state & I_DELETED && inode->i_state & I_HAS_WORKER) {
		// If this is printed please let jhlee know.
		// application do fsync after unlink + close
		// varmail do
		oxb_warn("will be deleted inode(%lu) state 0x%lx", inode->i_ino,
			 inode->i_state);
		inode_unlock(inode);
		return 0;
	}
	// no open structure and unlink done.
	// closed but fsynced file ... is it right?
	if (inode->i_state & I_DELETED && !(inode->i_state & I_HAS_WORKER)) {
		oxb_warn("deleted inode(%lu) state 0x%lx", inode->i_ino,
			 inode->i_state);
		inode_unlock(inode);
		return 0;
	}

	stx = new_stage_tx(inode);
	if (!stx) {
		inode_unlock(inode);
		return -1;
	}

	stx->ext_blk = NULL;

	resv_blk(stx); // Only reserve a block for desc block. It is copied later.

	inode_unlock(inode);

	stx->desc_blk_copy.h.blocktype = STAGE_DESC_BLK;
	stx->desc_blk_copy.h.ino = inode->i_ino;
	stx->desc_blk_copy.h.nr_etag_blks = 0;
	stx->desc_blk_copy.h.nr_tags = 0;
	stx->desc_blk_copy.h.next_etag_blk = 0;

	/* Build transaction based on fsynced file */
	PF_TL_START(ac___evt_sd_stg_build_tx);

	if (S_ISREG(inode->i_mode)) {
		if (build_stage_tx(inode, stx)) {
			oxb_error("build fail");
			goto ret;
		}
	} else if (S_ISDIR(inode->i_mode)) {
		if (build_stage_dir(inode, stx)) {
			oxb_error("build fail");
			goto ret;
		}
	} else {
		oxb_error("not expected mode inode(%lu)", inode->i_ino);
		goto ret;
	}

	PF_TL_END(ac___evt_sd_stg_build_tx);

	PF_TL_START(ar___evt_sd_stg_io_tx);

	// if (stx->desc_blk_copy.h.nr_tags)
	// 	print_desctag_block(&stx->desc_blk_copy);
	// if (stx->ext_blk && stx->ext_blk->h.nr)
	// 	print_tag_block(stx->ext_blk);

	// stx->start should already be set.
	oxbow_assert(stx->start);
	oxbow_assert(stx->stage_area_reserved);

	// desc block is written at the beginning of the tx.
	desc_baddr = stx->start;

	// do remaining IO
	if (stx->nr_pending) {
		// oxb_debug("Do remaining IO tls_tid=%d start=%u nr_blks=%u",
		// 	 tls_tid, stx->start + stx->nr_issued,
		// 	 stx->nr_pending); // TMP

		// If this is the first I/O, write desc block together.
		if (!stx->desc_blk_skipped) {
			char *first_page;

			// Fill the first page with desc block.
			first_page = nvme_get_seq_buffer(0);
			memcpy(first_page, &stx->desc_blk_copy,
			       sizeof(struct stage_descriptor_block));
		}

#ifdef ASYNC_DESC_COMMIT_WRITE
		uint32_t issued_tail_reqs = nvme_direct_write_async_track(
			stx->seq_idx + 1, stx->start + stx->nr_issued,
			stx->nr_pending, false, stx_nvme_slot_complete, stx);
		if (!issued_tail_reqs) {
			oxb_error("failed to issue tail data writes");
			stx->io_error = true;
			goto ret;
		}
		stx_mark_slots_issued_prefix(stx, issued_tail_reqs);
		stx_track_req_issue(stx, issued_tail_reqs, STX_REQ_TAIL_DATA);
#else
		uint32_t sync_tail_reqs = (uint32_t)stx->seq_idx + 1;
		stx_mark_slots_issued_prefix(stx, sync_tail_reqs);
		stx_track_req_issue(stx, sync_tail_reqs, STX_REQ_TAIL_DATA);
		// Sync IO.
		nvme_direct_write(stx->seq_idx + 1, stx->start + stx->nr_issued,
				  stx->nr_pending, false);
		stx_track_req_complete(stx, sync_tail_reqs);
		stx_mark_slots_completed_prefix(stx, sync_tail_reqs);
#endif

		PF_TRACK_TPUT(SYNC_io_data, stx->nr_pending);

		// Reset states.
		stx->seq_idx = 0;
		stx->nr_issued += stx->nr_pending;
		stx->nr_pending = 0;
		stx->cur = nvme_get_seq_buffer(0);
		stx->nr_blocks = g_max_nvme_max_io_size / OXBOW_BLOCK_SIZE;
	} else {
		sync_debug("no pending IO stx->seq: %d", stx->seq_idx);
	}

	PF_TL_END(ar___evt_sd_stg_io_tx);


	// If DESC BLOCK has been skipped, we persist it here.
	if (stx->desc_blk_skipped) {
		PF_TL_START(as___evt_sd_stg_io_desc);

		memcpy(stx->desc_blk_buf, &stx->desc_blk_copy,
		       sizeof(struct stage_descriptor_block));

		sync_debug(
			"(tls_tid=%d) Write desc block separately. (baddr=%lu index=%u (inum=%u) size=%u)",
			tls_tid, desc_baddr, stx->desc_blk_copy.h.inode_index,
			stx->desc_blk_copy.h.ino,
			stx->desc_blk_copy.h.inode_size);

		sync_debug("(tls_tid=%d) desc_blk_buf: %p index=%u (inum=%u)",
			 tls_tid, stx->desc_blk_buf,
			 ((struct stage_descriptor_block *)stx->desc_blk_buf)
				 ->h.inode_index,
			 ((struct stage_descriptor_block *)stx->desc_blk_buf)
				 ->h.ino);

#ifdef ASYNC_DESC_COMMIT_WRITE
		nvme_direct_write_with_buf_async(desc_baddr, stx->desc_blk_buf, 1);
		stx_track_req_issue(stx, 1, STX_REQ_DESC);
#else
		stx_track_req_issue(stx, 1, STX_REQ_DESC);
		// Sync IO.
		nvme_direct_write_with_buf(desc_baddr, stx->desc_blk_buf, 1);
		stx_track_req_complete(stx, 1);

#if 0
		// TMP validate I/O
		memset(stx->desc_blk_buf, 0, sizeof(struct stage_descriptor_block));

		// Read back the desc block.
		{
			struct bio *bio;
			bio = alloc_bio(1, REQ_OP_READ);
			if (bio) {
				bio->bi_start = desc_baddr;
				add_bvec(bio, stx->desc_blk_buf);
				bio->bi_vcnt = bio->bi_vtotal;
				bio->end_io = sync_end_io;
				nvme_rd_submit_bh(bio);

				if (memcmp(stx->desc_blk_buf, &stx->desc_blk_copy,
					   sizeof(struct stage_descriptor_block)) != 0) {
					oxb_error("DESC block verify failed at baddr=%lu", desc_baddr);
				} else {
					oxb_warn("############# DESC block verified OK at baddr=%lu", desc_baddr);
					oxb_warn(
						"### AFTER READ desc_blk_buf: %p index=%u (inum=%u)",
						stx->desc_blk_buf,
						((struct stage_descriptor_block
							  *)stx->desc_blk_buf)
							->h.inode_index,
						((struct stage_descriptor_block
							  *)stx->desc_blk_buf)
							->h.ino);
				}
			} else {
				oxb_error("alloc_bio failed for DESC verify read");
			}
		}
#endif

#endif

		// NOTE: Do not increase stx->nr_issued because it has already
		// been reflected by resv_blk().

		PF_TL_END(as___evt_sd_stg_io_desc);
	}

	/* Write COMMIT BLOCK at the end of the tx. */
	PF_TL_START(at___evt_sd_stg_io_commit);

	BUG_ON(stx->nr_blocks == 0, "more than 0");

	commit_blk = (struct stage_commit_block *)stx->commit_blk_buf;

#if (STG_WAIT_MODE == ALWAYS_PERSIST)
	// Fill commit block.
	if (stx->waiting_dirty_skipped[0]) {
		commit_blk->waiting_dirty_data_blks[0] = 0;
		commit_blk->waiting_dirty_ird_blks[0] = 0;
		commit_blk->waiting_dirty_data_blks[1] =
			stx->nr_waiting_dirty_data_blks[1];
		commit_blk->waiting_dirty_ird_blks[1] =
			stx->nr_waiting_dirty_ird_blks[1];

		nr_waiting_dirty_ird_blks_written =
			stx->nr_waiting_dirty_ird_blks[1];

		commit_baddr = stx->start + stx->nr_issued +
			       stx->nr_waiting_dirty_data_blks[0] +
			       stx->nr_waiting_dirty_ird_blks[0];

	} else if (stx->waiting_dirty_skipped[1]) {
		commit_blk->waiting_dirty_data_blks[0] =
			stx->nr_waiting_dirty_data_blks[0];
		commit_blk->waiting_dirty_ird_blks[0] =
			stx->nr_waiting_dirty_ird_blks[0];
		commit_blk->waiting_dirty_data_blks[1] = 0;
		commit_blk->waiting_dirty_ird_blks[1] = 0;

		nr_waiting_dirty_ird_blks_written =
			stx->nr_waiting_dirty_ird_blks[0];

		commit_baddr = stx->start + stx->nr_issued +
			       stx->nr_waiting_dirty_data_blks[1] +
			       stx->nr_waiting_dirty_ird_blks[1];

	} else { // No skipping.
		commit_blk->waiting_dirty_data_blks[0] =
			stx->nr_waiting_dirty_data_blks[0];
		commit_blk->waiting_dirty_ird_blks[0] =
			stx->nr_waiting_dirty_ird_blks[0];
		commit_blk->waiting_dirty_data_blks[1] =
			stx->nr_waiting_dirty_data_blks[1];
		commit_blk->waiting_dirty_ird_blks[1] =
			stx->nr_waiting_dirty_ird_blks[1];

		nr_waiting_dirty_ird_blks_written =
			stx->nr_waiting_dirty_ird_blks[0] +
			stx->nr_waiting_dirty_ird_blks[1];

		commit_baddr = stx->start + stx->nr_issued;
	}
#else
		commit_baddr = stx->start + stx->nr_issued;
#endif

	commit_blk->pad[0] = 'C';

	// Validation: the number of blocks written.
	// TMP: Print assertion case.
	// if (stx->nr_issued != stx->data_blks_cnt + stx->nr_other_blks +
	// 			      nr_waiting_dirty_ird_blks_written) {
	// 	oxb_error(
	// 		"(tls_tid=%d) nr_issued mismatch. nr_issued(%u) != data_blks_cnt(%u) + nr_other_blks(%u) + nr_waiting_dirty_ird_blks(%u)",
	// 		tls_tid, stx->nr_issued, stx->data_blks_cnt,
	// 		stx->nr_other_blks, nr_waiting_dirty_ird_blks_written);
	// }
	oxbow_assert(stx->nr_issued == stx->data_blks_cnt + stx->nr_other_blks + nr_waiting_dirty_ird_blks_written);

	oxbow_assert(stx->nr_pending == 0);

	// TMP: Print assertion case.
	// if (stx->commit_baddr != commit_baddr) {
	// 	oxb_error(
	// 		"(tls_tid=%d) commit_baddr mismatch. stx->commit_baddr(fixed)=%lu commit_baddr(calculated)=%lu",
	// 		tls_tid, stx->commit_baddr, commit_baddr);

	// 	oxb_warn(
	// 		"(tls_tid=%d) commit_baddr(fixed)=%lu (nr_other=%u nr_dirty=%u nr_waiting_dirty_data_blks[0]=%u nr_waiting_dirty_ird_blks[0]=%u nr_waiting_dirty_data_blks[1]=%u nr_waiting_dirty_ird_blks[1]=%u)",
	// 		tls_tid, stx->commit_baddr, stx->nr_other_blks,
	// 		stx->nr_dirty_blks, stx->nr_waiting_dirty_data_blks[0],
	// 		stx->nr_waiting_dirty_ird_blks[0],
	// 		stx->nr_waiting_dirty_data_blks[1],
	// 		stx->nr_waiting_dirty_ird_blks[1]);

	// 	oxb_warn(
	// 		"(tls_tid=%d) commit_baddr(counted)=%lu (stx->start=%lu + stx->nr_issued=%u --> data_blks_cnt=%u nr_other_blks=%u nr_waiting_dirty_ird=%)",
	// 		tls_tid, commit_baddr, stx->start, stx->nr_issued,
	// 		stx->data_blks_cnt, stx->nr_other_blks,
	// 		nr_waiting_dirty_ird_blks_written);
	// }
	oxbow_assert(stx->commit_baddr == commit_baddr);

#ifdef ASYNC_DESC_COMMIT_WRITE
	nvme_direct_write_with_buf_async(stx->commit_baddr, stx->commit_blk_buf, 1);
	stx_track_req_issue(stx, 1, STX_REQ_COMMIT);

	PF_TL_END(at___evt_sd_stg_io_commit); // Issue only.

	// Wait for all outstanding IOs (reclaim + tail + desc + commit).
	PF_TL_START(au___evt_sd_stg_io_async_wait);
	stx_drain_all_outstanding(stx);
	PF_TL_END(au___evt_sd_stg_io_async_wait);
#else
	stx_track_req_issue(stx, 1, STX_REQ_COMMIT);
	nvme_direct_write_with_buf(stx->commit_baddr, stx->commit_blk_buf, 1);
	stx_track_req_complete(stx, 1);

	PF_TL_END(at___evt_sd_stg_io_commit);
#endif
	if (stx->req_completed_total < stx->req_issued_total)
		stx_drain_all_outstanding(stx);
	stx->nr_issued++;
	oxbow_assert(!stx->io_error);
	oxbow_assert(stx->req_issued_total ==
		     (uint64_t)stx->req_issued_reclaim +
			     (uint64_t)stx->req_issued_tail_data +
			     (uint64_t)stx->req_issued_desc +
			     (uint64_t)stx->req_issued_commit);
	oxbow_assert(stx->req_completed_total == stx->req_issued_total);
	oxbow_assert(stx->busy_slots == 0);
	atomic_store(&stx->is_completed, true);

	// do IO with FUA

	ret = 0;

	sync_debug(
		"[STAGE FILE] inode=%lu mrc_txid=%d start_lba=%lu, total_nblks=%lu (desc_blk=%lu commit_blk=%lu), total_nblks=%lu (%.2f KB, %.2f MB)\n",
		inode->i_ino, stx->desc_blk_copy.h.mrc_tx_id, stx->start,
		stx->nr_issued, desc_baddr, stx->commit_baddr, stx->data_blks_cnt,
		(float)(stx->data_blks_cnt * OXBOW_BLOCK_SIZE) / 1024.0,
		(float)(stx->data_blks_cnt * OXBOW_BLOCK_SIZE) /
			(1024.0 * 1024.0));
ret:

	PF_TL_END(ab__evt_sd_stg);

	return ret;
}

int do_fsync(struct inode *inode)
{
	struct super_block *sb;
	int ret;

#ifdef NO_STAGING
	// disable staging on fsync.
	return 0;
#endif

	nvme_fsync_inflight_inc();

	/* if journal is on */
	sb = inode->i_sb;
	if (sb->journal)
		ret = stage_file(inode);
	else
		ret = sync_file(inode);

	nvme_fsync_inflight_dec();
	return ret;
}
