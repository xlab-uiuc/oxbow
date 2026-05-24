#include "io_dispatcher.h"
#include <spdk/env.h>
#include <stddef.h>
#include <stdint.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/user.h>
#define __USE_GNU
#include <sched.h>

#include "io/nvme.h"
#include "spdk/nvme.h"
#include "global.h"
#include "spdk/likely.h"
#include "thpool.h"
#include "oxbow_debug.h"
#include "common/oxbow.h"
#include "cpu_pinning.h"
#include "fs.h"
#include "profile_secure_daemon.h"
#include "common/ring_buffer_mpmc.h"

/*
 * Report the effective DPDK IOVA mode (PA, VA or DC) selected by SPDK/DPDK.
 * We avoid depending on DPDK headers directly by forward-declaring the
 * minimal enum and rte_eal_iova_mode() symbol needed for logging.
 */
enum rte_iova_mode {
	RTE_IOVA_DC = 0,        /* Don't care mode */
	RTE_IOVA_PA = (1 << 0), /* DMA using physical address */
	RTE_IOVA_VA = (1 << 1)  /* DMA using virtual address */
};

extern enum rte_iova_mode rte_eal_iova_mode(void);

static const char *oxb_iova_mode_to_str(enum rte_iova_mode mode)
{
	switch (mode) {
	case RTE_IOVA_PA:
		return "pa";
	case RTE_IOVA_VA:
		return "va";
	case RTE_IOVA_DC:
	default:
		return "dc";
	}
}

// NOTE: Currently, all IOs by other than IO workers are handled by IO worker
// thread 0 as tls_tid is 0.
//
// #define SD_TID_VALIDATE
#ifdef SD_TID_VALIDATE
static uint32_t g_tid_validate_table[16] = { 0 };
#endif

PF_TL_EVT(e001_nvme_rd_submit_bh);
PF_TL_EVT(e002_nvme_rd_submit_bio);
PF_TL_EVT(e002a_nvme_seq_acquire);
PF_TL_EVT(e002a1_nvme_seq_check_free);
PF_TL_EVT(e002a2_nvme_seq_mark_busy);
PF_TL_EVT(e002a3_nvme_seq_qdepth_sample);
PF_TL_EVT(e002b_nvme_spdk_submit);
PF_TL_EVT(e002c_nvme_poll_loop);
PF_TL_EVT(e002d_nvme_poll_once);
PF_TL_EVT(e002rb_nvme_copy_from_spdk);
PF_TL_EVT(e002rc_nvme_end_io);
PF_TL_EVT(e002r_read_complete);

/* Real-time bandwidth/queue-depth tracking for NVMe read path.
 *  - g_bw_nvme_submit  : bytes submitted to NVMe (per SPDK read command)
 *  - g_bw_nvme_complete: bytes completed back to upper layer (__read_complete)
 *  - g_q_nvme_seqs     : sequence ring occupancy (aggregated)
 *  - g_q_nvme_inflight : per-worker inflight I/O count in nvme_submit_bio()
 *
 * check_rt_bw() / check_rt_q() print real-time stats once per second.
 */
#ifdef OXBOW_TRACK_TPUT
static rt_bw_stat g_bw_nvme_submit;
static rt_bw_stat g_bw_nvme_complete;
/* Real-time queue depth for NVMe sequence ring (aggregated across workers). */
static rt_q_stat g_q_nvme_seqs;
/* Per-IO-worker inflight NVMe I/O count seen by nvme_submit_bio(). */
static rt_q_stat *g_q_nvme_inflight;
static char **g_q_nvme_inflight_name;
/* Read pump/fsync QoS gauges for tuning. */
static rt_q_stat g_q_nvme_rd_pump_active;
static rt_q_stat g_q_nvme_rd_pump_scheduled;
static rt_q_stat g_q_nvme_fsync_inflight;
#endif

unsigned long g_nvme_ra_blknr;
unsigned long g_nvme_read_blknr;

/* NVMe read statistics split by BIO source.
 *  - g_nvme_user_ra_pages: pages read for BIO_FLAG_USER_RA (user-level RA)
 *  - g_nvme_other_pages  : pages read for all other BIOs (kernel RA/READPAGE/etc.)
 *
 * These counters are best-effort (relaxed atomics) and are dumped at daemon
 * shutdown via nvme_ra_stats_dump() for offline analysis.
 */
static atomic_ullong g_nvme_user_ra_pages;
static atomic_ullong g_nvme_other_pages;

/* Budgeted NVMe read pump control.
 *
 * Read BIOs are drained by short-lived pump jobs scheduled on demand.
 * This keeps the pool work-conserving (no dedicated read thread reservation)
 * while allowing fsync-aware QoS knobs.
 */
static atomic_int g_nvme_rd_workers_stop = ATOMIC_VAR_INIT(0);
static atomic_int g_nvme_rd_pump_active = ATOMIC_VAR_INIT(0);
static atomic_int g_nvme_rd_pump_scheduled = ATOMIC_VAR_INIT(0);
static atomic_int g_nvme_fsync_inflight = ATOMIC_VAR_INIT(0);

/* QoS defaults. Keep them conservative and tune by experiments.
 *
 * The submit_budget caps how many BIOs a single pump processes before
 * exiting; it no longer controls concurrent pump fan-out (see
 * nvme_rd_desired_pumps()). With fan-out decoupled, this can be raised
 * up to nvme_target_qd to also drive per-qpair queue depth.
 *
 * The fsync read-pump cap is expressed as a percentage of g_num_qpair
 * so it scales automatically when storage_engine_thread_num is
 * adjusted. 25% reproduces the historical absolute value of 2 pumps on
 * an 8-qpair configuration.
 */
static int g_nvme_rd_pump_max_normal = 0; /* 0 means "auto: g_num_qpair". */
static int g_nvme_rd_pump_pct_during_fsync = 25; /* NOTE: configuration knob: % of g_num_qpair. */
static int g_nvme_rd_submit_budget_normal = 16;
static int g_nvme_rd_submit_budget_during_fsync = 8;
static int g_nvme_rd_min_quota_during_fsync = 1;

// struct config {
// 	struct spdk_nvme_transport_id nvme_dev_trid;
// };
// static struct config g_config = { 0 };

/* Per-VF transport IDs filled in by nvme_init(). */
static struct spdk_nvme_transport_id g_trids[NVME_MAX_VFS];
struct ctrlr_entry {
	struct spdk_nvme_ctrlr *ctrlr;
	TAILQ_ENTRY(ctrlr_entry) link;
	char name[1024];
	char traddr[NVME_MAX_TRADDR_LEN]; /* PCIe BDF of this ctrlr (VF). */
};

/* =========================
 * Controller capability helpers
 * ========================= */

struct nvme_feat_ctx {
	uint32_t value;
	int done;
};

/* Completion callback for synchronous admin Get Feature (Number of Queues). */
static void nvme_get_num_queues_cb(void *arg,
				   const struct spdk_nvme_cpl *cpl)
{
	struct nvme_feat_ctx *ctx = arg;

	if (spdk_nvme_cpl_is_error(cpl))
		ctx->value = 0;
	else
		ctx->value = cpl->cdw0;

	ctx->done = 1;
}

/**
 * @brief Query HW Number of Queues (Feature ID = 7) and return
 *        effective maximum IO qpair count (min(NSQ+1, NCQ+1)).
 *
 * This uses a synchronous admin Get Feature command on the controller.
 * On failure, it returns 0.
 */
static uint32_t nvme_get_hw_max_io_qpairs(struct spdk_nvme_ctrlr *ctrlr)
{
	struct nvme_feat_ctx ctx;
	int rc;

	ctx.value = 0;
	ctx.done = 0;

	/* Feature ID 7 (Number of Queues) per NVMe spec. */
	rc = spdk_nvme_ctrlr_cmd_get_feature(
		ctrlr,
		0x07, /* Number of Queues */
		0,    /* cdw11 */
		NULL,
		0,
		nvme_get_num_queues_cb,
		&ctx);
	if (rc != 0)
		return 0;

	/* Drive admin completions until callback runs. */
	while (!ctx.done) {
		rc = spdk_nvme_ctrlr_process_admin_completions(ctrlr);
		if (rc < 0)
			return 0;
	}

	/* cdw0: bits 15:00 = NSQ (Submission queues - 1)
	 *       bits 31:16 = NCQ (Completion queues - 1)
	 */
	{
		uint16_t nsq = (uint16_t)(ctx.value & 0xFFFFu) + 1;
		uint16_t ncq = (uint16_t)((ctx.value >> 16) & 0xFFFFu) + 1;

		return (nsq < ncq) ? nsq : ncq;
	}
}

/* Sequence allocator API (forward declarations) */
int nvme_seqs_try_acquire(uint32_t needed, uint32_t *out_start);
int nvme_seqs_try_acquire_one(uint32_t *out_idx);
void nvme_seqs_release(uint32_t start, uint32_t count);
uint32_t nvme_seqs_available(void);
uint32_t nvme_seqs_capacity_current(void);
struct nvme_sequence *nvme_seqs_get_slot(uint32_t idx);
void nvme_seqs_reset(void);
int nvme_seqs_index_of(struct nvme_sequence *slot);
void nvme_seqs_release_one_by_ptr(struct nvme_sequence *slot);

struct ns_entry {
	struct spdk_nvme_ctrlr *ctrlr;
	struct spdk_nvme_ns *ns;
	TAILQ_ENTRY(ns_entry) link;
	uint64_t max_io_size; // Max size of a io request.
	uint32_t max_io_queue_requests; // Max number of the queues in a qpair.
	uint32_t nsid; // Namespace id (copied for convenience / logging).
	char traddr[NVME_MAX_TRADDR_LEN]; // PCIe BDF owning this ns_entry.
};

struct nvme_sequence {
	char *spdk_buf;
	struct bio *bio;
	baddr_t bi_start; // block address
	uint32_t nr; // how many blocks?
	nvme_write_track_cb wr_track_cb;
	void *wr_track_ctx;
	uint32_t wr_track_slot_idx;
};

// spdk user thread (not pthread)
struct io_thread {
	struct spdk_nvme_qpair *qpair;
	// struct spdk_thread *thread;
	struct ns_entry *ns_entry;
	struct nvme_sequence *seqs;
	struct nvme_sequence bh_seqs;
	char *desc_blk_buf; // 4KB DMA buffer for desc block.
	char *commit_buf; // 4KB DMA buffer for commit block.
	// Sequence ring allocator state (for concurrent users of seqs) (Read IO only)
	uint32_t seqs_head; // next index to allocate from
	uint32_t seqs_tail; // oldest in-use index (advances as completions free)
	uint32_t seqs_used; // number of slots currently in use
	uint32_t seqs_capacity; // total slots in seqs
	uint8_t *seqs_busy; // busy bitmap; 1 means in use, 0 means free
	// Pending read BIO being partially submitted by this worker.
	// Used to continue submission when NVMe queue was full in a previous round.
	struct bio *pending_rd_bio;
	uint32_t pending_rd_pg_off; // how many pages of pending_rd_bio already submitted
};
static int g_num_qpair = 1;
static int seq_max_per_thread = 1;

static inline int nvme_is_fsync_inflight(void)
{
	return atomic_load_explicit(&g_nvme_fsync_inflight,
				    memory_order_relaxed) > 0;
}

static inline int nvme_rd_budget_for_qos(int fsync_active)
{
	int budget = fsync_active ? g_nvme_rd_submit_budget_during_fsync :
				    g_nvme_rd_submit_budget_normal;

	if (budget <= 0)
		budget = 1;
	return budget;
}

static inline int nvme_rd_max_pumps_for_qos(int fsync_active)
{
	int hard_max = (g_num_qpair > 0) ? g_num_qpair : 1;
	int cfg_max;

	if (fsync_active) {
		int pct = g_nvme_rd_pump_pct_during_fsync;

		if (pct < 0)
			pct = 0;
		if (pct > 100)
			pct = 100;
		/* ceil(hard_max * pct / 100): scales with qpair count. */
		cfg_max = (hard_max * pct + 99) / 100;
	} else if (g_nvme_rd_pump_max_normal > 0)
		cfg_max = g_nvme_rd_pump_max_normal;
	else
		cfg_max = hard_max;

	if (cfg_max <= 0)
		cfg_max = 1;
	if (cfg_max > hard_max)
		cfg_max = hard_max;
	return cfg_max;
}

static inline int nvme_rd_min_quota_for_qos(int fsync_active)
{
	int min_quota;
	int max_pumps;

	if (!fsync_active)
		return 0;

	min_quota = g_nvme_rd_min_quota_during_fsync;
	max_pumps = nvme_rd_max_pumps_for_qos(fsync_active);

	if (min_quota < 0)
		min_quota = 0;
	if (min_quota > max_pumps)
		min_quota = max_pumps;
	return min_quota;
}
/* Target queue depth for nvme_submit_bio().
 * This is a soft limit: the submit/poll loop will try to keep
 * inflight I/Os close to this value without introducing extra
 * latency when workload cannot fully saturate it.
 * Tune this value (e.g., 8, 16, 32) for performance experiments.
 */
int nvme_target_qd = 32; // OPTIMIZE: 16 or 32 is the best for seq read with num app 1 or 8.

/* Debug/experimental knob:
 *   - When non-zero, nvme_submit_bio() disables pipelined submission
 *     while polling completions. It will:
 *       * submit one batch of work via submit_fn(), then
 *       * poll until all of that batch completes,
 *       * and only then submit the next batch.
 *
 *   - When zero, the original pipelined behaviour is used: nvme_submit_bio()
 *     keeps topping up inflight IOs up to nvme_target_qd while interleaving
 *     periodic completion polling.
 *
 * Controlled via OXBOW_NVME_NO_ASYNC_SUBMIT_DURING_POLL in oxbow_debug.h.
 */
static int nvme_no_async_submit_during_poll =
#ifdef OXBOW_NVME_NO_ASYNC_SUBMIT_DURING_POLL
	1;
#else
	0;
#endif
struct io_thread *g_io_threads;
static int is_io_thpool_init = false;
uint64_t g_max_nvme_max_io_size;

static TAILQ_HEAD(, ctrlr_entry)
	g_controllers = TAILQ_HEAD_INITIALIZER(g_controllers);
static TAILQ_HEAD(,
		  ns_entry) g_namespaces = TAILQ_HEAD_INITIALIZER(g_namespaces);

static inline uint32_t lba_to_sector(baddr_t lba)
{
	return lba * (OXBOW_BLOCK_SIZE / SECTOR_SIZE);
}
static inline uint32_t bytes_to_sector(uint32_t size_in_bytes)
{
	return CEILING(size_in_bytes, SECTOR_SIZE);
}

#ifdef OXBOW_TRACK_TPUT
static inline rt_q_stat *nvme_inflight_qstat_for_current_worker(void)
{
	int tid = tls_tid;

	if (!g_q_nvme_inflight)
		return NULL;

	if (tid < 0)
		tid = 0;
	if (tid >= g_num_qpair)
		tid = g_num_qpair - 1;
	if (tid < 0)
		return NULL;

	return &g_q_nvme_inflight[tid];
}

