#include "oxbow_debug.h"
#include "profile.h"
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <stdatomic.h>
#include "global.h"
#include "oxbow.h"
#include "config.h"
#include "log.h"
#include "thpool.h"
#include "data_fetcher.h"
#include "journal.h"
#include "storage_engine.h"
#include "common/journal_op.h"
#include "profile_devfs.h"
#include "common/utils/exp_flag.h"

#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <sys/syscall.h> /* For SYS_gettid */
#include <sys/types.h> /* For pid_t etc. */

#ifdef USE_NVME_STORAGE_ENGINE
#include "utils/sync_device.h"
#endif

#ifdef DEBUG_PRINT_JNL_TX
#include "utils/print_tx.h"
#endif

/* For debugging and testing.*/
// #define VALIDATE_STAGE_DESC_BLK

// Helper function to get thread name - forward declaration
// static const char *get_thread_name(void);

// #ifndef _GNU_SOURCE
// static int pthread_getname_np(pthread_t thread, char *name, size_t len)
// {
// 	// Simple fallback - just return empty string
// 	if (len > 0)
// 		name[0] = '\0';
// 	return 0;
// }
// #endif

// // Helper function to get thread name
// static const char *get_thread_name(void)
// {
// 	static __thread char thread_name[16] = {
// 		0
// 	}; // Thread-local buffer for thread name

// 	// Only try to get thread name once per thread
// 	if (thread_name[0] == '\0') {
// 		if (pthread_getname_np(pthread_self(), thread_name,
// 				       sizeof(thread_name)) != 0) {
// 			// Failed to get thread name, use TID instead
// 			snprintf(thread_name, sizeof(thread_name), "tid-%d",
// 				 gettid());
// 		}

// 		// If thread name is empty, use TID
// 		if (thread_name[0] == '\0') {
// 			snprintf(thread_name, sizeof(thread_name), "tid-%d",
// 				 gettid());
// 		}
// 	}

// 	return thread_name;
// }

/**
 * @brief Dumps memory content in a readable hexdump format
 * @param addr Starting address of memory to dump
 * @param size Number of bytes to dump (default 1024)
 */
static void dump_memory(void *addr, size_t size)
{
	unsigned char *p = (unsigned char *)addr;
	size_t i, j;
	char ascii[17];

	// Default to 1024 bytes if size is 0
	if (size == 0) {
		size = 1024;
	}

	ascii[16] = '\0';

	// Process 16 bytes per line
	for (i = 0; i < size; i += 16) {
		// Print address
		printf("%p: ", p + i);

		// Print hex values
		for (j = 0; j < 16; j++) {
			if (i + j < size) {
				printf("%02x ", p[i + j]);
				ascii[j] = (p[i + j] >= 32 && p[i + j] <= 126) ?
						   p[i + j] :
						   '.';
			} else {
				printf("   ");
				ascii[j] = ' ';
			}

			// Add extra space at the middle of the row
			if (j == 7) {
				printf(" ");
			}
		}

		// Print ASCII representation
		printf(" |%s|\n", ascii);
	}
}

PF_EVT(COMMIT_DATA_IO);
PF_TL_EVT(a_persist_data);

#define CKPT_THREAD_NUM 1 // Checkpoint must be in order.
#define CKPT_PREPARE_THREAD_NUM                                                \
	1 // It should be 1 to preserve the order of checkpointing.

// TODO: Find proper thresholds.
#define CKPT_THRESHOLD_FREE_TARGET                                             \
	80 // Checkpoint until 80% of journal space is free.
// #define CKPT_THRESHOLD_FREE_TRIGGER                                            \
// 	20 // Start checkpointing if free space is less than 20%.

// Reduced for experiments.
#define CKPT_THRESHOLD_FREE_TRIGGER                                            \
	2 // Start checkpointing if free space is less than 2%.

// #define ASYNC_DISPATCH

// #define VALIDATE_WORKER_IDS // Should be turned off for performance.

enum workerid_state {
	WRITE_DISPATCHED = -3,
	POLLED = -2,
	NOT_USED = -1,
	// non-negative values --> allocated worker id.
};

//-----------------------------------------------------------------------

/* Fetching data state */
struct fetch_ctx {
	atomic_uint req_cnt; /* The most recent request id.
				  * Requests are processed in ascending order.
				  */
	pthread_mutex_t req_mutex; /* Mutex for the request id. */
	pthread_cond_t req_cond; /* Condition variable for the request id. */
	atomic_ulong desc_blk_baddr; /* Address of the first block of log.
				 * It is filled when commit is requested.
				 */
	atomic_ulong total_log_size; // For validation.
};

struct superblock {
	char buf[4096]; // 1 block.
} __attribute__((aligned(OXBOW_BLOCK_SIZE)));

atomic_uint g_sb_loaded;

/* Global superblock loaded at init.
   WARN: this is not updated after loading.
   Use it only for looking up constants.  */
char g_sb_static[4096] __attribute__((aligned(4096))) = { 0 };

// Two identical data fetchers enable concurrent data io from two transactions.
struct data_fetcher_ctx *g_df_ctx[2];

char *g_df_buf[2]; // Allocate with spdk_dma_zmalloc to reduce memcopy.

struct journal_ctx g_journal_ctx = { 0 }; // Global journal state.

threadpool ckpt_thpool;
threadpool ckpt_prepare_thpool;
threadpool worker_thpool;

// Global transaction sequence number.
atomic_uint g_tx_seqn;
pthread_mutex_t g_tx_seqn_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t g_tx_seqn_cond = PTHREAD_COND_INITIALIZER;
baddr_t g_jsb_baddr; // A block address of journal superblock.
struct fetch_ctx g_fetch_ctx[2]; // State of fetching data blocks.

uint64_t g_total_stg_trace_blks_processed; // Total # of stage trace blocks processed.
uint64_t g_total_stg_txs_processed; // Total # of stage TXs processed.
uint64_t g_total_etag_blks_in_stg_tx_processed; // Total # of tag blocks (excluding desc) in stage tx.
uint64_t g_total_tags_in_stg_tx_processed; // Total # of tags in stage tx.
uint64_t g_total_blks_in_stg_tx_processed; // Total # of data, index, ... blocks processed per checkpoint. (stage tx)

uint64_t g_total_etag_blks_in_bg_tx_processed; // Total # of etag blocks in bg journal.
uint64_t g_total_tags_in_bg_tx_processed; // Total # of tags in bg journal.
uint64_t g_total_blks_in_bg_tx_processed; // Total # of data, index, ... blocks processed per checkpoint. (bg tx)

/**
 * A global checkpoint list (queue). A new committed tx is added to the tail.
 */
LIST_HEAD(g_ckpt_list);
pthread_spinlock_t g_ckpt_list_lock; // Lock for the g_ckpt_list.

static int journal_free_percent(struct journal_log *j_log);

// Add progress tracking variables at global scope
static uint64_t g_total_bytes_processed = 0;
static uint64_t g_last_report_bytes = 0;
static const uint64_t REPORT_INTERVAL = 100 * 1024 * 1024; // 100MB

/**
 * @brief Check whether wrap-around occurs in a log with the given start block
 * and size (n_blks).
 * 
 * @param start Start block address.
 * @param n_blks The number of blocks. (size)
 * @param last The last block address in log.
 * @return int 1 if wrap-around occurs. 0 otherwise.
 */
static int check_wrap_around(baddr_t start, uint64_t n_blks, baddr_t last)
{
	return start + n_blks - 1 > last;
}

static int check_wrap_around_stage_tx(baddr_t start, uint64_t n_blks)
{
	baddr_t last;

	last = g_j_ops->last_stage_log_blk(g_sb_static);
	return check_wrap_around(start, n_blks, last);
}

static int check_wrap_around_journal_tx(baddr_t start, uint64_t n_blks,
					struct journal_log *j_log)
{
	return check_wrap_around(start, n_blks, j_log->last);
}

/**
 * @brief Allocate tx_meta.
 *
 * @param n_meta_blks # of meta blocks (etag & stage trace).
 * @return struct tx_meta* Returns allocated tx_meta. Null on failure.
 */
static struct tx_meta *alloc_tx_meta(uint32_t n_meta_blks)
{
	struct tx_meta *tx_meta;

	tx_meta = calloc(1, sizeof(struct tx_meta));
	if (!tx_meta)
		return NULL;

	tx_meta->meta_start = malloc(n_meta_blks * OXBOW_BLOCK_SIZE);
	if (!tx_meta->meta_start)
		goto err1;

	return tx_meta;

err1:
	free(tx_meta);

	oxb_error("Memory allocation failed.");
	return NULL;
}

static void free_tx_meta(struct tx_meta *tx_meta)
{
	free(tx_meta->meta_start);
	free(tx_meta);
}

static struct ckpt_list_entry *alloc_ckpt_entry(uint32_t n_meta_blks)
{
	struct ckpt_list_entry *ce;

	ce = calloc(1, sizeof(struct ckpt_list_entry));

	if (!ce) {
		oxb_error("Memory allocation failed.");
		return NULL;
	}
	// init some of the ckpt list entry.
	INIT_LIST_HEAD(&ce->list);
	atomic_init(&ce->ready, false);

	ce->tx_meta = alloc_tx_meta(n_meta_blks);
	if (!ce->tx_meta) {
		oxb_error("Tx meta allocation failed.");
		free(ce);
		return NULL;
	}

	return ce;
}

static void free_ckpt_list_entry(struct ckpt_list_entry *ce)
{
	free_tx_meta(ce->tx_meta);
	free(ce);
}

int init_data_fetcher(void)
{
	int ret, i;

#if DATA_FETCHER_CHANNEL_MODE == 1
	int port;

	for (i = 0; i < 2; i++) {
		g_df_buf[i] = NULL;

		g_df_buf[i] = se_alloc_dma_buffer(DATA_FETCHER_BUF_SIZE *
						  DATA_FETCHER_BUF_CNT);
		if (!g_df_buf[i]) {
			oxb_error("Failed to allocate SPDK buffer.");
			exit(-1);
			return -1;
		}

		oxb_info("Data Fetcher buf[%d]: 0x%lx", i, g_df_buf[i]);

		if (i == 0) {
			port = g_devfs_conf.data_fetcher_rdma_port;
		} else {
			port = g_devfs_conf.data_fetcher_rdma_port_second;
		}

		ret = init_df_server(port, DATA_FETCHER_BUF_SIZE,
				     DATA_FETCHER_BUF_CNT, &g_df_ctx[i],
				     g_df_buf[i]);
		if (ret) {
			oxb_error(
				"Initializing Data Fetcher server failed. ret=%d",
				ret);
			return -1;
		}
	}

#elif DATA_FETCHER_CHANNEL_MODE == 2
	char shm_path[128];
	size_t aligned_size;

	// Align the size to 2MB boundary.
	aligned_size = (DATA_FETCHER_BUF_SIZE * DATA_FETCHER_BUF_CNT +
			(4096 * 512) - 1) &
		       ~(4096 * 512 - 1);

	for (i = 0; i < 2; i++) {
		// We allocate data fetcher buffers in shared memory first while
		// initializing data fetcher servers.
		// Then, we register the buffers to SPDK DMA region.

		// It should be same as the client's shm path.
		snprintf(shm_path, sizeof(shm_path), "%s_%d",
			 DATA_FETCHER_SHM_PATH, i);

		ret = init_df_server_shm(shm_path, DATA_FETCHER_BUF_SIZE,
					 DATA_FETCHER_BUF_CNT, &g_df_ctx[i]);

		if (ret) {
			oxb_error(
				"Initializing Data Fetcher server failed. ret=%d",
				ret);
			return -1;
		}

		g_df_buf[i] = df_get_shm_buf_base(g_df_ctx[i]);

		oxb_info("Data Fetcher path=%s offset=%lu buf[%d]=0x%lx",
			 shm_path, aligned_size * i, i, g_df_buf[i]);

		ret = se_register_buf_to_spdk(g_df_buf[i],
					      df_get_shm_buf_size(g_df_ctx[i]));
		if (ret) {
			oxb_error("Failed to register buffer to SPDK.");
			return -1;
		}
	}
#else
	oxb_error("Unknown data fetcher channel.");
	return -1;
#endif
	oxb_info("Data Fetcher server is running.");

	return 0;
}

void exit_data_fetcher(void)
{
	oxb_info("Terminating Data Fetcher.");
}

/**
 * @brief Set the N-th bvec of a contiguous bio.
 * 
 * @param bio 
 * @param buf 
 * @param n_blks 
 */
static void set_bvec(struct bio *bio, int bvec_id, char *buf, uint32_t n_blks)
{
	oxbow_assert(bvec_id < bio->bi_vcnt);
	oxbow_assert(buf != NULL);

	// ckpt_info("bio=0x%lx bvec_id=%d buf=0x%lx n_blks=%u", (uintptr_t)bio,
	// 	  bvec_id, (uintptr_t)buf, n_blks);

	bio->bi_io_vec[bvec_id].bv_buf = buf;
	bio->bi_io_vec[bvec_id].bv_len = nblks_to_bytes(n_blks);
	bio->total_size += bio->bi_io_vec[bvec_id].bv_len;
}

/**
 * @brief Fetch superblock from disk to g_journal_ctx.
 */
void load_journal_sb(void *arg)
{
	struct bio_list *bl_sb;
	struct bio_list *bl_jsb;
	struct bio *bio;
	uint32_t sb_offset = 0;
	char tmp_buf[4096];
	int any_df_id = 0; // Any data fetcher id is okay.

	bl_sb = alloc_bl();

	if (g_j_ops->get_sb_offset) {
		sb_offset = g_j_ops->get_sb_offset();
	}

	// Read super block to get the block address of journal superblock.
	bio = alloc_bio_n_bvecs(OXBOW_SUPER_BLOCK_NR, 1);
	bio_list_add(bl_sb, bio);

	if (sb_offset) {
		set_bvec(bio, 0, tmp_buf, 1);
		se_dispatch_io_sync(bl_sb, 1);

		// To work around the issue of filling data after completion returns.
		se_nvmf_poll_complete(0);
		sleep(2);
		se_nvmf_poll_complete(0);
		se_nvmf_poll_complete(0);

		// Read one more to make sure the data is read.
		se_dispatch_io_sync(bl_sb, 1);

		memcpy(&g_sb_static, &tmp_buf[sb_offset], 4096 - sb_offset);
	} else {
		set_bvec(bio, 0, g_sb_static, 1);
		se_dispatch_io_sync(bl_sb, 1);

		// To work around the issue of filling data after completion returns.
		se_nvmf_poll_complete(0);
		sleep(2);
		se_nvmf_poll_complete(0);
		se_nvmf_poll_complete(0);

		// Read one more to make sure the data is read.
		se_dispatch_io_sync(bl_sb, 1);
	}

	g_jsb_baddr = g_j_ops->get_journal_sb_baddr(g_sb_static);
	oxb_warn("g_jsb_baddr=%lu", g_jsb_baddr);

	bl_jsb = alloc_bl();

	// Read journal super block.
	bio = alloc_bio_n_bvecs(g_jsb_baddr, 1);
	set_bvec(bio, 0, (char *)&g_journal_ctx.sb, 1);
	bio_list_add(bl_jsb, bio);
	se_dispatch_io_sync(bl_jsb, 1);

	print_journal_sb(&g_journal_ctx.sb);
	// dump_memory((char *)&g_journal_ctx.sb, 128);

	free_bl(bl_sb);
	free_bl(bl_jsb);

	atomic_fetch_add(&g_sb_loaded, 1);
}

/**
 * @brief Flush in-memory superblock state to disk.
 * 
 */
void sync_journal_sb(struct journal_log *j_log)
{
	struct bio_list *bl;
	struct bio *bio;

	bl = alloc_bl();
	bio = alloc_bio_n_bvecs(g_jsb_baddr, 1);
	set_bvec(bio, 0, (char *)&g_journal_ctx.sb, 1);
	bio_list_add(bl, bio);

	// Sync log state.
	g_journal_ctx.sb.start = j_log->tail;

	print_journal_sb(&g_journal_ctx.sb);

	se_request_dispatch_io_sync(bl, 0);

	free_bl(bl);
}

// TODO: to be implemented.
void do_recovery(void)
{
	oxb_warn(
		"Crash detected. sb.start=%lu sb.first=%lu. (Recovery is not supported yet.)",
		g_journal_ctx.sb.start, g_journal_ctx.sb.first);
}

static void reset_fetch_ctx(struct fetch_ctx *ctx)
{
	atomic_store(&ctx->req_cnt, 0);
	atomic_store(&ctx->desc_blk_baddr, 0);
	atomic_store(&ctx->total_log_size, 0);

	oxb_debug("[TRACK_REQ_CNT][%s] reset_fetch_ctx: req_cnt set to %u",
		  get_thread_name(), atomic_load(&ctx->req_cnt));
}

/**
 * @brief Reset journal context when a client is re-connected. (Temporary)
 *
 * Note: Correct implementation would be doing checkpointing to update the
 * recent state of superblock and Secure Daemon loads it from the disk.
 */
void reset_jnr_ctx(void)
{
	atomic_store(&g_tx_seqn, 0);

	// pthread_mutex_init(&g_fetch_ctx.req_mutex, NULL);
	// pthread_cond_init(&g_fetch_ctx.req_cond, NULL);
	for (int i = 0; i < 2; i++) {
		reset_fetch_ctx(&g_fetch_ctx[i]);
	}
}

static void init_fetch_ctx(struct fetch_ctx *ctx)
{
	pthread_mutex_init(&ctx->req_mutex, NULL);
	pthread_cond_init(&ctx->req_cond, NULL);
	atomic_init(&ctx->req_cnt, 0);
	atomic_init(&ctx->desc_blk_baddr, 0);
	atomic_init(&ctx->total_log_size, 0);
}

