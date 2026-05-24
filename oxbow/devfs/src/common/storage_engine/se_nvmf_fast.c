#define _GNU_SOURCE
#include <unistd.h>

#include "spdk/nvme.h"
#include "spdk/likely.h"
#include "spdk/util.h"
#include "storage_engine.h"
#include "global.h"
#include "oxbow_debug.h"
#include "se_nvmf.h"
#include "thpool.h"
#include "profile_devfs.h"
#include "config.h"
#include "spdk/stdinc.h"

#include "spdk/config.h"
#include "spdk/env.h"
#include "spdk/fd.h"
#include "spdk/nvme.h"
#include "spdk/vmd.h"
#include "spdk/queue.h"
#include "spdk/string.h"
#include "spdk/nvme_intel.h"
#include "spdk/histogram_data.h"
#include "spdk/endian.h"
#include "spdk/dif.h"
#include "spdk/util.h"
#include "spdk/log.h"
#include "spdk/likely.h"
#include "spdk/sock.h"
#include "spdk/zipf.h"
#include "spdk/nvmf.h"
#include "bit_array.h"
#include <pthread.h>
#include <stdatomic.h>
#include <stddef.h>
#include <string.h>

// #define DEVFS_TID_VALIDATE
#ifdef DEVFS_TID_VALIDATE
static uint32_t g_tid_validate_table[16] = { 0 };
#endif

/*
 * Outstanding write inflight cap, configurable via DevFS config
 * `nvmf_max_inflight_per_qpair` (devfs_conf.sh). The cap throttles per-qpair
 * inflight requests so burst dispatch stays within the device's sustained
 * write capability.
 *
 * - If the config value is 0 (or negative), fall back to default_cap
 *   (= spdk_max_io_requests_in_qpair / 2 at the call site).
 * - If positive, the effective cap is min(config, default_cap) so we never
 *   exceed the qpair's actual capacity.
 */
static inline uint32_t nvmf_inflight_cap(uint32_t default_cap)
{
	int v = g_devfs_conf.nvmf_max_inflight_per_qpair;
	uint32_t conf_cap;

	oxbow_assert(default_cap > 0);

	if (v <= 0)
		return default_cap;
	conf_cap = (uint32_t)v;
	return conf_cap < default_cap ? conf_cap : default_cap;
}

// Add function declarations
void copy_bio_to_buf(char *buf, struct bio *bio);
void copy_buf_to_bio(struct bio *bio, char *buf);

// Forward declaration for debug function
static void dump_nvmf_init_info(void);
static void dump_memory(void *addr, size_t size);

struct ctrlr_entry {
	struct spdk_nvme_ctrlr *ctrlr;
	enum spdk_nvme_transport_type trtype;

	TAILQ_ENTRY(ctrlr_entry) link;
	char name[1024];
};

struct ns_fn_table;

struct ns_entry {
	const struct ns_fn_table *fn_table;

	union {
		struct {
			struct spdk_nvme_ctrlr *ctrlr;
			struct spdk_nvme_ns *ns;
		} nvme;
	} u;

	TAILQ_ENTRY(ns_entry) link;
	uint32_t io_size_blocks;
	uint32_t num_io_requests;
	uint64_t size_in_ios;
	uint32_t block_size;
	uint32_t md_size; // To be deleted.
	bool md_interleave;
	bool pi_loc;
	enum spdk_nvme_pi_type pi_type;
	uint32_t io_flags;
	char name[1024];
	uint64_t max_io_size;
	uint32_t max_io_queue_requests;
};

struct ns_worker_stats {
	uint64_t io_submitted;
	uint64_t io_completed;
	uint64_t last_io_completed;
	uint64_t total_tsc;
	uint64_t min_tsc;
	uint64_t max_tsc;
	uint64_t last_tsc;
	uint64_t busy_tsc;
	uint64_t idle_tsc;
	uint64_t last_busy_tsc;
	uint64_t last_idle_tsc;
};

struct ns_worker_ctx {
	struct ns_entry *entry;
	struct ns_worker_stats stats;
	uint64_t offset_in_ios;
	bool is_draining;

	union {
		struct {
			int num_active_qpairs;
			int num_all_qpairs;
			struct spdk_nvme_qpair **qpair;
			struct spdk_nvme_poll_group *group;
			int last_qpair;
		} nvme;

	} u;

	TAILQ_ENTRY(ns_worker_ctx) link;

	TAILQ_HEAD(, perf_task) queued_tasks;

	uint32_t qpair_inflight;
	uint32_t qpair_completion_count;

	int status;
	struct bio_list *bl;
};

struct perf_task {
	struct iovec *iovs; /* array of iovecs to transfer. */
	int iovcnt; /* Number of iovecs in iovs array. */
	int iovpos; /* Current iovec position. */
	uint32_t iov_offset; /* Offset in current iovec. */
	uint64_t submit_tsc;
	bool is_read;
	size_t size; /* Length of data.  */
	baddr_t start;
	uint32_t bio_submit_cnt;
	uint32_t bio_cmpl_cnt;
	TAILQ_ENTRY(perf_task) link;
	struct bio *bio; // For copying to user buffer on completion (only read).
	char *spdk_buf;
	bool is_custom_buf;
	struct ns_worker_ctx *ns_ctx;
};

struct worker_thread {
	TAILQ_HEAD(, ns_worker_ctx) ns_ctx;
	TAILQ_ENTRY(worker_thread) link;
};

struct ns_fn_table {
	void (*setup_payload)(struct perf_task *task, struct bio *bio);
	int (*submit_io)(struct perf_task *task, struct ns_worker_ctx *ns_ctx,
			 struct ns_entry *entry);

	int64_t (*check_io)(struct ns_worker_ctx *ns_ctx);

	void (*verify_io)(struct perf_task *task, struct ns_entry *entry);

	int (*init_ns_worker_ctx)(struct ns_worker_ctx *ns_ctx);

	void (*cleanup_ns_worker_ctx)(struct ns_worker_ctx *ns_ctx);
	void (*dump_transport_stats)(uint32_t lcore,
				     struct ns_worker_ctx *ns_ctx);
};

struct worker_bitmap {
	void *map;
	pthread_spinlock_t lock;
};

static TAILQ_HEAD(, ctrlr_entry)
	g_controllers = TAILQ_HEAD_INITIALIZER(g_controllers);
static TAILQ_HEAD(,
		  ns_entry) g_namespaces = TAILQ_HEAD_INITIALIZER(g_namespaces);
static uint32_t g_num_namespaces;
static TAILQ_HEAD(,
		  worker_thread) g_workers = TAILQ_HEAD_INITIALIZER(g_workers);
static uint32_t g_num_workers = 0;

static struct worker_thread **g_worker_array;
static uint32_t g_io_align = 0x1000; // 4KB
static uint32_t g_io_size_bytes;
static uint64_t g_max_io_size;
static uint32_t g_max_io_size_blocks;
static uint32_t g_queue_depth;
static int g_nr_io_queues_per_ns = 1; // Queue pair per worker.
static uint32_t g_disable_sq_cmb;
static bool g_warn;
static bool g_header_digest;
static bool g_data_digest;
static bool g_no_shn_notification;
// static bool g_mix_specified;
/* The flag is used to exit the program while keep alive fails on the transport */
static bool g_exit;
/* Default to 10 seconds for the keep alive value. This value is arbitrary. */
static uint32_t g_keep_alive_timeout_in_ms = 10000;
/* Set default io_queue_size to UINT16_MAX, NVMe driver will then reduce this
 * to MQES to maximize the io_queue_size as much as possible.
 */
static uint32_t g_io_queue_size = UINT16_MAX;

static uint8_t g_transport_tos = 0;

static uint32_t g_max_io_requests_in_qpair;