static inline void nvme_sample_qos_gauges(void)
{
	int active;
	int scheduled;
	int inflight;

	active = atomic_load_explicit(&g_nvme_rd_pump_active,
				      memory_order_relaxed);
	scheduled = atomic_load_explicit(&g_nvme_rd_pump_scheduled,
					 memory_order_relaxed);
	inflight = atomic_load_explicit(&g_nvme_fsync_inflight,
					memory_order_relaxed);

	if (active < 0)
		active = 0;
	if (scheduled < 0)
		scheduled = 0;
	if (inflight < 0)
		inflight = 0;

	check_rt_q(&g_q_nvme_rd_pump_active, (uint32_t)active, 0);
	check_rt_q(&g_q_nvme_rd_pump_scheduled, (uint32_t)scheduled, 0);
	check_rt_q(&g_q_nvme_fsync_inflight, (uint32_t)inflight, 0);
}
#else
static inline void nvme_sample_qos_gauges(void)
{
}
#endif

static bool probe_cb(void *cb_ctx, const struct spdk_nvme_transport_id *trid,
		     struct spdk_nvme_ctrlr_opts *opts)
{
	// To suppress Unused warning.
	ALL_UNUSED(cb_ctx, trid, opts);

	// oxb_debug("Probing %s -D: %s", trid->traddr,
	// 	  &g_config.nvme_dev_trid.traddr);

	// if (strcmp(g_config.nvme_dev_trid.traddr, trid->traddr) == 0) {
	// 	oxb_debug("Attaching to %s", trid->traddr);
	// 	return true;
	// } else {
	// 	oxb_debug("Not attaching to %s", trid->traddr);
	// 	return false;
	// }

	return true;
}

static void register_ns(struct spdk_nvme_ctrlr *ctrlr, struct spdk_nvme_ns *ns,
			const char *traddr)
{
	struct ns_entry *entry;

	if (!spdk_nvme_ns_is_active(ns)) {
		return;
	}

	entry = malloc(sizeof(struct ns_entry));
	if (entry == NULL) {
		perror("ns_entry malloc");
		exit(1);
	}

	entry->ctrlr = ctrlr;
	entry->ns = ns;
	entry->nsid = spdk_nvme_ns_get_id(ns);
	if (traddr)
		snprintf(entry->traddr, sizeof(entry->traddr), "%s", traddr);
	else
		entry->traddr[0] = '\0';
	TAILQ_INSERT_TAIL(&g_namespaces, entry, link);

	log_info("  traddr=%s Namespace ID: %d size: %juGB", entry->traddr,
		 entry->nsid, spdk_nvme_ns_get_size(ns) / 1000000000);
}

static void attach_cb(void *cb_ctx, const struct spdk_nvme_transport_id *trid,
		      struct spdk_nvme_ctrlr *ctrlr,
		      const struct spdk_nvme_ctrlr_opts *opts)
{
	int nsid;
	uint32_t hw_max_qpairs = 0;
	struct ctrlr_entry *entry;
	struct spdk_nvme_ns *ns;
	const struct spdk_nvme_ctrlr_data *cdata;

	// To suppress Unused warning.
	ALL_UNUSED(cb_ctx, NULL);

	entry = malloc(sizeof(struct ctrlr_entry));
	if (entry == NULL) {
		perror("ctrlr_entry malloc");
		exit(1);
	}

	/* Log controller-level queue depth and (best-effort) max IO queue count. */
	hw_max_qpairs = nvme_get_hw_max_io_qpairs(ctrlr);
	if (hw_max_qpairs > 0) {
		oxb_info("Attached to %s  Q_dep: %d, HW max_io_qpairs=%u",
			 trid->traddr,
			 opts->io_queue_size,
			 hw_max_qpairs);
	} else {
		oxb_info("Attached to %s  Q_dep: %d, HW max_io_qpairs=unknown",
			 trid->traddr,
			 opts->io_queue_size);
	}

	/*
	* spdk_nvme_ctrlr is the logical abstraction in SPDK for an NVMe
	*  controller.  During initialization, the IDENTIFY data for the
	*  controller is read using an NVMe admin command, and that data
	*  can be retrieved using spdk_nvme_ctrlr_get_data() to get
	*  detailed information on the controller.  Refer to the NVMe
	*  specification for more details on IDENTIFY for NVMe controllers.
	*/
	cdata = spdk_nvme_ctrlr_get_data(ctrlr);

	snprintf(entry->name, sizeof(entry->name), "%-20.20s (%-20.20s)",
		 cdata->mn, cdata->sn);

	entry->ctrlr = ctrlr;
	snprintf(entry->traddr, sizeof(entry->traddr), "%s", trid->traddr);
	TAILQ_INSERT_TAIL(&g_controllers, entry, link);

	/*
	* Each controller has one or more namespaces.  An NVMe namespace is basically
	*  equivalent to a SCSI LUN.  The controller's IDENTIFY data tells us how
	*  many namespaces exist on the controller.  For Intel(R) P3X00 controllers,
	*  it will just be one namespace.
	*
	* Note that in NVMe, namespace IDs start at 1, not 0.
	*/
	for (nsid = spdk_nvme_ctrlr_get_first_active_ns(ctrlr); nsid != 0;
	     nsid = spdk_nvme_ctrlr_get_next_active_ns(ctrlr, nsid)) {
		ns = spdk_nvme_ctrlr_get_ns(ctrlr, nsid);
		if (ns == NULL) {
			continue;
		}
		register_ns(ctrlr, ns, trid->traddr);
	}
}

// For now, we only consider the case, `lba_count > sectors_per_max_io` in
// `_nvme_ns_cmd_rw()` function.
static uint64_t get_max_io_size(struct ns_entry *entry)
{
	uint32_t sector_size, sectors_per_max_io, sectors_per_max_io_no_md;

	sectors_per_max_io = spdk_nvme_ns_get_max_io_xfer_size(entry->ns) /
			     spdk_nvme_ns_get_extended_sector_size(entry->ns);

	sectors_per_max_io_no_md =
		spdk_nvme_ns_get_max_io_xfer_size(entry->ns) /
		spdk_nvme_ns_get_sector_size(entry->ns);

	sector_size = spdk_nvme_ns_get_sector_size(entry->ns);

	BUG_ON(sector_size != SECTOR_SIZE, "size wrong");

	// Because io_flags == 0
	BUG_ON(sectors_per_max_io != sectors_per_max_io_no_md, "config");

	return (uint64_t)sectors_per_max_io_no_md * sector_size;
}

/**
 * @brief Allocate per-worker SPDK resources distributed across multiple VFs.
 *
 * Workers are bound to VFs in order. Each VF v owns qpairs_per_vf[v]
 * consecutive tid slots. Inside each slot we allocate a qpair from that VF's
 * controller and wire worker->ns_entry to that VF's ns_entry so every IO
 * path (which dereferences g_io_threads[tls_tid]) naturally routes to the
 * owning VF.
 *
 * @param ns_entries     Array of ns_entry* in probe order (one per VF).
 * @param nr_vfs         Number of VFs (length of ns_entries / qpairs_per_vf).
 * @param qpairs_per_vf  How many qpairs to allocate from each VF.
 * @param num_qpair      Total qpair (worker) count = number of g_io_threads
 *                       slots to provision. Must equal sum(qpairs_per_vf).
 * @param iod_thread_nr  Size of the iod_workers thpool (tid 0..iod_thread_nr-1).
 *                       Trailing tids [iod_thread_nr, num_qpair) are not
 *                       owned by the thpool; external workers (e.g., D-2
 *                       read_workers) bind to them manually.
 */
static struct thpool_ *create_spdk_io_threads_multi(
	struct ns_entry **ns_entries, int nr_vfs,
	const int *qpairs_per_vf, int num_qpair, int iod_thread_nr)
{
	struct spdk_nvme_io_qpair_opts qpair_opts;
	struct thpool_ *ret = NULL;
	uint64_t min_max_io_size = UINT64_MAX;
	uint32_t min_io_queue_size = UINT32_MAX;
	int tid = 0;
	int v, k, j;
	int total_check = 0;

	if (is_io_thpool_init) {
		oxb_error("IO thpool already initialized");
		return NULL;
	}

	for (v = 0; v < nr_vfs; v++)
		total_check += qpairs_per_vf[v];
	if (total_check != num_qpair) {
		oxb_error("qpairs_per_vf sum(%d) != num_qpair(%d)",
			  total_check, num_qpair);
		return NULL;
	}

	/* Take the minimum of per-VF capabilities so every worker has
	 * consistent buffer sizes and queue depth. SR-IOV VFs of the same
	 * namespace should report identical values, but defend against
	 * mismatched VFs just in case.
	 */
	for (v = 0; v < nr_vfs; v++) {
		uint64_t vf_max_io;
		vf_max_io = get_max_io_size(ns_entries[v]);
		if (vf_max_io < min_max_io_size)
			min_max_io_size = vf_max_io;

		spdk_nvme_ctrlr_get_default_io_qpair_opts(
			ns_entries[v]->ctrlr, &qpair_opts,
			sizeof(qpair_opts));
		if (qpair_opts.io_queue_size < min_io_queue_size)
			min_io_queue_size = qpair_opts.io_queue_size;
	}

	g_max_nvme_max_io_size = min_max_io_size;
#ifdef OXBOW_EXPERIMENT_NVME_IO_SIZE_CAP_KB
	/* Experimental: cap max read I/O size to a smaller value to study
	 * how per-command size vs. queue depth affects throughput.
	 */
	if (g_max_nvme_max_io_size >
	    ((uint64_t)OXBOW_EXPERIMENT_NVME_IO_SIZE_CAP_KB << 10)) {
		g_max_nvme_max_io_size =
			(uint64_t)OXBOW_EXPERIMENT_NVME_IO_SIZE_CAP_KB << 10;
	}
#endif
	seq_max_per_thread = (int)min_io_queue_size;

	oxb_debug("[%s] nr_vfs=%d max IO size %lu KiB, max req %d",
		  __func__, nr_vfs, g_max_nvme_max_io_size >> 10,
		  seq_max_per_thread);

	for (v = 0; v < nr_vfs; v++) {
		struct ns_entry *ne = ns_entries[v];

		/* Use this VF's own default qpair opts so controller-specific
		 * settings (e.g., qd) are respected. delay_cmd_submit=0 to
		 * match the original single-VF behaviour.
		 */
		spdk_nvme_ctrlr_get_default_io_qpair_opts(
			ne->ctrlr, &qpair_opts, sizeof(qpair_opts));
		qpair_opts.delay_cmd_submit = 0;

		for (k = 0; k < qpairs_per_vf[v]; k++, tid++) {
			g_io_threads[tid].ns_entry = ne;

			g_io_threads[tid].qpair =
				spdk_nvme_ctrlr_alloc_io_qpair(
					ne->ctrlr, &qpair_opts, 0);
			if (!g_io_threads[tid].qpair) {
				fprintf(stderr,
					"Failed to alloc qpair for tid=%d "
					"(VF=%s)\n",
					tid, ne->traddr);
				exit(1);
			}

			g_io_threads[tid].seqs = malloc(
				seq_max_per_thread *
				sizeof(struct nvme_sequence));
			if (!g_io_threads[tid].seqs) {
				perror("malloc fail");
				exit(1);
			}

			for (j = 0; j < seq_max_per_thread; j++) {
				g_io_threads[tid].seqs[j].spdk_buf =
					spdk_malloc(g_max_nvme_max_io_size,
						    0x1000, NULL,
						    SPDK_ENV_SOCKET_ID_ANY,
						    SPDK_MALLOC_DMA);
				if (!g_io_threads[tid].seqs[j].spdk_buf) {
					perror("spdk_malloc fail");
					exit(1);
				}
				g_io_threads[tid].seqs[j].wr_track_cb = NULL;
				g_io_threads[tid].seqs[j].wr_track_ctx = NULL;
				g_io_threads[tid].seqs[j].wr_track_slot_idx =
					0;
			}

			g_io_threads[tid].seqs_capacity =
				(uint32_t)seq_max_per_thread;
			g_io_threads[tid].seqs_head = 0;
			g_io_threads[tid].seqs_tail = 0;
			g_io_threads[tid].seqs_used = 0;
			g_io_threads[tid].seqs_busy = calloc(
				(size_t)seq_max_per_thread, sizeof(uint8_t));
			if (!g_io_threads[tid].seqs_busy) {
				perror("calloc fail");
				exit(1);
			}

			g_io_threads[tid].bh_seqs.spdk_buf = spdk_malloc(
				PAGE_SIZE, 0x1000, NULL,
				SPDK_ENV_SOCKET_ID_ANY, SPDK_MALLOC_DMA);
			if (!g_io_threads[tid].bh_seqs.spdk_buf) {
				perror("spdk_malloc fail");
				exit(1);
			}

			g_io_threads[tid].desc_blk_buf = spdk_malloc(
				PAGE_SIZE, 0x1000, NULL,
				SPDK_ENV_SOCKET_ID_ANY, SPDK_MALLOC_DMA);
			if (!g_io_threads[tid].desc_blk_buf) {
				perror("spdk_malloc fail");
				exit(1);
			}

			g_io_threads[tid].commit_buf = spdk_malloc(
				PAGE_SIZE, 0x1000, NULL,
				SPDK_ENV_SOCKET_ID_ANY, SPDK_MALLOC_DMA);
			if (!g_io_threads[tid].commit_buf) {
				perror("spdk_malloc fail");
				exit(1);
			}

			g_io_threads[tid].pending_rd_bio = NULL;
			g_io_threads[tid].pending_rd_pg_off = 0;
		}
	}

	ret = spdk_thpool_init_with_pinning(iod_thread_nr, "oxbow_ioworkers",
					    OXB_PIN_DOMAIN_SECURE_DAEMON);

	is_io_thpool_init = true;

	return ret;
}

/* Build the DPDK EAL core mask by unioning the per-tid pinning targets
 * returned by oxb_sd_pin_cpu_for_tid(). Using the same policy as the IO
 * thread pool ensures DPDK initializes lcores on every NUMA node that
 * will host an IO worker, so hugepage pools and per-lcore structures
 * are spread across sockets and spdk_malloc(SPDK_ENV_SOCKET_ID_ANY) DMA
 * buffers land on the same socket as the IO worker that uses them.
 */
static int create_core_mask(int num_qpair, char *core_mask_str)
{
	unsigned long core_mask = 0;
	int numa_cpu_count[2] = { 0, 0 };

	if (num_qpair > OXBOW_TOTAL_IO_THREAD_NR_MAX) {
		int requested = num_qpair;

		num_qpair = OXBOW_TOTAL_IO_THREAD_NR_MAX;
		oxb_info("max io worker %d but you ask for %d: setting to max",
			 OXBOW_TOTAL_IO_THREAD_NR_MAX, requested);
	}

	for (int tid = 0; tid < num_qpair; tid++) {
		int cpu = oxb_sd_pin_cpu_for_tid(tid, num_qpair);
		int numa = oxb_sd_pin_numa_for_tid(tid, num_qpair);
		if (cpu < 0 || cpu >= (int)(sizeof(core_mask) * 8)) {
			oxb_error("create_core_mask: cpu %d for tid %d out of "
				  "mask range (total=%d); check "
				  "cpu_pinning.h",
				  cpu, tid, num_qpair);
			return -1;
		}
		core_mask |= (1ULL << cpu);
		if (numa >= 0 && numa < 2)
			numa_cpu_count[numa]++;
		oxb_info("[pinning] io_worker tid=%-2d -> CPU %-3d (NUMA%d)",
			 tid, cpu, numa);
	}

	snprintf(core_mask_str, 19, "0x%lx", core_mask);

	oxb_info("[pinning] total io workers=%d, NUMA0=%d, NUMA1=%d, "
		 "DPDK core_mask=%s",
		 num_qpair, numa_cpu_count[0], numa_cpu_count[1],
		 core_mask_str);
	return 0;
}

