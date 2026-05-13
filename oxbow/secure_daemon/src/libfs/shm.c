#include "fs.h"
#include "common/shm.h"
#include "oxbow_debug.h"
#include "common/oxb_slab.h"
#include <bits/pthreadtypes.h>
#include <stddef.h>
#include <sys/ioctl.h>
#include "profile_secure_daemon.h"
#include <asm-generic/errno-base.h>
#include <fcntl.h>
#include "shm.h"
#include <pthread.h>
#include <stdint.h>
#include <sys/acl.h>
// sudo apt-get install libacl1-dev

#include <sys/mman.h>
#include <sys/types.h>
#include <unistd.h>
#include <pwd.h>
#include <grp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { SHM_ENT_IN_USE = 0, SHM_ENT_INIT, SHM_ENT_FREED };
struct shm_state_entry {
	int state;
	int shm_fd;
	char name[16];
	void *start;
	void *ird;
	void *ird_journal;
	void *ird_journal_waiting0;
	void *ird_journal_waiting1;
};

enum {
	GLOBAL_SHM_INIT = 0,
	GLOBAL_SHM_GC,
	GLOBAL_SHM_EXIT,
};

pthread_t shm_gc_worker;
static int shm_gc_worker_event = 0;
static pthread_cond_t gc_worker_cond = PTHREAD_COND_INITIALIZER;
static pthread_mutex_t gc_worker_mutex = PTHREAD_MUTEX_INITIALIZER;

// per-process fd to shared memory keys

struct global_shm_table {
	struct shm_state_entry she[OXBOW_MAX_SHM];
	pthread_spinlock_t g_lock;
	int free; /* # of free shm */
	int cur;
};

static struct global_shm_table g_shm_table = { 0 };

void log_shm_table(void)
{
	log_info("==== SHM Table ====");
	log_info(" free: %d / max: %d ", g_shm_table.free, OXBOW_MAX_SHM);
	log_info("===================");
}

struct proc_fd_map_entry {
	char name[16];
	int shm_fd;
	int pid;
	// struct shm_dfd *keys;
	atomic_int *keys;
};
struct global_proc_fd_map_table {
	struct proc_fd_map_entry ents[OXBOW_MAX_PROC];
	pthread_spinlock_t lock;
	int connected_proc_nr;
};

static struct global_proc_fd_map_table g_proc_fd_map_table = { 0 };

static oxb_slab_t g_ird_slab; // Slab for inode_range_dirty of waiting dirty list.

static void alloc_bitmap_set_bit(void *page, int bit_pos)
{
	shm_page_set_bit(page, bit_pos);
}

static int clean_unused_shm(void)
{
	pthread_rwlockattr_t attr;
	struct shm_state_entry *she;
	struct shm_shared_state *ptr;
	int i, new, ret = 0;

	pthread_spin_lock(&g_shm_table.g_lock);
	for (i = 0; i < OXBOW_MAX_SHM; i++) {
		if (g_shm_table.she[i].state == SHM_ENT_FREED) {
			she = &g_shm_table.she[i];
			pthread_spin_unlock(&g_shm_table.g_lock);

			munlock(she->start, PAGE_SIZE * 4);
			munmap(she->start, SHM_SIZE);
			close(she->shm_fd);
			shm_unlink(she->name);

			new = shm_open(she->name, O_CREAT | O_RDWR, 0600);
			if (new < 0) {
				perror("shm_open failed");
				goto ret;
			}
			ftruncate(new, SHM_SIZE);
			ptr = mmap(0, SHM_SIZE, PROT_READ | PROT_WRITE,
				   MAP_SHARED, new, 0);
			if (ptr == MAP_FAILED) {
				close(new);
				shm_unlink(she->name);
				goto ret;
			}
			memset(ptr, 0, PAGE_SIZE * 4);
			mlock(ptr, PAGE_SIZE * 4);

			pthread_rwlockattr_init(&attr);
			pthread_rwlockattr_setpshared(&attr,
						      PTHREAD_PROCESS_SHARED);
			pthread_rwlock_init(&ptr->i_rwlock, &attr);
			alloc_bitmap_set_bit((char *)ptr + PAGE_SIZE, 0);

			pthread_spin_lock(&g_shm_table.g_lock);
			she->state = SHM_ENT_INIT;
			g_shm_table.free++;
			ret++;
		}
	}
	pthread_spin_unlock(&g_shm_table.g_lock);
ret:
	return ret;
}

void *shm_gc_thread(void *arg)
{
	int nr;
	pthread_mutex_lock(&gc_worker_mutex);

wait:
	while (!shm_gc_worker_event)
		pthread_cond_wait(&gc_worker_cond, &gc_worker_mutex);
work:
	shm_gc_worker_event = GLOBAL_SHM_INIT;
	nr = clean_unused_shm();
	// log_debug("[%s] freeing %d shm ents", __func__, nr);

	pthread_mutex_unlock(&gc_worker_mutex);
	if (shm_gc_worker_event == GLOBAL_SHM_GC)
		goto work;
	if (shm_gc_worker_event == GLOBAL_SHM_EXIT)
		pthread_exit(0);
	goto wait;
}

