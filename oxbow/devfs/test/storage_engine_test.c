#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "config.h"
#include "devfs.h"
#include "test_global.h"
#include "storage_engine.h"
#include "log.h"
#include <unistd.h>

// #define BUF_SIZE (1024 * 1024 * 512) // 512MB
#define BUF_SIZE (4096 * 16) // test
#define NVME_QPAIR_NUM 8

static enum storage_engine_type g_se_type = 0;

void set_buf_data(char *buf)
{
	// Set write buffer.
	for (unsigned long i = 0; i < BUF_SIZE; i++)
		buf[i] = '0' + (i % 10);
}

// Return 1 if there is no problem.
int check_buf_data(char *buf)
{
	for (unsigned long i = 0; i < BUF_SIZE; i++) {
		if (buf[i] != (char)('0' + (i % 10)))
			return 0;
	}
	return 1;
}

static void construct_bvec(struct bio *bio, uint64_t bio_size,
			   const uint64_t bvec_size, char *data_buf)
{
	int total_bvec_cnt, i;
	struct bio_vec *bvec;

	if (bio_size % bvec_size != 0) {
		printf("(Warn) bio_size(%lu) cannot be divided by bvec_size(%lu).\n",
		       bio_size, bvec_size);
	}

	total_bvec_cnt = bio_size / bvec_size;

	log_info("total_bvec_cnt(per bio)=%d", total_bvec_cnt);

	// Alloc all bio vectors.
	bio->bi_io_vec = calloc(1, sizeof(struct bio_vec) * total_bvec_cnt);

	for (i = 0; i < total_bvec_cnt; i++) {
		bvec = &bio->bi_io_vec[i];
		bvec->bv_buf = data_buf + bvec_size * i;
		bvec->bv_len = bvec_size;
		bio->total_size += bvec_size;
	}

	bio->bi_vcnt = total_bvec_cnt;
}

static void construct_bio_list(struct bio_list *bl, const uint64_t bio_size,
			       const uint64_t bvec_size, char *data_buf)
{
	int total_bio_cnt;
	int i, size;
	struct bio *bio;
	baddr_t blk_no;
	char *buf;

	if (BUF_SIZE % bio_size != 0) {
		printf("(Warn) buffer size(%lu) cannot be divided by bio_size(%lu).\n",
		       (uint64_t)BUF_SIZE, bio_size);
	}

	total_bio_cnt = BUF_SIZE / bio_size;
	log_info("total_bio_cnt=%d", total_bio_cnt);

	bio_list_init(bl);

	// Alloc all BIOs.
	size = sizeof(struct bio) * total_bio_cnt;
	bio = calloc(1, size);
	// printf("bio:0x%lx size:%lu end:%lx\n", bio, size, ((char *)bio) + size);

	buf = data_buf;
	blk_no = 322122549; // Start from the block number 1.

	// For all BIOs
	for (i = 0; i < total_bio_cnt; i++) {
		construct_bvec(bio, bio_size, bvec_size, buf);

		bio->bi_iter.bi_bvec_done = 0;
		bio->bi_iter.bi_blk_no = blk_no;

		bio_list_add(bl, bio);

		// Advance pointer.
		buf += bio_size;
		blk_no += bio_size >> OXBOW_BLOCK_SIZE_SHIFT;
		bio++; // Point to the next bio.
	}
}

static void free_bio_list(struct bio_list *bl)
{
	struct bio *bio;

	// For all bio vectors.
	bio_list_for_each (bio, bl) {
		free(bio->bi_io_vec);
	}

	// Free all BIOs.
	free(bio_list_peek(bl));
}

static void usage(char *argv[])
{
	printf("Usage: %s [nvme|nvmf]", argv[0]);
}

static void check_opt(int argc, char *argv[])
{
	if (argc != 2)
		goto invalid;
	else if (strcmp(argv[1], "nvme") == 0)
		g_se_type = SE_NVME;
	else if (strcmp(argv[1], "nvmf") == 0)
		g_se_type = SE_NVMF;
	else
		goto invalid;

	return;

invalid:
	usage(argv);
}

int main(int argc, char *argv[])
{
	int rc;
	char *write_buf;
	char *read_buf;
	struct se_config se_conf;
	struct bio_list w_bl, r_bl;
	uint64_t bio_size, bvec_size;
	int i;

	check_opt(argc, argv);

	init_devfs();

	printf("STORAGE ENGINE TEST\n");

	log_info("Init Storage Engine.");

	if (g_se_type == SE_NVME) {
		set_nvme_config(&se_conf.nvme, g_devfs_conf.pcie_nvme_addr,
				g_devfs_conf.spdk_max_io_requests_in_qpair);
		rc = init_storage_engine(SE_NVME, &se_conf,
					 g_devfs_conf.rpc_thread_num);

	} else if (g_se_type == SE_NVMF) {
		set_nvmf_config(&se_conf.nvmf, g_devfs_conf.nvmf_ip_addr,
				g_devfs_conf.nvmf_port,
				g_devfs_conf.nvmf_subnqn,
				g_devfs_conf.spdk_max_io_requests_in_qpair);
		rc = init_storage_engine(SE_NVMF, &se_conf,
					 g_devfs_conf.rpc_thread_num);

	} else {
		log_error("Unknown storage engine type: %d", g_se_type);
		return -1;
	}

	if (rc != 0) {
		printf("Storage engine init failed.\n");
		return rc;
	}

	// Init buffer.
	write_buf = calloc(1, BUF_SIZE);
	read_buf = calloc(1, BUF_SIZE);

	log_debug("write_buf=0x%lx read_buf=0x%lx", write_buf, read_buf);

	set_buf_data(write_buf);

	// Set bio and bvec size.
	// BIOs are processed by multiple threads.
	// BVECs are processed by a single thread asynchronously.
	bio_size = 4096 * 4; // 4KB * 4
	bvec_size = 4096; // 4KB
	printf("bio_size=%lu bvec_size=%lu\n", bio_size, bvec_size);

	i = 0;
retry:

	// Construct bio list write.
	construct_bio_list(&w_bl, bio_size, bvec_size, write_buf);
	log_info("Construct write bio_list done.");

	se_write(&w_bl);
	log_info("Write done.");

	free_bio_list(&w_bl);

	// Construct bio list for read.
	construct_bio_list(&r_bl, bio_size, bvec_size, read_buf);
	log_info("Construct read bio_list done.");

	se_read(&r_bl);
	log_info("Read done.");

	free_bio_list(&r_bl);

	if (check_buf_data(read_buf))
		printf("Test passed.\n");
	else {
		printf("Test failed. Data is corrupted.\n");
		return 1;
	}

	if (i < 100) {
		i++;
		sleep(5);
		goto retry;
	}

	free(write_buf);
	free(read_buf);

	log_info("Clean up NVMe.");
	exit_storage_engine();

	return 0;
}