/**
 * @param iod_thread_nr (# of iod_workers thpool threads)
 * @param total_qpair_nr (# of g_io_threads slots, including any reserved
 *                       for external workers like D-2 read_workers)
 * @return iod_workers thpool handle on success, NULL on failure.
 */
struct thpool_ *nvme_init(struct nvme_config *nvme_conf, int iod_thread_nr,
			  int total_qpair_nr)
{
	struct spdk_env_opts opts;
	struct ns_entry *ns_iter;
	struct thpool_ *ret = NULL;
	// char core_mask[20] = "0xFFFF0000";
	char core_mask[20];
	struct ns_entry *ns_entries[NVME_MAX_VFS] = { NULL };
	int qpairs_per_vf[NVME_MAX_VFS] = { 0 };
	int nr_vfs;
	int ctrlr_cnt;
	int ns_cnt;
	int base_qp, rem_qp;
	int num_qpair = total_qpair_nr;
	struct ctrlr_entry *ctrlr_iter;

	if (iod_thread_nr <= 0 || total_qpair_nr < iod_thread_nr) {
		fprintf(stderr,
			"nvme_init: invalid iod_thread_nr(%d) or "
			"total_qpair_nr(%d)\n",
			iod_thread_nr, total_qpair_nr);
		return ret;
	}

	if (!nvme_conf || nvme_conf->nr_addrs <= 0) {
		fprintf(stderr,
			"nvme_init: no PCIe BDFs provided (nr_addrs=%d)\n",
			nvme_conf ? nvme_conf->nr_addrs : -1);
		return ret;
	}
	if (nvme_conf->nr_addrs > NVME_MAX_VFS) {
		fprintf(stderr,
			"nvme_init: nr_addrs(%d) > NVME_MAX_VFS(%d)\n",
			nvme_conf->nr_addrs, NVME_MAX_VFS);
		return ret;
	}
	nr_vfs = nvme_conf->nr_addrs;

	if (num_qpair < nr_vfs) {
		fprintf(stderr,
			"nvme_init: num_qpair(%d) < nr_vfs(%d); need at "
			"least one qpair per VF\n",
			num_qpair, nr_vfs);
		return ret;
	}

	if (create_core_mask(num_qpair, core_mask))
		return ret;

#ifdef OXBOW_TRACK_TPUT
	/* Initialize real-time bandwidth stats for NVMe read path. */
	init_rt_bw_stat(&g_bw_nvme_submit, "nvme_submit");
	init_rt_bw_stat(&g_bw_nvme_complete, "nvme_complete");
	/* Initialize queue-depth tracker for NVMe sequence ring. */
	init_rt_q_stat(&g_q_nvme_seqs, "nvme_seqs");
	init_rt_q_stat(&g_q_nvme_rd_pump_active, "nvme_rd_pump_active");
	init_rt_q_stat(&g_q_nvme_rd_pump_scheduled, "nvme_rd_pump_sched");
	init_rt_q_stat(&g_q_nvme_fsync_inflight, "nvme_fsync_inflight");
	/* Allocate per-worker inflight tracker for nvme_submit_bio() loop. */
	g_q_nvme_inflight = calloc((size_t)num_qpair,
				   sizeof(*g_q_nvme_inflight));
	g_q_nvme_inflight_name = calloc(
		(size_t)num_qpair, sizeof(*g_q_nvme_inflight_name));
	if (!g_q_nvme_inflight || !g_q_nvme_inflight_name) {
		fprintf(stderr,
			"Unable to allocate nvme inflight queue stats\n");
		goto exit;
	}
	for (int i = 0; i < num_qpair; i++) {
		g_q_nvme_inflight_name[i] = malloc(32);
		if (!g_q_nvme_inflight_name[i]) {
			fprintf(stderr,
				"Unable to allocate name for nvme inflight queue stats\n");
			goto exit;
		}
		snprintf(g_q_nvme_inflight_name[i], 32, "nvme_inflight_%d",
			 i);
		init_rt_q_stat(&g_q_nvme_inflight[i],
			       g_q_nvme_inflight_name[i]);
	}
#endif

	spdk_env_opts_init(&opts);

	/* Populate one trid per VF. All share the PCIe transport and the
	 * discovery subnqn; only traddr differs.
	 */
	for (int v = 0; v < nr_vfs; v++) {
		spdk_nvme_trid_populate_transport(&g_trids[v],
						  SPDK_NVME_TRANSPORT_PCIE);
		snprintf(g_trids[v].traddr, SPDK_NVMF_TRADDR_MAX_LEN + 1,
			 "%s", nvme_conf->pcie_addrs[v]);
		snprintf(g_trids[v].subnqn, sizeof(g_trids[v].subnqn), "%s",
			 SPDK_NVMF_DISCOVERY_NQN);
	}

	opts.name = "oxbow_nvme_local";
	opts.core_mask = core_mask;
	if (spdk_env_init(&opts) < 0) {
		fprintf(stderr, "Unable to initialize SPDK env\n");
		return ret;
	}

	/*
	 * Log the effective DPDK IOVA mode chosen after SPDK environment
	 * initialization so that we can confirm whether DMA uses VA or PA.
	 */
	{
		enum rte_iova_mode eff_mode = rte_eal_iova_mode();

		oxb_info(
			"[nvme_init] Effective DPDK IOVA mode after spdk_env_init: %s (enum=%d)",
			oxb_iova_mode_to_str(eff_mode), (int)eff_mode);
	}

	/* Probe each VF in order. attach_cb() stores per-ctrlr traddr and
	 * register_ns() stores per-ns traddr / nsid, so we can later map
	 * nsid-identical ns_entries back to their owning VFs reliably.
	 */
	for (int v = 0; v < nr_vfs; v++) {
		oxb_info("Probing NVMe VF[%d]: traddr=%s", v,
			 g_trids[v].traddr);
		if (spdk_nvme_probe(&g_trids[v], NULL, probe_cb, attach_cb,
				    NULL)) {
			fprintf(stderr,
				"spdk_nvme_probe() failed for traddr=%s\n",
				g_trids[v].traddr);
			goto exit;
		}
	}

	if (TAILQ_EMPTY(&g_controllers)) {
		fprintf(stderr, "no NVMe controllers found\n");
		goto exit;
	}

	ctrlr_cnt = 0;
	TAILQ_FOREACH(ctrlr_iter, &g_controllers, link) ctrlr_cnt++;
	if (ctrlr_cnt != nr_vfs) {
		fprintf(stderr,
			"nvme_init: attached ctrlr count(%d) != nr_vfs(%d). "
			"Check SR-IOV binding / SPDK allow-list.\n",
			ctrlr_cnt, nr_vfs);
		goto exit;
	}

	/* Collect one ns_entry per VF in probe order. With SR-IOV, each VF
	 * exposes the same logical namespace, so we pick the first active
	 * namespace owned by each ctrlr (typically the only one).
	 */
	ns_cnt = 0;
	for (int v = 0; v < nr_vfs; v++) {
		const char *want = g_trids[v].traddr;
		TAILQ_FOREACH(ns_iter, &g_namespaces, link)
		{
			if (strcmp(ns_iter->traddr, want) == 0) {
				ns_entries[v] = ns_iter;
				ns_cnt++;
				break;
			}
		}
		if (!ns_entries[v]) {
			fprintf(stderr,
				"nvme_init: no active namespace under VF "
				"traddr=%s\n",
				want);
			goto exit;
		}
	}

	/* Validate that all VFs expose the same logical namespace. This is
	 * a hard SR-IOV invariant for the design to make sense: tids on
	 * different VFs must write to the same backing LBA space.
	 */
	{
		uint32_t nsid0 = ns_entries[0]->nsid;
		uint32_t sector_size0 =
			spdk_nvme_ns_get_sector_size(ns_entries[0]->ns);
		for (int v = 1; v < nr_vfs; v++) {
			uint32_t nsid_v = ns_entries[v]->nsid;
			uint32_t sector_size_v = spdk_nvme_ns_get_sector_size(
				ns_entries[v]->ns);
			if (nsid_v != nsid0 || sector_size_v != sector_size0) {
				fprintf(stderr,
					"nvme_init: VF[%d] (traddr=%s) "
					"nsid=%u sector=%u mismatch with "
					"VF[0] (traddr=%s) nsid=%u "
					"sector=%u\n",
					v, ns_entries[v]->traddr, nsid_v,
					sector_size_v,
					ns_entries[0]->traddr, nsid0,
					sector_size0);
				goto exit;
			}
		}
	}

	/* Per-VF qpair count = even split with remainder on the first VFs.
	 * Then clamp each VF's count to its HW limit and adjust num_qpair
	 * accordingly so downstream per-tid allocations remain consistent.
	 */
	base_qp = num_qpair / nr_vfs;
	rem_qp = num_qpair % nr_vfs;
	for (int v = 0; v < nr_vfs; v++)
		qpairs_per_vf[v] = base_qp + (v < rem_qp ? 1 : 0);

	{
		int adjusted_total = 0;
		for (int v = 0; v < nr_vfs; v++) {
			uint32_t hw_cap = nvme_get_hw_max_io_qpairs(
				ns_entries[v]->ctrlr);
			if (hw_cap > 0 &&
			    (uint32_t)qpairs_per_vf[v] > hw_cap) {
				oxb_warn(
					"VF[%d] traddr=%s: requested "
					"qpairs=%d exceeds HW cap=%u. "
					"Clamping.",
					v, ns_entries[v]->traddr,
					qpairs_per_vf[v], hw_cap);
				qpairs_per_vf[v] = (int)hw_cap;
			}
			adjusted_total += qpairs_per_vf[v];
		}
		if (adjusted_total != num_qpair) {
			oxb_warn(
				"nvme_init: adjusting num_qpair from %d to "
				"%d after per-VF HW clamping.",
				num_qpair, adjusted_total);
			num_qpair = adjusted_total;
		}
	}

	if (num_qpair <= 0) {
		fprintf(stderr,
			"nvme_init: no qpairs available after HW clamping\n");
		goto exit;
	}

	g_io_threads = malloc(num_qpair * sizeof(struct io_thread));
	if (!g_io_threads) {
		perror("malloc fail");
		goto exit;
	}
	g_num_qpair = num_qpair;

	/* HW clamping above may have shrunk num_qpair. Clamp iod_thread_nr
	 * accordingly so the thpool size never exceeds available qpairs. */
	if (iod_thread_nr > num_qpair) {
		oxb_warn("nvme_init: clamping iod_thread_nr from %d to %d "
			 "after HW qpair clamping",
			 iod_thread_nr, num_qpair);
		iod_thread_nr = num_qpair;
	}

	ret = create_spdk_io_threads_multi(ns_entries, nr_vfs, qpairs_per_vf,
					   num_qpair, iod_thread_nr);
	if (!ret)
		goto exit;

	// Print worker, NS, and qpair information
	nvme_info("===== NVMe Initialization Summary =====");
	nvme_info("  nr_vfs = %d, total qpairs = %d", nr_vfs, num_qpair);
	for (int v = 0; v < nr_vfs; v++) {
		nvme_info("  VF[%d] traddr=%s  nsid=%u  qpairs=%d", v,
			  ns_entries[v]->traddr, ns_entries[v]->nsid,
			  qpairs_per_vf[v]);
	}
	for (int i = 0; i < num_qpair; i++) {
		struct io_thread *worker = &g_io_threads[i];
		struct ns_entry *ns = worker->ns_entry;

		(void)worker; /* silence unused warn when nvme_info is off */
		(void)ns;

		nvme_info("Worker %d:", i);
		nvme_info("  -> traddr: %s", ns->traddr);
		nvme_info("  -> Namespace ID: %u", ns->nsid);
		nvme_info("  -> QPair: %p", worker->qpair);
		nvme_info("  -> Block size: %u bytes",
			  spdk_nvme_ns_get_sector_size(ns->ns));
		nvme_info("  -> Extended block size: %u bytes",
			  spdk_nvme_ns_get_extended_sector_size(ns->ns));
		nvme_info("  -> Max IO size: %lu bytes",
			  g_max_nvme_max_io_size);
		nvme_info("  -> Max queue requests: %d", seq_max_per_thread);
	}
	nvme_info("=======================================");

#ifdef SD_TID_VALIDATE
	oxb_warn("SD_TID_VALIDATE is enabled. Disable it for performance.");
#endif

	return ret;

exit:
	nvme_exit();
	return ret;
}

void nvme_init_rd_workers(int worker_nr)
{
	UNUSED1(worker_nr);
	atomic_store_explicit(&g_nvme_rd_workers_stop, 0, memory_order_relaxed);
	atomic_store_explicit(&g_nvme_rd_pump_active, 0, memory_order_relaxed);
	atomic_store_explicit(&g_nvme_rd_pump_scheduled, 0,
			      memory_order_relaxed);
	nvme_sample_qos_gauges();
}

void nvme_signal_rd_worker(void)
{
	nvme_schedule_read_pump();
}

void nvme_destroy_rd_workers(void)
{
	atomic_store_explicit(&g_nvme_rd_workers_stop, 1, memory_order_relaxed);
	nvme_sample_qos_gauges();
}

void nvme_fsync_inflight_inc(void)
{
	atomic_fetch_add_explicit(&g_nvme_fsync_inflight, 1,
				  memory_order_relaxed);
	nvme_sample_qos_gauges();
}

void nvme_fsync_inflight_dec(void)
{
	int prev = atomic_fetch_sub_explicit(&g_nvme_fsync_inflight, 1,
					     memory_order_relaxed);

	if (prev <= 0)
		atomic_store_explicit(&g_nvme_fsync_inflight, 0,
				      memory_order_relaxed);
	nvme_sample_qos_gauges();
}

int nvme_fsync_inflight_get(void)
{
	int v = atomic_load_explicit(&g_nvme_fsync_inflight,
				     memory_order_relaxed);
	return v < 0 ? 0 : v;
}

/*
 * Determine how many concurrent read pumps should be active for the
 * current backlog.
 *
 * Fan-out is one pump per pending BIO, capped by the per-qpair count
 * (max_pumps). The per-pump submit_budget no longer participates in
 * this decision; it only bounds how many BIOs a single pump processes
 * before exiting (enforced inside nvme_submit_bio()).
 *
 * Background: the previous formula `ceil(backlog / submit_budget)`
 * coupled the per-pump work cap with the concurrent pump count. With
 * the default budget=16 and a backlog under 16, only one pump was
 * desired regardless of how many qpairs were available, so a 4-app
 * sequential read burst on 8 qpairs serialized through a single qpair.
 * That mechanism is the dominant 4-16 app sequential read regression
 * documented in plans/2026-04-25-read-throughput-scalability-analysis.md
 * (§3.2.1).
 */
static inline int nvme_rd_desired_pumps(size_t backlog, int fsync_active)
{
	int desired;
	int max_pumps = nvme_rd_max_pumps_for_qos(fsync_active);
	int min_quota = nvme_rd_min_quota_for_qos(fsync_active);

	desired = (backlog > (size_t)max_pumps) ? max_pumps : (int)backlog;
	if (desired < 1)
		desired = 1;
	if (desired < min_quota)
		desired = min_quota;
	return desired;
}

