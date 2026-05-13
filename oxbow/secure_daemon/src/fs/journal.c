#define _GNU_SOURCE
#include <fcntl.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/queue.h>
#include "fs/journal.h"
#include "oxbow_debug.h"

#ifdef DEBUG_PRINT_JNL_TX
#include "utils/print_tx.h"
#endif

#include "fs/journal.h"
#include "fs/fs.h"
#include "fs/buffer_head.h"
#include "global.h"
#include "config.h"
#include "msg.h"
#include "oxbow.h"
#include "data_fetcher.h"
#include "kernfs.h"
#include "profile_secure_daemon.h"
#include "dirty_mgmt.h"
#include "utils/jlock_profile.h"
#include <time.h>
#include <unistd.h>
#include <sys/mman.h>

PF_TL_EVT(c_bg_jnl);
PF_TL_EVT(ca_build_tx);
PF_TL_EVT(caa_wait_ref_down);
PF_TL_EVT(cab_snapshot_meta);
PF_TL_EVT(cac_snapshot_file);
PF_TL_EVT(caca_jtx_reg_jnl_meta);
PF_TL_EVT(cacaa_shinode_lock_wait);
PF_TL_EVT(cacab_reg_inode);
PF_TL_EVT(cacabs_alloc_blocks);
PF_TL_EVT(cacabt_write_inode);
PF_TL_EVT(cacabu_write_inode);
PF_TL_EVT(cacabv_dirty_blocks);
PF_TL_EVT(cacad_gather_dirty);
PF_TL_EVT(cacadf_gather_dirty_clear_bit);
PF_TL_EVT(cacadg_gather_dirty_bits);
PF_TL_EVT(cacadga_add_range_dirty);
PF_TL_EVT(cacb_jtx_reg_jnl_stage);
PF_TL_EVT(cacc_jtx_reg_jnl_file);
PF_TL_EVT(caccm_copy_data_blks);
PF_TL_EVT(caccmi_data_copy);
PF_TL_EVT(caccmj_unlock_pagebit);
PF_TL_EVT(caccmk_request_df);
PF_TL_EVT(caccml_tx_alloc_df_buf);
PF_TL_EVT(caccn_idx_lock_wait);
PF_TL_EVT(cacco_get_blocks);
PF_TL_EVT(cad_snapshot_inode);
PF_TL_EVT(cb_commit);
PF_TL_EVT(cc_alloc_df);

// memcpy in building journal tx.
PF_TL_EVT(d_meta_copy);

// Real-time throughput.
PF_EVT(JNL_memcpy_data);
PF_EVT(JNL_dma_copy);

PF_TL_EVT(y1_fs_wrlockup_time);
PF_TL_EVT(y2_fs_rdlock_wait);
PF_TL_EVT(y3_fs_wrlock_wait);

PF_TL_EVT(z01_jnl_nblks_data);
PF_TL_EVT(z02_jnl_nblks_sb_meta); // including inode, bitmap, sb blks.
PF_TL_EVT(z03_jnl_nblks_inode_meta);
PF_TL_EVT(z04_jnl_nblks_inode);
PF_TL_EVT(z05_jnl_nblks_dirent);
PF_TL_EVT(z06_stg_n_trace_blks);
PF_TL_EVT(z07_stg_tx_cnt);

// Global data fetcher context.
struct data_fetcher_ctx *g_df_ctxs[2] = { NULL, NULL };
// To manage data fetchers.
bool g_df_occupied[2] = { false, false };
pthread_mutex_t g_df_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t g_df_cond = PTHREAD_COND_INITIALIZER;

struct journal_control_ctx *g_journal_ctx = NULL;

uint64_t g_total_blks_data;
uint64_t g_total_blks_sb_meta;
uint64_t g_total_blks_inode_meta;
uint64_t g_total_blks_inode;
uint64_t g_total_blks_dirent;
uint64_t g_total_stg_tx_cnt;

threadpool snapshot_thpool;

/**
 * @brief  exposed api to adding journal, [TODO] wakeup when full
 */
static journal_tx *get_running_trans(struct journal_control_ctx *journal)
{
	journal_tx *tr;

retry:
	jlock_acquire(&journal->lock);
	tr = journal->front;
	pthread_spin_unlock(&journal->lock);
	if (!tr) {
		oxb_error("no running trans");
		return NULL;
	}

	pthread_spin_lock(&tr->j_lock);
	BUG_ON(!(tr->state == TR_LOCKED || tr->state == TR_RUNNING ||
		 tr->state == TR_PAUSE || tr->state == TR_BUILD),
	       "wrong state");
	if (tr->state == TR_LOCKED || tr->state == TR_BUILD) {
		pthread_spin_unlock(&tr->j_lock);
		goto retry;
	} else if (tr->state == TR_RUNNING)
		atomic_fetch_add(&tr->ref, 1);
	else {
		pthread_spin_unlock(&tr->j_lock);
		log_error("wrong state %d", tr->state);
		panic("Journal tx in wrong state");
		return NULL;
	}
	pthread_spin_unlock(&tr->j_lock);

	return tr;
}

static void tr_change_state(journal_tx *tr, int state)
{
	pthread_spin_lock(&tr->j_lock);
	tr->state = state;
	pthread_spin_unlock(&tr->j_lock);
}

/**
 * @brief add this file to current journal transaction
 * 
 * @param inode (target file)
 * @return int (success on 0)
 */
int add_journal_file(struct inode *inode)
{
	struct journal_control_ctx *journal;
	journal_tx *running;

	journal = inode->i_sb->journal;
	BUG_ON(!journal, "no journal should not call this");
	jnl_trace("[%s] inode(%d) journal(%p)", __func__, inode->i_ino,
		 journal);

	running = start_journal(journal);
	if (!running) {
		oxb_error("[%s] get running fail", __func__);
		return -1;
	}

	iget(inode); // journal get inode
	pthread_spin_lock(&running->j_lock);

	// Add to unified hash set
	int ret;
	khiter_t k = kh_put(inode_set, running->j_files_set, (uint64_t)inode, &ret);
	if (ret == -1) {
		pthread_spin_unlock(&running->j_lock);
		oxb_error("Failed to add inode to hash set");
		iput(inode);
		end_journal(running);
		panic("Failed to add inode to hash set");
		return -1;
	}

	pthread_spin_unlock(&running->j_lock);

	// Mark as dirty file
	inode->i_state |= I_IN_JTX_DIRTY_LIST;
	uint32_t j_txid_old = atomic_load(&inode->i_journal_txid);
	atomic_store(&inode->i_journal_txid, running->tid);
	jnl_trace("[j_files ADD] (dirty) inode=%d to tx_id=%u (j_txid_old=%u)",
		  inode->i_ino, running->tid, j_txid_old);

	end_journal(running);
	jnl_trace("[%s] inode(%d) added to journal(tx_id=%d)", __func__,
		  inode->i_ino, running->tid);
	return 0;
}

/**
 * @brief Add a file to the journal transaction if it is not already added.
 * (called in the staging path)
 * NOTE: inode lock (inode_lock()) should be held by its caller.
 * NOTE: shared inode lock (shared_inode_lock()) should be held by its caller.
 * (to synchronize inode->i_state changes with the journaling thread)
 * 
 * @param inode 
 * @return int 
 */
