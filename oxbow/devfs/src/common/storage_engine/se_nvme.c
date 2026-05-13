#include <stdint.h>
#define __USE_GNU
#include <unistd.h>
#define __USE_GNU
#include <sched.h>

#include "spdk/nvme.h"
#include "spdk/likely.h"
#include "spdk/util.h"
// #include "spdk/env.h"
#include "global.h"
#include "storage_engine.h"
#include "global.h"
#include "se_nvme.h"
#include "oxbow_debug.h"
#include <assert.h>
#include "common/config.h"
#include <pthread.h>
#include <semaphore.h>
#include <stddef.h>
#include <sys/mman.h>
#include "thpool.h"

struct config {
	struct spdk_nvme_transport_id nvme_dev_trid;
};

struct ctrlr_entry {
	struct spdk_nvme_ctrlr *ctrlr;
	TAILQ_ENTRY(ctrlr_entry) link;
	char name[1024];
};

struct ns_entry {
	struct spdk_nvme_ctrlr *ctrlr;
	struct spdk_nvme_ns *ns;
	TAILQ_ENTRY(ns_entry) link;
	struct spdk_nvme_qpair **qpairs;
	struct spdk_nvme_poll_group *group;
	uint64_t max_io_size; // Max size of a io request.
	uint32_t max_io_queue_requests; // Max number of the queues in a qpair.
};

struct se_nvme_sequence {
	struct ns_entry *ns_entry;
	char *spdk_buf;
	char *user_buf;
	size_t io_size;
	int qpair_id;
};

struct worker_thread {
	sem_t w_sema;
	TAILQ_ENTRY(worker_thread) link;
	unsigned core;
};

static TAILQ_HEAD(,
		  worker_thread) g_workers = TAILQ_HEAD_INITIALIZER(g_workers);

static pthread_rwlock_t *g_qpair_lock;
static bool g_exit = false;

/* what if end_io takes too long? */
// static threadpool g_io_finisher;

struct se_nvme_sequence **seq_pool;

static struct spdk_nvme_transport_id g_trid = { 0 };
static struct config g_config = { 0 };
static int g_num_qpair = 0;

// TODO: Using only one name space and on controller can remove linked lists.
static TAILQ_HEAD(, ctrlr_entry)
	g_controllers = TAILQ_HEAD_INITIALIZER(g_controllers);
static TAILQ_HEAD(,
		  ns_entry) g_namespaces = TAILQ_HEAD_INITIALIZER(g_namespaces);

static bool probe_cb(void *cb_ctx, const struct spdk_nvme_transport_id *trid,
		     struct spdk_nvme_ctrlr_opts *opts)
{
	// To suppress Unused warning.
	ALL_UNUSED(cb_ctx, opts);

	oxb_debug("Probing %s -D: %s", trid->traddr,
		  &g_config.nvme_dev_trid.traddr);

	if (strcmp(g_config.nvme_dev_trid.traddr, trid->traddr) == 0) {
		oxb_debug("Attaching to %s", trid->traddr);
		return true;
	} else {
		oxb_debug("Not attaching to %s", trid->traddr);
		return false;
	}
}

static void register_ns(struct spdk_nvme_ctrlr *ctrlr, struct spdk_nvme_ns *ns)
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
	TAILQ_INSERT_TAIL(&g_namespaces, entry, link);

	printf("  Namespace ID: %d size: %juGB\n", spdk_nvme_ns_get_id(ns),
	       spdk_nvme_ns_get_size(ns) / 1000000000);
}

