#include "fs.h"
#include "msg.h"
#include "oxbow_debug.h"
#include "profile_libfs.h"

static int journal_start(struct lfs_inode *inode)
{
	PF_TL_START(aba_msg_send_add_jnl);
	l_debug("[%s] send add_journal request. inode=%lu inode->path=%s",
		 __func__, inode->shm_header->i_ino, inode->path);
	msg_send_sd_add_journal(inode->shm_header->daemon_inode_va);
	PF_TL_END(aba_msg_send_add_jnl);
	return 1;
}

int libfs_journal_start(struct lfs_inode *inode)
{
	unsigned long state;

	state = atomic_load(&inode->shm_header->i_state);
	if (state & (SHM_RUNNING_TX | SHM_RUNNING_TX_REQUESTED))
		return 0;

	atomic_fetch_or(&inode->shm_header->i_state, SHM_RUNNING_TX_REQUESTED);

	return journal_start(inode);
}

int libfs_journal_end(struct lfs_inode *inode)
{
	unsigned int count;
	// TODO: wait on a bit rather than busy waiting
	unsigned long state;

	count = 0;
	state = atomic_load(&inode->shm_header->i_state);
	oxb_debug("waiting for add_journal completion. inode=%lu state=0x%lx",
		 inode->shm_header->i_ino, state);

	while (!(state & (SHM_RUNNING_TX | SHM_I_DELETED))) {
		state = atomic_load(&inode->shm_header->i_state);
		oxbow_spin_backoff(count++);

		// if (count > 100000000) {
		// 	oxb_warn("[%d] inode(%lu) due to count max", gettid(),
		// 		 inode->shm_header->i_ino);
		// 	count = 0;
		// }
	}

	atomic_fetch_and(&inode->shm_header->i_state, ~SHM_RUNNING_TX_REQUESTED);

	oxb_debug("add_journal completed. inode=%lu state=0x%lx count=%d",
		 inode->shm_header->i_ino, state, count);
	return 0;
}
