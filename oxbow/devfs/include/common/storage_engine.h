#ifndef _STORAGE_ENGINE_H_
#define _STORAGE_ENGINE_H_

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdatomic.h>
#include "oxbow.h"
#include "bio.h"
#include "sjd.h"
#include "list.h"
#include "thpool.h"
enum storage_engine_type { SE_NVME = 1, SE_NVMF };

struct nvme_config {
	char pcie_addr[50]; // TODO: Is it sufficient?
	uint32_t num_io_requests; // The max number of the io requests in qpair.
};

struct nvmf_config {
	char target_ip_addr[16];
	int port;
	char subnqn_name[128]; // TODO: Is it sufficient?
	uint32_t num_io_requests; // The max number of the io requests in qpair.
	int num_qpair; // The number of qpairs == # of io threads. == # of rpc handler threads.
	threadpool worker_thpool; // Created thread pool is returned.
};

struct se_config {
	union {
		struct nvme_config nvme;
		struct nvmf_config nvmf;
	};
};

struct storage_operations {
	int (*init)(struct se_config *config, int num_qpair);
	size_t (*read)(char *buf, baddr_t baddr, size_t io_size, int qpair_id);
	size_t (*write)(char *buf, baddr_t baddr, size_t io_size, int qpair_id);
	size_t (*write_nocopy)(char *buf, baddr_t baddr, size_t io_size,
			       int qpair_id);
	char *(*get_buffer)(int qpair_id);
	void (*poll_complete)(size_t submit_size, int qpair_id);
	void (*poll_complete2)(size_t submit_size, int qpair_id);
	// void (*nvmf_poll_complete)(int worker_id);
	void (*nvmf_poll_complete_fast)(uint32_t req_tot);
	bool (*nvmf_peek_completion)(int worker_id);
	void (*nvmf_release_worker)(int worker_id);
	void (*exit)(void);
	uint64_t (*get_max_io_size)(void);
	char *(*alloc_dma_buffer)(size_t);
	int (*register_buf_to_spdk)(void *buf, size_t size);
	size_t (*write_nvmf)(char *buf, baddr_t baddr, size_t io_size,
			     int qpair_id);
	// int (*dispatch_io)(int df_id, struct bio_list *bl, bool is_read,
	// 		   char *custom_buf);
	uint32_t (*dispatch_io_fast)(struct bio_list *bl, bool is_read,
				     char *custom_buf);
};

extern struct storage_operations storage_ops_nvme;
extern struct storage_operations storage_ops_nvmf;
extern struct storage_operations storage_ops_nvmf2;

/* Interface to use storage engine. */
void set_nvme_config(struct nvme_config *conf, const char *pcie_nvme_addr,
		     uint32_t num_io_requests);
void set_nvmf_config(struct nvmf_config *conf, const char *target_ip_addr,
		     const int port, const char *subnqn_name,
		     uint32_t num_io_requests);
int init_storage_engine(enum storage_engine_type type, struct se_config *config,
			int thread_num);
uint64_t se_get_max_io_size(void);
void se_write(struct bio_list *bl);
void se_write_async(struct bio_list *bl);
void se_read(struct bio_list *bl);
void se_read_async(struct bio_list *bl);
int se_is_completed(struct bio_list *bl);
int exit_storage_engine(void);

static inline size_t spdk_align_roundup(size_t x, uint64_t align)
{
	return ((x + (align - 1)) & ~(align - 1));
}

char *se_alloc_dma_buffer(size_t size);
int se_register_buf_to_spdk(void *buf, size_t size);
uint32_t se_dispatch_io_async(struct bio_list *bl, bool is_read);
int se_dispatch_io_sync(struct bio_list *bl, bool is_read);
int se_request_dispatch_io_sync(struct bio_list *bl, bool is_read);
uint32_t se_dispatch_io_nocopy_async(struct bio_list *bl, bool is_read,
				     char *custom_buf);
int se_dispatch_io_nocopy_sync(struct bio_list *bl, bool is_read,
			       char *custom_buf);
void se_nvmf_poll_complete(uint32_t req_cnt);
int se_nvme_submit_io(struct ra_bio *, int qpair_id);

#endif
