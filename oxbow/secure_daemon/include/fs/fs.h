#ifndef _FS_H_
#define _FS_H_
#include "bit_array.h"
#include "common/khash.h" /* used in macro*/
#include "common/shm.h"
#include "common/list.h"
#include "common/linux/oxbow_kernel.h"
#include "common/global.h"
#include "common/kerncompat.h"
#include "oxbow.h"
#include "common/sjd.h"

/* standard library */
#include <bits/pthreadtypes.h>
#include <stdatomic.h>
#include <sys/stat.h>
#include <stddef.h>
#include <sys/types.h>
#include <semaphore.h>

#define OXBOW_PATH_MAX 256

extern struct super_block *g_super_block;
extern int g_mount;
extern atomic_int g_file_worker_count;
extern atomic_int g_file_worker_exit_count;
extern atomic_int g_file_mmap_count;
extern atomic_int g_file_munmap_count;
extern atomic_int g_dir_worker_count;
extern atomic_int g_dir_worker_exit_count;

extern unsigned long g_nvme_ra_blknr;
extern unsigned long g_nvme_read_blknr;

#define LOG_GLOBAL_NVME_STATS()                                                \
	log_info("\n==== Global NVME Stats ====\n"                             \
		 "  g_nvme_read_blknr : %d\n"                                  \
		 "  g_nvme_read_IO(MB) : %lu\n",                               \
		 g_nvme_read_blknr, (g_nvme_read_blknr / 4) >> 10)

#define LOG_GLOBAL_WORKER_STATS()                                              \
	log_info("\n==== Global Worker Stats ====\n"                           \
		 "  g_file_worker_count      : %d\n"                           \
		 "  g_file_worker_exit_count : %d\n"                           \
		 "  g_dir_worker_count       : %d\n"                           \
		 "  g_dir_worker_exit_count  : %d\n"                           \
		 "  g_file_mmap_count        : %d\n"                           \
		 "  g_file_munmap_count      : %d\n",                          \
		 g_file_worker_count, g_file_worker_exit_count,                \
		 g_dir_worker_count, g_dir_worker_exit_count,                  \
		 g_file_mmap_count, g_file_munmap_count)

/* RA_STATS helpers (see fs/mpage.c) */
void mpage_ra_stats_dump(void);

/* journal.h */
typedef struct journal_transaction journal_tx;
typedef struct stage_transaction stage_tx;
typedef struct journal_worker j_worker_t;

typedef unsigned short umode_t;
typedef uint64_t sector_t;
typedef unsigned long pgoff_t;
typedef uint32_t uid_t;
typedef uint32_t gid_t;
typedef u_int32_t lba_map_t;

enum {
	LBA_NONE = 0,
	LBA_UPTODATE,
};

struct readahead_control {
	struct inode *inode;

	/* infos from kernel */
	kaddr_t kernel_ractl;
	pgoff_t index;
	unsigned int nr_pages;

	// other reason to not doing IO
	bool need;
};

struct read_control {
	struct inode *inode;
	int uffd;

	/* infos from kernel */
	kaddr_t folio;
	pgoff_t index;
};

struct buffer_extent {
	u64 iblock; // Logical block address.
	u32 nr;
	u64 lba; // Physical block address.
};

/* [NOTE] do not use such macro in secure daemon!
   it dereference base address so that it occur page fault
   I didn't find the reason but... Record it as unused macro */
#define PAGE_ADDR(s, idx) ((s->base) + (PAGE_SIZE * idx))
struct address_space {
	const struct address_space_operations *a_ops;
	/* lwext4: extent blocks, xattr blocks
	 * sefs: d_info_blocks, ei_blocks
	 */
	pthread_spinlock_t private_lock;
	struct list_head private_list; // filesystem private data blocks
	unsigned long nr_entries;
};

#define I_NEW (1 << 3)
#define I_SET_ONCE (1 << 4)
#define I_HAS_WORKER (1 << 5) // FIXME: Need renaming. No per-file worker anymore.

