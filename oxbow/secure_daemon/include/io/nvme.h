#ifndef _NVME_H_
#define _NVME_H_

#include <stddef.h>
#include <stdint.h>
#include <stdatomic.h>
#include "oxbow.h"
#include "list.h"
#include "io/sd_bio.h"
#include <sys/types.h>
#include <string.h>
#include <assert.h>

/* Maximum number of NVMe VFs (virtual functions) that can be probed
 * concurrently. Must match (or exceed) MAX_NVME_VFS used in
 * secure_daemon/include/config.h.
 */
#define NVME_MAX_VFS 4
/* Must be >= SPDK_NVMF_TRADDR_MAX_LEN + 1 (257) so snprintf from a
 * SPDK trid->traddr never needs truncation.
 */
#define NVME_MAX_TRADDR_LEN 260

struct nvme_config {
	/* Array of PCIe BDFs for each VF to probe. */
	char pcie_addrs[NVME_MAX_VFS][NVME_MAX_TRADDR_LEN];
	int nr_addrs;
};

typedef void (*nvme_write_track_cb)(void *ctx, uint32_t slot_idx,
				    int is_error);

extern size_t g_max_nvme_max_io_size;
extern int nvme_target_qd;

/* Multi-VF helper: pass an array of BDFs. Each entry must fit into
 * NVME_MAX_TRADDR_LEN. For a single-VF setup, pass nr_addrs=1.
 */
static inline void set_nvme_config_list(struct nvme_config *conf,
					char *const *pcie_nvme_addrs,
					int nr_addrs)
{
	assert(nr_addrs > 0 && nr_addrs <= NVME_MAX_VFS);
	conf->nr_addrs = nr_addrs;
	for (int i = 0; i < nr_addrs; i++) {
		assert(pcie_nvme_addrs[i]);
		assert(strlen(pcie_nvme_addrs[i]) < NVME_MAX_TRADDR_LEN);
		strcpy(conf->pcie_addrs[i], pcie_nvme_addrs[i]);
	}
}

/*
 * Initialize SPDK + per-tid IO resources and return the iod_workers thpool.
 *
 * iod_thread_nr   : Size of the iod_workers thpool (tid 0..iod_thread_nr-1).
 *                   These threads handle write/fsync/RPC and the legacy
 *                   read pump.
 * total_qpair_nr  : Total number of g_io_threads slots to provision (each
 *                   gets its own SPDK qpair + per-tid resources).
 *                   Must be >= iod_thread_nr. The trailing
 *                   total_qpair_nr - iod_thread_nr slots are reserved for
 *                   external workers (e.g., D-2 read_workers in
 *                   src/kernfs/file_ops.c) that bind to those tids manually.
 *
 * For non-D-2 builds, pass iod_thread_nr == total_qpair_nr.
 */
struct thpool_ *nvme_init(struct nvme_config *, int iod_thread_nr,
			  int total_qpair_nr);
void nvme_exit(void);

/*
 * Submit a list of read BIOs to the current worker's qpair, inline,
 * polling completions until all submitted BIOs are done. Caller must run
 * on a thread that owns a SPDK qpair (tls_tid set, tls_ioworker == 1) and
 * must hold tls_inline_batch_active during the call so iod_submit_bio()
 * routes to the inline path.
 */
void nvme_submit_bio_inline(struct bio **bios, int n);

void nvme_wr_submit_bio(void *arg);
void nvme_rd_submit_bio(void *arg);
void nvme_stop_rd_workers(void);
void nvme_init_rd_workers(int worker_nr);
void nvme_signal_rd_worker(void);
void nvme_destroy_rd_workers(void);
void nvme_schedule_read_pump(void);
void nvme_fsync_inflight_inc(void);
void nvme_fsync_inflight_dec(void);
/* Snapshot of the fsync-inflight counter for QoS decisions. */
int nvme_fsync_inflight_get(void);
void nvme_rd_submit_bh(void *arg);
void nvme_wr_submit_bh(void *arg);

void *nvme_get_seq_buffer(int seq_idx);
int nvme_get_seq_max(void);
void nvme_direct_write(int seq_nr, baddr_t start, unsigned int nr_blks,
		       bool skip_first_blk);
uint32_t nvme_direct_write_async(int seq_nr, baddr_t start,
				 unsigned int nr_blks, bool skip_first_blk);
uint32_t nvme_direct_write_async_track(int seq_nr, baddr_t start,
				       unsigned int nr_blks,
				       bool skip_first_blk,
				       nvme_write_track_cb track_cb,
				       void *track_ctx);
void nvme_direct_write_async_wait_complete(uint32_t req_tot);
uint32_t nvme_poll_completions(uint32_t max_completions);
int nvme_direct_write_one_seq_async(int seq_idx, baddr_t start,
				    unsigned int nr_blks);
void nvme_direct_write_one_seq(int seq_idx, baddr_t start, unsigned int nr_blks);
int nvme_direct_write_with_buf_async(baddr_t start, char *buf, unsigned int nr_blks);
void nvme_direct_write_with_buf(baddr_t start, char *buf, unsigned int nr_blks);
void *nvme_get_desc_blk_buffer(void);
void *nvme_get_commit_blk_buffer(void);
void nvme_ra_stats_dump(void);

#endif
