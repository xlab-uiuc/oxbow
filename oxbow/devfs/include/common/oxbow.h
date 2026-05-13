#ifndef _OXBOW_H_
#define _OXBOW_H_

// Global things related to Oxbow file system.
#include <stdio.h> // oxbow_assert
#include <stdint.h>
#include <time.h>
#include "global.h"

typedef uint64_t baddr_t; // Block address. (A.k.a. Block number)

#ifndef OXBOW_BLOCK_SIZE // Also defined in include/common/linux/oxbow_kernel.h
#define OXBOW_BLOCK_SIZE (4096UL) // 4 KB
#endif
#define OXBOW_BLOCK_SIZE_SHIFT 12 // 2^12 = 4096
#define OXBOW_SUPER_BLOCK_NR 0 // Shared between Secure Daemon and DevFS.
#define OXBOW_JOURNAL_INIT_TXID 1 // Initial tx_id of journal super block.
#define SECTOR_SIZE (512UL) // 512 B
#define SECTOR_SIZE_SHIFT 9 // 2^9 = 512
#define OXBOW_MAX_LIBFS_NUM 128 // Cf. MAX_CLIENT_CONNECTION of rpc library.
#define OXBOW_PREFIX "/oxbow"

// Size of one data fetcher buffer. Note that there are two buffers.
// It should match BIO_MAX_BIO_SIZE in bio.h.
#define DATA_FETCHER_BUF_SIZE (32UL * 1024 * 1024) // 32 MB
#define DATA_FETCHER_BUF_CNT 80
// #define DATA_FETCHER_BUF_SIZE (64UL * 1024 * 1024) // 64 MB
// #define DATA_FETCHER_BUF_CNT 64

#define MAX_CONF_MICROBENCH 1
#define MAX_CONF_FILEBENCH 2
#define MAX_CONF_LEVELDB 3

// Set proper configuration for max shm and max file size.
#define MAX_CONFIG MAX_CONF_MICROBENCH

#if (MAX_CONFIG == MAX_CONF_MICROBENCH)
#define OXBOW_MAX_SHM 1000
// #define OXBOW_MAX_FILE_SIZE (1 << 30) /* 1GB */
#define OXBOW_MAX_FILE_SIZE (OXBOW_FILE_SIZE_MAX)

#elif (MAX_CONFIG == MAX_CONF_FILEBENCH)
/* When you running filebench: application that require lot of files */
#define OXBOW_MAX_SHM 50000 // Filebench configuration.
#define OXBOW_MAX_FILE_SIZE (2UL * (1 << 30)) /* 2GB */

#elif (MAX_CONFIG == MAX_CONF_LEVELDB)
#define OXBOW_MAX_SHM 10000 // Leveldb configuration.
#define OXBOW_MAX_FILE_SIZE (5UL * (1 << 30)) /* 5GB */
#endif

#define OXBOW_DIRWORKER_NR 1
#define OXBOW_FILEWORKER_NR 4 /* Used only when OXBOW_RD_INLINE_SUBMIT is OFF. */
/*
 * Compile-time upper bound on the D-2 read worker pool size.
 *
 * The actual pool size at runtime is `g_sd_conf.read_worker_thread_num`
 * (configured via `read_worker_thread_num` in secure_daemon_conf.sh /
 * myconf.sh). This macro only bounds the static pthread_t array and
 * the sum constraint with iod_workers, it is NOT the working value.
 *
 * Sizing constraints at runtime:
 *   - storage_engine_thread_num + read_worker_thread_num == total qpair
 *     count. OXBOW_TOTAL_IO_THREAD_NR_MAX bounds the total static qpair
 *     slots. With the NUMA1 primary+HT pinning policy, totals above 16
 *     spill onto NUMA1 HT siblings (CPU 48..63), not NUMA0.
 *   - read_worker_thread_num must also be <= OXBOW_RD_WORKER_NR_MAX.
 *   - For read-heavy workloads, prefer giving more cores to read_workers
 *     (e.g., iod=6 + read=10) by reducing storage_engine_thread_num.
 */
#define OXBOW_RD_WORKER_NR_MAX 16
#define OXBOW_TOTAL_IO_THREAD_NR_MAX 24
#define INLINE_BATCH_MAX 64 /* Per-pump BIO batch cap for inline submit. */

// maximum open file descriptor for process
// #define OXBOW_MAX_OPEN_FILE 1024
#define OXBOW_MAX_OPEN_FILE 16384
#define OXBOW_MAX_PROC 100

/* If HOST_JOURNALING is enabled, RPC SHMEM is used between DevFS and Secure
Daemon. SHMEM channel is used for data fetcher as well. */
// #define HOST_JOURNALING // Run DevFS on the host.
#ifdef HOST_JOURNALING
#define DATA_FETCHER_CHANNEL_MODE 2 // 1: RDMA(remote), 2: shared memory(local)

// NOTE: /dev/hugepages is the path where the hugepages are mounted (by script: scripts/host/setup_spdk.sh)
#define DATA_FETCHER_SHM_PATH "/dev/hugepages/oxbow_df_shm"

// If set, DevFS uses nvme instead of nvmf for host journaling.
// Note that ASYNC_DISPATCH should be disabled. (not supported)
#define USE_NVME_STORAGE_ENGINE

