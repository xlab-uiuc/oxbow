#include <assert.h>
#include <limits.h>
#include <pthread.h>
#include <semaphore.h>
#include <stdatomic.h>
#include <stddef.h>
#include <stdio.h>
#include <stdbool.h>
#include "global.h"
#include "thpool.h"
#include "config.h"
#include "storage_engine.h"
#include "se_nvme.h"
#include "se_nvmf.h"
#include <string.h>

#include "oxbow_debug.h"
#include "common/time_stat.h"

static bool has_baddr_in_bl(struct bio_list *bl, baddr_t target_blk_addr);

static struct storage_operations g_sops;
static threadpool se_thpool; // Worker thread pool.

struct storage_operations storage_ops_nvme = {
	.init = nvme_init,
	.read = nvme_read,
	.write = nvme_write,
	.write_nocopy = nvme_write_nocopy,
	.get_buffer = nvme_get_buffer,
	.poll_complete = nvme_poll_complete,
	.poll_complete2 = nvme_poll_complete2,
	.exit = nvme_exit,
	.get_max_io_size = nvme_get_max_io_size,
	.register_buf_to_spdk = NULL,
	.dispatch_io_fast = NULL,
};
struct storage_operations storage_ops_nvmf = {
	.init = nvmf_init,
	// .read = nvmf_read, // REFACTOR: To be deleted or changed.
	// .write = nvmf_write, // REFACTOR: To be deleted or changed.
	// .nvmf_poll_complete = NULL,
	.nvmf_poll_complete_fast = nvmf_poll_complete_fast,
	.nvmf_peek_completion = NULL,
	// .nvmf_release_worker = nvmf_release_worker,
	.nvmf_release_worker = NULL,
	.exit = nvmf_exit,
	.get_max_io_size = nvmf_get_max_io_size,
	.alloc_dma_buffer = nvmf_alloc_dma_buf,
	.register_buf_to_spdk = nvmf_register_buf_to_spdk,
	// .dispatch_io = nvmf_dispatch_io,
	// .dispatch_io = NULL,
	.dispatch_io_fast = nvmf_dispatch_io_fast,
};

void set_nvme_config(struct nvme_config *conf, const char *pcie_nvme_addr,
		     uint32_t num_io_requests)
{
	assert(strlen(pcie_nvme_addr) < sizeof(conf->pcie_addr));
	strcpy(conf->pcie_addr, pcie_nvme_addr);
	conf->num_io_requests = num_io_requests;
}

void set_nvmf_config(struct nvmf_config *conf, const char *target_ip_addr,
		     const int port, const char *subnqn_name,
		     uint32_t num_io_requests)
{
	assert(strlen(target_ip_addr) < sizeof(conf->target_ip_addr));
	assert(strlen(subnqn_name) < sizeof(conf->subnqn_name));

	strcpy(conf->target_ip_addr, target_ip_addr);
	conf->port = port;
	strcpy(conf->subnqn_name, subnqn_name);
	conf->num_io_requests = num_io_requests;
}

/**
 * @brief 
 * 
 * @param type 
 * @param config nvme or nvmf config.
 * @param thread_num 
 * @return int 
 */
int init_storage_engine(enum storage_engine_type type, struct se_config *config,
			int thread_num)
{
	int rc;

	switch (type) {
	case SE_NVME:
		g_sops = storage_ops_nvme;
		break;

	case SE_NVMF:
		g_sops = storage_ops_nvmf;

		// FIXME:
		// No threadpool for NVMF engine. The number of thread is
		// statically set at init_nvmf() in se_nvmf2.c.
		rc = g_sops.init(config, thread_num);
		if (rc != 0) {
			printf("Storage engine initialization failed.\n");
			return rc;
		}

		se_thpool = config->nvmf.worker_thpool;

		return 0;

	default:
		log_error("Unknown storage engine type: %d", type);
		return -1;
	}

	// Init storage device. One queue pair for each thread. Add one more for fsync
	rc = g_sops.init(config, thread_num + 1);
	if (rc != 0) {
		printf("Storage engine initialization failed.\n");
		return rc;
	}

	/** @note  Why add 1 to tls_tid?
 	*  	Thread id and qpair_id is one to one mapping but qpair_id is for fsync path.
 	*	It's hard to modify thpool to get thread ID start with 1.
	*	So just map it to next qpair, therefore no thread can access qpair_id 0
 	*/
	se_thpool = thpool_init(thread_num, "se_thpool");
	oxb_info(
		"Storage engine threads initialized. %d worker threads created.",
		thread_num);

	return 0;
}

