#ifndef _SE_NVMF_H_
#define _SE_NVMF_H_

#include <stdio.h>
#include "oxbow.h"
#include "storage_engine.h"

/* NVMe storage engine interface */
int nvmf_init(struct se_config *se_config, int num_qpair);
void nvmf_exit(void);

/**
 * @brief This function should be called by a single thread.
 * SPDK doesn't allow multiple threads' access to a single I/O queue.
 * 
 * @param buf 
 * @param baddr
 * @param io_size 4KB aligned size.
 * @param qpair_id
 * @return uint32_t 
 */
size_t nvmf_read(char *buf, baddr_t baddr, size_t io_size, int qpair_id);

/**
 * @brief This function should be called by a single thread.
 * SPDK doesn't allow multiple threads' access to a single I/O queue.
 * 
 * @param df_id Data fetcher ID.
 * @param bl
 * @param is_read
 * @param custom_buf Preallocated SPDK buffer (custom buffer) that contains data to
 * copy. (Currently supports only write.)
 * @return uint32_t Allocated worker id. -1 on error.
 */
int nvmf_dispatch_io(int df_id, struct bio_list *bl, bool is_read,
		     char *custom_buf);
uint32_t nvmf_dispatch_io_fast(struct bio_list *bl, bool is_read,
			       char *custom_buf);

/**
 * @brief This function should be called by the dispatch thread that issued I/O requests.
 * It does busy waiting until all the submitted IOs are completed.
 * 
 * @param worker_id The id of SPDK worker thread.
 */
void nvmf_poll_complete(int worker_id);

void nvmf_poll_complete_fast(uint32_t req_tot);

/**
 * @brief Lookup the completion status. It should not be used together with
 * nvmf_poll_complete(). The caller should call nvmf_release_worker() after
 * confirming the I/O is completed.
 * 
 * @param worker_id 
 * @return true 
 * @return false 
 */
bool nvmf_peek_completion(int worker_id);

/**
 * @brief Releasing worker. It is used in combination with nvmf_peek_completion().
 * 
 * @param worker_id 
 */
void nvmf_release_worker(int worker_id);

/**
 * @brief
 * 
 * @return uint64_t Return max IO requests in a qpair. Return 0 on error.
 */
uint64_t nvmf_get_max_io_size(void);

/**
 * @brief Allocate SPDK dma buffer.
 * 
 * @param qpair_id 
 * @return char* 
 */
char *nvmf_alloc_dma_buf(size_t size);

/**
 * @brief Register buffer to SPDK DMA region.
 * 
 * @param buf 
 * @param size 
 */
int nvmf_register_buf_to_spdk(void *buf, size_t size);
#endif