static void attach_cb(void *cb_ctx, const struct spdk_nvme_transport_id *trid,
		      struct spdk_nvme_ctrlr *ctrlr,
		      const struct spdk_nvme_ctrlr_opts *opts)
{
	int nsid;
	struct ctrlr_entry *entry;
	struct spdk_nvme_ns *ns;
	const struct spdk_nvme_ctrlr_data *cdata;

	// To suppress Unused warning.
	ALL_UNUSED(cb_ctx);

	entry = malloc(sizeof(struct ctrlr_entry));
	if (entry == NULL) {
		perror("ctrlr_entry malloc");
		exit(1);
	}

	printf("Attached to %s\n", trid->traddr);
	oxb_info("NVMe IO queue depth: %d", opts->io_queue_size);

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
		register_ns(ctrlr, ns);
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

	oxb_debug("permaxio %d, nomd %d", sectors_per_max_io,
		  sectors_per_max_io_no_md);

	sector_size = spdk_nvme_ns_get_sector_size(entry->ns);

	oxbow_assert(sector_size == SECTOR_SIZE);

	// Because io_flags == 0
	oxbow_assert(sectors_per_max_io == sectors_per_max_io_no_md);

	return (uint64_t)sectors_per_max_io_no_md * sector_size;
}

static void pinning_cpu(int id)
{
	pthread_t thr = pthread_self();
	cpu_set_t set;

	CPU_ZERO(&set);

	CPU_SET(id, &set);
	pthread_setaffinity_np(thr, CPU_SETSIZE, &set);
}

/**
 * @brief polling thread routine function.
 * 
 * @param args 
 * @return void* 
 */

static void *poll_complete_queue(void *args)
{
	struct ns_entry *ns_entry = NULL, *entry = NULL;
	struct worker_thread *worker = (struct worker_thread *)args;
	int ret, wait_count, timer;

	pinning_cpu(0);
	wait_count = 0;

	d_debug("[%s] worker(%d) (%d,%d)", __func__, worker->core, getpid(),
		gettid());

	TAILQ_FOREACH(entry, &g_namespaces, link)
	{
		ns_entry = entry;
	}

wait:
	sem_wait(&worker->w_sema);
	wait_count++;
	timer = 10;

	while (spdk_likely(!g_exit)) {
		pthread_rwlock_wrlock(&g_qpair_lock[worker->core]);

		ret = spdk_nvme_qpair_process_completions(
			ns_entry->qpairs[worker->core], 0);

		pthread_rwlock_unlock(&g_qpair_lock[worker->core]);

		if (ret < 0) {
			oxb_error("[%s]", __func__);
			break;
		}

		if (ret)
			wait_count -= ret;

		if (wait_count < 1 && timer < 0)
			goto wait;

		timer--;
	}
	pthread_exit(0);
}

/**
 * @brief if not exist, worker thread can be zombie affecting daemon down.
 *
 */
static void sig_handler(int signo)
{
	UNUSED1(signo);
	g_exit = true;
}

static int setup_sig_handlers(void)
{
	struct sigaction sigact = { 0 };
	int rc;

	sigemptyset(&sigact.sa_mask);
	sigact.sa_handler = sig_handler;

	rc = sigaction(SIGTERM, &sigact, NULL);
	if (rc < 0) {
		fprintf(stderr, "sigaction(SIGTERM) failed, errno %d (%s)\n",
			errno, strerror(errno));
		return -1;
	}

	return 0;
}