static void wake_gc_worker(void)
{
	pthread_spin_unlock(&g_shm_table.g_lock);
	pthread_mutex_lock(&gc_worker_mutex);
	shm_gc_worker_event = GLOBAL_SHM_GC;
	pthread_cond_signal(&gc_worker_cond);
	pthread_mutex_unlock(&gc_worker_mutex);
}

static int find_empty_shm(void)
{
	bool wakeup_gc = false;
	int i, j;

retry:
	pthread_spin_lock(&g_shm_table.g_lock);
	if (g_shm_table.free == 0) {
		log_warn("shm is not enough!");
		wake_gc_worker();
		sleep(1);
		goto retry;
	}

	for (i = 0; i < OXBOW_MAX_SHM; i++) {
		j = (i + g_shm_table.cur + 1) % OXBOW_MAX_SHM;
		if (g_shm_table.she[i].state == SHM_ENT_INIT) {
			g_shm_table.she[i].state = SHM_ENT_IN_USE;
			g_shm_table.cur = j;
			g_shm_table.free--;
			if (g_shm_table.free < (OXBOW_MAX_SHM / 2) &&
			    shm_gc_worker_event != GLOBAL_SHM_GC)
				wakeup_gc = true;
			pthread_spin_unlock(&g_shm_table.g_lock);
			if (wakeup_gc)
				wake_gc_worker();
			return i;
		}
	}
	pthread_spin_unlock(&g_shm_table.g_lock);
	return -1;
}

int check_page_in_shm(struct inode *inode, size_t pg_idx)
{
	return shm_alloc_check(inode->shm_header, pg_idx);
}

int check_uptodate_in_shm(struct inode *inode, size_t pg_idx)
{
	atomic_char *bits;

	bits = get_page_bits((char *)inode->shm_header, pg_idx, UPTODATEBIT);
	return (*bits) & (1 << (pg_idx % BITS_IN_BYTE));
}

void mark_uptodate_in_shm(struct inode *inode, size_t pg_idx)
{
	char mask;
	atomic_char *bits;

	bits = get_page_bits((char *)inode->shm_header, pg_idx, UPTODATEBIT);
	if (!bits)
		return;

	mask = 1 << (pg_idx % BITS_IN_BYTE);
	atomic_fetch_or(bits, mask);
}

static int init_shm_permission(struct inode *inode, char *name)
{
	char path[40];
	struct stat file_stat;
	struct passwd *pw;
	struct group *gr;
	acl_t acl;
	acl_entry_t owner_entry;
	acl_permset_t owner_permset;
	acl_entry_t group_entry;
	acl_permset_t group_permset;

	// Get the permission infos from the opened file
	if (fstat(inode->fd, &file_stat) == -1) {
		oxb_error("fstat failed");
		return -EPERM;
	}

	if (file_stat.st_uid == geteuid())
		return 0;

	pw = getpwuid(file_stat.st_uid);
	gr = getgrgid(file_stat.st_gid);

	if (pw == NULL || gr == NULL) {
		oxb_error("failed");
		return -EPERM;
	}

	snprintf(path, sizeof(path), "%s/%s", SHM_ROOT, name);

	acl = acl_get_file(path, ACL_TYPE_ACCESS);
	if (acl == NULL) {
		oxb_error("acl fail");
		return -EINVAL;
	}

	// Give permission to the file owner
	if (acl_create_entry(&acl, &owner_entry) == -1) {
		oxb_error("acl_create_entry for owner failed");
		acl_free(acl);
		return -EPERM;
	}

	if (acl_set_tag_type(owner_entry, ACL_USER) == -1) {
		oxb_error("acl_set_tag_type for owner failed");
		acl_free(acl);
		return -EPERM;
	}

	if (acl_set_qualifier(owner_entry, &file_stat.st_uid) == -1) {
		oxb_error("acl_set_qualifier for owner failed");
		acl_free(acl);
		return -EPERM;
	}

	if (acl_get_permset(owner_entry, &owner_permset) == -1) {
		oxb_error("acl_get_permset for owner failed");
		acl_free(acl);
		return -EPERM;
	}
	acl_add_perm(owner_permset, ACL_READ | ACL_WRITE);

	if (file_stat.st_gid == getegid())
		goto set;

	// Give permission the the group for that file
	if (acl_create_entry(&acl, &group_entry) == -1) {
		oxb_error("acl_create_entry for group failed");
		acl_free(acl);
		return -EPERM;
	}

	if (acl_set_tag_type(group_entry, ACL_GROUP) == -1) {
		oxb_error("acl_set_tag_type for group failed");
		acl_free(acl);
		return -EPERM;
	}

	if (acl_set_qualifier(group_entry, &file_stat.st_gid) == -1) {
		oxb_error("acl_set_qualifier for group failed");
		acl_free(acl);
		return -EPERM;
	}

	if (acl_get_permset(group_entry, &group_permset) == -1) {
		oxb_error("acl_get_permset for group failed");
		acl_free(acl);
		return -EPERM;
	}
	acl_add_perm(group_permset, ACL_READ | ACL_WRITE);

set:
	// Apply to the shared memory
	if (acl_set_file(path, ACL_TYPE_ACCESS, acl) == -1) {
		perror("acl_set_fail");
		oxb_error("acl_set_file failed %d", errno);
		acl_free(acl);
		return -EPERM;
	}

	d_debug("Access permissions set for owner: %s and group: %s\n",
		pw->pw_name, gr->gr_name);

	acl_free(acl);

	return 0;
}

