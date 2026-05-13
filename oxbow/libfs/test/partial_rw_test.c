#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <getopt.h>

#include "test_global.h"

enum test_mode {
	MODE_PARTIAL = 0,
	MODE_ALIGNED,
	MODE_BOTH,
};

enum op_mode {
	OP_WRITE = 0,
	OP_READ,
	OP_RW,
};

static void print_usage(const char *prog)
{
	printf("Usage: %s [--mode partial|aligned|both] [--op write|read|rw]\n",
	       prog);
	printf("  --mode|-m  Test mode (default: both)\n");
	printf("  --op|-o    Operation: write, read, or rw (default: rw)\n");
}

static int do_one_test(const char *file_name, size_t size, enum op_mode op)
{
	int fd;
	ssize_t ret;
	char *wbuf;
	char *rbuf;
	char path[4096];

	memset(path, 0, sizeof(path));
	snprintf(path, sizeof(path) - 1, "%s/%s", TEST_DIR, file_name);

	printf("[partial_rw_test] file=%s size=%zu op=%d\n", path, size, op);

	/* Allocate buffers */
	wbuf = (char *)malloc(size);
	rbuf = (char *)malloc(size);
	if (!wbuf || !rbuf) {
		fprintf(stderr, "malloc failed\n");
		free(wbuf);
		free(rbuf);
		return -1;
	}

	/* Fill write buffer with an easy-to-recognize repeating ASCII pattern */
	{
		static const char pattern[] = "OXBOW1234";
		size_t pat_len = sizeof(pattern) - 1;

		for (size_t i = 0; i < size; i++)
			wbuf[i] = pattern[i % pat_len];
	}

	/* Ensure test directory exists if it is not the root mount path */
	if (strcmp(TEST_DIR, "/oxbow") != 0 &&
	    mkdir(TEST_DIR, 0700) < 0 && errno != EEXIST) {
		perror("mkdir TEST_DIR");
		free(wbuf);
		free(rbuf);
		return -1;
	}

	/* Open and write if requested */
	if (op == OP_WRITE || op == OP_RW) {
		fd = open(path, O_CREAT | O_TRUNC | O_RDWR, 0600);
		if (fd < 0) {
			perror("open for write");
			free(wbuf);
			free(rbuf);
			return -1;
		}

		ret = write(fd, wbuf, size);
		if (ret < 0) {
			perror("write");
			close(fd);
			free(wbuf);
			free(rbuf);
			return -1;
		}
		if ((size_t)ret != size) {
			fprintf(stderr, "short write: %zd vs %zu\n", ret, size);
			close(fd);
			free(wbuf);
			free(rbuf);
			return -1;
		}

		if (fsync(fd) < 0) {
			perror("fsync");
			close(fd);
			free(wbuf);
			free(rbuf);
			return -1;
		}

		if (close(fd) < 0) {
			perror("close after write");
			free(wbuf);
			free(rbuf);
			return -1;
		}
	}

	/* Re-open and read/verify if requested */
	if (op == OP_READ || op == OP_RW) {
		fd = open(path, O_RDONLY);
		if (fd < 0) {
			perror("open for read");
			free(wbuf);
			free(rbuf);
			return -1;
		}

		ret = read(fd, rbuf, size);
		if (ret < 0) {
			perror("read");
			close(fd);
			free(wbuf);
			free(rbuf);
			return -1;
		}
		if ((size_t)ret != size) {
			fprintf(stderr, "short read: %zd vs %zu\n", ret, size);
			close(fd);
			free(wbuf);
			free(rbuf);
			return -1;
		}

		if (close(fd) < 0) {
			perror("close after read");
			free(wbuf);
			free(rbuf);
			return -1;
		}

		/* Compare */
		if (memcmp(wbuf, rbuf, size) != 0) {
			size_t i;

			fprintf(stderr,
				"\n============================================================\n");
			fprintf(stderr,
				"[partial_rw_test] DATA MISMATCH\n");
			fprintf(stderr,
				"  file : %s\n"
				"  size : %zu (0x%zx)\n",
				path, size, size);

			for (i = 0; i < size; i++) {
				if (wbuf[i] != rbuf[i]) {
					size_t from_end = size - 1 - i;

					fprintf(stderr,
						"  first mismatch at offset %zu (0x%zx)\n",
						i, i);
					fprintf(stderr,
						"    distance from end: %zu bytes\n",
						from_end);
					fprintf(stderr,
						"    written: 0x%02x  read: 0x%02x\n",
						(unsigned char)wbuf[i],
						(unsigned char)rbuf[i]);

					/* Show a small window around the mismatch */
					{
						size_t start = (i > 8) ? i - 8 : 0;
						size_t end = (i + 8 < size) ? i + 8 : size - 1;
						size_t pos;

						fprintf(stderr,
							"    context (written vs read):\n");
						for (pos = start; pos <= end; pos++) {
							fprintf(stderr,
								"      [%zu]%s 0x%02x  / 0x%02x\n",
								pos,
								(pos == i) ? " *" : "  ",
								(unsigned char)wbuf[pos],
								(unsigned char)rbuf[pos]);
						}
					}

					break;
				}
			}

			fprintf(stderr,
				"============================================================\n\n");

			free(wbuf);
			free(rbuf);
			return -1;
		}
	}

	printf("[partial_rw_test] OK for file=%s size=%zu\n", path, size);

	free(wbuf);
	free(rbuf);
	return 0;
}