int check_and_add_journal_file(struct inode *inode)
{
	struct journal_control_ctx *journal;
	journal_tx *running;

	journal = inode->i_sb->journal;
	BUG_ON(!journal, "no journal should not call this");

	running = start_journal(journal);
	if (!running) {
		oxb_error("[%s] get running fail", __func__);
		panic("get running fail");
		return -1;
	}

	jnl_trace("[%s] inode(%d) journal(tx_id=%d)", __func__, inode->i_ino, running->tid);

	iget(inode); // journal get inode
	pthread_spin_lock(&running->j_lock);

	// Add to unified hash set
	int ret;
	khiter_t k = kh_put(inode_set, running->j_files_set, (uint64_t)inode, &ret);
	if (ret == -1) {
		pthread_spin_unlock(&running->j_lock);
		oxb_error("Failed to add inode to hash set in staging path.");
		iput(inode);
		end_journal(running);
		panic("Failed to add inode to hash set in staging path.");
		return -1;

	} else if (ret == 0) { // already in the set
		pthread_spin_unlock(&running->j_lock);
		goto end;
	}

	pthread_spin_unlock(&running->j_lock);

	// Mark as dirty file.
	inode->i_state |= I_IN_JTX_DIRTY_LIST;
	uint32_t j_txid_old = atomic_load(&inode->i_journal_txid);
	atomic_store(&inode->i_journal_txid, running->tid);
	// oxb_warn("[j_files ADD] (staged) inode=%d to tx_id=%d (j_txid_old=%u)",
	// 	  inode->i_ino, running->tid, j_txid_old);

end:
	end_journal(running);
	return 0;
}

// Not used.
#if 0
/**
 * @brief add this block to current journal transaction
 */
void add_journal_bh(struct buffer_head *bh)
{
	struct journal_control_ctx *journal;
	journal_tx *running;

	journal = bh->journal;

	jnl_debug("[%s] baddr=%lu", __func__, bh->b_blocknr);

	jlock_acquire(&journal->lock);
	running = journal->front;
	pthread_spin_unlock(&journal->lock);
	BUG_ON(!running, "must have pointer");

	pthread_spin_lock(&running->j_lock);
	list_add_tail(&bh->j_blk_list, &running->j_blks);
	pthread_spin_unlock(&running->j_lock);

	set_buffer_dirty(bh);
}
#endif

/* add this inode block to the journal... */
int add_journal_inode(struct buffer_head *bh)
{
	struct journal_control_ctx *journal;
	journal_tx *tr;

	if (!bh) {
		oxb_warn("[%s] bh is NULL. skip adding to journal", __func__);
		return -1;
	}

	journal = bh->journal;

	/* inode write is done in building transaction */
	tr = journal->back;
	BUG_ON(!tr, "must have pointer");

	jnl_trace("[%s] baddr=%lu added to journal tx(tid=%d)", __func__,
		  bh->b_blocknr, tr->tid);

	pthread_spin_lock(&tr->j_lock);
	list_add_tail(&bh->j_blk_list, &tr->j_blks);
	pthread_spin_unlock(&tr->j_lock);

	set_buffer_injournal(bh);

	return 0;
}

int commit_journal(struct super_block *sb)
{
	// TODO: if jworker is working then wait until next commit can start
	oxb_info("not implemented yet %p", sb->journal);
	return 0;
}

journal_tx *start_journal(struct journal_control_ctx *journal)
{
	return get_running_trans(journal);
}

void end_journal(journal_tx *tr)
{
	if (!tr)
		return;

	atomic_fetch_sub(&tr->ref, 1);
}

// Allocate a data fetcher.
static int alloc_data_fetcher(void){
	int alloc_id = -1;

	pthread_mutex_lock(&g_df_mutex);

	while (alloc_id == -1) {
		for (int i = 0; i < 2; i++) {
			if (!g_df_occupied[i]) {
				g_df_occupied[i] = true;
				alloc_id = i;
				break;
			}
		}
		if (alloc_id == -1)
			pthread_cond_wait(&g_df_cond, &g_df_mutex);
	}
	pthread_mutex_unlock(&g_df_mutex);
	return alloc_id;
}

static void free_data_fetcher(int df_id)
{
	pthread_mutex_lock(&g_df_mutex);
	g_df_occupied[df_id] = false;
	pthread_mutex_unlock(&g_df_mutex);
	pthread_cond_signal(&g_df_cond);
}

/**
 * @brief journal worker routines.
 */

/* this is critical path TODO use SLAB instead of malloc */
static journal_tx *new_transaction(struct journal_control_ctx *j)
{
	journal_tx *new;

	// It is freed at RPC callback: rpc_rdma_client_handler()
	// Or, in build_transaction() if there is nothing to commit.
	new = calloc(1, sizeof *new);
	if (!new) {
		oxb_error("malloc fail");
		return NULL;
	}

	pthread_spin_init(&new->j_lock, PTHREAD_PROCESS_PRIVATE);
	
	// Initialize unified hash set for files
	new->j_files_set = kh_init(inode_set);
	if (!new->j_files_set) {
		oxb_error("Failed to initialize hash set");
		free(new);
		panic("Failed to initialize hash set");
		return NULL;
	}
	
	INIT_LIST_HEAD(&new->j_files_waiting);
	INIT_LIST_HEAD(&new->j_blks);
	new->tid = j->j_transaction_sequence;

	/* no lock */
	j->j_transaction_sequence++;

	PF_TL_START(cc_alloc_df);
	new->df_id = alloc_data_fetcher();
	PF_TL_END(cc_alloc_df);

	return new;
}

static int tx_alloc_meta_buffer(journal_tx *tx)
{
	tx->df_md_buf_id = df_alloc_buffer(
		g_df_ctxs[tx->df_id],
		(char **)&tx->df_md_rdma_buf); // get a data buffer.

	tx->md_cur = tx->df_md_rdma_buf;

	if (!tx->md_cur)
		return -1;

	tx->md_nr_blocks = BG_JOURNAL_COMMIT_SIZE / OXBOW_BLOCK_SIZE;
	// TOCHECK: Do we need to count the number of metadata blocks?

	return 0;
}

static int tx_alloc_buffer(journal_tx *tx)
{
	// In initialization, one buffer is allocated.
	oxbow_assert(tx);

	tx->df_buf_id = df_alloc_buffer(g_df_ctxs[tx->df_id], (char **)&tx->df_rdma_buf);
	tx->cur = tx->df_rdma_buf;

	if (!tx->cur)
		return -1;

	jnl_debug("tx_alloc_buffer: n_df_bufs=%u --> %u", tx->n_df_bufs,
		  tx->n_df_bufs + 1);
	tx->n_df_bufs++;
	tx->nr_blocks = BG_JOURNAL_COMMIT_SIZE / OXBOW_BLOCK_SIZE;
	tx->tot_nr_blocks += tx->nr_blocks;

	return 0;
}

/**
 * @brief change current journal state and make new transaction
 * 
 * @param journal
 * @return journal_tx*
 */
static void start_transaction(struct journal_control_ctx *journal,
			      journal_tx *new)
{
	jlock_acquire(&journal->lock);
	journal->front = new;
	new->state = TR_RUNNING;
	pthread_spin_unlock(&journal->lock);

	jnl_trace("[j_files START] new set: tid=%u", new->tid);
}

/**
 * @brief reserve metadata block area from pre-allocated buffer
 * 
 */
static void *reserve_mdblk(journal_tx *tx, unsigned int cnt)
{
	void *ret;

	BUG_ON(tx->md_nr_blocks < cnt, "overflow");

	ret = tx->md_cur;
	tx->md_cur += cnt * OXBOW_BLOCK_SIZE;
	tx->md_nr_blocks--;

	if (tx->md_nr_blocks == 0) {
		// TODO : allocate more buffer for metadata block
		oxb_error("[%s] not implemented", __func__);
		return NULL;
	}

	return ret;
}

/**
 * @brief Send a request for DevFS to fetch data (one buffer).
 * NOTE: This function should be called after the last block of the tx is filled.
 * 
 * @param tx 
 * @return int 
 */
