#include <stdio.h>
#include <stdlib.h>
#include "config.h"

#define LOAD_CONFIG_STR(var) g_devfs_conf.var = getenv(#var);
#define LOAD_CONFIG_INT(var) g_devfs_conf.var = get_val_from_env(#var);
#define PRINT_CONFIG_STR(var)                                                  \
	do {                                                                   \
		printf(#var "=%s\n", g_devfs_conf.var);                        \
	} while (0)
#define PRINT_CONFIG_INT(var)                                                  \
	do {                                                                   \
		printf(#var "=%d\n", g_devfs_conf.var);                        \
	} while (0)

struct devfs_config g_devfs_conf = { 0 };

static int get_val_from_env(char *conf_name)
{
	return getenv(conf_name) ? atoi(getenv(conf_name)) : 0; // default is 0
}

void load_devfs_configs(void)
{
	LOAD_CONFIG_STR(pcie_nvme_addr);
	LOAD_CONFIG_STR(nvmf_ip_addr);
	LOAD_CONFIG_INT(nvmf_port);
	LOAD_CONFIG_STR(nvmf_subnqn);
	LOAD_CONFIG_STR(rpc_rdma_ip_addr);
	LOAD_CONFIG_INT(rpc_rdma_port);
	LOAD_CONFIG_INT(data_fetcher_rdma_port);
	LOAD_CONFIG_INT(data_fetcher_rdma_port_second);
	LOAD_CONFIG_INT(spdk_max_io_requests_in_qpair);
	LOAD_CONFIG_INT(nvmf_max_inflight_per_qpair);
	LOAD_CONFIG_INT(storage_engine_thread_num);
	LOAD_CONFIG_INT(rpc_thread_num);
}

void print_devfs_configs(void)
{
	printf("---- DevFS Configurations ----\n");
	PRINT_CONFIG_STR(pcie_nvme_addr);
	PRINT_CONFIG_STR(nvmf_ip_addr);
	PRINT_CONFIG_INT(nvmf_port);
	PRINT_CONFIG_STR(nvmf_subnqn);
	PRINT_CONFIG_STR(rpc_rdma_ip_addr);
	PRINT_CONFIG_INT(rpc_rdma_port);
	PRINT_CONFIG_INT(data_fetcher_rdma_port);
	PRINT_CONFIG_INT(data_fetcher_rdma_port_second);
	PRINT_CONFIG_INT(spdk_max_io_requests_in_qpair);
	PRINT_CONFIG_INT(nvmf_max_inflight_per_qpair);
	PRINT_CONFIG_INT(storage_engine_thread_num);
	PRINT_CONFIG_INT(rpc_thread_num);
	printf("--------------------------------------\n");
}
