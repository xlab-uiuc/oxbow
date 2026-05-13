#ifndef _OXBOW_DEBUG_H_
#define _OXBOW_DEBUG_H_

// Overwrite ENABLE_PRINT of log.c library.
// #define ENABLE_PRINT 0
#include "log.h"
#include <ctype.h>
#include <stdio.h>
#include <string.h>
#include <sys/syscall.h>
#include <unistd.h>

#define get_tid() syscall(__NR_gettid)

// Uncomment if you are in VM without devFS. Enabling this will disable staging
// and background journaling, doing in-place update.
// #define VM_ENV_NO_DEVFS

/*
 * Direct kernel-to-read-worker inline submit (D-2 design).
 *
 * When defined, dedicated read_workers handle epoll events themselves
 * and submit BIOs to their own SPDK qpair inline (no ring + pump
 * indirection for kernel-initiated reads). This collapses the read
 * pipeline from 2 wakeups (kernel -> file_worker -> iod_worker pump)
 * to 1 wakeup (kernel -> read_worker).
 *
 * See plans/2026-04-25-read-throughput-scalability-analysis.md.
 *
 * Strict invariant: when this is defined, iod_submit_bio(REQ_OP_READ)
 * may only be called from a read_worker (tls_inline_batch_active=1).
 * Other callers (sync_device.c admin debug) must use
 * iod_submit_bio_general() which falls back to the ring + pump path.
 */
#define OXBOW_RD_INLINE_SUBMIT

// Uncomment to disable background journaling. Only staging occurs.
// NOTE: There is no way to checkpoint for now.
// #define DISABLE_BACKGROUND_JOURNALING

/*
 * Uncomment to disable staging (a.k.a. fsync is noop)
 * It is useful when debugging background journaling.
 */
// #define NO_STAGING

/*
 * Uncomment to make data read I/O a no-op.
 * It immediately notifies the kernel that I/O is completed without
 * touching underlying storage. Useful for isolating memcpy/CPU bottlenecks.
 */
// #define OXBOW_NOOP_BIO_READ

// #define OXBOW_NOOP_MPAGE_BIO_SUBMIT

// #define OXBOW_NOOP_RD_SUBMIT_BIO

/*
 * Uncomment to make NVMe read submission a no-op.
 * It skips submitting to SPDK, which is useful to isolate NVMe software stack
 * overhead from the rest of the software stack.
 * NOTE: Enabling this will break data correctness (read contents undefined).
 */
// #define OXBOW_NOOP_BIO_NVME_READ

/*
 * Uncomment to make NVMe read completion memcpy a no-op.
 * It skips copying from SPDK DMA buffer to SHM/file page, which is useful
 * to isolate memcpy overhead from the rest of the NVMe software stack.
 * NOTE: Enabling this will break data correctness (read contents undefined).
 */
// #define OXBOW_NOOP_NVME_READ_COPY

/*
 * Optional: cap NVMe read I/O size (in KiB) for experiments.
 * If defined to a non-zero value, nvme.c will limit per-command read length
 * to this size even if the device supports larger I/Os.
 *
 * Example:
 *   #define OXBOW_EXPERIMENT_NVME_IO_SIZE_CAP_KB 64  // cap to 64 KiB
 */
// #define OXBOW_EXPERIMENT_NVME_IO_SIZE_CAP_KB 64

/*
 * Experimental: disable pipelined NVMe submit while polling completions.
 *
 * When enabled, nvme_submit_bio() will:
 *   - submit one batch of bios via submit_fn(),
 *   - poll completions until that batch fully completes,
 *   - and only then submit the next batch.
 *
 * This removes asynchronous submissions during the poll loop, which is
 * useful when debugging timing-sensitive interactions between BIO
 * submission and completion.
 * NOTE: Enabling this will drop performance a lot, throughput in particular.
 */
// #define OXBOW_NVME_NO_ASYNC_SUBMIT_DURING_POLL