#define MAX_ALLOWED_PCI_DEVICE_NUM 128
static struct spdk_pci_addr g_allowed_pci_addr[MAX_ALLOWED_PCI_DEVICE_NUM];

struct trid_entry {
	struct spdk_nvme_transport_id trid;
	uint16_t nsid;
	char hostnqn[SPDK_NVMF_NQN_MAX_LEN + 1];
	TAILQ_ENTRY(trid_entry) tailq;
};

static TAILQ_HEAD(,
		  trid_entry) g_trid_list = TAILQ_HEAD_INITIALIZER(g_trid_list);

static int g_file_optind; /* Index of first filename in argv */

static inline void handle_complete(struct perf_task *task);

static void io_complete_cb(void *ctx, const struct spdk_nvme_cpl *cpl);

static void set_iovec(struct bio *bio, struct perf_task *task, char *iovec_buf)
{
	struct bio_vec *bvec;
	int i;
	char *iovec_buf_p;

	// Point to the start.
	iovec_buf_p = iovec_buf;

	// After copying bvecs to iovecs, we will have a single iovec.
	// NOTE: bio represents a contiguous target block.
	task->iovcnt = 1;

	// Setup iovec.
	// Where to free at complete callback fn if there no more IO to submit.
	task->iovs = calloc(task->iovcnt, sizeof(struct iovec));
	task->iovs[0].iov_len = bio->total_size;
	task->iovs[0].iov_base = iovec_buf;

	if (task->is_read) {
		task->bio = bio;
		return;
	}

	// Copy bvecs to iovec.
	for (i = 0; i < bio->bi_vcnt; i++) {
		oxbow_assert((uint64_t)(iovec_buf_p - iovec_buf) <=
			     bio->total_size);

		bvec = &bio->bi_io_vec[i];
		oxbow_assert(bvec->bv_len > 0);
		oxbow_assert(bvec->bv_len % 0x1000 == 0); // 4KB aligned.
		oxbow_assert(bvec->bv_len % SECTOR_SIZE == 0); // n * sectors.
		oxbow_assert(bvec->bv_buf != NULL);

		memcpy(iovec_buf_p, bvec->bv_buf, bvec->bv_len);

		// advance pointer.
		iovec_buf_p += bvec->bv_len;
	}
}

static void nvme_setup_payload(struct perf_task *task, struct bio *bio)
{
	void *buf;

	oxbow_assert(bio->total_size != 0);

	// Set start block addr.
	task->start = bio->bi_iter.bi_blk_no;

	buf = spdk_dma_zmalloc(bio->total_size, g_io_align, NULL);
	if (buf == NULL) {
		fprintf(stderr, "task->buf spdk_dma_zmalloc failed\n");
		exit(1);
	}

	// OPTIMIZE: We can avoid memcopy. For example, sb, commit block, and
	// OPTIMIZE: data bufs can be allocated as a dma buffer.
	// Copy each bvec to iovec.
	set_iovec(bio, task, buf);
}

/**
	 * Checks if all bio_vecs in a bio have contiguous buffer addresses.
	 * Returns true if all are contiguous, false otherwise.
	 */
static bool is_bio_contiguous(struct bio *bio)
{
	if (bio->bi_vcnt <= 1) {
		return true; // Single or zero bio_vec is always contiguous
	}

	struct bio_vec *prev_bvec, *curr_bvec;

	for (int i = 1; i < bio->bi_vcnt; i++) {
		prev_bvec = &bio->bi_io_vec[i - 1];
		curr_bvec = &bio->bi_io_vec[i];

		// Check if end of previous buffer is contiguous with start of current buffer
		if ((prev_bvec->bv_buf + prev_bvec->bv_len) !=
		    curr_bvec->bv_buf) {
			return false;
		}
	}

	return true;
}

static void perf_disconnect_cb(struct spdk_nvme_qpair *qpair, void *ctx)
{
	struct ns_worker_ctx *ns_ctx = ctx;

	UNUSED1(qpair);

	ns_ctx->is_draining = true;
	ns_ctx->status = 1;
}

/*
 * TODO: If a controller has multiple namespaces, they could all use the same queue.
 *  For now, give each namespace/thread combination its own queue.
 */
static int nvme_init_ns_worker_ctx(struct ns_worker_ctx *ns_ctx)
{
	const struct spdk_nvme_ctrlr_opts *ctrlr_opts;
	struct spdk_nvme_io_qpair_opts opts;
	struct ns_entry *entry = ns_ctx->entry;
	struct spdk_nvme_qpair *qpair;
	uint64_t poll_timeout_tsc;
	int i, rc;

	ns_ctx->u.nvme.num_active_qpairs = g_nr_io_queues_per_ns;
	ns_ctx->u.nvme.num_all_qpairs = g_nr_io_queues_per_ns;
	ns_ctx->u.nvme.qpair = calloc(ns_ctx->u.nvme.num_all_qpairs,
				      sizeof(struct spdk_nvme_qpair *));
	if (!ns_ctx->u.nvme.qpair) {
		return -1;
	}

	spdk_nvme_ctrlr_get_default_io_qpair_opts(entry->u.nvme.ctrlr, &opts,
						  sizeof(opts));
	if (opts.io_queue_requests < entry->num_io_requests) {
		opts.io_queue_requests = entry->num_io_requests;
	}
	// opts.delay_cmd_submit = true;
	opts.delay_cmd_submit = false;
	opts.create_only = true;

	ctrlr_opts = spdk_nvme_ctrlr_get_opts(entry->u.nvme.ctrlr);
	opts.async_mode = !(
		spdk_nvme_ctrlr_get_transport_id(entry->u.nvme.ctrlr)->trtype ==
			SPDK_NVME_TRANSPORT_PCIE &&
		ns_ctx->u.nvme.num_all_qpairs > ctrlr_opts->admin_queue_size);

	for (i = 0; i < ns_ctx->u.nvme.num_all_qpairs; i++) {
		ns_ctx->u.nvme.qpair[i] = spdk_nvme_ctrlr_alloc_io_qpair(
			entry->u.nvme.ctrlr, &opts, sizeof(opts));
		qpair = ns_ctx->u.nvme.qpair[i];

		se_debug("Alloc qpair[%d]=0x%lx qpair=0x%lx ns_ctx=0x%lx", i,
			 ns_ctx->u.nvme.qpair[i], ns_ctx->u.nvme.qpair, ns_ctx);

		if (!qpair) {
			printf("ERROR: spdk_nvme_ctrlr_alloc_io_qpair failed\n");
			goto qpair_failed;
		}

		if (spdk_nvme_ctrlr_connect_io_qpair(entry->u.nvme.ctrlr,
						     qpair)) {
			printf("ERROR: unable to connect I/O qpair.\n");
			spdk_nvme_ctrlr_free_io_qpair(qpair);
			goto qpair_failed;
		}
	}

	return 0;

qpair_failed:
	for (; i > 0; --i) {
		spdk_nvme_ctrlr_free_io_qpair(ns_ctx->u.nvme.qpair[i - 1]);
	}

	spdk_nvme_poll_group_destroy(ns_ctx->u.nvme.group);
poll_group_failed:
	free(ns_ctx->u.nvme.qpair);
	return -1;
}

static void nvme_cleanup_ns_worker_ctx(struct ns_worker_ctx *ns_ctx)
{
	int i;

	for (i = 0; i < ns_ctx->u.nvme.num_all_qpairs; i++) {
		spdk_nvme_ctrlr_free_io_qpair(ns_ctx->u.nvme.qpair[i]);
	}

	spdk_nvme_poll_group_destroy(ns_ctx->u.nvme.group);
	free(ns_ctx->u.nvme.qpair);
}

