#include <stdio.h>
#include <string.h>
#include "config.h"
#include "devfs.h"
#include "test_global.h"
#include "storage_engine.h"
#include <unistd.h>

#define BUF_SIZE (4096 * 100) // 40KB
#define IO_SIZE 4096 * 100
#define START_BLK_ADDR 322122549

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

#define ROUND 100

// int main(int argc, char **argv)
int main(void)
{
	struct storage_operations sops = storage_ops_nvmf;
	int rc, i;
	char write_buf[BUF_SIZE];
	char read_buf[BUF_SIZE];
	size_t io_size;
	struct se_config se_conf;

	io_size = ((size_t)IO_SIZE);

	init_devfs();

	printf("NVME TEST\n");

	memset(write_buf, 0, sizeof(write_buf));
	memset(read_buf, 0, sizeof(read_buf));

	set_buf_data(write_buf);

	printf("Init NVMe. Queue pair count : 1\n");

	set_nvmf_config(&se_conf.nvmf, g_devfs_conf.nvmf_ip_addr,
			g_devfs_conf.nvmf_port, g_devfs_conf.nvmf_subnqn,
			g_devfs_conf.spdk_max_io_requests_in_qpair);
	rc = sops.init(&se_conf, 1);

	if (rc != 0) {
		printf("Initialization failed.\n");
		return rc;
	}

	i = 0;
retry:
	printf("Write data.\n");
	sops.write(write_buf, START_BLK_ADDR, io_size, 0);
	sops.poll_complete(io_size, 0);

	printf("Read data.\n");
	sops.read(read_buf, START_BLK_ADDR, io_size, 0);
	sops.poll_complete(io_size, 0);

	if (check_buf_data(read_buf))
		printf("Test passed.\n");
	else {
		printf("Test failed. Data is corrupted.\n");
		return 1;
	}

	i++;
	printf("ROUND=%d\n", i);
	// sleep(5);

	if (i < ROUND)
		goto retry;

	printf("Clean up NVMe.\n");
	sops.exit();

	return 0;
}