int nvme_init(struct se_config *se_config, int num_qpair)
{
	int rc, i;
	struct spdk_env_opts opts;
	struct ns_entry *ns_entry;
	struct spdk_nvme_io_qpair_opts qpair_opts;
	struct spdk_nvme_poll_group *group;
	struct spdk_nvme_qpair *qpair;
	struct nvme_config *nvme_conf;
	struct worker_thread *worker;
	size_t buf_size;

	if (setup_sig_handlers()) {
		oxb_error("sig setup fail");
		return -1;
	}

	spdk_env_opts_init(&opts);

	spdk_nvme_trid_populate_transport(&g_trid, SPDK_NVME_TRANSPORT_PCIE);
	snprintf(g_trid.subnqn, sizeof(g_trid.subnqn), "%s",
		 SPDK_NVMF_DISCOVERY_NQN);

	nvme_conf = &se_config->nvme;

	snprintf(&g_config.nvme_dev_trid.traddr[0],
		 SPDK_NVMF_TRADDR_MAX_LEN + 1, "%s", nvme_conf->pcie_addr);

	opts.name = "oxbow_se_nvme";
	// opts.core_mask = "0xFF";

	if (spdk_env_init(&opts) < 0) {
		fprintf(stderr, "Unable to initialize SPDK env\n");
		return 1;
	}

	rc = spdk_nvme_probe(&g_trid, NULL, probe_cb, attach_cb, NULL);
	if (rc != 0) {
		fprintf(stderr, "spdk_nvme_probe() failed\n");
		rc = 1;
		goto exit;
	}

	if (TAILQ_EMPTY(&g_controllers)) {
		fprintf(stderr, "no NVMe controllers found\n");
		rc = 1;
		goto exit;
	}

	// Initialize queue pairs.
	g_num_qpair = num_qpair;
	TAILQ_FOREACH(ns_entry, &g_namespaces, link)
	{
		ns_entry->qpairs =
			calloc(num_qpair, sizeof(struct spdk_nvme_qpair *));
		if (!ns_entry->qpairs) {
			rc = 1;
			goto exit;
		}

		spdk_nvme_ctrlr_get_default_io_qpair_opts(
			ns_entry->ctrlr, &qpair_opts, sizeof(qpair_opts));

		// Bvec size is restricted by the number of io requests in a
		// qpair (io_queue_requests).
		if (qpair_opts.io_queue_requests !=
		    nvme_conf->num_io_requests) {
			log_warn(
				"NVME default io_queue_requests is "
				"different from the current configuration. %d/%d",
				qpair_opts.io_queue_requests,
				nvme_conf->num_io_requests);
		}
		qpair_opts.io_queue_requests = nvme_conf->num_io_requests;
		oxb_info("NVME io_queue_requests=%u\n",
			 qpair_opts.io_queue_requests);

		// qpair_opts.delay_cmd_submit = true; // TODO: [OPTIMIZE] Disable for low latency.
		qpair_opts.create_only = true;

		ns_entry->group = spdk_nvme_poll_group_create(NULL, NULL);
		if (ns_entry->group == NULL) {
			rc = 2;
			goto poll_group_failed;
		}

		group = ns_entry->group;
		for (i = 0; i < num_qpair; i++) {
			ns_entry->qpairs[i] = spdk_nvme_ctrlr_alloc_io_qpair(
				ns_entry->ctrlr, &qpair_opts,
				sizeof(qpair_opts));
			qpair = ns_entry->qpairs[i];
			if (!qpair) {
				printf("ERROR: spdk_nvme_ctrlr_alloc_io_qpair failed\n");
				rc = 3;
				goto qpair_failed;
			}

			if (spdk_nvme_poll_group_add(group, qpair)) {
				printf("ERROR: unable to add I/O qpair to poll group.\n");
				spdk_nvme_ctrlr_free_io_qpair(qpair);
				rc = 3;
				goto qpair_failed;
			}

			if (spdk_nvme_ctrlr_connect_io_qpair(ns_entry->ctrlr,
							     qpair)) {
				printf("ERROR: unable to connect I/O qpair.\n");
				spdk_nvme_ctrlr_free_io_qpair(qpair);
				rc = 3;
				goto qpair_failed;
			}
		}

		ns_entry->max_io_size = get_max_io_size(ns_entry);
		ns_entry->max_io_queue_requests = nvme_conf->num_io_requests;
		oxb_info("NVME max_io_size %d", ns_entry->max_io_size);
	}

	pthread_t thr;

	g_qpair_lock = malloc(num_qpair * sizeof(*g_qpair_lock));
	if (!g_qpair_lock) {
		oxb_error("malloc fail");
		return -1;
	}

	for (i = 0; i < num_qpair; i++)
		pthread_rwlock_init(&g_qpair_lock[i], 0);

	worker = calloc(1, sizeof(*worker));
	if (!worker) {
		oxb_error("calloc fail");
		goto qpair_failed;
	}

	sem_init(&worker->w_sema, PTHREAD_PROCESS_PRIVATE, 0);
	worker->core = 0;
	TAILQ_INSERT_TAIL(&g_workers, worker, link);

	d_debug("[%s] worker pinning", __func__);
	if (pthread_create(&thr, NULL, poll_complete_queue, worker)) {
		oxb_error("pthread create");
		goto qpair_failed;
	}
	pthread_detach(thr);
	// spdk_env_thread_launch_pinned(0, poll_complete_queue, worker);
	d_debug("[%s] worker pinning", __func__);

	// TODO: [OPTIMIZE] If we use global buffer, initialize it here.
	seq_pool = malloc(num_qpair * sizeof(struct se_nvme_sequence *));
	buf_size = (g_conf.spdk_max_io_requests_in_qpair - 1) *
		   se_get_max_io_size();

	seq_pool[0] = malloc(sizeof(struct se_nvme_sequence));
	/*	qpair_id = 0 is for fsync staging path, which uses se_slab instead of spdk_buf */
	for (i = 1; i < num_qpair; i++) {
		seq_pool[i] = malloc(sizeof(struct se_nvme_sequence));
		(*seq_pool[i]).qpair_id = i;
		(*seq_pool[i]).spdk_buf =
			spdk_zmalloc(buf_size, 0x1000, NULL,
				     SPDK_ENV_SOCKET_ID_ANY, SPDK_MALLOC_DMA);

		d_debug("[qpair_id: %d] [0x%lx] se_worker buffer %ld Mbytes", i,
			(unsigned long)(*seq_pool[i]).spdk_buf, buf_size >> 20);
	}

	printf("NVMe initialization complete.\n");
	return 0;

qpair_failed:
	for (; i > 0; --i)
		spdk_nvme_ctrlr_free_io_qpair(ns_entry->qpairs[i - 1]);

	spdk_nvme_poll_group_destroy(ns_entry->group);

poll_group_failed:
	free(ns_entry->qpairs);
	ns_entry->qpairs = NULL;

exit:
	nvme_exit();
	return rc;
}

