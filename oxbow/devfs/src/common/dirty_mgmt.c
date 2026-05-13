#include "dirty_mgmt.h"
#include "oxbow_debug.h"

#include <dirent.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <stdatomic.h>
#include <time.h>

/*
 * Dirty page counter is used to track the number of dirty pages in the
 * system. It is used to determine when to flush the dirty pages to the
 * disk.
 *
 * NOTE: It does not count the exact number of dirty pages (especially resetting
 * the counters). It only estimates the number of dirty pages to frequently
 * trigger background journaling based on the dirty ratio.
 */

#define TOTAL_DIRTY_COUNTER_FILE "total_dirty_counter"

// Each process updates the global counter every 10MB of dirty pages.
// #define DIRTY_PAGE_UPDATE_GLOBAL_THRESHOLD 4096 // 4096 pages = 16MB
// #define DIRTY_PAGE_UPDATE_GLOBAL_THRESHOLD 2560 // 2560 pages = 10MB
#define DIRTY_PAGE_UPDATE_GLOBAL_THRESHOLD 1024 // 1024 pages = 4MB

/* Threshold for dirty pages to trigger journal (16384 pages = 64MB == DATA_FETCHER_BUF_SIZE) */
// #define DIRTY_PAGE_THRESHOLD 16384
// #define DIRTY_PAGE_THRESHOLD 25600 // 25600 pages = 100MB
#define DIRTY_PAGE_THRESHOLD 4096 // 4096 pages = 16MB

/* Check interval for dirty page threshold in milliseconds */
#define DIRTY_CHECK_INTERVAL_MS 500

struct dirty_page_counter {
	atomic_ulong count;
	atomic_ulong prev_count;
};

static struct dirty_page_counter proc_dirty_counter;
static struct dirty_page_counter *global_dirty_counter = NULL;

/**
 * @brief Initialize the global dirty counter
 * 
 * @return int 0 on success, negative on failure
 */
static int init_global_dirty_counter(void)
{
	int fd;

	// Create or open the total counter file
	fd = shm_open(TOTAL_DIRTY_COUNTER_FILE, O_CREAT | O_RDWR, 0666);
	if (fd == -1) {
		perror("Failed to create total dirty counter file");
		return -1;
	}

	// Set file size
	if (ftruncate(fd, sizeof(struct dirty_page_counter)) == -1) {
		perror("ftruncate failed for total dirty counter");
		close(fd);
		return -1;
	}

	global_dirty_counter = mmap(NULL, sizeof(struct dirty_page_counter),
				    PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);

	if (global_dirty_counter == MAP_FAILED) {
		perror("mmap failed for total dirty counter");
		close(fd);
		return -1;
	}

	// Initialize the counter
	atomic_init(&global_dirty_counter->count, 0);
	atomic_init(&global_dirty_counter->prev_count, 0);

	close(fd);

	return 0;
}

/**
 * @brief Increment the dirty page counter for this process
 * 
 * @param count Number of pages to add
 */
void increment_dirty_page_count(unsigned long count)
{
	unsigned long priv_total_prev, priv_total_cur, diff;

	priv_total_prev = atomic_load(&proc_dirty_counter.prev_count);
	priv_total_cur = atomic_fetch_add(&proc_dirty_counter.count, count);
	priv_total_cur += count; // Upto this point.

	diff = (priv_total_cur >= priv_total_prev) ?
		       (priv_total_cur - priv_total_prev) :
		       0;

	// Update the global counter if the threshold is reached.
	if (diff >= DIRTY_PAGE_UPDATE_GLOBAL_THRESHOLD) {
		atomic_store(&proc_dirty_counter.prev_count, priv_total_cur);

		// Also update the total counter (increase from the last
		// update).
		// Increase by multiple of DIRTY_PAGE_UPDATE_GLOBAL_THRESHOLD.
		atomic_fetch_add(&global_dirty_counter->count,
				 diff / DIRTY_PAGE_UPDATE_GLOBAL_THRESHOLD *
					 DIRTY_PAGE_UPDATE_GLOBAL_THRESHOLD);
	}
}

/**
 * @brief Reset process dirty page counter to 0 and update global counter
 * 
 * This function clears the process dirty counter and decrements the global counter
 * by the corresponding amount in multiples of DIRTY_PAGE_UPDATE_GLOBAL_THRESHOLD.
 */
void clear_process_dirty_counter(void)
{
	unsigned long priv_prev;
	unsigned long to_subtract, current_count;

	// Decrement the global counter by its contribution.
	priv_prev = atomic_load(&proc_dirty_counter.prev_count);
	to_subtract = priv_prev / DIRTY_PAGE_UPDATE_GLOBAL_THRESHOLD *
		      DIRTY_PAGE_UPDATE_GLOBAL_THRESHOLD;

	// Check if we can safely subtract without underflow
	current_count = atomic_load(&global_dirty_counter->count);
	if (current_count >= to_subtract) {
		atomic_fetch_sub(&global_dirty_counter->count, to_subtract);
	} else {
		// If not enough to subtract, just set to 0
		atomic_store(&global_dirty_counter->count, 0);
	}

	// Reset process counter to 0
	atomic_store(&proc_dirty_counter.count, 0);
	atomic_store(&proc_dirty_counter.prev_count, 0);
}

/**
 * @brief Initialize the dirty page management system
 * 
 * @return int 0 on success, negative on failure
 */
int init_dirty_mgmt(void)
{
	int ret;

	ret = init_global_dirty_counter();
	if (ret < 0)
		oxb_error("Failed to create dirty page counter.");

	atomic_init(&proc_dirty_counter.count, 0);
	atomic_init(&proc_dirty_counter.prev_count, 0);

	return ret;
}

void exit_dirty_mgmt_libfs(void)
{
	// Free total dirty counter if it exists
	if (global_dirty_counter) {
		munmap(global_dirty_counter, sizeof(struct dirty_page_counter));
		global_dirty_counter = NULL;
	}

	oxb_info("Dirty page management cleanup done");
}

/**
 * @brief Clean up the dirty page management system
 */
void exit_dirty_mgmt(void)
{
	// Free total dirty counter if it exists
	if (global_dirty_counter) {
		munmap(global_dirty_counter, sizeof(struct dirty_page_counter));
		global_dirty_counter = NULL;

		// Unlink the shared memory object
		shm_unlink(TOTAL_DIRTY_COUNTER_FILE);
	}

	oxb_info("Dirty page management cleanup done");
}

/**
 * @brief Check if global dirty count exceeds threshold and reset if needed
 * 
 * This function checks if the global dirty count exceeds the threshold
 * and resets the counter if it does.
 * 
 * @return int 1 if threshold was exceeded and counter was reset, 0 otherwise
 */
int check_and_reset_global_dirty_threshold(void)
{
	unsigned long current_count;

	// Check if threshold is exceeded
	if (global_dirty_counter == NULL) {
		oxb_warn("Global dirty counter is NULL. We do not count dirty pages.");
		return 0;
	}

	current_count = atomic_load(&global_dirty_counter->count);

	// oxb_warn("current_global_dirty_count=%lu", current_count);

	if (current_count >= DIRTY_PAGE_THRESHOLD) {
		// Reset the global counter
		atomic_store(&global_dirty_counter->count, 0);

		// Also reset the prev_count in the global counter to avoid inconsistency
		atomic_store(&global_dirty_counter->prev_count, 0);

		oxb_debug(
			"Global dirty count exceeded threshold. Resetting to 0. (current_cnt=%lu)",
			current_count);

		return 1;
	}

	return 0;
}