void nvme_schedule_read_pump(void)
{
	size_t backlog;
	int fsync_active;
	int desired;
	int active;
	int scheduled;
	int outstanding;
	int needed;

	if (atomic_load_explicit(&g_nvme_rd_workers_stop, memory_order_relaxed))
		return;
	if (!iod_workers)
		return;

	backlog = ring_buffer_mpmc_count(&g_rd_bio_ring_buf);
	if (!backlog) {
		nvme_sample_qos_gauges();
		return;
	}

	fsync_active = nvme_is_fsync_inflight();
	desired = nvme_rd_desired_pumps(backlog, fsync_active);

	active = atomic_load_explicit(&g_nvme_rd_pump_active,
				      memory_order_relaxed);
	scheduled = atomic_load_explicit(&g_nvme_rd_pump_scheduled,
					 memory_order_relaxed);
	outstanding = active + scheduled;
	needed = desired - outstanding;

	while (needed > 0) {
		atomic_fetch_add_explicit(&g_nvme_rd_pump_scheduled, 1,
					  memory_order_relaxed);
		if (thpool_add_work(iod_workers, nvme_rd_submit_bio, NULL)) {
			atomic_fetch_sub_explicit(&g_nvme_rd_pump_scheduled, 1,
						  memory_order_relaxed);
			break;
		}
		needed--;
		nvme_sample_qos_gauges();
	}

	nvme_sample_qos_gauges();
}

static void cleanup(void)
{
	struct ns_entry *ns_entry, *tmp_ns_entry;
	struct ctrlr_entry *ctrlr_entry, *tmp_ctrlr_entry;
	struct spdk_nvme_detach_ctx *detach_ctx = NULL;

	TAILQ_FOREACH_SAFE(ns_entry, &g_namespaces, link, tmp_ns_entry)
	{
		TAILQ_REMOVE(&g_namespaces, ns_entry, link);
		free(ns_entry);
	}

	TAILQ_FOREACH_SAFE(ctrlr_entry, &g_controllers, link, tmp_ctrlr_entry)
	{
		TAILQ_REMOVE(&g_controllers, ctrlr_entry, link);
		spdk_nvme_detach_async(ctrlr_entry->ctrlr, &detach_ctx);
		free(ctrlr_entry);
	}

	if (detach_ctx) {
		spdk_nvme_detach_poll(detach_ctx);
	}
}

void nvme_exit(void)
{
	int i, j;

	if (g_io_threads) {
		for (i = 0; i < g_num_qpair; i++) {
			for (j = 0; j < seq_max_per_thread; j++) {
				spdk_free(g_io_threads[i].seqs[j].spdk_buf);
			}
			if (g_io_threads[i].seqs_busy)
				free(g_io_threads[i].seqs_busy);
			if (g_io_threads[i].bh_seqs.spdk_buf)
				spdk_free(g_io_threads[i].bh_seqs.spdk_buf);
			if (g_io_threads[i].desc_blk_buf)
				spdk_free(g_io_threads[i].desc_blk_buf);
			if (g_io_threads[i].commit_buf)
				spdk_free(g_io_threads[i].commit_buf);

			free(g_io_threads[i].seqs);

			spdk_nvme_ctrlr_free_io_qpair(g_io_threads[i].qpair);
		}
		free(g_io_threads);
	}

	cleanup();
	spdk_env_fini();
}

static void bio_wr_complete(void *arg, const struct spdk_nvme_cpl *completion)
{
	struct nvme_sequence *seq = arg;
	struct bio *bio = seq->bio;
	struct io_thread *worker = &g_io_threads[tls_tid];

	/* See if an error occurred. If so, display information
	 * about it, and set completion value so that I/O
	 * caller is aware that an error occurred.
	 */
	if (spdk_unlikely(spdk_nvme_cpl_is_error(completion))) {
		spdk_nvme_qpair_print_completion(
			worker->qpair, (struct spdk_nvme_cpl *)completion);
		fprintf(stderr, "I/O error status: %s\n",
			spdk_nvme_cpl_get_status_string(&completion->status));
		fprintf(stderr, "Write I/O failed, aborting run\n");
		exit(1); // TODO: Handle error.
		// TODO: We can pass bio->is_complete and set it to 2 when error occurs.
	}

	d_debug("bio(%p) left_bi %d seq %d total %d", bio, bio->bi_vcnt,
		seq->nr, bio->bi_vtotal);

	if (bio->bi_vcnt < seq->nr) {
		oxb_error("wrong bi_vcnt(%lu) seq(%lu)", bio->bi_vcnt, seq->nr);
		if (bio->bi_vcnt == 0) {
			nvme_seqs_release_one_by_ptr(seq);
			return;
		}
		/*
		 * Even on error, honour end_io so that upper layers do not
		 * hang waiting for completion.  Treat this as "all done"
		 * for this BIO.
		 */
		bio->bi_vcnt = 0;
		if (bio->end_io)
			bio->end_io(bio);
		nvme_seqs_release_one_by_ptr(seq);
		return;
	}

	bio->bi_vcnt -= seq->nr;
	if (bio->bi_vcnt == 0) {
		if (bio->end_io)
			bio->end_io(bio);
	}

	// Release the sequence slot now that the write is complete
	nvme_seqs_release_one_by_ptr(seq);
}

static int32_t nvme_process_cmpls(uint32_t inflight)
{
	struct io_thread *worker;
	int32_t processed = 0;

	worker = &g_io_threads[tls_tid];

	// polling for remaining
	if (inflight > 0) {
		processed =
			spdk_nvme_qpair_process_completions(worker->qpair, 0);

		if (processed < 0) {
			oxb_error("process completions error %d", processed);
			panic("Process completions error.");
			return processed;

		} else if ((uint32_t)processed > inflight) {
			oxb_error("overflow: Processed %d, Inflight %d",
				  processed, inflight);
			panic("Completions more than submitted.");
			return processed;
		}

		nvme_debug("[%s] %d requests processed.", __func__, processed);
	}

	return processed;
}

static uint32_t __nvme_wr_submit_bio(void)
{
	struct bio *bio = NULL;
	struct ns_entry *ns_entry;
	struct io_thread *worker;
	size_t bytes;
	uint32_t bv_cnt, bio_blk_cnt, req_tot;
	int rc, acq_rc;
	uint32_t cur_idx;
	struct nvme_sequence *cur_seq = NULL;

	rc = ring_buffer_mpmc_try_dequeue(&g_rd_bio_ring_buf, (void **)&bio);
	if (rc == -EAGAIN) {
		// Dequeue nothing. Empty.
	} else if (rc) {
		// Bio can be handled by another thread. If so, we just return
		// to save CPU cycles.
		panic("Dequeue failed.");
	} else {
		// Dequeue success.
		// oxb_warn("Dequeue success. bio=%p", bio);
	}

	worker = &g_io_threads[tls_tid];
	ns_entry = worker->ns_entry;
	req_tot = 0;
	nvme_debug("[%s] bio(%p) total:%d", __func__, bio, bio->bi_vtotal);

	while (bio) {
		// if bio contains bigger than spdk_max_io_size
		// send command with n times (n sequences required)
		// bio is contiguous on disk area
		bio_blk_cnt = 0;
	next_seq:
		// Acquire one sequence slot (may need to poll completions if full)
		do {
			acq_rc = nvme_seqs_try_acquire_one(&cur_idx);
			if (acq_rc == -EAGAIN)
				spdk_nvme_qpair_process_completions(
					worker->qpair, 0);
			else if (acq_rc) {
				panic("Failed to submit bio due to seq allocation failure.");
				// goto poll_remains;
			}
		} while (acq_rc == -EAGAIN);

		// oxb_info("Acquire sequence slot. cur_idx=%d", cur_idx);

		cur_seq = nvme_seqs_get_slot(cur_idx);
		bv_cnt = bytes = 0;
		cur_seq->bio = bio;
		cur_seq->bi_start = bio->bi_start + bio_blk_cnt;

		while (bytes < bio->bi_vtotal * PAGE_SIZE) {
			// copy bvec to spdk_buffer
			nvme_debug(
				"bytes(%lx) spdk_buf(0x%lx) bv_cnt%d buf 0x%lx",
				bytes, cur_seq->spdk_buf, bv_cnt,
				bio->bi_io_vec[bv_cnt].bv_buf);

			memcpy(cur_seq->spdk_buf + bytes,
			       bio->bi_io_vec[bv_cnt].bv_buf, PAGE_SIZE);
			bytes += PAGE_SIZE;
			bv_cnt++;
			bio_blk_cnt++;

			// OPTIMIZE: Issue the first io without batching for
			// better latency and pipelining. Or, reduce the size
			// of spdk_buf.

			// issue io command when gets max size
			if (bytes == g_max_nvme_max_io_size) {
				cur_seq->nr = bv_cnt;
				oxb_info("bio(%p) full send %d", bio, bv_cnt);
				rc = spdk_nvme_ns_cmd_write(
					ns_entry->ns, worker->qpair,
					cur_seq->spdk_buf,
					lba_to_sector(cur_seq->bi_start),
					bytes_to_sector(bytes), bio_wr_complete,
					cur_seq, 0);

				oxb_info("[%s] Submit to SPDK seqs[%d] write %d bytes(%d sectors) bi_start=%lu spdk_buf=%p",
					   __func__, cur_idx, bytes,
					   bytes_to_sector(bytes),
					   cur_seq->bi_start,
					   cur_seq->spdk_buf);

				// bio->opf -- io_flag
				if (rc) {
					oxb_error("spdk_nvme_ns_cmd_write error %d", rc);
					panic("spdk_nvme_ns_cmd_write error.");
				} else
					req_tot++;

				goto next_seq;
			}
		}

		if (bytes == 0)
			goto poll_remains;

		oxb_info("bio(%p) send %d", bio, bv_cnt);
		cur_seq->nr = bv_cnt;
		rc = spdk_nvme_ns_cmd_write(
			ns_entry->ns, worker->qpair, cur_seq->spdk_buf,
			lba_to_sector(cur_seq->bi_start),
			bytes_to_sector(bytes), bio_wr_complete,
			cur_seq, 0);

		oxb_info("[%s] Submit to SPDK seqs[%d] write %d bytes(%d sectors) bi_start=%lu spdk_buf=%p",
				__func__, cur_idx, bytes,
				bytes_to_sector(bytes),
				cur_seq->bi_start,
				cur_seq->spdk_buf);

		if (rc) {
			oxb_error("spdk_nvme_ns_cmd_write error %d", rc);
			panic("spdk_nvme_ns_cmd_write error.");
		} else
			req_tot++;

		bio = NULL;
	}

poll_remains:
	return req_tot;
}

typedef uint32_t (*nvme_submit_bio_fn)(void);
static uint32_t nvme_submit_bio(nvme_submit_bio_fn submit_fn,
				uint32_t submit_budget)
{
	int64_t inflight = 0;
	int64_t target_qd;
	int ret = 0;
	uint64_t req_tot = 0; // Statistics (may be used for future logging)
	uint64_t submitted_tot = 0;
	int budget_reached = 0;

	/* Clamp target_qd to a sane per-thread capacity. */
	{
		uint32_t cap = nvme_seqs_capacity_current();

		if (nvme_target_qd <= 0)
			target_qd = (int64_t)cap;
		else if ((uint32_t)nvme_target_qd > cap)
			target_qd = (int64_t)cap;
		else
			target_qd = (int64_t)nvme_target_qd;

		if (target_qd <= 0)
			/* Nothing to submit on this worker. */
			return 0;
	}

	PF_TL_START(e002c_nvme_poll_loop);

	if (nvme_no_async_submit_during_poll) {
		/*
		 * Non-pipelined mode:
		 *   - Submit one batch via submit_fn().
		 *   - Then poll until all of that batch completes.
		 *   - Repeat until submit_fn() reports no more work.
		 *
		 * This avoids interleaving new submissions while we are
		 * polling completions, making the IO pattern easier to
		 * reason about for debugging and profiling.
		 */
		for (;;) {
			int64_t submitted;

			if (!budget_reached) {
				submitted = (int64_t)submit_fn();
				if (submitted <= 0) {
					if (inflight == 0)
						break;
				} else {
					submitted_tot += (uint64_t)submitted;
					if (submit_budget &&
					    submitted_tot >= submit_budget)
						budget_reached = 1;
					inflight = submitted;
				}
			} else if (inflight == 0) {
				break;
			}

#ifdef OXBOW_TRACK_TPUT
			check_rt_q(nvme_inflight_qstat_for_current_worker(),
				   (uint32_t)inflight, 0);
#endif

#ifdef OXBOW_NOOP_BIO_NVME_READ
			/* In no-op mode, treat all inflight as immediately done. */
			req_tot += (uint64_t)inflight;
			inflight = 0;
#else
			while (inflight > 0) {
				PF_TL_START(e002d_nvme_poll_once);
				ret = nvme_process_cmpls((uint32_t)inflight);
				PF_TL_END(e002d_nvme_poll_once);

				if (ret < 0) {
					oxb_error(
						"Completions more than submitted. %ld",
						inflight);
					inflight = 0;
					break;
				}

				req_tot += (uint64_t)ret;
				inflight -= (int64_t)ret;
#ifdef OXBOW_TRACK_TPUT
				check_rt_q(
					nvme_inflight_qstat_for_current_worker(),
					(uint32_t)inflight, 0);
#endif
			}
#endif /* !OXBOW_NOOP_BIO_NVME_READ */

			if (budget_reached && inflight == 0)
				break;
		}
	} else {
		/*
		 * Original pipelined mode:
		 *   - Keep topping up inflight IOs up to target_qd
		 *     while periodically polling completions.
		 */
		for (;;) {
			int64_t submitted;
			int can_submit;

			can_submit = !budget_reached;

			/* 1) If nothing is in flight, try to submit at least one request. */
			if (inflight == 0) {
				if (!can_submit)
					break;
				submitted = (int64_t)submit_fn();
				if (submitted <= 0) {
					/* No more work in the ring and no inflight I/O. */
					break;
				}
				inflight += submitted;
				submitted_tot += (uint64_t)submitted;
				if (submit_budget && submitted_tot >= submit_budget)
					budget_reached = 1;
#ifdef OXBOW_TRACK_TPUT
				check_rt_q(
					nvme_inflight_qstat_for_current_worker(),
					(uint32_t)inflight, 0);
#endif
			}

			/* 2) Top up inflight I/Os up to target_qd, draining the ring. */
			while (!budget_reached && inflight < target_qd) {
				submitted = (int64_t)submit_fn();
				if (submitted <= 0)
					break;
				inflight += submitted;
				submitted_tot += (uint64_t)submitted;
				if (submit_budget && submitted_tot >= submit_budget)
					budget_reached = 1;
#ifdef OXBOW_TRACK_TPUT
				check_rt_q(
					nvme_inflight_qstat_for_current_worker(),
					(uint32_t)inflight, 0);
#endif
			}

			/* 3) Poll for completions once. This keeps latency low even
			 * when workload cannot fully saturate target_qd.
			 */
			PF_TL_START(e002d_nvme_poll_once);
#ifdef OXBOW_NOOP_BIO_NVME_READ
			ret = (int)inflight;
#else
			ret = nvme_process_cmpls((uint32_t)inflight);
#endif
			PF_TL_END(e002d_nvme_poll_once);

			if (ret < 0) {
				oxb_error(
					"Completions more than submitted. %ld",
					inflight);
				inflight = 0;
				break;
			}

			req_tot += (uint64_t)ret;
			inflight -= (int64_t)ret;
#ifdef OXBOW_TRACK_TPUT
			check_rt_q(nvme_inflight_qstat_for_current_worker(),
				   (uint32_t)inflight, 0);
#endif

			/* Loop will try to submit more again if ring is non-empty. */
			if (budget_reached && inflight == 0)
				break;
		}
	}

	PF_TL_END(e002c_nvme_poll_loop);

	// oxb_info("[%s] total request %lu completed", __func__, req_tot);
	return (uint32_t)req_tot;
}

