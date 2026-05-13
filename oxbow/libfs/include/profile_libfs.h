#ifndef _PROFILE_LIBFS_H_
#define _PROFILE_LIBFS_H_
#include "profile.h"

// Use SIGRTMIN. (SIGUSR1 is used by filebench, SIGUSR2 is used by thpool library.)
#define PF_SIGNAL SIGRTMIN

#ifdef OXBOW_PROFILE
/* Events. */
// Thread local events.
extern PF_TL_EVT(a_evt_write);
extern PF_TL_EVT(aa_before_sh_inode_lock_wait);
extern PF_TL_EVT(aa_sh_inode_lock_wait);
extern PF_TL_EVT(ab_journal_start);
extern PF_TL_EVT(aba_msg_send_add_jnl);
extern PF_TL_EVT(ac_page_lock_bit);
extern PF_TL_EVT(aca_page_lock_wait);
extern PF_TL_EVT(ad_write_memcpy);
extern PF_TL_EVT(ae_journal_handle_end);
extern PF_TL_EVT(b_evt_fsync);
extern PF_TL_EVT(b_evt_fsync_shm);
extern PF_TL_EVT(b_evt_fsync_msg);
extern PF_TL_EVT(c_evt_read);
extern PF_TL_EVT(ca_memcpy);
extern PF_TL_EVT(ca_mmap);
extern PF_TL_EVT(shm_connection);
extern PF_TL_EVT(shma_loop);
extern PF_TL_EVT(shmb_open);
extern PF_TL_EVT(shmc_mmap);

// extern PF_TL_EVT(close_a);
// extern PF_TL_EVT(close_ab_sysclose);
#endif

#ifdef OXBOW_TRACK_TPUT
extern PF_EVT(libfs_write_memcpy);
#endif

#endif