#define I_FAILED (1 << 6)
#define I_DELETED (1 << 7)
#define I_EVICT (1 << 8)

#define I_IO_ACTIVE (1 << 9) // Draining epoll event.
#define I_CLOSING (1 << 10) // Closing file worker.

#define I_DIRTY (1 << 12)

#define I_USE_JOURNAL (1 << 13) /* journal on */
#define I_IN_JTX_DIRTY_LIST (1 << 14) /* background journal dirty check */
#define I_IN_JTX_STG_LIST (1 << 15) /* background journal staged check */

struct inode_auth {
	mode_size_t i_comp;
	uid_t i_uid;
	gid_t i_gid;
} __attribute__((packed));

#define RANGE_DIRTY_ENT_MAX 340
typedef struct {
	pgoff_t start;
	int nr; // contiguous
} __attribute__((packed)) range_dirty;
struct inode_range_dirty {
	struct inode_range_dirty *next;
	range_dirty dirty_pages[RANGE_DIRTY_ENT_MAX];
	uint32_t count; // # of range_dirty entries.
	uint32_t dirty_blk_nr;
};

enum {
	DW_FLAG_INIT = 0,
	DW_FLAG_SYNC_ONGOING,
	DW_FLAG_JNL_CLEAN_READY,
	DW_FLAG_CLEANED_BY_JNL,
	DW_FLAG_CLEANED_BY_STG,
};

struct inode {
	/* Cached on-disk inode data */
	unsigned long i_ino;
	unsigned int i_nlink;
	umode_t i_mode;
	uid_t i_uid;
	gid_t i_gid;

	/* inode maintain snapshot of metadata */
	size_t i_size;
	struct timespec i_atime; // TOCHECK: Not used?
	struct timespec i_mtime; // TOCHECK: Not used?
	struct timespec i_ctime; // TOCHECK: Not used?
	blkcnt_t i_blocks;

	/* shared state among libFS and daemon */
	struct shm_shared_state *shm_header;

	/* dirty list used in staging */
	struct inode_range_dirty *i_stg_dirty_start;
	struct inode_range_dirty *
		i_stg_dirty_tail; // To avoid traversing from the start in add_range_dirty

	/* dirty list used in background journaling */
	struct inode_range_dirty *i_journal_dirty_start;
	struct inode_range_dirty *
		i_journal_dirty_tail; // To avoid traversing from the start in add_range_dirty

	/* journaling dirty list that has been requested but not yet completed
	 * NOTE: this list must be built after holding shared_inode_lock.
	 * It is built when building journal tx and cleared when a response from
	 * devfs arrives.
	 * One for each data fetcher.
	 */
	struct inode_range_dirty *i_journal_waiting_dirty_start[2];
	struct inode_range_dirty *i_journal_waiting_dirty_tail[2];

	/* Transaction ID of ongoing background journaling.
	 * -1: no ongoing bg commit.
	 * It is set in building journal tx and cleared when a response from
	 * devfs arrives. sync context checks this value to see whether it still
	 * needs to write (or wait for) the waiting dirty data.
	 */
	atomic_int i_journal_waiting_dirty_txid[2];

	// atomic update of i_journal_waiting_dirty_start and i_journal_waiting_dirty_txid.
	pthread_spinlock_t i_journal_waiting_dirty_lock[2];

#if (STG_WAIT_MODE == ALWAYS_WAIT)
	// Sleep/wake support for waiting on background journal completion.
	// Only in ALWAYS_WAIT mode.
	pthread_mutex_t i_journal_waiting_dirty_mutex[2];
	pthread_cond_t i_journal_waiting_dirty_cond[2];
#endif

	// Total number of waiting dirty blocks in the dirty list.
	uint32_t i_journal_waiting_nr_dirty_blks[2];