/*
 * Experimental: enable user-level readahead in secure_daemon.
 * When enabled, mpage_readahead() will additionally prefetch pages ahead
 * of the kernel's readahead window and populate SHM so that future reads
 * can be served without extra NVMe I/O.
 *
 * OXBOW_USER_RA_WINDOW_MULT controls how many times larger the user-level
 * prefetch window is compared to the kernel-provided rac->nr_pages.
 *
 * For debugging READPAGE hangs, you can temporarily disable user-level
 * readahead by commenting out OXBOW_USER_READAHEAD below. Kernel
 * readahead (mpage_readahead) will still be active.
 *
 * If OXBOW_NOOP_USER_RA_IO is defined, user-level readahead will still
 * *plan* its window and update statistics, but it will not issue any
 * NVMe I/O. This is useful for isolating the overhead/benefit of
 * speculative user-level prefetching without changing kernel RA.
 */
// #define OXBOW_USER_READAHEAD

// (MULT-1)*ra_pages(=32 pages by default) should be less than or equal to
// ra_pages mount option.
// #define OXBOW_USER_RA_WINDOW_MULT 5

// #define OXBOW_NOOP_USER_RA_IO

/*
 * Debug option: fully disable kernel readahead path in mpage_readahead().
 *
 * When enabled, readahead windows from the kernel are not turned into BIOs;
 * instead, the daemon immediately reports RA_END for the whole window without
 * issuing any NVMe I/O.  READPAGE (single-page) path remains unchanged.
 *
 * Use this to isolate hangs caused by the READPAGE path: if hangs disappear
 * with kernel readahead disabled, we know interactions between mpage_readahead
 * and NVMe submission/completion are involved.
 *
 * Keep this commented out while debugging full RA (kernel + user-level),
 * so that mpage_readahead() issues real I/O and interacts with NVMe.
 */
// #define OXBOW_DISABLE_KERNEL_RA

/*
 * Make background journaling slower. (for example 10 seconds)
 * It is useful when debugging staging.
 */
// #define SLOW_BG_JOURNAL

/* Enable print messages */
#define PRINT_OXBOW_INFO
// #define PRINT_OXBOW_DEBUG
// #define PRINT_DAEMON_TRACE
// #define PRINT_DAEMON_DEBUG
#define PRINT_MPAGE_INFO
// #define PRINT_MPAGE_DEBUG
// #define PRINT_URA_DEBUG
// #define PRINT_SEFS_DEBUG
// #define PRINT_EXT4_DEBUG
// #define PRINT_INODE_DEBUG
// #define PRINT_MSG_INFO
// #define PRINT_MSG_DEBUG
// #define PRINT_SYNC_DEBUG
// #define PRINT_NVME_INFO
// #define PRINT_NVME_DEBUG
// #define PRINT_BIO_DEBUG // Not used?
// #define PRINT_BH_DEBUG
// #define PRINT_STAGE_DEBUG
// #define PRINT_STAGE_INFO
// #define PRINT_FILE_SYSTEM_INFO
// #define PRINT_FILE_SYSTEM_DEBUG
// #define PRINT_JOURNAL_INFO
// #define PRINT_JOURNAL_DEBUG

// #define PRINT_DUMP_BLK // Dump blocks.

/* Print hexdump of rdma (or shm) buffer. */
// #define DEBUG_HEXDUMP_JNL_DATA_PAGES

/* Print a transaction in rdma buffer. */
// #define DEBUG_PRINT_JNL_TX

/* Print a stage transaction. */
// #define DEBUG_PRINT_STG_TX

/* Enable extent tree logging to file */
// #define PRINT_EXT4_EXT_DEBUG
// #define PRINT_EXT4_EXT_TREE // Print extent tree.
// #define EXT4_EXT_LOG_TO_FILE