int main(int argc, char **argv)
{
	/* Sizes chosen to exercise partial-block and aligned writes */
	const size_t partial_sizes[] = {
		16,
		4096 + 16,
		2 * 4096 + 16,
	};
	const size_t aligned_sizes[] = {
		4096,
		// 2 * 4096,
		// 3 * 4096,
	};
	const size_t n_partial_sizes = sizeof(partial_sizes) /
				       sizeof(partial_sizes[0]);
	const size_t n_aligned_sizes = sizeof(aligned_sizes) /
				       sizeof(aligned_sizes[0]);
	enum test_mode mode = MODE_BOTH;
	enum op_mode op = OP_RW;
	int opt;
	int opt_index = 0;
	const struct option long_opts[] = {
		{ "mode", required_argument, 0, 'm' },
		{ "op", required_argument, 0, 'o' },
		{ "help", no_argument, 0, 'h' },
		{ 0, 0, 0, 0 }
	};
	int ret = 0;

	while ((opt = getopt_long(argc, argv, "m:o:h", long_opts, &opt_index)) !=
	       -1) {
		switch (opt) {
		case 'm':
			if (strcmp(optarg, "partial") == 0)
				mode = MODE_PARTIAL;
			else if (strcmp(optarg, "aligned") == 0)
				mode = MODE_ALIGNED;
			else if (strcmp(optarg, "both") == 0)
				mode = MODE_BOTH;
			else {
				fprintf(stderr, "Unknown mode: %s\n", optarg);
				print_usage(argv[0]);
				return 1;
			}
			break;
		case 'o':
			if (strcmp(optarg, "write") == 0)
				op = OP_WRITE;
			else if (strcmp(optarg, "read") == 0)
				op = OP_READ;
			else if (strcmp(optarg, "rw") == 0)
				op = OP_RW;
			else {
				fprintf(stderr, "Unknown op: %s\n", optarg);
				print_usage(argv[0]);
				return 1;
			}
			break;
		case 'h':
		default:
			print_usage(argv[0]);
			return 1;
		}
	}

	if (mode == MODE_PARTIAL || mode == MODE_BOTH) {
		for (size_t i = 0; i < n_partial_sizes; i++) {
			char name[64];

			memset(name, 0, sizeof(name));
			snprintf(name, sizeof(name) - 1, "partial_rw_%zu.dat",
				 partial_sizes[i]);

			if (do_one_test(name, partial_sizes[i], op) != 0) {
				fprintf(stderr,
					"[partial_rw_test] FAILED (partial) for size=%zu\n",
					partial_sizes[i]);
				ret = 1;
				break;
			}
		}
	}

	if (ret == 0 &&
	    (mode == MODE_ALIGNED || mode == MODE_BOTH)) {
		for (size_t i = 0; i < n_aligned_sizes; i++) {
			char name[64];

			memset(name, 0, sizeof(name));
			snprintf(name, sizeof(name) - 1, "aligned_rw_%zu.dat",
				 aligned_sizes[i]);

			if (do_one_test(name, aligned_sizes[i], op) != 0) {
				fprintf(stderr,
					"[partial_rw_test] FAILED (aligned) for size=%zu\n",
					aligned_sizes[i]);
				ret = 1;
				break;
			}
		}
	}

	if (ret == 0)
		printf("[partial_rw_test] All tests passed.\n");

	return ret;
}


