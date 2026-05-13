#include "fs.h"
#include "log.h"
#include "oxbow_debug.h"
#include "common/shm.h"
#include "shmem.h"
#include "profile_libfs.h"
#include "common/dirty_mgmt.h"
#include "global.h"
#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <syscall.h>
#include <libsyscall_intercept_hook_point.h>

#ifndef SYSV_SHM
#include <unistd.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <unistd.h>
#include <fcntl.h>
#include <stdlib.h>
#include <stdatomic.h>
#endif

/* libfs inode states structure managed as hashtable */

khash_t(lfs_inode_hash) * g_lfs_inode_hash;
static pthread_spinlock_t g_lfs_inode_hash_lock;

void lfs_inode_hash_init(void)
{
	g_lfs_inode_hash = kh_init(lfs_inode_hash);
	pthread_spin_init(&g_lfs_inode_hash_lock, PTHREAD_PROCESS_PRIVATE);
}

void lfs_inode_hash_destory(void)
{
	struct lfs_inode *li;
	khint_t k;
	long ret;

	// Free malloc-ed memory by strdup().
	for (k = 0; k < kh_end(g_lfs_inode_hash); ++k)
		if (kh_exist(g_lfs_inode_hash, k)) {
			li = kh_val(g_lfs_inode_hash, k);
			if (li->shm_header == NULL)
				goto free_inode;

			ret = syscall_no_intercept(SYS_munmap, li->shm_header,
						   SHM_SIZE);
			if (ret < 0)
				log_error("munmap error (%ld)", ret);

			ret = syscall_no_intercept(SYS_close, li->shm_fd);
			if (ret < 0)
				log_error("close error (%ld)", ret);

		free_inode:
			if (li->data) {
				ret = syscall_no_intercept(SYS_munmap, li->data,
							   OXBOW_MAX_FILE_SIZE);
				if (ret < 0)
					log_error("munmap error (%ld) li->data(%p)", ret, li->data);
			}

			// if (li->path)
			// 	free(li->path);
			// free(li);
			free((char *)kh_key(g_lfs_inode_hash, k));
		}

	kh_destroy(lfs_inode_hash, g_lfs_inode_hash);
}

/**
 * @brief insert inode cache to hashmap, must use path in inode object.
 * 
 * @param li (target)
 * @return int (success on 0)
 */
static int insert_hash(struct lfs_inode *li)
{
	khint_t k;
	int absent, ret;

	BUG_ON(!li->path, "no key");

	ret = -1;
	k = kh_put(lfs_inode_hash, g_lfs_inode_hash, li->path, &absent);

	if (absent == -1) {
		kh_del(lfs_inode_hash, g_lfs_inode_hash, k);
		oxb_error("[%s] path(%s) key(%d)", __func__, li->path, k);
	} else {
		kh_value(g_lfs_inode_hash, k) = li;
		ret = 0;
		l_trace("[%s] path(%s) added key(%d) absent(%d)", __func__,
			li->path, k, absent);
	}

	return ret;
}

static int change_hash(struct lfs_inode *inode, const char *oldpath,
		       const char *newpath)
{
	khint_t k;
	int absent, ret = -1;
	char *old_key, *new_key;

	pthread_spin_lock(&g_lfs_inode_hash_lock);

	k = kh_get(lfs_inode_hash, g_lfs_inode_hash, oldpath);
	if (k == kh_end(g_lfs_inode_hash)) {
		oxb_error("[%s] old path(%s) not found", __func__, oldpath);
		goto out_unlock;
	}

	old_key = (char *)kh_key(g_lfs_inode_hash, k);

	new_key = malloc(strlen(newpath) + 1);
	if (!new_key) {
		log_error("malloc fail");
		goto out_unlock;
	}
	strcpy(new_key, newpath);

	/* Remove old entry from hash */
	kh_del(lfs_inode_hash, g_lfs_inode_hash, k);

	/* Update inode to point to the new path buffer */
	inode->path = new_key;

	/* Insert new entry with updated key */
	k = kh_put(lfs_inode_hash, g_lfs_inode_hash, inode->path, &absent);
	if (absent == -1) {
		kh_del(lfs_inode_hash, g_lfs_inode_hash, k);
		oxb_error("[%s] path(%s) key(%d)", __func__, inode->path, k);

		/* Roll back inode->path on failure */
		inode->path = old_key;
		free(new_key);
		goto out_unlock;
	}

	kh_value(g_lfs_inode_hash, k) = inode;
	ret = 0;

	/* Old key buffer is no longer referenced by the hash table or inode */
	free(old_key);

out_unlock:
	pthread_spin_unlock(&g_lfs_inode_hash_lock);

	return ret;
}

