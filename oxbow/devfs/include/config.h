#ifndef _CONFIG_H_
#define _CONFIG_H_

struct devfs_config {
	char *pcie_nvme_addr;
	char *nvmf_ip_addr;
	int nvmf_port;
	char *nvmf_subnqn;
	char *rpc_rdma_ip_addr;
	int rpc_rdma_port;
	int data_fetcher_rdma_port;
	int data_fetcher_rdma_port_second;
	int spdk_max_io_requests_in_qpair;
	int nvmf_max_inflight_per_qpair;
	int rpc_thread_num;
};

extern struct devfs_config g_devfs_conf;

void load_devfs_configs(void);
void print_devfs_configs(void);

#endif
