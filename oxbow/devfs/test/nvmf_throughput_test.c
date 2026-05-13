/**
 * nvmf_throughput_test.c
 * 
 * A benchmark program to measure IO throughput of se_nvmf storage engine.
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
#include <sys/syscall.h>
#include <ctype.h> // For tolower()

#include "common/storage_engine.h"
#include "common/se_nvmf.h"
#include "common/bio.h"
#include "oxbow_debug.h"
#include "thpool.h"

threadpool thpool;

// Global statistics for real-time monitoring
struct {
	pthread_spinlock_t lock;
	uint64_t bytes_completed;
	uint64_t ios_completed;
	uint64_t last_bytes;
	uint64_t last_ios;
	uint64_t last_time;
	bool test_running;
} g_stats;

// Global preload buffer
static char *g_preload_buffer = NULL;

// Get thread ID for Linux
static inline pid_t gettid(void)
{
	return syscall(SYS_gettid);
}

#define DEFAULT_BLOCK_SIZE 40960
#define DEFAULT_TEST_DURATION 5 // seconds
#define MIN_IO_SIZE (4 * 1024) // 4KB
#define MAX_IO_SIZE (16 * 1024 * 1024) // 16MB
#define DEFAULT_NUM_THREADS 8
#define DEFAULT_NUM_IO_REQUESTS 512
#define PRELOAD_SIZE (20ULL * 1024 * 1024 * 1024) // 20GB preload buffer

// Forward declarations
static void *monitoring_thread(void *arg);

// Default NVMf configurations if environment variables are not set
#define DEFAULT_TARGET_IP "192.168.14.113"
#define DEFAULT_TARGET_PORT 4420
#define DEFAULT_SUBNQN "oxbow-nvmf"

#define SYNC_IO

struct thread_args {
	int thread_id;
	size_t io_size;
	bool is_read;
	uint64_t start_time;
	uint64_t end_time;
	uint64_t total_bytes;
	uint64_t total_ios;
};

// Get time in microseconds
static uint64_t get_time_usec(void)
{
	struct timeval tv;
	gettimeofday(&tv, NULL);
	return (uint64_t)tv.tv_sec * 1000000 + tv.tv_usec;
}

// Function to preload data into memory
static void preload_data(void)
{
	printf("Preloading 20GB of data into memory...\n");

	// Allocate 20GB of memory
	g_preload_buffer = malloc(PRELOAD_SIZE);
	if (!g_preload_buffer) {
		fprintf(stderr,
			"Failed to allocate preload buffer. Continuing without preloading.\n");
		return;
	}

	// Fill buffer with random data
	printf("Filling preload buffer with random data...\n");
	// for (size_t i = 0; i < PRELOAD_SIZE; i += 4096) {
	// 	// Fill in 4KB chunks to avoid excessive memory pressure
	// 	for (size_t j = 0; j < 4096 && (i + j) < PRELOAD_SIZE; j++) {
	// 		g_preload_buffer[i + j] = (char)(rand() % 256);
	// 	}

	// 	// Print progress every 1GB
	// 	if (i % (1024 * 1024 * 1024) == 0 && i > 0) {
	// 		printf("Preloaded %zu GB\n", i / (1024 * 1024 * 1024));
	// 	}
	// }

	printf("Preloading complete. 20GB loaded into memory.\n");
}

// Free preloaded data
static void free_preloaded_data(void)
{
	if (g_preload_buffer) {
		free(g_preload_buffer);
		g_preload_buffer = NULL;
		printf("Freed preloaded memory buffer.\n");
	}
}

// Initialize a bio for testing
static struct bio *initialize_test_bio(baddr_t blk_no, size_t io_size)
{
	struct bio *bio;
	char *buffer;

	// Allocate bio with one bio_vec
	bio = alloc_bio_n_bvecs(blk_no, 1);
	if (!bio) {
		fprintf(stderr, "Failed to allocate bio\n");
		return NULL;
	}

	// Allocate buffer aligned to DEFAULT_BLOCK_SIZE
	buffer = se_alloc_dma_buffer(io_size);
	if (!buffer) {
		fprintf(stderr, "Failed to allocate buffer\n");
		free_bio(bio);
		return NULL;
	}

	// Fill buffer with data from preloaded buffer if available
	if (g_preload_buffer) {
		// Use modulo to wrap around the preload buffer
		// size_t offset = (blk_no * DEFAULT_BLOCK_SIZE) % PRELOAD_SIZE;
		// if (offset + io_size <= PRELOAD_SIZE) {
		// 	// Copy continuous chunk if it fits
		// 	memcpy(buffer, g_preload_buffer + offset, io_size);
		// } else {
		// 	// Handle wrap-around case
		// 	size_t first_part = PRELOAD_SIZE - offset;
		// 	memcpy(buffer, g_preload_buffer + offset, first_part);
		// 	memcpy(buffer + first_part, g_preload_buffer,
		// 	       io_size - first_part);
		// }
	} else {
		// Fallback to random data if preload buffer is not available
		// for (size_t i = 0; i < io_size; i++) {
		// 	buffer[i] = (char)(rand() % 256);
		// }
	}

	// Initialize bio_vec
	bio->bi_io_vec[0].bv_buf = buffer;
	bio->bi_io_vec[0].bv_len = io_size;
	bio->total_size = io_size;

	return bio;
}

// Worker thread function
static void worker_thread(void *arg)
{
	struct thread_args *args = (struct thread_args *)arg;
	struct bio_list bl;
	uint64_t start_time, end_time;
	uint64_t elapsed_time;
	uint64_t total_ios = 0;
	uint64_t total_bytes = 0;
	baddr_t block_address;
	uint32_t req_cnt;
	int batch_size = 4;
	int dispatch_count = 0;
	int max_pending_ios =
		4; // Maximum number of pending I/Os before polling
	char *buffer;

	// Set different block addresses for different threads
	block_address =
		args->thread_id * (MAX_IO_SIZE / DEFAULT_BLOCK_SIZE) * 10;

	bio_list_init(&bl);

	// Keep track of time
	start_time = get_time_usec();
	args->start_time = start_time;

	struct bio *bio;
	// Create a new bio for testing
	bio = initialize_test_bio(block_address, args->io_size);
	if (!bio) {
		fprintf(stderr, "Failed to initialize test bio\n");
	}

	// Add to bio list
	bio_list_add(&bl, bio);

	buffer = se_alloc_dma_buffer(args->io_size);
	if (!buffer) {
		fprintf(stderr, "Failed to allocate buffer\n");
		free(buffer);
		exit(1);
	}

	while (1) {
		// Check if test duration has elapsed
		end_time = get_time_usec();
		elapsed_time = end_time - start_time;
		if (elapsed_time >= args->end_time) {
			break;
		}
#ifdef SYNC_IO

		// Dispatch IO
		req_cnt =
			se_dispatch_io_nocopy_sync(&bl, args->is_read, buffer);
		// printf("DISPATCHING IO done. dispatch_count: %d\n",
		//        dispatch_count);
		if (req_cnt < 0) {
			fprintf(stderr, "Failed to dispatch IO\n");
			break;
		}

#else
		// Poll for completed I/Os when we have enough pending I/Os
		// This ensures we're continuously processing completions
		if (dispatch_count >= max_pending_ios) {
			// Poll for a portion of pending I/Os to maintain balance
			int poll_count = dispatch_count / 2;
			for (int i = 0; i < poll_count; i++) {
				se_nvmf_poll_complete(i % batch_size);
				dispatch_count--;
			}
			// printf("POLLING %d IOs done. Remaining pending: %d\n",
			//        poll_count, dispatch_count);
		}

		// Create a new bio for testing
		bio = initialize_test_bio(block_address, args->io_size);
		if (!bio) {
			fprintf(stderr, "Failed to initialize test bio\n");
			break;
		}

		// Add to bio list
		bio_list_add(&bl, bio);

		// Dispatch IO
		req_cnt =
			se_dispatch_io_nocopy_async(&bl, args->is_read, buffer);
		dispatch_count++;
		// printf("DISPATCHING IO done. dispatch_count: %d\n",
		//        dispatch_count);
		if (req_cnt < 0) {
			fprintf(stderr, "Failed to dispatch IO\n");
			break;
		}
#endif

		// Update statistics
		total_ios++;
		total_bytes += args->io_size;

		// Update global statistics for real-time monitoring
		pthread_spin_lock(&g_stats.lock);
		g_stats.bytes_completed += args->io_size;
		g_stats.ios_completed++;
		pthread_spin_unlock(&g_stats.lock);

		// Move to next block address (wrap around if needed)
		block_address += (args->io_size / DEFAULT_BLOCK_SIZE);
		if (block_address > (MAX_IO_SIZE / DEFAULT_BLOCK_SIZE) * 100) {
			block_address = args->thread_id *
					(MAX_IO_SIZE / DEFAULT_BLOCK_SIZE) * 10;
		}
	}
	// Free resources
	while ((bio = bio_list_pop(&bl)) != NULL) {
		free_bio(bio);
	}

	// Make sure to poll for any remaining I/Os before exiting
#ifndef SYNC_IO
	while (dispatch_count > 0) {
		se_nvmf_poll_complete(dispatch_count % batch_size);
		dispatch_count--;
		// printf("Final POLLING IO done. dispatch_count: %d\n",
		//        dispatch_count);
	}
#endif

	args->end_time = end_time;
	args->total_bytes = total_bytes;
	args->total_ios = total_ios;

	printf("Thread %d finished. total_bytes: %lu total_ios: %lu\n",
	       args->thread_id, total_bytes, total_ios);

	return NULL;
}

// Run throughput test with specific parameters
static void run_throughput_test(size_t io_size, bool is_read, int num_threads,
				int test_duration)
{
	pthread_t *threads;
	pthread_t monitor_thread;
	struct thread_args *args;
	uint64_t total_bytes = 0;
	uint64_t total_ios = 0;
	uint64_t min_start_time = UINT64_MAX;
	uint64_t max_end_time = 0;
	double elapsed_sec;
	double throughput_mbs;
	double iops;

	printf("Running %s test with IO size: %zu bytes, Threads: %d, Duration: %d seconds\n",
	       is_read ? "READ" : "WRITE", io_size, num_threads, test_duration);

	// Initialize global statistics
	pthread_spin_init(&g_stats.lock, PTHREAD_PROCESS_PRIVATE);
	pthread_spin_lock(&g_stats.lock);
	g_stats.bytes_completed = 0;
	g_stats.ios_completed = 0;
	g_stats.last_bytes = 0;
	g_stats.last_ios = 0;
	g_stats.last_time = get_time_usec();
	g_stats.test_running = true;
	pthread_spin_unlock(&g_stats.lock);

	// Start monitoring thread
	if (pthread_create(&monitor_thread, NULL, monitoring_thread, NULL) !=
	    0) {
		fprintf(stderr, "Failed to create monitoring thread\n");
	}

	// Allocate memory for thread management
	threads = malloc(num_threads * sizeof(pthread_t));
	args = malloc(num_threads * sizeof(struct thread_args));
	if (!threads || !args) {
		fprintf(stderr, "Memory allocation failed\n");
		if (threads)
			free(threads);
		if (args)
			free(args);
		return;
	}

	// Initialize and start all threads
	for (int i = 0; i < num_threads; i++) {
		args[i].thread_id = i;
		args[i].io_size = io_size;
		args[i].is_read = is_read;
		args[i].end_time =
			test_duration *
			1000000ULL; // Convert seconds to microseconds

		thpool_add_work(thpool, worker_thread, &args[i]);

		// if (pthread_create(&threads[i], NULL, worker_thread,
		// 		    &args[i]) != 0) {
		// 	fprintf(stderr, "Failed to create thread %d\n", i);
		// 	// Continue with fewer threads
		// }
	}

	// Wait for all threads to complete
	for (int i = 0; i < num_threads; i++) {
		// pthread_join(threads[i], NULL);
		thpool_wait(thpool);

		// Aggregate statistics
		total_bytes += args[i].total_bytes;
		total_ios += args[i].total_ios;

		if (args[i].start_time < min_start_time) {
			min_start_time = args[i].start_time;
		}

		if (args[i].end_time > max_end_time) {
			max_end_time = args[i].end_time;
		}
	}

	// Stop monitoring thread
	pthread_spin_lock(&g_stats.lock);
	g_stats.test_running = false;
	pthread_spin_unlock(&g_stats.lock);
	pthread_join(monitor_thread, NULL);

	// Calculate throughput
	elapsed_sec = (double)(max_end_time - min_start_time) / 1000000.0;
	throughput_mbs =
		((double)total_bytes / (1024.0 * 1024.0)) / elapsed_sec;
	iops = (double)total_ios / elapsed_sec;

	// Print results
	printf("Test Results:\n");
	printf("  IO Type:       %s\n", is_read ? "READ" : "WRITE");
	printf("  IO Size:       %zu bytes\n", io_size);
	printf("  Total IOs:     %lu\n", total_ios);
	printf("  Total Bytes:   %lu bytes (%.2f MB)\n", total_bytes,
	       (double)total_bytes / (1024.0 * 1024.0));
	printf("  Elapsed Time:  %.2f seconds\n", elapsed_sec);
	printf("  Throughput:    %.2f MB/s\n", throughput_mbs);
	printf("  IOPS:          %.2f IO/s\n", iops);
	printf("\n");

	// Clean up
	pthread_spin_destroy(&g_stats.lock);
	free(threads);
	free(args);
}

// Monitoring thread for real-time throughput reporting
static void *monitoring_thread(void *arg)
{
	uint64_t current_time, elapsed_time;
	uint64_t current_bytes, bytes_since_last;
	uint64_t current_ios, ios_since_last;
	double mb_per_sec, iops;

	while (g_stats.test_running) {
		sleep(1); // Print stats every second

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
			mb_per_sec =
				((double)bytes_since_last / (1024.0 * 1024.0)) /
				((double)elapsed_time / 1000000.0);
			iops = (double)ios_since_last /
			       ((double)elapsed_time / 1000000.0);

			printf("Real-time stats: %.2f MB/s, %.2f IOPS\n",
			       mb_per_sec, iops);
		}
	}

	return NULL;
}

// Get an environment variable with a default value if not set
static const char *get_env_with_default(const char *name,
					const char *default_value)
{
	const char *value = getenv(name);
	return value ? value : default_value;
}

int main(int argc, char *argv[])
{
	struct se_config se_config;
	struct nvmf_config *nvmf_conf;
	int num_threads = DEFAULT_NUM_THREADS;
	int test_duration = DEFAULT_TEST_DURATION;
	int rc;
	const char *nvmf_ip_addr;
	int nvmf_port;
	const char *nvmf_subnqn;
	uint32_t num_io_requests = DEFAULT_NUM_IO_REQUESTS;
	const char *env_value;
	size_t single_io_size = 0; // 0 means run with multiple IO sizes
	bool skip_preload = false;

	// Process command line arguments
	for (int i = 1; i < argc; i++) {
		if (strcmp(argv[i], "--threads") == 0 && i + 1 < argc) {
			num_threads = atoi(argv[i + 1]);
			i++;
		} else if (strcmp(argv[i], "--duration") == 0 && i + 1 < argc) {
			test_duration = atoi(argv[i + 1]);
			i++;
		} else if (strcmp(argv[i], "--io-size") == 0 && i + 1 < argc) {
			char *endptr;
			single_io_size = strtoul(argv[i + 1], &endptr, 10);

			// Handle unit suffix
			if (*endptr != '\0') {
				if (tolower(*endptr) == 'k') {
					single_io_size *= 1024;
				} else if (tolower(*endptr) == 'm') {
					single_io_size *= 1024 * 1024;
				} else if (tolower(*endptr) == 'g') {
					single_io_size *= 1024 * 1024 * 1024;
				} else {
					fprintf(stderr,
						"Invalid IO size unit: %s\n",
						endptr);
					fprintf(stderr,
						"Valid units are: K, M, G (case insensitive)\n");
					return 1;
				}
			}
			i++;
		} else if (strcmp(argv[i], "--skip-preload") == 0) {
			skip_preload = true;
		} else if (strcmp(argv[i], "--help") == 0) {
			printf("Usage: %s [OPTIONS]\n", argv[0]);
			printf("Options:\n");
			printf("  --threads N    Number of worker threads (default: %d)\n",
			       DEFAULT_NUM_THREADS);
			printf("  --duration N   Test duration in seconds (default: %d)\n",
			       DEFAULT_TEST_DURATION);
			printf("  --io-size N[K|M|G]  Use a single IO size instead of multiple sizes\n");
			printf("                   Units: K=KiB, M=MiB, G=GiB (e.g., 4K, 1M)\n");
			printf("                   If not specified, tests run with multiple IO sizes\n");
			printf("  --skip-preload Skip preloading 20GB of data into memory\n");
			printf("  --help         Display this help message\n");
			return 0;
		}
	}

	// Initialize random number generator
	srand(time(NULL));

	// Preload data if not skipped
	if (!skip_preload) {
		preload_data();
	}

	// Get configuration from environment variables with defaults
	nvmf_ip_addr = get_env_with_default("nvmf_ip_addr", DEFAULT_TARGET_IP);

	env_value = getenv("nvmf_port");
	nvmf_port = env_value ? atoi(env_value) : DEFAULT_TARGET_PORT;

	nvmf_subnqn = get_env_with_default("nvmf_subnqn", DEFAULT_SUBNQN);

	env_value = getenv("spdk_max_io_requests_in_qpair");
	if (env_value) {
		num_io_requests = atoi(env_value);
	}

	// Configure NVMf target
	nvmf_conf = &se_config.nvmf;
	strncpy(nvmf_conf->target_ip_addr, nvmf_ip_addr,
		sizeof(nvmf_conf->target_ip_addr));
	nvmf_conf->port = nvmf_port;
	strncpy(nvmf_conf->subnqn_name, nvmf_subnqn,
		sizeof(nvmf_conf->subnqn_name));
	nvmf_conf->num_io_requests = num_io_requests;

	// Initialize NVMf storage engine using init_storage_engine instead of direct nvmf_init
	printf("Initializing NVMf storage engine...\n");
	rc = init_storage_engine(SE_NVMF, &se_config, num_threads);
	if (rc != 0) {
		fprintf(stderr, "Failed to initialize NVMf storage engine\n");
		return 1;
	}
	thpool = se_config.nvmf.worker_thpool;

	printf("NVMf throughput test\n");
	printf("====================\n");
	printf("Target IP:       %s\n", nvmf_conf->target_ip_addr);
	printf("Target Port:     %d\n", nvmf_conf->port);
	printf("SubNQN:          %s\n", nvmf_conf->subnqn_name);
	printf("IO Requests:     %d\n", nvmf_conf->num_io_requests);
	printf("Worker Threads:  %d\n", num_threads);
	printf("Test Duration:   %d seconds\n", test_duration);
	if (single_io_size > 0) {
		printf("IO Size:         %zu bytes", single_io_size);
		if (single_io_size >= 1024 * 1024 * 1024) {
			printf(" (%.2f GiB)\n",
			       (double)single_io_size / (1024 * 1024 * 1024));
		} else if (single_io_size >= 1024 * 1024) {
			printf(" (%.2f MiB)\n",
			       (double)single_io_size / (1024 * 1024));
		} else if (single_io_size >= 1024) {
			printf(" (%.2f KiB)\n", (double)single_io_size / 1024);
		} else {
			printf("\n");
		}
	} else {
		printf("IO Sizes:        Multiple sizes (%d B to %d MB)\n",
		       MIN_IO_SIZE, MAX_IO_SIZE / (1024 * 1024));
	}
	printf("\n");

	// run_throughput_test(16 * 1024 * 1024, false, num_threads,
	// 		    test_duration);
	// run_throughput_test(4096, false, num_threads, test_duration);
	run_throughput_test(32 * 1024 * 1024, false, num_threads,
			    test_duration);
	// run_throughput_test(128*1024*1024, false, num_threads, test_duration);

	//	if (single_io_size > 0) {
	//		// Run tests with a single IO size
	//		printf("Running tests with IO size: %zu bytes\n",
	//		       single_io_size);
	//
	//		// Run write test
	//		run_throughput_test(single_io_size, false, num_threads,
	//				    test_duration);
	//
	//		// Run read test
	//		run_throughput_test(single_io_size, true, num_threads,
	//				    test_duration);
	//	} else {
	//		// Run tests with different IO sizes
	//		for (size_t io_size = MIN_IO_SIZE; io_size <= MAX_IO_SIZE;
	//		     io_size *= 4) {
	//			// Run write test
	//			run_throughput_test(io_size, false, num_threads,
	//					    test_duration);
	//
	//			// Run read test
	//			run_throughput_test(io_size, true, num_threads, test_duration);
	//		}
	//	}

	// Clean up
	printf("Shutting down NVMf storage engine...\n");
	nvmf_exit();

	// Free preloaded data
	free_preloaded_data();

	printf("Test completed successfully\n");
	return 0;
}