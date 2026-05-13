
#include "fs.h"
#include "sefs/sefs.h"

#include "common/list.h"
#include "oxbow_debug.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/queue.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <unistd.h>
#include <pthread.h>
#include <immintrin.h>
#include <stddef.h>

#include "oxbow.h"
#include "journal.h"

void print_inode_state(struct inode *inode)
{
	printf("Inode Number (i_ino): %lu\n", inode->i_ino);
	printf("Number of Hard Links (i_nlink): %u\n", inode->i_nlink);
	printf("File Mode (i_mode): %o\n", inode->i_mode);
	printf("User ID of Owner (i_uid): %d\n", inode->i_uid);
	printf("Group ID of Owner (i_gid): %d\n", inode->i_gid);
	printf("File Size (i_size): %zu bytes\n", inode->i_size);
	printf("Last Access Time (i_atime): %ld.%09ld seconds\n",
	       inode->i_atime.tv_sec, inode->i_atime.tv_nsec);
	printf("Last Modification Time (i_mtime): %ld.%09ld seconds\n",
	       inode->i_mtime.tv_sec, inode->i_mtime.tv_nsec);
	printf("Last Metadata Change Time (i_ctime): %ld.%09ld seconds\n",
	       inode->i_ctime.tv_sec, inode->i_ctime.tv_nsec);
	printf("Number of Blocks Allocated (i_blocks): %lld\n",
	       (long long)inode->i_blocks);
}

int inode_idx_lock(struct inode *inode)
{
	return pthread_rwlock_wrlock(&inode->i_idx_lock);
}

int inode_idx_lock_shared(struct inode *inode)
{
	return pthread_rwlock_rdlock(&inode->i_idx_lock);
}

int inode_idx_unlock(struct inode *inode)
{
	return pthread_rwlock_unlock(&inode->i_idx_lock);
}

int inode_lock(struct inode *inode)
{
	return pthread_spin_lock(&inode->i_lock);
}

int inode_unlock(struct inode *inode)
{
	return pthread_spin_unlock(&inode->i_lock);
}

void inode_sb_list_add(struct inode *inode)
{
	pthread_spin_lock(&inode->i_sb->s_inode_list_lock);
	list_add_tail(&inode->i_sb_list, &inode->i_sb->s_inodes);
	pthread_spin_unlock(&inode->i_sb->s_inode_list_lock);
}

void inode_sb_list_del(struct inode *inode)
{
	pthread_spin_lock(&inode->i_sb->s_inode_list_lock);
	list_del_init(&inode->i_sb_list);
	pthread_spin_unlock(&inode->i_sb->s_inode_list_lock);
}

/**
 * @brief Inode hashtable (Key is inode number)
 * 
 */
#define HASH_SIZE 1024
static struct hlist_head *inode_hashtable;
static pthread_spinlock_t inode_hash_lock;

/*
 * inode->i_lock must be held
 */
void iget(struct inode *inode)
{
	atomic_fetch_add(&inode->i_count, 1);
}

void iput_final(struct inode *inode)
{
	oxb_debug(
		"[%s] inode(%lu) final put: i_nlink=%u i_count(before)=%d state=0x%lx mode=0%o",
		__func__, inode->i_ino, inode->i_nlink, atomic_load(&inode->i_count), inode->i_state,
		inode->i_mode);

	if (S_ISDIR(inode->i_mode))
		log_error("inode(%lu) is directory", inode->i_ino);
	/* delete from super block inode list */
	inode_sb_list_del(inode);

	/* delete from global inode hash */
	BUG_ON(&inode->i_hash == NULL, "no hash");
	pthread_spin_lock(&inode_hash_lock);
	hlist_del(&inode->i_hash);
	pthread_spin_unlock(&inode_hash_lock);

	/* Free on-disk inode when link count reaches zero. */
	if (inode->i_nlink == 0) {
		inode->i_sb->s_op->free_inode(inode);
	}

	/* Destroy in-memory inode state (shm, mappings, fd, etc). */
	destroy_inode(inode, false);
}