int init_journal(void)
{
	int ret;
	struct se_config se_conf;
	struct journal_log *j_log_first;
	struct journal_log *j_log_second;

#ifdef VALIDATE_WORKER_IDS
	oxb_warn(
		"Validating worker ids is on. Turn it off (unset VALIDATE_WORKER_IDS in journal.c) for performance.");
#endif

	// Initial value.
	atomic_init(&g_tx_seqn, 0);

	// Initialize fetch context.
	for (int i = 0; i < 2; i++) {
		init_fetch_ctx(&g_fetch_ctx[i]);
	}

	/* Init storage engine first than data fetcher as data fetcher uses spdk
	dma buffer.*/
#ifdef USE_NVME_STORAGE_ENGINE
	set_nvme_config(&se_conf.nvme, g_devfs_conf.pcie_nvme_addr,
			g_devfs_conf.spdk_max_io_requests_in_qpair);

	// NOTE: DevFS NVMe storage engine is configured in se_nvmf_fast.c.
#else
	set_nvmf_config(&se_conf.nvmf, g_devfs_conf.nvmf_ip_addr,
			g_devfs_conf.nvmf_port, g_devfs_conf.nvmf_subnqn,
			g_devfs_conf.spdk_max_io_requests_in_qpair);
#endif
	// rpc handler thread pool is shared with storage engine.
	ret = init_storage_engine(SE_NVMF, &se_conf,
				  g_devfs_conf.rpc_thread_num);
	worker_thpool = se_conf.worker_thpool;

	if (ret != 0) {
		oxb_error("Storage engine init failed. ret=%d", ret);
		return -1;
	}

	oxb_info("Initializing Storage Engine done.");

	ret = init_data_fetcher();
	if (ret < 0) {
		oxb_error("Failed to init Data Fetcher.");
		return -1;
	}

	// Checkpointing thread pool.
	ckpt_thpool = thpool_init(CKPT_THREAD_NUM, "ckpt_thread");
	ckpt_prepare_thpool =
		thpool_init(CKPT_PREPARE_THREAD_NUM, "ckpt_prep_thread");
	oxb_info("Checkpoint thread pool is initialized. # of threads: %d",
		 CKPT_THREAD_NUM);
	pthread_spin_init(&g_ckpt_list_lock, PTHREAD_PROCESS_PRIVATE);

	oxb_info("Initializing Storage Engine.");

	atomic_init(&g_sb_loaded, 0);

	// Only worker thread can do IO.
	thpool_add_work(worker_thpool, load_journal_sb, NULL);
	thpool_wait(worker_thpool);

	while (atomic_load(&g_sb_loaded) == 0) {
		usleep(1000);
	}

	/* We divide the log into two parts. (per-data-fetcher log) */
	j_log_first = &g_journal_ctx.logs[0];
	j_log_second = &g_journal_ctx.logs[1];

	j_log_first->df_id = 0;
	j_log_second->df_id = 1;

	pthread_spin_init(&j_log_first->lock, PTHREAD_PROCESS_PRIVATE);
	pthread_spin_init(&j_log_second->lock, PTHREAD_PROCESS_PRIVATE);

	/* Split the log area equally between j_log_first and j_log_second */
	uint64_t total_log_size = g_journal_ctx.sb.maxlen;
	uint64_t half_log_size = total_log_size / 2;

	j_log_first->first = g_journal_ctx.sb.first;
	j_log_first->last = j_log_first->first + half_log_size - 1;

	j_log_second->first = j_log_first->last + 1;
	j_log_second->last = j_log_second->first + half_log_size - 1;

	// Update dynamic pointers.
	j_log_first->tail = j_log_first->first;
	j_log_first->head = j_log_first->first;
	atomic_init(&j_log_first->n_free_blks,
		    j_log_first->last - j_log_first->first + 1);

	j_log_second->tail = j_log_second->first;
	j_log_second->head = j_log_second->first;
	atomic_init(&j_log_second->n_free_blks,
		    j_log_second->last - j_log_second->first + 1);

	// Crash is detected. (sb.start is set to 0 on shutdown.)
	if (g_journal_ctx.sb.start != 0)
		do_recovery();

#ifdef USE_NVME_STORAGE_ENGINE
	// Store block addresses for copying.
	sync_dev_ssb_baddr = g_j_ops->get_stage_sb_baddr(g_sb_static);
	sync_dev_ssb_nr_blks = g_j_ops->get_nr_stage_log_blocks(g_sb_static) +
			       1; // +1 for super block.
	sync_dev_fs_area_start_baddr =
		g_j_ops->get_fs_area_start_baddr(g_sb_static);
	sync_dev_fs_area_nr_blks = g_j_ops->get_nr_fs_area_blocks(g_sb_static);
#endif

	oxb_info(
		"Log1 initialized. first=%lu last=%lu tail=%lu head=%lu free_blks=%lu",
		j_log_first->first, j_log_first->last, j_log_first->tail,
		j_log_first->head, atomic_load(&j_log_first->n_free_blks));
	oxb_info(
		"Log2 initialized. first=%lu last=%lu tail=%lu head=%lu free_blks=%lu",
		j_log_second->first, j_log_second->last, j_log_second->tail,
		j_log_second->head, atomic_load(&j_log_second->n_free_blks));

#ifdef VALIDATE_STAGE_DESC_BLK
	oxb_warn(
		"Validating stage desc blks is on. Turn it off (unset VALIDATE_STAGE_DESC_BLK in journal.c) for performance.");
#endif

	return 0;
}

void exit_journal(void)
{
	oxb_info("Terminating Journal module.");

	thpool_wait(worker_thpool);
	thpool_destroy(worker_thpool);

	// TODO: do checkpoint. (log is cleared.)

	thpool_wait(ckpt_prepare_thpool);
	thpool_destroy(ckpt_prepare_thpool);

	thpool_wait(ckpt_thpool);
	thpool_destroy(ckpt_thpool);

	exit_data_fetcher();
}

char *fetch_data_from_host(int df_id, int df_buf_id, size_t size)
{
	char *buf;

	buf = fetch_data(g_df_ctx[df_id], df_buf_id, size);
	if (!buf) {
		oxb_error("Failed to fetch data.");
		return NULL;
	}

	return buf;
}

/**
 * @brief  Get the percentage of the free space.
 * 
 * @return int
 */
static int journal_free_percent(struct journal_log *j_log)
{
	uint64_t total_blks, free_blks;
	int percent;

	total_blks = j_log->last - j_log->first + 1;
	free_blks = atomic_load(&j_log->n_free_blks);

	percent = (free_blks * 100) / total_blks;

	return percent;
}

/**
 * @brief Allocate log to write data. The log is circular buffer.
 * A new space is allocated from the header of the log. The caller should hold
 * the lock (j_log->lock).
 * 
 * @param df_id Data Fetcher id.
 * @param size Allocation size in bytes.
 * @return baddr_t Start blk address of the allocated space.
 */
static baddr_t alloc_journal_space(struct journal_log *j_log, size_t size)
{
	baddr_t start_baddr;
	uint64_t n_blk_req, alloced, remains;
	uint32_t nr_free_blks;

	// # of blocks requested.
	n_blk_req = bytes_to_nblks(size);

retry:
	// Not required for now. (Use coarse-grained lock.)
	// pthread_spin_lock(&g_journal_ctx.log_head_lock);

	// Check free blocks. Wait until there are sufficient free blocks.
	// TODO: [OPTIMIZE] Use atomic variable? Or sleep for a while?
	nr_free_blks = atomic_load(&j_log->n_free_blks);
	if (nr_free_blks < n_blk_req) {
		// pthread_spin_unlock(&g_journal_ctx.log_head_lock);
		oxb_warn("No free space in journal. requested=%lu free=%lu",
			 n_blk_req, nr_free_blks); // Print temporarily.

		// Checkpoint to release space.
		oxb_warn("Checkpoint triggered!.");
		oxbow_assert(0); // TMP
		thpool_add_work(ckpt_thpool, start_checkpoint,
				(void *)CKPT_THRESHOLD_FREE_TARGET);
		goto retry;
	}

	start_baddr = j_log->head;

	oxb_debug("j_log->head=%lu j_log->last=%lu", j_log->head, j_log->last);
	oxb_debug("j_log->first=%lu j_log->tail=%lu", j_log->first,
		  j_log->tail);

	// Update head.
	if (j_log->head + n_blk_req > j_log->last) { // Wrap-around occurs.
		alloced = j_log->last - j_log->head +
			  1; // Allocated before wrap-around.
		remains = n_blk_req - alloced; // Allocated after wrap-around.
		j_log->head = j_log->first + remains; // New header.

	} else { // No wrap-around occurs.
		j_log->head += n_blk_req;
	}

	// Update # of free blocks.
	nr_free_blks = atomic_fetch_sub(&j_log->n_free_blks, n_blk_req);

	// Print free space.
	oxb_debug("Journal alloc: %lu blks alloced (first - last: %lu - %lu)",
		  n_blk_req, start_baddr, start_baddr + n_blk_req - 1);
	oxb_info(
		"Journal Free Space (df_id=%d): %d %% (%lu / %lu blks are free)",
		j_log->df_id, journal_free_percent(j_log),
		nr_free_blks - n_blk_req, j_log->last - j_log->first + 1);

	// pthread_spin_unlock(&g_journal_ctx.log_head_lock);
	return start_baddr;
}

/**
 * @brief Advance log pointers. The caller should hold the lock (j_log->lock).
 * 
 * @param n_blk_added 
 */
static void update_log_state(struct journal_log *j_log, uint64_t n_blk_added)
{
	uint64_t alloced, remains;
	uint32_t nr_free_blks;
	baddr_t start_baddr;

	start_baddr = j_log->head;

	// Update head.
	if (j_log->head + n_blk_added > j_log->last) { // Wrap-around occurs.
		alloced = j_log->last - j_log->head +
			  1; // Allocated before wrap-around.
		remains = n_blk_added - alloced; // Allocated after wrap-around.
		j_log->head = j_log->first + remains; // New header.

	} else { // No wrap-around occurs.
		j_log->head += n_blk_added;
	}

	// Update # of free blocks.
	nr_free_blks = atomic_fetch_sub(&j_log->n_free_blks, n_blk_added);

	// Print free space.
	oxb_debug(
		"Journal state updated: %lu blks (1 desc + data) alloced (first - last: %lu - %lu)",
		n_blk_added, start_baddr, start_baddr + n_blk_added - 1);
	oxb_debug("Journal Free Space: %d %% (%lu / %lu blks are free)",
		  journal_free_percent(j_log), nr_free_blks - n_blk_added,
		  j_log->last - j_log->first + 1);
}

/**
 * @brief Get the start address of the chunk. Chunk is a unit of data fetching.
 * 
 * @param req_id 
 * @return baddr_t 
 */
static baddr_t get_chunck_start(struct journal_log *j_log, int req_id)
{
	/// No need to use locks for journal log pointers as they are changed
	/// only in persist_meta_and_commit() after all the data_persist() are done.
	baddr_t start;
	uint64_t n_blks_per_chunk;

	n_blks_per_chunk = DATA_FETCHER_BUF_SIZE >> OXBOW_BLOCK_SIZE_SHIFT;

	if (req_id == 0)
		start = j_log->head;
	else
		// +1: for desc blk (the first block is reserved for desc blk).
		start = j_log->head + req_id * n_blks_per_chunk + 1;

	return start;
}

/**
 * @brief Free the allocated log space. It frees the blocks from the tail of the
 * log. So it should be called in order of allocation.
 * 
 * @param start_baddr 
 * @param size Size in bytes.
 * @param sync Do sync with on-disk super block.
 */
static void free_journal_space(struct journal_log *j_log, baddr_t start_baddr,
			       size_t size, int sync)
{
	uint64_t n_blk_req, freed, remains;

	// # of blocks requested.
	n_blk_req = bytes_to_nblks(size);

	// It is not the oldest log. Trying to free logs in the middle.
	if (start_baddr != j_log->tail) {
		oxb_error("start_baddr=%lu tail=%lu", start_baddr, j_log->tail);
		oxbow_assert(0);
	}

	pthread_spin_lock(&j_log->lock);

	// Update tail.
	if (j_log->tail + n_blk_req > j_log->last) { // Wrap-around occurs.
		freed = j_log->last - j_log->tail +
			1; // Freed before wrap-around.
		remains = n_blk_req - freed; // Freed after wrap-around.
		j_log->tail = j_log->first + remains; // New tail.

	} else { // No wrap-around occurs.
		j_log->tail += n_blk_req;
	}

	// Update # of free blocks.
	atomic_fetch_add(&j_log->n_free_blks, n_blk_req);

	// It is important flushing superblock within the lock.
	// We can allocate this journal space to others after the superblock is
	// persisted.
	if (sync)
		sync_journal_sb(j_log);

	pthread_spin_unlock(&j_log->lock);
}

/**
 * @brief Increase in-memory journal superblock on checkpointing.
 * 
 * @param this_tx_id Checkpointed tx id.
 */
static void increase_journal_sb_tx_id(uint32_t this_tx_id)
{
	if (g_journal_ctx.sb.tx_id != this_tx_id)
		oxb_warn(
			"tx_id mismatch. this_tx_id=%u g_journal_ctx.sb.tx_id=%u",
			this_tx_id, g_journal_ctx.sb.tx_id);

	// Increase tx_id;
	g_journal_ctx.sb.tx_id = this_tx_id + 1;
}

/**
 * @brief Create bios and add them to the bio list. It does not consider
 * wrap-aroundness. So, callers should call this functions for the contiguous
 * blocks.
 * 
 * @param bl 
 * @param buf 
 * @param start 
 * @param n_blks 
 */
static void add_bio_to_bl(struct bio_list *bl, char *buf, baddr_t start,
			  uint32_t n_blks)
{
	struct bio *bio;
	size_t size_copied;
	size_t size_to_copy;
	size_t size_total;
	uint32_t n_blks_to_copy;
	baddr_t cur_start;
	char *buf_p;

	size_total = nblks_to_bytes(n_blks);
	buf_p = buf;
	cur_start = start;

	for (size_copied = 0; size_copied < size_total;
	     size_copied += BIO_MAX_BIO_SIZE) {
		bio = alloc_bio_n_bvecs(cur_start, 1);
		bio_list_add(bl, bio);

		size_to_copy = min(size_total - size_copied, BIO_MAX_BIO_SIZE);
		n_blks_to_copy = bytes_to_nblks(size_to_copy);

		set_bvec(bio, 0, buf_p, n_blks_to_copy);

		cur_start += n_blks_to_copy;
		buf_p += size_to_copy;
	}
}

/**
 * @brief
 * 
 * @param j_log 
 * @param bl 
 * @param start_baddr 
 * @param buf 
 * @param size 
 * @return baddr_t The address of the block next to the last block. (wrap not checked)
 */
static baddr_t add_bios_considering_wrap(struct journal_log *j_log,
					 struct bio_list *bl,
					 baddr_t start_baddr, char *buf,
					 size_t size)
{
	bool wrap;
	uint32_t n_blks_total, n_blks1, n_blks2;
	char *buf2;
	baddr_t next;

	n_blks_total = bytes_to_nblks(size);

	// Check whether wrap-around occurs.
	wrap = check_wrap_around_journal_tx(start_baddr, n_blks_total, j_log);
	if (wrap) {
		// Split into two bios.
		/* First bio. (start ~ end of stage log) */
		n_blks1 = j_log->last - start_baddr + 1;
		add_bio_to_bl(bl, buf, start_baddr, n_blks1);

		/* Second bio. (start of stage log ~ ) */
		buf2 = buf + nblks_to_bytes(n_blks1);
		n_blks2 = n_blks_total - n_blks1;
		add_bio_to_bl(bl, buf2, j_log->first, n_blks2);

		next = j_log->first + n_blks2;

		oxb_debug("Wrap-around occurs. n_blks1=%u n_blks2=%u", n_blks1,
			  n_blks2);

	} else {
		add_bio_to_bl(bl, buf, start_baddr, n_blks_total);

		next = start_baddr + n_blks_total;

		oxb_debug("No wrap-around occurs. n_blks_total=%u",
			  n_blks_total);
	}

	return next;
}

/**
 * @brief Returns the second buffer address if wrap-around occurs.
 * 
 * @param bl_first 
 * @param bl_second 
 * @param start_baddr 
 * @param buf 
 * @param size 
 * @return char* 
 */
static char *add_bios_considering_wrap_into_separate_bls(
	struct journal_log *j_log, struct bio_list *bl_first,
	struct bio_list *bl_second, baddr_t start_baddr, char *buf, size_t size)
{
	bool wrap;
	uint32_t n_blks_total, n_blks1, n_blks2;
	char *buf2 = NULL;

	n_blks_total = bytes_to_nblks(size);

	// Check whether wrap-around occurs.
	wrap = check_wrap_around_journal_tx(start_baddr, n_blks_total, j_log);
	if (wrap) {
		// Split into two bios.
		/* First bio. (start ~ end of stage log) */
		n_blks1 = j_log->last - start_baddr + 1;
		add_bio_to_bl(bl_first, buf, start_baddr, n_blks1);

		/* Second bio. (start of stage log ~ ) */
		buf2 = buf + nblks_to_bytes(n_blks1);
		n_blks2 = n_blks_total - n_blks1;
		add_bio_to_bl(bl_second, buf2, j_log->first, n_blks2);

		oxb_warn(
			"Wrap-around occurs. start_baddr=%lu n_blks_total=%u j_log->last=%lu n_blks1=%u n_blks2=%u",
			start_baddr, n_blks_total, j_log->last, n_blks1,
			n_blks2);

	} else {
		add_bio_to_bl(bl_first, buf, start_baddr, n_blks_total);

		// oxb_warn("No wrap-around occurs. n_blks_total=%u",
		// 	 n_blks_total);
	}

