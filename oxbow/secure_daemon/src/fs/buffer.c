#include "buffer_head.h"
#include "fs.h"
#include "io_dispatcher.h"
#include "common/global.h"
#include <pthread.h>
#include "oxbow_debug.h"
#include <spdk/env.h>

/* ======================================================================
 * BH Sentinel Debug
 *
 * Appends a sentinel guard zone after each bh->b_data allocation.
 * At bh_allfree() (and on demand via bh_check_all_sentinels()),
 * the guard is verified.  Any byte that differs from the magic
 * value indicates an out-of-bounds write that reached this buffer's
 * trailing area — the likely source of SPDK allocator metadata
 * corruption.
 *
 * To disable: comment out the #define below.
 * ====================================================================== */
// #define BH_SENTINEL_DEBUG

#ifdef BH_SENTINEL_DEBUG

#define BH_SENTINEL_SIZE	128
#define BH_SENTINEL_MAGIC	0xFE
#define BH_ALLOC_SIZE		(PAGE_SIZE + BH_SENTINEL_SIZE)

static void bh_sentinel_fill(void *data)
{
	memset((char *)data + PAGE_SIZE, BH_SENTINEL_MAGIC, BH_SENTINEL_SIZE);
}

/* Returns 1 if corrupted, 0 if OK. */
static int bh_sentinel_check(struct buffer_head *bh)
{
	const unsigned char *p = (const unsigned char *)bh->b_data + PAGE_SIZE;
	int first_bad = -1;
	int bad_cnt = 0;
	int i;

	for (i = 0; i < BH_SENTINEL_SIZE; i++) {
		if (p[i] != BH_SENTINEL_MAGIC) {
			if (first_bad < 0)
				first_bad = i;
			bad_cnt++;
		}
	}

	if (first_bad < 0)
		return 0; /* sentinel intact */

	fprintf(stderr,
		"\n==== BH SENTINEL CORRUPTION DETECTED ====\n"
		"  bh blocknr : %lu\n"
		"  b_data     : %p\n"
		"  b_state    : 0x%lx\n"
		"  corrupted  : %d / %d bytes  (first bad at PAGE_SIZE+%d)\n"
		"  sentinel region hex dump (%d bytes after PAGE_SIZE):\n",
		(unsigned long)bh->b_blocknr,
		(void *)bh->b_data,
		(unsigned long)atomic_load(&bh->b_state),
		bad_cnt, BH_SENTINEL_SIZE, first_bad,
		BH_SENTINEL_SIZE);

	for (i = 0; i < BH_SENTINEL_SIZE; i++) {
		if (i % 32 == 0)
			fprintf(stderr, "    +%03d: ", i);
		fprintf(stderr, "%02x ", p[i]);
		if ((i + 1) % 32 == 0)
			fprintf(stderr, "\n");
	}
	fprintf(stderr, "==========================================\n\n");

	return 1; /* corrupted */
}

#else /* !BH_SENTINEL_DEBUG */

#define BH_ALLOC_SIZE		PAGE_SIZE
static inline void bh_sentinel_fill(void *data) { (void)data; }
static inline int  bh_sentinel_check(struct buffer_head *bh)
{
	(void)bh;
	return 0;
}

#endif /* BH_SENTINEL_DEBUG */

struct block_device *g_bdev = NULL;

static int trylock_buffer(struct buffer_head *bh)
{
	return likely(!test_set_buffer_locked(bh));
}

static void __lock_buffer(struct buffer_head *bh)
{
	bh_debug("[%s] (lba:%d) slow path", __func__, bh->b_blocknr);

	while (!trylock_buffer(bh)) {
		pthread_mutex_lock(&bh->io_mtx);
		while (buffer_locked(bh))
			pthread_cond_wait(&bh->io_cond, &bh->io_mtx);
		pthread_mutex_unlock(&bh->io_mtx);
	}

	// stashed code
	// pthread_mutex_lock(&bh->io_mtx);
	// while (buffer_locked(bh))
	// 	pthread_cond_wait(&bh->io_cond, &bh->io_mtx);
	// test_set_buffer_locked(bh);
	// pthread_mutex_unlock(&bh->io_mtx);
}

void lock_buffer(struct buffer_head *bh)
{
	if (!trylock_buffer(bh))
		__lock_buffer(bh);
}