static void read_complete(void *arg, const struct spdk_nvme_cpl *completion)
{
	struct se_nvme_sequence *sequence = arg;

	/* See if an error occurred. If so, display information
	 * about it, and set completion value so that I/O
	 * caller is aware that an error occurred.
	 */
	if (spdk_unlikely(spdk_nvme_cpl_is_error(completion))) {
		spdk_nvme_qpair_print_completion(
			sequence->ns_entry->qpairs[sequence->qpair_id],
			(struct spdk_nvme_cpl *)completion);
		fprintf(stderr, "I/O error status: %s\n",
			spdk_nvme_cpl_get_status_string(&completion->status));
		fprintf(stderr, "Read I/O failed, aborting run\n");
		exit(1); // TODO: Handle error.
		// TODO: We can pass bio->is_completed and set it to 2 when error occurs.
	}

	memcpy(sequence->user_buf, sequence->spdk_buf, sequence->io_size);
	oxb_debug("READ: qpair_id=%d target=0x%lx", sequence->qpair_id,
		  sequence->user_buf);
	spdk_free(sequence->spdk_buf);
	free(sequence);
}

/**
 * @brief 
 * 
 * @param buf x1
 * @param baddr 
 * @param io_size 
 * @param qpair_id
 * @return size_t 
 */
size_t nvme_read(char *buf, baddr_t baddr, size_t io_size, int qpair_id)
{
	struct ns_entry *ns_entry;
	struct se_nvme_sequence *sequence;
	size_t ret = 0;
	int rc;

	// Check io_size is 4KB aligned.
	if (io_size % OXBOW_BLOCK_SIZE != 0) {
		log_error("io_size is not aligned by 4KB.\n");
		return -1;
	}

	// TODO: [OPTIMIZE] Remove malloc from the critical path.
	sequence = malloc(sizeof(struct se_nvme_sequence));
	// sequence = seq_pool[qpair_id];
	sequence->user_buf = buf;
	sequence->io_size = io_size;
	sequence->qpair_id = qpair_id;

	TAILQ_FOREACH(ns_entry, &g_namespaces, link)
	{
		sequence->spdk_buf =
			spdk_zmalloc(io_size, 0x1000, NULL,
				     SPDK_ENV_SOCKET_ID_ANY, SPDK_MALLOC_DMA);

		if (sequence->spdk_buf == NULL) {
			printf("ERROR: read buffer allocation failed\n");
			return -1;
		}
		sequence->ns_entry = ns_entry;

		rc = spdk_nvme_ns_cmd_read(
			ns_entry->ns, ns_entry->qpairs[qpair_id],
			sequence->spdk_buf, baddr_to_lba(baddr), /* LBA start */
			byte_to_lba_cnt(io_size), /* number of LBAs */
			read_complete, sequence, 0);
		if (rc != 0) {
			fprintf(stderr, "starting read I/O failed\n");
			exit(1);
		}
		ret += spdk_align_roundup(io_size, ns_entry->max_io_size);
	}
	return ret;
}