	return buf2;
}

int persist_data(int df_id, int req_id, char *rdma_buf, size_t size)
{
	// TODO: Change to the stack variables.
	struct bio_list *bl_first = NULL,
			*bl_second = NULL; // used for wrap-around.
	baddr_t data_start, chunk_start;
	struct fetch_ctx *f_ctx;
	int ret = 0;
	size_t tmp;
	char *rdma_buf_second;
	UNUSED1(tmp);
	int nr_total_reqs = -1;
	struct journal_log *j_log;

	j_log = &g_journal_ctx.logs[df_id];

	f_ctx = &g_fetch_ctx[df_id];

	// Track req_cnt at the beginning
	oxb_debug(
		"[TRACK_REQ_CNT][%s] persist_data start: df_id=%d req_id=%d current req_cnt=%u",
		get_thread_name(), df_id, req_id, atomic_load(&f_ctx->req_cnt));

	PF_TL_START(a_persist_data);

	oxbow_assert(size); // Size = 0.
	oxbow_assert(rdma_buf); // Buffer is NULL.

	chunk_start = data_start = get_chunck_start(j_log, req_id);

	if (req_id == 0) {
		data_start = chunk_start + 1; // Skip desc blk.

		// Check it is reset correctly.
		oxbow_assert(atomic_load(&f_ctx->desc_blk_baddr) == 0);

		atomic_store(&f_ctx->desc_blk_baddr, chunk_start);
		commit_trace(
			"[PERSIST_DATA] Store desc_blk (prealloc): df_id=%d baddr=%lu",
			df_id, chunk_start);
	}

	oxb_debug(
		"PERSIST_DATA: req_id=%d size=%lu chunk_start=%lu data_start=%lu tid=%d",
		req_id, size, chunk_start, data_start, get_tid());

	/* IOs are performed in parallel */
	// TODO: Create one or two bio list considering wrap-around.

	bl_first = alloc_bl();
	bl_second = alloc_bl();

	rdma_buf_second = add_bios_considering_wrap_into_separate_bls(
		&g_journal_ctx.logs[df_id], bl_first, bl_second, data_start,
		rdma_buf, size);

	commit_trace(
		"[PERSIST_DATA] Store data: df_id=%d baddr=%lu size=%lu(%u blks)",
		df_id, data_start, size, size / OXBOW_BLOCK_SIZE);

	/* NOTE: We have to send a single bio separately to the storage engine when we
	use custom buffer.*/

	// First bio.
	print_bl(bl_first);

	// se_dispatch_io_nocopy_sync(bl_first, 0, rdma_buf);

#ifdef DEBUG_HEXDUMP_JNL_DATA_PAGES
	{
		char debug_label[128];
		int i;
		for (i = 0; i < 10; i++) {
			snprintf(debug_label, sizeof(debug_label),
				 "buf=0x%lx, i=%d (total %lu pages)",
				 (uintptr_t)rdma_buf + i * PAGE_SIZE, i,
				 size / PAGE_SIZE);

			quick_hexdump(rdma_buf + i * PAGE_SIZE, PAGE_SIZE,
				      debug_label);
		}
	}
#endif

	// TODO: Can we do it async?
	nr_total_reqs = se_dispatch_io_nocopy_async(bl_first, 0, rdma_buf);
	if (nr_total_reqs == 0) {
		oxb_error("Dispatching log bio list failed.");
		goto free;
	}

	// Second bio. (There is wrap-around.)
	if (!bio_list_empty(bl_second)) {
		oxb_warn("Dispatching second bio.");
		print_bl(bl_second);

		ret = se_dispatch_io_nocopy_async(bl_second, 0,
						  rdma_buf_second);
		if (ret < 0) {
			oxb_error("Dispatching log bio list failed.");
			goto free;
		}
		nr_total_reqs += ret;
	}

	// Wait for the first bio to complete.
	se_nvmf_poll_complete(nr_total_reqs);

	tmp = atomic_fetch_add(&f_ctx->total_log_size, size);
	oxb_debug("fd_id=%d f_ctx->total_log_size updated: %lu + %lu -> %lu",
		  df_id, tmp, size, tmp + size);

	PF_TRACK_TPUT(COMMIT_DATA_IO, size >> OXBOW_BLOCK_SIZE_SHIFT);

	// req_cnt is checked at the beginning of this function and writeback_meta_commit() function.
	pthread_mutex_lock(&f_ctx->req_mutex);
	uint32_t old_req_cnt = atomic_load(&f_ctx->req_cnt);
	atomic_fetch_add(&f_ctx->req_cnt, 1);
	uint32_t new_req_cnt = atomic_load(&f_ctx->req_cnt);

	oxb_debug(
		"[TRACK_REQ_CNT][%s] persist_data: df_id=%d req_id=%d req_cnt: %u -> %u",
		get_thread_name(), df_id, req_id, old_req_cnt, new_req_cnt);

	// Wake up commit thread.
	pthread_cond_broadcast(&f_ctx->req_cond);
	pthread_mutex_unlock(&f_ctx->req_mutex);

free:
	free_bl(bl_first);
	free_bl(bl_second);

	PF_TL_END(a_persist_data);

	return ret;
}

static void commit_tx(struct bio_list *bl)
{
	// Synchronous I/O.
	se_dispatch_io_sync(bl, 0);
}

/**
 * @brief Enqueue a new entry to the checkpoint list. (To the tail.)
 * 
 * @param ce A new checkpoint list entry.
 */
static void ckpt_list_enqueue(struct ckpt_list_entry *ce)
{
	pthread_spin_lock(&g_ckpt_list_lock);
	list_add_tail(&ce->list, &g_ckpt_list);
	pthread_spin_unlock(&g_ckpt_list_lock);
}

/**
 * @brief Dequeue the oldest entry from the checkpoint list. (From the head.)
 * 
 * @return struct ckpt_list_entry* A dequeued entry. Null if the list is empty
 * or there is no entry ready.
 */
static struct ckpt_list_entry *ckpt_list_dequeue(void)
{
	struct ckpt_list_entry *ce;

	pthread_spin_lock(&g_ckpt_list_lock);
	ce = list_first_entry_or_null(&g_ckpt_list, struct ckpt_list_entry,
				      list);
	if (ce == NULL ||
	    atomic_load_explicit(&ce->ready, memory_order_acquire) == false) {
		ce = NULL;
		goto no_ready_entry;
	}
	list_del(&ce->list);

no_ready_entry:
	pthread_spin_unlock(&g_ckpt_list_lock);

	return ce;
}

/**
 * @brief 
 * 
 * @param first log first baddr.
 * @param last log last baddr.
 * @param target target baddr.
 * @return baddr_t 
 */
static inline baddr_t wrapped_baddr(baddr_t first, baddr_t last, baddr_t target)
{
	return first + (target - last - 1);
}

/**
 * @brief Add a block to the bio list as a new bio.
 * 
 * @param j_log 
 * @param bl 
 * @param dst_baddr 
 * @param buf 
 * @return baddr_t The address of the block next to the added block. (wrap not checked)
 */
static baddr_t add_bio_considering_wrap(struct journal_log *j_log,
					struct bio_list *bl, baddr_t dst_baddr,
					char *buf)
{
	bool wrap;
	struct bio *bio;

	wrap = check_wrap_around_journal_tx(dst_baddr, 1, j_log);
	if (wrap)
		dst_baddr = wrapped_baddr(j_log->first, j_log->last, dst_baddr);

	// add_bio_to_bl(bl, (char *)sb_buf, sb_baddr, 1);
	bio = alloc_bio_n_bvecs(dst_baddr, 1);
	bio_list_add(bl, bio);
	set_bvec(bio, 0, buf, 1);

	return dst_baddr + 1;
}

/**
 * @brief Do pre-tasks like coalescing before checkpointing.
 * 
 * @param arg
 */
void prepare_ckpt(void *arg)
{
	int free_perc;
	struct ckpt_list_entry *ce;

	ce = (struct ckpt_list_entry *)arg;

	// Construct in-memory stage trace blocks.
	// FIXME: When is the best to do this job? If there are bursty commit
	// requests, it is better to postpone it as late as possible.
	//
	// Construct in-memory stage trace blocks.
	// set_stage_trace_blks_of_tx_meta(ce);

	// TODO: Do coalescing.

	// Ready to checkpoint.
	atomic_store_explicit(&ce->ready, true, memory_order_release);

	free_perc = journal_free_percent(ce->j_log);

	// Trigger checkpoint if required.
	if (free_perc < CKPT_THRESHOLD_FREE_TRIGGER) {
		oxb_warn("Checkpoint triggered! free_perc=%d threshold=%d",
			 free_perc, CKPT_THRESHOLD_FREE_TRIGGER);
		oxbow_assert(0); // TMP
		thpool_add_work(ckpt_thpool, start_checkpoint,
				(void *)CKPT_THRESHOLD_FREE_TARGET);
	}
}

/**
 * @brief Copy descriptor and etag blocks for checkpointing.
 *
 * @param rdma_bufs rdma buf (pointing to desc blk of meta_buf).
 * @param ce
 */
static void copy_desc_and_meta_to_tx_meta(char *rdma_buf,
					  struct ckpt_list_entry *ce)
{
	struct journal_descriptor_block *jdh;
	char *meta;

	jdh = (struct journal_descriptor_block *)rdma_buf;

	/* Copy descriptor block. */
	memcpy(&ce->tx_meta->desc_blk, jdh,
	       sizeof(struct journal_descriptor_block));

	// There is only a descriptor block.
	if (jdh->h.nr_etag_blks == 0 && jdh->h.nr_stage_trace_blks == 0)
		return;

	/* Copy meta (etag & stage trace) blocks. */
	meta = (char *)jdh + OXBOW_BLOCK_SIZE; // Advance by 4096 bytes

	memcpy(ce->tx_meta->meta_start, meta,
	       (jdh->h.nr_etag_blks + jdh->h.nr_stage_trace_blks) *
		       OXBOW_BLOCK_SIZE);
}

/**
 * @brief Do sync checkpoint if there is no enough free blocks.
 * 
 * @param size Requested size.
 */

static void do_sync_checkpoint(struct journal_log *j_log, size_t size)
{
	uint64_t n_blk_req;
	uint32_t nr_free_blks;

	// # of blocks requested.
	n_blk_req = bytes_to_nblks(size);
retry:
	// Not required for now. There is only one transaction at specific time.
	// pthread_spin_lock(&g_journal_ctx.log_head_lock);

	nr_free_blks = atomic_load(&j_log->n_free_blks);
	if (nr_free_blks < n_blk_req) {
		// pthread_spin_unlock(&g_journal_ctx.log_head_lock);
		oxb_warn("No free space in journal. requested=%lu free=%lu",
			 n_blk_req, nr_free_blks); // Print temporarily.

		// Checkpoint to release space.
		thpool_add_work(ckpt_thpool, start_checkpoint,
				(void *)CKPT_THRESHOLD_FREE_TARGET);
		goto retry;
	}
}

void wait_for_all_persist_data_completion(struct fetch_ctx *f_ctx,
					  size_t total_data_size,
					  uint32_t n_fetch_reqs)
{
	oxb_debug(
		"[TRACK_REQ_CNT][%s] wait_for_all_persist_data_completion start: expecting req_cnt=%u, current=%u",
		get_thread_name(), n_fetch_reqs, atomic_load(&f_ctx->req_cnt));

	// Wait until data fetch jobs are completed.
	pthread_mutex_lock(&f_ctx->req_mutex);

	do {
		uint32_t current_req_cnt = atomic_load(&f_ctx->req_cnt);
		if (current_req_cnt == n_fetch_reqs) {
			oxb_debug(
				"[TRACK_REQ_CNT][%s] wait_for_all_persist_data_completion: target reached req_cnt=%u",
				get_thread_name(), current_req_cnt);
			break;
		}

		oxb_debug(
			"[TRACK_REQ_CNT][%s] wait_for_all_persist_data_completion: waiting... current req_cnt=%u, expected=%u",
			get_thread_name(), current_req_cnt, n_fetch_reqs);

		pthread_cond_wait(&f_ctx->req_cond, &f_ctx->req_mutex);
	} while (1);

	pthread_mutex_unlock(&f_ctx->req_mutex);

	oxbow_assert(total_data_size == atomic_load(&f_ctx->total_log_size));
}

/**
 * @brief
 *
 * @param df_id Data Fetcher id.
 * @param rdma_buf Buffer storing metadata (descriptor block and etag blocks).
 * @param md_size Metadata size.
 * @param total_data_size Total data size.
 * @param n_fetch_reqs The number of fetch requests.
 * @return int 0 on success. -1 on error.
 */