void iput(struct inode *inode)
{
	int prev;
	// i_debug("[%s] ino(%lu) count(%d)", __func__, inode->i_ino,
	// 	atomic_load(&inode->i_count));

	inode_lock(inode);
	prev = atomic_fetch_sub(&inode->i_count, 1);
	if (prev == 1) {
		if (inode->i_state & I_EVICT) {
			log_error("inode(%lu) already evicted", inode->i_ino);
			inode_unlock(inode);
			return;
		}
		inode->i_state |= I_EVICT;
		inode_unlock(inode);
		iput_final(inode);
		return;
	}
	if (prev < 1)
		log_error("inode(%lu) count(%d)", inode->i_ino, prev);
	inode_unlock(inode);
}

static unsigned long ihash(unsigned long ino)
{
	return ino % HASH_SIZE;
}

static struct inode *find_inode(struct hlist_head *head, unsigned long ino)
{
	struct inode *inode = NULL;

	hlist_for_each_entry (inode, head, i_hash) {
		if (inode->i_ino == ino) {
			iget(inode);
			return inode;
		}
	}

	return NULL;
}

struct inode *ihold(unsigned long ino)
{
	struct hlist_head *head = &inode_hashtable[ihash(ino)];
	struct inode *inode = NULL;
	// again:
	pthread_spin_lock(&inode_hash_lock);
	inode = find_inode(head, ino);
	pthread_spin_unlock(&inode_hash_lock);

	if (inode)
		iput(inode);
	return inode;
}

/* Get inode struct with being locked (I_NEW state).
 * This function must be called only by filesystem. (No ioworker or something) */
struct inode *iget_locked(struct super_block *sb, unsigned long ino)
{
	struct hlist_head *head = &inode_hashtable[ihash(ino)];
	struct inode *inode = NULL;
	// again:
	pthread_spin_lock(&inode_hash_lock);
	inode = find_inode(head, ino);
	pthread_spin_unlock(&inode_hash_lock);

	if (inode)
		return inode;

	if (!sb) {
		log_error("no super block");
		return NULL;
	}

	inode = sb->s_op->alloc_inode(sb);
	if (inode) {
		struct inode *old;

		pthread_spin_lock(&inode_hash_lock);
		old = find_inode(head, ino);
		if (!old) { /* not in hash means not initiated yet */
			inode->i_ino = ino;
			inode_lock(inode);
			inode->i_state |= I_NEW;

			oxb_debug(
				"[iget_locked] set I_NEW inode(%lu) state=0x%lx",
				inode->i_ino, inode->i_state);

			hlist_add_head(&inode->i_hash, head);
			inode_unlock(inode);
			inode_sb_list_add(inode);
			pthread_spin_unlock(&inode_hash_lock);
			iget(inode);
			return inode;
		}

		// (Comment from the Linux kernel source.)
		// Somebody else created the same inode. Use the old inode
		// instead of the one we just allocated.
		oxb_error(
			"[iget_locked] race: old inode(%lu) already exists in hash",
			old->i_ino);
		pthread_spin_unlock(&inode_hash_lock);

		// destroy inode
		inode->i_sb->s_op->destroy_inode(inode);

		inode = old;

		// Wait until old is prepared.
		// TODO: To be implemented. Refer to the kernel code.

		// If old preparation is failed:
		// iput(inode);
		// goto again;

		// FIXME: Does it really happen? Need to consider this path
		// later.
		oxbow_assert(0);

	} else {
		log_error("inode alloc fails");
	}

	return inode;
}

/**
 * @brief destroy the inode related caches and free the inode structure
 *
 * @param inode 
 * @param evict (Not used for now. We always destroy the inode.)
 */