static int request_data_fetch(journal_tx *tx)
{
	size_t size_to_send;
	uint32_t n_blks_to_send;
	int df_buf_id;
	int req_id; // Request_id. I.e., this is N-th request for data fetch.

	n_blks_to_send = tx->used_blk_cnt - tx->nr_sent_blks;

	// The new buffer is never used.
	if (n_blks_to_send == 0)
		return -1;

	size_to_send = nblks_to_bytes(n_blks_to_send);

	oxb_debug("A df buffer utilization: %u / %u (MB)",
		 size_to_send / 1024 / 1024,
		 df_buf_size(g_df_ctxs[tx->df_id]) / 1024 / 1024);

	df_buf_id = tx->df_buf_id;
	req_id = tx->n_df_bufs - 1;

	// oxb_warn("tx_id=%u Total blk size:%lu", tx->tid,
	// 	 ((size_t)tx->used_blk_cnt) * OXBOW_BLOCK_SIZE);

	msg_send_devfs_fetch_data(tx->df_id, req_id, df_buf_id, size_to_send);

	tx->nr_sent_blks += n_blks_to_send;

	PF_TRACK_TPUT(JNL_dma_copy, n_blks_to_send);

	return 0;
}

/**
 * @brief Copy data from source to the pre-allocated buffer
 * 
 */
static void copy_blk(journal_tx *tx, const void *src, u32 cnt)
{
	void *dst;
	int ret;

	BUG_ON(tx->nr_blocks < cnt, "overflow");

	dst = tx->cur;
	tx->cur += cnt * PAGE_SIZE;
	tx->used_blk_cnt += cnt;
	// oxb_warn("tx_id=%u used_blk_cnt=%u", tx->tid, tx->used_blk_cnt);
	tx->nr_blocks -= cnt;

	PF_TL_START(d_meta_copy);
	memcpy(dst, src, cnt * PAGE_SIZE);
	PF_TL_END(d_meta_copy);

	if (tx->nr_blocks == 0) {
		/* Send request previous allocated buffer */
		ret = request_data_fetch(tx);
		oxbow_assert(ret == 0); // No data to send.

		/* Alloc new df buffer. */
		tx_alloc_buffer(tx);
	}
}

// Difference from copy_blk(): unlocking pagebit.
static void copy_data_blks(struct inode *inode, journal_tx *tx, pgoff_t start,
			   u32 cnt)
{
	void *src;
	unsigned int i, ret;

	src = (char *)inode->data + start * PAGE_SIZE;

	for (i = 0; i < cnt; i++) {
		// PF_TL_START(caccmi_data_copy);

#ifdef DEBUG_HEXDUMP_JNL_DATA_PAGES
		// // Debug: Hexdump source page before copy
		// {
		// 	char debug_label[128];
		// 	snprintf(debug_label, sizeof(debug_label), 
		// 			"Page copy to rdma buffer(0x%lx) - inode %lu, page %lu (before copy)", 
		// 			(uintptr_t)tx->cur,
		// 			inode->i_ino, start + i);
		// 	quick_hexdump((char *)src + i * PAGE_SIZE, PAGE_SIZE, debug_label);
		// }
#endif

		memcpy(tx->cur, (char *)src + i * PAGE_SIZE, PAGE_SIZE);

#ifdef DEBUG_HEXDUMP_JNL_DATA_PAGES
		// Debug: Hexdump destination after copy (optional verification)
		{
			char debug_label[128];
			snprintf(debug_label, sizeof(debug_label), 
					"Page copy to rdma buffer(0x%lx) - inode %lu, page %lu (after copy)", 
					(uintptr_t)tx->cur,
					inode->i_ino, start + i);
			quick_hexdump(tx->cur, PAGE_SIZE, debug_label);
		}
#endif

		// PF_TL_END(caccmi_data_copy);
		PF_TRACK_TPUT(JNL_memcpy_data, 1);

		// PF_TL_START(caccmj_unlock_pagebit);
		inode_unlock_pagebit(inode, start + i);
		// PF_TL_END(caccmj_unlock_pagebit);

		tx->cur += PAGE_SIZE;
		tx->used_blk_cnt++;
		tx->nr_blocks--;

		if (tx->nr_blocks == 0) {
			/* Send request previous allocated buffer */
			PF_TL_START(caccmk_request_df);
			ret = request_data_fetch(tx);
			PF_TL_END(caccmk_request_df);

			oxbow_assert(ret == 0); // No data to send.

			/* Alloc new df buffer. */
			// PF_TL_START(caccml_tx_alloc_df_buf);
			tx_alloc_buffer(tx);
			// PF_TL_END(caccml_tx_alloc_df_buf);
		}
	}

	g_total_blks_data += cnt;
	PF_TL_CNT(z01_jnl_nblks_data, cnt);
}

/**
 * @brief Fill tag in extent_tag block. 
 * 
 */
static int fill_tag_etagblk(struct journal_extent_tag_block *je, u64 start,
			    u32 cnt)
{
	je->tags[je->h.nr].start = start;
	je->tags[je->h.nr].cnt = cnt;
	je->h.nr++;

	jnl_debug("[%s] start=%lu cnt=%u to etag blk", __func__, start, cnt);

	if (je->h.nr == JOURNAL_ETAG_MAX)
		return 1;

	return 0;
}

/**
 * @brief Fill tag in descriptor block. Use remain space of desc block.
 */
static int fill_tag_descblk(struct journal_descriptor_block *jdb, u64 start,
			    u32 cnt)
{
	BUG_ON(jdb->h.nr_tags >= JOURNAL_DESC_TAG_MAX, "idx bug");

	jdb->tags[jdb->h.nr_tags].start = start;
	jdb->tags[jdb->h.nr_tags].cnt = cnt;
	jdb->h.nr_tags++;

	jnl_debug("[%s] start=%lu cnt=%u to desc blk", __func__, start, cnt);

	if (jdb->h.nr_tags == JOURNAL_DESC_TAG_MAX)
		return 1;

	return 0;
}

/**
 * @brief journal transaction write tag to proper block
 */
void fill_tagblk(journal_tx *tx, u64 start, u32 cnt)
{
	if (tx->ext_blk) {
		if (fill_tag_etagblk(tx->ext_blk, start, cnt)) {
			tx->desc_blk->h.nr_etag_blks++;
			d_trace("[TX_META] another etag blk at index %lu",
				get_cur_mdblk_index(tx));
			tx->ext_blk = reserve_mdblk(tx, 1);
			tx->ext_blk->h.nr = 0;
			tx->ext_blk->h.blocktype = ETAG_BLK;
		}

		// oxb_warn(
		// 	"ETAG: tag_blk_idx=%ld start=%lu cnt=%u total_tags=%u (desc_blk_tags=%u cur_ext_blk_tags=%u upto_prev_etag_blks_tags=%u)",
		// 	(long)tx->desc_blk->h.nr_etag_blks - 1, start, cnt,
		// 	tx->desc_blk->h.nr_tags + tx->ext_blk->h.nr +
		// 		(tx->desc_blk->h.nr_etag_blks-1) * JOURNAL_ETAG_MAX,
		// 	tx->desc_blk->h.nr_tags, tx->ext_blk->h.nr,
		// 	(tx->desc_blk->h.nr_etag_blks-1) * JOURNAL_ETAG_MAX);

	} else {
		if (fill_tag_descblk(tx->desc_blk, start, cnt)) {
			tx->desc_blk->h.nr_etag_blks = 1;
			d_trace("[TX_META] first etag blk at index %lu",
				get_cur_mdblk_index(tx));
			tx->ext_blk = reserve_mdblk(tx, 1);
			tx->ext_blk->h.nr = 0;
			tx->ext_blk->h.blocktype = ETAG_BLK;
		}

		// oxb_warn("ETAG: tag_blk_idx=%ld start=%lu cnt=%u total_tags=%u",
		// 	 (long)tx->desc_blk->h.nr_etag_blks - 1, start, cnt,
		// 	 tx->desc_blk->h.nr_tags);
	}

}