static struct lfs_inode *get_lfs_inode_fast(const char *path)
{
	struct lfs_inode *tar;
	khint_t k;

	tar = NULL;

	k = kh_get(lfs_inode_hash, g_lfs_inode_hash, path);

	if (k != kh_end(g_lfs_inode_hash)) {
		tar = kh_value(g_lfs_inode_hash, k);
		atomic_fetch_add(&tar->ref, 1);
	}

	l_trace("[%s] %s -> %s", __func__, path, tar ? "Exist" : "Not exist");

	return tar;
}

struct lfs_inode *get_lfs_inode(const char *path)
{
	struct lfs_inode *inode, *old;

	inode = old = NULL;

	pthread_spin_lock(&g_lfs_inode_hash_lock);
	inode = get_lfs_inode_fast(path);
	pthread_spin_unlock(&g_lfs_inode_hash_lock);
	if (inode) {
		l_debug("[%s] fastpath", __func__);
		return inode;
	}

	/* no entry so that alloc new one */
	inode = calloc(1, sizeof(struct lfs_inode));
	if (!inode) {
		log_error("malloc fail");
		return NULL;
	}
	inode->path = malloc(strlen(path) + 1);
	if (!inode->path) {
		oxb_error("malloc failed");
		return NULL;
	}
	strcpy(inode->path, path);
	pthread_spin_init(&inode->li_lock, PTHREAD_PROCESS_PRIVATE);

	pthread_spin_lock(&g_lfs_inode_hash_lock);
	old = get_lfs_inode_fast(path);
	if (!old) {
		/* store path for inode */
		if (insert_hash(inode) < 0) {
			free(inode->path);
			goto fail;
		}

		inode->li_state |= LFS_INODE_FREE;
		atomic_init(&inode->ref, 1);
		pthread_spin_unlock(&g_lfs_inode_hash_lock);
		// on success
		return inode;
	} else {
		/* other thread already insert this inode */
		free(inode->path);
		free(inode);
		pthread_spin_unlock(&g_lfs_inode_hash_lock);
		return old;
	}

fail:
	pthread_spin_unlock(&g_lfs_inode_hash_lock);
	free(inode);
	return NULL;
}

/**
 * @brief clean inode and free related objects
 *		  caller must lock both inode and hash lock.
 * 
 * @param inode 
 */
static void clean_lfs_inode(struct lfs_inode *inode)
{
	khint_t k;

	pthread_spin_lock(&g_lfs_inode_hash_lock);
	k = kh_get(lfs_inode_hash, g_lfs_inode_hash, inode->path);
	l_debug("[%s] inode(%s) free key(%d)", __func__, inode->path, k);
	BUG_ON(k == kh_end(g_lfs_inode_hash), "key not exist");
	kh_del(lfs_inode_hash, g_lfs_inode_hash, k);
	pthread_spin_unlock(&g_lfs_inode_hash_lock);

	if (inode->shm_header == NULL)
		goto free_inode;

	if (syscall_no_intercept(SYS_munmap, inode->shm_header, SHM_SIZE) < 0)
		log_error("munmap error");

	if (syscall_no_intercept(SYS_close, inode->shm_fd) < 0)
		perror("close error");

	inode->shm_header = NULL;

free_inode:
	if (inode->data && syscall_no_intercept(SYS_munmap, inode->data,
						OXBOW_MAX_FILE_SIZE) < 0)
		log_error("munmap error");

	// pthread_spin_destroy(&inode->li_lock);
	free(inode->path);
	free(inode);
}

int remove_lfs_inode(const char *path)
{
	struct lfs_inode *inode;

	inode = NULL;

	pthread_spin_lock(&g_lfs_inode_hash_lock);
	inode = get_lfs_inode_fast(path);
	pthread_spin_unlock(&g_lfs_inode_hash_lock);
	if (!inode) {
		// log_debug("[%s] inode not exist", __func__);
		return 0;
	}

	clean_lfs_inode(inode);
	return 1;
}

