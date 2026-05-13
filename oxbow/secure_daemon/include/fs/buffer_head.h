#ifndef _BUFFER_HEAD_H_
#define _BUFFER_HEAD_H_

#include "fs/fs.h"
#include "kerncompat.h"
#include "linux/bitmap.h" /* used in macro */
#include "common/list.h"
#include "common/oxbow.h"

#include <pthread.h>
#include <unistd.h>
#include <stdatomic.h>
#include <stdbool.h>

enum bh_state_bits {
	/* Block state on I/O (dynamic) */
	BH_New =
		1, // A new buffer is allocated but contents are not read from disk yet.
	BH_Uptodate,
	BH_Dirty,

	/* Static, decided on allocation */
	BH_Huge,
	BH_Sub,

	/* Concurrency mechanism */
	BH_Lock,
	BH_WaitIO, // May be on I/O

	// Represents that the buffer has been added to journal. Only used for
	// an inode block when bg-journaling is enabled.
	BH_InJournal,
};

// typedef void(bh_end_io_t)(struct buffer_head *bh, int uptodate);

struct buffer_head {
	// unsigned long b_state; /* buffer state bitmap */
	atomic_ulong b_state;
	atomic_ulong b_lock;
	sector_t b_blocknr; /* start block number(LBA) */
	char *b_data;

	struct block_device *b_bdev;
	// bh_end_io_t *b_end_io; // async IO not supported
	atomic_int b_count;
	struct list_head elem;
	struct address_space *b_assoc_map;
	pthread_mutex_t io_mtx;
	pthread_cond_t io_cond;

	struct journal_control_ctx *journal;
	struct list_head j_blk_list;
};

static inline int oxbow_test_and_set_bit(int nr, atomic_ulong *addr)
{
	unsigned long mask = 1UL << (nr % (8 * sizeof(unsigned long)));
	int idx = nr / (8 * sizeof(unsigned long));
	unsigned long old = atomic_fetch_or_explicit(&addr[idx], mask,
						     memory_order_seq_cst);
	return (old & mask) != 0;
}

static inline int oxbow_test_and_clear_bit(int nr, atomic_ulong *addr)
{
	unsigned long mask = 1UL << (nr % (8 * sizeof(unsigned long)));
	int idx = nr / (8 * sizeof(unsigned long));
	unsigned long old = atomic_fetch_and_explicit(&addr[idx], ~mask,
						      memory_order_seq_cst);
	return (old & mask) != 0;
}
static inline int oxbow_test_bit(int nr, atomic_ulong *addr)
{
	unsigned long mask = 1UL << (nr % (8 * sizeof(unsigned long)));
	unsigned long val = atomic_load_explicit(
		&addr[nr / (8 * sizeof(unsigned long))], memory_order_seq_cst);
	return (val & mask) != 0;
}

/*
 * macro tricks to expand the set_buffer_foo(), clear_buffer_foo()
 * and buffer_foo() functions.
 */
#define BUFFER_FNS(bit, name)                                                  \
	static inline void set_buffer_##name(struct buffer_head *bh)           \
	{                                                                      \
		__set_bit(BH_##bit, &(bh)->b_state);                           \
	}                                                                      \
	static inline void clear_buffer_##name(struct buffer_head *bh)         \
	{                                                                      \
		__clear_bit(BH_##bit, &(bh)->b_state);                         \
	}                                                                      \
	static inline int buffer_##name(const struct buffer_head *bh)          \
	{                                                                      \
		return oxbow_test_bit(BH_##bit, &(bh)->b_state);               \
	}

/*
 * test_set_buffer_foo() and test_clear_buffer_foo()
 */
#define TAS_BUFFER_FNS(bit, name)                                              \
	static inline int test_set_buffer_##name(struct buffer_head *bh)       \
	{                                                                      \
		return oxbow_test_and_set_bit(BH_##bit, &(bh)->b_state);       \
	}                                                                      \
	static inline int test_clear_buffer_##name(struct buffer_head *bh)     \
	{                                                                      \
		return oxbow_test_and_clear_bit(BH_##bit, &(bh)->b_state);     \
	}

#define BUFFER_FNS2(bit, name)                                                 \
	static inline void set_buffer_##name(struct buffer_head *bh)           \
	{                                                                      \
		__set_bit(BH_##bit, &(bh)->b_lock);                            \
	}                                                                      \
	static inline void clear_buffer_##name(struct buffer_head *bh)         \
	{                                                                      \
		__clear_bit(BH_##bit, &(bh)->b_lock);                          \
	}                                                                      \
	static inline int buffer_##name(const struct buffer_head *bh)          \
	{                                                                      \
		return oxbow_test_bit(BH_##bit, &(bh)->b_lock);                \
	}

#define TAS_BUFFER_FNS2(bit, name)                                             \
	static inline int test_set_buffer_##name(struct buffer_head *bh)       \
	{                                                                      \
		return oxbow_test_and_set_bit(BH_##bit, &(bh)->b_lock);        \
	}                                                                      \
	static inline int test_clear_buffer_##name(struct buffer_head *bh)     \
	{                                                                      \
		return oxbow_test_and_clear_bit(BH_##bit, &(bh)->b_lock);      \
	}

/*
 * Emit the buffer bitops functions.   Note that there are also functions
 * of the form "mark_buffer_foo()".  These are higher-level functions which
 * do something in addition to setting a b_state bit.
 */
BUFFER_FNS(New, new)
BUFFER_FNS(Uptodate, uptodate)
BUFFER_FNS(Dirty, dirty)
BUFFER_FNS(InJournal, injournal)
TAS_BUFFER_FNS(Dirty, dirty)
BUFFER_FNS(Huge, huge)
BUFFER_FNS(Sub, sub)
BUFFER_FNS2(Lock, locked)
TAS_BUFFER_FNS2(Lock, locked)

void lock_buffer(struct buffer_head *);
void unlock_buffer(struct buffer_head *);

int sb_mballoc(struct super_block *, sector_t block, size_t nr);
struct buffer_head *sb_balloc(struct super_block *sb, baddr_t block);
struct buffer_head *sb_bread(struct super_block *, baddr_t);
struct buffer_head *__sb_bread(struct buffer_head *);
int sb_bwrite(struct super_block *sb, const void *buf, baddr_t block);
void __brelse(struct buffer_head *);
void mark_buffer_dirty(struct buffer_head *);
void mark_buffer_dirty_inode(struct buffer_head *, struct inode *);
void mark_buffer_dirty_sb(struct buffer_head *, struct super_block *);
void free_buffer_head_inode(baddr_t, struct inode *);
int sync_dirty_buffer(struct buffer_head *);

void free_buffer_head(baddr_t bno);

static inline void get_bh(struct buffer_head *bh)
{
	atomic_fetch_add(&bh->b_count, memory_order_acq_rel);
}

static inline void put_bh(struct buffer_head *bh)
{
	atomic_fetch_sub(&bh->b_count, memory_order_acq_rel);
}

static inline void brelse(struct buffer_head *bh)
{
	if (bh)
		__brelse(bh);
}

/* block device */
KHASH_MAP_INIT_INT64(bhmap, struct buffer_head *)
int bh_allfree(struct super_block *);
int bh_check_all_sentinels(void);
struct block_device {
	struct super_block *sb;
	pthread_spinlock_t bd_lock;
	kh_bhmap_t *bhs;
};

extern struct block_device *g_bdev;
int init_block_device(void);

#endif
