#ifndef _JOURNAL_H_
#define _JOURNAL_H_
#include <stdio.h>
#include <bits/pthreadtypes.h>
#include <stddef.h>
#include <stdint.h>
#include <pthread.h>
#include <semaphore.h>
#include <stdatomic.h>
#include "oxbow.h"
#include "sjd.h"
#include "thpool.h"
#include "list.h"

/* Commit header written to commit blocks. */
struct commit_header {
	struct journal_header common;
	// unsigned char h_chksum_type;
	// unsigned char h_chksum_size;
	// unsigned char h_padding[2];
	// uint32_t h_chksum[JBD2_CHECKSUM_BYTES];
	uint64_t h_commit_sec;
	uint32_t h_commit_nsec;
};

/**
 * @brief It describes journal log state.
 * |       |tx1-----|tx2-----|       |
 *         |tail             |head
 * seqn = 1
 */
struct journal_log {
	int df_id; // 0 or 1. Data fetcher id.

	/* Static */
	baddr_t first; // The first block number of log. (=sb.first)
	baddr_t last; // The last block number of log.

	/* Dynamic */
	baddr_t head; // head block number. (The next available block.) TODO: change to atomic?
	baddr_t tail; // tail block number. TODO: change to atomic?
	atomic_ulong n_free_blks; // # of free blocks in log.
	pthread_spinlock_t
		lock; //Exclusive access to head, tail, and n_free_blks.
};

/* In-memory journal state */
struct journal_ctx {
	struct journal_superblock sb;
	struct journal_log logs[2];
};

// FIXME: This data structure is temporary. It will be replaced with extent
// tree for coalescing.
// For now, we use simple arrays.
// 1. List of transaction metadata.
// 2. Each transaction metadata has an array of tags.
struct tx_meta {
	uint32_t tx_id;
	uint32_t st_blk_id; // To iterate.
	struct journal_descriptor_block desc_blk;
	char *meta_start; // Start address of meta blocks (etag & stage trace).
};

struct ckpt_list_entry {
	struct journal_log *j_log;
	struct list_head list;
	baddr_t j_start_baddr; // The first descriptor block address in journal. To free journal space.
	size_t total_tx_size; // Allocated journal size in bytes. To free journal space.
	struct tx_meta *
		tx_meta; // In-memory optimization. On recovery, it should be filled with on-disk data.
	atomic_bool ready; // All the pre-tasks are done.
};

/* Tag list struct (for coalescing in checkpointing) */
struct tag_list_node {
	char *buf;
	baddr_t start; /* start lba */
	baddr_t src_start_baddr;
	u32 cnt;
	int stage_tx;
	struct list_head list;
};

/**
 * @brief Get the fetch size.
 * 
 * @param df_buf_id The current Data Fetcher buffer id.
 * @param n_bufs The number of total Data Fetcher buffers in a transaction.
 * @param size Total size in bytes.
 * @return size_t 
 */
static inline size_t get_fetch_size(int df_buf_id, int n_bufs, size_t size)
{
	if (df_buf_id == n_bufs - 1)
		return size % DATA_FETCHER_BUF_SIZE;
	else
		return DATA_FETCHER_BUF_SIZE;
}

// Worker thread: RPC handler & io worker.
extern threadpool worker_thpool;

// Checkpoint thread pool.
extern threadpool ckpt_thpool;

// Global superblock loaded at init (from journal.c)
extern char g_sb_static[4096];

int init_journal(void);
void exit_journal(void);
char *fetch_data_from_host(int df_id, int df_buf_id, size_t size);
int persist_data(int df_id, int req_id, char *rdma_bufs, size_t size);
int persist_meta_and_commit(int df_id, char *rdma_buf, size_t md_size,
			    size_t total_size, int n_fetch_reqs);
void start_checkpoint(void *arg);

/* Argument struct for start_stg_checkpoint(). */
struct stg_ckpt_arg {
	uint32_t nr_blks_to_free;
	atomic_uint nr_blks_freed; /* Actual blocks freed (written by worker). */
	sem_t *done; /* Semaphore to post when done. */
};

void start_stg_checkpoint(void *arg);
void reset_jnr_ctx(void);

#endif
