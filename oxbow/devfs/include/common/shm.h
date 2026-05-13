#ifndef _SH_INODE_H_
#define _SH_INODE_H_
#include <stddef.h>
#include <stdatomic.h> /* used in macro */
#include <sys/cdefs.h>
#include <sys/types.h>
#include <pthread.h>
#include "oxbow_debug.h"
#include "common/linux/oxbow_kernel.h"
#include "bit_array.h"
#include "oxbow.h"
#include "global.h"

#ifdef SYSV_SHM

/*  PDL_Bitmap (Page dirty & locking bitmap), two-bit bitmap
 *
 *  E.g. one byte configuration of PDL_bitmap.
	Bit:     7 6 5 4   3 2 1 0
			-+-+-+-+---+-+-+-+-
	Usage:   Dirty   | Locking
	Value:   1 1 1 1   0 0 0 0
	       p3,p2,p1,p0
*/

/* Page Mapping Table  
 *	: stores mapping of key, which is related to DL_bitmap */

// +--------+--------------------+
// | dirty? | index of key_array |
// +--------+--------------------+
// | dirty? | index of key_array |  <---- use for key_array
// +--------+--------------------+
//  		........
// +--------+--------------------+
// | dirty? | index of key_array |
// +--------+--------------------+
//
// One entry covers 16384 pages it is about 64MiB.
// 2048 entries per one table, It covers about 128GiB.

/* Key Array */

// +---------+
// | shm_key |
// +---------+
// | shm_key |  <---- each element stores shm_key
// +---------+
//   ........
// +---------+
// | shm_key |  ----> use for get address of DL_bitmap(shm);
// +---------+
//

#define SHM_PMT_NR 8 // # of page mapping tables
#define SHM_PMT_ENT_NR 2048 //  # of entries for single page mapping table
#define SHM_KEY_ARRAY_NR 16
#define SHM_KEY_ARRAY_ELEM_NR 1024
#define PAGE_BITS_IN_ONE_BYTE 4
#define BITS_IN_BYTE 8
#define BITS_IN_PAGE (PAGE_SIZE * BITS_IN_BYTE)
#define NR_PAGES_IN_PEB (BITS_IN_PAGE / 2)
#define DIRTY_BIT_SHIFT 4

// page mapping table entry
typedef struct {
	uint16_t dirty : 1;
	uint16_t idx_of_key : 15;
} pmt_ent_t __attribute__((aligned(2))); // 2 bytes

// page mapping table
struct shm_pgmaptbl {
	pmt_ent_t ent[SHM_PMT_ENT_NR];
} __attribute__((aligned(PAGE_SIZE)));

struct shm_keyarray {
	key_t keys[SHM_KEY_ARRAY_ELEM_NR];
} __attribute__((aligned(PAGE_SIZE)));

// index of shm_pmt_ptrs
static inline int shm_get_pmt_idx(size_t pg_index)
{
	return pg_index / (SHM_PMT_ENT_NR * NR_PAGES_IN_PEB);
}

// index of pmt entry in the single pmt
static inline size_t shm_get_pmt_ent_idx(size_t pg_index)
{
	return (pg_index % (SHM_PMT_ENT_NR * NR_PAGES_IN_PEB)) /
	       NR_PAGES_IN_PEB;
}

#define SHM_DIRENT_DIRTY 0x8000
#define SHM_DIRENT_INDEX_MASK 0x7FFF

static inline uint16_t get_key_map_index(pmt_ent_t *d)
{
	return d->idx_of_key & SHM_DIRENT_INDEX_MASK;
}

static inline void dirty_shm_dirent(pmt_ent_t *d)
{
	d->dirty |= SHM_DIRENT_DIRTY;
}

static inline void clear_dirty_shm_dirent(pmt_ent_t *d)
{
	d->dirty &= ~SHM_DIRENT_DIRTY;
}

static inline uint16_t get_key_map_table_index(uint16_t kmap_idx)
{
	return kmap_idx / SHM_KEY_ARRAY_ELEM_NR;
}
#endif

// Shared memory between daemon and libfs
/*
 * [Shared State Header Block]
 * [Page-Directory Alloc-Checking Block]
 * [Page-Directory Dirty-Grouping Block]
 * [Page-Entry (State of Pages) Block] --- cover 32MB data
 * [Page-Entry (State of Pages) Block]
 * [Page-Entry (State of Pages) Block]
 * ...
*/

#define SHM_ROOT "/dev/shm"

enum {
	DIRTYBIT = 0,
	LOCKBIT,
	UPTODATEBIT,
	SKIPREAD_BIT,
};