int rename_lfs_inode(const char *oldpath, const char *newpath)
{
	struct lfs_inode *inode;

	inode = NULL;

	pthread_spin_lock(&g_lfs_inode_hash_lock);
	inode = get_lfs_inode_fast(oldpath);
	pthread_spin_unlock(&g_lfs_inode_hash_lock);
	if (!inode)
		return 0;


	if (change_hash(inode, oldpath, newpath) < 0) {
		log_error("[%s] failed", __func__);
		return -1;
	}
	return 1;
}

// void put_lfs_inode(struct lfs_inode *inode)
// {
// 	void *shm = NULL;
// 	int shm_fd = 0;

// 	// pthread_spin_lock(&g_lfs_inode_hash_lock);
// 	pthread_spin_lock(&inode->li_lock);
// 	atomic_fetch_sub(&inode->ref, 1);
// 	if (inode->ref == 0 && inode->shm_header) {
// 		shm = inode->shm_header;
// 		shm_fd = inode->shm_fd;
// 		inode->shm_header = NULL;
// 		inode->shm_fd = 0;
// 		inode->li_state = LFS_INODE_INIT;
// 		// clean_lfs_inode(inode);
// 		// return;
// 	}
// 	pthread_spin_unlock(&inode->li_lock);
// 	// pthread_spin_unlock(&g_lfs_inode_hash_lock);

// 	if (shm) {
// 		if (syscall_no_intercept(SYS_munmap, shm, SHM_SIZE) < 0)
// 			log_error("munmap error");
// 		if (syscall_no_intercept(SYS_close, shm_fd) < 0)
// 			perror("close error");
// 	}
// }

void put_lfs_inode(struct lfs_inode *inode)
{
	pthread_spin_lock(&g_lfs_inode_hash_lock);
	atomic_fetch_sub(&inode->ref, 1);
	// if (inode->ref == 0) {
	// 	clean_lfs_inode(inode);
	// 	return;
	// }
	pthread_spin_unlock(&g_lfs_inode_hash_lock);
}

/**
 * @brief 
 * 
 * @param inode 
 * @param pg_idx 
 * @param bits (for unlocking)
 * @return int (success on 0)
 */
int lock_page_bit(struct lfs_inode *inode, size_t pg_idx, atomic_char **bits)
{
	char mask, exp, dxrd;
	int count = 0;

	*bits = get_page_bits((void *)inode->shm_header, pg_idx, LOCKBIT);
	if (*bits == NULL)
		return -1;

	mask = 1 << (pg_idx % BITS_IN_BYTE);

	PF_TL_START(aca_page_lock_wait);

	// spinning for locking mechanism for pagebit
	do {
		exp = atomic_load(*bits);

		if ((exp & mask) == 0) {
			dxrd = exp | mask;
			if (atomic_compare_exchange_strong(*bits, &exp, dxrd))
				break;
		}

		// Just for debugging...
		count++;
		if (count == 1000000)
			oxb_warn("[%s] %s at %d", __func__, inode->path,
				 pg_idx);
	} while (1);

	PF_TL_END(aca_page_lock_wait);

	// print_page_bits(*bits);
	return 0;
}

void dirty_page_group(struct lfs_inode *inode, size_t pg_idx)
{
	char *ptr = (char *)inode->shm_header + 2 * PAGE_SIZE;
	int pos = pg_idx * PAGE_SIZE / PEB_COVERAGE;

	shm_page_set_bit((void *)ptr, pos);
}

void dirty_page_bit(struct lfs_inode *inode, size_t pg_idx)
{
	char mask;
	atomic_char *bits;
	int prev;

	bits = get_page_bits((void *)inode->shm_header, pg_idx, DIRTYBIT);
	if (bits == NULL)
		return;

	mask = 1 << (pg_idx % BITS_IN_BYTE);
	prev = atomic_fetch_or(bits, mask);

	if ((prev & mask) == 0) {
		// Count dirty page as the bit is changed.
		increment_dirty_page_count(1);
	}

	// print_page_bits(*bits);
	l_debug("[%s] bit(%x)", __func__, *bits);
}

/**
 * @brief setting uptodate bit for page, daemon will not read this page from disk.
 */
void uptodate_page_bit(struct lfs_inode *inode, size_t pg_idx)
{
	char mask;
	atomic_char *bits;

	bits = get_page_bits((void *)inode->shm_header, pg_idx, UPTODATEBIT);
	if (bits == NULL)
		return;

	mask = 1 << (pg_idx % BITS_IN_BYTE);
	atomic_fetch_or(bits, mask);
}