int persist_meta_and_commit(int df_id, char *rdma_buf, size_t md_size,
			    size_t total_data_size, int n_fetch_reqs)
{
	struct bio_list *meta_bl, *commit_bl;
	baddr_t start_baddr, commit_baddr, sb_baddr;
	struct commit_header ch;
	struct timespec ts;
	struct ckpt_list_entry *ce;
	size_t jnl_meta_alloc_size; // Including etag, stage trace, super, commit blks.
	size_t total_tx_size; // Total journal log size.
	size_t jnl_md_size; // etag, stage trace blocks (except desc blk in metadata buf).
	int ret = 0;
	struct journal_descriptor_block *jdb;
	uint32_t cur_seqn;
	struct journal_superblock sb_buf; // A buffer for a super block snapshot.
	uint32_t total_req;
	baddr_t next_baddr;
	baddr_t jnl_alloc_start;
	baddr_t log_tail;
	struct fetch_ctx *f_ctx;
	struct journal_log *j_log;

	f_ctx = &g_fetch_ctx[df_id];
	j_log = &g_journal_ctx.logs[df_id];

	oxbow_assert(md_size); // Size = 0.
	oxbow_assert(rdma_buf); // Buffer is NULL.

	/* Alloc first to reduce lock contention. */
	pthread_spin_lock(&j_log->lock);

	if (n_fetch_reqs == 0) { // If there was no data received.
		oxbow_assert(total_data_size == 0);
		jnl_alloc_start = alloc_journal_space(
			j_log, OXBOW_BLOCK_SIZE /* desc blk */);

		// Check it is reset correctly.
		uint32_t current_req_cnt = atomic_load(&f_ctx->req_cnt);
		oxb_debug(
			"[TRACK_REQ_CNT][%s] persist_meta_and_commit: checking req_cnt reset - expected=0, actual=%u",
			get_thread_name(), current_req_cnt);
		oxbow_assert(current_req_cnt == 0);
		oxbow_assert(atomic_load(&f_ctx->desc_blk_baddr) == 0);
		oxbow_assert(atomic_load(&f_ctx->total_log_size) == 0);

		atomic_store(&f_ctx->desc_blk_baddr, jnl_alloc_start);
		oxb_warn("desc_blk_baddr is set to %lu", jnl_alloc_start);
	} else {
		// Before wait
		oxb_debug(
			"[TRACK_REQ_CNT][%s] persist_meta_and_commit: before wait, df_id=%d n_fetch_reqs=%d current req_cnt=%u",
			get_thread_name(), df_id, n_fetch_reqs,
			atomic_load(&f_ctx->req_cnt));

		wait_for_all_persist_data_completion(f_ctx, total_data_size,
						     n_fetch_reqs);

		// After wait
		oxb_debug(
			"[TRACK_REQ_CNT][%s] persist_meta_and_commit: after wait, df_id=%d n_fetch_reqs=%d current req_cnt=%u",
			get_thread_name(), df_id, n_fetch_reqs,
			atomic_load(&f_ctx->req_cnt));

		// Update log state for data blocks. (+1 for desc blk)
		update_log_state(
			j_log, (total_data_size >> OXBOW_BLOCK_SIZE_SHIFT) + 1);
	}

	// Additional two blocks for a super block and a commit block.
	//
	// | desc_blk | data_blks .. | etag / stage trace blks .. | super_blk | commit_blk |
	//
	//  DevFS moves desc_blk from the first block of metadata buf
	//  to the first block of jnl tx.
	jnl_md_size = md_size - OXBOW_BLOCK_SIZE /* desc_blk in meta buf */;
	jnl_meta_alloc_size =
		jnl_md_size + OXBOW_BLOCK_SIZE * 2 /* super blk + commit blk */;

	start_baddr = alloc_journal_space(j_log, jnl_meta_alloc_size);

	log_tail = j_log->tail;

	pthread_spin_unlock(&j_log->lock);

	// I/O the followings to the journal log (1-3 as one bl.):
	// 1. metadata (desc + etag blocks)
	// 2. super block
	// 3. desc block --> To the first block of the log. (for recovery)
	// 4. commit block

	meta_bl = alloc_bl();

	// Add metadata to the bio list.
	next_baddr = add_bios_considering_wrap(
		j_log, meta_bl, start_baddr,
		rdma_buf + OXBOW_BLOCK_SIZE /* skip desc_blk */, jnl_md_size);

	// Add super block to the bio list.
	memcpy(&sb_buf, &g_journal_ctx.sb, sizeof(struct journal_superblock));

	// Update superblock that will be written to the disk.
	sb_buf.start = log_tail;

	// After the last etag block.
	sb_baddr = next_baddr;

	next_baddr = add_bio_considering_wrap(j_log, meta_bl, sb_baddr,
					      (char *)&sb_buf);

	// The first block is a descriptor block.
	jdb = (struct journal_descriptor_block *)rdma_buf;
	jdb->h.meta_start_baddr = start_baddr; // start baddr of metadata.

	// Add descriptor block to the bio list.
	add_bio_considering_wrap(j_log, meta_bl,
				 atomic_load(&f_ctx->desc_blk_baddr),
				 (char *)jdb);

	commit_info(
		"[COMMIT] Store to baddr: df_id=%d desc_blk(%lu) metadata(%lu, size=%lu(%u blks)) super_blk(%lu)",
		df_id, atomic_load(&f_ctx->desc_blk_baddr), start_baddr,
		jnl_md_size, jnl_md_size >> OXBOW_BLOCK_SIZE_SHIFT, sb_baddr);

	// Submit parallel IOs with multiple threads.
	commit_debug("Writing desc_blk + metadata + super_blk.");

	print_bl(meta_bl);

	// TODO: Can we do it async?
	// total_req = se_dispatch_io_async(meta_bl, 0);
	total_req = se_dispatch_io_sync(meta_bl, 0);

	/********************************************************************
	 * Do Some Async Jobs before calling se_is_completed() for meta_bl. *
	 ********************************************************************/

	/* Prepare commit_bl while doing IO. */
	commit_bl = alloc_bl();

	// after super block.
	commit_baddr = next_baddr /* after superblock */;
	add_bio_considering_wrap(j_log, commit_bl, commit_baddr, (char *)&ch);

	commit_trace("[COMMIT] Store commit_blk(df_id=%d baddr=%lu)", df_id,
		     commit_baddr);

	// Fill the commit block except timestamp.
	ch = (struct commit_header){
		.common.h_magic = g_journal_ctx.sb.common.h_magic,
		.common.h_blocktype = COMMIT_BLK,
		.common.h_txid = jdb->h.transaction_id,
	};

	total_tx_size = total_data_size + jnl_meta_alloc_size +
			nblks_to_bytes(1) /* the first desc blk */;

	// Add a ckpt entry. It is freed after checkpointing.
	ce = alloc_ckpt_entry(jdb->h.nr_etag_blks + jdb->h.nr_stage_trace_blks);
	if (!ce) {
		// Wait until the io is completed and return.
		ret = -1;
		goto alloc_failed;
	}

	// Fill ce and tx meta.
	ce->j_log = j_log;
	ce->j_start_baddr = atomic_load(&f_ctx->desc_blk_baddr);
	ce->total_tx_size = total_tx_size;

	ce->tx_meta->tx_id = jdb->h.transaction_id;

	commit_debug(
		"Create Checkpoint Entry: df_id=%d start_baddr=%lu total_tx_size=%lu (%u blks),\n"
		"\t\t\t\t\t TX_META: tx_id=%u n_meta_blks=%u (n_etag_blks=%u + n_st_blks=%u)",
		df_id, ce->j_start_baddr, ce->total_tx_size,
		ce->total_tx_size >> OXBOW_BLOCK_SIZE_SHIFT, ce->tx_meta->tx_id,
		jdb->h.nr_etag_blks + jdb->h.nr_stage_trace_blks,
		jdb->h.nr_etag_blks, jdb->h.nr_stage_trace_blks);

	// Optimization: Store desc and etag blocks in memory for checkpointing.
	// NOTE: stage trace blocks are stored later.
	copy_desc_and_meta_to_tx_meta(rdma_buf, ce);

alloc_failed:

	// Wait until all the log blocks (meta_bl) are flushed.
	// se_nvmf_poll_complete(total_req);

	if (ret < 0) {
		oxb_error("Checkpoint list entry allocation failed.");
		free_bl(meta_bl);
		free_bl(commit_bl);
		return -1;
	}

	/*
	 * Enforce sequence number order here.
	 * It guarantees 1) commit and 2) checkpointing be performed in
	 * tx_id (sequence number) order. (Pushing a job to ckpt_prepare_thpool
	 * in order.)
	 */
	pthread_mutex_lock(&g_tx_seqn_mutex);

	while ((cur_seqn = atomic_load(&g_tx_seqn)) !=
		       0 && // Initial value is 0.
	       jdb->h.transaction_id != cur_seqn + 1) {
		// Wait for my turn.
		oxb_info("Waiting for my turn. g_cur_seqn=%u my_tx_id=%u",
			 cur_seqn, jdb->h.transaction_id);
		pthread_cond_wait(&g_tx_seqn_cond, &g_tx_seqn_mutex);
	}

	oxb_info("Waiting done. g_cur_seqn=%u my_tx_id=%u", cur_seqn,
		 jdb->h.transaction_id);

	// Update superblock in memory. (Synced on checkpointing.)
	g_journal_ctx.sb.start = log_tail;

	// Update commit block timestamp.
	timestamp_now(&ts);
	ch.h_commit_sec = ts.tv_sec;
	ch.h_commit_nsec = ts.tv_nsec;

	/* Write a commit record to the transaction. */
	oxb_trace("Writing Commit block.");
	commit_tx(commit_bl);

	// Initialize tx_id if it is the first commit.
	if (g_journal_ctx.sb.tx_id == 0)
		g_journal_ctx.sb.tx_id = jdb->h.transaction_id;

#ifdef DEBUG_PRINT_JNL_TX
	print_jnl_tx(ce, (uint64_t)-1, ~0U); // Dump all block types
	// print_jnl_tx(ce, (uint64_t)-1,
	// 	     DUMP_DESC_BLK | DUMP_STAGE_TRACE_BLK | DUMP_ETAG_BLK |
	// 		     DUMP_STAGE_DESC_BLK);
	// print_jnl_tx(ce, (uint64_t)-1, DUMP_EXT4_INODE_TABLE);
	// print_jnl_tx(ce, (uint64_t)-1, DUMP_EXT4_EXTENT_BLOCKS);
	// print_jnl_tx(ce, (uint64_t)-1, 0U);
#endif

	free_bl(meta_bl);
	free_bl(commit_bl);
	reset_fetch_ctx(f_ctx);

	// NOTE: We don't need to flush log state (g_journal_ctx.log).
	// It can be constructed from the commit logs in the recovery stage.

	// Add to the ckpt list in order.
	// At this point ce->ready is false.
	ckpt_list_enqueue(ce);

	// FIXME: The checkpointing should also be triggered while persisting
	// data if there is no enough free blocks. However, free blocks are not
	// updated during persisting data (for parallelization). Hence, we check
	// free blocks after committing a transaction. It may incur a situation
	// that there is no space to persist data if a transaction is very
	// large. (Cf. do_sync_checkpoint())
	//
	// Do some pre-tasks. (E.g., coalescing) ce->ready becomes true after preparation.
	thpool_add_work(ckpt_prepare_thpool, prepare_ckpt, (void *)ce);

	// Update the global transaction sequence number.
	atomic_store(&g_tx_seqn, jdb->h.transaction_id);

	// Wake up the other threads.
	pthread_cond_broadcast(&g_tx_seqn_cond);

	/*
	 * Should we unlock it after sending RPC response?
	 * No. Although we do that, we cannot guarantee responses arrive in
	 * order. Instead, it is more reliable to check the most recent
	 * committed tx_id in Secure Daemon.
	 */
	pthread_mutex_unlock(&g_tx_seqn_mutex);

	return 0;
}

#if 0
static void construct_first_desc_bl(struct bio_list *bl,
				    struct journal_descriptor_block *jdh,
				    baddr_t d_baddr)
{
	struct bio *bio;

	bio = alloc_bio_single_bvec(d_baddr);
	set_first_bvec(bio, (char *)jdh, 1);
	bio_list_add(bl, bio);
}
#endif

static void read_stage_desc_blk(struct stage_descriptor_block *sdbs,
				struct journal_stage_trace_block *st_blk)
{
	struct bio_list *bl;
	struct bio *bio;
	uint32_t i, n_stage_txs;
	baddr_t dst;
	bool wrap;

	n_stage_txs = st_blk->h.nr;

	// Read the block.
	bl = alloc_bl();

	// Get all the stage desc blks.
	for (i = 0; i < n_stage_txs; i++) {
		// TOCHECK: Can wrap-around occur?
		wrap = check_wrap_around_stage_tx(st_blk->tx_list[i], 1);

		if (wrap)
			dst = wrapped_baddr(
				g_j_ops->first_stage_log_blk(g_sb_static),
				g_j_ops->last_stage_log_blk(g_sb_static),
				st_blk->tx_list[i]);
		else
			dst = st_blk->tx_list[i];

		ckpt_trace("Read stage desc blk(tx_list[%u] baddr=%lu)", i,
			   dst);

		bio = alloc_bio_n_bvecs(dst, 1);
		set_bvec(bio, 0, (char *)&sdbs[i], 1);
		bio_list_add(bl, bio);
	}

	se_request_dispatch_io_sync(bl, 1);

#ifdef VALIDATE_STAGE_DESC_BLK
	// Validate the stage desc blk.
	for (i = 0; i < n_stage_txs; i++) {
		if (sdbs[i].h.blocktype != STAGE_DESC_BLK) {
			ckpt_trace(
				"Validation failed: stage desc blk(tx_list[%u] blocktype=%u baddr=%lu)",
				i, sdbs[i].h.blocktype, sdbs[i].h.inode_baddr);
			print_stage_descriptor_block(&sdbs[i]);
		}
	}
#endif

	free_bl(bl);
}

/**
 * @brief Read all etag blocks in the stage transaction.
 * 
 * @param sdb
 * @param etag_blk_start 
 * @param jebs Target buffer to store etag blocks.
 */
static void read_etag_blocks(struct stage_descriptor_block *sdb,
			     baddr_t etag_blk_start,
			     struct journal_extent_tag_block *jebs)
{
	struct bio_list *bl;
	int wrap; // Whether there is a wrap-around.
	uint64_t n_blks;
	baddr_t start;
	struct journal_extent_tag_block *dst;

	ckpt_trace("[STG] Reading etag_blks nr=%u", sdb->h.nr_etag_blks);

	wrap = check_wrap_around_stage_tx(etag_blk_start, sdb->h.nr_etag_blks);

	bl = alloc_bl();

	if (wrap) {
		// Read twice for each separate part. Use different bio for I/O
		// parallelism.

		/* First bio. (start ~ end of stage log) */
		n_blks = g_j_ops->last_stage_log_blk(g_sb_static) -
			 etag_blk_start + 1;

		ckpt_trace(
			"[STG] Read etag blks(wrap) first read: start=%lu n_blks=%u",
			etag_blk_start, n_blks);

		add_bio_to_bl(bl, (char *)jebs, etag_blk_start, n_blks);

		/* Second bio. (start of stage log ~ ) */
		start = g_j_ops->first_stage_log_blk(g_sb_static);
		dst = jebs + n_blks; // Advance pointer.
		n_blks = sdb->h.nr_etag_blks - n_blks;

		ckpt_trace(
			"[STG] Read etag blks(wrap) second read: start=%lu n_blks=%u",
			start, n_blks);

		add_bio_to_bl(bl, (char *)dst, start, n_blks);

	} else {
		// We can read all etag blocks at once.
		ckpt_trace("[STG] Read etag blks: start=%lu n_blks=%u",
			   etag_blk_start, sdb->h.nr_etag_blks);

		add_bio_to_bl(bl, (char *)jebs, etag_blk_start,
			      sdb->h.nr_etag_blks);
	}

	se_request_dispatch_io_sync(bl, 1);
	free_bl(bl);

	ckpt_trace("[STG] Reading etag_blks done. nr=%u", sdb->h.nr_etag_blks);
}

/**
 * @brief Return after all the bio lists are completed.
 * 
 * @param worker_ids 
 * @param n_bls 
 */
static void wait_bls_completion(int *worker_ids, uint32_t n_bls)
{
	oxbow_assert(0);
	// uint32_t i;
	// for (i = 0; i < n_bls; i++) {
	// 	if (worker_ids[i] == -2) {
	// 		worker_ids[i] = -3;
	// 		continue;
	// 	}
	// 	se_nvmf_poll_complete(worker_ids[i]);
	// 	worker_ids[i] = POLLED;
	// }
}

/**
 * This struct and free_ckpt_resources() function are to make the last stage tx
 * I/O be async. The last stage tx I/O is not necessary to be synchronous until
 * the next stage tx I/O is issued or the background journal checkpointing I/O
 * is issued. So, freeing is delayed until the sync point is reached and the
 * required arguments are delivered.
 */
struct free_args {
	uint32_t n_data_bufs;
	struct journal_extent_tag_block *jebs;
	char *temp_buf;
	char **bufs;
	char ***list_of_bufs;
	struct bio_list **read_bls;
	struct bio_list **write_bls;
	int *read_worker_ids;
	int *write_worker_ids;
	uint32_t sdb_nr_tags; // number of tags in the stage descriptor block.
};
static void free_ckpt_resources(struct free_args *args)
{
	uint32_t i, j, bufs_size;
	uint32_t n_data_bufs;
	uint32_t n_tags;
	struct journal_extent_tag_block *jebs;
	char **bufs;
	char ***list_of_bufs;
	struct bio_list **read_bls;
	struct bio_list **write_bls;

	// ckpt_trace("[FREE] Free args (addr: 0x%lx) values:", (uintptr_t)args);
	// ckpt_trace("  - n_data_bufs: %u", args->n_data_bufs);
	// ckpt_trace("  - sdb_nr_tags: %u", args->sdb_nr_tags);
	// ckpt_trace("  - jebs: 0x%lx", (uintptr_t)args->jebs);
	// ckpt_trace("  - list_of_bufs: 0x%lx", (uintptr_t)args->list_of_bufs);
	// ckpt_trace("  - read_bls: 0x%lx", (uintptr_t)args->read_bls);
	// ckpt_trace("  - write_bls: 0x%lx", (uintptr_t)args->write_bls);
	// ckpt_trace("  - read_worker_ids: 0x%lx",
	// 	   (uintptr_t)args->read_worker_ids);
	// ckpt_trace("  - write_worker_ids: 0x%lx",
	// 	   (uintptr_t)args->write_worker_ids);

	n_data_bufs = args->n_data_bufs;
	jebs = args->jebs;
	list_of_bufs = args->list_of_bufs;
	read_bls = args->read_bls;
	write_bls = args->write_bls;

	for (i = 0; i < n_data_bufs; i++) {
		if (i == 0)
			n_tags = args->sdb_nr_tags;
		else
			n_tags = jebs[i - 1].h.nr;

		bufs_size = n_tags + 1;

		bufs = list_of_bufs[i];

		for (j = 0; j < bufs_size; j++) {
			if (bufs[j]) {
				free(bufs[j]);
				// ckpt_info("STG Freed buffer: %p", bufs[j]);
			}
		}

		free(bufs);
		free_bl(read_bls[i]);
		free_bl(write_bls[i]);
	}

	free(list_of_bufs);
	free(read_bls);
	free(write_bls);
	free(args->read_worker_ids);
	free(args->write_worker_ids);
	if (jebs)
		free(jebs);
}

/**
 * @brief Wait for previous stage tx checkpoint completion and free resources.
 * 
 * @param f_args 
 */
static void wait_prev_stage_ckpt(struct free_args *f_args)
{
	wait_bls_completion(f_args->write_worker_ids, f_args->n_data_bufs);
	free_ckpt_resources(f_args);
	free(f_args);
}

/**
 * @brief Checkpoint inode block.
 * 
 * @param sdb 
 * @return int Worker ID. (need to call se_nvmf_release_worker() later.)
 */
static int checkpoint_inode(struct stage_descriptor_block *sdb)
{
	struct bio *bio;
	char inode_blk_buf[OXBOW_BLOCK_SIZE];
	int ret = 0;
	char *dest;
	struct bio_list *bl_read;
	struct bio_list *bl_write;

	bl_read = alloc_bl();
	bl_write = alloc_bl();

	bio = alloc_bio_n_bvecs(sdb->h.inode_baddr, 1);
	bio_list_add(bl_read, bio);

	// Read inode block.
	set_bvec(bio, 0, inode_blk_buf, 1);
	se_request_dispatch_io_sync(bl_read, 1);

	// Update inode block.
	dest = inode_blk_buf + sdb->h.inode_index * sdb->h.inode_size;
	memcpy(dest, sdb->h.inode, sdb->h.inode_size);

	// Write back to the disk.
	bio = alloc_bio_n_bvecs(sdb->h.inode_baddr, 1);
	bio_list_add(bl_write, bio);

	set_bvec(bio, 0, inode_blk_buf, 1);

#ifdef ASYNC_DISPATCH // inode.
	// This path must be re implemented.
	oxbow_assert(0);
	// Write back to the disk. (reuse bl and bio)
	while (1) {
		ret = se_request_dispatch_io_async(bl, 0);
		if (ret < 0) {
			log_warn("Dispatch write I/O failed.");
			continue;
		}
		break;
	}
#else
	se_request_dispatch_io_sync(bl_write, 0);
#endif

	ckpt_warn(
		"[STG] Checkpoint inode block. baddr=%lu index=%u (inum=%u) size=%u",
		sdb->h.inode_baddr, sdb->h.inode_index, sdb->h.inode_index + 1,
		sdb->h.inode_size);

#ifdef PRINT_DUMP_BLK
	hex_dump((char *)sdb->h.inode, sdb->h.inode_size);
#endif

	return ret; // return worker id.
}

static inline void validate_worker_ids_dispatched(int *worker_ids,
						  uint32_t n_bufs)
{
#ifdef VALIDATE_WORKER_IDS
	uint32_t i;
	for (i = 0; i < n_bufs; i++) {
		oxbow_assert(worker_ids[i] == POLLED || worker_ids[i] >= 0);
	}
#endif
}

static inline void validate_read_worker_ids_done(int *worker_ids,
						 uint32_t n_bufs)
{
#ifdef VALIDATE_WORKER_IDS
	uint32_t i;
	for (i = 0; i < n_bufs; i++) {
		oxbow_assert(worker_ids[i] == WRITE_DISPATCHED);
	}
#endif
}