static const struct ns_fn_table nvme_fn_table = {
	.setup_payload = nvme_setup_payload,
	.submit_io = NULL,
	.check_io = NULL,
	.init_ns_worker_ctx = nvme_init_ns_worker_ctx,
	.cleanup_ns_worker_ctx = nvme_cleanup_ns_worker_ctx,
	.dump_transport_stats = NULL
};

static int build_nvme_name(char *name, size_t length,
			   struct spdk_nvme_ctrlr *ctrlr)
{
	const struct spdk_nvme_transport_id *trid;
	int res = 0;

	trid = spdk_nvme_ctrlr_get_transport_id(ctrlr);

	switch (trid->trtype) {
	case SPDK_NVME_TRANSPORT_PCIE:
		res = snprintf(name, length, "PCIE (%s)", trid->traddr);
		break;
	case SPDK_NVME_TRANSPORT_RDMA:
		res = snprintf(name, length, "RDMA (addr:%s subnqn:%s)",
			       trid->traddr, trid->subnqn);
		break;
	case SPDK_NVME_TRANSPORT_TCP:
		res = snprintf(name, length, "TCP (addr:%s subnqn:%s)",
			       trid->traddr, trid->subnqn);
		break;
	case SPDK_NVME_TRANSPORT_VFIOUSER:
		res = snprintf(name, length, "VFIOUSER (%s)", trid->traddr);
		break;
	case SPDK_NVME_TRANSPORT_CUSTOM:
		res = snprintf(name, length, "CUSTOM (%s)", trid->traddr);
		break;

	default:
		fprintf(stderr, "Unknown transport type %d\n", trid->trtype);
		break;
	}
	return res;
}

static void build_nvme_ns_name(char *name, size_t length,
			       struct spdk_nvme_ctrlr *ctrlr, uint32_t nsid)
{
	int res = 0;

	res = build_nvme_name(name, length, ctrlr);
	if (res > 0) {
		snprintf(name + res, length - res, " NSID %u", nsid);
	}
}

// For now, we only consider the case, `lba_count > sectors_per_max_io` in
// `_nvme_ns_cmd_rw()` function.
static uint64_t get_max_io_size(struct ns_entry *entry)
{
	uint32_t sector_size, sectors_per_max_io, sectors_per_max_io_no_md;

	sectors_per_max_io =
		spdk_nvme_ns_get_max_io_xfer_size(entry->u.nvme.ns) /
		spdk_nvme_ns_get_extended_sector_size(entry->u.nvme.ns);
	sectors_per_max_io_no_md =
		spdk_nvme_ns_get_max_io_xfer_size(entry->u.nvme.ns) /
		spdk_nvme_ns_get_sector_size(entry->u.nvme.ns);

	sector_size = spdk_nvme_ns_get_sector_size(entry->u.nvme.ns);

	oxbow_assert(sector_size == SECTOR_SIZE);

	// Because io_flags == 0
	oxbow_assert(sectors_per_max_io == sectors_per_max_io_no_md);

	return (uint64_t)sectors_per_max_io_no_md * sector_size;
}

uint64_t nvmf_get_max_io_size(void)
{
	// Assuming there is only one ns_entry.
	struct ns_entry *ns_entry;
	TAILQ_FOREACH(ns_entry, &g_namespaces, link)
	{
		return ns_entry->max_io_size;
	}

	return 0;
}

static void register_ns(struct spdk_nvme_ctrlr *ctrlr, struct spdk_nvme_ns *ns)
{
	struct ns_entry *entry;
	const struct spdk_nvme_ctrlr_data *cdata;
	uint32_t max_xfer_size, entries, sector_size;
	uint64_t ns_size;
	struct spdk_nvme_io_qpair_opts opts;

	cdata = spdk_nvme_ctrlr_get_data(ctrlr);

	if (!spdk_nvme_ns_is_active(ns)) {
		printf("Controller %-20.20s (%-20.20s): Skipping inactive NS %u\n",
		       cdata->mn, cdata->sn, spdk_nvme_ns_get_id(ns));
		g_warn = true;
		return;
	}

	ns_size = spdk_nvme_ns_get_size(ns);
	sector_size = spdk_nvme_ns_get_sector_size(ns);

	if (ns_size < g_io_size_bytes || sector_size > g_io_size_bytes) {
		printf("WARNING: controller %-20.20s (%-20.20s) ns %u has invalid "
		       "ns size %" PRIu64 " / block size %u for I/O size %u\n",
		       cdata->mn, cdata->sn, spdk_nvme_ns_get_id(ns), ns_size,
		       spdk_nvme_ns_get_sector_size(ns), g_io_size_bytes);
		g_warn = true;
		return;
	}

	max_xfer_size = spdk_nvme_ns_get_max_io_xfer_size(ns);
	spdk_nvme_ctrlr_get_default_io_qpair_opts(ctrlr, &opts, sizeof(opts));
	/* NVMe driver may add additional entries based on
	 * stripe size and maximum transfer size, we assume
	 * 1 more entry be used for stripe.
	 */
	entries = (g_io_size_bytes - 1) / max_xfer_size + 2;
	if ((g_queue_depth * entries) > opts.io_queue_size) {
		printf("Controller IO queue size %u, less than required.\n",
		       opts.io_queue_size);
		printf("Consider using lower queue depth or smaller IO size, because "
		       "IO requests may be queued at the NVMe driver.\n");
	}
	/* For requests which have children requests, parent request itself
	 * will also occupy 1 entry.
	 */
	entries += 1;

	entry = calloc(1, sizeof(struct ns_entry));
	if (entry == NULL) {
		perror("ns_entry malloc");
		exit(1);
	}

	entry->fn_table = &nvme_fn_table;
	entry->u.nvme.ctrlr = ctrlr;
	entry->u.nvme.ns = ns;
	entry->num_io_requests =
		entries *
		spdk_divide_round_up(g_queue_depth, g_nr_io_queues_per_ns);

	entry->size_in_ios = ns_size / g_io_size_bytes;
	entry->io_size_blocks = g_io_size_bytes / sector_size;

	entry->block_size = spdk_nvme_ns_get_extended_sector_size(ns);
	entry->md_size = spdk_nvme_ns_get_md_size(ns);
	entry->md_interleave = spdk_nvme_ns_supports_extended_lba(ns);
	entry->pi_loc = spdk_nvme_ns_get_data(ns)->dps.md_start;
	entry->pi_type = spdk_nvme_ns_get_pi_type(ns);

	/* If metadata size = 8 bytes, PI is stripped (read) or inserted (write),
	 *  and so reduce metadata size from block size.  (If metadata size > 8 bytes,
	 *  PI is passed (read) or replaced (write).  So block size is not necessary
	 *  to change.)
	 */
	if ((entry->io_flags & SPDK_NVME_IO_FLAGS_PRACT) &&
	    (entry->md_size == 8)) {
		entry->block_size = spdk_nvme_ns_get_sector_size(ns);
	}

	if (g_io_size_bytes % entry->block_size != 0) {
		printf("WARNING: IO size %u (-o) is not a multiple of nsid %u sector size %u."
		       " Removing this ns from test\n",
		       g_io_size_bytes, spdk_nvme_ns_get_id(ns),
		       entry->block_size);
		g_warn = true;
		free(entry);
		return;
	}

	if (g_max_io_size_blocks < entry->io_size_blocks) {
		g_max_io_size_blocks = entry->io_size_blocks;
	}

	build_nvme_ns_name(entry->name, sizeof(entry->name), ctrlr,
			   spdk_nvme_ns_get_id(ns));

	g_max_io_size = get_max_io_size(entry);
	entry->max_io_size = get_max_io_size(entry);
	entry->max_io_queue_requests = g_max_io_requests_in_qpair;

	g_num_namespaces++;
	TAILQ_INSERT_TAIL(&g_namespaces, entry, link);
}

