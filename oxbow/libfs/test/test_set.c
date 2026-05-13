#include "test_set.h"
#include <stdlib.h>
#include <errno.h>

#define OXBOW_PREFIX "/oxbow"
#define TEST_FILE_NAME "/oxbow/test_name_01"
#define TEST_DIR_NAME "/oxbow/test_dir"
#define BUF_SIZE 4096

/* To check functionality of each funcitons */
int file_basic_operation_test(void)
{
	char *read_buffer = NULL;
	char write_buffer[20] = "Hello world\n";
	char overwrite_buffer[20] = "Bye Jaehwan Lee\n";
	int fd, success, ret = -1;

	success = 0;

	read_buffer = malloc(BUF_SIZE);
	if (read_buffer == NULL) {
		perror("malloc faile");
		return ret;
	}
	memset(read_buffer, 0, BUF_SIZE);

	/* create test*/
	printf("[%s] create(%s) start \n", __func__, TEST_FILE_NAME);
	fd = open(TEST_FILE_NAME, O_RDWR | O_CREAT, 0600);
	if (fd < 0) {
		perror("open with O_CREAT");
		goto free;
	}
	printf("[%s] create(%s) done \n", __func__, TEST_FILE_NAME);

	/* write test*/
	printf("[%s] write(%s) %lu bytes\n", __func__, write_buffer,
	       strlen(write_buffer) + 1);
	ret = write(fd, write_buffer, strlen(write_buffer) + 1);
	if (ret < 0 || ret != (int)strlen(write_buffer) + 1) {
		perror("write failed");
		goto free;
	}
	printf("[%s] write %d bytes done \n", __func__, ret);

	printf("[%s] fsync(%d) start \n", __func__, fd);
	ret = fsync(fd);
	if (ret < 0) {
		perror("fsync failed");
		goto free;
	}
	printf("[%s] fsync(%d) done \n", __func__, fd);

	printf("[%s] close(%d) start \n", __func__, fd);
	ret = close(fd);
	if (ret < 0) {
		perror("close failed");
		goto free;
	}
	printf("[%s] close(%d) done \n", __func__, fd);

	// ret = mkdir(TEST_DIR_NAME, 0777);
	// if (ret < 0 && ret != -EEXIST) {
	// 	perror("mkdir failed");
	// 	goto free;
	// }
	// printf("\n[file_basic_operation_test]\n[mkdir] %s\n", TEST_DIR_NAME);

	printf("[%s] open(%s) start \n", __func__, TEST_FILE_NAME);
	fd = open(TEST_FILE_NAME, O_RDWR);
	if (fd < 0) {
		perror("open failed");
		goto free;
	}
	printf("[%s] open(%s) done \n", __func__, TEST_FILE_NAME);

	// basic read
	printf("[%s] read start \n", __func__);
	ret = read(fd, read_buffer, BUF_SIZE);
	if (ret < 0) {
		perror("read failed");
		goto free;
	}
	printf("[%s] read(%s) done (%d bytes) \n", __func__, read_buffer, ret);

	ret = lseek(fd, 0, SEEK_SET);
	if (ret < 0) {
		perror("lseek failed");
		goto free;
	}

	ret = write(fd, overwrite_buffer, strlen(overwrite_buffer) + 1);
	if (ret < 0) {
		perror("overwrite failed");
		goto free;
	}

	printf("[%s] pread start \n", __func__);
	memset(read_buffer, 0, BUF_SIZE);
	ret = pread(fd, read_buffer, BUF_SIZE, 0);
	if (ret < 0) {
		perror("pread failed");
		goto free;
	}
	printf("[%s] pread(%s) done (%d bytes) \n", __func__, read_buffer, ret);

	printf("[%s] close(%d) start \n", __func__, fd);
	ret = close(fd);
	if (ret < 0) {
		perror("close failed");
		goto free;
	}
	printf("[%s] close ret(%d) \n", __func__, ret);

	// printf("\n[file_basic_operation_test]\n[SLEEP] 1 second\n\n");
	// sleep(1);

	// printf("\n[file_basic_operation_test]\n[SYNC]\n\n");
	// sync();

	printf("[%s] unlink(%s) \n", __func__, TEST_FILE_NAME);
	ret = unlink(TEST_FILE_NAME);
	if (ret < 0) {
		perror("unlink failed");
		goto free;
	}
	printf("[%s] unlink ret(%d) \n", __func__, ret);

	/* make it again */
	printf("[%s] create(%s) start \n", __func__, TEST_FILE_NAME);
	fd = open(TEST_FILE_NAME, O_RDWR | O_CREAT, 0600);
	if (fd < 0) {
		perror("open with O_CREAT");
		goto free;
	}
	printf("[%s] create(%s) done \n", __func__, TEST_FILE_NAME);

	// basic read
	printf("[%s] read start \n", __func__);
	ret = read(fd, read_buffer, BUF_SIZE);
	if (ret < 0) {
		perror("read failed");
		goto free;
	}
	printf("[%s] read(%s) done (%d bytes) \n", __func__, read_buffer, ret);
	// fd = open(TEST_FILE_NAME, O_RDWR);
	// if (fd < 0)
	// 	perror("open failed");
	// else {
	// 	printf("Open must be failed but get %d", fd);
	// 	goto free;
	// }
	// printf("\n[file_basic_operation_test]\n[open] %s\n", TEST_FILE_NAME);

	/* success !*/
	success = 1;

free:
	free(read_buffer);
	printf("\n[BASIC test] result %s\n", success ? "SUCCESS" : "FAILED");

	return ret;
}