void unlock_buffer(struct buffer_head *bh)
{
	pthread_mutex_lock(&bh->io_mtx);
	test_clear_buffer_locked(bh);
	pthread_cond_broadcast(&bh->io_cond);
	pthread_mutex_unlock(&bh->io_mtx);
}

// void wait_on_buffer(struct buffer_head *bh)
// {
// 	pthread_mutex_lock(&bh->io_mtx);
// 	while (buffer_locked(bh))
// 		pthread_cond_wait(&bh->io_cond, &bh->io_mtx);
// 	pthread_mutex_unlock(&bh->io_mtx);
// }

void wait_on_buffer(struct buffer_head *bh)
{
	struct timespec timeout;
	int ret;

	pthread_mutex_lock(&bh->io_mtx);
	while (buffer_locked(bh)) {
		clock_gettime(CLOCK_REALTIME, &timeout);
		timeout.tv_sec += 5;
		ret = pthread_cond_timedwait(&bh->io_cond, &bh->io_mtx,
					     &timeout);
		if (ret == ETIMEDOUT) {
			fprintf(stderr, "[WARN] wait_on_buffer: bh(%lu)\n",
				bh->b_blocknr);
		}
	}
	pthread_mutex_unlock(&bh->io_mtx);
}

static struct buffer_head *get_buffer_head(struct block_device *bdev,
					   sector_t block)
{
	struct buffer_head *ret;
	khint64_t k;

	pthread_spin_lock(&bdev->bd_lock);
	k = kh_get(bhmap, bdev->bhs, block);
	ret = k == kh_end(bdev->bhs) ? NULL : kh_value(bdev->bhs, k);
	pthread_spin_unlock(&bdev->bd_lock);

	return ret;
}

static struct buffer_head *slow_bread(struct buffer_head *bh)
{
	lock_buffer(bh);
	if (buffer_uptodate(bh)) {
		unlock_buffer(bh);
		return bh;
	} else {
		get_bh(bh);
		submit_bh(REQ_OP_READ, bh);

		// TODO: io error check here
		oxb_debug("[%s] bh(%lu) wait", __func__, bh->b_blocknr);
		wait_on_buffer(bh);
		if (buffer_uptodate(bh))
			return bh;
	}
	/* Fail to read buffer head */
	oxb_error("[%s] bh(%lu) not uptodate", __func__, bh->b_blocknr);
	brelse(bh);
	return NULL;
}

static struct buffer_head *alloc_buffer(struct block_device *bdev,
					sector_t block)
{
	struct buffer_head *bh;
	khint64_t k;
	void *data;
	int absent;

	bh = calloc(1, sizeof(struct buffer_head));
	if (!bh) {
		oxb_error("malloc failed");
		return NULL;
	}

	bh->b_blocknr = block;
	atomic_init(&bh->b_state, 0);
	atomic_init(&bh->b_lock, 0);
	set_buffer_new(bh);
	INIT_LIST_HEAD(&bh->elem);
	INIT_LIST_HEAD(&bh->j_blk_list);

	pthread_mutex_init(&bh->io_mtx, 0);
	pthread_cond_init(&bh->io_cond, 0);

	data = spdk_malloc(BH_ALLOC_SIZE, PAGE_SIZE, NULL,
			   SPDK_ENV_SOCKET_ID_ANY, SPDK_MALLOC_DMA);
	if (!data) {
		oxb_error("spdk_malloc fail");
		free(bh);
		panic("spdk_malloc fail");
		return NULL;
	}
	bh_sentinel_fill(data);

	bh->b_bdev = bdev;
	if (bdev->sb->journal)
		bh->journal = bdev->sb->journal;

	pthread_spin_lock(&bdev->bd_lock);
	k = kh_put(bhmap, bdev->bhs, block, &absent);
	if (absent > 0) {
		kh_key(bdev->bhs, k) = block;
		kh_value(bdev->bhs, k) = bh;
		bh->b_data = data;
	} else if (absent == -1) {
		kh_del(bhmap, bdev->bhs, k);
		oxb_error("fail hash");
	}
	pthread_spin_unlock(&bdev->bd_lock);

	if (absent < 0) {
		oxb_error("failed to insert bh");
		free(bh);
		panic("failed to insert bh");
		return NULL;
	}

	if (absent == 0) {
		free(bh); // someone already alloced
		spdk_free(data);
		oxb_warn("bh(%lu) someone alloc ealry", block);
		// panic("bh someone alloc ealry"); // TMP: to be removed
		return get_buffer_head(bdev, block);
	}

	return bh;
}