static void unregister_namespaces(void)
{
	struct ns_entry *entry, *tmp;

	TAILQ_FOREACH_SAFE(entry, &g_namespaces, link, tmp)
	{
		TAILQ_REMOVE(&g_namespaces, entry, link);
		free(entry);
	}
}

static void register_ctrlr(struct spdk_nvme_ctrlr *ctrlr,
			   struct trid_entry *trid_entry)
{
	struct spdk_nvme_ns *ns;
	struct ctrlr_entry *entry = malloc(sizeof(struct ctrlr_entry));
	uint32_t nsid;

	if (entry == NULL) {
		perror("ctrlr_entry malloc");
		exit(1);
	}

	build_nvme_name(entry->name, sizeof(entry->name), ctrlr);

	entry->ctrlr = ctrlr;
	entry->trtype = trid_entry->trid.trtype;
	TAILQ_INSERT_TAIL(&g_controllers, entry, link);

	if (trid_entry->nsid == 0) {
		for (nsid = spdk_nvme_ctrlr_get_first_active_ns(ctrlr);
		     nsid != 0;
		     nsid = spdk_nvme_ctrlr_get_next_active_ns(ctrlr, nsid)) {
			ns = spdk_nvme_ctrlr_get_ns(ctrlr, nsid);
			if (ns == NULL) {
				continue;
			}
			register_ns(ctrlr, ns);
		}
	} else {
		oxbow_assert(0); // multi namespaces not supported.

		// ns = spdk_nvme_ctrlr_get_ns(ctrlr, trid_entry->nsid);
		// if (!ns) {
		// 	perror("Namespace does not exist.");
		// 	exit(1);
		// }

		// register_ns(ctrlr, ns);
	}
}

static struct perf_task *allocate_task(struct bio *bio, char *spdk_buf,
				       uint32_t bio_req_cnt, bool is_read)
{
	struct perf_task *task;

	// OPTIMIZE: Use slab.
	task = calloc(1, sizeof(*task));
	if (task == NULL) {
		fprintf(stderr, "Out of memory allocating tasks\n");
		exit(1);
	}

	task->is_read = is_read;
	task->bio_submit_cnt = bio_req_cnt;
	task->bio_cmpl_cnt = 0;
	task->bio = bio;
	task->spdk_buf = spdk_buf;

	// oxb_warn("Task ALLOC: task->bio_submit_cnt=%u", task->bio_submit_cnt);

	return task;
}

static inline void handle_complete(struct perf_task *task)
{
	task->bio_cmpl_cnt++;

	if (task->is_custom_buf) {
		oxb_debug(
			"tls_tid=%u task_complete called. is_custom_buf=%d, task=0x%lx, bio_cmpl_cnt=%u",
			tls_tid, task->is_custom_buf, task, task->bio_cmpl_cnt);
		if (task->bio_submit_cnt == task->bio_cmpl_cnt)
			free(task);
		return;
	}

	oxb_debug(
		"tls_tid=%u task_complete called. task=0x%lx, bio_cmpl_cnt=%u",
		tls_tid, task, task->bio_cmpl_cnt);

	if (task->bio_submit_cnt == task->bio_cmpl_cnt) { // all done.
		oxb_debug("It is last: Copy and free task. tls_tid=%u",
			  tls_tid);
		if (!task->is_read)
			goto free;

		// Read. Copy data from spdk buffer to bio.
		copy_buf_to_bio(task->bio, task->spdk_buf);

	free:
		// OPTIMIZE Reuse struct task. You have to reallocate iovec buffer because io
		// size is different.
		// Free resources.
		spdk_dma_free(task->spdk_buf);
		free(task);
	}
}

// NOTE: It is called whenever a queue depth (a bio) is completed. (Not
// identical to rc in nvme_check_io().)
static void io_complete_cb(void *ctx, const struct spdk_nvme_cpl *cpl)
{
	struct perf_task *task = ctx;
	struct ns_worker_ctx *ns_ctx = task->ns_ctx;

	se_trace("tid(%d) io_complete called. task=0x%lx", gettid(), task);

	if (spdk_unlikely(spdk_nvme_cpl_is_error(cpl))) {
		if (task->is_read) {
			oxb_warn("Read completed with error (sct=%d, sc=%d)\n",
				 cpl->status.sct, cpl->status.sc);
		} else {
			oxb_warn("Write completed with error (sct=%d, sc=%d)\n",
				 cpl->status.sct, cpl->status.sc);
		}
	}

	ns_ctx->qpair_completion_count++;
	if (ns_ctx->qpair_inflight == 0) {
		se_warn("(tls_tid=%u) completion callback without tracked inflight request",
			tls_tid);
	} else {
		ns_ctx->qpair_inflight--;
	}
	handle_complete(task);
}

static int init_ns_worker_ctx(struct ns_worker_ctx *ns_ctx)
{
	TAILQ_INIT(&ns_ctx->queued_tasks);
	return ns_ctx->entry->fn_table->init_ns_worker_ctx(ns_ctx);
}

static void cleanup_ns_worker_ctx(struct ns_worker_ctx *ns_ctx)
{
	struct perf_task *task, *ttask;

	TAILQ_FOREACH_SAFE(task, &ns_ctx->queued_tasks, link, ttask)
	{
		TAILQ_REMOVE(&ns_ctx->queued_tasks, task, link);
		handle_complete(task);
	}
	ns_ctx->entry->fn_table->cleanup_ns_worker_ctx(ns_ctx);
}

void exit_nvmf(void)
{
	struct ns_worker_ctx *ns_ctx = NULL;
	struct worker_thread *worker;

	TAILQ_FOREACH(worker, &g_workers, link)
	{
		TAILQ_FOREACH(ns_ctx, &worker->ns_ctx, link)
		{
			cleanup_ns_worker_ctx(ns_ctx);
		}
	}
}

static void unregister_trids(void)
{
	struct trid_entry *trid_entry, *tmp;

	TAILQ_FOREACH_SAFE(trid_entry, &g_trid_list, tailq, tmp)
	{
		TAILQ_REMOVE(&g_trid_list, trid_entry, tailq);
		free(trid_entry);
	}
}

static int add_trid(const char *trid_str)
{
	struct trid_entry *trid_entry;
	struct spdk_nvme_transport_id *trid;
	char *ns;
	char *hostnqn;

	trid_entry = calloc(1, sizeof(*trid_entry));
	if (trid_entry == NULL) {
		return -1;
	}

	trid = &trid_entry->trid;
	trid->trtype = SPDK_NVME_TRANSPORT_PCIE;
	snprintf(trid->subnqn, sizeof(trid->subnqn), "%s",
		 SPDK_NVMF_DISCOVERY_NQN);

	if (spdk_nvme_transport_id_parse(trid, trid_str) != 0) {
		fprintf(stderr, "Invalid transport ID format '%s'\n", trid_str);
		free(trid_entry);
		return 1;
	}

	ns = strcasestr(trid_str, "ns:");
	if (ns) {
		char nsid_str[6]; /* 5 digits maximum in an nsid */
		int len;
		int nsid;

		ns += 3;

		len = strcspn(ns, " \t\n");
		if (len > 5) {
			fprintf(stderr,
				"NVMe namespace IDs must be 5 digits or less\n");
			free(trid_entry);
			return 1;
		}

		memcpy(nsid_str, ns, len);
		nsid_str[len] = '\0';

		nsid = spdk_strtol(nsid_str, 10);
		if (nsid <= 0 || nsid > 65535) {
			fprintf(stderr,
				"NVMe namespace IDs must be less than 65536 and greater than 0\n");
			free(trid_entry);
			return 1;
		}

		trid_entry->nsid = (uint16_t)nsid;
	}

	hostnqn = strcasestr(trid_str, "hostnqn:");
	if (hostnqn) {
		size_t len;

		hostnqn += strlen("hostnqn:");

		len = strcspn(hostnqn, " \t\n");
		if (len > (sizeof(trid_entry->hostnqn) - 1)) {
			fprintf(stderr, "Host NQN is too long\n");
			free(trid_entry);
			return 1;
		}

		memcpy(trid_entry->hostnqn, hostnqn, len);
		trid_entry->hostnqn[len] = '\0';
	}

	TAILQ_INSERT_TAIL(&g_trid_list, trid_entry, tailq);
	return 0;
}