/**
 * @brief A write job for each thread.
 * 
 * @param arg
 */
static void do_write(void *arg)
{
	struct bio *bio;
	struct bio_vec *bvec;
	size_t submitted; // size in byte.
	int i;
	baddr_t blk_no;

	bio = (struct bio *)arg;

	// oxb_debug("Start do_write");

	blk_no = bio->bi_iter.bi_blk_no;

	// For each bi_io_vec, submit all I/O requests.
	for (i = 0; i < bio->bi_vcnt; i++) {
		bvec = &bio->bi_io_vec[i];
		oxbow_assert(bvec->bv_len > 0);
		oxbow_assert(bvec->bv_len % 0x1000 == 0); // 4KB aligned.
		oxbow_assert(bvec->bv_len % SECTOR_SIZE == 0); // n * sectors.
		oxbow_assert(bvec->bv_buf != NULL);

		oxb_debug(
			"WRITE SUBMIT: qpair=%d blk_no=%lu size=%lu(%lu blks)",
			tls_tid + 1, blk_no, bvec->bv_len,
			bytes_to_nblks(bvec->bv_len));

		submitted = g_sops.write(bvec->bv_buf, blk_no, bvec->bv_len,
					 tls_tid + 1);
		if (submitted != bvec->bv_len) {
			log_error(
				"Storage engine WRITE failed. io_size(requested)=%lu submitted(returned)=%lu",
				bvec->bv_len, submitted);
			panic("Storage engine WRITE failed.");
		}

		blk_no += bytes_to_nblks(bvec->bv_len);
		g_sops.poll_complete(submitted, tls_tid + 1);
	}

	// TODO: [OPTIMIZE] Can we do better async polling?
	// Poll after all the I/O requests are submitted.
	// g_sops.poll_complete(submitted, tls_tid + 1);

	set_is_completed(bio, 1);
}

void se_write(struct bio_list *bl)
{
	struct bio *bio;

	bio_list_for_each (bio, bl) {
		// bio->is_completed = 0; // relaxed memory order is sufficient.
		set_is_completed(bio, 0);

		oxb_debug("Add write work.");

		// Invoke a worker thread.
		// Here, we have to choose the unit of parallelism.
		// We can parallelize bios or bi_io_vecs.
		// Currently, each worker thread processes one bio request
		// which is composed of a (or rarely two) bi_io_vecs.
		thpool_add_work(se_thpool, do_write, (void *)bio);
	}

	// TODO: [OPTIMIZE] Sleep for saving CPU resource. Or, hybrid polling,
	// TODO: I/O time estimation can be adopted.
	//
	// Polling all the bio requests are completed.
	bio_list_for_each (bio, bl) {
		while (is_completed(bio) == 0)
			;
	}
}

/**
 * @brief This function does not check the completion. The caller has to call
 * `se_is_completed()` to check the I/O of the bio list has been completed.
 * 
 * @param bl 
 */
void se_write_async(struct bio_list *bl)
{
	struct bio *bio;

	bio_list_for_each (bio, bl) {
		// bio->is_completed = 0; // relaxed memory order is sufficient.
		set_is_completed(bio, 0);

		oxb_debug("Add write work.");

		// Invoke a worker thread.
		// Here, we have to choose the unit of parallelism.
		// We can parallelize bios or bi_io_vecs.
		// Currently, each worker thread processes one bio request
		// which is composed of a (or rarely two) bi_io_vecs.
		thpool_add_work(se_thpool, do_write, (void *)bio);
	}
}

/**
 * @brief Return true if the I/O of the bio list is completed.
 * 
 * @param bl 
 */
int se_is_completed(struct bio_list *bl)
{
	struct bio *bio;

	// Return false if any bio request is not completed.
	bio_list_for_each (bio, bl) {
		if (!is_completed(bio))
			return 0;
	}

	return 1;
}