int open_latency_test(int times)
{
	struct time_stats stats;
	int iter, ret, fd;

	fd = open("/oxbow/test_open_lat", O_RDWR | O_CREAT, 0600);
	if (fd < 0) {
		perror("open with O_CREAT");
		return -1;
	}
	ret = close(fd);
	if (ret < 0) {
		perror("close error");
		return 1;
	}
	sleep(1);

	iter = 0;
	time_stats_init(&stats, times);
	while (iter < times) {
		time_stats_start(&stats);
		fd = open("/oxbow/test_open_lat", O_RDWR, 0600);
		if (fd < 0) {
			perror("open with O_CREAT");
			return -1;
		};
		time_stats_stop(&stats);

		ret = close(fd);
		if (ret < 0) {
			perror("close error");
			return 1;
		}
		printf("iter...%d/%d\n", iter, times);
		sleep(1);
		iter++;
	}
	time_stats_print(&stats, "[open_latency_test]");
	return ret;
}

int sq_read_test(int io_size)
{
	struct time_stats stats;
	char buffer[PAGE_SIZE] = { 0 };
	size_t pos;
	int ret, fd;

	pos = 0;
	fd = open("/oxbow/testfile0-0-0", O_RDWR, 0600);
	if (fd < 0) {
		perror("open");
		return -1;
	}
	sleep(1);

	time_stats_init(&stats, TEST_FILE_SIZE / PAGE_SIZE);

	printf("test start with size : %lu\n", TEST_FILE_SIZE);
	// time_stats_start(&stats);
	while (pos < TEST_FILE_SIZE) {
		time_stats_start(&stats);
		ret = read(fd, buffer, io_size);
		time_stats_stop(&stats);
		if (ret < 0) {
			perror("read");
			return -1;
		}
		pos += io_size;
	}
	// time_stats_stop(&stats);

	ret = close(fd);
	if (ret < 0) {
		perror("close error");
		return 1;
	}
	time_stats_print(&stats, "[sequential_read_throughput_test]");
	return ret;
}

int sq_write_test(int io_size)
{
	struct time_stats stats;
	char *buffer = NULL;
	size_t pos;
	int ret, fd;

	buffer = malloc(io_size);

	pos = 0;
	fd = open("/oxbow/testfile0-0-0", O_RDWR | O_CREAT, 0600);
	if (fd < 0) {
		perror("open");
		return -1;
	}
	sleep(1);

	time_stats_init(&stats, TEST_FILE_SIZE / PAGE_SIZE);

	printf("test start file max size %lu MB\n", TEST_FILE_SIZE >> 10);

	time_stats_start(&stats);
	while (pos < (TEST_FILE_SIZE)) {
		ret = write(fd, buffer, io_size);
		if (ret < 0) {
			perror("write");
			return -1;
		}
		pos += io_size;

		// printf("@@@@ iter %lu / %lu \n", pos / PAGE_SIZE,
		//        SIMPLEFS_FILE_MAX / (unsigned long)PAGE_SIZE);
	}
	fsync(fd);
	time_stats_stop(&stats);
	ret = close(fd);
	if (ret < 0) {
		perror("close error");
		return 1;
	}
	// ret = unlink("/oxbow/test_write_thr");
	// if (ret < 0) {
	// 	perror("unlink error");
	// 	return 1;
	// }
	time_stats_print(&stats, "[sequential_write_throughput_test]");
	free(buffer);
	return ret;
}

int fsync_lat_test(int io_size)
{
	struct time_stats stats;
	char *buffer = NULL;
	size_t pos;
	int ret, fd;

	buffer = malloc(io_size);

	pos = 0;
	fd = open("/oxbow/testfile0-0-0", O_RDWR | O_CREAT, 0600);
	if (fd < 0) {
		perror("open");
		return -1;
	}
	sleep(1);

	time_stats_init(&stats, TEST_FILE_SIZE / PAGE_SIZE / 10);

	printf("test start file max size %lu MB\n", TEST_FILE_SIZE >> 10);

	while (pos < (TEST_FILE_SIZE / 10)) {
		ret = write(fd, buffer, io_size);
		time_stats_start(&stats);
		fsync(fd);
		time_stats_stop(&stats);
		if (ret < 0) {
			perror("write");
			return -1;
		}
		pos += io_size;

		printf("@@@@ iter %lu / %lu \n", pos / PAGE_SIZE,
		       SEFS_FILE_MAX / (unsigned long)PAGE_SIZE / 10UL);
	}
	ret = close(fd);
	if (ret < 0) {
		perror("close error");
		return 1;
	}
	// ret = unlink("/oxbow/test_write_thr");
	// if (ret < 0) {
	// 	perror("unlink error");
	// 	return 1;
	// }
	time_stats_print(&stats, "[sequential_write_throughput_test]");
	free(buffer);
	return ret;
}

int mkdir_test(void)
{
	int ret;

	ret = mkdir("/oxbow/a/b/c", 0777);
	if (ret < 0 && ret != -EEXIST) {
		perror("mkdir failed");
	}

	return ret;
}