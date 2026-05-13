#ifndef _SE_NVME_H_
#define _SE_NVME_H_

#include <stdio.h>
#include "oxbow.h"
#include "storage_engine.h"

/* NVMe storage engine interface */
int nvme_init(struct se_config *se_config, int num_qpair);
void nvme_exit(void);

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
size_t nvme_read(char *buf, baddr_t baddr, size_t io_size, int qpair_id);

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
size_t nvme_write(char *buf, baddr_t baddr, size_t io_size, int qpair_id);

size_t nvme_write_nocopy(char *buf, baddr_t baddr, size_t io_size,
			 int qpair_id);

char *nvme_get_buffer(int qpair_id);

/**
 * @brief This function should be called by the same thread that issued I/O requests.
 * It does busy-waiting until the exact number of completions occurs.
 * 
 * @param submit_size The sized of submitted I/O requests in byte.
 * @param qpair_id
 */
void nvme_poll_complete(size_t submit_size, int qpair_id);

void nvme_poll_complete2(size_t submit_cnt, int qpair_id);

/**
 * @brief
 * 
 * @return uint64_t Return max IO requests in a qpair. Return 0 on error.
 */
uint64_t nvme_get_max_io_size(void);

#endif