// printf color set.
#define PRINT_COLOR
#ifdef PRINT_COLOR
#define ANSI_COLOR_RED "\x1b[0;31m"
#define ANSI_COLOR_RED_BOLD "\x1b[1;31m"
#define ANSI_COLOR_GREEN "\x1b[0;32m"
#define ANSI_COLOR_GREEN_BOLD "\x1b[1;32m"
#define ANSI_COLOR_YELLOW "\x1b[0;33m"
#define ANSI_COLOR_YELLOW_BOLD "\x1b[1;33m"
#define ANSI_COLOR_BLUE "\x1b[0;34m"
#define ANSI_COLOR_BLUE_BOLD "\x1b[1;34m"
#define ANSI_COLOR_MAGENTA "\x1b[0;35m"
#define ANSI_COLOR_MAGENTA_BOLD "\x1b[1;35m"
#define ANSI_COLOR_CYAN "\x1b[0;36m"
#define ANSI_COLOR_CYAN_BOLD "\x1b[1;36m"
#define ANSI_COLOR_BRIGHT_RED "\x1b[0;91m"
#define ANSI_COLOR_BRIGHT_RED_BOLD "\x1b[1;91m"
#define ANSI_COLOR_BRIGHT_GREEN "\x1b[0;92m"
#define ANSI_COLOR_BRIGHT_GREEN_BOLD "\x1b[1;92m"
#define ANSI_COLOR_BRIGHT_YELLOW "\x1b[0;93m"
#define ANSI_COLOR_BRIGHT_YELLOW_BOLD "\x1b[1;93m"
#define ANSI_COLOR_BRIGHT_BLUE "\x1b[0;94m"
#define ANSI_COLOR_BRIGHT_BLUE_BOLD "\x1b[1;94m"
#define ANSI_COLOR_BRIGHT_MAGENTA "\x1b[0;95m"
#define ANSI_COLOR_BRIGHT_MAGENTA_BOLD "\x1b[1;95m"
#define ANSI_COLOR_BRIGHT_CYAN "\x1b[0;96m"
#define ANSI_COLOR_BRIGHT_CYAN_BOLD "\x1b[1;96m"
#define ANSI_COLOR_RESET "\x1b[0m"
#else
#define ANSI_COLOR_RED
#define ANSI_COLOR_RED_BOLD
#define ANSI_COLOR_GREEN
#define ANSI_COLOR_GREEN_BOLD
#define ANSI_COLOR_YELLOW
#define ANSI_COLOR_YELLOW_BOLD
#define ANSI_COLOR_BLUE
#define ANSI_COLOR_BLUE_BOLD
#define ANSI_COLOR_MAGENTA
#define ANSI_COLOR_MAGENTA_BOLD
#define ANSI_COLOR_CYAN
#define ANSI_COLOR_CYAN_BOLD
#define ANSI_COLOR_BRIGHT_RED
#define ANSI_COLOR_BRIGHT_RED_BOLD
#define ANSI_COLOR_BRIGHT_GREEN
#define ANSI_COLOR_BRIGHT_GREEN_BOLD
#define ANSI_COLOR_BRIGHT_YELLOW
#define ANSI_COLOR_BRIGHT_YELLOW_BOLD
#define ANSI_COLOR_BRIGHT_BLUE
#define ANSI_COLOR_BRIGHT_BLUE_BOLD
#define ANSI_COLOR_BRIGHT_MAGENTA
#define ANSI_COLOR_BRIGHT_MAGENTA_BOLD
#define ANSI_COLOR_BRIGHT_CYAN
#define ANSI_COLOR_BRIGHT_CYAN_BOLD
#define ANSI_COLOR_RESET
#endif

#ifdef PRINT_OXBOW_TRACE
#define oxb_trace(...) log_trace(__VA_ARGS__)
#else
#define oxb_trace(...)                                                         \
	do {                                                                   \
	} while (0)
#endif

#ifdef PRINT_OXBOW_DEBUG
#define oxb_debug(...) log_debug(__VA_ARGS__)
#else
#define oxb_debug(...)                                                         \
	do {                                                                   \
	} while (0)
#endif
#if defined(PRINT_OXBOW_INFO) || defined(PRINT_OXBOW_DEBUG)
#define oxb_info(...) log_info(__VA_ARGS__)
#else
#define oxb_info(...)                                                          \
	do {                                                                   \
	} while (0)
#endif
#define oxb_warn(...)                                                                   \
	do {                                                                            \
		fflush(stdout); /* Adjust as needed, e.g., to a specific file stream */ \
		log_warn(__VA_ARGS__);                                                  \
		fflush(stdout); /* Adjust as needed, e.g., to a specific file stream */ \
	} while (0)
#define oxb_error(...) log_error(__VA_ARGS__)
#define oxb_fatal(...) log_fatal(__VA_ARGS__)

#ifdef PRINT_DAEMON_TRACE
#define d_trace(...) log_trace(__VA_ARGS__)
#else
#define d_trace(...)                                                           \
	do {                                                                   \
	} while (0)
#endif

#ifdef PRINT_DAEMON_DEBUG
#define d_debug(...) log_debug(__VA_ARGS__)
#else
#define d_debug(...)                                                           \
	do {                                                                   \
	} while (0)
#endif
#define d_info(...) log_info(__VA_ARGS__)