/**
 * @brief detach buffer head from hashmap, used for deleting file.
 * 	      This buffer head cannot be accessed by its block number
 */
void detach_buffer_head(baddr_t bno)
{
	khint64_t k;

	// if (bh->b_count != 1)
	// 	oxb_warn("[%s] b_count %d not 1", __func__, bh->b_count);

	pthread_spin_lock(&g_bdev->bd_lock);
	k = kh_get_bhmap(g_bdev->bhs, bno);
	if (k == kh_end(g_bdev->bhs))
		oxb_error("bh not exist");
	else
		kh_del_bhmap(g_bdev->bhs, k);
	pthread_spin_unlock(&g_bdev->bd_lock);
}

void free_buffer_head(baddr_t bno)
{
	struct buffer_head *bh;

	bh = get_buffer_head(g_bdev, bno);
	if (bh) {
		bh->b_assoc_map = NULL;
		bh->b_state = 0;
		INIT_LIST_HEAD(&bh->elem);
		INIT_LIST_HEAD(&bh->j_blk_list);
		// free(bh->b_data);
		// free(bh);
		// detach_buffer_head(bno);
	}
}

static int __sb_mballoc(struct block_device *bdev, sector_t block, size_t nr)
{
	struct buffer_head *bh;
	khint64_t k;
	char *mb_data;
	size_t i;
	int absent, success;

	mb_data = spdk_zmalloc(nr * PAGE_SIZE, PAGE_SIZE, NULL,
			       SPDK_ENV_SOCKET_ID_ANY, SPDK_MALLOC_DMA);
	if (!mb_data) {
		oxb_error("spdk_zmalloc fail");
		panic("spdk_zmalloc fail");
		goto err;
	}

	i = 0;
	while (i < nr) {
		bh = calloc(1, sizeof(struct buffer_head));
		if (!bh) {
			oxb_error("malloc failed");
			goto free;
		}
		bh->b_data = mb_data + (i * PAGE_SIZE);
		bh->b_blocknr = block + i;
		bh->b_state = 0;
		INIT_LIST_HEAD(&bh->elem);
		INIT_LIST_HEAD(&bh->j_blk_list);
		pthread_mutex_init(&bh->io_mtx, 0);
		pthread_cond_init(&bh->io_cond, 0);
		if (i == 0)
			set_buffer_huge(bh);
		else
			set_buffer_sub(bh);

		if (bdev->sb->journal)
			bh->journal = bdev->sb->journal;

		success = 0;
		pthread_spin_lock(&bdev->bd_lock);
		k = kh_put(bhmap, bdev->bhs, block + i, &absent);

		if (absent == -1) {
			kh_del(bhmap, bdev->bhs, k);
			oxb_error("fail hash");
		} else if (absent) {
			kh_key(bdev->bhs, k) = block + i;
			kh_value(bdev->bhs, k) = bh;
			success = 1;
		} else
			oxb_error("bh already exist in hash.");

		pthread_spin_unlock(&bdev->bd_lock);

		if (!success) {
			free(bh);
			goto free;
		}
		i++;
	}
	/* on success */
	return 0;

free:
	if (i == 0)
		goto err;

	while (i > 0) {
		free_buffer_head(block + i - 1);
		i--;
	}
	spdk_free(mb_data);
err:
	return -1;
}

/** @brief for contiguous address needs */
int sb_mballoc(struct super_block *sb, sector_t block, size_t nr)
{
	return __sb_mballoc(sb->bdev, block, nr);
}

struct buffer_head *__sb_bread(struct buffer_head *bh)
{
	if (!buffer_uptodate(bh)) {
		fs_trace("sb_bread: block(%d) Read from disk.", bh->b_blocknr);
		bh = slow_bread(bh);
	} else {
		fs_trace("sb_bread: block(%d) Already upto date.",
			 bh->b_blocknr);
		get_bh(bh);
	}
	return bh;
}

/**
 * @brief read a block from cached buffer, if not cached then alloc new.
 *  
 * @param head if allocate new buffer, add to this list.
 */
struct buffer_head *sb_bread(struct super_block *sb, baddr_t block)
{
	struct buffer_head *bh;
	struct block_device *bdev;

	fs_trace("sb_bread: block(%d) Requested", block);

