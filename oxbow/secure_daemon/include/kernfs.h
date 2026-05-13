#ifndef _KERNFS_H_
#define _KERNFS_H_

#include "fs.h"

typedef __u64 kaddr_t;

static inline void illufs_set_auth(struct inode *inode)
{
	struct inode_auth *auth_ent =
		&((struct inode_auth *)inode->i_sb->auth_mmap)[inode->i_ino];

	auth_ent->i_comp = PACK_MODE_SIZE(inode->i_mode, inode->i_size);
	auth_ent->i_uid = inode->i_uid;
	auth_ent->i_gid = inode->i_gid;
}

static inline void illufs_get_auth(struct inode *inode)
{
	struct inode_auth *auth_ent =
		&((struct inode_auth *)inode->i_sb->auth_mmap)[inode->i_ino];

	inode->i_uid = auth_ent->i_uid;
	inode->i_gid = auth_ent->i_gid;
	inode->i_mode = UNPACK_MODE(auth_ent->i_comp);
}

// set inode size in kernel
static inline void illufs_set_i_size(struct inode *inode)
{
	struct inode_auth *auth_ent =
		&((struct inode_auth *)inode->i_sb->auth_mmap)[inode->i_ino];

	auth_ent->i_comp = PACK_MODE_SIZE(inode->i_mode, inode->i_size);
}

/* dir_ops.c */
int request_dir_init(struct inode *);
int init_root_worker(struct super_block *);
int init_fs_manager(void);
void exit_manager(void);
void exit_file_workers(void);
/* file_ops.c */
int request_file_init(struct inode *);
int start_file_dispatcher(void);
int file_dispatcher_deregister(struct inode *);
#ifdef OXBOW_RD_INLINE_SUBMIT
int start_read_workers(void);
void stop_read_workers(void);
#endif

#define GET_ILLUFS_FILE_EVENT_NAME(event)                                      \
	((event) == ILLUFS_FILEWORKER_RA   ? "ILLUFS_FILEWORKER_RA" :          \
	 (event) == ILLUFS_FILEWORKER_READ ? "ILLUFS_FILEWORKER_READ" :        \
	 (event) == ILLUFS_FILEWORKER_NOTIFYOPEN ?                             \
					     "ILLUFS_FILEWORKER_NOTIFYOPEN" :  \
					     "UNKNOWN_EVENT")

#define GET_ILLUFS_EVENT_NAME(event)                                           \
	((event) == ILLUFS_DROP_PGCACHE	 ? "ILLUFS_DROP_PGCACHE" :             \
	 (event) == ILLUFS_MOUNT	 ? "ILLUFS_MOUNT" :                    \
	 (event) == ILLUFS_DIROP_LOOKUP	 ? "ILLUFS_DIROP_LOOKUP" :             \
	 (event) == ILLUFS_DIROP_CREATE	 ? "ILLUFS_DIROP_CREATE" :             \
	 (event) == ILLUFS_DIROP_READDIR ? "ILLUFS_DIROP_READDIR" :            \
	 (event) == ILLUFS_DIROP_UNLINK	 ? "ILLUFS_DIROP_UNLINK" :             \
	 (event) == ILLUFS_RENAME_NEW	 ? "ILLUFS_RENAME_NEW" :               \
	 (event) == ILLUFS_RENAME_OLD	 ? "ILLUFS_RENAME_OLD" :               \
	 (event) == ILLUFS_DIROPS_NOTIFYDIROPEN ?                              \
					"ILLUFS_DIROPS_NOTIFYDIROPEN" :        \
					"UNKNOWN_EVENT")

#define GET_ILLUFS_MANAGER_EVENT_NAME(event)                                   \
	((event) == ILLUFS_MANAGER_REVIVE     ? "ILLUFS_MANAGER_REVIVE" :      \
	 (event) == ILLUFS_MANAGER_NOTIFYOPEN ? "ILLUFS_MANAGER_NOTIFYOPEN" :  \
	 (event) == ILLUFS_MANAGER_EVICT_INODE ?                               \
						"ILLUFS_MANAGER_EVICT_INODE" : \
						"UNKNOWN_EVENT")

#endif