#define BITS_IN_BYTE 8UL
#define BITS_IN_PAGE (PAGE_SIZE * BITS_IN_BYTE)
#define BYTES_IN_PAGE PAGE_SIZE
#define NR_PAGES_IN_PEB (BITS_IN_PAGE / NR_PAGE_STATE)
#define NR_PAGE_STATE 4UL // 4 bit
#define BYTES_PER_STATE (PAGE_SIZE / NR_PAGE_STATE)

#define LOCKBIT_START_INDEX (BYTES_PER_STATE * LOCKBIT)
#define UPTODATEBIT_START_INDEX (BYTES_PER_STATE * UPTODATEBIT)
#define SKIPREAD_START_INDEX (BYTES_PER_STATE * SKIPREAD_BIT)

#define PEB_COVERAGE (PAGE_SIZE * BITS_IN_PAGE / NR_PAGE_STATE) // 32MB
#define PDB_ALLOC_CHECK_COVERAGE (PEB_COVERAGE * BITS_IN_PAGE) // 1TB
#define PDB_DIRTY_GROUP_COVERAGE (PDB_ALLOC_CHECK_COVERAGE)

#define PDB_ALLOC_CHECK_NR                                                     \
	((OXBOW_MAX_FILE_SIZE - 1) / PDB_ALLOC_CHECK_COVERAGE + 1)
#define PDB_DIRTY_GROUP_NR                                                     \
	((OXBOW_MAX_FILE_SIZE - 1) / PDB_DIRTY_GROUP_COVERAGE + 1)
#define PEB_NR ((OXBOW_MAX_FILE_SIZE - 1) / PEB_COVERAGE + 1)
#define PEB_START (1 + PDB_ALLOC_CHECK_NR + PDB_DIRTY_GROUP_NR)

/* Total size of shared memory for one regular file */
#define SHM_SIZE                                                               \
	(1ULL + PDB_ALLOC_CHECK_NR + PDB_DIRTY_GROUP_NR + PEB_NR) * PAGE_SIZE

#define SHM_USE_JOURNAL (1 << 12) // file system use journaling
#define SHM_RUNNING_TX (1 << 13) // (journaling) in running transaction
#define SHM_I_DELETED (1 << 14)
#define SHM_DATA_DIRTY (1 << 15)

// Already requested to secure daemon and waiting for completion.
#define SHM_RUNNING_TX_REQUESTED (1 << 16)

struct shm_shared_state {
	/* Shared file state (inode) */
	unsigned long i_ino; // debug
	unsigned int i_nlink;
	mode_t i_mode;
	uid_t i_uid;
	gid_t i_gid;
	size_t i_size;
	struct timespec i_atime; // TOCHECK: Not used?
	struct timespec i_mtime; // TOCHECK: Not used?
	struct timespec i_ctime; // TOCHECK: Not used?
	blkcnt_t i_blocks;
	atomic_ulong i_state; // differ from daemon inode
	pthread_rwlock_t i_rwlock; /* top-level lock */
	// lock, mutex, journal handle etc...

	// read only

	/* Daemon speficic information */
	void *daemon_inode_va; // LIBFS MUST NOT ACCESS THIS ADDRESS!

} __attribute__((aligned(PAGE_SIZE)));

struct shm_dfd {
	int dfd;
	char padding[60]; // To avoid cache-line false sharing
} __attribute__((aligned(64)));

void print_page_bits(char n);
void shm_page_set_bit(uint8_t *page, int bit_pos);
void shm_page_unset_bit(uint8_t *page, int bit_pos);
int shm_page_check_bit(uint8_t *page, int bit_pos);
atomic_char *get_page_bits(char *shm_start, size_t pg_idx, int bit_type);
void unlock_page_bit(atomic_char *bits, size_t pg_idx);
int shm_alloc_check(struct shm_shared_state *header, size_t pg_idx);
struct inode_range_dirty *alloc_ird(void);
void free_ird(struct inode_range_dirty *ird);
struct inode_range_dirty *snapshot_ird(struct inode_range_dirty *src_ird);
void free_ird_snapshot(struct inode_range_dirty *ird_snap);
void free_ird_except_first(struct inode_range_dirty *ird);

/* Compact IRD frame format shared between sync.c and shm.c */
struct ird_frame_header {
    uint64_t base_pg;    /* base page index for this frame */
    uint16_t n_entries;  /* number of entries following */
//     uint16_t flags;      /* reserved */
//     uint32_t crc32;      /* crc of entries area */
} __attribute__((packed));

struct ird_frame_entry {
    uint32_t delta; /* pages from previous end to new start (first: start-base_pg) */
    uint32_t len;   /* length in pages */
} __attribute__((packed));

/* Deserialize compact IRD frames into an IRD list. */
int deserialize_ird_compact(const void *buf,
                            uint32_t nr_blocks,
                            struct inode_range_dirty *dst_head);
#endif