void set_skipread_bit(struct lfs_inode *inode, size_t pg_idx)
{
	char mask;
	atomic_char *bits;

	bits = get_page_bits((void *)inode->shm_header, pg_idx, SKIPREAD_BIT);
	if (bits == NULL)
		return;

	mask = 1 << (pg_idx % BITS_IN_BYTE);
	atomic_fetch_or(bits, mask);
}

void clear_skipread_bit(struct lfs_inode *inode, size_t pg_idx)
{
	char mask;
	atomic_char *bits;

	bits = get_page_bits((void *)inode->shm_header, pg_idx, SKIPREAD_BIT);
	if (bits == NULL)
		return;

	mask = ~(1 << (pg_idx % BITS_IN_BYTE));
	atomic_fetch_and(bits, mask);
}

// int init_shared_inode(struct lfs_inode *inode, int fd)
// {
// 	char name[10];
// 	void *ptr;
// 	int daemon_fd, shm_fd, ret = -1;

// 	daemon_fd = get_daemon_file_key(fd);
// 	if (daemon_fd < 0) {
// 		log_error("invalid fd(%d) from daemon (%d)", fd, daemon_fd);
// 		return -1;
// 	} else if (daemon_fd == 0) {
// 		if (inode->data == NULL) {
// 		}
// 	}

// 	// name will be a file descriptor of daemon
// 	sprintf(name, "ox_%d", daemon_fd);

// 	pthread_spin_lock(&inode->li_lock);
// 	if (inode->li_state == LFS_INODE_OPEN) {
// 		/* initiation is already done by other thread */
// 		pthread_spin_unlock(&inode->li_lock);
// 		return 0;
// 	}

// 	shm_fd = shm_open(name, O_RDWR, 0666);
// 	if (shm_fd == -1) {
// 		log_error("name(%s)", name);
// 		perror("shm_open failed");
// 		goto unlock;
// 	}

// 	ptr = mmap(0, SHM_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd, 0);
// 	if (ptr == MAP_FAILED) {
// 		perror("mmap failed");
// 		close(shm_fd);
// 		goto unlock;
// 	}

// 	inode->shm_header = (struct shm_shared_state *)ptr;
// 	inode->li_state = LFS_INODE_OPEN;
// 	inode->shm_fd = shm_fd;
// 	ret = 0;

// unlock:
// 	pthread_spin_unlock(&inode->li_lock);
// 	return ret;
// }

size_t get_oxbow_isize(struct lfs_file *file)
{
	struct lfs_inode *inode = file->f_inode;
	if (!inode->shm_header)
		init_shared_file_metadata(file);

	// oxb_warn("f(%s) GET size %lu (%lu MB, %lu GB)", inode->path,
	// 	 inode->shm_header->i_size,
	// 	 inode->shm_header->i_size / 1024 / 1024,
	// 	 inode->shm_header->i_size / 1024 / 1024 / 1024);

	return inode->shm_header->i_size;
}

void set_oxbow_isize(struct lfs_file *file, size_t new)
{
	struct lfs_inode *inode = file->f_inode;
	if (!inode->shm_header)
		init_shared_file_metadata(file);

	inode->shm_header->i_size = new;

	// oxb_warn("f(%s) SET size %lu (%lu MB, %lu GB)", inode->path, new,
	// 	 new / 1024 / 1024, new / 1024 / 1024 / 1024);
}

int shared_inode_lock(struct lfs_inode *inode)
{
	if (inode->shm_header == NULL) {
		log_error("[%s] inode->shm_header is NULL", __func__);
		return 0;
	}

	return pthread_rwlock_wrlock(&inode->shm_header->i_rwlock);
}

int shared_inode_rdlock(struct lfs_inode *inode)
{
	if (inode->shm_header == NULL) {
		log_error("[%s] inode->shm_header is NULL", __func__);
		return 0;
	}

	return pthread_rwlock_rdlock(&inode->shm_header->i_rwlock);
}

int shared_inode_unlock(struct lfs_inode *inode)
{
	if (inode->shm_header == NULL) {
		log_error("[%s] inode->shm_header is NULL", __func__);
		return 0;
	}

	return pthread_rwlock_unlock(&inode->shm_header->i_rwlock);
}

void shared_data_dirty(struct lfs_inode *inode)
{
	if (inode->shm_header == NULL) {
		log_error("[%s] inode->shm_header is NULL", __func__);
		return;
	}

	atomic_fetch_or(&inode->shm_header->i_state, SHM_DATA_DIRTY);
}