// static void bio_finish(void *args)
// {
// 	struct ra_bio *bio = args;
// 	memcpy(bio->bvec, bio->spdk_buf, bio->nr_pages * PAGE_SIZE);
// 	spdk_free(bio->spdk_buf);
// 	bio->end_io(bio);
// }

static void bio_complete(void *args, const struct spdk_nvme_cpl *completion)
{
	struct ra_bio *bio = args;

	if (spdk_unlikely(spdk_nvme_cpl_is_error(completion))) {
		fprintf(stderr, "I/O error status: %s\n",
			spdk_nvme_cpl_get_status_string(&completion->status));
		fprintf(stderr, "Read I/O failed, aborting run\n");
		exit(1);
	}

	memcpy(bio->bvec, bio->spdk_buf, bio->nr_pages * PAGE_SIZE);
	spdk_free(bio->spdk_buf);
	bio->end_io(bio);

	// thpool_add_work(g_io_finisher, bio_finish, args);
}

int se_nvme_submit_io(struct ra_bio *bio, int qpair_id)
{
	struct ns_entry *ns_entry;
	int ret = 0;
	// struct timespec ts;

	// clock_gettime(CLOCK_MONOTONIC, &ts);
	// printf("[%04ld] [%s] bio(%lu)\n", ts.tv_nsec / 1000, __func__,
	//        bio->index);

	// d_debug("[%s]", __func__);

	bio->spdk_buf = spdk_malloc(bio->nr_pages * PAGE_SIZE, 0x1000, NULL,
				    SPDK_ENV_SOCKET_ID_ANY, SPDK_MALLOC_DMA);
	if (!bio->spdk_buf) {
		oxb_error("[%s] malloc fail", __func__);
		return -1;
	}

	TAILQ_FOREACH(ns_entry, &g_namespaces, link)
	{
		pthread_rwlock_rdlock(&g_qpair_lock[qpair_id]);
		ret = spdk_nvme_ns_cmd_read(
			ns_entry->ns, ns_entry->qpairs[qpair_id], bio->spdk_buf,
			baddr_to_lba(bio->bvec_lba), /* LBA start */
			byte_to_lba_cnt(bio->nr_pages *
					PAGE_SIZE), /* number of LBAs */
			bio_complete, bio, 0);
		pthread_rwlock_unlock(&g_qpair_lock[qpair_id]);
		if (ret)
			oxb_error("[%s] dispatch fail");
	}

	sem_post(&TAILQ_FIRST(&g_workers)->w_sema);

	// d_debug("[%s] done ret(%d)", __func__, ret);

	return ret;
}

/**
 * @brief Write callback function.
 * 
 * @param arg 
 * @param completion 
 */
static void write_complete(void *arg, const struct spdk_nvme_cpl *completion)
{
	struct se_nvme_sequence *sequence = arg;

	/* See if an error occurred. If so, display information
	 * about it, and set completion value so that I/O
	 * caller is aware that an error occurred.
	 */
	if (spdk_unlikely(spdk_nvme_cpl_is_error(completion))) {
		spdk_nvme_qpair_print_completion(
			sequence->ns_entry->qpairs[sequence->qpair_id],
			(struct spdk_nvme_cpl *)completion);
		fprintf(stderr, "I/O error status: %s\n",
			spdk_nvme_cpl_get_status_string(&completion->status));
		fprintf(stderr, "Write I/O failed, aborting run\n");
		exit(1); // TODO: Handle error.
		// TODO: We can pass bio->is_complete and set it to 2 when error occurs.
	}

	// spdk_free(sequence->spdk_buf);
	// free(sequence);
}