#ifndef OXBOW_NOOP_RD_SUBMIT_BIO
/* Forward declaration; defined alongside the ring-driven entry below. */
static uint32_t __nvme_rd_submit_bio_one(struct bio *bio);

/*
 * Inline read submission used by D-2 read_workers.
 *
 * Drains a caller-provided list of read BIOs through the current
 * worker's SPDK qpair, polling completions until every BIO is fully
 * submitted and inflight reaches zero. This bypasses
 * g_rd_bio_ring_buf and the pump scheduler entirely; instead, the
 * read_worker that produced the BIOs (via mpage_readahead /
 * mpage_readpage) submits and polls them on its own qpair in the
 * same thread context.
 *
 * Caller invariants:
 *  - tls_tid is set and identifies a worker with a valid qpair
 *    (i.e., this thread runs as an iod_worker or a D-2 read_worker
 *    bound to one of the trailing tids reserved by nvme_init()).
 *  - The thread acquired its slot in tls_inline_batch[] via
 *    iod_submit_bio(REQ_OP_READ) under tls_inline_batch_active=1, so
 *    `bios` are the BIOs for one inode-batch produced by the read
 *    side and are ready to dispatch.
 *  - Sequence buffers (per-tid) are large enough to absorb the
 *    batch; if not, this function polls completions to free slots
 *    and keeps going (uses worker->pending_rd_bio internally for
 *    partial submission, just like the ring path).
 */
void nvme_submit_bio_inline(struct bio **bios, int n)
{
	int idx = 0;
	int64_t inflight = 0;
	int64_t target_qd;
	int ret = 0;
	uint64_t req_tot = 0;
	struct io_thread *worker;

	if (n <= 0 || !bios)
		return;

	worker = &g_io_threads[tls_tid];

	/* Clamp target_qd to per-thread sequence pool capacity, mirroring
	 * nvme_submit_bio()'s policy. */
	{
		uint32_t cap = nvme_seqs_capacity_current();

		if (nvme_target_qd <= 0)
			target_qd = (int64_t)cap;
		else if ((uint32_t)nvme_target_qd > cap)
			target_qd = (int64_t)cap;
		else
			target_qd = (int64_t)nvme_target_qd;

		if (target_qd <= 0)
			target_qd = 1;
	}

	PF_TL_START(e002c_nvme_poll_loop);

	for (;;) {
		uint32_t submitted;

		/* Top up: drain pending first, then walk the input list,
		 * keeping inflight close to target_qd. */
		while (inflight < target_qd) {
			if (worker->pending_rd_bio) {
				submitted = __nvme_rd_submit_bio_one(NULL);
			} else if (idx < n) {
				submitted = __nvme_rd_submit_bio_one(
					bios[idx]);
				/* If submission consumed the whole BIO,
				 * pending was cleared; advance idx. If
				 * partial (seq pool full mid-BIO), pending
				 * stays set and we loop back to drain it
				 * after polling. */
				if (worker->pending_rd_bio == NULL)
					idx++;
			} else {
				break;
			}

			if (submitted == 0) {
				/* Seq pool full; need to poll completions
				 * before submitting more. */
				break;
			}

			inflight += (int64_t)submitted;
#ifdef OXBOW_TRACK_TPUT
			check_rt_q(
				nvme_inflight_qstat_for_current_worker(),
				(uint32_t)inflight, 0);
#endif
		}

		/* Done? */
		if (inflight == 0 && idx == n &&
		    worker->pending_rd_bio == NULL)
			break;

#ifdef OXBOW_NOOP_BIO_NVME_READ
		/* In no-op mode, every submission already fired end_io
		 * inside __nvme_rd_submit_bio_one(). Treat all nominal
		 * "inflight" as immediately drained. */
		req_tot += (uint64_t)inflight;
		inflight = 0;
#else
		/* Drain at least one completion before looping. */
		while (inflight > 0) {
			PF_TL_START(e002d_nvme_poll_once);
			ret = nvme_process_cmpls((uint32_t)inflight);
			PF_TL_END(e002d_nvme_poll_once);

			if (ret < 0) {
				oxb_error(
					"Completions more than submitted. %ld",
					inflight);
				inflight = 0;
				break;
			}

			req_tot += (uint64_t)ret;
			inflight -= (int64_t)ret;
#ifdef OXBOW_TRACK_TPUT
			check_rt_q(
				nvme_inflight_qstat_for_current_worker(),
				(uint32_t)inflight, 0);
#endif

			/* Free slots are likely available now. Break
			 * out so we can submit more before continuing
			 * to poll. */
			if (inflight < target_qd && idx < n)
				break;
			if (inflight < target_qd && worker->pending_rd_bio)
				break;
		}
#endif /* !OXBOW_NOOP_BIO_NVME_READ */
	}

	PF_TL_END(e002c_nvme_poll_loop);
	(void)req_tot;
}
#else /* OXBOW_NOOP_RD_SUBMIT_BIO */
void nvme_submit_bio_inline(struct bio **bios, int n)
{
	int i;

	for (i = 0; i < n; i++) {
		struct bio *bio = bios[i];

		if (bio && bio->end_io)
			bio->end_io(bio);
	}
}
#endif /* !OXBOW_NOOP_RD_SUBMIT_BIO */

#if 0

/* NOTE: Submission and completion pipelining is only adopted to read IOs.

	First, writes occurs in fsync path which needs synchronous I/O. That is,
	all the I/O requests should be completed before the next fsync is
	processed.

	Second, nvme_wr_submit_bio is only used for experimental purpose
	(sync_device.c and sync without journaling).

	If you still need to apply pipelining to nvme_wr_submit_bio(), you must
	employ separate read/write bio ring buffers.
 */


void nvme_wr_submit_bio(void *arg)
{
	UNUSED1(arg);
	nvme_submit_bio(__nvme_wr_submit_bio, 0);
}
#else
void nvme_wr_submit_bio(void *arg)
{
	struct bio *bio = arg;
	struct ns_entry *ns_entry;
	struct io_thread *worker;
	size_t bytes;
	uint32_t bv_cnt, bio_blk_cnt, req_done, req_tot;
	int rc;
	uint32_t cur_idx;
	int acq_rc;
	struct nvme_sequence *cur_seq = NULL;

	worker = &g_io_threads[tls_tid];
	ns_entry = worker->ns_entry;
	req_tot = req_done = 0;
	nvme_debug("[%s] bio(%p) total:%d", __func__, bio, bio->bi_vtotal);

	while (bio) {
		// if bio contains bigger than spdk_max_io_size
		// send command with n times (n sequences required)
		// bio is contiguous on disk area
		bio_blk_cnt = 0;
	next_seq:
		// Acquire a sequence slot for this chunk
		do {
			acq_rc = nvme_seqs_try_acquire_one(&cur_idx);
			if (acq_rc == -EAGAIN)
				spdk_nvme_qpair_process_completions(
					worker->qpair, 0);
			else if (acq_rc) {
				panic("Failed to submit bio due to seq allocation failure.");
				// goto poll;
			}
		} while (acq_rc == -EAGAIN);

		cur_seq = nvme_seqs_get_slot(cur_idx);

		bv_cnt = bytes = 0;
		cur_seq->bio = bio;
		cur_seq->bi_start = bio->bi_start + bio_blk_cnt;

		while (bio_blk_cnt < bio->bi_vtotal &&
		       bytes + PAGE_SIZE <= g_max_nvme_max_io_size) {
			// copy bvec to spdk_buffer
			nvme_debug(
				"bytes(%lx) spdk_buf(0x%lx) bv_cnt%d buf 0x%lx",
				bytes, cur_seq->spdk_buf, bv_cnt,
				bio->bi_io_vec[bio_blk_cnt].bv_buf);

			memcpy(cur_seq->spdk_buf + bytes,
			       bio->bi_io_vec[bio_blk_cnt].bv_buf, PAGE_SIZE);
			bytes += PAGE_SIZE;
			bv_cnt++;
			bio_blk_cnt++;

			// issue io command when gets max size
			if (bytes == g_max_nvme_max_io_size &&
			    bio_blk_cnt < bio->bi_vtotal) {
				cur_seq->nr = bv_cnt;
				// nvme_debug("bio(%p) full send %d", bio, bv_cnt);
				rc = spdk_nvme_ns_cmd_write(
					ns_entry->ns, worker->qpair,
					cur_seq->spdk_buf,
					lba_to_sector(cur_seq->bi_start),
					bytes_to_sector(bytes), bio_wr_complete,
					cur_seq, 0);

				oxb_debug(
					"[%s] Submit to SPDK seqs[%d] write %d bytes(%d sectors) bi_start=%lu spdk_buf=%p",
					__func__, cur_idx, bytes,
					bytes_to_sector(bytes),
					cur_seq->bi_start, cur_seq->spdk_buf);

				if (rc) {
					oxb_error(
						"spdk_nvme_ns_cmd_write error %d",
						rc);
					panic("spdk_nvme_ns_cmd_write error.");
				} else {
					req_tot++;
				}
				goto next_seq;
			}
		}

		if (bytes == 0)
			goto poll;

		// nvme_debug("bio(%p) send %d", bio, bv_cnt);
		cur_seq->nr = bv_cnt;
		rc = spdk_nvme_ns_cmd_write(
			ns_entry->ns, worker->qpair, cur_seq->spdk_buf,
			lba_to_sector(cur_seq->bi_start),
			bytes_to_sector(bytes), bio_wr_complete,
			cur_seq, 0);
		if (rc) {
			oxb_error("spdk_nvme_ns_cmd_write error %d", rc);
			panic("spdk_nvme_ns_cmd_write error.");
		} else {
			req_tot++;
		}

		nvme_debug("[%s] Submit to SPDK seqs[%d] write %d bytes(%d sectors) bi_start=%lu spdk_buf=%p",
				__func__, cur_idx, bytes,
				bytes_to_sector(bytes),
				cur_seq->bi_start,
				cur_seq->spdk_buf);

		// bio->opf -- io_flag
		// if (!rc)
		// TODO: error handling
		req_done +=
			spdk_nvme_qpair_process_completions(worker->qpair, 0);

		// bio = bio->bi_next;
		bio = NULL;
	}

poll:
	// polling for remaining
	while (req_done < req_tot) {
		req_done +=
			spdk_nvme_qpair_process_completions(worker->qpair, 0);

		if (req_done > req_tot) {
			oxb_error("overflow");
			return;
		}
	}
	nvme_debug("[%s] total request %d done", __func__, req_done);
}
#endif

// static void __read_complete(struct nvme_sequence *seq)
// {
// 	uint32_t idx_in_bio;
// 	struct bio *bio = seq->bio;

// 	idx_in_bio = seq->bi_start - bio->bi_start;

// 	nvme_debug("[%s] %p, lba: %lu bio_lba: %lu qpair(%d)", __func__,
// 		   bio->bi_io_vec->bv_buf, seq->bi_start, bio->bi_start,
// 		   tls_tid);
// 	nvme_debug("[%s] read done bio_bvec lef4 %d, seq done %d", __func__,
// 		   bio->bi_vcnt, seq->nr);

// 	memcpy(bio->bi_io_vec->bv_buf + PAGE_SIZE * idx_in_bio, seq->spdk_buf,
// 	       PAGE_SIZE * seq->nr);

// 	if (bio->bi_vcnt < seq->nr)
// 		oxb_error("inconsistent %d/%d", bio->bi_vcnt, seq->nr);

// 	bio->bi_vcnt -= seq->nr;
// 	if (bio->bi_vcnt == 0) {
// 		if (bio->end_io)
// 			bio->end_io(bio);
// 	}
// 	nvme_debug("[%s] done", __func__);
// }

static void __read_complete(void *arg)
{
	struct nvme_sequence *seq = arg;
	uint32_t idx_in_bio;
	struct bio *bio = seq->bio;
	struct inode *inode = NULL;
	size_t pg0;
	uint32_t i;
	int skip_copy = 0;
	int is_user_ra = 0;

	idx_in_bio = seq->bi_start - bio->bi_start;

	/*
	 * Trace NVMe read completion at the sequence level.
	 * Downgraded to debug-level so that normal performance runs are not
	 * dominated by per-sequence logging; can be re-enabled via
	 * PRINT_NVME_DEBUG when deep tracing is needed.
	 */
	nvme_debug("[NVME_RD_DONE] qpair=%d bio=%p seq_lba=%lu bio_lba=%lu idx_in_bio=%u vcnt=%u vtotal=%u",
		   tls_tid, bio,
		   (unsigned long)seq->bi_start,
		   (unsigned long)bio->bi_start,
		   idx_in_bio,
		   (unsigned int)bio->bi_vcnt,
		   (unsigned int)bio->bi_vtotal);
	nvme_debug("[%s] read done bio_bvec lef4 %d, seq done %d", __func__,
		   bio->bi_vcnt, seq->nr);

	/* Decide whether this completion belongs to user-level readahead.
	 * User-level RA BIOs are identified via BIO_FLAG_USER_RA, which is
	 * set by mpage_user_readahead() for its speculative BIOs.
	 */
	if (bio && (bio->flags & BIO_FLAG_USER_RA))
		is_user_ra = 1;

	/* Attribute NVMe read pages to user-level readahead vs other sources. */
	if (is_user_ra)
		atomic_fetch_add_explicit(&g_nvme_user_ra_pages,
					  (unsigned long long)seq->nr,
					  memory_order_relaxed);
	else
		atomic_fetch_add_explicit(&g_nvme_other_pages,
					  (unsigned long long)seq->nr,
					  memory_order_relaxed);

	/* For user-level readahead BIOs, we no longer copy directly into
	 * SHM-backed pages here.  Instead, the data is cached in a per-inode
	 * user RA cache and will be copied into SHM only when the kernel
	 * actually issues READPAGE/RA for those pages.
	 */
	PF_TL_START(e002rb_nvme_copy_from_spdk);
#ifdef OXBOW_NOOP_NVME_READ_COPY
	skip_copy = 1;
#elif defined(OXBOW_NOOP_USER_RA_IO)
	if (is_user_ra)
		skip_copy = 1;
#endif

	if (!skip_copy && !is_user_ra) {
		memcpy(bio->bi_io_vec->bv_buf + PAGE_SIZE * idx_in_bio,
		       seq->spdk_buf, PAGE_SIZE * seq->nr);
	}
	PF_TL_END(e002rb_nvme_copy_from_spdk);

#ifdef OXBOW_TRACK_TPUT
	/* Track how much data has been completed back to upper layer. */
	check_rt_bw(&g_bw_nvme_complete,
		    (uint64_t)seq->nr * (uint64_t)PAGE_SIZE);
#endif

	if (bio->bi_vcnt < seq->nr)
		oxb_error("inconsistent %d/%d", bio->bi_vcnt, seq->nr);

	bio->bi_vcnt -= seq->nr;

	/* After data has been (optionally) copied into SHM-backed pages,
	 * mark the corresponding page indices as uptodate in the shared
	 * metadata so that future READPAGE/RA paths can skip real I/O.
	 *
	 * Only BIOs that originate from the filesystem layer (mpage /
	 * READPAGE / user-level readahead) carry a valid inode +
	 * daemon_fd pair.  Raw/test BIOs submitted via iod_submit_bio()
	 * (e.g., nvme_bw_test, iod_test) repurpose bi_private for their
	 * own per-thread context and leave daemon_fd at -1.  Treating
	 * those as struct inode would corrupt memory and can cause a
	 * segfault here.
	 */
	if (bio->daemon_fd >= 0)
		inode = (struct inode *)bio->bi_private;
	else
		inode = NULL;
	if (inode) {
		pg0 = (size_t)bio->page_index + (size_t)idx_in_bio;
		if (is_user_ra) {
			/* Cache user-level readahead pages in the per-inode
			 * RA cache. They will be copied into SHM and marked
			 * uptodate later when the kernel actually requests
			 * these pages via READPAGE/RA.
			 */
			for (i = 0; i < seq->nr; i++) {
				inode_ra_cache_insert(
					inode,
					(pgoff_t)(pg0 + (size_t)i),
					(const char *)seq->spdk_buf +
						(size_t)i * (size_t)PAGE_SIZE);
			}
		} else {
			for (i = 0; i < seq->nr; i++)
				mark_uptodate_in_shm(inode, pg0 + (size_t)i);
		}
	}
	if (bio->bi_vcnt == 0) {
		if (bio->end_io) {
			PF_TL_START(e002rc_nvme_end_io);
			bio->end_io(bio);
			PF_TL_END(e002rc_nvme_end_io);
		}
	}
	nvme_debug("[%s] done", __func__);
}