	// Total number of blocks needed to store the waiting dirty list.
	//
	// NOTE: We dump ird list instead of tag blocks to fix the number of
	// blocks required for staging in advance of building journal tx. It
	// reduces the lock contention.
	uint32_t i_journal_waiting_nr_ird_blks[2];

	/* Per-inode shared memory message ring (mmap'd from kernel) */
	struct oxbow_inode_msg_ring *msg_ring;

	/* concurrency managing */
	unsigned long i_state;

	// It indicates the recent journal transaction id that sets
	// I_IN_JTX_DIRTY_LIST or I_IN_JTX_STG_LIST bit.
	// A journal thread checks this value to decide whether the bits need to be reset.
	atomic_uint i_journal_txid;

	pthread_spinlock_t i_lock; // inode state
	pthread_rwlock_t i_idx_lock; // for inode index
	pthread_rwlock_t i_dir_lock; // since dir inode not have shm
	atomic_uint i_count;

	struct hlist_node i_hash;
	union {
		const struct file_operations *i_fop;
		void (*free_inode)(struct inode *);
	};
	const struct inode_operations *i_op;

	/* file data parts */
	struct address_space *i_mapping; // regular file data blocks
	void *data; // mmapped by daemon

	/* for journaling and staging */
	struct list_head i_journal_waiting_lists[2]; // for ongoing bg journaling dirty file list.
	struct list_head i_stage_list; // The list of stage txs of this inode.

	/* super-block related things */
	struct super_block *i_sb;
	struct list_head i_sb_list;

	/* Get from kernel */
	int fd; /* Opened in daemon, only once */
	int shm_index; // for later close
	int dofd; /* for directory operation from kernel */

	/* For profiling: current number of readahead pages in-flight
	 * (requested by kernel but not yet RA_END'ed) for this inode.
	 * Always present; only used when OXBOW_TRACK_TPUT is enabled.
	 */
	_Atomic uint32_t ra_inflight_pages;

	/* User-level readahead state:
	 * Highest page index (exclusive) that secure_daemon has already
	 * prefetched into SHM for this inode.
	 *
	 * This is a performance hint only and does not affect correctness.
	 */
	pgoff_t ra_user_prefetch_idx;

	/* User-level readahead cache.
	 *
	 * When enabled, secure_daemon can cache pages prefetched by
	 * user-level readahead in a per-inode buffer so that later
	 * READPAGE/RA requests from the kernel can be satisfied from
	 * this cache without issuing additional NVMe I/O.
	 *
	 * The cache is implemented as a fixed-size sliding window:
	 *   - ra_cache_buf   : contiguous buffer of N pages
	 *   - ra_cache_index : page indices for each slot
	 *   - ra_cache_valid : validity bitmap per slot
	 *
	 * Accesses are protected by ra_cache_lock.
	 */
	unsigned int ra_cache_nr_slots;
	void *ra_cache_buf;
	pgoff_t *ra_cache_index;
	unsigned char *ra_cache_valid;
	unsigned int ra_cache_cursor;
	pthread_spinlock_t ra_cache_lock;
};

static inline bool is_dirty_list_empty(struct inode_range_dirty *ird)
{
	bool ret = false;

	// The head of ird must not NULL.
	if(!ird) {
		oxb_warn("Checking the emptiness of the NULL ird.");
		return true;
	}

	ret = ird->count == 0;

	if (ret && ird->next != NULL)
		oxb_warn("The ird count is 0 but it has the next ird.");

	return ret;
}

static inline bool is_dirty_waiting_list_empty(struct inode *inode, int df_id)
{
	return is_dirty_list_empty(inode->i_journal_waiting_dirty_start[df_id]);
}


void print_inode_state(struct inode *);

/* Readahead cache helpers (fs/mpage.c) */
int inode_ra_cache_insert(struct inode *inode, pgoff_t idx, const void *src);
int inode_ra_cache_consume(struct inode *inode, pgoff_t idx);

#define SIZE_TO_BLK_NR(size) (size == 0 ? 0 : (size - 1) / PAGE_SIZE + 1)