/**
 * @brief metadata : inodes, bitmap, superblock 
 */
static void snapshot_metadata_blocks(journal_tx *tx)
{
	struct super_block *sb = tx->sb;
	struct buffer_head *bh;
	struct list_head *pos, *n;

	pthread_spin_lock(&sb->s_mapping.private_lock);
	list_for_each_safe (pos, n, &sb->s_mapping.private_list) {
		bh = list_entry(pos, struct buffer_head, elem);
		lock_buffer(bh);
		copy_blk(tx, bh->b_data, 1);

		g_total_blks_sb_meta++;
		PF_TL_CNT(z02_jnl_nblks_sb_meta, 1);

		fill_tagblk(tx, bh->b_blocknr, 1);
		clear_buffer_dirty(bh);
		unlock_buffer(bh);
		list_del_init(pos);
	}
	pthread_spin_unlock(&sb->s_mapping.private_lock);
}

/**
 * @brief metadata : inodes, bitmap, superblock 
 */
static void snapshot_inode_blocks(journal_tx *tx)
{
	struct buffer_head *bh;
	struct list_head *pos, *n;

	list_for_each_safe (pos, n, &tx->j_blks) {
		bh = list_entry(pos, struct buffer_head, j_blk_list);
		lock_buffer(bh);
		copy_blk(tx, bh->b_data, 1);

		g_total_blks_inode++;
		PF_TL_CNT(z04_jnl_nblks_inode, 1);

		fill_tagblk(tx, bh->b_blocknr, 1);
		clear_buffer_dirty(bh);

		oxbow_assert(buffer_injournal(bh)); // It should be always true.
		clear_buffer_injournal(bh);

		unlock_buffer(bh);
		list_del_init(pos);
	}
}
/**
 * @brief File indexing delay allocation occurs here, 
 *        If inode modified, reflect to cache (not on journal for here)
 *
 * @return int (success on 0)
 */
static int journal_reg_inode(journal_tx *tx, struct inode *inode,
			     struct buffer_extent *be)
{
	struct buffer_head *bh, *bh_tmp;
	pthread_spinlock_t *lock;
	struct address_space *mapping;
	struct list_head tmp;
	loff_t old;

	mapping = inode->i_mapping;
	lock = &mapping->private_lock;

	INIT_LIST_HEAD(&tmp);

	// This function must be called on holding shm lock.
	jnl_debug("[%s] inode(%d)", __func__, inode->i_ino);

	if (inode->i_size != get_recent_isize(inode)) {
		old = inode->i_size;
		inode->i_size = get_recent_isize(inode);
		illufs_set_i_size(inode);
		inode->i_state |= I_DIRTY;

		/* alloc block if we needed */
		PF_TL_START(cacabs_alloc_blocks);
		if (inode_alloc_blocks(inode, be, old))
			return -1;
		PF_TL_END(cacabs_alloc_blocks);
	}

	/* Write dirty modified inode to its block cache */
	PF_TL_START(cacabu_write_inode);
	if (inode->i_state & I_DIRTY) {
		PF_TL_START(cacabt_write_inode);
		if (inode->i_sb->s_op->write_inode(inode))
			return -1;
		PF_TL_END(cacabt_write_inode);
	}
	PF_TL_END(cacabu_write_inode);

	/* dirty blocks, mostly indexing block of file */
	PF_TL_START(cacabv_dirty_blocks);
	pthread_spin_lock(lock);

	list_for_each_entry_safe (bh, bh_tmp, &mapping->private_list, elem) {
		list_del_init(&bh->elem);
		bh->b_assoc_map = NULL;

		if (buffer_dirty(bh)) {
			list_add_tail(&bh->elem, &tmp);
			bh->b_assoc_map = mapping;

			pthread_spin_unlock(lock);

			lock_buffer(bh);
			copy_blk(tx, bh->b_data, 1);

			g_total_blks_inode_meta++;
			PF_TL_CNT(z03_jnl_nblks_inode_meta, 1);

			fill_tagblk(tx, bh->b_blocknr, 1);
			clear_buffer_dirty(bh);
			unlock_buffer(bh);

			// inode blocks should not be handled here.
			oxbow_assert(!buffer_injournal(bh));

			pthread_spin_lock(lock);
		}
	}

	while (!list_empty(&tmp)) {
		bh = list_entry(tmp.prev, struct buffer_head, elem);
		mapping = bh->b_assoc_map;
		list_del_init(&bh->elem);
		bh->b_assoc_map = NULL;
		if (buffer_dirty(bh)) {
			list_add_tail(&bh->elem, &mapping->private_list);
			bh->b_assoc_map = mapping;
		}
		pthread_spin_unlock(lock);
		brelse(bh);
		pthread_spin_lock(lock);
	}

	pthread_spin_unlock(lock);

	PF_TL_END(cacabv_dirty_blocks);

	jnl_debug("[%s] inode(%d) done", __func__, inode->i_ino);

	return 0;
}

/**
 * @brief Journal staged infos not to lose modified data
 * 
 * @return int (success on 0)
 */
static int journal_stage_trace(journal_tx *tx, struct list_head *e,
			       struct list_head *head)
{
	stage_tx *stx;

	jnl_debug("[%s] exist", __func__);

next_blk:
	jnl_debug("[TX_META] stage trace blk at index %lu",
		get_cur_mdblk_index(tx));

	tx->trace_blk = reserve_mdblk(tx, 1);
	tx->trace_blk->h.nr = 0;
	tx->trace_blk->h.blocktype = STAGE_TRACE_BLK;
	tx->desc_blk->h.nr_stage_trace_blks++;

	PF_TL_CNT(z06_stg_n_trace_blks, 1);

	for (;;) {
		stx = list_entry(e, stage_tx, elem);

		// Do we sync with staging here? May no due to inode lock

		tx->trace_blk->tx_list[tx->trace_blk->h.nr] = stx->start;

		// jrnl_debug(
		// 	"stage desc block (%lu) is added to stage trace blk at id=%u",
		// 	stx->start, tx->trace_blk->h.nr);

		tx->trace_blk->h.nr++;

		/* Last staged stx */
		if (list_is_last(e, head))
			break;

		/* Need to allocate a new trace block */
		if (tx->trace_blk->h.nr == JOURNAL_TRACE_MAX) {
			jnl_debug("trace max");
			g_total_stg_tx_cnt += tx->trace_blk->h.nr;
			PF_TL_CNT(z07_stg_tx_cnt, tx->trace_blk->h.nr);
			print_stage_trace_block(tx->trace_blk);

			e = e->next;
			goto next_blk;
		}

		e = e->next;
	}

	oxbow_assert(tx->trace_blk->h.nr > 0);
	g_total_stg_tx_cnt += tx->trace_blk->h.nr;
	PF_TL_CNT(z07_stg_tx_cnt, tx->trace_blk->h.nr);

	print_stage_trace_block(tx->trace_blk);

	jnl_debug(
		"[%s] exist done. Total stage trace blks=%u, Total stage TXs: %lu",
		__func__, tx->desc_blk->h.nr_stage_trace_blks,
		g_total_stg_tx_cnt);
	return 0;
}

static void print_ird_contents(struct inode_range_dirty *ird)
{
	jnl_warn("--- Printing ird contents ---");
	jnl_warn("ird initial: count=%d, dirty_blk_nr=%d", ird->count, ird->dirty_blk_nr);
	struct inode_range_dirty *current_ird = ird;
	int list_idx = 0;
	while (!is_dirty_list_empty(current_ird)) {
		jnl_warn("  ird list[%d]: count=%d", list_idx, current_ird->count);
		for (uint32_t page_idx = 0; page_idx < current_ird->count; page_idx++) {
			if (current_ird->dirty_pages[page_idx].nr > 0) {
				jnl_warn("    dirty_pages[%d]: start=%lu, nr=%lu",
					 page_idx,
					 current_ird->dirty_pages[page_idx].start,
					 current_ird->dirty_pages[page_idx].nr);
			}
		}
		current_ird = current_ird->next;
		list_idx++;
	}
	jnl_warn("--- End of ird contents ---");
}

