#ifndef _JOURNAL_H_
#define _JOURNAL_H_

#include "oxbow.h"
#include "fs/fs.h"
#include "common/sjd.h"
#include "buffer_head.h"
#include "oxbow_debug.h"
#include "common/khash.h"
#include <pthread.h>
#include <stdatomic.h>
#include <stddef.h>

typedef unsigned int tid_t;
#define START_JOURNAL_TID 1

// Define hash set for inode pointers (using 64-bit integer set)
KHASH_SET_INIT_INT64(inode_set)

// TODO: Move the followings to env config
/* linux default background wb is 5 seconds */
#if defined(VM_ENV_NO_DEVFS) || defined(DISABLE_BACKGROUND_JOURNALING)
#define BG_JOURNAL_COMMIT_PERIOD 10000000 // 10000s 
#define BG_JOURNAL_SLEEP_TIME 10000000 // in us. 10s

#else

#ifdef SLOW_BG_JOURNAL
#define BG_JOURNAL_COMMIT_PERIOD 30000 // in ms. (30 seconds)
#else
#define BG_JOURNAL_COMMIT_PERIOD 1000 // in ms.
#endif
#define BG_JOURNAL_SLEEP_TIME 100000 // in us. 100ms

#endif

#define BG_JOURNAL_COMMIT_SIZE (DATA_FETCHER_BUF_SIZE)

/* global journal control structure */
struct journal_control_ctx {
	/* spin lock for transaction pointer */
	pthread_spinlock_t lock;

	/* pointer to transactions. Pipelining used to manage transactions.
	 * front is running transaction. all updates are reflected to this transaction.
	 * back is for build and commit it to device */
	journal_tx *front;
	journal_tx *back;

	tid_t j_transaction_sequence;

	/* most recent committed transactions id */
	tid_t mrc_tx_id;

	/* file stage context (circular buffer) */
	u32 stage_start;     // Head: next block to allocate.
	u32 stage_tail;      // Tail: earliest un-checkpointed block.
	u32 stage_gap_start; // Start of skipped gap at boundary (0 = no gap).
	u32 stage_end;       // The last block number + 1 of the stage area.
	u32 stage_total;     // The total number of blocks in the stage area.

	atomic_bool stg_ckpt_in_flight; // True if a ckpt request is pending.
};

struct journal_worker {
	/* main journal control structure */
	struct journal_control_ctx *journal_s;
};

struct sjd_rdma_buffer {
	void *current;

	/* left blocks of RDMA journal chunk */
	size_t left_blocks;
};

enum {
	TR_NEW = 0,
	TR_RUNNING,
	TR_LOCKED,
	TR_BUILD,
	TR_COMMIT,
	TR_COMPLETE,
	TR_PAUSE,
};

struct journal_transaction {
	struct super_block *sb;

	struct journal_transaction *prev;

	tid_t tid; // Transaction id

	/* journal lock */
	pthread_spinlock_t j_lock;

	/* file list to be committed. (dirty files + staged files) */
	khash_t(inode_set) *j_files_set;
	struct list_head j_files_waiting;

	/* dirtied block not file (filesys metadata) */
	struct list_head j_blks;

	atomic_uint ref; // reference counter (used in Locked phase)
	int state;

	/* Data buffer for RDMA states */
	char *cur; // Cursor pointer to reserve next block
	unsigned int nr_blocks; // # of block for this buffer (How many left)

	/* Metadata buffer for RDMA states */
	char *md_cur; // <--------- Use as a metadata buffer cursor. (desc blk, tag blks)
	unsigned int md_nr_blocks; // # of block for this buffer (How many left)
	int df_md_buf_id; // df buffer id (for metadata).
	void *df_md_rdma_buf; // df buffer (for metadata).

	/* Block infos */
	unsigned int used_blk_cnt; // # of blocks currently used for data
	unsigned int tot_nr_blocks; // total allocated # of blocks

	/* Data Fetcher buffer id array */
	int df_buf_id; // Current df buffer id (for data).

	/* The number of Data Fetcher buffer allocated. */
	/* Data buf id sent to devfs is calculated based on it. */
	int n_df_bufs;

	void *df_rdma_buf; // Current df buffer (for data).

