/**
 * nvme_mpage_read_throughput_test.c
 *
 * A benchmark program to measure NVMe block-layer read throughput via
 * iod_submit_bio() / nvme_rd_submit_bio().
 *
 * This exercises the same NVMe read path that mpage_readahead() uses
 * (mpage_bio_submit() -> iod_submit_bio() -> nvme_rd_submit_bio()).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <sys/time.h>
#include <stdbool.h>
#include <pthread.h>
#include <assert.h>
#include <ctype.h>
#include <stdint.h>
#include <inttypes.h>
#include <stdatomic.h>
#include <semaphore.h>
#include <signal.h>

#include "config.h"
#include "profile_secure_daemon.h"
#include "io_dispatcher.h"
#include "io/sd_bio.h"
#include "io/nvme.h"
#include "fs/buffer_head.h"

#define BYTES_PER_PAGE		4096

/* Simulate readahead: fixed 32-page (128 KiB) BIOs. */
#define RA_PAGES		32
#define MIN_IO_SIZE		((size_t)RA_PAGES * BYTES_PER_PAGE)
#define MAX_IO_SIZE		MIN_IO_SIZE
#define DEFAULT_IO_SIZE	MIN_IO_SIZE

#define DEFAULT_TEST_DURATION	5	/* seconds */
#define DEFAULT_NUM_THREADS	1

struct thpool_ *thpool_unused __attribute__((unused));

struct {
	pthread_spinlock_t lock;
	uint64_t bytes_completed;
	uint64_t ios_completed;
	uint64_t last_bytes;
	uint64_t last_ios;
	uint64_t last_time;
	bool test_running;
} g_stats;

struct thread_args {
	int		thread_id;
	size_t		io_size;
	int		duration_sec;
	uint64_t	start_time;
	uint64_t	end_time;
	uint64_t	total_bytes;
	uint64_t	total_ios;
	baddr_t		start_blk;
	sem_t		done;
	atomic_int	inflight;
	char		*rbuf;
};

static uint64_t get_time_usec(void)
{
	struct timeval tv;

	gettimeofday(&tv, NULL);
	return (uint64_t)tv.tv_sec * 1000000ULL + (uint64_t)tv.tv_usec;
}

static struct bio *build_single_bio(blk_opf_t opf, char *buf,
				       baddr_t start_blk, size_t len)
{
	struct bio *bio;
	unsigned int nr_vecs;

	if (len == 0)
		return NULL;

	nr_vecs = (unsigned int)(len / BYTES_PER_PAGE);
	if (len % BYTES_PER_PAGE)
		nr_vecs++;

	if (nr_vecs == 0 || nr_vecs > BIO_MAX_VECS) {
		fprintf(stderr,
			"build_single_bio: io_size %zu bytes exceeds BIO_MAX_VECS (%u pages)\n",
			len,
			(unsigned int)BIO_MAX_VECS);
		return NULL;
	}

	bio = alloc_bio((unsigned short)nr_vecs, opf);
	if (!bio)
		return NULL;

	bio->bi_start = start_blk;
	bio->bi_vcnt = nr_vecs;
	bio->bi_vtotal = nr_vecs;

	for (unsigned int i = 0; i < nr_vecs; i++) {
		bio->bi_io_vec[i].bv_buf = buf + ((size_t)i * BYTES_PER_PAGE);
		bio->bi_io_vec[i].bv_len = BYTES_PER_PAGE;
	}

	return bio;
}

static void bio_end_io_read(void *args)
{
	struct bio *bio = (struct bio *)args;
	struct thread_args *t = (struct thread_args *)bio->bi_private;
	size_t bytes = (size_t)bio->bi_vtotal * BYTES_PER_PAGE;

	/* Per-thread stats. */
	t->total_bytes += bytes;
	t->total_ios++;

	/* Global stats for real-time monitoring. */
	pthread_spin_lock(&g_stats.lock);
	g_stats.bytes_completed += bytes;
	g_stats.ios_completed++;
	pthread_spin_unlock(&g_stats.lock);

	if (atomic_fetch_sub_explicit(&t->inflight, 1, memory_order_acq_rel) == 1)
		sem_post(&t->done);

	if (bio->bi_io_vec)
		free(bio->bi_io_vec);
	free(bio);
}