#ifdef PRINT_MPAGE_DEBUG
#define mpage_dbg(...) log_debug(__VA_ARGS__)
#else
#define mpage_dbg(...)                                                         \
	do {                                                                   \
	} while (0)
#endif
#if defined(PRINT_MPAGE_INFO) || defined(PRINT_MPAGE_DEBUG)
#define mpage_info(...) log_info(__VA_ARGS__)
#else
#define mpage_info(...)                                                          \
	do {                                                                   \
	} while (0)
#endif

#ifdef PRINT_INODE_DEBUG
#define i_debug(...) log_debug(__VA_ARGS__)
#else
#define i_debug(...)                                                           \
	do {                                                                   \
	} while (0)
#endif

#ifdef PRINT_MSG_DEBUG
#define msg_trace(...) log_trace(__VA_ARGS__)
#define msg_debug(...) log_debug(__VA_ARGS__)
#else
#define msg_trace(...)                                                         \
	do {                                                                   \
	} while (0)
#define msg_debug(...)                                                         \
	do {                                                                   \
	} while (0)
#endif
#if defined(PRINT_MSG_INFO) || defined(PRINT_MSG_DEBUG)
#define msg_info(...) log_info(__VA_ARGS__)
#else
#define msg_info(...)                                                          \
	do {                                                                   \
	} while (0)
#endif
#define msg_warn(...) log_warn(__VA_ARGS__)
#define msg_error(...) log_error(__VA_ARGS__)
#define msg_fatal(...) log_fatal(__VA_ARGS__)

#ifdef PRINT_NVME_DEBUG
#define nvme_debug(...)                                                                 \
	do {                                                                            \
		fflush(stdout); /* Adjust as needed, e.g., to a specific file stream */ \
		log_debug(__VA_ARGS__);                                                 \
		fflush(stdout); /* Adjust as needed, e.g., to a specific file stream */ \
	} while (0)
#else
#define nvme_debug(...)                                                        \
	do {                                                                   \
	} while (0)
#endif
#if defined(PRINT_NVME_INFO) || defined(PRINT_NVME_DEBUG)
#define nvme_info(...) log_info(__VA_ARGS__)
#else
#define nvme_info(...)                                                         \
	do {                                                                   \
	} while (0)
#endif

#ifdef PRINT_SYNC_DEBUG
#define sync_debug(...)                                                                 \
	do {                                                                            \
		fflush(stdout); /* Adjust as needed, e.g., to a specific file stream */ \
		log_debug(__VA_ARGS__);                                                 \
		fflush(stdout); /* Adjust as needed, e.g., to a specific file stream */ \
	} while (0)
#else
#define sync_debug(...)                                                        \
	do {                                                                   \
	} while (0)
#endif

#ifdef PRINT_BH_DEBUG
#define bh_debug(...) log_debug(__VA_ARGS__)
#else
#define bh_debug(...)                                                          \
	do {                                                                   \
	} while (0)
#endif

#ifdef PRINT_STAGE_DEBUG
#define stg_debug(...) log_debug(__VA_ARGS__)
#else
#define stg_debug(...)                                                         \
	do {                                                                   \
	} while (0)
#endif

#if defined(PRINT_STAGE_INFO) || defined(PRINT_STAGE_DEBUG)
#define stg_info(...) log_info(__VA_ARGS__)
#else
#define stg_info(...)                                                          \
	do {                                                                   \
	} while (0)
#endif

#ifdef PRINT_SEFS_DEBUG
#define sefs_trace(...) log_trace(__VA_ARGS__)
#define sefs_debug(...) log_debug(__VA_ARGS__)
#else
#define sefs_trace(...)                                                        \
	do {                                                                   \
	} while (0)
#define sefs_debug(...)                                                        \
	do {                                                                   \
	} while (0)
#endif

#ifdef PRINT_EXT4_DEBUG
#define ext4_debug(...) log_debug(__VA_ARGS__)
#else
#define ext4_debug(...)                                                        \
	do {                                                                   \
	} while (0)
#endif

#ifdef PRINT_EXT4_EXT_DEBUG
#define ext4_ext_debug(...) log_debug(__VA_ARGS__)
#else
#define ext4_ext_debug(...)                                                    \
	do {                                                                   \
	} while (0)
#endif
#define ext4_ext_warn(...) log_warn(__VA_ARGS__)
#define ext4_ext_error(...) log_error(__VA_ARGS__)