static inline void validate_write_worker_ids_done(int *worker_ids,
						  uint32_t n_bufs)
{
#ifdef VALIDATE_WORKER_IDS
	uint32_t i;
	for (i = 0; i < n_bufs; i++) {
		oxbow_assert(worker_ids[i] == POLLED);
	}
#endif
}

/**
 * @brief Poll previously dispatched I/O to release worker id.
 * 
 * @param worker_ids 
 * @param buf_id 
 */
// static void try_poll_previous_io(int df_id, int *worker_ids, uint32_t buf_id)
// {
// 	uint32_t i = 0;
// 	for (i = 0; i < buf_id; i++) {
// 		// while (1) {
// 		if (worker_ids[i] >= 0) { // If dispatched
// 			if (se_nvmf_is_completed(worker_ids[i])) {
// 				se_nvmf_release_worker(worker_ids[i]);
// 				worker_ids[i] = POLLED; // Mark as done.
// 				return;
// 			}
// 		}

// 		// i++;

// 		// if (i == buf_id)
// 		// 	i = 0;
// 	}
// 	// No worker_id is released.
// 	// usleep(1);
// }

/**
 * Update and print checkpoint progress
 */
static void update_checkpoint_progress(uint64_t n_blks)
{
	g_total_bytes_processed += (uint64_t)n_blks * OXBOW_BLOCK_SIZE;

	// Show progress every 100MB
	if (g_total_bytes_processed - g_last_report_bytes >= REPORT_INTERVAL) {
		g_last_report_bytes = g_total_bytes_processed;
		ckpt_info("Checkpoint Progress: %.2f MB processed",
			  (double)g_total_bytes_processed / (1024.0 * 1024.0));
	}
}

/**
 * @brief Checkpoint a stage transaction.
 * 
 * @param sdb 
 * @param stage_start Start block address of the stage transaction (= baddr of
 * the stage descriptor block).
 * @param prev_fargs If not NULL, wait for the previous stage tx ckpt completed.
 * @param last It is the last stage tx in the stage trace block.
 * @return struct free_args* Arguments to free resources if last is true.
 * Otherwise, NULL.
 */
static struct free_args *
checkpoint_stage_tx(int df_id, struct stage_descriptor_block *sdb,
		    baddr_t stage_start, struct free_args *prev_fargs, int last)
{
	uint32_t i, j;
	struct bio_list **read_bls;
	struct bio_list **write_bls;
	int *read_worker_ids, *write_worker_ids; /* not used: NOT_USED
						  * polled: POLLED
						  * write io dispatched: WRITE_DISPATCHED
						  * allocated worker id: non-negative values
						  */
	baddr_t src_start_baddr, dst_start_baddr;
	uint32_t n_blks;
	etag_t *tags;
	char *temp_buf; // Read to and write from this buffer.
	char **bufs; // Array of temp buf pointers.
	char ***list_of_bufs; // Array of temp_bufs pointers.
	uint32_t n_data_bufs; // Number of stage data buffers
	uint32_t n_tags; // Tags in a block.
	uint32_t n_tags_processed = 0; // Total # of tags processed.
	uint32_t n_issued = 0;
	uint32_t bufs_size;
	struct journal_extent_tag_block *jebs = NULL; // Array of etag blocks.
	int wrap;
	int ret;
	int inode_worker_id;
	uint32_t n_read_polled = 0;
	struct free_args *args;
	struct free_args tmp_fargs;

	ckpt_trace("Checkpoint stage tx. stage_start(desc_blk)=%lu",
		   stage_start);

	/*
	 * Stage tx layout:
	 * | desc_blk | etag_blks ... | data_blks ... | commit_blk |
	 * 
	 * Note: desc_blk has inode which also needs to be checkpointed.
	 */

	inode_worker_id = checkpoint_inode(sdb);

	// Empty stage tx. Only updating inode.
	if (sdb->h.nr_tags == 0) {
		// OPTIMIZE: If this case occurs, it waits all the dispatched
		// I/Os which makes pipeline stalled. Let's implement in this
		// way assuming that it is rare case.

		oxbow_assert(sdb->h.nr_etag_blks == 0);

		ckpt_warn("[STG] Empty stage tx. Only updating inode.");

#ifdef ASYNC_DISPATCH
		se_nvmf_poll_complete(df_id, inode_worker_id);
		inode_worker_id = -1;
#endif

		// Wait for the previous stage tx ckpt is done.
		// It guarantees the stage transactions are checkpointed in order.
		if (prev_fargs) {
			// ckpt_trace("There is a previous fargs. Free it.");
			wait_prev_stage_ckpt(prev_fargs);
		}

		goto ret;
	}

	if (sdb->h.nr_etag_blks > 0) {
		jebs = malloc(sizeof(struct journal_extent_tag_block) *
			      sdb->h.nr_etag_blks);

		if (jebs == NULL) {
			oxb_error(
				"Failed to allocate memory for etag blocks (sdb->h.nr_etag_blks=%u).",
				sdb->h.nr_etag_blks);
			oxbow_assert(0);
		}

		// Read all etag blocks at once.
		read_etag_blocks(sdb, stage_start + 1,
				 jebs); // desc blk baddr + 1
	}

	// Number of buffers = number of tag blocks + 1 (desc block has tags)
	n_data_bufs = sdb->h.nr_etag_blks + 1;

	g_total_etag_blks_in_stg_tx_processed += sdb->h.nr_etag_blks;

	ckpt_trace("[STG] n_data_bufs (1 desc blk + %u nr_etag_blks)=%u",
		   sdb->h.nr_etag_blks, n_data_bufs);

	read_bls = malloc(sizeof(struct bio_list *) * n_data_bufs);
	write_bls = malloc(sizeof(struct bio_list *) * n_data_bufs);
	list_of_bufs = malloc(sizeof(char *) * n_data_bufs);

	read_worker_ids = malloc(n_data_bufs * sizeof(int));
	write_worker_ids = malloc(n_data_bufs * sizeof(int));

	// Set all worker ids to NOT_USED.
	memset(read_worker_ids, NOT_USED, n_data_bufs * sizeof(int));
	memset(write_worker_ids, NOT_USED, n_data_bufs * sizeof(int));

	// NOTE: All etag blocks are located after descriptor
	// block in stage tx. Data blocks follow them.
	src_start_baddr = stage_start + sdb->h.nr_etag_blks + 1 /* desc blk */;

	// For each etag(and desc) block.
	for (i = 0; i < n_data_bufs; i++) {
		read_bls[i] = alloc_bl();
		write_bls[i] = alloc_bl();

		if (i == 0) {
			n_tags = sdb->h.nr_tags;
			tags = sdb->tags;
		} else {
			n_tags = jebs[i - 1].h.nr;
			tags = jebs[i - 1].tags;
		}

		n_tags_processed += n_tags;
		g_total_tags_in_stg_tx_processed += n_tags;

		// NOTE: We allocate one more entry for the case of wrap-around.
		// Rationale: There is at most one wrap-around in a stage tx.
		// The last entry is used for the split bio.
		bufs_size = n_tags + 1;

		bufs = calloc(bufs_size, sizeof(char *));
		list_of_bufs[i] = bufs;

		// For each tag.
		for (j = 0; j < n_tags; j++) {
			dst_start_baddr = tags[j].start;
			n_blks = tags[j].cnt;

			g_total_blks_in_stg_tx_processed += n_blks;
			update_checkpoint_progress(n_blks);

			// Check whether wrap-around occurs.
			wrap = check_wrap_around_stage_tx(src_start_baddr,
							  n_blks);

			if (wrap) {
				// Split into two bios.
				/* First bio. */
				n_blks = g_j_ops->last_stage_log_blk(
						 g_sb_static) -
					 src_start_baddr + 1;
				temp_buf = malloc(nblks_to_bytes(n_blks));
				oxbow_assert(temp_buf != NULL);
				bufs[j] = temp_buf;

				add_bio_to_bl(read_bls[i], temp_buf,
					      src_start_baddr, n_blks);
				add_bio_to_bl(write_bls[i], temp_buf,
					      dst_start_baddr, n_blks);

				// Rotate.
				src_start_baddr = g_j_ops->first_stage_log_blk(
					g_sb_static);
				dst_start_baddr += n_blks;

				/* Second bio. */
				// The number of remaining blocks.
				n_blks = tags[j].cnt - n_blks;
				temp_buf = malloc(nblks_to_bytes(n_blks));
				oxbow_assert(temp_buf != NULL);

				// NOTE: Add at the end of the list.
				bufs[bufs_size - 1] = temp_buf;

				ckpt_trace(
					"[CKPT_STG] BIO: (%lu-%lu) -> (%lu-%lu), # of blks=%u",
					src_start_baddr,
					src_start_baddr + n_blks - 1,
					dst_start_baddr,
					dst_start_baddr + n_blks - 1, n_blks);

				add_bio_to_bl(read_bls[i], temp_buf,
					      src_start_baddr, n_blks);
				add_bio_to_bl(write_bls[i], temp_buf,
					      dst_start_baddr, n_blks);

				src_start_baddr += n_blks;

			} else {
				// Alloc data buffer.
				// OPTIMIZE: Remove malloc time.
				temp_buf = malloc(nblks_to_bytes(n_blks));
				oxbow_assert(temp_buf != NULL);
				bufs[j] = temp_buf;

				ckpt_trace(
					"[CKPT_STG] BIO: (%lu-%lu) -> (%lu-%lu), # of blks=%u",
					src_start_baddr,
					src_start_baddr + n_blks - 1,
					dst_start_baddr,
					dst_start_baddr + n_blks - 1, n_blks);

				add_bio_to_bl(read_bls[i], temp_buf,
					      src_start_baddr, n_blks);
				add_bio_to_bl(write_bls[i], temp_buf,
					      dst_start_baddr, n_blks);

				// Update src_start_baddr for the next tag.
				src_start_baddr += n_blks;
			}
		}

		ckpt_trace("[STG] Dispatch async read I/O of bls[%u]", i);

#ifdef ASYNC_DISPATCH // inode.                                                \
	// Poll completion and release inode worker just before dispatching the next I/O.
		if (i == 0) {
			se_nvmf_poll_complete(df_id, inode_worker_id);
			inode_worker_id = -1;
		}
#endif

#ifdef ASYNC_DISPATCH
		/* Trigger async I/O */
		// REFACTOR: make it as a function.
		while (1) {
			ret = se_dispatch_io_async(df_id, read_bls[i], 1);
			if (ret >= 0) // success.
				break;

			// failed.
			log_warn("Dispatch read I/O failed.");

			// Complete the previous I/O to release worker id.
			try_poll_previous_io(df_id, read_worker_ids, i);
		}
		read_worker_ids[i] = ret;
#else
		while (se_request_dispatch_io_sync(read_bls[i], 1) == -1) {
			ckpt_warn("Dispatch read I/O failed.");
			usleep(1);
		}
		read_worker_ids[i] = POLLED;
#endif

		// OPTIMIZE: Start write IO earlier.
		// Start write IO if anyone is ready.
		// for (k = 0; k < i; k++) {
		// 	if (se_is_completed(read_bls[k])) {
		// 		se_write_async(write_bls[k]);
		// 		n_issued++;
		// 	}
		// }

		ckpt_trace(
			"[STG] Processed etag(desc) blks=%u Processed tags=%u (total=%u)",
			i + 1, n_tags_processed,
			g_total_tags_in_stg_tx_processed);
	}

	validate_worker_ids_dispatched(read_worker_ids, n_data_bufs);

	// Wait for the previous stage tx ckpt is done.
	// It guarantees the stage transactions are checkpointed in order.
	if (prev_fargs) {
		// ckpt_trace("There is a previous fargs. Free it.");
		wait_prev_stage_ckpt(prev_fargs);
	}

	// Trigger async write I/O.
	i = 0;
	n_read_polled = 0;

#ifdef ASYNC_DISPATCH
	while (1) {
		// Until all the I/Os are issued.
		if (n_issued == n_data_bufs)
			break;

		if (read_worker_ids[i] == POLLED || // already polled.
		    se_nvmf_is_completed(df_id, read_worker_ids[i])) {
			n_read_polled++;

			// Release worker id first to guarantee there is at
			// least one worker id to be allocated.
			if (read_worker_ids[i] != POLLED)
				se_nvmf_release_worker(df_id,
						       read_worker_ids[i]);

			while (1) {
				ret = se_dispatch_io_async(df_id, write_bls[i],
							   0);

				if (ret >= 0) // success.
					break;

				// failed.
				log_warn("Dispatch write I/O failed.");

				// only when all read I/Os are polled, try to poll
				// previous write I/O to release worker id.
				if (n_read_polled == n_data_bufs)
					try_poll_previous_io(
						df_id, write_worker_ids, i);
			}
			write_worker_ids[i] = ret;
			read_worker_ids[i] = WRITE_DISPATCHED;
			n_issued++;
		}

		i++;

		// Return to the first.
		if (i == n_data_bufs)
			i = 0;
	}

	// If last, delay waiting.
	// Resources will be freed after confirming all write I/O is completed.
	if (last) {
		// ckpt_trace("It is the last stage tx. Delay freeing.");

		/* Free buffers. */
		// OPTIMIZE: We can remove this malloc.
		args = malloc(sizeof(struct free_args));
		*args = (struct free_args){ .n_data_bufs = n_data_bufs,
					    .sdb_nr_tags = sdb->h.nr_tags,
					    .jebs = jebs,
					    .list_of_bufs = list_of_bufs,
					    .read_bls = read_bls,
					    .write_bls = write_bls,
					    .read_worker_ids = read_worker_ids,
					    .write_worker_ids =
						    write_worker_ids };

		// ckpt_trace("[MALLOC] Free args (addr: 0x%lx) values:",
		// 	   (uintptr_t)args);
		// ckpt_trace("  - n_data_bufs: %u", args->n_data_bufs);
		// ckpt_trace("  - sdb_nr_tags: %u", args->sdb_nr_tags);
		// ckpt_trace("  - jebs: 0x%lx", (uintptr_t)args->jebs);
		// ckpt_trace("  - list_of_bufs: 0x%lx",
		// 	   (uintptr_t)args->list_of_bufs);
		// ckpt_trace("  - read_bls: 0x%lx", (uintptr_t)args->read_bls);
		// ckpt_trace("  - write_bls: 0x%lx", (uintptr_t)args->write_bls);
		// ckpt_trace("  - read_worker_ids: 0x%lx",
		// 	   (uintptr_t)args->read_worker_ids);
		// ckpt_trace("  - write_worker_ids: 0x%lx",
		// 	   (uintptr_t)args->write_worker_ids);

		return args;
	}

	validate_worker_ids_dispatched(write_worker_ids, n_data_bufs);

	ckpt_trace("Wait for all the write IO to be completed.");

	// Wait for all the write I/O to be completed.
	wait_bls_completion(df_id, write_worker_ids, n_data_bufs);

#else
	for (i = 0; i < n_data_bufs; i++) {
		while (se_request_dispatch_io_sync(write_bls[i], 0) == -1) {
			ckpt_warn("Dispatch write I/O failed.");
			usleep(1);
		}
		read_worker_ids[i] = WRITE_DISPATCHED;
		write_worker_ids[i] = POLLED;
	}
#endif

	validate_read_worker_ids_done(read_worker_ids, n_data_bufs);
	validate_write_worker_ids_done(write_worker_ids, n_data_bufs);

	tmp_fargs = (struct free_args){ .n_data_bufs = n_data_bufs,
					.sdb_nr_tags = sdb->h.nr_tags,
					.jebs = jebs,
					.list_of_bufs = list_of_bufs,
					.read_bls = read_bls,
					.write_bls = write_bls,
					.read_worker_ids = read_worker_ids,
					.write_worker_ids = write_worker_ids };

	// ckpt_trace("It is not the last stage tx. Free resources now.");
	free_ckpt_resources(&tmp_fargs);

	oxbow_assert(n_tags_processed > 0);

ret:
	ckpt_debug("[STG] # of tags processed: %u in this stage tx.",
		   n_tags_processed);

	return NULL;
}

/**
 * @brief Checkpoint a stage trace block. (There are multiple stage
 * transactions in a stage trace block.)
 *
 * @return struct free_args* Arguments for freeing resources.
 */
static struct free_args *
checkpoint_stage_trace_blk(int df_id, struct journal_stage_trace_block *st_blk,
			   struct free_args *prev_fargs)
{
	struct stage_descriptor_block *sdbs;
	uint32_t n_stage_txs, i;
	struct free_args *f_args;

	/* OPTIMIZE: Coalescing (No need to write if recent stage tx or
	 * background tx overwrite it.)
	 */

	n_stage_txs = st_blk->h.nr;

	g_total_stg_txs_processed += n_stage_txs;

	ckpt_trace("Checkpoint stage_trace_blk: n_stage_txs=%u", n_stage_txs);

	// Alloc buffers for stage trace blocks.
	sdbs = malloc(sizeof(struct stage_descriptor_block) * n_stage_txs);

	// Read from the stage area.
	read_stage_desc_blk(sdbs, st_blk);

	f_args = prev_fargs;

	// Checkpoint stage txs.
	for (i = 0; i < n_stage_txs; i++) {
		f_args =
			checkpoint_stage_tx(df_id, &sdbs[i], st_blk->tx_list[i],
					    f_args, i == n_stage_txs - 1);
		ckpt_trace("Processed Stage TXs: %u", i + 1);
	}

	free(sdbs);

	ckpt_trace("Checkpoint stage_trace_blk is done: n_stage_txs=%u",
		   n_stage_txs);

	return f_args;
}

/**
 * @brief  Main function doing checkpointing. In overall, this function looks
 * into the in-memory metadata (tx_meta) and copies data from journal area to
 * file system area. This version does coalescing.
 * 
 * @return int 
 */