int init_shm_shared_state(struct inode *inode)
{
	struct shm_shared_state *ptr;
	int i, ret = -1;

	i = find_empty_shm();
	if (i == -1) {
		oxb_error("no empty shm");
		goto err;
	}

	ptr = g_shm_table.she[i].start;

	// It is used exclusively by secure daemon.
	inode->i_stg_dirty_start = g_shm_table.she[i].ird;
	inode->i_journal_dirty_start = g_shm_table.she[i].ird_journal;
	inode->i_journal_waiting_dirty_start[0] = g_shm_table.she[i].ird_journal_waiting0;
	inode->i_journal_waiting_dirty_start[1] = g_shm_table.she[i].ird_journal_waiting1;
	// TODO: It is for file permission checking, disabled for now
	// ret = init_shm_permission(inode, g_shm_table.she[i].name);
	// if (ret)
	// 	goto err;

	ptr->i_ino = inode->i_ino;
	ptr->i_nlink = inode->i_nlink;
	ptr->i_mode = inode->i_mode;
	ptr->i_uid = inode->i_uid;
	ptr->i_gid = inode->i_gid;
	ptr->i_size = inode->i_size;
	atomic_store(&ptr->i_state, 0);
	if (inode->i_state & I_USE_JOURNAL)
		atomic_fetch_or(&ptr->i_state, SHM_USE_JOURNAL);
	ptr->i_blocks = inode->i_blocks;
	ptr->daemon_inode_va = inode;

	inode_lock(inode);
	inode->shm_header = ptr;
	inode->shm_index = i;
	inode_unlock(inode);

	set_oxbow_isize(inode);
	// log_info("[%s] inode(%d) size(%lu) shm_index(%d)", __func__,
	// 	 inode->i_ino, inode->i_size, i);
	if (S_ISDIR(inode->i_mode))
		return 0;

	if (ioctl(inode->fd, ILLUFS_IOCTL_NEWFILE,
		  (u64)((char *)ptr + PEB_START * PAGE_SIZE)) < 0) {
		oxb_error("register failed ino(%lu) fd(%d)", inode->i_ino,
			  inode->fd);
		goto err;
	}

	d_debug("[%s] inode(%d) at 0x%lx", __func__, inode->i_ino,
		(unsigned long)inode);

	ret = 0;
	goto ret;

err:
	inode_lock(inode);
	inode->i_state |= I_FAILED;
	inode_unlock(inode);
ret:
	return ret;
}

void free_shm_shared_state(struct inode *inode)
{
	g_shm_table.she[inode->shm_index].state = SHM_ENT_FREED;

	if (g_shm_table.free < 3 * (OXBOW_MAX_SHM / 4))
		wake_gc_worker();
	// char name[20];

	// d_debug("[%s] inode(%d)", __func__, inode->i_ino);

	// sprintf(name, "%d", inode->fd);

	// if (pthread_rwlock_destroy(&inode->shm_header->i_rwlock) < 0)
	// 	perror("pthread_rwlock_destroy");

	// if (munmap(inode->shm_header, SHM_SIZE) == -1)
	// 	perror("munmap");

	// if (close(inode->shm_fd) < 0)
	// 	perror("close");

	// if (shm_unlink(name) < 0)
	// 	perror("shm_unlink");
}

struct inode_range_dirty *alloc_ird(void)
{
	struct inode_range_dirty *ird;

	ird = oxb_slab_alloc(&g_ird_slab);
	return ird;
}

void free_ird(struct inode_range_dirty *ird)
{
	oxb_slab_free(&g_ird_slab, ird);
}


/**
 * @brief Snapshot the ird by cutting the list. (Cut the list and reset the
 * first entry of the live list.)
 * 
 * @param src_ird The source ird to snapshot.
 * @return struct inode_range_dirty* The snapshot ird.
 */
struct inode_range_dirty *snapshot_ird(struct inode_range_dirty *src_ird)
{
	struct inode_range_dirty *ird = alloc_ird();

	memcpy(ird, src_ird, sizeof(struct inode_range_dirty));

	// Clear the live list.
	src_ird->count = 0;
	src_ird->dirty_blk_nr = 0;
	src_ird->next = NULL;
	return ird;
}

/**
 * @brief Different from the clear function in journal.c, we free the first
 * entry as well because it is allocated while cutting the list.
 * 
 * @param ird_snap 
 */
void free_ird_snapshot(struct inode_range_dirty *ird_snap)
{
	struct inode_range_dirty *cur_ird, *temp_ird;
	cur_ird = ird_snap;

	while (cur_ird != NULL) {
		temp_ird = cur_ird;
		cur_ird = cur_ird->next;

		free_ird(temp_ird);
	}
}