static void journal_file_data(journal_tx *tx, struct inode *inode,
			      struct buffer_extent *be)
{
	struct inode_range_dirty *ird;
	bit_index_t start, tmp, cnt, split;
	uint32_t i;
	int ret;
	// long nr_irds = -1;
	uint64_t total_irds = 0;

	ird = inode->i_journal_dirty_start;

next_ird:
	// nr_irds++;
	for (i = 0; i < ird->count; i++) {
		start = ird->dirty_pages[i].start;
		cnt = ird->dirty_pages[i].nr;

		if (!cnt)
			continue;
		jnl_debug("[%s] inode(%d) start(%lu) cnt(%lu)", __func__,
			  inode->i_ino, start, cnt);

		total_irds++;
		// oxb_info(
		// 	"IRD: inode(%d) ird[%u] count=%u dirty_pages[%u] start(%lu) cnt(%lu) total_irds=%lu",
		// 	inode->i_ino, nr_irds, ird->count, i, start, cnt, total_irds);

		/* Contiguous data can be larger than rdma buffer */
		tmp = split = 0;
		if (cnt > tx->nr_blocks) {
		split:
			split = tx->nr_blocks;

			// PF_TL_START(caccm_copy_data_blks);
			copy_data_blks(inode, tx, start + tmp, split);
			// PF_TL_END(caccm_copy_data_blks);

			tmp += split;
			if (cnt - tmp > tx->nr_blocks)
				goto split;
		}
		if (cnt > tmp) {
			// PF_TL_START(caccm_copy_data_blks);
			copy_data_blks(inode, tx, start + tmp, cnt - tmp);
			// PF_TL_END(caccm_copy_data_blks);
		}

		// PF_TL_START(caccn_idx_lock_wait);
		inode_idx_lock_shared(inode);
		// PF_TL_END(caccn_idx_lock_wait);

		// PF_TL_START(cacco_get_blocks);
		ret = inode->i_op->get_blocks(inode, be, start, 0);
		// PF_TL_END(cacco_get_blocks);

		inode_idx_unlock(inode);
		if (ret) {
			oxb_error("ino(%lu) getblk fail", inode->i_ino);
			return;
		}

		/* Fill the tags (data lba) in transaction */
	retry:
		if (start + cnt <= be->iblock + be->nr) {
			fill_tagblk(tx, be->lba + start - be->iblock, cnt);
			cnt = 0;
		} else if (start < be->iblock + be->nr) {
			tmp = be->iblock + be->nr - start;
			fill_tagblk(tx, be->lba + start - be->iblock, tmp);
			cnt -= tmp;
			start += tmp;
		}

		if (cnt != 0) {
			BUG_ON((long)cnt < 0, "overflow");

			// PF_TL_START(caccn_idx_lock_wait);
			inode_idx_lock_shared(inode);
			// PF_TL_END(caccn_idx_lock_wait);

			// PF_TL_START(cacco_get_blocks);
			ret = inode->i_op->get_blocks(inode, be, start, 0);
			// PF_TL_END(cacco_get_blocks);

			inode_idx_unlock(inode);
			if (ret) {
				oxb_error("ino(%lu) getblk fail", inode->i_ino);
				return;
			}
			goto retry;
		}

		// Clear entry.
		ird->dirty_pages[i].start = 0;
		ird->dirty_pages[i].nr = 0;
	}

	// clear;
	ird->count = 0; // stage will handle this at inode_get_dirty_blocks_nr()

	if (ird->next && ird->next->count) {
		ird = ird->next;
		goto next_ird;
	}

	// oxb_warn("IRD: inode(%d) TOTAL_irds=%lu", inode->i_ino, total_irds);
}

/**
 * @brief Clear dirty waiting list of inodes.
 * 
 * @param inode 
 * @param df_id 
 */
static void clear_waiting_dirty_list_of_inode(struct inode *inode, int df_id)
{
	jnl_debug("Remove dirty waiting list. inode=%lu df_id=%d", inode->i_ino,
		  df_id);

	free_ird_except_first(inode->i_journal_waiting_dirty_start[df_id]);
}

/**
 * @brief Clear all waiting dirty list in the transaction.
 * 
 * @param tx 
 */
void clear_waiting_dirty_lists(journal_tx *tx)
{
	struct inode *inode;
	struct list_head *pos, *n;
	// int wd_txid;

	list_for_each_safe (pos, n, &tx->j_files_waiting) {
		inode = list_entry(pos, struct inode, i_journal_waiting_lists[tx->df_id]);

#if (STG_WAIT_MODE == ALWAYS_WAIT)
		/* Acquire the condvar mutex first, then the spinlock to avoid
		 * blocking while holding the spinlock and to keep lock ordering consistent. */
		pthread_mutex_lock(&inode->i_journal_waiting_dirty_mutex[tx->df_id]);
#endif
		pthread_spin_lock(&inode->i_journal_waiting_dirty_lock[tx->df_id]);

		// TMP: For validation.
		// wd_txid = atomic_load(
		// 		&inode->i_journal_waiting_dirty_txid[tx->df_id]);
		// oxbow_assert(wd_txid == (int)tx->tid || wd_txid == -1);

		// Update waiting dirty list tx id. It is checked in the sync
		// context before persisting the waiting dirty data.
		atomic_store(&inode->i_journal_waiting_dirty_txid[tx->df_id], -1);

		if (!is_dirty_waiting_list_empty(inode, tx->df_id))
			clear_waiting_dirty_list_of_inode(inode, tx->df_id);

		inode->i_journal_waiting_nr_dirty_blks[tx->df_id] = 0;
		inode->i_journal_waiting_nr_ird_blks[tx->df_id] = 0;

		// Remove the inode from the waiting file list.
		list_del_init(&inode->i_journal_waiting_lists[tx->df_id]);

		pthread_spin_unlock(&inode->i_journal_waiting_dirty_lock[tx->df_id]);

#if (STG_WAIT_MODE == ALWAYS_WAIT)
		// Wake up any threads waiting for this tx to complete.
		pthread_cond_broadcast(&inode->i_journal_waiting_dirty_cond[tx->df_id]);
		pthread_mutex_unlock(&inode->i_journal_waiting_dirty_mutex[tx->df_id]);
#endif
	}
}

/**
 * @brief Gather dirty page bits.
 * Waiting dirty list is built while gathering dirty pagebits.
 * 
 * @param inode 
 * @param df_id 
 */
static void build_dirty_lists(struct inode *inode, int df_id)
{
	pthread_spin_lock(&inode->i_journal_waiting_dirty_lock[df_id]);
	inode_gather_dirty_pagebits(inode, 1, df_id);
	pthread_spin_unlock(&inode->i_journal_waiting_dirty_lock[df_id]);
}

/**
 * @brief Build transaction with dirty regular file.
 * 
 * @return int (success on 0)
 */
