// Random 4KiB read benchmark with data verification.
// This test is intended to detect cases where the filesystem reports
// reads as successful before the underlying data is fully populated
// (e.g., incorrect uptodate/skipread handling or premature completion).

#include "test_global.h"

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#ifndef PAGE_SIZE
#define PAGE_SIZE 4096
#endif

/* Default total file size used for the benchmark (in bytes). */
#define DEFAULT_FILE_SIZE_BYTES (512UL * 1024UL * 1024UL) /* 512 MiB */

/* Default number of 4KiB random reads to perform. */
#define DEFAULT_IO_COUNT (DEFAULT_FILE_SIZE_BYTES / PAGE_SIZE)

static double timespec_to_seconds(const struct timespec *ts)
{
	return (double)ts->tv_sec + (double)ts->tv_nsec / 1e9;
}

static int fill_file_with_pattern(int fd, uint64_t file_size_bytes)
{
	uint64_t page_count;
	uint8_t *buf = NULL;
	int rc = -1;

	if (file_size_bytes == 0 || (file_size_bytes % PAGE_SIZE) != 0) {
		fprintf(stderr,
		        "File size must be a positive multiple of PAGE_SIZE "
		        "(%u bytes), got %" PRIu64 " bytes\n",
		        PAGE_SIZE, file_size_bytes);
		return -1;
	}

	page_count = file_size_bytes / PAGE_SIZE;

	buf = (uint8_t *)malloc(PAGE_SIZE);
	if (!buf) {
		perror("malloc");
		return -1;
	}

	for (uint64_t page_idx = 0; page_idx < page_count; page_idx++) {
		uint64_t *words = (uint64_t *)buf;
		size_t words_per_page = PAGE_SIZE / sizeof(uint64_t);

		for (size_t w = 0; w < words_per_page; w++) {
			/*
			 * Simple deterministic pattern:
			 * every 8-byte word in this 4KiB page is equal to
			 * the page index. Any stale, unmapped, or mis-mapped
			 * data will be caught by the verifier.
			 */
			words[w] = page_idx;
		}

		off_t off = (off_t)page_idx * PAGE_SIZE;
		ssize_t written = pwrite(fd, buf, PAGE_SIZE, off);
		if (written < 0) {
			perror("pwrite");
			goto out;
		}
		if (written != PAGE_SIZE) {
			fprintf(stderr,
			        "Short write at page %" PRIu64
			        ": expected %u bytes, got %zd\n",
			        page_idx, PAGE_SIZE, written);
			goto out;
		}
	}

	if (fsync(fd) < 0) {
		perror("fsync");
		goto out;
	}

	rc = 0;

out:
	free(buf);
	return rc;
}