void free_ird_except_first(struct inode_range_dirty *ird)
{
	struct inode_range_dirty *cur_ird, *temp_ird;
	bool first = true;
	cur_ird = ird;

	while (cur_ird != NULL) {
		temp_ird = cur_ird;
		cur_ird = cur_ird->next;

		if (first) {
			// We do not free the first entry because it is shared
			// memory.
			temp_ird->count = 0;
			temp_ird->dirty_blk_nr = 0;
			temp_ird->next = NULL;

			first = false;
			continue;
		}

		free_ird(temp_ird);
	}
}

static int clear_first_set_bit(char *byte)
{
	int pos = 0;

	while ((*byte & (1 << pos)) == 0)
		pos++;

	*byte &= ~(1 << pos);

	return pos;
}

/**
 * @brief Allocate a new ird and add it to the tail of the list.
 * 
 * @param ird The last ird which is full.
 * @return struct inode_range_dirty* An allocated ird.
 */
static struct inode_range_dirty *alloc_next_ird(struct inode_range_dirty *ird)
{
	oxbow_assert(ird->count == RANGE_DIRTY_ENT_MAX);

	ird->next = alloc_ird();
	if (!ird->next) {
		oxb_error("fail to allocate ird using slab.");
		return NULL;
	}

	// As slab does not zero the memory.
	ird->next->count = 0;
	ird->next->dirty_blk_nr = 0;
	ird->next->next = NULL;

	return ird->next;
}

/**
 * @brief Get the next available ird from the list. Allocate a new ird if there
 * is no available ird in the list.
 * 
 * @param head The ird list.
 * @return struct inode_range_dirty* ird list.
 */
static struct inode_range_dirty *get_next_ird(struct inode_range_dirty *head){
	struct inode_range_dirty *ird = head;

	while (ird->count == RANGE_DIRTY_ENT_MAX) {
		if (!ird->next) {
			ird->next = alloc_next_ird(ird);
			if (!ird->next) {
				perror("fail");
				return NULL;
			}
		}
		ird = ird->next;
	}
	return ird;
}

/**
 * @brief Count how many PAGE_SIZE blocks are needed to dump the IRD list.
 * Each non-empty IRD node is serialized as one block.
 */
uint32_t count_ird_dump_blocks(const struct inode_range_dirty *ird)
{
	/* Compact format: header + (delta,len) entries per block.
	 * header and entry sizes follow struct definitions in common/shm.h.
	 * Capacity per block ~= (PAGE_SIZE - header) / 8.
	 */
	if (!ird)
		return 0;

	/* Count total entries after merging adjacent ranges within a node. */
	uint64_t total_entries = 0;
	const struct inode_range_dirty *cur = ird;
	while (cur) {
		if (cur->count > 0)
			total_entries += cur->count;
		cur = cur->next;
	}

	/* Entries per block, based on actual struct sizes */
	const uint32_t header_bytes = (uint32_t)sizeof(struct ird_frame_header);
	const uint32_t entry_bytes = (uint32_t)sizeof(struct ird_frame_entry);
	uint32_t entries_per_block = (PAGE_SIZE > header_bytes) ?
		((PAGE_SIZE - header_bytes) / entry_bytes) : 0;
	if (entries_per_block == 0)
		return 0;

	uint32_t n_blocks = (uint32_t)((total_entries + entries_per_block - 1) / entries_per_block);
	return n_blocks;
}


static int add_range_dirty_journal(struct inode *inode, pgoff_t start, int nr,
				   int df_id)
{
	struct inode_range_dirty *ird;
	struct inode_range_dirty *ird_waiting = NULL;

	/* Ignore zero or negative ranges */
	if (nr <= 0)
		return 0;

	range_dirty rd = { .start = start, .nr = nr };

	// Use i_dirty_tail if available to avoid traversing from the
	// start. (Caching pointer)
	ird = inode->i_journal_dirty_tail ? inode->i_journal_dirty_tail :
					    inode->i_journal_dirty_start;
	ird_waiting = inode->i_journal_waiting_dirty_tail[df_id] ?
			      inode->i_journal_waiting_dirty_tail[df_id] :
			      inode->i_journal_waiting_dirty_start[df_id];

	PF_TL_START(cacadga_add_range_dirty);

	BUG_ON(!ird, "no ird");

	ird = get_next_ird(ird);
	if (!ird) {
		oxb_error("Failed to get next ird.");
		return -1;
	}

	ird->dirty_pages[ird->count] = rd;
	ird->count++;
	ird->dirty_blk_nr += nr;

	// d_debug("[%s] ird->count(%d) %lu %d", __func__, ird->count, start, nr);

	// Update the waiting dirty list.
	ird_waiting = get_next_ird(ird_waiting);
	if (!ird_waiting) {
		oxb_error("Failed to get next waiting dirty ird.");
		return -1;
	}

	ird_waiting->dirty_pages[ird_waiting->count] = rd;
	ird_waiting->count++;
	ird_waiting->dirty_blk_nr += nr;

	// Count total number of waiting dirty blocks.
	// It is used in stage_fix_blocks().
	inode->i_journal_waiting_nr_dirty_blks[df_id] += nr;

	// Update the tail pointer to the current node
	inode->i_journal_dirty_tail = ird;
	inode->i_journal_waiting_dirty_tail[df_id] = ird_waiting;
	PF_TL_END(cacadga_add_range_dirty);

	return 0;
}