	/* pointer to desc block and etag block */
	struct journal_descriptor_block *desc_blk;
	struct journal_extent_tag_block *ext_blk;
	struct journal_stage_trace_block *trace_blk;

	size_t nr_sent_blks; // Sent to DevFS.

	int df_id; // Data Fetcher ID. There are two Data Fetchers.

	/* Need to save for freeing stage txs. */
	struct list_head *stage_txs; // List of stage txs.
	struct list_head *head; // Head of the list. (to check the last)
};

struct stage_transaction {
	struct journal_control_ctx *j;

	baddr_t start; // Start block number of this Tx
	struct list_head elem; // listed in inode to preserve ordering
	bool stage_area_reserved; // Whether stage area is reserved. (For validation)

	/* SPDK(DMA) buffer states */
	/* sequence -> pre-allocated spdk buffer entry */
	char *cur; // Cursor pointer to DMA buffer (aligned by block size)
	char *desc_blk_buf; // 4KB DMA buffer for desc block.
	char *commit_blk_buf; // 4KB DMA buffer for commit block.
	int seq_idx; // current sequence index
	int seq_max;
	unsigned int nr_blocks; // # of block for this buffer (How many left)
	unsigned int nr_pending; // # of not issued yet
	unsigned int nr_issued; // # of issued blocks
	unsigned int nr_dirty_blks; // # of dirty blocks of the inode excluding waiting dirty blocks.
	unsigned int nr_waiting_dirty_data_blks[2]; // # of waiting dirty blocks. (data + IRD blocks)
	unsigned int nr_waiting_dirty_ird_blks[2]; // # of waiting dirty IRD blocks. (For validation)
	unsigned int nr_other_blks; // # of other blocks. (index, tag, etc.)

	/* pointer to desc block and etag block */
	// struct stage_commit_block *commit_blk; // [TODO] fill contents // Not used.
	struct journal_extent_tag_block *ext_blk;

	uint64_t data_blks_cnt; // # of data blocks persisted. (for stats)
	bool desc_blk_skipped; // Whether desc block is persisted.

	/* In-flight request accounting for fsync staging.
	 * req_issued_total must match req_completed_total before fsync returns.
	 */
	uint64_t req_issued_total;
	uint64_t req_completed_total;
	uint32_t req_issued_reclaim;
	uint32_t req_issued_tail_data;
	uint32_t req_issued_desc;
	uint32_t req_issued_commit;

	/* Sequence slot tracking (P0 groundwork).
	 * Slot allocation/reuse policy is in-order (head/tail).
	 */
	uint32_t slot_head;
	uint32_t slot_tail;
	uint32_t slot_capacity;
	uint32_t busy_slots;
	uint8_t *slot_busy;
	uint8_t *slot_done;
	bool io_error;

	struct stage_descriptor_block desc_blk_copy;

	// Whether this staging is completed.
	// It is used to check whether it is safe to free this stage_tx.
	atomic_bool is_completed;
	baddr_t commit_baddr;

	bool waiting_dirty_skipped[2]; // For validation. Whether waiting dirty list is skipped.
};

// Global data fetcher context.
extern struct data_fetcher_ctx *g_df_ctxs[2]; // There are two data fetchers.
extern struct journal_control_ctx *g_journal_ctx;

static inline u64 get_cur_mdblk_index(journal_tx *tx)
{
	return (tx->md_cur - (char *)tx->df_md_rdma_buf) / OXBOW_BLOCK_SIZE;
}

int init_journal(struct super_block *);
journal_tx *start_journal(struct journal_control_ctx *);
void end_journal(journal_tx *);
int add_journal_file(struct inode *);
int check_and_add_journal_file(struct inode *inode);
int add_journal_inode(struct buffer_head *);
// void add_journal_bh(struct buffer_head *);
int commit_journal(struct super_block *);

u32 get_stage_used_blks(struct journal_control_ctx *);
int get_stage_usage_pct(struct journal_control_ctx *, u32 used_blks);
void stage_free_blks(struct journal_control_ctx *, u32 nr_blks);

int init_data_fetcher(void);
void exit_data_fetcher(void);

void free_transaction(journal_tx *);
void clear_waiting_dirty_lists(journal_tx *tx);

#endif
