#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <getopt.h>
#include <ctype.h>

#include "test_global.h"

#define DEFAULT_FILE_NAME "unaligned_write.test"

enum write_mode {
	MODE_ALIGNED = 0,
	MODE_UNALIGNED = 1,
};

static void print_usage(const char *prog)
{
	printf("Usage: %s [--mode aligned|unaligned] [--size N] [--io-size N] [--misalign N] [--file-name NAME]\n",
	       prog);
	printf("  --mode|-m       Write mode: aligned or unaligned (default: unaligned)\n");
	printf("  --size|-s       Total bytes to write (supports suffix k/m/g, default: 1m)\n");
	printf("  --io-size|-i    IO size per write/read (default: 4096)\n");
	printf("  --misalign|-o   Offset misalignment in bytes for unaligned mode (default: 123)\n");
	printf("  --file-name|-f  File name to use (alias: --file_name). Default: unaligned_write.test.<pid>\n");
}

static size_t parse_size_with_suffix(const char *s)
{
	char *end = NULL;
	unsigned long long base = strtoull(s, &end, 10);
	unsigned long long mult = 1ULL;
	if (end && *end != '\0') {
		char c = (char)tolower((unsigned char)*end);
		if (c == 'k')
			mult = 1024ULL;
		else if (c == 'm')
			mult = 1024ULL * 1024ULL;
		else if (c == 'g')
			mult = 1024ULL * 1024ULL * 1024ULL;
		else {
			fprintf(stderr, "Invalid size suffix: %s\n", end);
			exit(EXIT_FAILURE);
		}
	}
	return (size_t)(base * mult);
}

int main(int argc, char **argv)
{
	int opt;
	int opt_index = 0;
	enum write_mode mode = MODE_UNALIGNED;
	size_t total_size = 0; /* set after parse */
	size_t io_size = 4096;
	size_t misalign = 123;
	const char *file_name = NULL;
	const struct option long_opts[] = {
		{ "mode", required_argument, 0, 'm' },
		{ "size", required_argument, 0, 's' },
		{ "io-size", required_argument, 0, 'i' },
		{ "misalign", required_argument, 0, 'o' },
		{ "file-name", required_argument, 0, 'f' },
		{ "file_name", required_argument, 0, 'f' },
		{ "help", no_argument, 0, 'h' },
		{ 0, 0, 0, 0 }
	};

	/* Defaults */
	total_size = parse_size_with_suffix("40k");

	while ((opt = getopt_long(argc, argv, "m:s:i:o:f:h", long_opts,
				  &opt_index)) != -1) {
		switch (opt) {
		case 'm':
			if (strcmp(optarg, "aligned") == 0)
				mode = MODE_ALIGNED;
			else if (strcmp(optarg, "unaligned") == 0)
				mode = MODE_UNALIGNED;
			else {
				fprintf(stderr, "Unknown mode: %s\n", optarg);
				print_usage(argv[0]);
				return EXIT_FAILURE;
			}
			break;
		case 's':
			total_size = parse_size_with_suffix(optarg);
			break;
		case 'i':
			io_size = parse_size_with_suffix(optarg);
			break;
		case 'o':
			misalign = parse_size_with_suffix(optarg);
			break;
		case 'f':
			file_name = optarg;
			break;
		case 'h':
			print_usage(argv[0]);
			return EXIT_SUCCESS;
		default:
			print_usage(argv[0]);
			return EXIT_FAILURE;
		}
	}

	/* Ensure test directory exists */
	if (strcmp(TEST_DIR, "/oxbow") != 0 &&
	    mkdir(TEST_DIR, 0700) < 0 && errno != EEXIST) {
		perror("mkdir TEST_DIR");
		return EXIT_FAILURE;
	}

	char path[4096];
	memset(path, 0, sizeof(path));
	if (file_name && file_name[0] != '\0')
		snprintf(path, sizeof(path) - 1, "%s/%s", TEST_DIR, file_name);
	else
		snprintf(path, sizeof(path) - 1, "%s/%s.%d", TEST_DIR,
			 DEFAULT_FILE_NAME, (int)getpid());

	printf("[unaligned_write] Config: mode=%s, size=%zu, io_size=%zu, misalign=%zu\n",
	       mode == MODE_ALIGNED ? "aligned" : "unaligned", total_size,
	       io_size, misalign);
	printf("[unaligned_write] Target: %s\n", path);

	int fd = open(path, O_RDWR | O_CREAT | O_TRUNC, 0600);
	if (fd < 0) {
		perror("open");
		return EXIT_FAILURE;
	}

	/* Allocate IO buffer */
	char *wbuf = (char *)malloc(io_size);
	if (!wbuf) {
		fprintf(stderr, "malloc failed\n");
		close(fd);
		return EXIT_FAILURE;
	}

	/* Fill write buffer with a simple pattern */
	for (size_t i = 0; i < io_size; i++)
		wbuf[i] = (char)((i % 251) + 1);

	/* Select starting offset */
	off_t off = (mode == MODE_UNALIGNED) ? (off_t)misalign : 0;
	size_t written = 0;
	while (written < total_size) {
		size_t chunk = io_size;
		if (total_size - written < chunk)
			chunk = total_size - written;

		printf("\tpwrite %zu bytes at offset %zu\n", chunk, off);
		ssize_t ret = pwrite(fd, wbuf, chunk, off);
		if (ret < 0) {
			perror("pwrite");
			free(wbuf);
			close(fd);
			return EXIT_FAILURE;
		}
		if ((size_t)ret != chunk) {
			fprintf(stderr, "Short pwrite: %zd vs %zu\n", ret,
				chunk);
			free(wbuf);
			close(fd);
			return EXIT_FAILURE;
		}
		written += chunk;
		off += (off_t)chunk;
	}

	if (fsync(fd) < 0) {
		perror("fsync");
		free(wbuf);
		close(fd);
		return EXIT_FAILURE;
	}

	if (close(fd) < 0) {
		perror("close");
		free(wbuf);
		return EXIT_FAILURE;
	}

	free(wbuf);

	printf("[unaligned_write] Completed successfully.\n");
	return EXIT_SUCCESS;
}