static int add_range_dirty_staging(struct inode *inode, pgoff_t start, int nr)
{
	struct inode_range_dirty *ird;

	/* Ignore zero or negative ranges */
	if (nr <= 0)
		return 0;

	range_dirty rd = { .start = start, .nr = nr };

	// Use i_dirty_tail if available to avoid traversing from the
	// start. (Caching pointer)
	ird = inode->i_stg_dirty_tail ? inode->i_stg_dirty_tail :
					inode->i_stg_dirty_start;
	PF_TL_START(aja_____evt_add_range_dirty);

	BUG_ON(!ird, "no ird");

	ird = get_next_ird(ird);
	if (!ird) {
		oxb_error("Failed to get next ird.");
		return -1;
	}

	ird->dirty_pages[ird->count] = rd;
	ird->count++;
	ird->dirty_blk_nr += nr;

	// d_debug("[%s] ird->count(%d) %lu %d", __func__, ird->count, start, nr);

	inode->i_stg_dirty_tail = ird;
	PF_TL_END(aja_____evt_add_range_dirty);

	return 0;
}

/**
 * @brief 
 * 
 * @param inode 
 * @param start 
 * @param nr 
 * @param is_journal 1 if it is called from background journal thread. 0 from
 * staging context.
 * @return int 
 */
static int add_range_dirty(struct inode *inode, pgoff_t start, int nr, int is_journal, int df_id)
{
	if (is_journal)
		return add_range_dirty_journal(inode, start, nr, df_id);
	else
		return add_range_dirty_staging(inode, start, nr);
}

// #define OPTIMIZED_GATHER_DIRTY_BITS
#ifdef OPTIMIZED_GATHER_DIRTY_BITS

// OPTIMIZE: gather dirty_bits. It takes around 23% of random write microbenchmark.

#else

// #define DIRTY_BITS_MASK 0xF0F0F0F0F0F0F0F0ULL  // dirty bit pattern: ... 1111 0000 1111 0000
#define DIRTY_BITS_MASK 0xFFFFFFFFFFFFFFFFULL // no pattern for dirty bits
#define DIRTY_BITS_MASK_CHAR 0xFF // no pattern for dirty bits

static int gather_dirty_bits(struct inode *inode, loff_t bmap_idx, int is_journal, int df_id)
{
	// Optimized.
	char *dl_bmap;
	pgoff_t pg_idx;
	int count = 0, pos = 0;
	uint64_t *word_ptr;
	uint64_t word;
	unsigned char curr_byte;
	int i, bit;
	int byte_offset, lock_byte_offset;

	dl_bmap = get_dirty_lock_bmap(inode, bmap_idx);
	if (!dl_bmap) {
		oxb_error("[%s] dl_bmap not exist", __func__);
		return -1;
	}

	// Reset the pointer for caching.
	if (is_journal){
		inode->i_journal_dirty_tail = NULL;
		inode->i_journal_waiting_dirty_tail[df_id] = NULL;
	} else
		inode->i_stg_dirty_tail = NULL;

	// Process 8 bytes at a time
	// Dirty part is fisrt 1024 bytes of page entry block
	word_ptr = (uint64_t *)dl_bmap;
	for (i = 0; i < (int)BYTES_PER_STATE / 8; i++) {
		word = word_ptr[i];

		// Skip if no dirty bits
		if (!(word & DIRTY_BITS_MASK)) {
			if (count) {
				pg_idx = pos + bmap_idx * NR_PAGES_IN_PEB;
				add_range_dirty(inode, pg_idx, count, is_journal, df_id);
				count = 0;
			}
			continue;
		}

		// Process each byte in the word
		for (int byte = 0; byte < 8; byte++) {
			curr_byte = word >> (byte * 8);
			if (!(curr_byte & DIRTY_BITS_MASK_CHAR)) {
				if (count) {
					pg_idx = pos + bmap_idx * NR_PAGES_IN_PEB;
					add_range_dirty(inode, pg_idx, count, is_journal, df_id);
					count = 0;
				}
				continue;
			}

			byte_offset = i * 8 + byte;
			lock_byte_offset = byte_offset + LOCKBIT_START_INDEX;
			for (bit = 0; bit < (int)BITS_IN_BYTE; bit++) {
				if (curr_byte & (1 << bit)) {
					if (count == 0)
						pos = bit +
						      byte_offset *
							      BITS_IN_BYTE;
					count++;

					// Clear dirty bit and set lock bit
					dl_bmap[byte_offset] &= ~(1 << bit);

					if (dl_bmap[lock_byte_offset] &
					    (1 << bit)) {
						oxb_error("not locked here!");
						return -1;
					}
					dl_bmap[lock_byte_offset] |= 1 << bit;
				} else if (count) {
					pg_idx = pos +
						 bmap_idx * NR_PAGES_IN_PEB;
					add_range_dirty(inode, pg_idx, count, is_journal, df_id);
					count = 0;
				}
			}
		}
	}

	// Handle any remaining dirty pages
	if (count) {
		pg_idx = pos + bmap_idx * NR_PAGES_IN_PEB;
		add_range_dirty(inode, pg_idx, count, is_journal, df_id);
	}

	return 0;
}
#endif

