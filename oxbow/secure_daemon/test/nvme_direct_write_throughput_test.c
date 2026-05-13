/**
 * nvme_direct_write_throughput_test.c
 * 
 * A benchmark program to measure IO throughput of nvme_direct_write function.
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
#include <stdint.h> // Include for uint64_t definition
#include "io/nvme.h"
#include "thpool.h"

// Include debug functions
#include "oxbow_debug.h"
#include "fs/buffer_head.h"

struct thpool_ *thpool = NULL;

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
#define DEFAULT_NUM_THREADS 1
#define DEFAULT_SEQ_NR 256  // Default number of sequences to use
#define PRELOAD_SIZE (20ULL * 1024 * 1024 * 1024) // 20GB preload buffer

// Forward declarations
static void *monitoring_thread(void *arg);

// Default NVMe configurations if environment variables are not set
#define DEFAULT_NVME_PCIE_ADDR "0000:d8:00.1"

struct thread_args {
    int thread_id;
    size_t io_size;
    uint64_t start_time;
    uint64_t end_time;
    uint64_t total_bytes;
    uint64_t total_ios;
    int seq_nr;
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
        fprintf(stderr, "Failed to allocate preload buffer. Continuing without preloading.\n");
        return;
    }

    // Fill buffer with random data
//     printf("Filling preload buffer with random data...\n");
//     for (size_t i = 0; i < PRELOAD_SIZE; i += 1024*1024) {
//         // Fill in 1MB chunks to avoid excessive memory pressure
//         for (size_t j = 0; j < 1024*1024 && (i + j) < PRELOAD_SIZE; j++) {
//             g_preload_buffer[i + j] = (char)(rand() % 256);
//         }

//         // Print progress every 5GB
//         if (i % (5ULL * 1024 * 1024 * 1024) == 0 && i > 0) {
//             printf("Preloaded %zu GB\n", i / (1024 * 1024 * 1024));
//         }
//     }

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

// Fill NVMe buffer with data
static void fill_nvme_buffer(int thread_id, int seq_idx, unsigned int nr_blocks)
{
    char *buffer;
    size_t io_size = nr_blocks * OXBOW_BLOCK_SIZE;

    // Get the pre-allocated buffer from NVMe
    buffer = nvme_get_seq_buffer(seq_idx);
    if (!buffer) {
        fprintf(stderr, "Failed to get NVMe sequence buffer\n");
        return;
    }

    // Fill buffer with data from preloaded buffer if available
    if (g_preload_buffer) {
        // // Use different sections of the preloaded buffer for each thread
        // size_t offset = (thread_id * 1024 * 1024 * 1024 + seq_idx * 256 * 1024 * 1024) % PRELOAD_SIZE;
        
        // if (offset + io_size <= PRELOAD_SIZE) {
        //     // Copy continuous chunk if it fits
        //     memcpy(buffer, g_preload_buffer + offset, io_size);
        // } else {
        //     // Handle wrap-around case
        //     size_t first_part = PRELOAD_SIZE - offset;
        //     memcpy(buffer, g_preload_buffer + offset, first_part);
        //     memcpy(buffer + first_part, g_preload_buffer, io_size - first_part);
        // }
    } else {
        // Fallback to random data if preload buffer is not available
        // for (size_t i = 0; i < io_size; i++) {
        //     buffer[i] = (char)(rand() % 256);
        // }
    }
}

// Worker thread function
static void *worker_thread(void *arg)
{
    struct thread_args *args = (struct thread_args *)arg;
    uint64_t start_time, end_time;
    uint64_t elapsed_time;
    uint64_t total_ios = 0;
    uint64_t total_bytes = 0;
    baddr_t block_address;
    unsigned int nr_blocks;
    int seq_nr = args->seq_nr;
    int seq_max = nvme_get_seq_max();
    int tid = gettid();

    // Make sure we don't exceed maximum sequence count
    if (seq_nr > seq_max) {
        seq_nr = seq_max;
        printf("Thread %d: Requested seq_nr %d exceeds maximum %d, using maximum instead\n", 
               args->thread_id, args->seq_nr, seq_max);
    }

    // Calculate number of blocks for this IO size
    nr_blocks = args->io_size / OXBOW_BLOCK_SIZE;
    if (args->io_size % OXBOW_BLOCK_SIZE != 0) {
        nr_blocks++;
    }

    // Set different block addresses for different threads to avoid overlaps
    block_address = args->thread_id * nr_blocks * 100;

    printf("Thread %d: Using seq_nr=%d, block_address=%lu, nr_blocks=%u\n", 
           args->thread_id, seq_nr, block_address, nr_blocks);

    // Pre-fill all sequence buffers with test data
    for (int i = 0; i < seq_nr; i++) {
        fill_nvme_buffer(args->thread_id, i, nr_blocks);
    }

    // Keep track of time
    start_time = get_time_usec();
    args->start_time = start_time;

    while (1) {
        // Check if test duration has elapsed
        end_time = get_time_usec();
        elapsed_time = end_time - start_time;
        if (elapsed_time >= args->end_time) {
            break;
        }

        // Call nvme_direct_write
        nvme_direct_write(seq_nr, block_address, 8192, false);

        // printf("seq_nr: %d, block_address: %lu, nr_blocks: %u g_max_nvme_max_io_size: %u\n", seq_nr, block_address, nr_blocks, g_max_nvme_max_io_size);

        // Update statistics
        total_ios++;
        total_bytes += args->io_size;

        // Update global statistics for real-time monitoring
        pthread_spin_lock(&g_stats.lock);
        g_stats.bytes_completed += args->io_size;
        g_stats.ios_completed++;
        pthread_spin_unlock(&g_stats.lock);

        // Move to next block address (wrap around if needed)
        block_address += nr_blocks;
        if (block_address > args->thread_id * nr_blocks * 100 + nr_blocks * 1000) {
            block_address = args->thread_id * nr_blocks * 100;
        }
    }

    args->end_time = end_time;
    args->total_bytes = total_bytes;
    args->total_ios = total_ios;

    printf("Thread %d finished. total_bytes: %lu total_ios: %lu\n",
           args->thread_id, total_bytes, total_ios);

    return NULL;
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
            mb_per_sec = ((double)bytes_since_last / (1024.0 * 1024.0)) /
                         ((double)elapsed_time / 1000000.0);
            iops = (double)ios_since_last / ((double)elapsed_time / 1000000.0);

            printf("Real-time stats: %.2f MB/s, %.2f IOPS\n", mb_per_sec, iops);
        }
    }

    return NULL;
}

// Run throughput test with specific parameters
static void run_throughput_test(size_t io_size, int num_threads, int test_duration, int seq_nr)
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

    printf("Running nvme_direct_write test with IO size: %zu bytes, Threads: %d, Duration: %d seconds, Seq_nr: %d\n",
           io_size, num_threads, test_duration, seq_nr);

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
    if (pthread_create(&monitor_thread, NULL, monitoring_thread, NULL) != 0) {
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
        args[i].seq_nr = seq_nr;
        args[i].end_time = test_duration * 1000000ULL; // Convert seconds to microseconds

        thpool_add_work(thpool, worker_thread, &args[i]);

        // if (pthread_create(&threads[i], NULL, worker_thread, &args[i]) != 0) {
        //     fprintf(stderr, "Failed to create thread %d\n", i);
        //     // Continue with fewer threads
        // }
    }

    // Wait for all threads to complete
    for (int i = 0; i < num_threads; i++) {
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
    throughput_mbs = ((double)total_bytes / (1024.0 * 1024.0)) / elapsed_sec;
    iops = (double)total_ios / elapsed_sec;

    // Print results
    printf("Test Results:\n");
    printf("  IO Type:       WRITE (nvme_direct_write)\n");
    printf("  IO Size:       %zu bytes\n", io_size);
    printf("  Total IOs:     %lu\n", total_ios);
    printf("  Total Bytes:   %lu bytes (%.2f MB)\n", total_bytes, (double)total_bytes / (1024.0 * 1024.0));
    printf("  Elapsed Time:  %.2f seconds\n", elapsed_sec);
    printf("  Throughput:    %.2f MB/s\n", throughput_mbs);
    printf("  IOPS:          %.2f IO/s\n", iops);
    printf("\n");

    // Clean up
    pthread_spin_destroy(&g_stats.lock);
    free(threads);
    free(args);
}

// Get an environment variable with a default value if not set
static const char *get_env_with_default(const char *name, const char *default_value)
{
    const char *value = getenv(name);
    return value ? value : default_value;
}

int main(int argc, char *argv[])
{
    struct nvme_config nvme_conf;
    int num_threads = DEFAULT_NUM_THREADS;
    int test_duration = DEFAULT_TEST_DURATION;
    const char *nvme_pcie_addr;
    size_t single_io_size = 0; // 0 means run with multiple IO sizes
    bool skip_preload = false;
    int seq_nr = DEFAULT_SEQ_NR;
    
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
                    fprintf(stderr, "Invalid IO size unit: %s\n", endptr);
                    fprintf(stderr, "Valid units are: K, M, G (case insensitive)\n");
                    return 1;
                }
            }
            i++;
        } else if (strcmp(argv[i], "--seq-nr") == 0 && i + 1 < argc) {
            seq_nr = atoi(argv[i + 1]);
            i++;
        } else if (strcmp(argv[i], "--skip-preload") == 0) {
            skip_preload = true;
        } else if (strcmp(argv[i], "--help") == 0) {
            printf("Usage: %s [OPTIONS]\n", argv[0]);
            printf("Options:\n");
            printf("  --threads N       Number of worker threads (default: %d)\n", DEFAULT_NUM_THREADS);
            printf("  --duration N      Test duration in seconds (default: %d)\n", DEFAULT_TEST_DURATION);
            printf("  --io-size N[K|M|G] Use a single IO size instead of multiple sizes\n");
            printf("                    Units: K=KiB, M=MiB, G=GiB (e.g., 4K, 1M)\n");
            printf("                    If not specified, tests run with multiple IO sizes\n");
            printf("  --seq-nr N        Number of sequences to use for nvme_direct_write (default: %d)\n", DEFAULT_SEQ_NR);
            printf("  --skip-preload    Skip preloading 20GB of data into memory\n");
            printf("  --help            Display this help message\n");
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
    nvme_pcie_addr = get_env_with_default("nvme_pcie_addr", DEFAULT_NVME_PCIE_ADDR);

    // Configure NVMe. This test drives a single VF; wrap the BDF into a
    // 1-element list for set_nvme_config_list().
    {
        char *addrs[1] = { (char *)nvme_pcie_addr };
        set_nvme_config_list(&nvme_conf, addrs, 1);
    }

    // Initialize NVMe (test has no D-2 read_workers, so iod == total)
    printf("Initializing NVMe storage engine...\n");
    thpool = nvme_init(&nvme_conf, num_threads, num_threads);
    if (!thpool) {
        fprintf(stderr, "Failed to initialize NVMe storage engine\n");
        return 1;
    }

    printf("nvme_direct_write throughput test\n");
    printf("================================\n");
    printf("NVMe PCIe Address: %s\n", nvme_conf.pcie_addrs[0]);
    printf("Worker Threads:    %d\n", num_threads);
    printf("Test Duration:     %d seconds\n", test_duration);
    printf("Sequence Count:    %d\n", seq_nr);

    single_io_size = 32 * 1024 * 1024;

    if (single_io_size > 0) {
        printf("IO Size:           %zu bytes", single_io_size);
        if (single_io_size >= 1024 * 1024 * 1024) {
            printf(" (%.2f GiB)\n", (double)single_io_size / (1024 * 1024 * 1024));
        } else if (single_io_size >= 1024 * 1024) {
            printf(" (%.2f MiB)\n", (double)single_io_size / (1024 * 1024));
        } else if (single_io_size >= 1024) {
            printf(" (%.2f KiB)\n", (double)single_io_size / 1024);
        } else {
            printf("\n");
        }
    } else {
        printf("IO Sizes:          Multiple sizes (%d B to %d MB)\n", MIN_IO_SIZE, MAX_IO_SIZE / (1024 * 1024));
    }
    printf("\n");

    // if (single_io_size > 0) {
        // Run test with a single IO size
        run_throughput_test(single_io_size, num_threads, test_duration, seq_nr);
    // } else {
    //     // Run tests with different IO sizes
    //     for (size_t io_size = MIN_IO_SIZE; io_size <= MAX_IO_SIZE; io_size *= 4) {
    //         run_throughput_test(io_size, num_threads, test_duration, seq_nr);
    //     }
    // }

    // Clean up
    printf("Shutting down NVMe storage engine...\n");
    nvme_exit();

    // Free preloaded data
    free_preloaded_data();

    printf("Test completed successfully\n");
    return 0;
} 