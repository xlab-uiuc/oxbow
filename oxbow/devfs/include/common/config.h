#ifndef _COMMON_CONFIG_H_
#define _COMMON_CONFIG_H_

struct common_config {
	char *pcie_nvme_addr;
	char *rpc_rdma_ip_addr;
	int rpc_rdma_port;
	int storage_engine_thread_num;
	int rpc_rdma_thread_num;
	int rpc_shmem_thread_num;
	char *filesystem;
	int spdk_max_io_requests_in_qpair;
};

extern struct common_config g_conf;

void load_common_configs(void);
void print_common_configs(void);

#endif