struct inode_operations {
	struct inode *(*lookup)(struct inode *, const char *);
	int (*create)(struct inode *, struct inode **, mode_t, const char *);
	int (*unlink)(struct inode *dir, const char *);
	int (*rename_new)(struct inode *, const char *, unsigned long ino);
	int (*rename_old)(struct inode *, const char *, unsigned long ino);

	// Secure Daemon specific operations.

	/**
	 * @brief Allocate multiple blocks.
	 * 
	 * @param inode 
	 * @param be For optimizing. Don't need to return it the caller can
	 * retrieve be again by calling get_block.
	 * @param iblock 
	 * @param create 
	 * @return int 
	 */
	int (*get_blocks)(struct inode *inode, struct buffer_extent *be,
			  sector_t iblock, u32 create);

	/* Allocate single block. (if a file system does not support get_blocks,
	 * it falls back to get_block function.)
	 */
	// int (*get_block)(struct inode *, struct buffer_extent *, sector_t);
};

typedef int (*get_blocks_t)(struct inode *inode, struct buffer_extent *be,
			    sector_t iblock, u32 create);

struct file_operations {
	int (*iterate_shared)(struct inode *, unsigned long pos, char **, int *,
			      unsigned long **);
};

struct address_space_operations {
	int (*block_alloc)(struct inode *, size_t);
	void (*readpage)(struct read_control *);
	void (*readahead)(struct readahead_control *);
};

/* general filesystem infos */
struct super_block {
	const struct super_operations *s_op;

	/* filesystem static metadata */
	uint32_t filename_max;
	uint32_t nr_inodes;
	uint32_t nr_blocks; // TOCHECK: Not used?
	uint32_t inodes_per_blk;
	uint32_t max_subfiles; // TOCHECK: Not used?

	/* compatible with kernel */
	loff_t s_maxbytes;
	unsigned long s_magic;
	struct inode *s_root;
	struct block_device *bdev;

	void *s_fs_info; /* filesystem private data */

	/* objects for managinig file system */
	pthread_rwlock_t fs_lock; // lock for entire file system
	struct buffer_head *s_bh; // superblock itself
	pthread_spinlock_t s_inode_list_lock;
	struct list_head s_inodes; // all inodes

	/* filesystem private data
	 * ext4: block group descriptor blocks
	 */
	struct address_space s_mapping;

	/* default journal structure  will be NULL if not support journal */
	struct journal_control_ctx *journal;

	/* inode auth infos, shared memory with kernel */
	int oxbow_manager_fd;
	void *auth_mmap;
	size_t auth_mmap_len;
};

// Prevent other threads from modifying buffer caches while secure daemon is
// taking a snapshot.
#define FS_LOCK(sb) pthread_rwlock_wrlock(&(sb)->fs_lock)
#define FS_RDLOCK(sb) pthread_rwlock_rdlock(&(sb)->fs_lock)
#define FS_UNLOCK(sb) pthread_rwlock_unlock(&(sb)->fs_lock)

struct super_operations {
	struct inode *(*iget)(struct super_block *sb, unsigned long ino);
	struct inode *(*alloc_inode)(
		struct super_block *sb); // Allocate in-memory inode.
	void (*destroy_inode)(struct inode *);
	void (*free_inode)(struct inode *);
	int (*write_inode)(
		struct inode *); // Sync inode to disk or add to journal.
	int (*stage_inode)(struct inode *, struct journal_stage_header *);
	sector_t (*get_free_blocks)(struct super_block *,
				    u32 nr); // sefs specific
	void (*put_blocks)(struct super_block *, sector_t,
			   u32 nr); // sefs specific
	unsigned long (*get_free_inode)(struct super_block *); // sefs specific
	void (*put_inode)(struct super_block *,
			  unsigned long ino); // sefs specific
	int (*fill_journal_sb)(struct super_block *);