static void do_read(void *arg)
{
	struct bio *bio;
	struct bio_vec *bvec;
	int i;
	size_t submitted = 0;
	baddr_t blk_no;

	bio = (struct bio *)arg;

	blk_no = bio->bi_iter.bi_blk_no;

	// For each bi_io_vec, submit all I/O requests.
	for (i = 0; i < bio->bi_vcnt; i++) {
		bvec = &bio->bi_io_vec[i];
		oxbow_assert(bvec->bv_len > 0);
		oxbow_assert(bvec->bv_len % 0x1000 == 0); // 4KB aligned.
		oxbow_assert(bvec->bv_len % SECTOR_SIZE == 0); // n * sectors.
		oxbow_assert(bvec->bv_buf != NULL);

		oxb_debug("READ SUBMIT: qpair=%d blk_no=%lu size=%lu", 2,
			  blk_no, bvec->bv_len);

		submitted += g_sops.read(bvec->bv_buf, blk_no, bvec->bv_len, 1);

		blk_no += bytes_to_nblks(bvec->bv_len);
	}

	g_sops.poll_complete(submitted, 1);
}

void se_read(struct bio_list *bl)
{
	struct bio *bio;

	bio_list_for_each (bio, bl) {
		// bio->is_completed = 0; // relaxed memory order is sufficient.
		// set_is_completed(bio, 0);

		// Invoke a worker thread.
		// Here, we have to choose the unit of parallelism.
		// We can parallelize bios or bi_io_vecs.
		// Currently, each worker thread processes one bio request
		// which is composed of several bi_io_vecs.
		// (In DevFS, there are at most 2 bi_io_vecs.)
		// thpool_add_work(se_thpool, do_read, (void *)bio);
		do_read(bio);
	}

	// Polling all the bio requests are completed.
	// bio_list_for_each (bio, bl) {
	// 	while (is_completed(bio) == 0)
	// 		;
	// }
}

void se_read_async(struct bio_list *bl)
{
	struct bio *bio;

	bio_list_for_each (bio, bl) {
		// bio->is_completed = 0; // relaxed memory order is sufficient.
		set_is_completed(bio, 0);

		oxb_debug("Add read work.");

		// Invoke a worker thread.
		// Here, we have to choose the unit of parallelism.
		// We can parallelize bios or bi_io_vecs.
		// Currently, each worker thread processes one bio request
		// which is composed of several bi_io_vecs.
		// (In DevFS, there are at most 2 bi_io_vecs.)
		thpool_add_work(se_thpool, do_read, (void *)bio);
	}
}

uint64_t se_get_max_io_size(void)
{
	return g_sops.get_max_io_size();
}

char *se_alloc_dma_buffer(size_t size)
{
	return g_sops.alloc_dma_buffer(size);
}

int se_register_buf_to_spdk(void *buf, size_t size)
{
	return g_sops.register_buf_to_spdk(buf, size);
}

/**
 * @brief Return after submitting I/O requests. Caller should call
 * se_nvmf_io_completed() manually.
 * 
 * @param bl 
 * @param is_read 
 */
uint32_t se_dispatch_io_async(struct bio_list *bl, bool is_read)
{
	uint32_t req_cnt;
	req_cnt = g_sops.dispatch_io_fast(bl, is_read, NULL);
	if (req_cnt == 0) {
		log_warn("Nothing dispatched.");
		return 0;
	}
	return req_cnt;
}

/**
  * @brief Dispatch io requests to NVMf target.
  * 
  * @param bl 
  * @param is_read 
  * @return int Number of dispatched requests.
  */
int se_dispatch_io_sync(struct bio_list *bl, bool is_read)
{
	uint32_t req_cnt;

	req_cnt = g_sops.dispatch_io_fast(bl, is_read, NULL);
	if (req_cnt == 0) {
		log_warn("Nothing dispatched.");
		return 0;
	}

	// Do busy polling until all I/Os are completed.
	g_sops.nvmf_poll_complete_fast(req_cnt);
	return 0;
}

struct io_dispatch_arg {
	struct bio_list *bl;
	bool is_read;
	sem_t *sem;
};

