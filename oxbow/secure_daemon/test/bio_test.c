#include <stdio.h>
#include <stdlib.h>
#include "config.h"
#include "test_global.h"
#include "log.h"
#include "bio.h"
#include "io_dispatcher.h"

#define BUF_SIZE (4096 * 16) // test

static unsigned long start;
// static int a = 0;

void set_buf_data(char *buf)
{
	// Make start random.
	srand(time(NULL));
	start = rand() % 10;

	log_info("Start character:%d", start);

	// Set write buffer.
	for (unsigned long i = 0; i < BUF_SIZE; i++) {
		buf[i] = '0' + ((i + start) % 10);

		// if (a < 10) {
		// 	a++;
		// 	log_info("buf[%lu] = %c", i, buf[i]);
		// }
	}
}

// Return 0 if there is no problem.
int check_buf_data(char *buf)
{
	for (unsigned long i = 0; i < BUF_SIZE; i++) {
		if (buf[i] != (char)('0' + ((i + start) % 10)))
			return -1;
	}
	return 0;
}

int main(void)
{
	struct bio_list *bl;
	char *write_buf, *read_buf;
	int ret, rc = 0;

	printf("BIO TEST\n");

	load_secure_daemon_configs();
	print_secure_daemon_configs();

	ret = init_io_dispatcher();
	if (ret < 0) {
		log_error("Failed to initialize IO Dispatcher.");
		return -1;
	}

	write_buf = calloc(1, BUF_SIZE);
	read_buf = calloc(1, BUF_SIZE);

	set_buf_data(write_buf);

	// Write.
	bl = alloc_bl();

	log_info("Do write.");
	ret = iod_add_io_req_to_bl(bl, write_buf, 1, BUF_SIZE);

	if (ret < 0) {
		log_error("Adding write I/O request to bio list failed.");
		free_bl(bl);
		rc = -1;
		goto err;
	}

	iod_do_write(bl);
	free_bl(bl);

	// Read;
	bl = alloc_bl();

	log_info("Do read.");
	ret = iod_add_io_req_to_bl(bl, read_buf, 1, BUF_SIZE);

	if (ret < 0) {
		log_error("Adding read I/O request to bio list failed.");
		free_bl(bl);
		rc = -1;
		goto err;
	}

	iod_do_read(bl);
	free_bl(bl);

	check_buf_data(read_buf);

	log_info("Data is valid.");

err:
	free(write_buf);
	free(read_buf);

	return rc;
}