static int do_checkpoint_coalesce(void)
{
	// From do_checkpoint()
	struct ckpt_list_entry *ce;
	etag_t *tags;
	char *cur; // pointing to the current meta block.
	baddr_t src_start_baddr;
	uint32_t n_tags; // Tags in a block.
	uint32_t n_total_blks; // Total number of blocks to handle.
	struct journal_descriptor_block *jdb;
	struct journal_extent_tag_block *jetb;
	struct journal_log *j_log;
	int df_id;

	// From checkpoint_stage_trace_blk()
	struct journal_stage_trace_block *st_blk;
	struct stage_descriptor_block *sdbs;
	uint32_t n_stage_txs;

	// From checkpoint_stage_tx()
	struct stage_descriptor_block *sdb;
	baddr_t stage_start;
	struct journal_extent_tag_block *jebs = NULL; // Array of etag blocks.
	uint32_t n_data_bufs_stage;

	// Coalescing
	struct list_head tag_list;
	struct tag_list_node *node, *node_r, *temp, *node_new, *node_prev;
	unsigned long i, j, k, l, m;
	baddr_t start, start_r;
	u32 cnt, cnt_r;
	baddr_t last_src_start_baddr_group = -1;

	// Checkpointing
	struct bio_list *read_bl;
	struct bio_list *write_bl;
	baddr_t current_src_start_baddr = 0;
	bool wrapped = false;
	uint64_t this_buf_size = 0, cur_buf_size = 0;
	struct tag_list_node *last_tag_node_with_buf = NULL;
	struct bio *bio_to_free = NULL;

	// Statistics
	u64 n_total_tags_proceed = 0;
	u64 n_total_blks_proceed = 0;
	u64 n_total_tags_created = 0;
	u64 n_total_tags_invalidated = 0;
	u64 n_total_tags_coalesced = 0;
	struct timespec time_start, time_end;
	double time_usage = 0.00;

	clock_gettime(CLOCK_MONOTONIC, &time_start);

	/**
	 *  Checkout with coalesce is done with 6 passes.
	 *
	 *  Pass 1: STB scan. Function will scan through the journaling area and build a tag
	 *                    list with all tags contained in the STB (stage tracing block).
	 *  Pass 2: Journal scan. Function will scan through the journaling area again, process
	 *                        the desc block first, and then all tag blocks, and continue
	 *                        building the tag list contained in these blocks.
	 *                        After the two passes, the tag list will be partitioned into
	 *                        groups based on the corresponding src_start_baddr, as they are
	 *                        different in the different STBs and in the journal area.
	 *  Pass 3: Optimize. Function will scan through the tag list, separate the deprecated
     *                    tag as invalid by setting the start baddr to -1.
	 *  Pass 4: Coalesce. Function will scan through the optimized tag list, and try to
	 *                    merge tags if: (1) they belong to the same group; (2) they are
	 *                    all valid (not deprecated).
	 *  Pass 5: Checkpoint. Function will scan through the coalesced tag list, and calculate
	 *                      the src_start_baddr based on the group information. Function
	 *                      will then build the bio lists with all valid tags and dispatch
	 *                      the I/O for actual checkpointing.
	 *  Pass 6: Clean up. Function will clean all the lists and buffers created.
	 */

	ce = ckpt_list_dequeue();
	if (!ce) {
		oxb_warn("No transaction to checkpoint. Free space:");
		oxb_warn("  - Journal 1 free space: %d%%",
			 journal_free_percent(&g_journal_ctx.logs[0]));
		oxb_warn("  - Journal 2 free space: %d%%",
			 journal_free_percent(&g_journal_ctx.logs[1]));
		return 0;
	}
	j_log = ce->j_log;
	df_id = j_log->df_id;

	ckpt_info("=================================================");
	ckpt_info("                CHECKPOINT STARTED                ");
	ckpt_info("=================================================");
	ckpt_info("  - Journal 0 free space: %d%%",
		  journal_free_percent(&g_journal_ctx.logs[0]));
	ckpt_info("  - Journal 1 free space: %d%%",
		  journal_free_percent(&g_journal_ctx.logs[1]));
	ckpt_info("  - Journal start baddr: %lu", ce->j_start_baddr);
	ckpt_info("  - Journal tail: %lu", ce->j_log->tail);
	ckpt_info("  - Journal head: %lu", ce->j_log->head);
	ckpt_info("  - Journal size: %lu bytes", ce->total_tx_size);
	ckpt_info("  - Transaction ID: %lu", ce->tx_meta->tx_id);
	ckpt_info("  - Used Data Fetcher: %d", ce->j_log->df_id);

#ifdef DEBUG_PRINT_JNL_TX
	print_jnl_tx(ce, (uint64_t)-1, ~0U);
#endif

	// Reset stat cnt.
	g_total_stg_trace_blks_processed = 0;
	g_total_stg_txs_processed = 0;
	g_total_etag_blks_in_stg_tx_processed = 0;
	g_total_tags_in_stg_tx_processed = 0;
	g_total_blks_in_stg_tx_processed = 0;

	g_total_etag_blks_in_bg_tx_processed = 0;
	g_total_tags_in_bg_tx_processed = 0;
	g_total_blks_in_bg_tx_processed = 0;

	// Reset progress tracking at start of checkpoint
	g_total_bytes_processed = 0;
	g_last_report_bytes = 0;

	// Add each tag into the list.
	INIT_LIST_HEAD(&tag_list);

	jdb = &ce->tx_meta->desc_blk;

	cur = ce->tx_meta->meta_start;

	// This is without the desc block. We cannot add desc block here in the STB scan.
	n_total_blks = jdb->h.nr_etag_blks + jdb->h.nr_stage_trace_blks;

	//ckpt_info("[COALESCE] there are %llu blocks (tag, stage, w/o desc) in total.", n_total_blks);

	// Pass 1 - STB scan
	ckpt_info(
		"[COALESCE] starting pass 1: STB scan and inode checkpointing.");
	for (i = 0; i < n_total_blks; i++) {
		if (!is_stage_trace_blk(cur))
			goto advance_one_block_stb;

		//ckpt_info("[COALESCE] processing on block %llu.", i);

		st_blk = (struct journal_stage_trace_block *)cur;

		// Optimize inside the stage trace block
		n_stage_txs = st_blk->h.nr;

		g_total_stg_txs_processed += n_stage_txs;

		//ckpt_info("[COALESCE] there are %llu entries in the stage trace block.", n_stage_txs);

		// Alloc buffers for stage trace blocks.
		sdbs = malloc(sizeof(struct stage_descriptor_block) *
			      n_stage_txs);
		oxbow_assert(sdbs != NULL);

		// Read from the stage area.
		read_stage_desc_blk(sdbs, st_blk);

		// Read from stage txs.
		for (k = 0; k < n_stage_txs; k++) {
			//ckpt_info("[COALESCE] processing on entry %llu in the stage trace block.", k);

			sdb = &sdbs[k];
			stage_start = st_blk->tx_list[k];

			/*
			 * Stage tx layout:
			 * | desc_blk | etag_blks ... | data_blks ... | commit_blk |
			 * 
			 * Note: desc_blk has inode which also needs to be checkpointed.
			 */
			//ckpt_info("[COALESCE] checkpointing inode for the current entry.");
			checkpoint_inode(sdb);

			// Empty stage tx. Only updating inode.
			if (sdb->h.nr_tags == 0) {
				oxbow_assert(sdb->h.nr_etag_blks == 0);

				ckpt_warn(
					"Empty stage tx. Only updating inode.");

				continue;
			}

			jebs = NULL;
			if (sdb->h.nr_etag_blks > 0) {
				jebs = malloc(
					sizeof(struct journal_extent_tag_block) *
					sdb->h.nr_etag_blks);
				oxbow_assert(jebs != NULL);

				// Read all etag blocks at once.
				read_etag_blocks(sdb, stage_start + 1,
						 jebs); // desc blk baddr + 1
			}

			// Number of buffers = number of tag blocks + 1 (desc block has tags)
			n_data_bufs_stage = sdb->h.nr_etag_blks + 1;

			g_total_etag_blks_in_stg_tx_processed +=
				sdb->h.nr_etag_blks;
			//ckpt_info("[COALESCE] there are %llu tag blocks (+desc block) in the entry.", n_data_bufs_stage);

			// NOTE: All etag blocks are located after descriptor
			// block in stage tx. Data blocks follow them.
			src_start_baddr = stage_start + sdb->h.nr_etag_blks +
					  1 /* desc blk */;
			ckpt_info("Stage TX source at %llu.", src_start_baddr);

			// For each etag(and desc) block.
			for (l = 0; l < n_data_bufs_stage; l++) {
				//ckpt_info("[COALESCE] processing on tag block %llu in the entry.", l);
				if (l == 0) {
					//ckpt_info("[COALESCE] processing the descriptor block.");
					n_tags =
						sdb->h.nr_tags; // It is safe to use n_tags and tags here
					tags = sdb->tags;
				} else {
					n_tags = jebs[l - 1].h.nr;
					tags = jebs[l - 1].tags;
				}
				//ckpt_info("[COALESCE] there are %llu tags in the tag block.", n_tags);

				g_total_tags_in_stg_tx_processed += n_tags;

				uint32_t nblks_processed = 0;

				// For each tag.
				for (m = 0; m < n_tags; m++) {
					//ckpt_info("[COALESCE] adding tag %llu in the tag block, start %llu, count %lu.", m, tags[m].start, tags[m].cnt);
					node = malloc(
						sizeof(struct tag_list_node));
					oxbow_assert(node != NULL);
					INIT_LIST_HEAD(&node->list);

					node->start = tags[m].start;
					node->cnt = tags[m].cnt;
					node->src_start_baddr = src_start_baddr;
					node->stage_tx = 1;
					node->buf = NULL;

					ckpt_trace(
						"[COALESCE] STG tags: src_baddr=(%lu-%lu) dst_baddr=(%lu-%lu) n_blks=%u",
						node->src_start_baddr +
							nblks_processed,
						node->src_start_baddr +
							nblks_processed +
							node->cnt - 1,
						node->start,
						node->start + node->cnt - 1,
						node->cnt);

					nblks_processed += tags[m].cnt;
					g_total_blks_in_stg_tx_processed +=
						tags[m].cnt;

					list_add_tail(&node->list, &tag_list);
				}
			}
			if (jebs)
				free(jebs);
		}
		free(sdbs);

		ckpt_trace("Checkpoint stage_trace_blk is done: n_stage_txs=%u",
			   n_stage_txs);

		g_total_stg_trace_blks_processed++;

		ckpt_trace("Processed Stage trace blocks: %u",
			   g_total_stg_trace_blks_processed);

	advance_one_block_stb:
		cur += OXBOW_BLOCK_SIZE;
	}

	// Pass 2 - Journal scan.
	if (jdb->h.nr_tags == 0) {
		ckpt_warn(
			"No tags in this transaction. Skipping the journal scan.");

		oxbow_assert(jdb->h.nr_etag_blks == 0);

		goto no_tag;
	}

	// Compensate the total block number with desc blk.
	n_total_blks += 1;

	// Reset cursor
	cur = ce->tx_meta->meta_start;

	// Set the data block start address. (desc block + 1)
	src_start_baddr = ce->j_start_baddr + 1;

	g_total_etag_blks_in_bg_tx_processed += jdb->h.nr_etag_blks;

	// NOTE: i == 0: handle desc block which is not in the meta buffer.
	ckpt_info("[COALESCE] starting pass 2: Journal scan.");
	for (i = 0; i < n_total_blks; i++) {
		//ckpt_info("[COALESCE] processing on block %llu.", i);

		// It is etag or desc block.
		if (i == 0) { // desc block.
			//ckpt_info("[COALESCE] processing the descriptor block.");
			n_tags = jdb->h.nr_tags;
			tags = jdb->tags;
		} else if (is_stage_trace_blk(cur)) { // stage tracing block.
			goto advance_one_block_journal;
		} else { // etag block.
			oxbow_assert(is_etag_blk(cur));
			jetb = (struct journal_extent_tag_block *)cur;
			n_tags = jetb->h.nr;
			tags = jetb->tags;
		}
		//ckpt_info("[COALESCE] there are %llu tags in the tag block.", n_tags);
		g_total_tags_in_bg_tx_processed += n_tags;

		uint32_t nblks_processed = 0;

		// For each tag.
		for (j = 0; j < n_tags; j++) {
			//ckpt_info("[COALESCE] adding tag %llu in the tag block, start %llu, count %lu.", m, tags[j].start, tags[j].cnt);
			node = malloc(sizeof(struct tag_list_node));
			oxbow_assert(node != NULL);
			INIT_LIST_HEAD(&node->list);

			node->start = tags[j].start;
			node->cnt = tags[j].cnt;
			node->src_start_baddr = src_start_baddr;
			node->stage_tx = 0;
			node->buf = NULL;

			ckpt_trace(
				"[COALESCE] JNL tags: src_baddr=(%lu-%lu) dst_baddr=(%lu-%lu) n_blks=%u",
				node->src_start_baddr + nblks_processed,
				node->src_start_baddr + nblks_processed +
					node->cnt - 1,
				node->start, node->start + node->cnt - 1,
				node->cnt);

			nblks_processed += tags[j].cnt;
			g_total_blks_in_bg_tx_processed += node->cnt;

			list_add_tail(&node->list, &tag_list);
		}

	advance_one_block_journal:
		if (i != 0) // skip desc block. It is not in the meta buffer.
			cur += OXBOW_BLOCK_SIZE;
	}

no_tag:
	// No empty tx allowed.
	oxbow_assert(g_total_stg_trace_blks_processed > 0 ||
		     g_total_tags_in_bg_tx_processed > 0);

	// Pass 3 - Optimize
	ckpt_info("[COALESCE] starting pass 3: Optimization.");
	list_for_each_entry (node, &tag_list, list) {
		start = node->start;
		cnt = node->cnt;
		node_r = node;
		list_for_each_entry_continue_reverse (node_r, &tag_list, list) {
			// Optimize the reverse tags
			start_r = node_r->start;
			cnt_r = node_r->cnt;

			// If the tag is already invalid, we skip it.
			if (node_r->start == (unsigned long)-1)
				continue;

			if (start_r + cnt_r <= start ||
			    start + cnt <= start_r) {
				/**
				 *                |* Later tag *|
				 *  |** Old tag **|
				 * -------------- OR --------------
				 *  |* Later tag *|
				 *                |** Old tag **|
				 */
				// No overlap.
				continue;
			} else if (start == start_r && cnt == cnt_r) {
				/**
				 *  |****** Later tag ******|
				 *  |******* Old tag *******|
				 *  |**** Optimized tag ****|
				 */
				// Complete overlap.
				node_r->start = -1;
				n_total_tags_invalidated++;
			} else if (start >= start_r &&
				   (start + cnt) <= (start_r + cnt_r)) {
				/**
				 *       |** Later tag **|
				 *  |************** Old tag **************|
				 *  |***||** New tag 1 **||** New tag 2 **|
				 */
				// New tag 2
				if ((start_r + cnt_r) - (start + cnt) > 0) {
					node_new = malloc(sizeof(struct tag_list_node));
					oxbow_assert(node_new != NULL);
					INIT_LIST_HEAD(&node_new->list);
					node_new->start = start + cnt;
					node_new->cnt =
						(start_r + cnt_r) - (start + cnt);
					node_new->src_start_baddr = node_r->src_start_baddr;
					node_new->stage_tx = node_r->stage_tx;
					node_new->buf = NULL;
					list_add(&node_new->list, &node_r->list);  // Insert new tag 2 first.
					n_total_tags_created++;
				}

				// New tag 1
				if (cnt > 0) {
					node_new = malloc(sizeof(struct tag_list_node));
					oxbow_assert(node_new != NULL);
					INIT_LIST_HEAD(&node_new->list);
					node_new->start = -1;
					node_new->cnt = cnt;
					node_new->src_start_baddr = node_r->src_start_baddr;
					node_new->stage_tx = node_r->stage_tx;
					node_new->buf = NULL;
					list_add(&node_new->list, &node_r->list);  // Insert new tag 1 before new tag 2.
					n_total_tags_created++;
				}

				// Optimized tag
				node_r->cnt = start - start_r;
				if (node_r->cnt == 0) {  // If blk cnt is zero, it is naturally invalid.
					node_r->start = -1;
					n_total_tags_invalidated++;
				}
			} else if (start <= start_r &&
				   (start + cnt) >= (start_r + cnt_r)) {
				/**
				 *  |****** Later tag ******|
				 *       |** Old tag **|
				 *       |*************|
				 */
				// Optimized tag
				node_r->start = -1;
				n_total_tags_invalidated++;
			} else if (start < start_r &&
				   (start + cnt) < (start_r + cnt_r)) {
				/**
				 * |** Later tag **|
				 *       |******** Old tag *******|
				 *       |*********||** New tag **|
				 */
				// New tag
				if ((start_r + cnt_r) - (start + cnt) > 0) {
					node_new = malloc(sizeof(struct tag_list_node));
					oxbow_assert(node_new != NULL);
					INIT_LIST_HEAD(&node_new->list);
					node_new->start = start + cnt;
					node_new->cnt =
						(start_r + cnt_r) - (start + cnt);
					node_new->src_start_baddr = node_r->src_start_baddr;
					node_new->stage_tx = node_r->stage_tx;
					node_new->buf = NULL;
					list_add(&node_new->list, &node_r->list);
					n_total_tags_created++;
				}

				// Optimized tag
				node_r->start = -1;
				node_r->cnt = (start + cnt) - start_r;
				n_total_tags_invalidated++;
			} else if (start_r < start &&
				   (start_r + cnt_r) < (start + cnt)) {
				/**
				 *      |****** Later tag ******|
				 *  |****** Old tag ******|
				 *  |**||**** New tag ****|
				 */
				// New tag
				if ((start_r + cnt_r) - start > 0) {
					node_new = malloc(sizeof(struct tag_list_node));
					oxbow_assert(node_new != NULL);
					INIT_LIST_HEAD(&node_new->list);
					node_new->start = -1;
					node_new->cnt = (start_r + cnt_r) - start;
					node_new->src_start_baddr = node_r->src_start_baddr;
					node_new->stage_tx = node_r->stage_tx;
					node_new->buf = NULL;
					list_add(&node_new->list, &node_r->list);
					n_total_tags_created++;
				}

				// Optimized tag
				node_r->cnt = start - start_r;
				if (node_r->cnt == 0) {
					node_r->start = -1;
					n_total_tags_invalidated++;
				}
			} else {
				fprintf(stderr,
					"FATAL: Incorrect coalesce case\n");
				abort();
			}
		}
	}

	// Pass 4 - coalesce
	ckpt_info("[COALESCE] starting pass 4: Coalescing.");
	list_for_each_entry_safe (node, temp, &tag_list, list) {
		/*
		 * Coalesce conditions:
		 * 1. They belong to the same group (same src_start_baddr).
		 * 2. They can be both invalid, as they are contiguous in src address.
		 *    We don't really care if they are contiguous in dst address, as they are invalid.
		 *    It is only used to push forward the current_src_start_baddr with the blk cnts they carry.
		 * 3. They can be both valid and contiguous in dst address.
		 *    This is the natural coalesce condition.
		 */
		
		// If the tag is the first of new tag group, we skip it.
		if (last_src_start_baddr_group != node->src_start_baddr) {
			last_src_start_baddr_group = node->src_start_baddr;
			continue;
		}

		// Try to coalesce.
		node_prev = list_entry(node->list.prev, struct tag_list_node, list);
		if (((node->start == (unsigned long)-1) && (node_prev->start == (unsigned long)-1))
			|| (((node->start != (unsigned long)-1) && (node_prev->start != (unsigned long)-1)) && (node->start == node_prev->start + node_prev->cnt))) {
			//ckpt_info("[COALESCE] found tag to coalesce: prev->start=%llu, prev->cnt=%llu, current->start=%llu.", node_prev->start, node_prev->cnt, node->start);
			node_prev->cnt += node->cnt;
			list_del(&node->list);
			free(node);

			n_total_tags_coalesced++;
		}
	}

	// Pass 5 - checkpoint
	read_bl = alloc_bl();
	write_bl = alloc_bl();
	oxbow_assert(read_bl != NULL);
	oxbow_assert(write_bl != NULL);

	last_src_start_baddr_group = -1;

	ckpt_info("[COALESCE] starting pass 5: Checkpointing.");
	ckpt_info("[COALESCE] current max BIO buffer size is %llu MB.", OPTIMIZE_CHECKPOINT_MAX_BUF_SIZE >> 20);
	list_for_each_entry (node, &tag_list, list) {
		//ckpt_info("[COALESCE] get tag in the tag list: start=%llu, cnt=%llu, src_start=%llu", node->start, node->cnt, node->src_start_baddr);

		// If the blk cnt is zero, we can safely skip it here.
		// Invalidated tag (if they carry a blk cnt) must continue.
		if (node->cnt == 0)
			continue;

		// Determine the actual src_start_baddr.
		// No matter the tag is valid or not, they must contribute.
		if (last_src_start_baddr_group != node->src_start_baddr) {
			// Reset the current src_start_baddr.
			last_src_start_baddr_group = node->src_start_baddr;
			current_src_start_baddr = last_src_start_baddr_group;
		}
		node->src_start_baddr = current_src_start_baddr;

		// Check whether wrap-around occurs. If so, we break it into two tags.
		wrapped = false;
		if (node->stage_tx) {
			if (check_wrap_around_stage_tx(current_src_start_baddr,
						       node->cnt)) {
				//ckpt_info("[COALESCE] stage tx wrap around detected: src_start=%llu, cnt=%llu", current_src_start_baddr, node->cnt);

				// First tag.
				cnt = node->cnt;
				node->cnt = g_j_ops->last_stage_log_blk(g_sb_static) - current_src_start_baddr + 1;

				// Rotate
				current_src_start_baddr = g_j_ops->first_stage_log_blk(g_sb_static);
				wrapped = true;
			}
		} else {
			if (check_wrap_around_journal_tx(current_src_start_baddr,
							 node->cnt, j_log)) {
				//ckpt_info("[COALESCE] journal tx wrap around detected: src_start=%llu, cnt=%llu", current_src_start_baddr, node->cnt);

				// First tag.
				cnt = node->cnt;
				node->cnt = j_log->last - current_src_start_baddr + 1;

				// Rotate
				current_src_start_baddr = j_log->first;
				wrapped = true;
			}
		}

		// If wrapped and new tag blk cnt > 0, create the second tag.
		if (wrapped && cnt - node->cnt > 0) {
			node_new = malloc(sizeof(struct tag_list_node));
			oxbow_assert(node_new != NULL);
			INIT_LIST_HEAD(&node_new->list);
			node_new->start = node->start == (unsigned long)-1 ? -1 : node->start + node->cnt;
			node_new->cnt = cnt - node->cnt;
			node_new->src_start_baddr = last_src_start_baddr_group;  // New node still belongs to the same group.
			node_new->stage_tx = node->stage_tx;
			node_new->buf = NULL;
			list_add(&node_new->list, &node->list);

			if (node->cnt == 0) {  // If blk cnt is zero, it is naturally invalid.
				node->start = -1;
				n_total_tags_invalidated++;
			}
		}
		
		// If not wrapped, move the current src_start_baddr forward.
		// If wrapped, current_src_start_baddr has been updated.
		if (!wrapped) {
			current_src_start_baddr += node->cnt;
		}

		// If the tag is invalid, we can now skip it here.
		if (node->start == (unsigned long)-1)
			continue;

		ckpt_trace(
			"[CKPT] BIO: src_baddr=(%lu-%lu) -> dst_baddr=(%lu-%lu), n_blks=%u",
			node->src_start_baddr,
			node->src_start_baddr + node->cnt - 1, node->start,
			node->start + node->cnt - 1, node->cnt);

		n_total_tags_proceed++;
		n_total_blks_proceed += node->cnt;
		update_checkpoint_progress(node->cnt);

		this_buf_size = nblks_to_bytes(node->cnt);
		cur_buf_size += this_buf_size;
		node->buf = malloc(this_buf_size);
		oxbow_assert(node->buf != NULL); // Malloc failed.

		// Save this node if last node ptr is NULL and this node has a buffer.
		if (last_tag_node_with_buf == NULL)
			last_tag_node_with_buf = node;

		add_bio_to_bl(read_bl, node->buf, node->src_start_baddr,
			      node->cnt);
		add_bio_to_bl(write_bl, node->buf, node->start, node->cnt);

		// If the current buffer size exceeds the max limit, we dispatch I/O now.
		if (cur_buf_size >= OPTIMIZE_CHECKPOINT_MAX_BUF_SIZE) {
			// OPTIMIZE: async dispatch?
			ckpt_info("[COALESCE] dispatching I/Os due to max buffer size limit.");
			while (se_request_dispatch_io_sync(read_bl, 1) == -1) {
				ckpt_warn("Dispatch read I/O failed.");
				usleep(1);
			}
			while (se_request_dispatch_io_sync(write_bl, 0) == -1) {
				ckpt_warn("Dispatch write I/O failed.");
				usleep(1);
			}

			oxbow_assert(last_tag_node_with_buf != NULL);

			// Free up memory.
			list_for_each_entry_from(last_tag_node_with_buf, &tag_list, list) {
				if (last_tag_node_with_buf->buf) {
					free(last_tag_node_with_buf->buf);
					last_tag_node_with_buf->buf = NULL;
				}
				if (last_tag_node_with_buf == node)
					break;
			}

			// Reset tracking.
			last_tag_node_with_buf = NULL;
			cur_buf_size = 0;

			// Free bl and alloc new ones.
			while (!bio_list_empty(read_bl)) {
				bio_to_free = bio_list_pop(read_bl);
				free_bio(bio_to_free);
			}
			while (!bio_list_empty(write_bl)) {
				bio_to_free = bio_list_pop(write_bl);
				free_bio(bio_to_free);
			}

			ckpt_info("[COALESCE] buffer memory freed. Continuing checkpointing.");
		}
	}

	// Dispatch I/Os.
	ckpt_info("[COALESCE] pass 5 finished, dispatching final I/Os.");
	if (!bio_list_empty(read_bl)) {
		while (se_request_dispatch_io_sync(read_bl, 1) == -1) {
			ckpt_warn("Dispatch read I/O failed.");
			usleep(1);
		}
	}
	if (!bio_list_empty(write_bl)) {
		while (se_request_dispatch_io_sync(write_bl, 0) == -1) {
			ckpt_warn("Dispatch write I/O failed.");
			usleep(1);
		}
	}

	// Pass 6 - clean up
	ckpt_info("[COALESCE] starting pass 6: Cleaning up.");
	list_for_each_entry_safe (node, temp, &tag_list, list) {
		list_del(&node->list);
		if (node->buf)
			free(node->buf);
		free(node);
	}
	free_bl(read_bl);
	free_bl(write_bl);

	// Update (the least) tx_id of the in-memory super block.
	increase_journal_sb_tx_id(ce->tx_meta->tx_id);

	// Persist superblock and free journal space.
	//
	// Note that tx_id should be updated before this is called.
	// free_journal_space will call sync_journal_sb() internally which sync
	// in-memory superblock state to disk.
	free_journal_space(ce->j_log, ce->j_start_baddr, ce->total_tx_size, 1);

	clock_gettime(CLOCK_MONOTONIC, &time_end);
	time_usage = (time_end.tv_sec - time_start.tv_sec) +
		     (time_end.tv_nsec - time_start.tv_nsec) / 1e9;

	///////////////////////////////////////////////////
	/* Checkpoint is completed. Journal is released. */
	///////////////////////////////////////////////////
	ckpt_info(
		"%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%");
	ckpt_info(
		" CHECKPOINT WITH COALESCING COMPLETED. Journal space released:");
	ckpt_info(
		"%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%");
	ckpt_info("  - This journal: Journal %d", df_id);
	ckpt_info("  - Journal 0 free space: %d%%",
		  journal_free_percent(&g_journal_ctx.logs[0]));
	ckpt_info("  - Journal 1 free space: %d%%",
		  journal_free_percent(&g_journal_ctx.logs[1]));
	ckpt_info("  - Journal start baddr: %lu", ce->j_start_baddr);
	ckpt_info("  - Journal size: %lu bytes (%.2f KB, %.2f MB)",
		  ce->total_tx_size, (double)ce->total_tx_size / 1024.0,
		  (double)ce->total_tx_size / (1024.0 * 1024.0));
	ckpt_info("  - Transaction ID: %lu", ce->tx_meta->tx_id);
	ckpt_info("  - Processed:");
	ckpt_info("    - Stage trace blocks: %lu",
		  g_total_stg_trace_blks_processed);
	ckpt_info("    - Stage transactions: %lu", g_total_stg_txs_processed);
	ckpt_info("    - Stage tx etag blocks: %lu",
		  g_total_etag_blks_in_stg_tx_processed);
	ckpt_info("    - Stage tx tags: %lu", g_total_tags_in_stg_tx_processed);
	ckpt_info("    - Stage tx blocks: %lu",
		  g_total_blks_in_stg_tx_processed);
	ckpt_info("    - BG tx etag blocks: %lu",
		  g_total_etag_blks_in_bg_tx_processed);
	ckpt_info("    - BG tx tags: %lu", g_total_tags_in_bg_tx_processed);
	ckpt_info("    - BG tx blocks: %lu", g_total_blks_in_bg_tx_processed);
	ckpt_info("  - Coalescing Savings:");
	ckpt_info("    - Actual tx tags after coalescing: %lu",
		  n_total_tags_proceed);
	ckpt_info("    - Actual tx blocks after coalescing: %lu",
		  n_total_blks_proceed);
	ckpt_info("    - Tags saved after coalescing: %lu",
		  g_total_tags_in_stg_tx_processed +
			  g_total_tags_in_bg_tx_processed -
			  n_total_tags_proceed);
	ckpt_info("    - Blocks saved after coalescing: %lu",
		  g_total_blks_in_stg_tx_processed +
			  g_total_blks_in_bg_tx_processed -
			  n_total_blks_proceed);
	ckpt_info("    - Tags created during coalescing: %lu",
		  n_total_tags_created);
	ckpt_info("    - Tags invaldated during coalescing: %lu",
		  n_total_tags_invalidated);
	ckpt_info("    - Tags coalesced during coalescing: %lu",
		  n_total_tags_coalesced);
	ckpt_info("  - Total runtime (seconds): %f", time_usage);

	return 0;
}