static int random_read_and_verify(int fd,
				  uint64_t file_size_bytes,
				  uint64_t io_count)
{
	uint64_t page_count;
	uint8_t *buf = NULL;
	struct timespec ts_start, ts_end;
	double elapsed_sec;

	if (file_size_bytes == 0 || (file_size_bytes % PAGE_SIZE) != 0) {
		fprintf(stderr,
		        "File size must be a positive multiple of PAGE_SIZE "
		        "(%u bytes), got %" PRIu64 " bytes\n",
		        PAGE_SIZE, file_size_bytes);
		return -1;
	}

	page_count = file_size_bytes / PAGE_SIZE;

	if (io_count == 0) {
		fprintf(stderr, "io_count must be > 0\n");
		return -1;
	}

	buf = (uint8_t *)malloc(PAGE_SIZE);
	if (!buf) {
		perror("malloc");
		return -1;
	}

	/* Deterministic random sequence for reproducibility. */
	srand(0);

	if (clock_gettime(CLOCK_MONOTONIC, &ts_start) != 0) {
		perror("clock_gettime");
		free(buf);
		return -1;
	}

	for (uint64_t i = 0; i < io_count; i++) {
		uint64_t page_idx = (uint64_t)(rand() % (int)page_count);
		off_t off = (off_t)page_idx * PAGE_SIZE;
		ssize_t n;
		uint64_t *words;
		size_t words_per_page = PAGE_SIZE / sizeof(uint64_t);

		n = pread(fd, buf, PAGE_SIZE, off);
		if (n < 0) {
			perror("pread");
			free(buf);
			return -1;
		}
		if (n != PAGE_SIZE) {
			fprintf(stderr,
			        "Short read at page %" PRIu64
			        " (offset=%jd): expected %u bytes, got %zd\n",
			        page_idx, (intmax_t)off, PAGE_SIZE, n);
			free(buf);
			return -1;
		}

		/* Verify full 4KiB payload. */
		words = (uint64_t *)buf;
		for (size_t w = 0; w < words_per_page; w++) {
			uint64_t expected = page_idx;
			if (words[w] != expected) {
				fprintf(stderr,
				        "Data mismatch at page %" PRIu64
				        ", word %zu (offset=%jd): "
				        "expected=0x%016" PRIx64
				        ", got=0x%016" PRIx64 "\n",
				        page_idx,
				        w,
				        (intmax_t)off,
				        expected,
				        words[w]);
				free(buf);
				return -1;
			}
		}
	}

	if (clock_gettime(CLOCK_MONOTONIC, &ts_end) != 0) {
		perror("clock_gettime");
		free(buf);
		return -1;
	}

	elapsed_sec = timespec_to_seconds(&ts_end) -
		      timespec_to_seconds(&ts_start);

	if (elapsed_sec <= 0.0) {
		elapsed_sec = 1e-9;
	}

	{
		double total_bytes = (double)io_count * (double)PAGE_SIZE;
		double mib = total_bytes / (1024.0 * 1024.0);
		double iops = (double)io_count / elapsed_sec;
		double mib_per_sec = mib / elapsed_sec;

		printf("Random 4KiB read verify benchmark\n");
		printf("  file_size      : %" PRIu64 " bytes (%.2f MiB)\n",
		       file_size_bytes,
		       (double)file_size_bytes / (1024.0 * 1024.0));
		printf("  io_count       : %" PRIu64 " ops\n", io_count);
		printf("  elapsed        : %.6f seconds\n", elapsed_sec);
		printf("  throughput     : %.2f MiB/s\n", mib_per_sec);
		printf("  IOPS (4KiB)    : %.0f ops/s\n", iops);
	}

	free(buf);
	return 0;
}

int main(int argc, char **argv)
{
	const char *dir = TEST_DIR;
	const char *file_name = "random_read_verify.dat";
	char path[512];
	uint64_t file_size_bytes = DEFAULT_FILE_SIZE_BYTES;
	uint64_t io_count = DEFAULT_IO_COUNT;
	int fd = -1;
	int rc;

	/*
	 * Optional arguments:
	 *   argv[1] : total file size in MiB (default: 512)
	 *   argv[2] : number of random 4KiB reads (default: file_size / 4KiB)
	 *   argv[3] : directory path (default: TEST_DIR, usually /oxbow)
	 *
	 * These allow running the same binary against different backends
	 * (e.g., Oxbow vs. ext4) and with different working set sizes.
	 */
	if (argc >= 2) {
		long long size_mib = atoll(argv[1]);
		if (size_mib > 0) {
			file_size_bytes =
				(uint64_t)size_mib * 1024ULL * 1024ULL;
		}
	}

	if (argc >= 3) {
		long long ops = atoll(argv[2]);
		if (ops > 0) {
			io_count = (uint64_t)ops;
		}
	}

	if (argc >= 4) {
		dir = argv[3];
	}

	if (snprintf(path, sizeof(path), "%s/%s", dir, file_name) >=
	    (int)sizeof(path)) {
		fprintf(stderr, "Path buffer too small\n");
		return 1;
	}

	printf("Creating and filling test file: %s\n", path);
	fd = open(path, O_CREAT | O_RDWR | O_TRUNC, 0600);
	if (fd < 0) {
		perror("open");
		return 1;
	}

	rc = fill_file_with_pattern(fd, file_size_bytes);
	if (rc != 0) {
		fprintf(stderr, "Failed to fill file with pattern\n");
		close(fd);
		return 1;
	}

	if (close(fd) < 0) {
		perror("close");
		return 1;
	}

	/* Re-open read-only for the benchmark phase. */
	fd = open(path, O_RDONLY);
	if (fd < 0) {
		perror("open (read-only)");
		return 1;
	}

	printf("Starting random 4KiB read + verify phase...\n");
	rc = random_read_and_verify(fd, file_size_bytes, io_count);
	if (rc != 0) {
		fprintf(stderr,
		        "Random read verify benchmark FAILED (data mismatch or "
		        "I/O error)\n");
		close(fd);
		return 1;
	}

	if (close(fd) < 0) {
		perror("close");
		return 1;
	}

	printf("Random read verify benchmark completed successfully.\n");
	printf("Test file left at: %s (remove manually if desired).\n", path);

	return 0;
}