/**
 * @brief 
 * 
 * @param buf 
 * @param baddr 
 * @param io_size It should be aligned by 4KB.
 * @param qpair_id
 * @return uint32_t Submitted `io_size`. -1 on error.
 */
size_t nvme_write(char *buf, baddr_t baddr, size_t io_size, int qpair_id)
{
	struct ns_entry *ns_entry;
	struct se_nvme_sequence *sequence;
	int rc;

	oxb_debug("NVME_write qpair_id=%d", qpair_id);

	// Check io_size is 4KB aligned.
	if (io_size % OXBOW_BLOCK_SIZE != 0) {
		log_error("io_size is not aligned by 4KB.\n");
		return -1;
	}

	// TODO: [OPTIMIZE] Remove malloc from the critical path.
	// sequence = malloc(sizeof(struct se_nvme_sequence));
	sequence = seq_pool[qpair_id];

	// sequence->user_buf = buf; // Not required.
	sequence->io_size = io_size;
	// sequence->qpair_id = qpair_id;

	TAILQ_FOREACH(ns_entry, &g_namespaces, link)
	{
		// sequence->spdk_buf =
		// 	spdk_zmalloc(io_size, 0x1000, NULL,
		// 		     SPDK_ENV_SOCKET_ID_ANY, SPDK_MALLOC_DMA);
		if (sequence->spdk_buf == NULL) {
			printf("ERROR: write buffer allocation failed\n");
			return -1;
		}
		sequence->ns_entry = ns_entry;

		memcpy(sequence->spdk_buf, buf, io_size);

		oxb_debug("WRITE: qpair_id=%d from=0x%lx", qpair_id, buf);

		rc = spdk_nvme_ns_cmd_write(
			ns_entry->ns, ns_entry->qpairs[qpair_id],
			sequence->spdk_buf, baddr_to_lba(baddr), /* LBA start */
			byte_to_lba_cnt(io_size), /* number of LBAs */
			write_complete, sequence, 0);
		if (rc != 0) {
			fprintf(stderr,
				"spdk_nvme_ns_cmd_write failed. rc=%d\n", rc);
			exit(1);
		}
	}

	return io_size;
}

/**
 * @brief nvme_write with no memcpy, buffer is already filled.
 * 
 * @param baddr 
 * @param io_size It should be aligned by 4KB.
 * @param qpair_id
 * @return uint32_t Submitted `io_size`. -1 on error.
 */
size_t nvme_write_nocopy(char *buf, baddr_t baddr, size_t io_size, int qpair_id)
{
	struct ns_entry *ns_entry;
	struct se_nvme_sequence *sequence;
	int rc;

	d_debug("NVME_write qpair_id=%d", qpair_id);

	// Check io_size is 4KB aligned.
	if (io_size % OXBOW_BLOCK_SIZE != 0) {
		log_error("io_size is not aligned by 4KB.\n");
		return -1;
	}

	// [TODO] multi-process fsync not ready
	sequence = seq_pool[qpair_id];
	sequence->io_size = io_size;

	TAILQ_FOREACH(ns_entry, &g_namespaces, link)
	{
		sequence->ns_entry = ns_entry;

		oxb_debug("WRITE: qpair_id=%d from=0x%lx", qpair_id, buf);

		rc = spdk_nvme_ns_cmd_write(
			ns_entry->ns, ns_entry->qpairs[qpair_id], buf,
			baddr_to_lba(baddr), /* LBA start */
			byte_to_lba_cnt(io_size), /* number of LBAs */
			write_complete, sequence, 0);
		if (rc != 0) {
			fprintf(stderr,
				"spdk_nvme_ns_cmd_write failed. rc=%d\n", rc);
			exit(1);
		}
	}

	return io_size;
}

char *nvme_get_buffer(int qpair_id)
{
	oxb_debug("[nvme_get_buffer] qpair_id=%d", qpair_id);

	return seq_pool[qpair_id]->spdk_buf;
}