	// For retrieving metadata.
	baddr_t (*get_stage_sb_baddr)(void *superblock);
	uint32_t (*get_nr_stage_log_blocks)(void *superblock); // NOT including stage super block.
	baddr_t (*get_fs_area_start_baddr)(void *superblock);
	uint32_t (*get_nr_fs_area_blocks)(void *superblock); // including super block.
};

/* The file system specific init/exit function. */
extern int (*init_fs)(struct super_block *);
extern int (*exit_fs)(void);

/* super.c */
struct super_block *alloc_super_block(void);
sector_t get_free_blocks(struct super_block *, u32 nr);
void put_blocks(struct super_block *, sector_t bno, u32 nr);
unsigned long get_free_inode(struct super_block *);
void put_inode(struct super_block *, unsigned long ino);

/* inode.c */
int init_inode(void);
void inode_all_free(struct super_block *sb);
void iget(struct inode *);
struct inode *iget_locked(struct super_block *, unsigned long);
struct inode *ihold(unsigned long ino);
void destroy_inode(struct inode *, bool evict);
void evict_inode(struct inode *);
void iput(struct inode *);
int inode_idx_lock(struct inode *);
int inode_idx_lock_shared(struct inode *);
int inode_idx_unlock(struct inode *);
int inode_lock(struct inode *); // spin
int inode_unlock(struct inode *);
int inode_init_always(struct super_block *, struct inode *);
void unlock_new_inode(struct inode *);
void mark_inode_dirty(struct inode *);
void inode_add_journal(struct inode *);
int inode_alloc_blocks(struct inode *, struct buffer_extent *, loff_t old);

/* init.c */
int init_filesystem(const char *);
int exit_filesystem(void);

/* mpage.c */
void mpage_readahead(struct readahead_control *);
void mpage_readpage(struct read_control *);
int mpage_block_alloc(struct inode *, size_t);
void end_io_no_bio(int dfd, u64 folio);
void mpage_end_io_failed(pgoff_t start, int nr, struct readahead_control *rac);

/* sync.c */
int sync_fs(void);
int do_fsync(struct inode *);

/* dir.c */
int dir_inode_lock(struct inode *);
int dir_inode_rdlock(struct inode *);
int dir_inode_unlock(struct inode *);

/* posix.c */
int do_fstat(struct inode *);
int do_fallocate(struct inode *);
int do_ftruncate(struct inode *);

/* shm.c */
void log_shm_table(void);
int shared_inode_lock(struct inode *); // mutex
int shared_inode_unlock(struct inode *);
size_t get_recent_isize(struct inode *);
void set_oxbow_isize(struct inode *);
char *get_dirty_group_bmap(struct inode *);
char *get_dirty_lock_bmap(struct inode *, loff_t idx);
int inode_gather_dirty_pagebits(struct inode *, int is_journal, int df_id);
int inode_get_stg_dirty_blocks_nr(struct inode *, struct inode_range_dirty *ird);
void inode_unlock_pagebit(struct inode *, pgoff_t pg_idx);
int init_shm_system(void);
void exit_shm_system(void);
int init_shm_shared_state(struct inode *);
void free_shm_shared_state(struct inode *);
int check_page_in_shm(struct inode *, size_t pg_idx);
int check_uptodate_in_shm(struct inode *, size_t pg_idx);
void mark_uptodate_in_shm(struct inode *, size_t pg_idx);
void inode_set_shm_state(struct inode *, unsigned long flags);
void inode_unset_shm_state(struct inode *, unsigned long flags);
int get_proc_fd_map_ent(int pid, char *name, int *index);
int register_file_proc_fdmap(struct inode *inode, int fd, int pid);

/* ird helpers */
/**
 * Count how many blocks are needed to dump the entire IRD list
 * using the compact (delta,len) frame format.
 */
uint32_t count_ird_dump_blocks(const struct inode_range_dirty *ird);


/* workers.c */
int init_file_workers(void);
int init_dir_workers(void);

#endif
