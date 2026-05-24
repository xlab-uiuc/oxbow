#include "common/config.h"
#include <stdio.h>
#include <stdlib.h>

#define LOAD_CONFIG_STR(var) g_conf.var = getenv(#var);
#define LOAD_CONFIG_INT(var) g_conf.var = get_val_from_env(#var);
#define PRINT_CONFIG_STR(var)                                                  \
	do {                                                                   \
		printf(#var "=%s\n", g_conf.var);                              \
	} while (0)
#define PRINT_CONFIG_INT(var)                                                  \
	do {                                                                   \
		printf(#var "=%d\n", g_conf.var);                              \
	} while (0)

struct common_config g_conf = { 0 };

static int get_val_from_env(char *conf_name)
{
	return getenv(conf_name) ? atoi(getenv(conf_name)) : 0; // default is 0
}

void load_common_configs(void)
{
	LOAD_CONFIG_STR(pcie_nvme_addr);
	LOAD_CONFIG_STR(rpc_rdma_ip_addr);
	LOAD_CONFIG_INT(rpc_rdma_port);
	LOAD_CONFIG_INT(spdk_max_io_requests_in_qpair);
	LOAD_CONFIG_INT(rpc_rdma_thread_num);
	LOAD_CONFIG_INT(rpc_shmem_thread_num);
	LOAD_CONFIG_STR(filesystem);
}

void print_common_configs(void)
{
	printf("---- Common Configurations ----\n");
	PRINT_CONFIG_STR(pcie_nvme_addr);
	PRINT_CONFIG_STR(rpc_rdma_ip_addr);
	PRINT_CONFIG_INT(rpc_rdma_port);
	PRINT_CONFIG_INT(spdk_max_io_requests_in_qpair);
	PRINT_CONFIG_INT(rpc_rdma_thread_num);
	PRINT_CONFIG_INT(rpc_shmem_thread_num);
	PRINT_CONFIG_STR(filesystem);
	printf("--------------------------------------\n");
}