static void *monitoring_thread(void *arg)
{
	uint64_t current_time;
	uint64_t elapsed_time;
	uint64_t current_bytes;
	uint64_t bytes_since_last;
	uint64_t current_ios;
	uint64_t ios_since_last;
	double mb_per_sec;
	double iops;

	(void)arg;

	while (g_stats.test_running) {
		sleep(1);

		current_time = get_time_usec();

		pthread_spin_lock(&g_stats.lock);
		current_bytes = g_stats.bytes_completed;
		current_ios = g_stats.ios_completed;
		bytes_since_last = current_bytes - g_stats.last_bytes;
		ios_since_last = current_ios - g_stats.last_ios;
		elapsed_time = current_time - g_stats.last_time;

		g_stats.last_bytes = current_bytes;
		g_stats.last_ios = current_ios;
		g_stats.last_time = current_time;
		pthread_spin_unlock(&g_stats.lock);

		if (elapsed_time > 0) {
			mb_per_sec = ((double)bytes_since_last / (1024.0 * 1024.0)) /
					((double)elapsed_time / 1000000.0);
			iops = (double)ios_since_last /
			       ((double)elapsed_time / 1000000.0);

			printf("Real-time stats: %.2f MB/s, %.2f IOPS\n",
			       mb_per_sec, iops);
		}
	}

	return NULL;
}

static void *worker_thread(void *arg)
{
	struct thread_args *t = (struct thread_args *)arg;
	uint64_t start_time;
	uint64_t deadline;
	baddr_t block_address;
	unsigned int nr_blocks;

	nr_blocks = (unsigned int)(t->io_size / OXBOW_BLOCK_SIZE);
	if (t->io_size % OXBOW_BLOCK_SIZE)
		nr_blocks++;

	block_address = t->start_blk;

	start_time = get_time_usec();
	t->start_time = start_time;
	deadline = start_time + (uint64_t)t->duration_sec * 1000000ULL;

	while (1) {
		uint64_t now = get_time_usec();
		struct bio *bio;

		if (now >= deadline)
			break;

		atomic_store_explicit(&t->inflight, 1, memory_order_release);

		bio = build_single_bio(REQ_OP_READ, t->rbuf, block_address,
				       t->io_size);
		if (!bio) {
			fprintf(stderr,
				"Thread %d: failed to build read bio (io_size=%zu).\n",
				 t->thread_id, t->io_size);
			break;
		}

		bio->bi_private = t;
		bio->end_io = bio_end_io_read;

		iod_submit_bio(REQ_OP_READ, bio);

		sem_wait(&t->done);

		block_address += nr_blocks;
		if (block_address > t->start_blk + (baddr_t)nr_blocks * 1000U)
			block_address = t->start_blk;
	}

	t->end_time = get_time_usec();

	return NULL;
}

static const char *get_env_with_default(const char *name,
					      const char *default_value)
{
	const char *value = getenv(name);

	return value ? value : default_value;
}

