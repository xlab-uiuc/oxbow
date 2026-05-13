#include <fcntl.h>
#include <pthread.h>
#include <sys/mman.h>
#include <unistd.h>
#include <common/time_stat.h>
#include "fs.h"
#include "libfs/msg_handler.h"
#include "oxbow_debug.h"
#include "profile_secure_daemon.h"

PF_TL_EVT(z14_msg_add_journal);

int handle_fsync(struct msg_data *msg_body)
{
	struct inode *inode;

	inode = msg_body->fsync.daemon_inode_va;
	if (!inode) {
		oxb_error("no inode");
		return -1;
	}

	return do_fsync(inode);
}

/**
 * @brief We don't have shm for directory, instead use this
 * @return int 
 */
// int handle_ino_fsync(struct msg_data *msg_body)
// {
// 	struct inode *inode;
// 	int ret;

// 	// log_info("ino_fsync ino(%lu)", msg_body->ino_fsync.ino);

// 	inode = iget_locked(NULL, msg_body->ino_fsync.ino);
// 	if (!inode) {
// 		oxb_error("no inode");
// 		return -1;
// 	}

// 	ret = do_fsync(inode);

// 	iput(inode);
// 	return ret;
// }

int handle_sync(void)
{
	/* do we have to check pid? */
	return sync_fs();
}

int handle_add_journal(struct msg_data *msg_body)
{
	struct inode *inode;

	PF_TL_CNT(z14_msg_add_journal, 1);

	inode = msg_body->add_journal.daemon_inode_va;
	if (!inode) {
		oxb_error("no inode");
		return -1;
	}

	inode_add_journal(inode);
	return 0;
}

int handle_ftruncate(struct msg_data *msg_body)
{
	struct inode *inode;

	inode = msg_body->fsync.daemon_inode_va;
	if (!inode) {
		oxb_error("no inode");
		return -1;
	}

	if (S_ISDIR(inode->i_mode)) {
		oxb_error("not supported yet");
		return -1;
	}

	return do_ftruncate(inode);
}

int handle_fallocate(struct msg_data *msg_body)
{
	struct inode *inode;

	inode = msg_body->fsync.daemon_inode_va;
	if (!inode) {
		oxb_error("no inode");
		return -1;
	}

	if (S_ISDIR(inode->i_mode)) {
		oxb_error("not supported yet");
		return -1;
	}

	return do_fallocate(inode);
}