static int build_jtx_regular_file(struct inode *inode, journal_tx *tx)
{
	struct buffer_extent be = { 0 };
	struct list_head *stage;
	int ret = -1;

	jnl_debug("[%s] inode(%d)", __func__, inode->i_ino);

	inode_lock(inode);
	if (inode->i_state & I_DELETED || !(inode->i_state & I_HAS_WORKER)) {
		list_del_init(&inode->i_journal_waiting_lists[tx->df_id]);
		inode_unlock(inode);
		iput(inode);
		return 0;
	}

	if (!inode->shm_header) {
		if (inode->i_state & I_FAILED) {
			inode_unlock(inode);
			oxb_error("inode(%d) failed", inode->i_ino);
			goto ret;
		}
		oxb_info("shm not init yet... inode(%lu)", inode->i_ino);
		inode_unlock(inode);
		return 0; // Retry
	}
	inode_unlock(inode);

	/* Staging a file, first lock the file and stage the states 
	 * [1] [File metadata part] */

	PF_TL_START(caca_jtx_reg_jnl_meta);

	PF_TL_START(cacaa_shinode_lock_wait);
	shared_inode_lock(inode);
	PF_TL_END(cacaa_shinode_lock_wait);

	inode_lock(inode);

	/* Cut stage list so that after unlock, stage can be inserted */
	// NOTE: It is okay to include the stage tx of the next running journal
	//  tx. I.e., stage tx that is ready between start_transaction() and
	//  shared_inode_lock().
	if (list_empty(&inode->i_stage_list)) {
		stage = NULL;
	} else {

		stage = inode->i_stage_list.next;
		INIT_LIST_HEAD(&inode->i_stage_list);
	}

	// Nothing to do. (No dirty data and no stage tx)
	if (!(inode->i_state & I_IN_JTX_DIRTY_LIST) && !stage) {
		oxb_error("[%s] inode(%d) not dirty and no stage tx.", __func__,
			  inode->i_ino);
		list_del_init(&inode->i_journal_waiting_lists[tx->df_id]);
		inode_unlock(inode);
		shared_inode_unlock(inode);
		goto ret;
	}

	inode_unlock(inode);

	PF_TL_START(cacab_reg_inode);
	if (journal_reg_inode(tx, inode, &be)) {
		// Error occurred.
		oxb_error("[%s] ino(%d) reg fail", __func__, inode->i_ino);
		shared_inode_unlock(inode);
		goto ret;
	}
	PF_TL_END(cacab_reg_inode);

	if (!(inode->i_state & I_HAS_WORKER)) {
		list_del_init(&inode->i_journal_waiting_lists[tx->df_id]);
		iput(inode);
		panic("Has no worker");
		return 0;
	}

	// A dirty waiting list must empty and tx id must be -1.
	oxbow_assert(is_dirty_waiting_list_empty(inode, tx->df_id));
	oxbow_assert(atomic_load(&inode->i_journal_waiting_dirty_txid[tx->df_id]) == -1);

	// Gathering dirty page bits and building waiting dirty list.
	PF_TL_START(cacad_gather_dirty);
	build_dirty_lists(inode, tx->df_id);
	PF_TL_END(cacad_gather_dirty);

	// Update waiting dirty list tx id if the list is not empty.
	if (!is_dirty_waiting_list_empty(inode, tx->df_id)) {
		// Calculate the number of blocks to store ird list.
		inode->i_journal_waiting_nr_ird_blks[tx->df_id] =
			count_ird_dump_blocks(
				inode->i_journal_waiting_dirty_start[tx->df_id]);

		// oxb_info("inode(%lu) nr_ird_blks(%u)", inode->i_ino,
		// 	 inode->i_journal_waiting_nr_ird_blks[tx->df_id]);

		atomic_store(&inode->i_journal_waiting_dirty_txid[tx->df_id], tx->tid);
	}

	inode_lock(inode);

	// Clear dirty bit. Should be done holding shared inode lock.
	// NOTE: Do not clear bits if the inode has been added to the next journal
	// transaction (higher value than current tx id).
	uint32_t j_txid = atomic_load(&inode->i_journal_txid);
	if (j_txid == tx->tid) {
		inode->i_state &= ~I_IN_JTX_DIRTY_LIST;
		// oxb_warn(
		// 	"[j_files BIT CLEAR] (dirty) inode=%d to tx_id=%u (i_journal_txid=%u)",
		// 	inode->i_ino, tx->tid, j_txid);
	}

	inode_unlock(inode);

	inode_unset_shm_state(inode, SHM_RUNNING_TX);

	shared_inode_unlock(inode);

	PF_TL_END(caca_jtx_reg_jnl_meta);

	/* [2] If previously fsynced transactions (staging) exist. */
	if (stage) {
		// oxb_debug("STAGE TX FOUND. inode=%d", inode->i_ino);

		// Save for freeing stage txs in msg handler.
		tx->stage_txs = stage;
		tx->head = &inode->i_stage_list;

		PF_TL_START(cacb_jtx_reg_jnl_stage);
		journal_stage_trace(tx, stage, &inode->i_stage_list);
		PF_TL_END(cacb_jtx_reg_jnl_stage);

	}

	/* [3] [File data part] */
	PF_TL_START(cacc_jtx_reg_jnl_file);
	journal_file_data(tx, inode, &be);
	PF_TL_END(cacc_jtx_reg_jnl_file);

	// oxb_info("inode(%lu) nr_etag_blks=%u total_tags=%u", inode->i_ino,
	// 	 tx->desc_blk->h.nr_etag_blks,
	// 	 tx->desc_blk->h.nr_tags +
	// 		 (tx->ext_blk ? tx->ext_blk->h.nr : 0) +
	// 		 (tx->desc_blk->h.nr_etag_blks - 1) * JOURNAL_ETAG_MAX);

	// On success
	iput(inode);
	ret = 0;

ret:
	return ret;
}

static int build_jtx_directory(struct inode *dir, journal_tx *tx)
{
	struct buffer_head *bh;
	int ret = -1;

	// log_info("[%s] inode(%d)", __func__, dir->i_ino);

	inode_lock(dir);
	if (dir->i_state & I_DELETED) {
		inode_unlock(dir);
		iput(dir);
		return 0;
	}
	inode_unlock(dir);

	/* [1] [File metadata part] */
	dir_inode_lock(dir);

	/* stage inode itself and other related blocks */
	if (dir->i_state & I_DIRTY)
		if (dir->i_sb->s_op->write_inode(dir))
			goto err_unlock;

	dir->i_state &= ~I_IN_JTX_DIRTY_LIST;

	/* dirty blocks, mostly indexing block of file */
	/* directory entry blocks */
	pthread_spin_lock(&dir->i_mapping->private_lock);
	list_for_each_entry (bh, &dir->i_mapping->private_list, elem) {
		if (buffer_dirty(bh)) {
			copy_blk(tx, bh->b_data, 1);

			g_total_blks_dirent++;
			PF_TL_CNT(z05_jnl_nblks_dirent, 1);

			fill_tagblk(tx, bh->b_blocknr, 1);
			clear_buffer_dirty(bh);
		}
	}
	pthread_spin_unlock(&dir->i_mapping->private_lock);
	// TODO: change spin lock

	// TODO: fine-grain locking required
	dir_inode_unlock(dir);

	ret = 0; // on success
	goto ret;

err_unlock:
	dir_inode_unlock(dir);
ret:
	// iput(dir);
	return ret;
}

/**
 * @brief Journal dirty file on this transaction.
 * 
 * @return int (success on 0)
 */
static int build_tx_from_file(struct inode *inode, journal_tx *tr)
{
	// // TODO: make a new tag
	// if (inode->i_state & I_DELETED)

	if (S_ISREG(inode->i_mode))
		return build_jtx_regular_file(inode, tr);
	else if (S_ISDIR(inode->i_mode))
		return build_jtx_directory(inode, tr);
	else {
		oxb_error("not supported mode inode(%d)", inode->i_ino);
		panic("not supported mode inode");
		return -1;
	}
}

/**
 * @brief Discard the transaction because there is nothing to commit. (There is
 * only a descriptor block.)
 * 
 * @param tr 
 */
static void discard_transaction(struct journal_control_ctx *j, journal_tx *tx)
{
	// Free data fetcher buffers.
	df_free_buffer(g_df_ctxs[tx->df_id], tx->df_md_buf_id);
	df_free_buffer(g_df_ctxs[tx->df_id], tx->df_buf_id);

	// Revoke tid.
	j->front->tid--;
	j->j_transaction_sequence--;
}