static int register_workers(int num_workers)
{
	int i;
	struct worker_thread *worker;

	for (i = 0; i < num_workers; i++) {
		worker = calloc(1, sizeof(*worker));
		if (worker == NULL) {
			fprintf(stderr, "Unable to allocate worker\n");
			return -1;
		}

		TAILQ_INIT(&worker->ns_ctx);
		TAILQ_INSERT_TAIL(&g_workers, worker, link);
		g_num_workers++;
	}

	// Alloc and set worker pointer array.
	g_worker_array = calloc(g_num_workers, sizeof(struct worker_thread *));

	i = 0;
	TAILQ_FOREACH(worker, &g_workers, link)
	{
		g_worker_array[i] = worker;
		i++;
	}
	return 0;
}

static void unregister_workers(void)
{
	struct worker_thread *worker, *tmp_worker;
	struct ns_worker_ctx *ns_ctx, *tmp_ns_ctx;

	/* Free namespace context and worker thread */
	TAILQ_FOREACH_SAFE(worker, &g_workers, link, tmp_worker)
	{
		TAILQ_REMOVE(&g_workers, worker, link);

		TAILQ_FOREACH_SAFE(ns_ctx, &worker->ns_ctx, link, tmp_ns_ctx)
		{
			TAILQ_REMOVE(&worker->ns_ctx, ns_ctx, link);
			free(ns_ctx);
		}

		free(worker);
	}
}

static bool probe_cb(void *cb_ctx, const struct spdk_nvme_transport_id *trid,
		     struct spdk_nvme_ctrlr_opts *opts)
{
	struct trid_entry *trid_entry = cb_ctx;

	if (trid->trtype == SPDK_NVME_TRANSPORT_PCIE) {
		if (g_disable_sq_cmb) {
			opts->use_cmb_sqs = false;
		}
		if (g_no_shn_notification) {
			opts->no_shn_notification = true;
		}
	}

	if (trid->trtype != trid_entry->trid.trtype &&
	    strcasecmp(trid->trstring, trid_entry->trid.trstring)) {
		return false;
	}

	opts->io_queue_size = g_io_queue_size;

	/* Set the header and data_digest */
	opts->header_digest = g_header_digest;
	opts->data_digest = g_data_digest;
	opts->keep_alive_timeout_ms = g_keep_alive_timeout_in_ms;
	memcpy(opts->hostnqn, trid_entry->hostnqn, sizeof(opts->hostnqn));

	opts->transport_tos = g_transport_tos;
	if (opts->num_io_queues < g_num_workers * g_nr_io_queues_per_ns) {
		opts->num_io_queues = g_num_workers * g_nr_io_queues_per_ns;
	}

	return true;
}

static void attach_cb(void *cb_ctx, const struct spdk_nvme_transport_id *trid,
		      struct spdk_nvme_ctrlr *ctrlr,
		      const struct spdk_nvme_ctrlr_opts *opts)
{
	struct trid_entry *trid_entry = cb_ctx;
	struct spdk_pci_addr pci_addr;
	struct spdk_pci_device *pci_dev;
	struct spdk_pci_id pci_id;

	UNUSED(opts);

	if (trid->trtype != SPDK_NVME_TRANSPORT_PCIE) {
		printf("Attached to NVMe over Fabrics controller at %s:%s: %s\n",
		       trid->traddr, trid->trsvcid, trid->subnqn);
	} else {
		if (spdk_pci_addr_parse(&pci_addr, trid->traddr)) {
			return;
		}

		pci_dev = spdk_nvme_ctrlr_get_pci_device(ctrlr);
		if (!pci_dev) {
			return;
		}

		pci_id = spdk_pci_device_get_id(pci_dev);

		printf("Attached to NVMe Controller at %s [%04x:%04x]\n",
		       trid->traddr, pci_id.vendor_id, pci_id.device_id);
	}

	register_ctrlr(ctrlr, trid_entry);
}

static int register_controllers(void)
{
	struct trid_entry *trid_entry;

	printf("Initializing NVMe Controllers\n");

	TAILQ_FOREACH(trid_entry, &g_trid_list, tailq)
	{
		if (spdk_nvme_probe(&trid_entry->trid, trid_entry, probe_cb,
				    attach_cb, NULL) != 0) {
			fprintf(stderr,
				"spdk_nvme_probe() failed for transport address '%s'\n",
				trid_entry->trid.traddr);
			return -1;
		}
	}

	return 0;
}

static void unregister_controllers(void)
{
	struct ctrlr_entry *entry, *tmp;
	struct spdk_nvme_detach_ctx *detach_ctx = NULL;

	TAILQ_FOREACH_SAFE(entry, &g_controllers, link, tmp)
	{
		TAILQ_REMOVE(&g_controllers, entry, link);
		spdk_nvme_detach_async(entry->ctrlr, &detach_ctx);
		free(entry);
	}

	if (detach_ctx) {
		spdk_nvme_detach_poll(detach_ctx);
	}
}

static int allocate_ns_worker(struct ns_entry *entry,
			      struct worker_thread *worker)
{
	struct ns_worker_ctx *ns_ctx;

	// NS INIT.
	ns_ctx = calloc(1, sizeof(struct ns_worker_ctx));
	if (!ns_ctx) {
		return -1;
	}

	ns_ctx->stats.min_tsc = UINT64_MAX;
	ns_ctx->entry = entry;
	TAILQ_INSERT_TAIL(&worker->ns_ctx, ns_ctx, link);

	return 0;
}

static int associate_workers_with_ns(void)
{
	struct ns_entry *entry = TAILQ_FIRST(&g_namespaces);
	struct worker_thread *worker = TAILQ_FIRST(&g_workers);
	int i, count;

	count = g_num_namespaces > g_num_workers ? g_num_namespaces :
						   g_num_workers;

	for (i = 0; i < count; i++) {
		if (entry == NULL) {
			break;
		}

		if (allocate_ns_worker(entry, worker) != 0) {
			return -1;
		}

		worker = TAILQ_NEXT(worker, link);
		if (worker == NULL) {
			worker = TAILQ_FIRST(&g_workers);
		}

		entry = TAILQ_NEXT(entry, link);
		if (entry == NULL) {
			entry = TAILQ_FIRST(&g_namespaces);
		}
	}

	return 0;
}

static void *nvme_poll_ctrlrs(void *arg)
{
	struct ctrlr_entry *entry;
	int oldstate;
	int rc;

	UNUSED1(arg);

	spdk_unaffinitize_thread();

	while (true) {
		pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, &oldstate);

		TAILQ_FOREACH(entry, &g_controllers, link)
		{
			if (entry->trtype != SPDK_NVME_TRANSPORT_PCIE) {
				rc = spdk_nvme_ctrlr_process_admin_completions(
					entry->ctrlr);
				if (spdk_unlikely(rc < 0 && !g_exit)) {
					g_exit = true;
				}
			}
		}

		pthread_setcancelstate(PTHREAD_CANCEL_ENABLE, &oldstate);

		/* This is a pthread cancellation point and cannot be removed. */
		sleep(1);
	}

	return NULL;
}