int main(int argc, char *argv[])
{
	int num_threads = DEFAULT_NUM_THREADS;
	int test_duration = DEFAULT_TEST_DURATION;
	size_t io_size = DEFAULT_IO_SIZE;
	int i;
	pthread_t *threads = NULL;
	pthread_t monitor_thread;
	struct thread_args *args = NULL;
	uint64_t total_bytes = 0;
	uint64_t total_ios = 0;
	uint64_t min_start_time = UINT64_MAX;
	uint64_t max_end_time = 0;
	double elapsed_sec;
	double throughput_mbs;
	double iops;
	const char *filesystem;

	for (i = 1; i < argc; i++) {
		if (strcmp(argv[i], "--threads") == 0 && i + 1 < argc) {
			num_threads = atoi(argv[i + 1]);
			i++;
		} else if (strcmp(argv[i], "--duration") == 0 && i + 1 < argc) {
			test_duration = atoi(argv[i + 1]);
			i++;
		} else if (strcmp(argv[i], "--io-size") == 0 && i + 1 < argc) {
			char *endptr;

			io_size = strtoul(argv[i + 1], &endptr, 10);
			if (*endptr != '\0') {
				if (tolower(*endptr) == 'k')
					io_size *= 1024ULL;
				else if (tolower(*endptr) == 'm')
					io_size *= 1024ULL * 1024ULL;
				else if (tolower(*endptr) == 'g')
					io_size *= 1024ULL * 1024ULL * 1024ULL;
				else {
					fprintf(stderr,
						"Invalid IO size unit: %s\n",
						endptr);
					fprintf(stderr,
						"Valid units are: K, M, G (case insensitive)\n");
					return 1;
				}
			}
			i++;
		} else if (strcmp(argv[i], "--help") == 0) {
			printf("Usage: %s [OPTIONS]\n", argv[0]);
			printf("Options:\n");
			printf("  --threads N       Number of worker threads (default: %d)\n",
			       DEFAULT_NUM_THREADS);
			printf("  --duration N      Test duration in seconds (default: %d)\n",
			       DEFAULT_TEST_DURATION);
			printf("  --io-size N[K|M|G] (ignored, fixed to %zu bytes for readahead simulation)\n",
			       (size_t)DEFAULT_IO_SIZE);
			printf("  --help            Display this help message\n");
			return 0;
		}
	}

	if (num_threads <= 0)
		num_threads = 1;

	if (io_size < MIN_IO_SIZE)
		io_size = MIN_IO_SIZE;
	if (io_size > MAX_IO_SIZE) {
		fprintf(stderr,
			"Requested io_size %zu exceeds MAX_IO_SIZE %zu; clamping.\n",
			io_size, (size_t)MAX_IO_SIZE);
		io_size = MAX_IO_SIZE;
	}

	filesystem = get_env_with_default("filesystem", "sefs");

	load_secure_daemon_configs();
	print_secure_daemon_configs();

#ifdef OXBOW_PROFILE
	pf_init(PF_SIGNAL);
#endif

	if (init_io_dispatcher() < 0) {
		fprintf(stderr, "Failed to initialize IO Dispatcher.\n");
		return 1;
	}

	printf("nvme_mpage_read_throughput_test\n");
	printf("=================================\n");
	printf("Filesystem:        %s\n", filesystem);
	printf("Worker Threads:    %d\n", num_threads);
	printf("Test Duration:     %d seconds\n", test_duration);
	printf("IO Size:           %zu bytes", io_size);
	if (io_size >= 1024ULL * 1024ULL * 1024ULL)
		printf(" (%.2f GiB)\n", (double)io_size / (1024.0 * 1024.0 * 1024.0));
	else if (io_size >= 1024ULL * 1024ULL)
		printf(" (%.2f MiB)\n", (double)io_size / (1024.0 * 1024.0));
	else if (io_size >= 1024ULL)
		printf(" (%.2f KiB)\n", (double)io_size / 1024.0);
	else
		printf("\n");
	printf("\n");

	pthread_spin_init(&g_stats.lock, PTHREAD_PROCESS_PRIVATE);
	pthread_spin_lock(&g_stats.lock);
	g_stats.bytes_completed = 0;
	g_stats.ios_completed = 0;
	g_stats.last_bytes = 0;
	g_stats.last_ios = 0;
	g_stats.last_time = get_time_usec();
	g_stats.test_running = true;
	pthread_spin_unlock(&g_stats.lock);

	if (pthread_create(&monitor_thread, NULL, monitoring_thread, NULL) != 0) {
		fprintf(stderr, "Failed to create monitoring thread.\n");
	}

	threads = malloc(sizeof(pthread_t) * (size_t)num_threads);
	args = malloc(sizeof(struct thread_args) * (size_t)num_threads);
	if (!threads || !args) {
		fprintf(stderr, "Memory allocation failed.\n");
		free(threads);
		free(args);
		g_stats.test_running = false;
		pthread_join(monitor_thread, NULL);
		pthread_spin_destroy(&g_stats.lock);
		return 1;
	}

	for (i = 0; i < num_threads; i++) {
		args[i].thread_id = i;
		args[i].io_size = io_size;
		args[i].duration_sec = test_duration;
		args[i].total_bytes = 0;
		args[i].total_ios = 0;
		args[i].start_time = 0;
		args[i].end_time = 0;
		args[i].rbuf = calloc(1, io_size);
		if (!args[i].rbuf) {
			fprintf(stderr,
				"Allocation failed for thread %d buffer (size=%zu).\n",
				 i, io_size);
			g_stats.test_running = false;
			pthread_join(monitor_thread, NULL);
			for (int j = 0; j < i; j++) {
				free(args[j].rbuf);
				sem_destroy(&args[j].done);
			}
			free(threads);
			free(args);
			pthread_spin_destroy(&g_stats.lock);
			return 1;
		}

		/* Separate each thread's LBA range to avoid overlaps. */
		{
			baddr_t stride_blks;

			stride_blks = (baddr_t)(io_size / BYTES_PER_PAGE) + 128U;
			args[i].start_blk = (baddr_t)1 + (baddr_t)i * stride_blks;
		}

		sem_init(&args[i].done, 0, 0);
		atomic_store_explicit(&args[i].inflight, 0, memory_order_relaxed);

		if (pthread_create(&threads[i], NULL, worker_thread, &args[i]) != 0) {
			fprintf(stderr, "Failed to create worker thread %d.\n", i);
			g_stats.test_running = false;
			pthread_join(monitor_thread, NULL);
			for (int j = 0; j <= i; j++) {
				free(args[j].rbuf);
				sem_destroy(&args[j].done);
			}
			free(threads);
			free(args);
			pthread_spin_destroy(&g_stats.lock);
			return 1;
		}
	}

	for (i = 0; i < num_threads; i++) {
		pthread_join(threads[i], NULL);

		total_bytes += args[i].total_bytes;
		total_ios += args[i].total_ios;
		if (args[i].start_time && args[i].start_time < min_start_time)
			min_start_time = args[i].start_time;
		if (args[i].end_time > max_end_time)
			max_end_time = args[i].end_time;
	}

	pthread_spin_lock(&g_stats.lock);
	g_stats.test_running = false;
	pthread_spin_unlock(&g_stats.lock);
	pthread_join(monitor_thread, NULL);

	if (max_end_time > min_start_time)
		elapsed_sec = (double)(max_end_time - min_start_time) / 1000000.0;
	else
		elapsed_sec = (double)test_duration;

	throughput_mbs = ((double)total_bytes / (1024.0 * 1024.0)) /
			       (elapsed_sec > 0.0 ? elapsed_sec : 1.0);
	iops = (elapsed_sec > 0.0) ? ((double)total_ios / elapsed_sec) : 0.0;

	printf("Test Results:\n");
	printf("  IO Type:       READ (mpage/iod/nvme path)\n");
	printf("  IO Size:       %zu bytes\n", io_size);
	printf("  Total IOs:     %" PRIu64 "\n", total_ios);
	printf("  Total Bytes:   %" PRIu64 " bytes (%.2f MB)\n",
	       total_bytes,
	       (double)total_bytes / (1024.0 * 1024.0));
	printf("  Elapsed Time:  %.2f seconds\n", elapsed_sec);
	printf("  Throughput:    %.2f MB/s\n", throughput_mbs);
	printf("  IOPS:          %.2f IO/s\n", iops);
	printf("\n");

	for (i = 0; i < num_threads; i++) {
		free(args[i].rbuf);
		sem_destroy(&args[i].done);
	}
	free(threads);
	free(args);
	pthread_spin_destroy(&g_stats.lock);

	/* Stop dedicated NVMe read workers before tearing down IO dispatcher
	 * resources so that nvme_rd_submit_bio() loops exit cleanly.
	 */
	nvme_stop_rd_workers();

	exit_io_dispatcher();

	return 0;
}