void nvme_poll_complete(size_t submit_size, int qpair_id)
{
	uint64_t complete_cnt;
	int ret;
	uint64_t submit_cnt;
	struct ns_entry *entry = 0, *ns_entry = 0;

	// Currently, there is only one namespace.
	TAILQ_FOREACH(entry, &g_namespaces, link)
	{
		ns_entry = entry;
	}

	complete_cnt = 0;

	// Calculate submitted I/O request. SPDK splits large IO.
	submit_cnt = spdk_divide_round_up((uint64_t)submit_size,
					  ns_entry->max_io_size);

	// if the submit count is identical to the max io requests,
	// nvme request allocation failed. Submit count is decided by the size
	// of bvec.
	oxbow_assert(submit_cnt < ns_entry->max_io_queue_requests);

	oxb_debug(
		"POLL COMPLETE BEGIN qpair_id=%d submit_size=%lu spdk_max_io_size=%lu submit_cnt=%lu",
		qpair_id, submit_size, ns_entry->max_io_size, submit_cnt);

	while (complete_cnt < submit_cnt) {
		ret = spdk_nvme_qpair_process_completions(
			ns_entry->qpairs[qpair_id], 0);
		complete_cnt += ret;

		if (ret != 0)
			oxb_debug(" qpair_id=%d cnt: complete/submit=%d/%d",
				  qpair_id, complete_cnt, submit_cnt);
	}

	if (complete_cnt != submit_cnt)
		oxb_error("complete cnt %d/ submit_cnt %d", complete_cnt,
			  submit_cnt);
	// oxbow_assert(complete_cnt == submit_cnt);
}

void nvme_poll_complete2(size_t submit_cnt, int qpair_id)
{
	uint64_t complete_cnt;
	int ret;
	struct ns_entry *entry = 0, *ns_entry = 0;

	// Currently, there is only one namespace.
	TAILQ_FOREACH(entry, &g_namespaces, link)
	{
		ns_entry = entry;
	}

	complete_cnt = 0;

	// if the submit count is identical to the max io requests,
	// nvme request allocation failed. Submit count is decided by the size
	// of bvec.
	oxbow_assert(submit_cnt < ns_entry->max_io_queue_requests);

	oxb_debug(
		"POLL COMPLETE BEGIN qpair_id=%d submit_cnt=%lu spdk_max_io_size=%lu submit_cnt=%lu",
		qpair_id, submit_cnt, ns_entry->max_io_size, submit_cnt);

	while (complete_cnt < submit_cnt) {
		ret = spdk_nvme_qpair_process_completions(
			ns_entry->qpairs[qpair_id], 0);
		complete_cnt += ret;

		if (ret != 0)
			oxb_debug(" qpair_id=%d cnt: complete/submit=%d/%d",
				  qpair_id, complete_cnt, submit_cnt);
	}

	oxb_error("complete cnt %d/ submit_cnt %d", complete_cnt, submit_cnt);
	// oxbow_assert(complete_cnt == submit_cnt);
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
	struct ns_entry *ns_entry;
	int i;

	//TODO: Check whether all qpairs in the group finished.

	TAILQ_FOREACH(ns_entry, &g_namespaces, link)
	{
		if (ns_entry->qpairs != NULL) {
			for (i = g_num_qpair; i > 0; --i) {
				spdk_nvme_ctrlr_free_io_qpair(
					ns_entry->qpairs[i - 1]);
			}

			spdk_nvme_poll_group_destroy(ns_entry->group);
			free(ns_entry->qpairs);
			ns_entry->qpairs = NULL;
		}
	}

	cleanup();
	spdk_env_fini();

	/* free globally allocated memory */
	for (i = 0; i < g_num_qpair; i++) {
		if (!seq_pool[i])
			continue;
		if (seq_pool[i]->spdk_buf)
			spdk_free(seq_pool[i]->spdk_buf);
		free(seq_pool[i]);
	}
	free(seq_pool);
}

uint64_t nvme_get_max_io_size(void)
{
	// Assuming there is only one ns_entry.
	struct ns_entry *ns_entry;
	TAILQ_FOREACH(ns_entry, &g_namespaces, link)
	{
		return ns_entry->max_io_size;
	}

	return 0;
}