/**
 * @brief 
 * 
 * @param inode 
 * @param is_journal 1 if it is called from background journal thread. 0 from
 * staging context.
 * @param df_id 0 or 1. Not used if is_journal is 0.
 * @return int 
 */
int inode_gather_dirty_pagebits(struct inode *inode, int is_journal, int df_id)
{
	// Optimized.
	char *dg_bmap;
	int i;
	uint64_t *word_ptr;

	dg_bmap = get_dirty_group_bmap(inode);
	if (!dg_bmap) {
		oxb_error("[%s] dgbmap not exist", __func__);
		return -1;
	}

	if (is_journal)
		inode->i_journal_waiting_nr_dirty_blks[df_id] = 0;

	// Process 8 bytes at a time using 64-bit words
	word_ptr = (uint64_t *)dg_bmap;
	for (i = 0; i < BYTES_IN_PAGE / 8; i++) {
		// Skip if no bits are set
		if (!word_ptr[i])
			continue;

		// Process each byte in the word if it contains set bits
		while (word_ptr[i]) {
			int byte_pos = __builtin_ctzll(word_ptr[i]) / 8;
			char *curr_byte = &dg_bmap[i * 8 + byte_pos];

			if (is_journal)
				PF_TL_START(cacadf_gather_dirty_clear_bit);
			else
				PF_TL_START(ai_____evt_gather_dirty_clear_bit);

			int bit_pos = clear_first_set_bit(curr_byte);

			if (is_journal)
				PF_TL_END(cacadf_gather_dirty_clear_bit);
			else
				PF_TL_END(ai_____evt_gather_dirty_clear_bit);

			if (is_journal)
				PF_TL_START(cacadg_gather_dirty_bits);
			else
				PF_TL_START(aj_____evt_gather_dirty_bits);

			gather_dirty_bits(inode, bit_pos + ((i * 8 + byte_pos) *
							    BITS_IN_BYTE), is_journal, df_id);

			if (is_journal)
				PF_TL_END(cacadg_gather_dirty_bits);
			else
				PF_TL_END(aj_____evt_gather_dirty_bits);
		}
	}

	stg_debug("[%s] done", __func__);

	return 0;
}

void inode_unlock_pagebit(struct inode *inode, pgoff_t pg_idx)
{
	atomic_char *bits;

	bits = get_page_bits((void *)inode->shm_header, pg_idx, LOCKBIT);
	unlock_page_bit(bits, pg_idx);
}

int inode_get_stg_dirty_blocks_nr(struct inode *inode, struct inode_range_dirty *ird)
{
	int count = 0;

	while (ird) {
		count += ird->dirty_blk_nr;
		ird = ird->next;
	}

	return count;
}

int shared_inode_lock(struct inode *inode)
{
	BUG_ON(inode->shm_header == NULL, "shm_header is NULL");
	return pthread_rwlock_wrlock(&inode->shm_header->i_rwlock);
}

int shared_inode_unlock(struct inode *inode)
{
	BUG_ON(inode->shm_header == NULL, "shm_header is NULL");
	return pthread_rwlock_unlock(&inode->shm_header->i_rwlock);
}

// Get inode size from shm (libfs <--> daemon)
size_t get_recent_isize(struct inode *inode)
{
	BUG_ON(inode->shm_header == NULL, "shm_header is NULL");
	return inode->shm_header->i_size;
}

// Get inode size from shm (libfs <--> daemon)
void set_oxbow_isize(struct inode *inode)
{
	BUG_ON(inode->shm_header == NULL, "shm_header is NULL");
	// log_debug("[%s] inode(%lu) size: %lu", __func__, inode->i_ino,
	// 	  inode->i_size);
	inode->shm_header->i_size = inode->i_size;
}

char *get_dirty_group_bmap(struct inode *inode)
{
	BUG_ON(inode->shm_header == NULL, "shm_header is NULL");
	return (char *)inode->shm_header + PAGE_SIZE * (PDB_ALLOC_CHECK_NR + 1);
}

char *get_dirty_lock_bmap(struct inode *inode, loff_t idx)
{
	BUG_ON(inode->shm_header == NULL, "shm_header is NULL");
	return (char *)inode->shm_header +
	       PAGE_SIZE * (PDB_DIRTY_GROUP_NR + PDB_ALLOC_CHECK_NR + 1 + idx);
}

void inode_set_shm_state(struct inode *inode, unsigned long flags)
{
	// log_debug("[%s] inode(%lu) flags(%lu)", __func__, inode->i_ino, flags);
	BUG_ON(inode->shm_header == NULL, "shm_header is NULL");
	atomic_fetch_or(&inode->shm_header->i_state, flags);
}

void inode_unset_shm_state(struct inode *inode, unsigned long flags)
{
	BUG_ON(inode->shm_header == NULL, "shm_header is NULL");
	atomic_fetch_and(&inode->shm_header->i_state, ~flags);
}

