#ifndef _FS_H_
#define _FS_H_
#define _GNU_SOURCE
#include "types.h"
#include "common/shm.h"
#include <sys/stat.h>
#include "common/linux/oxbow_kernel.h"
#include "oxbow_debug.h"
#include "common/khash.h"
#include "common/global.h"

typedef unsigned long pgoff_t;

// Client of RPC SHMEM channel to Secure Daemon.
extern struct rpc_ch_info *g_rpc_sd_client;

enum {
	__LFS_INODE_FREE = 0, //
	__LFS_INODE_INIT, // we have to wait for it
	__LFS_INODE_OPEN, // shm done, don't do init
};

#define LFS_INODE_FREE (1 << __LFS_INODE_FREE)
#define LFS_INODE_INIT (1 << __LFS_INODE_INIT)
#define LFS_INODE_OPEN (1 << __LFS_INODE_OPEN)

/* libfs inode object (unique per file) */
// Why it necessary?
// If open the file multiple times, we must contain have one sinlge object.
struct lfs_inode {
	char *path; // used as a key
	void *data;
	pthread_spinlock_t li_lock;
	int li_state;
	atomic_int ref; // reference counter

	struct {
		/* shared file state with daemon */
		struct shm_shared_state *shm_header;
		int shm_fd;
	};
	struct {
		/* only for daemon */
		unsigned long ino;
		mode_t mode;
	};
};

/** @brief Libfs file state */
enum {
	LFS_FILE_INIT = 0,
	LFS_FILE_DONE, // qualift opened file
	LFS_FILE_BAD,
};

/**
 * @brief This object is created whenever libfs process open the file.
 * 		  Also, it is one-to-one with file descriptor from kernel.
 */
struct lfs_file {
	/* registered in libfs */
	int fd;
	int flags;

	/* file infos */
	loff_t pos;
	int state;

	struct lfs_inode *f_inode; // cached value
	void *data;

	/* concurrency */
	pthread_spinlock_t f_lock;
	atomic_uint ref_cnt;
};

struct open_file_table {
	/* Both array has offset of fd */
	struct lfs_file open_files[OXBOW_MAX_OPEN_FILE]; // fd as a offset
	pthread_spinlock_t lock;

	// struct shm_dfd *fd_key_map; // key_map int[OXBOW_MAX_OPEN_FILE]
	atomic_int *fd_key_map; // key_map int[OXBOW_MAX_OPEN_FILE]
	int index_from_daemon;
};

// TODO : APIs
void init_libfs_fdt(void);
void init_libfs_fdt_key_map(void *, int);
int get_daemon_file_key(int fd);
void close_daemon_file_key(struct lfs_inode *li, int fd);
int attach_lfs_file(int fd, const char *path, int flags);
struct lfs_file *get_lfs_file(int fd);
// int init_lfs_file(struct lfs_file *f, const char *path, int flags);
void put_lfs_file(struct lfs_file *f);
void free_lfs_file(struct lfs_file *f);
void increase_file_ref_cnt(struct lfs_file *f);
uint32_t decrease_file_ref_cnt(struct lfs_file *f);

/* l_inode.c */
// libfs inode management
KHASH_MAP_INIT_STR(lfs_inode_hash, struct lfs_inode *)
void lfs_inode_hash_init(void);
void lfs_inode_hash_destory(void);
struct lfs_inode *get_lfs_inode(const char *path);
void put_lfs_inode(struct lfs_inode *);
int remove_lfs_inode(const char *path);
int rename_lfs_inode(const char *oldpath, const char *newpath);

// shm management
#ifdef SYSV_SHM
int init_shared_inode(struct lfs_inode *, key_t);
int lock_page_bit(struct lfs_inode *, size_t pg_idx, pmt_ent_t **,
		  atomic_char **bits);
void unlock_page_bit(atomic_char *bits, size_t pg_idx);
void dirty_page_bit(pmt_ent_t *, atomic_char *bits, size_t pg_idx);
void clear_dirty_page_bit(atomic_char *bits, size_t pg_idx);
#else
int lock_page_bit(struct lfs_inode *, size_t pg_idx, atomic_char **bits);
void dirty_page_bit(struct lfs_inode *, size_t pg_idx);
void dirty_page_group(struct lfs_inode *, size_t pg_idx);
void uptodate_page_bit(struct lfs_inode *, size_t pg_idx);
void set_skipread_bit(struct lfs_inode *, size_t pg_idx);
void clear_skipread_bit(struct lfs_inode *, size_t pg_idx);

#endif
size_t get_oxbow_isize(struct lfs_file *);
void set_oxbow_isize(struct lfs_file *, size_t new);
int shared_inode_lock(struct lfs_inode *);
int shared_inode_rdlock(struct lfs_inode *);
int shared_inode_unlock(struct lfs_inode *);
void shared_data_dirty(struct lfs_inode *);

/* file.c */
int file_do_mmap(struct lfs_file *);
int init_shared_file_metadata(struct lfs_file *);
int libfs_file_fsync(struct lfs_file *);
int libfs_sync(void);
int libfs_file_fstat(struct lfs_file *, struct stat *);

/* read_write.c */
ssize_t libfs_read(struct lfs_file *, char *, size_t, loff_t *);
ssize_t libfs_write(struct lfs_file *, const char *, size_t, loff_t *);

/* journal.c */
int libfs_journal_start(struct lfs_inode *);
int libfs_journal_end(struct lfs_inode *);

void print_mode(umode_t mode);

#endif