void destroy_inode(struct inode *inode, bool evict)
{
	d_debug("[%s] inode(%d)", __func__, inode->i_ino);

	if (inode->shm_header) {
		d_trace("[%s] inode(%d) shm_header(%p)", __func__, inode->i_ino,
			inode->shm_header);
		free_shm_shared_state(inode);
		free_ird_except_first(inode->i_stg_dirty_start);
		free_ird_except_first(inode->i_journal_dirty_start);

		// Do not acquire locks.
		// Destroying inode does not occur while fsync or bg journaling
		// is processing.
		//
		// If we hold lock here, sync path should consider this case.
		//
		// pthread_spin_lock(&inode->i_journal_waiting_dirty_lock[0]);
		// pthread_spin_lock(&inode->i_journal_waiting_dirty_lock[1]);
		for (int i = 0; i < 2; i++) {
			if (inode->i_journal_waiting_dirty_start[i]->next) {
				oxb_warn(
					"i_journal_waiting_dirty_start is not NULL. "
					"There is a ongoing background journal commit.");
			}

			free_ird_except_first(inode->i_journal_waiting_dirty_start[i]);
		}
		// pthread_spin_unlock(&inode->i_journal_waiting_dirty_lock[0]);
		// pthread_spin_unlock(&inode->i_journal_waiting_dirty_lock[1]);

		if (inode->data) {
			if (munmap(inode->data, OXBOW_MAX_FILE_SIZE) < 0)
				perror("munmap fail");
			else
				atomic_fetch_add(&g_file_munmap_count, 1);
			inode->data = NULL;
		}
		if (inode->msg_ring) {
			if (munmap(inode->msg_ring, PAGE_SIZE) < 0)
				perror("munmap msg_ring fail");
			inode->msg_ring = NULL;
		}
		if (inode->fd >= 0) {
			if (close(inode->fd) < 0)
				perror("close fail");
			inode->fd = -1;
		}
	} else {
		oxb_warn("Resources may be leaked. inode(%lu)", inode->i_ino);

	}

	if (S_ISREG(inode->i_mode) && !(inode->i_state & I_SET_ONCE))
		log_warn("inode(%lu) not set", inode->i_ino);

	if (!evict) {
		BUG_ON(!inode->i_sb->s_op->destroy_inode, "no destroy_inode");
		inode->i_sb->s_op->destroy_inode(inode);
	}
}

void evict_inode(struct inode *inode)
{
	/* Now this inode is going to be freed from file system */
	inode_lock(inode);
	inode->i_state |= I_DELETED;
	inode_unlock(inode);
	iput(inode);
}

void unlock_new_inode(struct inode *inode)
{
	inode_lock(inode);
	inode->i_state &= ~I_NEW;
	inode_unlock(inode);
}

void mark_inode_dirty(struct inode *inode)
{
	d_trace("[%s] BEFORE tid=%d inode(%d) state(0x%lx)", __func__,
		get_tid(), inode->i_ino, inode->i_state);

	inode_lock(inode);
	inode->i_state |= I_DIRTY;
	if ((inode->i_state & I_USE_JOURNAL) &&
	    !(inode->i_state & I_IN_JTX_DIRTY_LIST)) {
		if (add_journal_file(inode)) {
			oxb_error("[%s] inode(%d) add_journal_file fail",
				  __func__, inode->i_ino);
			panic("add_journal_file fail");
			goto unlock;
		}
		inode->i_state |= I_IN_JTX_DIRTY_LIST;

		d_trace("[%s] AFTER tid=%d inode(%d) state(0x%lx) is added to journal",
			__func__, get_tid(), inode->i_ino, inode->i_state);
	}
unlock:
	inode_unlock(inode);
}

void inode_add_journal(struct inode *inode)
{
	BUG_ON(!(inode->i_state & I_USE_JOURNAL), "not journal inode");

	d_trace("[%s] BEFORE tid=%d inode(%d) state(0x%lx)", __func__,
		get_tid(), inode->i_ino, inode->i_state);

	inode_lock(inode);
	inode->i_state |= I_DIRTY;

	if (S_ISREG(inode->i_mode) && inode->i_state & I_DELETED) {
		log_warn("dirty file added journal? ino(%lu)", inode->i_ino);
		inode_set_shm_state(inode, SHM_I_DELETED);
		goto unlock;
	}

	// if already in journal transaction, skip
	if (inode->i_state & I_IN_JTX_DIRTY_LIST) {
		oxb_debug("[%s] inode(%d) already in journal tx", __func__, inode->i_ino);
		goto set_state;
	}

	// add this file to running journal transaction
	if (add_journal_file(inode)) {
		oxb_error("[%s] inode(%d) add_journal_file fail", __func__, inode->i_ino);
		panic("add_journal_file fail");
		goto unlock;
	}

	inode->i_state |= I_IN_JTX_DIRTY_LIST;
	// oxb_trace("[%s] inode(%d) I_IN_JTX_DIRTY_LIST is set", __func__, inode->i_ino);

set_state:
	if (S_ISREG(inode->i_mode))
		inode_set_shm_state(inode, SHM_RUNNING_TX);

	d_trace("[%s] AFTER tid=%d inode(%d) state(0x%lx)", __func__, get_tid(),
		inode->i_ino, inode->i_state);

unlock:
	inode_unlock(inode);
}