/**
 * @brief  Main function doing checkpointing. In overall, this function looks
 * into the in-memory metadata (tx_meta) and copies data from journal area to
 * file system area.
 * 
 * @return int 
 */
static int do_checkpoint(void)
{
	struct ckpt_list_entry *ce;
	// struct bio_list *first_desc_bl;
	// uint32_t n_desc_blks;
	// struct journal_descriptor_block *jdh;
	uint32_t buf_id, j;
	long i;
	// uint32_t k;
	struct bio_list **read_bls;
	struct bio_list **write_bls;
	int *read_worker_ids, *write_worker_ids; /* not used: NOT_USED
						  * polled: POLLED
						  * write io dispatched: WRITE_DISPATCHED
						  * allocated worker id: non-negative values
						  */
	baddr_t src_start_baddr, dst_start_baddr;
	uint32_t n_blks;
	etag_t *tags;
	char *cur, *tmp_cur; // pointing to the current meta block.
	char *temp_buf;
	char **bufs; // Array of temp buf pointers.
	char ***list_of_bufs; // Array of temp_bufs pointers.
	uint32_t n_data_bufs; // Number of I/O buffers
	uint32_t n_tags; // Tags in a block.
	uint32_t n_issued = 0;
	uint32_t bufs_size;
	uint32_t n_total_blks; // Total number of blocks to handle.
	uint32_t n_stb_at_beginning = 0;
	int wrap;
	int pending_desc_tags = 0;
	int ret;
	int no_tag = 0;
	int df_id;
	uint32_t n_read_polled;
	struct journal_descriptor_block *jdb;
	struct journal_extent_tag_block *jetb;
	struct free_args *f_args = NULL; /* Arguments for freeing resources. 
					  * Non-null indicates that there are issued I/O to wait for.
					  */
	struct journal_log *j_log;
	struct timespec time_start, time_end;
	double time_usage = 0.00;

	clock_gettime(CLOCK_MONOTONIC, &time_start);

	ce = ckpt_list_dequeue();
	j_log = ce->j_log;
	df_id = j_log->df_id;

	if (!ce) {
		oxb_warn("No transaction to checkpoint. Free space:");
		oxb_warn("  - Journal 1 free space: %d%%",
			 journal_free_percent(&g_journal_ctx.logs[0]));
		oxb_warn("  - Journal 2 free space: %d%%",
			 journal_free_percent(&g_journal_ctx.logs[1]));
		return 0;
	}
	ckpt_info("=================================================");
	ckpt_info("                CHECKPOINT STARTED                ");
	ckpt_info("=================================================");
	ckpt_info("  - Journal 0 free space: %d%%",
		  journal_free_percent(&g_journal_ctx.logs[0]));
	ckpt_info("  - Journal 1 free space: %d%%",
		  journal_free_percent(&g_journal_ctx.logs[1]));
	ckpt_info("  - Journal start baddr: %lu", ce->j_start_baddr);
	ckpt_info("  - Journal tail: %lu", ce->j_log->tail);
	ckpt_info("  - Journal head: %lu", ce->j_log->head);
	ckpt_info("  - Journal size: %lu bytes", ce->total_tx_size);
	ckpt_info("  - Transaction ID: %lu", ce->tx_meta->tx_id);
	ckpt_info("  - Used Data Fetcher: %d", ce->j_log->df_id);

	// Reset stat cnt.
	g_total_stg_trace_blks_processed = 0;
	g_total_stg_txs_processed = 0;
	g_total_etag_blks_in_stg_tx_processed = 0;
	g_total_tags_in_stg_tx_processed = 0;
	g_total_blks_in_stg_tx_processed = 0;

	g_total_etag_blks_in_bg_tx_processed = 0;
	g_total_tags_in_bg_tx_processed = 0;
	g_total_blks_in_bg_tx_processed = 0;

	// Reset progress tracking at start of checkpoint
	g_total_bytes_processed = 0;
	g_last_report_bytes = 0;

	jdb = &ce->tx_meta->desc_blk;

	if (jdb->h.nr_tags == 0) {
		ckpt_warn(
			"No tags in this transaction. There is only stage trace block.");

		oxbow_assert(jdb->h.nr_etag_blks == 0);

		no_tag = 1;
		n_data_bufs = 0;
	} else {
		// Number of buffers = number of tag blocks + 1 (desc block has tags)
		n_data_bufs = jdb->h.nr_etag_blks + 1;
	}

	ckpt_trace("n_data_bufs(nr_etag_blks + 1)=%u", n_data_bufs);

	/*
	 * Each tag has a starting block address and length (# of contiguous blocks).
	 * Hence, each tag can be represented by a bio that has one bio_vec.
	 * We are going to create:
	 * 	One bl for one etag block (or descriptor block).
	 * 	One bio for one etag.
	*/
	read_bls = malloc(sizeof(struct bio_list *) * n_data_bufs);
	write_bls = malloc(sizeof(struct bio_list *) * n_data_bufs);
	list_of_bufs = malloc(sizeof(char *) * n_data_bufs);

	read_worker_ids = malloc(n_data_bufs * sizeof(int));
	write_worker_ids = malloc(n_data_bufs * sizeof(int));

	// Set all worker ids to NOT_USED.
	memset(read_worker_ids, NOT_USED, n_data_bufs * sizeof(int));
	memset(write_worker_ids, NOT_USED, n_data_bufs * sizeof(int));

	cur = ce->tx_meta->meta_start;

	// Checking how many stage trace blocks are at the beginning.
	tmp_cur = cur;
	while (is_stage_trace_blk(tmp_cur)) {
		tmp_cur += OXBOW_BLOCK_SIZE;
		n_stb_at_beginning++;
	}
	ckpt_info(
		"Consecutive stage trace blocks at the beginning: n_stb_at_beginning=%u",
		n_stb_at_beginning);

	// Set the data block start address. (desc block + 1)
	src_start_baddr = ce->j_start_baddr + 1;

	n_total_blks = jdb->h.nr_etag_blks + jdb->h.nr_stage_trace_blks +
		       1 /* desc blk */;
	buf_id = 0;

	g_total_etag_blks_in_bg_tx_processed += jdb->h.nr_etag_blks;

	// For each meta block and desc block.
	// NOTE: i == 0: handle desc block which is not in the meta buffer.
	for (i = 0; i < n_total_blks; i++) {
		if (i == 0) {
			// NOTE: If the first metadata block is a stage trace
			// block, we have to process it before processing tags
			// in the descriptor block.
			if (is_stage_trace_blk(cur)) {
				pending_desc_tags = 1;
				continue; // without advancing cur.
			}

		} else if (is_stage_trace_blk(cur)) {
			// NOTE: Checkpoint order should be preserved:
			// Checkpoint stage first than the other data in a file.
			// ckpt_debug("f_args: 0x%lx", (uintptr_t)f_args);
			f_args = checkpoint_stage_trace_blk(
				df_id, (struct journal_stage_trace_block *)cur,
				f_args);
			g_total_stg_trace_blks_processed++;
			ckpt_trace("Processed Stage trace blocks: %u",
				   g_total_stg_trace_blks_processed);

			ckpt_info(
				"Processed Stage trace blocks: %u i=%u src_baddr=%lu, cur=0x%lx",
				g_total_stg_trace_blks_processed, i,
				src_start_baddr, cur);

			if (pending_desc_tags) {
				// Temporary reset the i and src_start_baddr to process
				// desc block (i == 0). They are restored at the
				// end of the loop.
				i = 0;
				src_start_baddr = ce->j_start_baddr + 1;
				ckpt_info(
					"pending_desc_tags is 1. Restore i to 0 and process tags in j desc blocks.");
			} else {
				// Increment src_start_baddr due to a journal
				// stage trace block.
				goto advance_one_block;
			}
		}

		if (no_tag)
			goto skip_read_io;

		// It is etag or desc block.

		read_bls[buf_id] = alloc_bl();
		write_bls[buf_id] = alloc_bl();

		if (i == 0) { // desc block.
			n_tags = jdb->h.nr_tags;
			tags = jdb->tags;

		} else { // etag block.
			oxbow_assert(is_etag_blk(cur));

			jetb = (struct journal_extent_tag_block *)cur;
			n_tags = jetb->h.nr;
			tags = jetb->tags;
		}

		g_total_tags_in_bg_tx_processed += n_tags;

		// NOTE: We allocate one more entry for the case of wrap-around.
		// Rationale: There is at most one wrap-around in a tx log.
		// The last entry is used for the split bio.
		bufs_size = n_tags + 1;

		bufs = calloc(bufs_size, sizeof(char *));
		list_of_bufs[buf_id] = bufs;

		// For each tag.
		for (j = 0; j < n_tags; j++) {
			dst_start_baddr = tags[j].start;
			n_blks = tags[j].cnt;

			g_total_blks_in_bg_tx_processed += n_blks;
			update_checkpoint_progress(n_blks);

			// Check whether wrap-around occurs.
			wrap = check_wrap_around_journal_tx(src_start_baddr,
							    n_blks, j_log);

			if (wrap) {
				// Split into two bios.
				/* First bio. */
				n_blks = j_log->last - src_start_baddr + 1;

				temp_buf = malloc(nblks_to_bytes(n_blks));
				oxbow_assert(temp_buf !=
					     NULL); // Malloc failed.
				bufs[j] = temp_buf;

				add_bio_to_bl(read_bls[buf_id], temp_buf,
					      src_start_baddr, n_blks);
				add_bio_to_bl(write_bls[buf_id], temp_buf,
					      dst_start_baddr, n_blks);

				// Rotate
				src_start_baddr = j_log->first;
				dst_start_baddr += n_blks;

				/* Second bio. */
				// The number of remaining blocks.
				n_blks = tags[j].cnt - n_blks;
				temp_buf = malloc(nblks_to_bytes(n_blks));
				oxbow_assert(temp_buf !=
					     NULL); // Malloc failed.

				// NOTE: Add at the end of the list.
				bufs[bufs_size - 1] = temp_buf;

				ckpt_trace(
					"[CKPT_JNL] BIO: (%lu-%lu) -> (%lu-%lu), # of blks=%u",
					src_start_baddr,
					src_start_baddr + n_blks - 1,
					dst_start_baddr,
					dst_start_baddr + n_blks - 1, n_blks);

				add_bio_to_bl(read_bls[buf_id], temp_buf,
					      src_start_baddr, n_blks);
				add_bio_to_bl(write_bls[buf_id], temp_buf,
					      dst_start_baddr, n_blks);

				src_start_baddr += n_blks;

			} else {
				// Alloc data buffer.
				// OPTIMIZE: Remove malloc time.
				temp_buf = malloc(nblks_to_bytes(n_blks));
				oxbow_assert(temp_buf !=
					     NULL); // Malloc failed.
				bufs[j] = temp_buf;

				ckpt_trace(
					"[CKPT_JNL] BIO: (%lu-%lu) -> (%lu-%lu), # of blks=%u",
					src_start_baddr,
					src_start_baddr + n_blks - 1,
					dst_start_baddr,
					dst_start_baddr + n_blks - 1, n_blks);

				/* OPTIMIZE: Split a large bl and pipelining.
				*  1) Free temp_bufs early.
				*  2) We can start I/O before all the bio lists are constructed. (v)
				*  3) We can start the writes before all the reads are
				*     done.
				* The same optimization can be applied for the stage checkpoint.
				*/
				add_bio_to_bl(read_bls[buf_id], temp_buf,
					      src_start_baddr, n_blks);
				add_bio_to_bl(write_bls[buf_id], temp_buf,
					      dst_start_baddr, n_blks);

				// Update src_start_baddr for the next tag.
				src_start_baddr += n_blks;
			}
		}

		ckpt_trace("Dispatch async read I/O of bls[%u]", i);

		/* OPTIMIZE: Trigger async IO as soon as bl is ready.
		 * 	     Write IO should start after read IO is completed.
		 * 	     Refer to the implementation below.
		 */

#ifdef ASYNC_DISPATCH
		/* Trigger async I/O. */
		// Read from journal log.
		while (1) {
			ret = se_dispatch_io_async(df_id, read_bls[buf_id], 1);
			if (ret >= 0) // success.
				break;

			// failed.
			log_warn("Dispatch read I/O failed.");

			// Complete the previous I/O to release worker id.
			try_poll_previous_io(df_id, read_worker_ids, buf_id);
		}
		read_worker_ids[buf_id] = ret;
#else
		while (se_request_dispatch_io_sync(read_bls[buf_id], 1) == -1) {
			ckpt_warn("Dispatch read I/O failed.");
			usleep(1);
		}
		read_worker_ids[buf_id] = POLLED;
#endif
		// ckpt_trace("read_worker_ids[%u]=%d", buf_id, ret);

		// OPTIMIZE: Start write IO earlier.
		// Start write IO if anyone is ready.
		// for (k = 0; k < i; k++) {
		// 	if (se_is_completed(read_bls[k])) {
		// 		se_write_async(write_bls[k]);
		// 		n_issued++;
		// 	}
		// }

		buf_id++;

		if (pending_desc_tags) {
			// Restore i and src_start_baddr.
			i = 1;
			pending_desc_tags = 0;
		}

	advance_one_block:
		if (i != 0) // skip desc block. It is not in the meta buffer.
			cur += OXBOW_BLOCK_SIZE;
	}

	validate_worker_ids_dispatched(read_worker_ids, n_data_bufs);

skip_read_io:
	oxbow_assert(buf_id == n_data_bufs);

	// No empty tx allowed.
	oxbow_assert(g_total_stg_trace_blks_processed > 0 ||
		     g_total_tags_in_bg_tx_processed > 0);

	// Stage checkpoint should be done before issuing bg journal checkpoint
	// because, otherwise, some blocks can be overwritten by the background
	// journal.
	//
	// Wait for completion of all write I/Os issued during stage checkpoint.
	//
	// It guarantees the stage transactions are checkpointed before
	// background journal checkpointing.
	if (f_args) {
		// ckpt_trace("There is a previous fargs. Free it.");
		wait_prev_stage_ckpt(f_args);
		f_args = NULL;
	}

	if (no_tag)
		goto skip_wait_write_io;

	// Trigger async write I/O.
	buf_id = 0;
	n_read_polled = 0;

#ifdef ASYNC_DISPATCH
	while (1) {
		// Until all the I/Os are issued.
		if (n_issued == n_data_bufs)
			break;

		if (read_worker_ids[buf_id] == POLLED || // already polled.
		    se_nvmf_is_completed(df_id, read_worker_ids[buf_id])) {
			n_read_polled++;

			// Release worker id first to guarantee there is at
			// least one worker id to be allocated.
			if (read_worker_ids[buf_id] != POLLED)
				se_nvmf_release_worker(df_id,
						       read_worker_ids[buf_id]);

			while (1) {
				ret = se_dispatch_io_async(
					df_id, write_bls[buf_id], 0);
				if (ret >= 0) // success.
					break;

				// failed.
				log_warn("Dispatch write I/O failed.");

				// only when all read I/Os are polled, try to poll previous
				// write I/O to release worker id.
				if (n_read_polled == n_data_bufs)
					try_poll_previous_io(df_id,
							     write_worker_ids,
							     buf_id);
			}
			write_worker_ids[buf_id] = ret;
			read_worker_ids[buf_id] = WRITE_DISPATCHED;
			n_issued++;
		}

		buf_id++;

		// Return to the first.
		if (buf_id == n_data_bufs)
			buf_id = 0;
	}

	validate_worker_ids_dispatched(write_worker_ids, n_data_bufs);

	ckpt_trace("Wait for all the write IO to be completed.");

	// Wait for all the write I/O to be completed.
	wait_bls_completion(df_id, write_worker_ids, n_data_bufs);

#else
	for (i = 0; i < n_data_bufs; i++) {
		while (se_request_dispatch_io_sync(write_bls[i], 0) == -1) {
			ckpt_warn("Dispatch write I/O failed.");
			usleep(1);
		}
		read_worker_ids[i] = WRITE_DISPATCHED;
		write_worker_ids[i] = POLLED;
	}
#endif
	validate_read_worker_ids_done(read_worker_ids, n_data_bufs);
	validate_write_worker_ids_done(write_worker_ids, n_data_bufs);

	ckpt_trace("All write IO Done.");

skip_wait_write_io:

	// Update (the least) tx_id of the in-memory super block.
	increase_journal_sb_tx_id(ce->tx_meta->tx_id);

	// Persist superblock and free journal space.
	//
	// Note that tx_id should be updated before this is called.
	// free_journal_space will call sync_journal_sb() internally which sync
	// in-memory superblock state to disk.
	free_journal_space(ce->j_log, ce->j_start_baddr, ce->total_tx_size, 1);

	clock_gettime(CLOCK_MONOTONIC, &time_end);
	time_usage = (time_end.tv_sec - time_start.tv_sec) +
		     (time_end.tv_nsec - time_start.tv_nsec) / 1e9;

	///////////////////////////////////////////////////
	/* Checkpoint is completed. Journal is released. */
	///////////////////////////////////////////////////
	ckpt_info("%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%");
	ckpt_info("  CHECKPOINT COMPLETED. Journal space released:");
	ckpt_info("%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%");
	ckpt_info("  - This journal: Journal %d", df_id);
	ckpt_info("  - Journal 0 free space: %d%%",
		  journal_free_percent(&g_journal_ctx.logs[0]));
	ckpt_info("  - Journal 1 free space: %d%%",
		  journal_free_percent(&g_journal_ctx.logs[1]));
	ckpt_info("  - Journal start baddr: %lu", ce->j_start_baddr);
	ckpt_info("  - Journal tail: %lu", ce->j_log->tail);
	ckpt_info("  - Journal head: %lu", ce->j_log->head);
	ckpt_info("  - Journal size: %lu bytes (%.2f KB, %.2f MB)",
		  ce->total_tx_size, (double)ce->total_tx_size / 1024.0,
		  (double)ce->total_tx_size / (1024.0 * 1024.0));
	ckpt_info("  - Transaction ID: %lu", ce->tx_meta->tx_id);
	ckpt_info("  - Processed:");
	ckpt_info("    - Stage trace blocks: %lu",
		  g_total_stg_trace_blks_processed);
	ckpt_info("    - Stage transactions: %lu", g_total_stg_txs_processed);
	ckpt_info("    - Stage tx etag blocks: %lu",
		  g_total_etag_blks_in_stg_tx_processed);
	ckpt_info("    - Stage tx tags: %lu", g_total_tags_in_stg_tx_processed);
	ckpt_info("    - Stage tx blocks: %lu",
		  g_total_blks_in_stg_tx_processed);
	ckpt_info("    - BG tx etag blocks: %lu",
		  g_total_etag_blks_in_bg_tx_processed);
	ckpt_info("    - BG tx tags: %lu", g_total_tags_in_bg_tx_processed);
	ckpt_info("    - BG tx blocks: %lu", g_total_blks_in_bg_tx_processed);
	ckpt_info("  - Total runtime (seconds): %f", time_usage);

	if (no_tag)
		goto skip_free_buffers;

	/* Free buffers. */
	buf_id = 0;
	cur = ce->tx_meta->meta_start;
	for (i = 0; i < n_total_blks; i++) {
		if (i != 0 && is_stage_trace_blk(cur)) {
			goto advance_one_block2;
		}

		if (i == 0) // desc block.
			n_tags = jdb->h.nr_tags;
		else { // etag block.
			jetb = (struct journal_extent_tag_block *)cur;
			n_tags = jetb->h.nr;
		}

		bufs_size = n_tags + 1;

		bufs = list_of_bufs[buf_id];

		for (j = 0; j < bufs_size; j++) {
			if (bufs[j]) {
				free(bufs[j]);
				// ckpt_info("STG Freed buffer: %p", bufs[j]);
			}
		}

		free(bufs);
		free_bl(read_bls[buf_id]);
		free_bl(write_bls[buf_id]);

		buf_id++;

	advance_one_block2:
		if (i != 0) // skip desc block. It is not in the meta buffer.
			cur += OXBOW_BLOCK_SIZE;
	}

	free(list_of_bufs);
	free(read_bls);
	free(write_bls);
	free(read_worker_ids);
	free(write_worker_ids);

skip_free_buffers:
	// Free the checkpoint list entry.
	free_ckpt_list_entry(ce);

	return 0;
}