#ifdef PRINT_FILE_SYSTEM_TRACE
#define fs_trace(...) log_trace(__VA_ARGS__)
#else
#define fs_trace(...)                                                          \
	do {                                                                   \
	} while (0)
#endif

#ifdef PRINT_FILE_SYSTEM_DEBUG
#define fs_debug(...) log_debug(__VA_ARGS__)
#else
#define fs_debug(...)                                                          \
	do {                                                                   \
	} while (0)
#endif
#if defined(PRINT_FILE_SYSTEM_INFO) || defined(PRINT_FILE_SYSTEM_DEBUG)
#define fs_info(...) log_info(__VA_ARGS__)
#else
#define fs_info(...)                                                           \
	do {                                                                   \
	} while (0)
#endif
#define fs_warn(...) log_warn(__VA_ARGS__)
#define fs_error(...) log_error(__VA_ARGS__)
#define fs_fatal(...) log_fatal(__VA_ARGS__)

#ifdef PRINT_URA_DEBUG
#define ura_debug(...) log_debug(__VA_ARGS__)
#else
#define ura_debug(...)                                                          \
	do {                                                                   \
	} while (0)
#endif

#ifdef PRINT_JOURNAL_DEBUG
#define jnl_trace(...) log_trace(__VA_ARGS__)
#define jnl_debug(...) log_debug(__VA_ARGS__)
#else
#define jnl_trace(...)                                                         \
	do {                                                                   \
	} while (0)
#define jnl_debug(...)                                                         \
	do {                                                                   \
	} while (0)
#endif

#if defined(PRINT_JOURNAL_INFO) || defined(PRINT_JOURNAL_DEBUG)
#define jnl_info(...) log_info(__VA_ARGS__)
#else
#define jnl_info(...)                                                          \
	do {                                                                   \
	} while (0)
#endif
#define jnl_warn(...) log_warn(__VA_ARGS__)
#define jnl_error(...) log_error(__VA_ARGS__)
#define jnl_fatal(...) log_fatal(__VA_ARGS__)

/**
 * @brief Hexdump a page of memory for debugging purposes
 * 
 * @param data Pointer to the data to dump
 * @param size Size of the data to dump (typically PAGE_SIZE)
 * @param label Label to print before the hexdump
 * @param offset_start Starting offset for display (for partial dumps)
 */
static inline void hexdump_page(const void *data, size_t size, const char *label, size_t offset_start)
{
	const unsigned char *bytes = (const unsigned char *)data;
	size_t i, j;
	
	if (!data || size == 0) {
		printf("[HEXDUMP] %s: NULL data or zero size\n", label);
		return;
	}

	// Limit dump size to avoid excessive output (max 512 bytes for debugging)
	size_t dump_size = (size > 512) ? 512 : size;
	
	printf("[HEXDUMP] %s (showing %zu/%zu bytes):\n", label, dump_size, size);
	
	for (i = 0; i < dump_size; i += 16) {
		char hex_line[128] = {0};
		char ascii_line[32] = {0};
		char full_line[256] = {0};
		
		// Build hex values
		for (j = 0; j < 16; j++) {
			if (i + j < dump_size) {
				sprintf(hex_line + strlen(hex_line), "%02x ", bytes[i + j]);
			} else {
				sprintf(hex_line + strlen(hex_line), "   ");
			}
		}
		
		// Build ASCII representation
		for (j = 0; j < 16 && (i + j) < dump_size; j++) {
			unsigned char c = bytes[i + j];
			ascii_line[j] = isprint(c) ? c : '.';
		}
		ascii_line[j] = '\0';
		
		// Combine into one line and print
		snprintf(full_line, sizeof(full_line), "%08zx: %s |%s|", 
				 offset_start + i, hex_line, ascii_line);
		printf("%s\n", full_line);
	}
	
	if (size > dump_size) {
		printf("[HEXDUMP] ... (truncated, %zu more bytes)\n", size - dump_size);
	}
}

/**
 * @brief Simplified hexdump for quick debugging
 * 
 * @param data Pointer to the data to dump
 * @param size Size of the data to dump
 * @param label Label to identify the dump
 */
static inline void quick_hexdump(const void *data, size_t size, const char *label)
{
	hexdump_page(data, size, label, 0);
}

#endif