int inode_alloc_blocks(struct inode *inode, struct buffer_extent *be,
		       loff_t old_size)
{
	size_t new, old, cnt, cur;
	uint32_t nr_blks_to_alloc;
	int ret;

	// TODO: Need to decoupling this function from BITMAP_BLK_NR.
	// The number of free blocks in a block group of lwext4 is 32254 or
	// 32223. It is not sufficient to allocate BITMAP_BLK_NR blocks at once.
	// For now, let's use half of BITMAP_BLK_NR. (In the sefs, it was BITMAP_BLK_NR.)
	nr_blks_to_alloc = BITMAP_BLK_NR / 2;

	old = SIZE_TO_BLK_NR(old_size);
	new = SIZE_TO_BLK_NR(inode->i_size);
	stg_debug(
		"[%s] inode(%lu, %lu KiB, %lu MiB) alloc (%lu blks, %lu KB, %lu MB)",
		__func__, inode->i_ino, inode->i_size >> 10,
		inode->i_size >> 20, new - old,
		((new - old) << OXBOW_BLOCK_SIZE_SHIFT) >> 10,
		((new - old) << OXBOW_BLOCK_SIZE_SHIFT) >> 20);

	if (old == new)
		return 0;

	/* [TODO] we do not handle truncate yet */
	if (new < old) {
		oxb_warn("[%s] Inode(%lu) new(%lu KiB) < old(%lu KiB)",
			 __func__, inode->i_ino, new >> 10, old >> 10);
		return 0;
	}

	cnt = new - old;
	cur = old;

	if (inode->i_op->get_blocks) {
		// If get_blocks is implemented, use it.

		/* to set up buffer extent do get_block first */
		if (cnt > nr_blks_to_alloc) {
			inode_idx_lock(inode);
			ret = inode->i_op->get_blocks(inode, be, cur,
						      nr_blks_to_alloc);
			inode_idx_unlock(inode);
			if (ret) {
				oxb_error("ino(%lu) getblk fail", inode->i_ino);
				return -1;
			}
			cnt -= nr_blks_to_alloc;
			cur += nr_blks_to_alloc;
		} else {
			inode_idx_lock(inode);
			ret = inode->i_op->get_blocks(inode, be, cur, cnt);
			inode_idx_unlock(inode);
			if (ret) {
				oxb_error("ino(%lu) getblk fail", inode->i_ino);
				return -1;
			}
			return 0;
		}

		/* allocate remain blocks */
		while (cnt > nr_blks_to_alloc) {
			inode_idx_lock(inode);
			ret = inode->i_op->get_blocks(inode, NULL, cur,
						      nr_blks_to_alloc);
			inode_idx_unlock(inode);
			if (ret) {
				oxb_error("ino(%lu) getblk fail", inode->i_ino);
				return -1;
			}
			cnt -= nr_blks_to_alloc;
			cur += nr_blks_to_alloc;
		}
		inode_idx_lock(inode);
		ret = inode->i_op->get_blocks(inode, NULL, cur, cnt);
		inode_idx_unlock(inode);
		if (ret) {
			oxb_error("ino(%lu) getblk fail", inode->i_ino);
			return -1;
		}
	} else {
		// TODO: to be implemented:
		// If get_blocks is not supported by a file system, use get_block it gets one
		// block at a time.
		panic("Not implemented.");
	}

	return 0;
}

/**
 * inode_init_always - perform inode structure initialisation
 * @sb: superblock inode belongs to
 * @inode: inode to initialise
 *
 * These are initializations that need to be done on every inode
 * allocation as the fields are not initialised by slab allocation.
 */