static void dispatch_io_sync_fn(void *arg)
{
	struct io_dispatch_arg *ida = (struct io_dispatch_arg *)arg;

	se_dispatch_io_sync(ida->bl, ida->is_read);
	sem_post(ida->sem);
}

/**
 * @brief Request IO worker thread to dispatch IO requests.
 * 
 * @param bl 
 * @param is_read 
 * @return int 
 */
int se_request_dispatch_io_sync(struct bio_list *bl, bool is_read)
{
	struct io_dispatch_arg ida;
	sem_t sem;
	int ret = 0;

	ida.bl = bl;
	ida.is_read = is_read;
	ida.sem = &sem;

	sem_init(&sem, 0, 0);

	ret = thpool_add_work(se_thpool, dispatch_io_sync_fn, (void *)&ida);
	if (ret == -1) {
		log_error("Failed to add work to thread pool.");
		return -1;
	}

	// Wait for the I/O request to be completed.
	sem_wait(&sem);
	sem_destroy(&sem);

	return 0;
}

/**
  * @brief Dispatch io requests to NVMf target with custom buffer (spdk dma buffer).
  * 
  * @param bl 
  * @param is_read 
  * @param custom_buf Pre-allocated spdk dma buffer.
  * @return int Number of dispatched requests.
  */
uint32_t se_dispatch_io_nocopy_async(struct bio_list *bl, bool is_read,
				     char *custom_buf)
{
	int req_cnt;

	// Not supporting read path for custom_buf yet.
	oxbow_assert(is_read == 0);

	req_cnt = g_sops.dispatch_io_fast(bl, is_read, custom_buf);
	if (req_cnt == 0) {
		log_warn("Nothing dispatched.");
		return 0;
	}

	return req_cnt;
}

int se_dispatch_io_nocopy_sync(struct bio_list *bl, bool is_read,
			       char *custom_buf)
{
	int req_cnt;

	// Not supporting read path for custom_buf yet.
	oxbow_assert(is_read == 0);

	req_cnt = g_sops.dispatch_io_fast(bl, is_read, custom_buf);
	if (req_cnt == 0) {
		log_warn("Nothing dispatched.");
		return 0;
	}

	// Do busy polling until all I/Os are completed.
	g_sops.nvmf_poll_complete_fast(req_cnt);
	return 0;
}

/**
 * @brief This function includes releasing the worker id.
 * 
 * @param req_cnt 
 */
void se_nvmf_poll_complete(uint32_t req_cnt)
{
	// Do busy polling until all I/Os are completed.
	g_sops.nvmf_poll_complete_fast(req_cnt);
}

/**
 * @brief Checks if a target block address is contained within any block range
 *        represented by the bio entries in a bio_list.
 *
 * @param bl The list of bio structures to check.
 * @param target_blk_addr The block address to search for.
 * @return true if the target block address is found within the range of any
 *         bio_vec in the list, false otherwise.
 */
static bool has_baddr_in_bl(struct bio_list *bl, baddr_t target_blk_addr)
{
	struct bio *bio;
	struct bio_vec *bvec;
	baddr_t current_blk_addr;
	uint64_t nblks;
	baddr_t end_blk_addr;
	int i;

	bio_list_for_each (bio, bl) {
		current_blk_addr = bio->bi_iter.bi_blk_no;
		for (i = 0; i < bio->bi_vcnt; i++) {
			bvec = &bio->bi_io_vec[i];
			// Assuming bytes_to_nblks is available and correctly converts bytes to blocks.
			// If bv_len is 0, nblks should be 0.
			if (bvec->bv_len == 0)
				continue;

			nblks = bytes_to_nblks(bvec->bv_len);
			end_blk_addr = current_blk_addr + nblks;

			// Check if the target block address is within the current range [current_blk_addr, end_blk_addr)
			if (target_blk_addr >= current_blk_addr &&
			    target_blk_addr < end_blk_addr) {
				return true;
			}

			current_blk_addr =
				end_blk_addr; // Move to the start of the next vec
		}
	}

	return false; // Target address not found in any bio_vec range
}

int exit_storage_engine(void)
{
	g_sops.exit();
	return 0;
}