static int lower_journal_free_percent(void)
{
	int free_perc1, free_perc2;

	free_perc1 = journal_free_percent(&g_journal_ctx.logs[0]);
	free_perc2 = journal_free_percent(&g_journal_ctx.logs[1]);

	return min(free_perc1, free_perc2);
}

/**
 * @brief Start stage area checkpoint to free the requested number of blocks.
 *
 * This is enqueued to ckpt_thpool from handle_stg_ckpt().
 * The caller passes a struct stg_ckpt_arg via @arg.
 * After completion, nr_blks_freed is updated and done semaphore is posted.
 * The caller is responsible for freeing arg.
 *
 * @param arg Pointer to struct stg_ckpt_arg.
 */
void start_stg_checkpoint(void *arg)
{
	struct stg_ckpt_arg *ckpt_arg = (struct stg_ckpt_arg *)arg;
	uint32_t nr_stg_blks_to_free = ckpt_arg->nr_blks_to_free;

	oxb_info("[STG CKPT] start_stg_checkpoint: nr_stg_blks_to_free=%u",
		 nr_stg_blks_to_free);

	/* TODO: Implement stage area checkpoint logic. */
// #ifdef OPTIMIZE_CHECKPOINT
// 		nr_stg_blks_to_free = do_checkpoint_coalesce();
// #else
// 		nr_stg_blks_to_free = do_checkpoint();
// #endif

	// TODO: Set number of blocks actually freed.
	atomic_store(&ckpt_arg->nr_blks_freed, nr_stg_blks_to_free);
	sem_post(ckpt_arg->done);
}

/**
 * @brief
 * 
 * @param arg Target percentage up to release the space. Between 0 and 100. For
 * example, 30 means do checkpointing until 30% of total journal space is free.
 */
void start_checkpoint(void *arg)
{
	int target_perc;

	target_perc = (int)(intptr_t)arg;

	EXP_FLAG_WRITE(FLAG_FILE_CKPT_DONE, "0");

	while (lower_journal_free_percent() < target_perc) {
#ifdef OPTIMIZE_CHECKPOINT
		do_checkpoint_coalesce();
#else
		do_checkpoint();
#endif
	}

	EXP_FLAG_WRITE(FLAG_FILE_CKPT_DONE, "1");
}