int inode_init_always(struct super_block *sb, struct inode *inode)
{
	static const struct inode_operations empty_iops;

	inode->i_sb = sb;
	inode->i_op = &empty_iops;
	inode->i_ino = 0;
	inode->i_nlink = 1;
	inode->i_size = 0;
	inode->i_state = 0;
	inode->data = NULL;
	inode->ra_user_prefetch_idx = 0;
	inode->ra_cache_nr_slots = 0;
	inode->ra_cache_buf = NULL;
	inode->ra_cache_index = NULL;
	inode->ra_cache_valid = NULL;
	inode->ra_cache_cursor = 0;
	inode->i_stg_dirty_tail = NULL;
	INIT_HLIST_NODE(&inode->i_hash);
	INIT_LIST_HEAD(&inode->i_journal_waiting_lists[0]);
	INIT_LIST_HEAD(&inode->i_journal_waiting_lists[1]);
	INIT_LIST_HEAD(&inode->i_stage_list);
	INIT_LIST_HEAD(&inode->i_sb_list);

	pthread_spin_init(&inode->i_lock, 0);
	pthread_rwlock_init(&inode->i_idx_lock, 0);
	pthread_rwlock_init(&inode->i_dir_lock, 0);
	pthread_spin_init(&inode->ra_cache_lock, 0);

#ifdef OXBOW_USER_READAHEAD
	/* Allocate per-inode user-level readahead cache buffers.
	 *
	 * Only needed when user-level readahead is compiled in; otherwise
	 * the cache is never populated (see nvme.c) nor consumed
	 * (see mpage.c), so the ~2 MiB per-inode allocation is pure
	 * overhead.  The pointers are already initialised to NULL above,
	 * so callers that still dereference ra_cache_* in the disabled
	 * build will see an empty cache.
	 */
	{
		const unsigned int ra_slots = 512; /* 512 pages ~= 2 MiB per-inode cache */

		inode->ra_cache_buf = malloc((size_t)ra_slots * (size_t)PAGE_SIZE);
		inode->ra_cache_index = malloc((size_t)ra_slots * sizeof(pgoff_t));
		inode->ra_cache_valid = calloc((size_t)ra_slots, sizeof(unsigned char));
		if (!inode->ra_cache_buf || !inode->ra_cache_index ||
		    !inode->ra_cache_valid) {
			free(inode->ra_cache_buf);
			free(inode->ra_cache_index);
			free(inode->ra_cache_valid);
			inode->ra_cache_buf = NULL;
			inode->ra_cache_index = NULL;
			inode->ra_cache_valid = NULL;
			inode->ra_cache_nr_slots = 0;
			log_error("failed to allocate RA cache for inode");
		} else {
			inode->ra_cache_nr_slots = ra_slots;
		}
	}
#endif /* OXBOW_USER_READAHEAD */

	for (int i = 0; i < 2; i++) {
		pthread_spin_init(&inode->i_journal_waiting_dirty_lock[i], PTHREAD_PROCESS_PRIVATE);
		atomic_init(&inode->i_journal_waiting_dirty_txid[i], -1);
#if (STG_WAIT_MODE == ALWAYS_WAIT)
		pthread_mutex_init(&inode->i_journal_waiting_dirty_mutex[i], NULL);
		pthread_cond_init(&inode->i_journal_waiting_dirty_cond[i], NULL);
#endif
		inode->i_journal_waiting_nr_dirty_blks[i] = 0;
		inode->i_journal_waiting_nr_ird_blks[i] = 0;
	}

	if (sb->journal)
		inode->i_state |= I_USE_JOURNAL;

	inode->i_mapping = malloc(sizeof(struct address_space));
	if (!inode->i_mapping) {
		log_error("malloc fail");
		return -1;
	}
	pthread_spin_init(&inode->i_mapping->private_lock, 0);
	INIT_LIST_HEAD(&inode->i_mapping->private_list);
	inode->i_mapping->nr_entries = 0;

	atomic_init(&inode->i_journal_txid, 0);
	atomic_init(&inode->i_count, 0);

	return 0;
}

int inode_hash_init(void)
{
	inode_hashtable = calloc(1, sizeof(struct hlist_head) * HASH_SIZE);
	if (!inode_hashtable) {
		oxb_error("malloc fail");
		return -1;
	}
	pthread_spin_init(&inode_hash_lock, 0);
	return 0;
}

void inode_all_free(struct super_block *sb)
{
	struct inode *inode;

	list_for_each_entry (inode, &sb->s_inodes, i_sb_list) {
		if (inode->data)
			munmap(inode->data, OXBOW_MAX_FILE_SIZE);
		if (inode->fd)
			close(inode->fd);
	}

	free(inode_hashtable);
	log_info("inode hashtable free");
}

int init_inode(void)
{
	if (inode_hash_init())
		return -1;

	log_info("inode system init done");
	return 0;
}
