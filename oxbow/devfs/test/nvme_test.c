#include <stdio.h>
#include <string.h>
#include "config.h"
#include "devfs.h"
#include "test_global.h"
#include "storage_engine.h"

#define LOOP_MODE 1
#ifdef LOOP_MODE
#include <unistd.h>
#endif

#define BUF_SIZE (4096 * 10) // 40KB

void set_buf_data(char *buf)
{
	// Set write buffer.
	for (unsigned long i = 0; i < BUF_SIZE; i++)
		buf[i] = '0' + (i % 10);
}

// Return 1 if there is no problem.
int check_buf_data(char *buf)
{
	for (long i = 0; i < BUF_SIZE; i++) {
		if (buf[i] != '0' + (i % 10))
			return 0;
	}
	return 1;
}

// int main(int argc, char **argv)
int main(void)
{
	struct storage_operations sops = storage_ops_nvme;
	int rc;
	char write_buf[BUF_SIZE];
	char read_buf[BUF_SIZE];
	struct se_config se_conf;
#ifdef LOOP_MODE
	int i = 0;
#endif

	init_devfs();

	printf("NVME TEST\n");

	memset(write_buf, 0, sizeof(write_buf));
	memset(read_buf, 0, sizeof(read_buf));

	set_buf_data(write_buf);

	printf("Init NVMe. Queue pair count : 1\n");
	set_nvme_config(&se_conf.nvme, g_devfs_conf.pcie_nvme_addr,
			g_devfs_conf.spdk_max_io_requests_in_qpair);
	rc = sops.init(&se_conf, 1);

	if (rc != 0) {
		printf("Initialization failed.\n");
		return rc;
	}

retry:

	printf("Write data.\n");
	sops.write(write_buf, 1, sizeof(write_buf), 0);
	sops.poll_complete(1, 0);

	printf("Read data.\n");
	sops.read(read_buf, 1, sizeof(read_buf), 0);
	sops.poll_complete(1, 0);

	if (check_buf_data(read_buf))
		printf("Test passed.\n");
	else {
		printf("Test failed. Data is corrupted.\n");
		return 1;
	}
#ifdef LOOP_MODE
	printf("Loop count : %d\n", i++);
	sleep(1);
	goto retry;
#endif

	printf("Clean up NVMe.\n");
	sops.exit();

	return 0;
}