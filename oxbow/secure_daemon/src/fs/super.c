#include "fs/fs.h"
#include "oxbow_debug.h"
#include "profile_secure_daemon.h"
/* standard lib */
#include <stdlib.h>

struct super_block *alloc_super_block(void)
{
	struct super_block *sb;

	sb = calloc(1, sizeof(*sb));
	if (!sb) {
		oxb_error("calloc failed");
		return NULL;
	}

	INIT_LIST_HEAD(&sb->s_inodes);
	INIT_LIST_HEAD(&sb->s_mapping.private_list);
	pthread_spin_init(&sb->s_inode_list_lock, PTHREAD_PROCESS_PRIVATE);
	pthread_spin_init(&sb->s_mapping.private_lock, PTHREAD_PROCESS_PRIVATE);
	pthread_rwlock_init(&sb->fs_lock, 0);

	return sb;
}

sector_t get_free_blocks(struct super_block *sb, u32 nr)
{
	u32 ret;

	ret = -1;
	if (!sb->s_op->get_free_blocks) {
		oxb_error("no function");
		goto ret;
	}

	PF_TL_START(y2_fs_rdlock_wait);

	FS_RDLOCK(sb);

	PF_TL_END(y2_fs_rdlock_wait);

	ret = sb->s_op->get_free_blocks(sb, nr);

	FS_UNLOCK(sb);

ret:
	return ret;
}

void put_blocks(struct super_block *sb, sector_t bno, u32 nr)
{
	if (!sb->s_op->put_blocks) {
		oxb_error("no function");
		return;
	}

	PF_TL_START(y2_fs_rdlock_wait);

	FS_RDLOCK(sb);

	PF_TL_END(y2_fs_rdlock_wait);

	sb->s_op->put_blocks(sb, bno, nr);

	FS_UNLOCK(sb);
}

unsigned long get_free_inode(struct super_block *sb)
{
	unsigned long ret;

	ret = -1;
	if (!sb->s_op->get_free_inode) {
		oxb_error("no function");
		goto ret;
	}

	PF_TL_START(y2_fs_rdlock_wait);

	FS_RDLOCK(sb);

	PF_TL_END(y2_fs_rdlock_wait);

	ret = sb->s_op->get_free_inode(sb);

	FS_UNLOCK(sb);
ret:
	return ret;
}

void put_inode(struct super_block *sb, unsigned long ino)
{
	if (!sb->s_op->put_inode) {
		oxb_error("no function");
		return;
	}

	PF_TL_START(y2_fs_rdlock_wait);

	FS_RDLOCK(sb);

	PF_TL_END(y2_fs_rdlock_wait);

	sb->s_op->put_inode(sb, ino);

	FS_UNLOCK(sb);
}