static void construct_nvmf_config_string(char *str,
					 const struct nvmf_config *nvmf_conf)
{
	// Example string:
	// "trtype:RDMA adrfam:IPv4 traddr:192.168.14.113 trsvcid:4420 subnqn:oxbow-nvmf"
	sprintf(str, "trtype:RDMA adrfam:IPv4 traddr:%s trsvcid:%d subnqn:%s",
		nvmf_conf->target_ip_addr, nvmf_conf->port,
		nvmf_conf->subnqn_name);

	se_info("NVMF Config String:%s", str);
}

static void construct_nvme_config_string(char *str,
					 const struct nvme_config *nvme_conf)
{
	// Example string:
	// "trtype:PCIe traddr:0000:00:05.0"
	sprintf(str, "trtype:PCIe traddr:%s", nvme_conf->pcie_addr);
	// sprintf(str, "trtype:PCIe traddr:0000:d8:00.0");

	se_info("NVMF Config String:%s", str);
}

static int create_devfs_core_mask(int num_qpair, char *core_mask_str,
				  size_t core_mask_str_size)
{
	unsigned long core_mask = 0;

	for (int tid = 0; tid < num_qpair; tid++) {
		int cpu = oxb_devfs_pin_cpu_for_tid(tid, num_qpair);
		int numa = oxb_devfs_pin_numa_for_tid(tid, num_qpair);
		(void)numa;

		if (cpu < 0 || cpu >= (int)(sizeof(core_mask) * 8)) {
			se_error("create_devfs_core_mask: cpu %d for tid %d "
				 "out of mask range (total=%d)",
				 cpu, tid, num_qpair);
			return -1;
		}

		core_mask |= (1ULL << cpu);
		se_info("[pinning] devfs io_worker tid=%-2d -> CPU %-3d "
			"(NUMA%d)",
			tid, cpu, numa);
	}

	snprintf(core_mask_str, core_mask_str_size, "0x%lx", core_mask);
	se_info("[pinning] devfs total io workers=%d, DPDK core_mask=%s",
		num_qpair, core_mask_str);
	return 0;
}

static int set_opts(struct spdk_env_opts *env_opts,
		    const struct se_config *se_config, int num_qpair,
		    char *core_mask_str, size_t core_mask_str_size)
{
	char nvmf_conf_str[256];
#ifdef USE_NVME_STORAGE_ENGINE
	construct_nvme_config_string(nvmf_conf_str, &se_config->nvme);
	env_opts->no_pci = false;
#else
	construct_nvmf_config_string(nvmf_conf_str, &se_config->nvmf);
	env_opts->no_pci = true;
#endif

	g_queue_depth = 128; // -q
	if (create_devfs_core_mask(num_qpair, core_mask_str,
				   core_mask_str_size)) {
		return -1;
	}
	env_opts->core_mask = core_mask_str;
	g_io_size_bytes =
		4096; // -o // Not used but required to pass some checking.

	if (add_trid(nvmf_conf_str)) { // -r
		se_error("Failed to add transport ID.");
		return -1;
	}

	g_file_optind = 13;

	return 0;
}

static pthread_t poll_ctrl_thread_id = 0;

static threadpool init_nvmf(struct se_config *se_config, int num_qpair)
{
	int rc;
	struct worker_thread *worker;
	struct ns_worker_ctx *ns_ctx;
	struct spdk_env_opts opts;
	threadpool thpool = NULL;
	char core_mask[20];

#ifdef USE_NVME_STORAGE_ENGINE
	g_max_io_requests_in_qpair = se_config->nvme.num_io_requests;
#else
	g_max_io_requests_in_qpair = se_config->nvmf.num_io_requests;
#endif

	/* Use the runtime PID to set the random seed */
	srand(getpid());

	spdk_env_opts_init(&opts);
	opts.name = "perf";
	opts.pci_allowed = g_allowed_pci_addr;

	// Config explicitly.
	rc = set_opts(&opts, se_config, num_qpair, core_mask,
		      sizeof(core_mask));
	if (rc < 0) {
		goto err;
	}

	if (spdk_env_init(&opts) < 0) {
		fprintf(stderr, "Unable to initialize SPDK env\n");
		unregister_trids();
		rc = -1;
		goto err;
	}

	if (register_workers(num_qpair) != 0) {
		rc = -1;
		goto cleanup;
	}

	if (register_controllers() != 0) {
		rc = -1;
		goto cleanup;
	}

	if (g_warn) {
		printf("WARNING: Some requested NVMe devices were skipped\n");
	}

	if (g_num_namespaces == 0) {
		fprintf(stderr,
			"No valid NVMe controllers or AIO or URING devices found\n");
		goto cleanup;
	}

	// We need it. Otherwise, connection fails.
	// Example error:
	// nvme_rdma.c:1081:nvme_rdma_addr_resolved: *ERROR*: RDMA address resolution error
	// nvme_rdma.c:2730:nvme_rdma_qpair_process_completions: *ERROR*: Failed to connect rqpair=0x2000003df580
	//
	rc = pthread_create(&poll_ctrl_thread_id, NULL, &nvme_poll_ctrlrs,
			    NULL);
	if (rc != 0) {
		fprintf(stderr,
			"Unable to spawn a thread to poll admin queues.\n");
		goto cleanup;
	}

	if (associate_workers_with_ns() != 0) {
		rc = -1;
		goto cleanup;
	}

	se_info("Initialization complete. Launching workers.");

	/* Launch all of the secondary workers */
	TAILQ_FOREACH(worker, &g_workers, link)
	{
		/* Allocate queue pairs for each namespace. */
		TAILQ_FOREACH(ns_ctx, &worker->ns_ctx, link)
		{
			if (init_ns_worker_ctx(ns_ctx) != 0) {
				printf("ERROR: init_ns_worker_ctx() failed\n");
				/* Wait on barrier to avoid blocking of successful workers */
				// pthread_barrier_wait(&g_worker_sync_barrier);
				ns_ctx->status = 1;
				oxbow_assert(false);
				return NULL;
			}
		}
	}

	// This threads are used as RPC message handler and IO worker.
	thpool = spdk_thpool_init_with_pinning(num_qpair, "ioworker",
					       OXB_PIN_DOMAIN_DEVFS);

	// Set initialization to true.
	se_info("NVMF initialized.");

	// Print worker and namespace information before returning
	dump_nvmf_init_info();

	// sleep(1);
	return thpool;

cleanup:
	if (poll_ctrl_thread_id && pthread_cancel(poll_ctrl_thread_id) == 0) {
		pthread_join(poll_ctrl_thread_id, NULL);
	}

	/* Collect errors from all workers and namespaces */
	TAILQ_FOREACH(worker, &g_workers, link)
	{
		if (rc != 0) {
			break;
		}

		TAILQ_FOREACH(ns_ctx, &worker->ns_ctx, link)
		{
			if (ns_ctx->status != 0) {
				rc = ns_ctx->status;
				break;
			}
		}
	}

	unregister_trids();
	unregister_namespaces();
	unregister_controllers();
	unregister_workers();

	spdk_env_fini();

	if (rc != 0) {
		// fprintf(stderr, "%s: errors occurred\n", argv[0]);
		se_error("NVMF init: errors occured.");
	}

#ifdef DEVFS_TID_VALIDATE
	oxb_warn("DEVFS_TID_VALIDATE is enabled. Disable it for performance.");
#endif

	return NULL;
err:
	se_error("init_nvmf failed: rc=%d", rc);
	return NULL;
}

/**
 * Dumps detailed information about all workers, their namespaces and queue pairs
 */