/**
 * @brief build memory chunk for transaction sent to devFS
 * 
 */
static journal_tx *build_transaction(struct journal_control_ctx *j)
{
	journal_tx *tx, *next;

	// Reset counters.
	g_total_blks_data = 0;
	g_total_blks_sb_meta = 0;
	g_total_blks_inode_meta = 0;
	g_total_blks_inode = 0;
	g_total_blks_dirent = 0;
	g_total_stg_tx_cnt = 0;

	tx = j->front;

	jnl_debug("[%s] tx(%d)", __func__, tx->tid);

	oxbow_assert(tx);

	if (tx_alloc_meta_buffer(tx)) {
		oxb_warn(
			"[BUILD TX] No available data fetcher buffer. Pass this time.");
		return NULL;
	}

	if (tx_alloc_buffer(tx)) {
		oxb_warn(
			"[BUILD TX] No available data fetcher buffer. Pass this time.");
		df_free_buffer(g_df_ctxs[tx->df_id], tx->df_md_buf_id);
		return NULL;
	}

	d_trace("[TX_META] desc blk at index %lu", get_cur_mdblk_index(tx));
	tx->desc_blk = reserve_mdblk(tx, 1);
	tx->ext_blk = NULL;

	tx->desc_blk->h.blocktype = DESC_BLK;
	tx->desc_blk->h.transaction_id = tx->tid;
	tx->desc_blk->h.nr_tags = 0;
	tx->desc_blk->h.nr_etag_blks = 0;
	tx->desc_blk->h.nr_stage_trace_blks = 0;

	next = new_transaction(j);
	if (!next)
		return NULL;

	tr_change_state(tx, TR_LOCKED);


	// We don't have to wait operations in the journal to be finished.
	// We have to get each of file's inode lock later.
	PF_TL_START(caa_wait_ref_down);

	while (atomic_load(&tx->ref) != 0)
		; // TODO: pre-snapshot while waiting for ref down

	PF_TL_END(caa_wait_ref_down);


	// I wonder this is necessary.
	// If no other operation modifies file system metadata, we don't need this.
	PF_TL_START(y3_fs_wrlock_wait);
	FS_LOCK(tx->sb);
	PF_TL_END(y3_fs_wrlock_wait);

	PF_TL_START(y1_fs_wrlockup_time);

	/* Now other operation can do its job except some operations (e.g. creat) */
	start_transaction(j, next);
	j->back = tx;
	next->sb = tx->sb;

	/* Snapshot all dirtied metadata blocks, superblock bitmaps... 
	 * Block allocated on inode_push_journal will be added to next tx */
	PF_TL_START(cab_snapshot_meta);
	snapshot_metadata_blocks(tx);
	PF_TL_END(cab_snapshot_meta);

	PF_TL_END(y1_fs_wrlockup_time);

	FS_UNLOCK(tx->sb);

	/* stage to build transaction */
	tr_change_state(tx, TR_BUILD);

	// TOCHECK: Does it take long time?
	// BUG_ON(atomic_load(&tx->ref) != 0, "get trans ptr during Locked");
	BUG_ON(!list_empty(&tx->j_blks), "must empty");

	// No need to hold j_lock as this tx is in TR_BUILD state.

	// Copy j_files to j_files_waiting.
	uint64_t inode_ptr;
	kh_foreach_set(tx->j_files_set, inode_ptr, {
		struct inode *inode = (struct inode *)inode_ptr;

		// Add to waiting list.
		list_add_tail(&inode->i_journal_waiting_lists[tx->df_id],
			      &tx->j_files_waiting);

		// oxb_debug("[j_files LIST COPY] tx_id=%d inode=%d", tx->tid,
		// 	  inode->i_ino);
	});

	/* build journal by iterating file array */
	PF_TL_START(cac_snapshot_file);
	kh_foreach_set(tx->j_files_set, inode_ptr, {
		struct inode *inode = (struct inode *)inode_ptr;

		// oxb_warn("[j_files LIST BUILD] tx_id=%d inode=%d", tx->tid,
		// 	 inode->i_ino);

		if (build_tx_from_file(inode, tx)) {
			oxb_error("fail to build");
			panic("Failed to build journal tx");
			return NULL;
		}
	});

	PF_TL_END(cac_snapshot_file);

	/* lastly inode itself (added to transaction's journal blocks list) */
	PF_TL_START(cad_snapshot_inode);
	snapshot_inode_blocks(tx);
	PF_TL_END(cad_snapshot_inode);

	tr_change_state(tx, TR_COMMIT);

	// Nothing to commit.
	if (tx->used_blk_cnt == 0 && tx->trace_blk == NULL) {
		jnl_debug("[%s] tx(%d) nothing to commit", __func__, tx->tid);
		discard_transaction(j, tx);
		free_transaction(tx);
		tx = NULL;
	} else {
		jnl_info("Commit tx(%d)", tx->tid);
		jnl_info("Data fetcher id: %d", tx->df_id);
		jnl_info(
			"  - Descriptor blk: contains %lu tags, %lu etag blks, %lu stage trace blks",
			tx->desc_blk->h.nr_tags, tx->desc_blk->h.nr_etag_blks,
			tx->desc_blk->h.nr_stage_trace_blks);
		jnl_info("  - Data buffer cnt:   %d",
			 (tx->used_blk_cnt - tx->nr_sent_blks == 0) ?
				 0 :
				 tx->n_df_bufs);
		jnl_info("  - Total stage TXs:   %lu", g_total_stg_tx_cnt);
		jnl_info("  - Total used blocks: %lu/%lu (%lu MB, %lu KB)",
			 tx->used_blk_cnt, tx->tot_nr_blocks,
			 tx->used_blk_cnt / 256, (tx->used_blk_cnt * 4));
		jnl_info(
			"    - Data: %lu, SB meta: %lu, Inode meta: %lu, Inode: %lu, Dirent: %lu",
			g_total_blks_data, g_total_blks_sb_meta,
			g_total_blks_inode_meta, g_total_blks_inode,
			g_total_blks_dirent);
	}

	return tx;
}

// TODO: Send metadata and the last data buf.

/**
 * @brief Send descriptor block, etag blocks, and the remaining data blocks.
 * Also, request to commit the transaction.
 * 
 * @param tx 
 * @return int (success on 0)
 */
static int request_commit(journal_tx *tx)
{
	size_t total_data_size, md_size;
	int ret;

	// 1. Request to fetch data buf (data not sent yet).
	ret = request_data_fetch(tx);

	if (ret < 0) {
		oxb_info("No data to send. Free the buffer.");

		// Free df buffer.
		df_free_buffer(g_df_ctxs[tx->df_id], tx->df_buf_id);

		// Revoke meta.
		tx->cur = NULL; // FIXME: Do we need this?
		tx->n_df_bufs--;
		tx->nr_blocks = 0;
		tx->tot_nr_blocks -= BG_JOURNAL_COMMIT_SIZE / OXBOW_BLOCK_SIZE;
	}

	// 2. Request to fetch metadata buf and to commit.
	total_data_size = tx->used_blk_cnt * OXBOW_BLOCK_SIZE;
	md_size = BG_JOURNAL_COMMIT_SIZE - tx->md_nr_blocks * OXBOW_BLOCK_SIZE;

	// Data Fetcher RDMA buffer is released on RPC callback (rpc_rdma_client_handler()).
	msg_send_devfs_journal(tx->df_id, tx->df_md_buf_id, md_size, total_data_size,
			       tx->n_df_bufs, (char *)tx, tx->tid);
	return ret;
}

