#ifndef _CONFIG_H_
#define _CONFIG_H_

enum journal_mode { JOURNAL_NONE = 1, JOURNAL_DATA = 2, JOURNAL_METADATA = 3 };

/* Maximum number of NVMe VFs (virtual functions) that can be probed
 * concurrently to expand total IO queue count beyond a single VF's
 * hardware cap (SR-IOV VFs typically expose <= 8 IO queues each).
 */
#define MAX_NVME_VFS 4

struct secure_daemon_config {
	/* Raw env string, comma/space-separated list of PCIe BDFs.
	 * Example: "0000:d8:00.1,0000:d8:00.2"
	 */
	char *pcie_nvme_addr;
	/* Parsed BDFs. pcie_nvme_nr_vfs is the valid count in the list. */
	char *pcie_nvme_addr_list[MAX_NVME_VFS];
	int pcie_nvme_nr_vfs;
	char *rpc_rdma_ip_addr;
	int rpc_rdma_port;
	int data_fetcher_rdma_port;
	int data_fetcher_rdma_port_second;
	// int spdk_max_io_requests_in_qpair;
	int storage_engine_thread_num;
	/*
	 * Dedicated D-2 read worker pool size. Only used when
	 * OXBOW_RD_INLINE_SUBMIT is defined. Total qpair pool size =
	 * storage_engine_thread_num + read_worker_thread_num; both NUMA
	 * pinning and MAX_IO_THREAD_NR cap apply to the sum.
	 *
	 * Reasonable range: [4, 16]. Keep sum <= NUMA1 core count (16 on
	 * Libra06) to stay NUMA-local.
	 */
	int read_worker_thread_num;
	int rpc_rdma_thread_num;
	int rpc_shmem_thread_num;
	int snapshot_thread_num;
	char *filesystem;
	int bg_journaling; // FIXME: Duplicate with VM_ENV_NO_DEVFS.
	enum journal_mode journal_mode;
};

extern struct secure_daemon_config g_sd_conf;

void load_secure_daemon_configs(void);
int validate_secure_daemon_configs(void);
void print_secure_daemon_configs(void);

#endif