// static void ____read_complete(void *arg)
// {
// 	struct nvme_sequence *seq = arg;
// 	uint32_t idx_in_bio;
// 	struct bio *bio = seq->bio;

// 	idx_in_bio = seq->bi_start - bio->bi_start;

// 	nvme_debug("[%s] %p, lba: %lu bio_lba: %lu qpair(%d)", __func__,
// 		   bio->bi_io_vec->bv_buf, seq->bi_start, bio->bi_start,
// 		   tls_tid);
// 	nvme_debug("[%s] read done bio_bvec lef4 %d, seq done %d", __func__,
// 		   bio->bi_vcnt, seq->nr);

// 	memcpy(bio->bi_io_vec->bv_buf + PAGE_SIZE * idx_in_bio, seq->spdk_buf,
// 	       PAGE_SIZE * seq->nr);

// 	if (bio->bi_vcnt < seq->nr)
// 		oxb_error("inconsistent %d/%d", bio->bi_vcnt, seq->nr);

// 	bio->bi_vcnt -= seq->nr;
// 	if (bio->bi_vcnt == 0) {
// 		if (bio->end_io)
// 			bio->end_io(bio);
// 	}
// 	spdk_free(seq->spdk_buf);
// 	free(seq);
// 	nvme_debug("[%s] done", __func__);
// }

static void read_complete(void *arg, const struct spdk_nvme_cpl *completion)
{
	struct nvme_sequence *seq = arg;
	struct io_thread *worker = &g_io_threads[tls_tid];

	PF_TL_START(e002r_read_complete);

	nvme_debug("[%s] qpair(%d)", __func__, tls_tid);
	/* See if an error occurred. If so, display information
	 * about it, and set completion value so that I/O
	 * caller is aware that an error occurred.
	 */
	if (spdk_unlikely(spdk_nvme_cpl_is_error(completion))) {
		spdk_nvme_qpair_print_completion(
			worker->qpair, (struct spdk_nvme_cpl *)completion);
		fprintf(stderr, "I/O error status: %s\n",
			spdk_nvme_cpl_get_status_string(&completion->status));
		fprintf(stderr, "Read I/O failed, aborting run\n");
		// exit(1); // TODO: Handle error.
	}

	__read_complete(seq);

	// Release the sequence slot allocated for this read
	nvme_seqs_release_one_by_ptr(seq);

	PF_TL_END(e002r_read_complete);
}

static void read_complete_bh(void *arg, const struct spdk_nvme_cpl *completion)
{
	struct nvme_sequence *seq = arg;
	struct io_thread *worker = &g_io_threads[tls_tid];
	struct bio *bio = seq->bio;

	nvme_debug("[%s] qpair(%d)", __func__, tls_tid);
	/* See if an error occurred. If so, display information
	 * about it, and set completion value so that I/O
	 * caller is aware that an error occurred.
	 */
	if (spdk_unlikely(spdk_nvme_cpl_is_error(completion))) {
		spdk_nvme_qpair_print_completion(
			worker->qpair, (struct spdk_nvme_cpl *)completion);
		fprintf(stderr, "I/O error status: %s\n",
			spdk_nvme_cpl_get_status_string(&completion->status));
		fprintf(stderr, "Read I/O failed, aborting run\n");
		// exit(1); // TODO: Handle error.
	}

	/* For bh path, data is read directly into bio->bi_io_vec->bv_buf
	 * (which points to bh->b_data as SPDK DMA buffer), so no memcpy. */
	if (bio->bi_vcnt < seq->nr)
		oxb_error("inconsistent %d/%d", bio->bi_vcnt, seq->nr);

	bio->bi_vcnt -= seq->nr;
	if (bio->bi_vcnt == 0) {
		if (bio->end_io)
			bio->end_io(bio);
	}
}

static void write_complete_bh(void *arg, const struct spdk_nvme_cpl *completion)
{
	struct nvme_sequence *seq = arg;
	struct bio *bio = seq->bio;
	struct io_thread *worker = &g_io_threads[tls_tid];

	/* See if an error occurred. If so, display information
	 * about it, and set completion value so that I/O
	 * caller is aware that an error occurred.
	 */
	if (spdk_unlikely(spdk_nvme_cpl_is_error(completion))) {
		spdk_nvme_qpair_print_completion(
			worker->qpair, (struct spdk_nvme_cpl *)completion);
		fprintf(stderr, "I/O error status: %s\n",
			spdk_nvme_cpl_get_status_string(&completion->status));
		fprintf(stderr, "Write I/O failed, aborting run\n");
		exit(1); // TODO: Handle error.
		// TODO: We can pass bio->is_complete and set it to 2 when error occurs.
	}

	if (bio->bi_vcnt < seq->nr) {
		oxb_error("wrong bi_vcnt(%lu) seq(%lu)", bio->bi_vcnt, seq->nr);
		bio->bi_vcnt = 0;
		if (bio->end_io)
			bio->end_io(bio);
		return;
	}

	bio->bi_vcnt -= seq->nr;
	if (bio->bi_vcnt == 0) {
		if (bio->end_io)
			bio->end_io(bio);
	}
}

void nvme_rd_submit_bh(void *arg)
{
	struct bio *bio = arg;
	struct ns_entry *ns_entry;
	struct io_thread *worker;
	struct nvme_sequence *seqs;
	uint32_t req_done, req_tot;
	int rc;

	PF_TL_START(e001_nvme_rd_submit_bh);
	// d_debug("[%s]", __func__);

#ifdef SD_TID_VALIDATE
	uint32_t mytid = get_tid();
	char thread_name[16];
	get_thread_name(thread_name);

	if (!tls_ioworker) {
		oxb_info(
			"It is not io worker thread (tls_ioworker=0). tls_tid=%d thread_name=%s",
			tls_tid, thread_name);
	} else if (g_tid_validate_table[tls_tid] != mytid) {
		if (g_tid_validate_table[tls_tid] == 0) {
			g_tid_validate_table[tls_tid] = mytid;
		} else {
			oxb_error(
				"TID[%d] mismatch: prev_tid=%u ----> mytid=%u",
				tls_tid, g_tid_validate_table[tls_tid], mytid);
			g_tid_validate_table[tls_tid] = mytid;
		}
	};
#endif

	worker = &g_io_threads[tls_tid];
	ns_entry = worker->ns_entry;
	seqs = &worker->bh_seqs;
	seqs->bio = bio;
	seqs->bi_start = bio->bi_start;
	seqs->nr = 1;
	req_tot = req_done = 0;

	// if bio contains bigger than spdk_max_io_size
	// send command with n times (n sequences required)
	// bio is contiguous on disk area
	nvme_debug("[qp %d] bio start lba: %lu IO total: %u", tls_tid,
		   bio->bi_start, bio->bi_vtotal);

	rc = spdk_nvme_ns_cmd_read(ns_entry->ns, worker->qpair,
				   bio->bi_io_vec->bv_buf,
				   lba_to_sector(seqs->bi_start),
				   bytes_to_sector(PAGE_SIZE), read_complete_bh,
				   seqs, 0);
	if (rc) {
		oxb_error("spdk_nvme_ns_cmd_read(bh) error %d", rc);
		panic("spdk_nvme_ns_cmd_read(bh) error.");
		// goto ret;
	}

	// bio->opf -- io_flag
	// if (!rc)
	// TODO: error handling
	req_tot++;

	// polling for remaining
	while (req_done < req_tot) {
		req_done +=
			spdk_nvme_qpair_process_completions(worker->qpair, 0);

		if (req_done > req_tot) {
			oxb_error("overflow %lu/%lu", req_done, req_tot);
			goto ret;
		}
	}

	nvme_debug("[%s] lba:%lu total request %d done", __func__,
		   seqs->bi_start, req_done);

ret:
	PF_TL_END(e001_nvme_rd_submit_bh);

	return;
}

void nvme_wr_submit_bh(void *arg)
{
	struct bio *bio = arg;
	struct ns_entry *ns_entry;
	struct io_thread *worker;
	struct nvme_sequence *seqs;
	uint32_t req_done, req_tot;
	int rc;
	uint32_t idx_in_bio;

	// d_debug("[%s]", __func__);

#ifdef SD_TID_VALIDATE
	uint32_t mytid = get_tid();
	char thread_name[16];
	get_thread_name(thread_name);

	if (!tls_ioworker) {
		oxb_info(
			"It is not io worker thread (tls_ioworker=0). tls_tid=%d thread_name=%s",
			tls_tid, thread_name);
	} else if (g_tid_validate_table[tls_tid] != mytid) {
		if (g_tid_validate_table[tls_tid] == 0) {
			g_tid_validate_table[tls_tid] = mytid;
		} else {
			oxb_error(
				"TID[%d] mismatch: prev_tid=%u ----> mytid=%u",
				tls_tid, g_tid_validate_table[tls_tid], mytid);
			g_tid_validate_table[tls_tid] = mytid;
		}
	};
#endif

	worker = &g_io_threads[tls_tid];
	ns_entry = worker->ns_entry;
	seqs = &worker->bh_seqs;
	seqs->bio = bio;
	seqs->bi_start = bio->bi_start;
	seqs->nr = 1;
	req_tot = req_done = 0;

	idx_in_bio = seqs->bi_start - bio->bi_start;

	oxbow_assert(!idx_in_bio);

	rc = spdk_nvme_ns_cmd_write(ns_entry->ns, worker->qpair,
				    bio->bi_io_vec->bv_buf,
				    lba_to_sector(seqs->bi_start),
				    bytes_to_sector(PAGE_SIZE),
				    write_complete_bh, seqs, 0);
	if (rc) {
		oxb_error("spdk_nvme_ns_cmd_write(bh) error %d", rc);
		panic("spdk_nvme_ns_cmd_write(bh) error.");
		// goto ret;
	}

	// bio->opf -- io_flag
	// TODO: error handling
	req_tot++;

	// polling for remaining
	while (req_done < req_tot) {
		req_done +=
			spdk_nvme_qpair_process_completions(worker->qpair, 0);

		if (req_done > req_tot) {
			oxb_error("overflow %lu/%lu", req_done, req_tot);
			goto ret;
		}
	}
ret:

	return;
}

// #define OXBOW_NVME_IO_QUEUE_DEPTH 16
// #define OXBOW_NVME_BATCH_COMPLETE 4

// void nvme_rd_submit_bio(void *arg)
// {
// 	struct bio *bio = arg;
// 	struct ns_entry *ns_entry;
// 	struct io_thread *worker;
// 	struct nvme_sequence *seq;
// 	size_t bytes;
// 	uint32_t bv_cnt, bio_blk_cnt, req_done, req_tot, ret;
// 	int rc, n = 0;

// 	nvme_debug("[%s]", __func__);

// 	worker = &g_io_threads[tls_tid];
// 	ns_entry = worker->ns_entry;
// 	req_tot = req_done = 0;

// 	while (bio) {

// 		// log_debug("bio start lba: %lu IO total: %u", bio->bi_start,
// 		// 	  bio->bi_vtotal);
// 		bio_blk_cnt = 0;
// 	next_seq:
// 		if (n == seq_max_per_thread) {
// 			oxb_error("bio too big!");
// 			goto poll;
// 		}

// 		if (req_tot != 0 &&
// 		    (req_tot % OXBOW_NVME_IO_QUEUE_DEPTH == 0)) {
// 			ret = spdk_nvme_qpair_process_completions(worker->qpair,
// 								  0);
// 			if (ret < 0) {
// 				oxb_error("poll error %d", ret);
// 				goto poll;
// 			}
// 			req_done += ret;
// 		}

// 		bv_cnt = bytes = 0;

// 		seq = malloc(sizeof(*seq));
// 		seq->bio = bio;
// 		seq->bi_start = bio->bi_start + bio_blk_cnt;

// 		while (bytes < bio->bi_vtotal * PAGE_SIZE) {
// 			bytes += PAGE_SIZE;
// 			bv_cnt++;
// 			bio_blk_cnt++;

// 			// issue io command when gets max size
// 			if (bytes == g_max_nvme_max_io_size) {
// 				// worker->seqs[n].nr = bv_cnt;
// 				seq->spdk_buf =
// 					spdk_malloc(bytes, 0x1000, NULL,
// 						    SPDK_ENV_SOCKET_ID_ANY,
// 						    SPDK_MALLOC_DMA);
// 				seq->nr = bv_cnt;
// 				rc = spdk_nvme_ns_cmd_read(
// 					ns_entry->ns, worker->qpair,
// 					seq->spdk_buf,
// 					lba_to_sector(seq->bi_start),
// 					bytes_to_sector(bytes), read_complete,
// 					seq, 0);

// 				// nvme_debug("[%s] Submit to SPDK seqs[%d] read %d bytes(%d sectors) bi_start=%lu spdk_buf=%p",
// 				// 	   __func__, n, bytes,
// 				// 	   bytes_to_sector(bytes),
// 				// 	   worker->seqs[n].bi_start,
// 				// 	   worker->seqs[n].spdk_buf);

// 				// bio->opf -- io_flag
// 				// if (!rc)
// 				// TODO: error handling
// 				req_tot++;
// 				n++;
// 				goto next_seq;
// 			}
// 		}
// 		g_nvme_read_blknr += bio_blk_cnt;

// 		seq->spdk_buf =
// 			spdk_malloc(bytes, 0x1000, NULL, SPDK_ENV_SOCKET_ID_ANY,
// 				    SPDK_MALLOC_DMA);
// 		seq->nr = bv_cnt;
// 		rc = spdk_nvme_ns_cmd_read(ns_entry->ns, worker->qpair,
// 					   seq->spdk_buf,
// 					   lba_to_sector(seq->bi_start),
// 					   bytes_to_sector(bytes),
// 					   read_complete, seq, 0);
// 		// nvme_debug(
// 		// 	"[%s] Submit to SPDK seqs[%d] read %d bytes(%d sectors) bi_start=%lu spdk_buf=%p",
// 		// 	__func__, n, bytes, bytes_to_sector(bytes),
// 		// 	worker->seqs[n].bi_start, worker->seqs[n].spdk_buf);

// 		req_tot++;
// 		n++;
// 		// bio->opf -- io_flag
// 		// if (!rc)
// 		// TODO: error handling

// 		ret = spdk_nvme_qpair_process_completions(worker->qpair, 0);
// 		if (ret < 0) {
// 			oxb_error("poll error %d", ret);
// 			goto poll;
// 		}
// 		req_done += ret;
// 		bio = NULL;
// 	}