#else
#define DATA_FETCHER_CHANNEL_MODE 1 // 1: RDMA(remote), 2: shared memory(local)
#endif

/* Enable coalescing in checkpointing. */
#define OPTIMIZE_CHECKPOINT // Note that ASYNC_DISPATCH should be disabled. (not supported)

/* 
 *  Checkpoint with coalescing may create excessive BIO buffers due to its long tag list.
 *  Set this to limit the maximum buffer size for one I/O dispatch.
 *  Note: this is not a strict limit to accommodate at least one tag node.
 */
// #define OPTIMIZE_CHECKPOINT_MAX_BUF_SIZE (1 * 1024 * 1024 * 1024) // 1GB
#define OPTIMIZE_CHECKPOINT_MAX_BUF_SIZE (5 * 1024 * 1024 * 1024) // 5GB

/* Stage policy in fsync path.*/
#define ALWAYS_WAIT 0
#define ALWAYS_PERSIST 1
#define ADAPTIVE_WAIT 2
#define NO_WRITE 3

/* Choose one of the following. */
// #define STG_WAIT_MODE ALWAYS_WAIT
#define STG_WAIT_MODE ALWAYS_PERSIST
// #define STG_WAIT_MODE ADAPTIVE_WAIT
// #define STG_WAIT_MODE NO_WRITE

/** RPC Related. */
// Format: RPC_SHMEM_PATH_<client>_<server>
#define RPC_SHMEM_CM_PATH_LIBFS_DAEMON                                         \
	oxbow_rpc_libfs_daemon // If you change this name, change secure_daemon/run.sh as well.
#define RPC_SHMEM_SEED_LIBFS_DAEMON 5400 // Arbitrary number.
#ifdef HOST_JOURNALING
#define RPC_SHMEM_SEED_DAEMON_DEVFS 4400 // Arbitrary number.
#define RPC_SHMEM_CM_PATH_DAEMON_DEVFS                                         \
	oxbow_rpc_daemon_devfs // If you change this name, change devfs/run.sh as well.
#endif

// We limit the number of inodes to 128M due to the size limit of
// kmalloc. We have to allocate shared memory for inode_auth of each inode.
// (https://github.com/casys-kaist-internal/oxbow.code/issues/85)
//
// For this, we limit the total device size to 2TB at file_dev_open().
#define OXBOW_MAX_INODE_CNT (128UL * 1024 * 1024) // 128M
#define OXBOW_MAX_DEV_SIZE (2UL * 1024 * 1024 * 1024 * 1024) // 2 TB

/* Manage journal area in a separate device.
 * This is for test. We may give a different device for background journaling to
 * DevFS and make DevFS manage the journal area directly. For example, it mkfs
 * independently from the Secure Daemon (devfs/mkfs/mkfs_journal.c).
 */
// #define SEPARATE_BG_JOURNAL

#ifdef SEPARATE_BG_JOURNAL
#define OXBOW_JOURNAL_SB_BADDR                                                 \
	0 // Journal super block address. TODO: static for now.
#define OXBOW_JOURNAL_AREA_SIZE (1024UL * 1024 * 1024) // 1GB
#define OXBOW_JOURNAL_PADDING_SIZE 1024 // 1KB
#define OXBOW_MAGIC 0x5b2a0853U // Random. (hexdump -C -n 256 /dev/random)
#endif

/** Constants for sync_device */
#define STAGE_AREA_FILE_NAME "/tmp/stage_area.dump"
#define FS_AREA_FILE_NAME "/tmp/fs_area.dump"
#define CEILING(x, y) (((x) + (y) - 1) / (y))

/** For experiments. */
#define EXP_FLAG_DIR "/mnt/oxbow_flag"
#define FLAG_FILE_CKPT_DONE EXP_FLAG_DIR "/ckpt_done"
#define FLAG_FILE_DEVFS_STATUS EXP_FLAG_DIR "/devfs_status"
#define FLAG_FILE_DAEMON_STATUS EXP_FLAG_DIR "/daemon_status"

static inline uint32_t byte_to_lba_cnt(uint64_t size_in_bytes)
{
	return CEILING(size_in_bytes, SECTOR_SIZE);
}

static inline size_t nblks_to_bytes(uint32_t n_blks)
{
	// NOTE: casting is required.
	return ((uint64_t)n_blks) << OXBOW_BLOCK_SIZE_SHIFT;
}

/**
 * @brief It does not check the alignment.
 * 
 * @param size_in_bytes 
 * @return uint32_t 
 */
static inline uint32_t bytes_to_nblks(size_t size_in_bytes)
{
	// return CEILING(size_in_bytes, OXBOW_BLOCK_SIZE_SHIFT);
	oxbow_assert(size_in_bytes % OXBOW_BLOCK_SIZE == 0);

	return (uint32_t)(size_in_bytes >> OXBOW_BLOCK_SIZE_SHIFT);
}

static inline uint32_t baddr_to_lba(baddr_t baddr)
{
	return baddr * (OXBOW_BLOCK_SIZE / SECTOR_SIZE);
}

static inline void timestamp_now(struct timespec *ts)
{
	// JBD2 calls ktime_get_coarse_real_ts64() that corresponds to CLOCK_REALTIME_COARSE.
	clock_gettime(CLOCK_REALTIME_COARSE, ts);
}

#endif