int get_proc_fd_map_ent(int pid, char *name, int *index)
{
	int i, ret = -1;

	pthread_spin_lock(&g_proc_fd_map_table.lock);
	if (g_proc_fd_map_table.connected_proc_nr == OXBOW_MAX_PROC) {
		log_error("Max process");
		goto unlock;
	}

	for (i = 0; i < OXBOW_MAX_PROC; i++)
		if (g_proc_fd_map_table.ents[i].pid == pid) {
			log_info("pid(%d) already has one", pid);
			strcpy(name, g_proc_fd_map_table.ents[i].name);
			*index = i;
			ret = 0;
			goto unlock;
		}

	for (i = 0; i < OXBOW_MAX_PROC; i++) {
		if (g_proc_fd_map_table.ents[i].pid == 0) {
			*index = i;
			strcpy(name, g_proc_fd_map_table.ents[i].name);
			g_proc_fd_map_table.ents[i].pid = pid;
			g_proc_fd_map_table.connected_proc_nr++;
			ret = 0;
			break;
		}
	}
unlock:
	pthread_spin_unlock(&g_proc_fd_map_table.lock);
	// log_info("[%s] pid(%d) has %d", __func__, pid, *index);
	return ret;
}

int register_file_proc_fdmap(struct inode *inode, int fd, int pid)
{
	struct proc_fd_map_entry *ent = NULL;
	int i, opened;

	opened = -1;
	// closed = -2;

	// log_debug("[%s] inode(%lu) pid(%d) fd(%d)", __func__, inode->i_ino, pid,
	// 	  fd);

	pthread_spin_lock(&g_proc_fd_map_table.lock);
	for (i = 0; i < OXBOW_MAX_PROC; i++) {
		if (g_proc_fd_map_table.ents[i].pid == pid) {
			ent = &g_proc_fd_map_table.ents[i];
			break;
		}
	}
	pthread_spin_unlock(&g_proc_fd_map_table.lock);

	if (!ent) {
		log_error("no entry for pid(%d)/fd(%d)", pid, fd);
		return -1;
	}

	// inode = iget_locked(g_super_block, ino);
	// if (!inode) {
	// 	log_error("inode not found");
	// 	return -1;
	// }
	// iput(inode);

	if (!inode->shm_header) {
		log_warn("[%s] ino(%lu) not initialized", __func__,
			 inode->i_ino);
		init_shm_shared_state(inode);
	}

	/* now libfs can initiate shared memory */
	// ent->keys[fd].dfd = inode->shm_index;
	// ent->keys[fd] = inode->shm_index;

	// retry:
	// if (atomic_compare_exchange_strong(&ent->keys[fd], &opened,
	// 				   inode->shm_index)) {
	// 	d_debug("ino(%lu) fd(%d) setshm(%d)", inode->i_ino, fd,
	// 		inode->shm_index);
	// } else {
	// 	log_warn("not expected value on atomic");
	// }

	// else if (atomic_compare_exchange_strong(&ent->keys[fd], &closed,
	// 					  opened)) {
	// 	d_debug("ino(%lu) fd(%d) already closed", inode->i_ino, fd);
	// } else
	// 	goto retry;

	// log_error("fd(%d) unexpected value(%d)", fd,
	// 	  atomic_load(&ent->keys[fd]));
	atomic_store(&ent->keys[fd], inode->shm_index);

	// log_debug(
	// 	"[%s] inode(%lu) fd_key_map: 0x%lx at 0x%lx pid(%d) fd(%d) done with %d",
	// 	__func__, inode->i_ino, ent->keys, &ent->keys[fd], pid, fd,
	// 	inode->shm_index);
	return 0;
}

int init_proc_fd_map(void)
{
	char name[12];
	void *ptr;
	int i, j, shm_fd;

	for (i = 0; i < OXBOW_MAX_PROC; i++) {
		sprintf(name, "oxp_%d", i);

		shm_fd = shm_open(name, O_CREAT | O_RDWR, 0666);
		if (shm_fd == -1) {
			perror("shm_open failed");
			return -1;
		}

		if (ftruncate(shm_fd, OXBOW_MAX_OPEN_FILE * sizeof(int)) ==
		    -1) {
			perror("ftruncate failed");
			return -1;
		}

		ptr = mmap(0, OXBOW_MAX_OPEN_FILE * sizeof(int),
			   PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd, 0);
		if (ptr == MAP_FAILED) {
			perror("mmap failed");
			return -1;
		}

		memset(ptr, -1, OXBOW_MAX_OPEN_FILE * sizeof(int));
		if (mlock(ptr, OXBOW_MAX_OPEN_FILE * sizeof(int)))
			return -1;

		g_proc_fd_map_table.ents[i].keys = ptr;
		for (j = 0; j < OXBOW_MAX_OPEN_FILE; j++)
			atomic_init(&g_proc_fd_map_table.ents[i].keys[j], -1);
		g_proc_fd_map_table.ents[i].shm_fd = shm_fd;
		g_proc_fd_map_table.ents[i].pid = 0;
		strcpy(g_proc_fd_map_table.ents[i].name, name);
	}
	pthread_spin_init(&g_proc_fd_map_table.lock, 0);
	g_proc_fd_map_table.connected_proc_nr = 0;

	oxb_info("[%s] done", __func__);
	return 0;
}

