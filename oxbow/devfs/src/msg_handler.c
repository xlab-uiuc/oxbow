#include "msg_handler.h"
#include "msg_op.h"
#include "log.h"
#include "journal.h"
#include "thpool.h"
#include "profile_devfs.h"
#include <semaphore.h>
#include <stdlib.h>

PF_TL_EVT(z1_bg_commits);
PF_TL_EVT(z2_fetch_reqs);
PF_TL_EVT(z3_data_buf_blks_persisted);
PF_TL_EVT(z4_meta_buf_blks_persisted);

/**
 * @brief 
 * 
 * @param df_id Data Fetcher id.
 * @param req_id 
 * @param buf_id 
 * @param size 
 * @return int 
 */
int handle_fetch_data(int df_id, int req_id, int buf_id, size_t size)
{
	char *read_buf;
	int ret = 0;

	oxb_debug(
		"[FETCH DATA] Requested: df_id=%d buf_id=%d data_size=%lu (%lu blks, %lu KB, %lu MB)",
		df_id, buf_id, size, bytes_to_nblks(size), size >> 10,
		size >> 20);

	// Fetch data
	PF_TL_START(evt_fetch_data);
	read_buf = fetch_data_from_host(df_id, buf_id, size);
	PF_TL_END(evt_fetch_data);

	if (!read_buf) {
		oxb_error("Failed to fetch data. buf_id=%d", buf_id);
		return -1;
	}

	ret = persist_data(df_id, req_id, read_buf, size);
	if (ret < 0) {
		oxb_error("Write back failed.");
		return -1;
	}

	PF_TL_CNT(z3_data_buf_blks_persisted, size >> OXBOW_BLOCK_SIZE_SHIFT);

	return 0;
}

/**
  * @brief 
  * 
  * @param df_id Data Fetcher id.
  * @param md_buf_id Data Fetcher buffer id for metadata (descriptor and etag blocks).
  * @param md_size The length of metadata (descriptor and etag blocks).
  * @param total_data_size The length of total data.
  * @param n_fetch_reqs 
  * @param tx_id tx id is stored here.
  * @return int 
  */
int handle_journal(int df_id, int md_buf_id, size_t md_size,
		   size_t total_data_size, int n_fetch_reqs, uint32_t *tx_id)
{
	char *read_buf;
	int ret = 0;
	struct journal_descriptor_block *jdh;

	PF_TL_START(evt_bg_journal);

	oxb_info(
		"[BG JOURNAL] Requested: df_id=%d md_buf_id=%d md_size=%lu (%u blks including desc_blk, %lu KB, %lu MB)\n"
		"\t\t\t\t\t\t total_data_size=%lu (%u blks, %lu KB, %lu MB) n_fetch_reqs=%d",
		df_id, md_buf_id, md_size, bytes_to_nblks(md_size),
		md_size >> 10, md_size >> 20, total_data_size,
		bytes_to_nblks(total_data_size), total_data_size >> 10,
		total_data_size >> 20, n_fetch_reqs);

	// Fetch metadata.
	PF_TL_START(evt_fetch_meta);
	read_buf = fetch_data_from_host(df_id, md_buf_id, md_size);
	PF_TL_END(evt_fetch_meta);

	if (!read_buf) {
		log_error("Failed to fetch metadata. df_id=%d buf_id=%d", df_id,
			  md_buf_id);
		return -1;
	}

	PF_TL_CNT(z1_bg_commits, 1);
	PF_TL_CNT(z2_fetch_reqs, n_fetch_reqs);
	PF_TL_CNT(z4_meta_buf_blks_persisted,
		  md_size >> OXBOW_BLOCK_SIZE_SHIFT);

	// Get tx_id for response. The first block is the descriptor block.
	jdh = (struct journal_descriptor_block *)read_buf;
	*tx_id = jdh->h.transaction_id;

	// Logging and commit.
	PF_TL_START(evt_commit);
	ret = persist_meta_and_commit(df_id, read_buf, md_size, total_data_size,
				      n_fetch_reqs);
	PF_TL_END(evt_commit);

	if (ret < 0) {
		log_error("Flush or commit failed.");
		return -1;
	}

	PF_TL_END(evt_bg_journal);

	return 0;
}

/**
 * @brief Handle stage area checkpoint request from secure daemon.
 *
 * Enqueue a checkpoint job to ckpt_thpool using start_stg_checkpoint().
 * For synchronous requests, wait until the job finishes before returning.
 *
 * @param stage_total_blks Total number of blocks in the stage area.
 * @param nr_blks_to_free Desired number of blocks to free.
 * @param is_sync 1: synchronous, 0: asynchronous.
 * @param nr_blks_freed Actual number of blocks freed (output).
 * @return 0 on success, negative on failure.
 */
int handle_stg_ckpt(uint32_t stage_total_blks, uint32_t nr_blks_to_free,
		    uint8_t is_sync, uint32_t *nr_blks_freed)
{
	struct stg_ckpt_arg *arg;
	sem_t done;

	oxb_info("[STG CKPT] Requested: stage_total_blks=%u nr_blks_to_free=%u is_sync=%u",
		 stage_total_blks, nr_blks_to_free, is_sync);

	arg = malloc(sizeof(*arg));
	if (!arg) {
		log_error("Failed to allocate stg_ckpt_arg.");
		return -1;
	}

	arg->nr_blks_to_free = nr_blks_to_free;
	atomic_init(&arg->nr_blks_freed, 0);
	sem_init(&done, 0, 0);
	arg->done = &done;

	thpool_add_work(ckpt_thpool, start_stg_checkpoint, (void *)arg);

	sem_wait(&done);
	sem_destroy(&done);
	*nr_blks_freed = atomic_load(&arg->nr_blks_freed);
	free(arg);

	return 0;
}