	bdev = sb->bdev;

	bh = get_buffer_head(bdev, block);
	/* allocate new buffer_head */
	if (!bh) {
		fs_trace("sb_bread: block(%d) Miss. Alloc new one.", block);
		bh = alloc_buffer(bdev, block);
	}

	if (bh)
		bh = __sb_bread(bh);

	return bh;
}

struct buffer_head *sb_balloc(struct super_block *sb, baddr_t block)
{
	struct buffer_head *bh;

	bh = get_buffer_head(sb->bdev, block);
	if (!bh) /* allocate new buffer_head */
		bh = alloc_buffer(sb->bdev, block);

	if (!bh)
		log_error("[%s] failed to get (%lu)", block);

	return bh;
}

// /**
//  * @brief read a block from cached buffer, if not cached then alloc new but do
//  * not fill it by reading data from disk.
//  */
// struct buffer_head *sb_bread_noread(struct super_block *sb, baddr_t block,
// 				    bool *is_new)
// {
// 	struct buffer_head *bh;
// 	struct block_device *bdev;

// 	bdev = sb->bdev;

// 	// fs_trace("sb_bread_noread: block(%d) Requested", block);

// 	bh = get_buffer_head(bdev, block);

// 	if (bh) {
// 		// fs_trace("sb_bread_noread: block(%d) Hit", block);

// 		// The existing cache's content is incorrect.
// 		// oxbow_assert(!buffer_new(bh));
// 		*is_new = 0;

// 	} else {
// 		fs_trace("sb_bread_noread: block(%d) Miss. Alloc new one.",
// 			 block);

// 		/* allocate new buffer_head */
// 		bh = alloc_buffer(bdev, block);
// 		if (!bh) {
// 			oxb_error("Failed to alloc a buffer.");
// 			return NULL;
// 		}

// 		// TOCHECK: Should it be done acquiring bdlock in alloc_buffer()?
// 		set_buffer_new(bh);
// 		// get_bh(bh);

// 		*is_new = 1;
// 	}

// 	return bh;
// }

/**
 * @brief Write to the cache (not flush).
 * 
 * @param sb 
 * @param buf Data to write. If NULL, data is already in the buffer cache.
 * @param block 
 * @return int 
 */
int sb_bwrite(struct super_block *sb, const void *buf, baddr_t block)
{
	struct buffer_head *bh;
	struct block_device *bdev;

	bdev = sb->bdev;

	bh = get_buffer_head(bdev, block);
	if (!bh) {
		panic("bh not exist");
		return -1;
	}

	// If buffer is not NULL, fill the buffer cache with it.
	if (buf)
		memcpy(bh->b_data, buf, OXBOW_BLOCK_SIZE);

	// No sync. Just mark it dirty.
	mark_buffer_dirty(bh);

	return 0;
}

void __brelse(struct buffer_head *buf)
{
	if (atomic_load(&buf->b_count)) {
		put_bh(buf);
		return;
	}
	// log_warn("brelse: Trying to free free buffer");
}

void mark_buffer_dirty(struct buffer_head *bh)
{
	if (buffer_dirty(bh))
		return;

	set_buffer_dirty(bh);
}

void mark_buffer_dirty_inode(struct buffer_head *bh, struct inode *inode)
{
	struct address_space *mapping = inode->i_mapping;

	mark_buffer_dirty(bh);
	if (!bh->b_assoc_map) {
		pthread_spin_lock(&mapping->private_lock);
		list_move_tail(&bh->elem, &mapping->private_list);
		bh->b_assoc_map = mapping;
		mapping->nr_entries++;
		pthread_spin_unlock(&mapping->private_lock);
	} else {
		if (bh->b_assoc_map != mapping) {
			oxb_error("inode(%lu) in bh(%lu) inconsistent",
				  inode->i_ino, bh->b_blocknr);
			pthread_spin_lock(&mapping->private_lock);
			list_move_tail(&bh->elem, &mapping->private_list);
			bh->b_assoc_map = mapping;
			pthread_spin_unlock(&mapping->private_lock);
		}
	}
}