// poll:
// 	// polling for remaining
// 	while (req_done < req_tot) {
// 		req_done +=
// 			spdk_nvme_qpair_process_completions(worker->qpair, 0);

// 		if (req_done > req_tot) {
// 			oxb_error("overflow %lu/%lu", req_done, req_tot);
// 			return;
// 		}
// 	}

// 	nvme_debug("[%s] total request %d done\n", __func__, req_done);
// }

#ifndef OXBOW_NOOP_RD_SUBMIT_BIO
#ifdef OXBOW_NOOP_BIO_NVME_READ
/*
 * Common no-op completion for a single BIO in OXBOW_NOOP_BIO_NVME_READ
 * mode. Marks SHM pages uptodate (so the kernel side observes a normal
 * read completion) and invokes end_io. Used by both the ring-driven
 * pump entry and the inline submission path.
 */
static void __nvme_rd_complete_noop_one(struct bio *bio)
{
	struct inode *inode = NULL;
	uint32_t i;

	if (!bio)
		return;

	PF_TL_START(e002_nvme_rd_submit_bio);

	/* As in __read_complete(), only BIOs from the filesystem layer
	 * carry a valid inode + daemon_fd. Test/raw BIOs reuse bi_private
	 * for other purposes and must not be interpreted as struct inode.
	 */
	if (bio->daemon_fd >= 0)
		inode = (struct inode *)bio->bi_private;

	if (inode) {
		size_t pg0 = (size_t)bio->page_index;

		for (i = 0; i < bio->bi_vtotal; i++)
			mark_uptodate_in_shm(inode, pg0 + (size_t)i);
	}

	bio->bi_vcnt = 0;
	if (bio->end_io) {
		PF_TL_START(e002g_nvme_end_io);
		bio->end_io(bio);
		PF_TL_END(e002g_nvme_end_io);
	}

	PF_TL_END(e002_nvme_rd_submit_bio);
}
#endif /* OXBOW_NOOP_BIO_NVME_READ */

/*
 * Submit pages from `bio` to the current worker's SPDK qpair.
 *
 * Caller may pass:
 *   - bio == NULL: resume submission for worker->pending_rd_bio (must be
 *     non-NULL in that case).
 *   - bio != NULL when worker->pending_rd_bio == NULL: take ownership;
 *     the helper sets pending_rd_bio = bio and starts at offset 0.
 *   - bio == worker->pending_rd_bio: equivalent to passing NULL.
 *
 * Returns the number of NVMe commands submitted in this call. Stops
 * early (returning the partial count) when the per-tid sequence pool
 * is full; on the next call the worker resumes from
 * worker->pending_rd_pg_off.
 *
 * Used by both the ring-driven pump (`__nvme_rd_submit_bio`) and the
 * D-2 inline submission path (`nvme_submit_bio_inline`).
 */
static uint32_t __nvme_rd_submit_bio_one(struct bio *bio)
{
#ifdef OXBOW_NOOP_BIO_NVME_READ
	/* No-op mode: pretend the IO completed and fire end_io. */
	struct io_thread *worker = &g_io_threads[tls_tid];

	if (!bio) {
		bio = worker->pending_rd_bio;
		worker->pending_rd_bio = NULL;
		worker->pending_rd_pg_off = 0;
	}
	if (!bio)
		return 0;

	__nvme_rd_complete_noop_one(bio);
	return 1;
#else  /* !OXBOW_NOOP_BIO_NVME_READ */
	struct ns_entry *ns_entry;
	struct io_thread *worker;
	size_t bytes;
	uint32_t bv_cnt, bio_blk_cnt, req_tot;
	int rc;
	uint32_t cur_idx;
	int acq_rc;
	struct nvme_sequence *cur_seq = NULL;

	worker = &g_io_threads[tls_tid];
	ns_entry = worker->ns_entry;
	req_tot = 0;

	if (worker->pending_rd_bio) {
		/* Resuming a partially-submitted BIO. The caller's `bio`
		 * argument is either NULL (continue) or the same BIO
		 * (no-op). We never accept a different BIO while pending
		 * is non-NULL because that would lose work. */
		oxbow_assert(bio == NULL ||
			     bio == worker->pending_rd_bio);
		bio = worker->pending_rd_bio;
	} else {
		if (!bio)
			return 0;
		worker->pending_rd_bio = bio;
		worker->pending_rd_pg_off = 0;
	}

	bio_blk_cnt = worker->pending_rd_pg_off;

	PF_TL_START(e002_nvme_rd_submit_bio);

	nvme_debug("[%s] bio=%p start_lba=%lu total_pg=%u pending_off=%u",
		   __func__, bio, bio->bi_start, bio->bi_vtotal,
		   bio_blk_cnt);

	while (bio && bio_blk_cnt < bio->bi_vtotal) {
		/* Acquire one sequence slot for this segment.
		 * Do not poll completions here; let nvme_submit_bio()
		 * drive qpair completions to keep the design simple.
		 */
		PF_TL_START(e002a_nvme_seq_acquire);
		acq_rc = nvme_seqs_try_acquire_one(&cur_idx);
		PF_TL_END(e002a_nvme_seq_acquire);

		if (acq_rc == -EAGAIN) {
			/* No free seq slots for now. Stop submitting in this
			 * round and let the outer poll loop make progress.
			 */
			goto out;
		} else if (acq_rc) {
			panic("Failed to submit bio due to seq allocation failure.");
		}

		cur_seq = nvme_seqs_get_slot(cur_idx);
		bv_cnt = 0;
		bytes = 0;
		cur_seq->bio = bio;
		cur_seq->bi_start = bio->bi_start + bio_blk_cnt;

		/* Pack as many contiguous pages from this BIO as allowed by
		 * g_max_nvme_max_io_size into a single NVMe command.
		 */
		while (bio_blk_cnt < bio->bi_vtotal &&
		       bytes + PAGE_SIZE <= g_max_nvme_max_io_size) {
			bytes += PAGE_SIZE;
			bv_cnt++;
			bio_blk_cnt++;
		}

		if (bytes == 0) {
			/* Should not happen, but guard against infinite loop. */
			break;
		}

		cur_seq->nr = bv_cnt;

		PF_TL_START(e002b_nvme_spdk_submit);
#ifdef OXBOW_TRACK_TPUT
		/* Track bytes submitted in this NVMe read command. */
		check_rt_bw(&g_bw_nvme_submit, (uint64_t)bytes);
#endif
		/* Normal path (including OXBOW_NOOP_USER_RA_IO): submit to
		 * the device. In OXBOW_NOOP_USER_RA_IO mode, user-level RA
		 * skipping is handled in __read_complete() by suppressing
		 * memcpy/uptodate for BIO_FLAG_USER_RA, not here.
		 */
		rc = spdk_nvme_ns_cmd_read(ns_entry->ns, worker->qpair,
					   cur_seq->spdk_buf,
					   lba_to_sector(cur_seq->bi_start),
					   bytes_to_sector(bytes),
					   read_complete, cur_seq, 0);

		nvme_debug(
			"[%s] Submit to SPDK: lba=%lu bytes=%zu nr_blks=%u bio=%p",
			__func__, cur_seq->bi_start, bytes,
			bytes_to_nblks(bytes), bio);

		// bio->opf -- io_flag
		if (rc) {
			oxb_error("spdk_nvme_ns_cmd_read error %d", rc);
			panic("spdk_nvme_ns_cmd_read error.");
		} else {
			req_tot++;
		}
		PF_TL_END(e002b_nvme_spdk_submit);

		/* If we still have remaining pages in this BIO, remember the
		 * offset so that the next submit round can continue.
		 * Otherwise, clear pending state and move on.
		 */
		if (bio_blk_cnt < bio->bi_vtotal) {
			worker->pending_rd_pg_off = bio_blk_cnt;
		} else {
			worker->pending_rd_bio = NULL;
			worker->pending_rd_pg_off = 0;
			bio = NULL;
			break;
		}
	}

out:
	PF_TL_END(e002_nvme_rd_submit_bio);

	return req_tot;
#endif /* OXBOW_NOOP_BIO_NVME_READ */
}

/*
 * Ring-driven pump entry. Continues a pending BIO if one is parked on
 * the worker, otherwise tries to dequeue a fresh BIO from
 * g_rd_bio_ring_buf. Returns 0 when there's no work.
 */
static uint32_t __nvme_rd_submit_bio(void)
{
#ifdef OXBOW_NOOP_BIO_NVME_READ
	/* Global no-op NVMe read: dequeue then fire end_io. The shared
	 * helper handles SHM uptodate marks and end_io invocation. */
	struct bio *bio = NULL;
	int rc;

	rc = ring_buffer_mpmc_try_dequeue(&g_rd_bio_ring_buf, (void **)&bio);
	if (rc)
		return 0;
	__nvme_rd_complete_noop_one(bio);
	return 1;
#else
	struct io_thread *worker = &g_io_threads[tls_tid];
	struct bio *bio = NULL;
	int rc;

	if (worker->pending_rd_bio)
		return __nvme_rd_submit_bio_one(NULL);

	rc = ring_buffer_mpmc_try_dequeue(&g_rd_bio_ring_buf, (void **)&bio);
	if (rc)
		return 0;
	return __nvme_rd_submit_bio_one(bio);
#endif
}
#endif /* !OXBOW_NOOP_RD_SUBMIT_BIO */

void nvme_stop_rd_workers(void)
{
	atomic_store_explicit(&g_nvme_rd_workers_stop, 1, memory_order_relaxed);
	nvme_sample_qos_gauges();
}

void nvme_rd_submit_bio(void *arg)
{
	int scheduled_prev;
	int fsync_active;
	int submit_budget;
	size_t backlog;
	uint32_t work_done = 0;

	UNUSED1(arg);

	scheduled_prev = atomic_fetch_sub_explicit(&g_nvme_rd_pump_scheduled, 1,
						   memory_order_relaxed);
	if (scheduled_prev <= 0)
		atomic_store_explicit(&g_nvme_rd_pump_scheduled, 0,
				      memory_order_relaxed);
	nvme_sample_qos_gauges();

	if (atomic_load_explicit(&g_nvme_rd_workers_stop, memory_order_relaxed))
		return;

	atomic_fetch_add_explicit(&g_nvme_rd_pump_active, 1,
				  memory_order_relaxed);
	nvme_sample_qos_gauges();
	fsync_active = nvme_is_fsync_inflight();
	submit_budget = nvme_rd_budget_for_qos(fsync_active);

#ifdef OXBOW_NOOP_RD_SUBMIT_BIO
	/* No-op mode: drain a bounded number of BIOs. */
	{
		struct bio *bio = NULL;
		int rc;
		uint32_t left = (uint32_t)submit_budget;

		while (left--) {
			rc = ring_buffer_mpmc_try_dequeue(&g_rd_bio_ring_buf,
							  (void **)&bio);
			if (rc)
				break;

			if (bio && bio->end_io)
				bio->end_io(bio);

			bio = NULL;
			work_done++;
		}
	}
#else
	/* Normal mode: budgeted submit+poll. */
	work_done = nvme_submit_bio(__nvme_rd_submit_bio,
				    (uint32_t)submit_budget);
#endif

	atomic_fetch_sub_explicit(&g_nvme_rd_pump_active, 1,
				  memory_order_relaxed);
	nvme_sample_qos_gauges();

	if (atomic_load_explicit(&g_nvme_rd_workers_stop, memory_order_relaxed))
		return;

	/* Keep draining while backlog exists. Scheduler applies fsync-aware caps. */
	backlog = ring_buffer_mpmc_count(&g_rd_bio_ring_buf);
	if (backlog > 0 || work_done > 0)
		nvme_schedule_read_pump();
	else
		nvme_sample_qos_gauges();
}

void nvme_ra_stats_dump(void)
{
	unsigned long long user_pages;
	unsigned long long other_pages;
	unsigned long long total_pages;
	unsigned long long total_mib;

	user_pages = atomic_load_explicit(&g_nvme_user_ra_pages,
					  memory_order_relaxed);
	other_pages = atomic_load_explicit(&g_nvme_other_pages,
					   memory_order_relaxed);
	total_pages = user_pages + other_pages;
	total_mib = (total_pages * (unsigned long long)PAGE_SIZE) >> 20;

	log_info("\n==== NVME Read Split (User RA vs Other) ====\n"
		 "  user_ra_nvme_pages  : %llu\n"
		 "  other_nvme_pages    : %llu\n"
		 "  total_nvme_pages    : %llu\n"
		 "  total_nvme_IO(MiB)  : %llu\n",
		 user_pages,
		 other_pages,
		 total_pages,
		 total_mib);
}

void *nvme_get_seq_buffer(int seq_idx)
{
	if (&g_io_threads[tls_tid] == NULL) {
		oxb_error("io thread not initialized");
		return NULL;
	}

	return g_io_threads[tls_tid].seqs[seq_idx].spdk_buf;
}

void *nvme_get_desc_blk_buffer(void)
{
	// tls_tid validation.
	oxbow_assert(tls_tid >= 0 || tls_tid < g_num_qpair);

	// Check if desc_blk_buf is initialized.
	oxbow_assert(g_io_threads && g_io_threads[tls_tid].desc_blk_buf);
	
	return g_io_threads[tls_tid].desc_blk_buf;
}

void *nvme_get_commit_blk_buffer(void)
{
	// tls_tid validation.
	oxbow_assert(tls_tid >= 0 || tls_tid < g_num_qpair);

	// Check if commit_buf is initialized.
	oxbow_assert(g_io_threads && g_io_threads[tls_tid].commit_buf);
	
	return g_io_threads[tls_tid].commit_buf;
}

int nvme_get_seq_max(void)
{
	return seq_max_per_thread;
}

/* =========================
 * Sequence ring allocator (Read IO only)
 * ========================= */

static inline uint32_t __seqs_idx_advance(uint32_t idx, uint32_t add, uint32_t cap)
{
	uint32_t res = idx + add;
	return (res >= cap) ? (res - cap) : res;
}

static inline uint32_t __seqs_contig_free_from_head(struct io_thread *worker,
						    uint32_t needed)
{
	uint32_t cap = worker->seqs_capacity;
	uint32_t free_total = cap - worker->seqs_used;
	uint32_t i, contig = 0;

	if (free_total == 0)
		return 0;

	for (i = 0; i < free_total && contig < needed; i++) {
		uint32_t idx = __seqs_idx_advance(worker->seqs_head, i, cap);
		if (worker->seqs_busy[idx])
			break;
		contig++;
	}
	return contig;
}

/* Try to acquire a contiguous range of seq slots for the current worker thread.
 * Returns 0 on success and sets *out_start to the starting index.
 * Returns -EAGAIN if not enough contiguous slots, -EINVAL on bad arguments. */