static void dump_nvmf_init_info(void)
{
	struct worker_thread *worker;
	struct ns_worker_ctx *ns_ctx;
	struct ns_entry *entry;
	int worker_idx = 0;

	se_info("========== NVMF Initialization Information ==========");
	se_info("Total workers: %d, Total namespaces: %d", g_num_workers,
		g_num_namespaces);

	TAILQ_FOREACH(worker, &g_workers, link)
	{
		se_info("Worker %d (0x%lx):", worker_idx, (uint64_t)worker);

		TAILQ_FOREACH(ns_ctx, &worker->ns_ctx, link)
		{
			entry = ns_ctx->entry;
			se_info("  |-- NS_ctx (0x%lx):", (uint64_t)ns_ctx);
			se_info("  |     |-- Name: %s", entry->name);
			se_info("  |     |-- Block size: %u",
				entry->block_size);
			se_info("  |     |-- Max IO size: %lu bytes (%lu KB)",
				entry->max_io_size, entry->max_io_size / 1024);
			se_info("  |     |-- Max IO queue requests: %u",
				entry->max_io_queue_requests);

			if (ns_ctx->u.nvme.qpair) {
				se_info("  |     |-- Queue pairs: %d active, %d total",
					ns_ctx->u.nvme.num_active_qpairs,
					ns_ctx->u.nvme.num_all_qpairs);

				for (int i = 0;
				     i < ns_ctx->u.nvme.num_all_qpairs; i++) {
					se_info("  |     |     |-- QPair[%d]: 0x%lx",
						i,
						(uint64_t)ns_ctx->u.nvme
							.qpair[i]);
				}
			} else {
				se_info("  |     |-- No queue pairs initialized");
			}
		}
		worker_idx++;
	}
	se_info("====================================================");
}

int nvmf_init(struct se_config *se_config, int num_qpairs)
{
	se_config->worker_thpool = init_nvmf(se_config, num_qpairs);

	return 0;
}

void nvmf_exit(void)
{
	struct ns_worker_ctx *ns_ctx;
	struct worker_thread *worker;
	int rc;

	TAILQ_FOREACH(worker, &g_workers, link)
	{
		TAILQ_FOREACH(ns_ctx, &worker->ns_ctx, link)
		{
			ns_ctx->is_draining = true;
		}
	}

	spdk_env_thread_wait_all();

	if (poll_ctrl_thread_id && pthread_cancel(poll_ctrl_thread_id) == 0) {
		pthread_join(poll_ctrl_thread_id, NULL);
	}

	/* Collect errors from all workers and namespaces */
	TAILQ_FOREACH(worker, &g_workers, link)
	{
		if (rc != 0) {
			break;
		}

		TAILQ_FOREACH(ns_ctx, &worker->ns_ctx, link)
		{
			if (ns_ctx->status != 0) {
				rc = ns_ctx->status;
				break;
			}
		}
	}

	unregister_trids();
	unregister_namespaces();
	unregister_controllers();
	unregister_workers();

	spdk_env_fini();

	se_error("NVMF exit.");

	/////// Free others.

	// Free bitmap

	// Free worker array.
	free(g_worker_array);

	return;
}

static struct ns_worker_ctx *worker_to_ns(struct worker_thread *worker)
{
	struct ns_worker_ctx *ns_ctx;
	// Assuming there is only one ns.
	TAILQ_FOREACH(ns_ctx, &worker->ns_ctx, link)
	{
		return ns_ctx;
	}
	return NULL;
}

// Fast path for persist_data. Use custom (preallocated spdk dma) buffer.
uint32_t nvmf_dispatch_io_fast(struct bio_list *bl, bool is_read,
			       char *custom_buf)
{
	struct worker_thread *worker;
	struct ns_worker_ctx *ns_ctx;
	struct ns_entry *entry;
	struct bio *bio;
	int rc;
	uint64_t lba_cnt;
	spdk_nvme_cmd_cb coml_cb;
	struct perf_task *task;
	uint32_t total_req_cnt = 0;
	uint32_t bio_req_cnt = 0;
	uint32_t compl_cnt = 0;
	size_t left_bytes;
	size_t single_io_size;
	char *payload_buf;
	baddr_t baddr;
	int i = 0;
#ifdef DEVFS_TID_VALIDATE
	uint32_t mytid = gettid();

	if (g_tid_validate_table[tls_tid] != mytid) {
		if (g_tid_validate_table[tls_tid] == 0) {
			g_tid_validate_table[tls_tid] = mytid;
		} else {
			se_error("TID[%d] mismatch: prev_tid=%u ----> mytid=%u",
				 tls_tid, g_tid_validate_table[tls_tid], mytid);
			g_tid_validate_table[tls_tid] = mytid;
		}
	};

#endif

	PF_TL_START(evt_nvmf_dispatch_fast);

	// we use tls_tid to select worker.
	worker = g_worker_array[tls_tid];
	ns_ctx = worker_to_ns(worker);
	entry = ns_ctx->entry;

	// There is only one qpair for each worker.
	oxbow_assert(ns_ctx->u.nvme.num_active_qpairs == 1);
	oxbow_assert(ns_ctx->u.nvme.num_all_qpairs == 1);

	if (custom_buf) {
		coml_cb = io_complete_cb;
		payload_buf = custom_buf;
	} else {
		coml_cb = io_complete_cb;
	}

	// Create a task for each bio.
	// NOTE: We cannot guarantee all bios are submitted in order if the
	// number of bios exceeds the queue depth.
	bio_list_for_each (bio, bl) {
		left_bytes = bio->bi_io_vec[0].bv_len;
		baddr = bio->bi_iter.bi_blk_no;

		bio_req_cnt = CEILING(bio->bi_io_vec[0].bv_len,
				      ns_ctx->entry->max_io_size);

		if (custom_buf) {
			// If we use custom buffer, only bio->total_size and
			// bio->bi_iter.bi_blk_no are referenced.
			oxbow_assert(bio->bi_vcnt == 1);

			// Only one bio is allowed with custom buffer.
			//
			// If this assertion fails, check BIO_MAX_BIO_SIZE and
			// DATA_FETCHER_BUF_SIZE in oxbow.h.
			oxbow_assert(i == 0);

			task = allocate_task(bio, payload_buf, bio_req_cnt,
					     is_read);
			task->is_custom_buf = true;
			task->ns_ctx = ns_ctx;
		} else {
			payload_buf = nvmf_alloc_dma_buf(bio->total_size);

			// Copy at once.
			if (!is_read)
				copy_bio_to_buf(payload_buf, bio);

			// Freed at callback (task_complete).
			task = allocate_task(bio, payload_buf, bio_req_cnt,
					     is_read);
			task->ns_ctx = ns_ctx;
		}

		uint32_t inflight_cap = nvmf_inflight_cap(
			ns_ctx->entry->max_io_queue_requests / 2);

		while (left_bytes > 0) {
			// Calculate single io size.
			single_io_size =
				(left_bytes > ns_ctx->entry->max_io_size) ?
					ns_ctx->entry->max_io_size :
					left_bytes;

			// Check if queue is getting full and process completions if needed
			if (ns_ctx->qpair_inflight >= inflight_cap) {
				se_debug(
					"Queue full (cap), processing completions to free space (inflight=%u, cap=%u)",
					ns_ctx->qpair_inflight, inflight_cap);

				while (ns_ctx->qpair_inflight >= inflight_cap) {
					// Process completions to free up space in the queue
					compl_cnt =
						spdk_nvme_qpair_process_completions(
							ns_ctx->u.nvme.qpair[0],
							0);

					if (compl_cnt > 0) {
						se_debug(
							"Processed %u completions, inflight requests: %u",
							compl_cnt,
							ns_ctx->qpair_inflight);
						break;
					}
				}
			}

			lba_cnt = byte_to_lba_cnt(single_io_size);

			se_debug(
				"[SUBMIT_IO_FAST] (tls_tid=%u) is_read=%d baddr=%lu payload=0x%lx size=%lu (%lu blks, %u MB) custom_buf=0x%lx",
				tls_tid, is_read, baddr, payload_buf,
				bio->bi_io_vec[0].bv_len,
				bio->bi_io_vec[0].bv_len >>
					OXBOW_BLOCK_SIZE_SHIFT,
				bio->bi_io_vec[0].bv_len >> 20,
				(uintptr_t)custom_buf);

		retry:
			if (is_read) {
				oxbow_assert(
					!custom_buf); // There is no read with custom buffer.

				rc = spdk_nvme_ns_cmd_read(
					entry->u.nvme.ns,
					ns_ctx->u.nvme.qpair[0], payload_buf,
					baddr_to_lba(baddr), lba_cnt, coml_cb,
					task, entry->io_flags);
			} else {
				oxbow_assert(
					ns_ctx->u.nvme
						.qpair[0]); // TMP: To be removed.
				rc = spdk_nvme_ns_cmd_write(
					entry->u.nvme.ns,
					ns_ctx->u.nvme.qpair[0], payload_buf,
					baddr_to_lba(baddr), lba_cnt, coml_cb,
					task, entry->io_flags);
			}

			// Handle errors:
			// -EINVAL(22): returned if qpair->free_req is not enough
			// -ENOMEM(12): returned if there are resource constraints
			if (rc == -EINVAL || rc == -ENOMEM) {
				if (rc == -EINVAL) {
					se_error(
						"qpair->free_req is not enough (EINVAL).");
				} else if (rc == -ENOMEM) {
					se_error(
						"Resource constraint in SPDK NVMe (ENOMEM).");
				}

				// oxbow_assert(false);

				// Process some completions to free up resources
				compl_cnt = spdk_nvme_qpair_process_completions(
					ns_ctx->u.nvme.qpair[0], 0);

				if (compl_cnt > 0) {
					se_debug(
						"Processed %u completions during error recovery (inflight=%u)",
						compl_cnt,
						ns_ctx->qpair_inflight);
				}

				goto retry;

			} else if (rc != 0) {
				// If we get an error that's not EINVAL or ENOMEM, log it but continue
				se_error(
					"Unexpected error in SPDK NVMe cmd: %d",
					rc);
				oxbow_assert(false);
				// goto retry;
			}

			// Advance the pointers.
			payload_buf += single_io_size;
			left_bytes -= single_io_size;
			baddr += (single_io_size >> OXBOW_BLOCK_SIZE_SHIFT);
			total_req_cnt++;
			ns_ctx->qpair_inflight++;
		}
		i++;
	}
	se_debug("DISPATCH tls_tid=%u total_req_cnt: %u inflight=%u completions=%u",
		 tls_tid, total_req_cnt, ns_ctx->qpair_inflight,
		 ns_ctx->qpair_completion_count);

	PF_TL_END(evt_nvmf_dispatch_fast);

	return total_req_cnt;
}