void free_stage_txs(journal_tx *jtx)
{
	stage_tx *stx;
	struct list_head *e, *head;

	if (!jtx->stage_txs)
		return;

	e = jtx->stage_txs;
	head = jtx->head;

	// We cannot use list_for_each_safe() because this list is cut from
	// the original head (inode->i_stage_list).
	for (;;) {
		uint32_t cnt = 0;

		stx = list_entry(e, stage_tx, elem);

		// Wait for stage tx to be completed.
		while (!atomic_load(&stx->is_completed)) {
			usleep(10);
			cnt++;
			if (cnt % 10000 == 0)
				oxb_warn("Waiting stage tx to complete. stx=%p", stx);
		}

		/* Last staged stx */
		if (list_is_last(e, head)){
			free(stx->slot_busy);
			free(stx->slot_done);
			free(stx);
			break;
		}

		e = e->next;
		free(stx->slot_busy);
		free(stx->slot_done);
		free(stx);
	}
}

void free_transaction(journal_tx *tr)
{
	// Clean up hash set
	if (tr->j_files_set) {
		kh_destroy(inode_set, tr->j_files_set);
		// oxb_debug("[j_files DESTROY] destroy: tid=%u", tr->tid);
	}

	// Free stage txs.
	free_stage_txs(tr);

	free_data_fetcher(tr->df_id);
	free(tr);
}

static struct timespec last_check_time = { 0, 0 };

static void schedule_bg_commit(void)
{
	struct timespec current_time;
	long time_diff_ms;

	clock_gettime(CLOCK_MONOTONIC, &current_time);

	if (last_check_time.tv_sec == 0)
		last_check_time = current_time;

	while (1) {
#ifndef SLOW_BG_JOURNAL
		if (check_and_reset_global_dirty_threshold())
			goto ret;
#endif

		// Check if enough time has passed since last check
		time_diff_ms =
			(current_time.tv_sec - last_check_time.tv_sec) * 1000 +
			(current_time.tv_nsec - last_check_time.tv_nsec) /
				1000000;

		if (time_diff_ms > BG_JOURNAL_COMMIT_PERIOD) {
			// oxb_warn("Time to commit journal. (time_diff_ms=%ld)", time_diff_ms);
			goto ret;
		}

		usleep(BG_JOURNAL_SLEEP_TIME);
		clock_gettime(CLOCK_MONOTONIC, &current_time);
	}

ret:
	last_check_time = current_time;
	return;
}

static void *journal_worker(void *arg)
{
	struct journal_control_ctx *j;
	journal_tx *tr;
	j_worker_t *jw;
	sem_t sem;

	sem_init(&sem, 0, 0);

	jw = arg;
	j = jw->journal_s;

	/* journal wake up timing ==> set to prev transaction committed */
	while (1) {

#ifndef VM_ENV_NO_DEVFS
		PF_TL_START(c_bg_jnl);

		/* Build transaction based on files */
		PF_TL_START(ca_build_tx);
		tr = build_transaction(j);
		PF_TL_END(ca_build_tx);


		if (tr) {
#ifdef DEBUG_PRINT_JNL_TX
			// print_jnl_tx(tr, 0, ~0U);
			print_jnl_tx(tr, 0,
				     DUMP_EXT4_INODE_TABLE |
					     DUMP_EXT4_EXTENT_BLOCKS |
					     DUMP_ETAG_BLK | DUMP_DESC_BLK |
					     DUMP_COMMIT_BLK);
			// print_jnl_tx(tr, 0, DUMP_EXT4_INODE_TABLE|DUMP_EXT4_EXTENT_BLOCKS);
			// print_jnl_tx(tr, 0,
			// 	     DUMP_EXT4_INODE_TABLE |
			// 		     DUMP_EXT4_EXTENT_BLOCKS |
			// 		     DUMP_STAGE_DESC_BLK | DUMP_STAGE_TRACE_BLK);
#endif
			// We don't need to wait for the previous commit request
			// completed. The order of commit requests is guaranteed
			// in the DevFS's persist_meta_and_commit() function.
			// The wait time is now moved to the allocation of data
			// fetcher. (alloc_data_fetcher())
			request_commit(tr);
		}

		PF_TL_END(c_bg_jnl);
#endif
		// It should be called after calling build_transaction once to
		// avoid start up issue: write before calling build_transaction
		// causes an error.
		schedule_bg_commit();
	}

	return NULL;
}

static int init_journal_worker(struct journal_control_ctx *journal)
{
	j_worker_t *jw;
	pthread_t thr;

	jw = calloc(1, sizeof(*jw));
	if (!jw) {
		oxb_error("calloc fail");
		goto err;
	}

	jw->journal_s = journal;

	if (pthread_create(&thr, NULL, journal_worker, (void *)jw)) {
		perror("create pthread fail");
		goto free;
	};

	// Set the name for debugging
	pthread_setname_np(thr, "jnl_worker");

	return 0;

free:
	free(jw);
err:
	return -1;
}

int init_data_fetcher(void)
{
	int ret;

#if DATA_FETCHER_CHANNEL_MODE == 1
	ret = init_df_client(g_sd_conf.rpc_rdma_ip_addr,
			     g_sd_conf.data_fetcher_rdma_port,
			     DATA_FETCHER_BUF_SIZE, DATA_FETCHER_BUF_CNT,
			     &g_df_ctxs[0]);
	if (ret < 0 || g_df_ctxs[0] == NULL) {
		log_error("Failed to init client. ret=%d", ret);
		return -1;
	}

	ret = init_df_client(g_sd_conf.rpc_rdma_ip_addr,
			     g_sd_conf.data_fetcher_rdma_port_second,
			     DATA_FETCHER_BUF_SIZE, DATA_FETCHER_BUF_CNT,
			     &g_df_ctxs[1]);
	if (ret < 0 || g_df_ctxs[1] == NULL) {
		log_error("Failed to init client. ret=%d", ret);
		return -1;
	}

#elif DATA_FETCHER_CHANNEL_MODE == 2
	char shm_path[128];
	int i;

	for (i = 0; i < 2; i++) {
		// It should be same as the server's shm path.
		snprintf(shm_path, sizeof(shm_path), "%s_%d",
			 DATA_FETCHER_SHM_PATH, i);

		ret = init_df_client_shm(shm_path, DATA_FETCHER_BUF_SIZE,
					 DATA_FETCHER_BUF_CNT, &g_df_ctxs[i]);

		if (ret < 0 || g_df_ctxs[i] == NULL) {
			log_error("Failed to init df client %d. ret=%d", i,
				  ret);
			return -1;
		}
	}

#else
	log_error("Unknown data fetcher channel.");
	return -1;
#endif
	log_info("Client is connected to server.");
	return ret;
}

void exit_data_fetcher(void)
{
	oxb_info("Terminating Data Fetcher.");
	// destroy_df_client(g_df_ctx);
}

int init_journal(struct super_block *sb)
{
	journal_tx *tr;

	if (sb->s_op->fill_journal_sb(sb)) {
		oxb_error("fill super block fail");
		goto err;
	}

	if (!g_sd_conf.bg_journaling)
		oxb_warn("%s with journal off", __func__);

	g_journal_ctx = sb->journal;

	pthread_spin_init(&g_journal_ctx->lock, 0);

#ifdef PERFILE_SNAPSHOT
	snapshot_thpool = thpool_init(
		g_sd_conf.snapshot_thread_num, "snapshot_thpool");
#endif

	// TODO: It should be read from disk journal superblock.
	// For now, just initialize to 0.
	g_journal_ctx->mrc_tx_id = 0;

	tr = new_transaction(g_journal_ctx);
	if (!tr)
		goto err;

	start_transaction(g_journal_ctx, tr);
	g_journal_ctx->back = NULL;
	tr->sb = sb;

	// TODO: add configuration that disable background journaling.
	if (init_journal_worker(sb->journal))
		goto free_tr;

	oxb_info("init_journal success");
	return 0;

free_tr:
	free(tr);
err:
	return -1;
}
