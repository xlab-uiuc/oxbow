#ifndef __DIRTY_MGMT_H__
#define __DIRTY_MGMT_H__

#include <sys/types.h>

/**
 * @brief Create per-process dirty page counter
 * 
 * @param pid Process ID
 * @return int 0 on success, negative on failure
 */
int create_process_dirty_counter(pid_t pid);

/**
 * @brief Increment the dirty page counter for this process
 * 
 * @param count Number of pages to add
 */
void increment_dirty_page_count(unsigned long count);

/**
 * @brief Free the process dirty counter
 */
void free_process_dirty_counter(void);

/**
 * @brief Reset process dirty page counter to 0 and update global counter
 * 
 * This function clears the process dirty counter and decrements the global counter
 * by the corresponding amount in multiples of DIRTY_PAGE_UPDATE_GLOBAL_THRESHOLD.
 */
void clear_process_dirty_counter(void);

/**
 * @brief Get total dirty page count across all processes
 * 
 * @return unsigned long Total dirty page count
 */
unsigned long get_total_dirty_page_count(void);

/**
 * @brief Check if global dirty count exceeds threshold and reset if needed
 * 
 * This function checks if the global dirty count exceeds a threshold (25600 pages = 100MB)
 * at a configurable interval (default 500ms) and resets the counter if it does.
 * The check interval is defined by DIRTY_CHECK_INTERVAL_MS in milliseconds.
 * 
 * @return int 1 if threshold was exceeded and counter was reset, 0 otherwise
 */
int check_and_reset_global_dirty_threshold(void);

/**
 * @brief Initialize the dirty page management system
 * 
 * @return int 0 on success, negative on failure
 */
int init_dirty_mgmt(void);

/**
 * @brief Clean up the dirty page management system
 */
void exit_dirty_mgmt(void);

/**
 * @brief Clean up the dirty page management system for libfs (per process)
 */
void exit_dirty_mgmt_libfs(void);

#endif /* __DIRTY_MGMT_H__ */