int nvme_seqs_try_acquire(uint32_t needed, uint32_t *out_start)
{
	struct io_thread *worker = &g_io_threads[tls_tid];
	uint32_t cap;
	uint32_t i;

	if (needed == 0 || out_start == NULL)
		return -EINVAL;

	cap = worker->seqs_capacity;
	if (needed > cap) {
		oxb_error(
			"Too many sequences requested. needed > cap : %d > %d",
			needed, cap);
		return -EINVAL;
	}

	/* Profile how much time is spent on free-slot checks and the
	 * contiguous-free scan. This helps explain e002a_nvme_seq_acquire.
	 */
	PF_TL_START(e002a1_nvme_seq_check_free);
	if (cap - worker->seqs_used < needed) {
		PF_TL_END(e002a1_nvme_seq_check_free);
		return -EAGAIN;
	}

	if (needed == 1) {
		/* Fast path: for single-slot allocations we only need the
		 * current head slot to be free. Scanning the entire free
		 * region is unnecessary and was showing up as a hot spot.
		 */
		uint32_t idx = worker->seqs_head;

		if (worker->seqs_busy[idx]) {
			PF_TL_END(e002a1_nvme_seq_check_free);
			return -EAGAIN;
		}
	} else {
		/* Slow path: multi-slot callers (e.g., direct writes) still
		 * need to know whether there are enough contiguous free slots
		 * starting from head, but the scan now stops as soon as
		 * 'needed' slots are found.
		 */
		if (__seqs_contig_free_from_head(worker, needed) < needed) {
			PF_TL_END(e002a1_nvme_seq_check_free);
			return -EAGAIN;
		}
	}
	PF_TL_END(e002a1_nvme_seq_check_free);

	/* Profile the cost of actually marking slots busy and updating
	 * allocator state.
	 */
	PF_TL_START(e002a2_nvme_seq_mark_busy);
	*out_start = worker->seqs_head;
	for (i = 0; i < needed; i++) {
		uint32_t idx = __seqs_idx_advance(worker->seqs_head, i, cap);
		worker->seqs_busy[idx] = 1u;
	}
	worker->seqs_head = __seqs_idx_advance(worker->seqs_head, needed, cap);
	worker->seqs_used += needed;
	PF_TL_END(e002a2_nvme_seq_mark_busy);

#ifdef OXBOW_TRACK_TPUT
	/* Sample how many sequence slots are in use on this worker and
	 * attribute the cost separately from the allocator itself.
	 */
	PF_TL_START(e002a3_nvme_seq_qdepth_sample);
	check_rt_q(&g_q_nvme_seqs, worker->seqs_used, 0);
	PF_TL_END(e002a3_nvme_seq_qdepth_sample);
#endif
	return 0;
}

/* Convenience wrapper for acquiring a single slot. */
int nvme_seqs_try_acquire_one(uint32_t *out_idx)
{
	return nvme_seqs_try_acquire(1u, out_idx);
}

/* Release a previously acquired contiguous range starting at start of length count. */
void nvme_seqs_release(uint32_t start, uint32_t count)
{
	struct io_thread *worker = &g_io_threads[tls_tid];
	uint32_t cap = worker->seqs_capacity;
	uint32_t k;

	if (count == 0 || count > worker->seqs_used)
		return;

	for (k = 0; k < count; k++) {
		uint32_t idx = __seqs_idx_advance(start, k, cap);
		if (!worker->seqs_busy[idx]) {
			// double free or logic error; keep going but warn
			oxb_warn("nvme_seqs_release: slot %u already free", idx);
		} else {
			worker->seqs_busy[idx] = 0u;
		}
	}

	if (worker->seqs_used >= count)
		worker->seqs_used -= count;
	else
		worker->seqs_used = 0; // should not happen

	// Move tail forward over any newly freed leading slots
	while (worker->seqs_used > 0 && worker->seqs_busy[worker->seqs_tail] == 0u) {
		worker->seqs_tail = __seqs_idx_advance(worker->seqs_tail, 1u, cap);
	}
#ifdef OXBOW_TRACK_TPUT
	/* Sample depth after releasing slots as well. */
	check_rt_q(&g_q_nvme_seqs, worker->seqs_used, 0);
#endif
}

/* Introspection helpers */
uint32_t nvme_seqs_available(void)
{
	struct io_thread *worker = &g_io_threads[tls_tid];
	return worker->seqs_capacity - worker->seqs_used;
}

uint32_t nvme_seqs_capacity_current(void)
{
	struct io_thread *worker = &g_io_threads[tls_tid];
	return worker->seqs_capacity;
}

struct nvme_sequence *nvme_seqs_get_slot(uint32_t idx)
{
	struct io_thread *worker = &g_io_threads[tls_tid];
	if (idx >= worker->seqs_capacity)
		return NULL;
	return &worker->seqs[idx];
}

void nvme_seqs_reset(void)
{
	struct io_thread *worker = &g_io_threads[tls_tid];
	uint32_t i;
	worker->seqs_head = 0;
	worker->seqs_tail = 0;
	worker->seqs_used = 0;
	for (i = 0; i < worker->seqs_capacity; i++)
		worker->seqs_busy[i] = 0u;
}

/* Return index of a slot within the worker's seqs array, or -1 on error. */
int nvme_seqs_index_of(struct nvme_sequence *slot)
{
	struct io_thread *worker = &g_io_threads[tls_tid];
	ptrdiff_t diff = slot - &worker->seqs[0];
	if (diff < 0 || (uint32_t)diff >= worker->seqs_capacity)
		return -1;
	return (int)diff;
}

/* Release a single slot by pointer convenience. No-op if pointer invalid. */
void nvme_seqs_release_one_by_ptr(struct nvme_sequence *slot)
{
	int idx = nvme_seqs_index_of(slot);
	if (idx < 0)
		return;
	nvme_seqs_release((uint32_t)idx, 1u);
}

static void wr_complete(void *arg, const struct spdk_nvme_cpl *completion)
{
	struct nvme_sequence *seq = arg;
	struct io_thread *worker = &g_io_threads[tls_tid];
	int is_error = 0;

	/* See if an error occurred. If so, display information
	 * about it, and set completion value so that I/O
	 * caller is aware that an error occurred.
	 */
	if (spdk_unlikely(spdk_nvme_cpl_is_error(completion))) {
		is_error = 1;
		spdk_nvme_qpair_print_completion(
			worker->qpair, (struct spdk_nvme_cpl *)completion);
		fprintf(stderr, "I/O error status: %s\n",
			spdk_nvme_cpl_get_status_string(&completion->status));
	}

	if (seq && seq->wr_track_cb) {
		seq->wr_track_cb(seq->wr_track_ctx, seq->wr_track_slot_idx,
				 is_error);
		seq->wr_track_cb = NULL;
		seq->wr_track_ctx = NULL;
	}

	if (is_error) {
		fprintf(stderr, "Write I/O failed, aborting run\n");
		exit(1); // TODO: Handle error.
		// TODO: We can pass bio->is_complete and set it to 2 when error occurs.
	}
}

// No argument is passed.
static void wr_complete_with_buf(void *arg, const struct spdk_nvme_cpl *completion)
{
	struct io_thread *worker = &g_io_threads[tls_tid];
	UNUSED1(arg);

	/* See if an error occurred. If so, display information
	 * about it, and set completion value so that I/O
	 * caller is aware that an error occurred.
	 */
	if (spdk_unlikely(spdk_nvme_cpl_is_error(completion))) {
		spdk_nvme_qpair_print_completion(
			worker->qpair, (struct spdk_nvme_cpl *)completion);
		fprintf(stderr, "I/O error status: %s\n",
			spdk_nvme_cpl_get_status_string(&completion->status));
		fprintf(stderr, "Write I/O failed, aborting run\n");
		exit(1); // TODO: Handle error.
		// TODO: We can pass bio->is_complete and set it to 2 when error occurs.
	}
}


static uint32_t nvme_direct_write_async_internal(int seq_nr, baddr_t start,
						 unsigned int nr_blks,
						 bool skip_first_blk,
						 nvme_write_track_cb track_cb,
						 void *track_ctx)
{
	struct ns_entry *ns_entry;
	struct io_thread *worker;
	size_t left_bytes, bytes, bytes_to_write;
	uint32_t req_tot;
	int n, rc;
	baddr_t start_blk;
	char *buf;

	worker = &g_io_threads[tls_tid];
	ns_entry = worker->ns_entry;
	req_tot = seq_nr;
	left_bytes = nr_blks * OXBOW_BLOCK_SIZE;
	bytes = g_max_nvme_max_io_size;
	// nr_seq_max = g_max_nvme_max_io_size / OXBOW_BLOCK_SIZE;

	nvme_debug("[%s] start %lu nr_blks %u req_tot %d qpair %d", __func__,
		   start, nr_blks, req_tot, tls_tid);

	for (n = 0; n < seq_nr; n++) {
		if (left_bytes < g_max_nvme_max_io_size)
			bytes = left_bytes;

		worker->seqs[n].bi_start =
			start + nr_blks - left_bytes / OXBOW_BLOCK_SIZE;

		nvme_debug("[%s] buf=%p bytes=%u", __func__,
			   worker->seqs[n].spdk_buf, bytes);

		buf = worker->seqs[n].spdk_buf;
		start_blk = worker->seqs[n].bi_start;
		bytes_to_write = bytes;
		worker->seqs[n].wr_track_cb = track_cb;
		worker->seqs[n].wr_track_ctx = track_ctx;
		worker->seqs[n].wr_track_slot_idx = (uint32_t)n;

		if (n == 0 && skip_first_blk) {
			buf += OXBOW_BLOCK_SIZE;
			start_blk++;
			bytes_to_write -= OXBOW_BLOCK_SIZE;
		}

		nvme_debug(
			"[%s] start_blk(dst)=%lu-%lu buf(src)=%p-%p bytes_to_write=%u (%u blks)",
			__func__, start_blk,
			start_blk + bytes_to_write / OXBOW_BLOCK_SIZE - 1, buf,
			buf + bytes_to_write - 1,
			bytes_to_write, bytes_to_write / OXBOW_BLOCK_SIZE);

		rc = spdk_nvme_ns_cmd_write(ns_entry->ns, worker->qpair,
					    buf,
					    lba_to_sector(start_blk),
					    bytes_to_sector(bytes_to_write),
					    wr_complete, &worker->seqs[n], 0);
		if (rc) {
			oxb_error("nvme_direct_write_async failed");
			worker->seqs[n].wr_track_cb = NULL;
			worker->seqs[n].wr_track_ctx = NULL;
			return 0;
		}

		left_bytes -= bytes;

		if (left_bytes == 0) {
			// n == seq_nr -2 is reserved but not used case
			if (!((n == seq_nr - 1) || n == seq_nr - 2))
				oxb_warn("Inconsistency %d/%d", n, seq_nr);

			/* Else, reserved sequence but not used (not a bug) */
			req_tot = n + 1;
			break;
		}
	}

	return req_tot;
}

/**
 * @brief Caller is the IO polling thread, use its pre-allocated spdk buffer.
 *
 * @param seq_nr - How many sequence used
 * @param start - The start block of IO
 * @param nr_blks - Total number of blocks
 * @param skip_first_blk - Skip the first block (for skipping desc block)
 */
uint32_t nvme_direct_write_async(int seq_nr, baddr_t start,
				 unsigned int nr_blks, bool skip_first_blk)
{
	return nvme_direct_write_async_internal(seq_nr, start, nr_blks,
						skip_first_blk, NULL, NULL);
}

uint32_t nvme_direct_write_async_track(int seq_nr, baddr_t start,
				       unsigned int nr_blks,
				       bool skip_first_blk,
				       nvme_write_track_cb track_cb,
				       void *track_ctx)
{
	return nvme_direct_write_async_internal(seq_nr, start, nr_blks,
						skip_first_blk, track_cb,
						track_ctx);
}

uint32_t nvme_poll_completions(uint32_t max_completions)
{
	struct io_thread *worker = &g_io_threads[tls_tid];
	int32_t done;

	done = spdk_nvme_qpair_process_completions(worker->qpair,
						   (int32_t)max_completions);
	if (done < 0) {
		oxb_error("nvme_poll_completions failed: %d", done);
		panic("nvme_poll_completions failed");
	}
	return (uint32_t)done;
}

void nvme_direct_write_async_wait_complete(uint32_t req_tot)
{
	uint32_t req_done = 0;

	// polling for complete
	while (req_done < req_tot) {
		req_done += nvme_poll_completions(0);

		if (req_done > req_tot) {
			oxb_error("overflow %lu/%lu", req_done, req_tot);
			return;
		}
	}
	nvme_debug("[%s] done", __func__);

}

/**
 * @brief Caller is the IO polling thread, use its pre-allocated spdk buffer.
 * 
 * @param seq_nr - How many sequence used
 * @param start - The start block of IO
 * @param nr_blks - Total number of blocks
 * @param skip_first_blk - Skip the first block (for skipping desc block)
 */
void nvme_direct_write(int seq_nr, baddr_t start, unsigned int nr_blks, bool skip_first_blk)
{
	uint32_t req_tot;

	req_tot = nvme_direct_write_async(seq_nr, start, nr_blks, skip_first_blk);
	nvme_direct_write_async_wait_complete(req_tot);
}

/**
 * @brief Write a single sequence buffer at the given index asynchronously.
 * 
 * @param seq_idx - The index of sequence buffer to write
 * @param start - The start block of IO
 * @param nr_blks - Number of blocks to write
 * @return int (success on 0)
 */
int nvme_direct_write_one_seq_async(int seq_idx, baddr_t start,
				    unsigned int nr_blks)
{
	struct ns_entry *ns_entry;
	struct io_thread *worker;
	size_t bytes;
	char *buf;
	int rc;

	worker = &g_io_threads[tls_tid];
	ns_entry = worker->ns_entry;
	bytes = nr_blks * OXBOW_BLOCK_SIZE;

	nvme_debug("[%s] seq_idx %d start %lu nr_blks %u qpair %d", __func__,
		   seq_idx, start, nr_blks, tls_tid);

	// Ensure seq_idx is within bounds
	if (seq_idx >= nvme_get_seq_max()) {
		oxb_error("seq_idx out of bounds: %d", seq_idx);
		return -1;
	}

	// Get buffer for the specified sequence index
	buf = worker->seqs[seq_idx].spdk_buf;
	worker->seqs[seq_idx].bi_start = start;

	rc = spdk_nvme_ns_cmd_write(ns_entry->ns, worker->qpair, buf,
				    lba_to_sector(start),
				    bytes_to_sector(bytes),
				    wr_complete, &worker->seqs[seq_idx], 0);
	if (rc) {
		oxb_error("%s failed", __func__);
		return -1;
	}

	return 0;
}

/**
 * @brief Write with given buffer.
 * 
 * @param seq_idx - The index of sequence buffer to write
 * @param buf - The buffer to write
 * @param start - The start block of IO
 * @param nr_blks - Number of blocks to write
 */
void nvme_direct_write_with_buf(baddr_t start, char *buf, unsigned int nr_blks)
{
	nvme_direct_write_with_buf_async(start, buf, nr_blks);
	nvme_direct_write_async_wait_complete(1);
}

/**
 * @brief Write with given buffer asynchronously.
 * 
 * @param start - The start block of IO
 * @param buf - The buffer to write
 * @param nr_blks - Number of blocks to write
 * @return int (success on 0)
 */
int nvme_direct_write_with_buf_async(baddr_t start, char *buf, unsigned int nr_blks)
{
	struct ns_entry *ns_entry;
	struct io_thread *worker;
	size_t bytes;
	int rc;

	worker = &g_io_threads[tls_tid];
	ns_entry = worker->ns_entry;
	bytes = nr_blks * OXBOW_BLOCK_SIZE;

	nvme_debug("[%s] baddr=(%lu - %lu) nr_blks=%u buf=%p qpair(tls_tid)=%d",
		   __func__, start, start + nr_blks - 1, nr_blks, buf, tls_tid);

	rc = spdk_nvme_ns_cmd_write(ns_entry->ns, worker->qpair, buf,
				    lba_to_sector(start),
				    bytes_to_sector(bytes),
				    wr_complete_with_buf, NULL, 0);
	if (rc) {
		oxb_error("%s failed", __func__);
		return -1;
	}

	return 0;
}