int init_file_metadata_map(void)
{
	char name[16];
	pthread_rwlockattr_t attr;
	struct shm_shared_state *ptr;
	int i, shm_fd;
	// name will be a file descriptor of daemon

	for (i = 0; i < OXBOW_MAX_SHM; i++) {
		sprintf(name, "ox_%d", i);

		shm_fd = shm_open(name, O_CREAT | O_RDWR, 0600);
		if (shm_fd == -1) {
			perror("shm_open failed");
			return -1;
		}

		if (ftruncate(shm_fd, SHM_SIZE) == -1) {
			perror("ftruncate failed");
			return -1;
		}

		ptr = mmap(0, SHM_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED,
			   shm_fd, 0);
		if (ptr == MAP_FAILED) {
			perror("mmap failed");
			return -1;
		}

		/* Header page + PDB alloc_check + PDB dirty_group + PEB */
		memset(ptr, 0, PAGE_SIZE * 4);
		if (mlock(ptr, PAGE_SIZE * 4))
			return -1;

		pthread_rwlockattr_init(&attr);
		pthread_rwlockattr_setpshared(&attr, PTHREAD_PROCESS_SHARED);
		pthread_rwlock_init(&ptr->i_rwlock, &attr);

		/* setting first entry is allocated */
		alloc_bitmap_set_bit((char *)ptr + PAGE_SIZE, 0);

		g_shm_table.she[i].start = ptr;
		g_shm_table.she[i].shm_fd = shm_fd;
		g_shm_table.she[i].state = SHM_ENT_INIT;
		g_shm_table.she[i].ird =
			calloc(1, sizeof(struct inode_range_dirty));
		if (!g_shm_table.she[i].ird) {
			perror("Allocate staging ird fail");
			return -1;
		}

		g_shm_table.she[i].ird_journal =
			calloc(1, sizeof(struct inode_range_dirty));
		if (!g_shm_table.she[i].ird_journal) {
			perror("Allocate journal ird fail");
			return -1;
		}

		g_shm_table.she[i].ird_journal_waiting0 =
			calloc(1, sizeof(struct inode_range_dirty));
		if (!g_shm_table.she[i].ird_journal_waiting0) {
			perror("Allocate journal ird fail");
			return -1;
		}

		g_shm_table.she[i].ird_journal_waiting1 =
			calloc(1, sizeof(struct inode_range_dirty));
		if (!g_shm_table.she[i].ird_journal_waiting1) {
			perror("Allocate journal ird fail");
			return -1;
		}

		strcpy(g_shm_table.she[i].name, name);
	}
	pthread_spin_init(&g_shm_table.g_lock, 0);
	g_shm_table.cur = 0;
	g_shm_table.free = OXBOW_MAX_SHM;
	oxb_info("[%s] done", __func__);
	return 0;
}

int init_shm_system(void)
{
	int ret;

	ret = oxb_slab_init(&g_ird_slab, sizeof(struct inode_range_dirty));
	if (ret)
		return ret;

	ret = init_proc_fd_map();
	if (ret)
		return ret;

	ret = init_file_metadata_map();
	if (ret)
		return ret;

	pthread_create(&shm_gc_worker, NULL, shm_gc_thread, NULL);
	pthread_detach(shm_gc_worker);

	ret = 0;
	return ret;
}

void exit_shm_system(void)
{
	oxb_slab_destroy(&g_ird_slab);

	oxb_info("[%s] done", __func__);
}

int deserialize_ird_compact(const void *buf,
                            uint32_t nr_blocks,
                            struct inode_range_dirty *dst_head)
{
    if (!buf || !dst_head)
        return -1;

    const uint8_t *in = (const uint8_t *)buf;
    const size_t hdr_sz = sizeof(struct ird_frame_header);
    const size_t ent_sz = sizeof(struct ird_frame_entry);

    struct inode_range_dirty *tail = dst_head;
    /* Reset head */
    dst_head->count = 0;
    dst_head->dirty_blk_nr = 0;
    dst_head->next = NULL;

    for (uint32_t b = 0; b < nr_blocks; b++) {
        const uint8_t *blk = in + (size_t)b * PAGE_SIZE;
        const struct ird_frame_header *hdr = (const struct ird_frame_header *)blk;
        const struct ird_frame_entry *ents = (const struct ird_frame_entry *)(blk + hdr_sz);
        uint32_t n = hdr->n_entries;
        if (hdr_sz + (size_t)n * ent_sz > PAGE_SIZE)
            return -1; /* corrupted */

        uint64_t cur = hdr->base_pg;
        for (uint32_t i = 0; i < n; i++) {
            uint64_t start = cur + (uint64_t)ents[i].delta;
            uint32_t len = ents[i].len;
            cur = start + len;

            /* Append (start,len) to IRD list */
            if (tail->count == RANGE_DIRTY_ENT_MAX) {
                struct inode_range_dirty *next = alloc_ird();
                if (!next)
                    return -1;
                next->count = 0;
                next->dirty_blk_nr = 0;
                next->next = NULL;
                tail->next = next;
                tail = next;
            }
            tail->dirty_pages[tail->count].start = (pgoff_t)start;
            tail->dirty_pages[tail->count].nr = (int)len;
            tail->count++;
            tail->dirty_blk_nr += len;
        }
    }

    return 0;
}