void mark_buffer_dirty_sb(struct buffer_head *bh, struct super_block *sb)
{
	struct address_space *mapping = &sb->s_mapping;

	mark_buffer_dirty(bh);
	if (!bh->b_assoc_map) {
		pthread_spin_lock(&mapping->private_lock);
		list_move_tail(&bh->elem, &mapping->private_list);
		bh->b_assoc_map = mapping;
		mapping->nr_entries++;
		pthread_spin_unlock(&mapping->private_lock);
	} else {
		if (bh->b_assoc_map != mapping) {
			oxb_error("sb's bh(%lu) inconsistent", bh->b_blocknr);
			pthread_spin_lock(&mapping->private_lock);
			list_move_tail(&bh->elem, &mapping->private_list);
			bh->b_assoc_map = mapping;
			pthread_spin_unlock(&mapping->private_lock);
		}
	}
}

void free_buffer_head_inode(baddr_t bno, struct inode *inode)
{
	struct address_space *mapping = inode->i_mapping;
	struct buffer_head *bh;

	bh = get_buffer_head(g_bdev, bno);
	if (bh) {
		buffer_locked(bh);
		if (bh->b_assoc_map == mapping) {
			pthread_spin_lock(&mapping->private_lock);
			list_del(&bh->elem);
			pthread_spin_unlock(&mapping->private_lock);
		}
		bh->b_assoc_map = NULL;
		bh->b_state = 0;
		INIT_LIST_HEAD(&bh->elem);
		INIT_LIST_HEAD(&bh->j_blk_list);
		unlock_buffer(bh);
	}
}

int sync_dirty_buffer(struct buffer_head *bh)
{
	int ret;

	d_debug("[%s] bno(%d)", __func__, bh->b_blocknr);

	if (!buffer_dirty(bh))
		return 0;

	ret = -1;
	lock_buffer(bh);
	if (buffer_dirty(bh)) {
		clear_buffer_dirty(bh);
		submit_bh(REQ_OP_WRITE, bh);
	} else
		unlock_buffer(bh);

	wait_on_buffer(bh);
	// TODO: maybe later io error check here
	ret = 0;
	return ret;
}

/**
 * @brief Check sentinels on all buffer heads (callable from anywhere).
 *
 * Returns the number of corrupted sentinels found.
 */
int bh_check_all_sentinels(void)
{
	struct block_device *bdev = g_bdev;
	struct buffer_head *bh;
	khint64_t k;
	int nr_bad = 0;

	if (!bdev || !bdev->bhs)
		return 0;

	pthread_spin_lock(&bdev->bd_lock);
	for (k = 0; k < kh_end(bdev->bhs); ++k) {
		if (kh_exist(bdev->bhs, k)) {
			bh = kh_value(bdev->bhs, k);
			if (!buffer_sub(bh))
				nr_bad += bh_sentinel_check(bh);
		}
	}
	pthread_spin_unlock(&bdev->bd_lock);

	if (nr_bad)
		fprintf(stderr,
			"[BH_SENTINEL] %d buffer(s) with corrupted sentinel.\n",
			nr_bad);

	return nr_bad;
}

int bh_allfree(struct super_block *sb)
{
	struct buffer_head *bh;
	struct block_device *bdev;
	khint64_t k;
	int nr_bad = 0;

	bdev = sb->bdev;

	pthread_spin_lock(&bdev->bd_lock);
	for (k = 0; k < kh_end(bdev->bhs); ++k)
		if (kh_exist(bdev->bhs, k)) {
			bh = kh_value(bdev->bhs, k);
			if (!buffer_sub(bh)) {
				nr_bad += bh_sentinel_check(bh);
				spdk_free(bh->b_data);
			}
			free(bh);
		}

	kh_destroy(bhmap, bdev->bhs);
	bdev->bhs = NULL;
	pthread_spin_unlock(&bdev->bd_lock);

	if (nr_bad)
		fprintf(stderr,
			"[BH_SENTINEL] === %d corrupted sentinel(s) at shutdown ===\n",
			nr_bad);
	else
		oxb_info("[BH_SENTINEL] All sentinels intact at shutdown.");

	oxb_info("buffer_head free done.");
	return 0;
}

int init_block_device(void)
{
	g_bdev = calloc(1, sizeof(struct block_device));
	if (!g_bdev) {
		oxb_error("malloc failed");
		return -1;
	}

	g_bdev->bhs = kh_init(bhmap);
	if (!g_bdev->bhs) {
		oxb_error("bhs alloc failed");
		return -1;
	}
	pthread_spin_init(&g_bdev->bd_lock, 0);

	oxb_info("block_device init done");
	return 0;
}