void nvmf_poll_complete_fast(uint32_t req_tot)
{
	struct worker_thread *worker;
	struct ns_worker_ctx *ns_ctx;
	uint32_t req_done = 0;
	uint32_t completions = 0;
	uint32_t consumed = 0;

	worker = g_worker_array[tls_tid];
	ns_ctx = worker_to_ns(worker);

	PF_TL_START(evt_nvmf_poll_fast);

	se_debug("[%s] (tls_tid=%u) waiting for %u completions", __func__,
		 tls_tid, req_tot);

	// Special case.
	if (req_tot == 0) {
		// Just do process completion once.
		spdk_nvme_qpair_process_completions(ns_ctx->u.nvme.qpair[0], 0);
		goto out;
	}

	// polling for complete
	while (req_done < req_tot) {
		if (ns_ctx->qpair_completion_count > 0) {
			consumed = (ns_ctx->qpair_completion_count >
				    req_tot - req_done) ?
					   req_tot - req_done :
					   ns_ctx->qpair_completion_count;
			ns_ctx->qpair_completion_count -= consumed;
			req_done += consumed;
			continue;
		}

		completions = spdk_nvme_qpair_process_completions(
			ns_ctx->u.nvme.qpair[0], 0);
		if (completions > 0) {
			if (req_done < req_tot) {
				se_debug(
					"[%s] (tls_tid=%u) polled %u completions (progress: %u/%u)",
					__func__, tls_tid, completions,
					req_done, req_tot);
			}
		}

		// Keep polling until all requests are completed.
	}

	se_debug(
		"[%s] (tls_tid=%u) done - processed %u completions for %u requests",
		__func__, tls_tid, req_done, req_tot);

out:
	PF_TL_END(evt_nvmf_poll_fast);
}

char *nvmf_alloc_dma_buf(size_t size)
{
	char *buf;

	buf = spdk_dma_zmalloc(size, g_io_align, NULL);
	if (buf == NULL) {
		se_error("Failed to allocate DMA buffer.");
		exit(1);
	}

	return buf;
}

int nvmf_register_buf_to_spdk(void *buf, size_t size)
{
	int ret = 0;

	ret = spdk_mem_register(buf, size);
	if (ret < 0) {
		se_error("Failed to register buffer to SPDK DMA region.");
		return -1;
	}

	se_info("Buffer (0x%lx size=%lu) is registered to SPDK DMA region.",
		(unsigned long)buf, size);

	return 0;
}

/**
 * @brief Copy data from bio vectors to a contiguous buffer
 * @param buf Destination buffer (must be large enough to hold all bio data)
 * @param bio Source bio containing the data vectors
 */
void copy_bio_to_buf(char *buf, struct bio *bio)
{
	struct bio_vec *bvec;
	int i;
	char *buf_ptr = buf;

	// Copy each bio_vec to the buffer sequentially
	for (i = 0; i < bio->bi_vcnt; i++) {
		bvec = &bio->bi_io_vec[i];
		oxbow_assert(bvec->bv_len > 0);
		oxbow_assert(bvec->bv_len % 0x1000 == 0); // 4KB aligned
		oxbow_assert(bvec->bv_len % SECTOR_SIZE == 0); // n * sectors
		oxbow_assert(bvec->bv_buf != NULL);
		oxbow_assert((uint64_t)(buf_ptr - buf) <= bio->total_size);

		memcpy(buf_ptr, bvec->bv_buf, bvec->bv_len);
		buf_ptr += bvec->bv_len;
	}
}

/**
 * @brief Copy data from a contiguous buffer to bio vectors
 * @param bio Destination bio to receive the data
 * @param buf Source buffer containing the data
 */
void copy_buf_to_bio(struct bio *bio, char *buf)
{
	struct bio_vec *bvec;
	int i;
	char *buf_ptr = buf;

	// Copy buffer segments to each bio_vec
	for (i = 0; i < bio->bi_vcnt; i++) {
		bvec = &bio->bi_io_vec[i];

		// TMP: Print assertion case.
		// if (bvec->bv_len % 0x1000 != 0 || bvec->bv_len == 0 ||
		//     bvec->bv_len % SECTOR_SIZE != 0 || bvec->bv_buf == NULL) {
		// 	print_bio(bio);
		// 	se_error("bvec->bv_len is not 4KB aligned: %u",
		// 		 bvec->bv_len);
		// 	oxbow_assert(false);
		// }

		oxbow_assert(bvec->bv_len > 0);
		oxbow_assert(bvec->bv_len % 0x1000 == 0); // 4KB aligned
		oxbow_assert(bvec->bv_len % SECTOR_SIZE == 0); // n * sectors
		oxbow_assert(bvec->bv_buf != NULL);
		oxbow_assert((uint64_t)(buf_ptr - buf) <= bio->total_size);

		se_debug("COPY: baddr=%lu buf=0x%lx size=%u",
			 bio->bi_iter.bi_blk_no, (unsigned long)buf,
			 bvec->bv_len);

		// dump_memory(buf, 128);

		memcpy(bvec->bv_buf, buf_ptr, bvec->bv_len);
		buf_ptr += bvec->bv_len;
	}
}

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
