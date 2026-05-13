#ifndef _PROFILE_DEVFS_H_
#define _PROFILE_DEVFS_H_
#include "profile.h"

// Use SIGRTMIN. (SIGUSR1 is used by filebench, SIGUSR2 is used by thpool library.)
#define PF_SIGNAL SIGRTMIN

#ifdef OXBOW_PROFILE

/* Events. */
// Thread local events.
extern PF_TL_EVT(evt_fetch_data); // Per RDMA server thread.

// Global events.
extern PF_TL_EVT(evt_bg_journal);
extern PF_TL_EVT(evt_fetch_meta);
extern PF_TL_EVT(evt_commit);
extern PF_TL_EVT(a_persist_data);
extern PF_TL_EVT(evt_nvmf_dispatch_fast);
extern PF_TL_EVT(evt_nvmf_poll_fast);

// Block counter.
extern PF_TL_EVT(z1_bg_commits); // Same as # of desc blks and commit blks.
extern PF_TL_EVT(z2_fetch_reqs);
extern PF_TL_EVT(
	z3_data_buf_blks_persisted); // blocks transferred via data buffer. (data, sb_meta, dirent blks)
extern PF_TL_EVT(
	z4_meta_buf_blks_persisted); // blocks transferred via meta buffer. (desc, etag, stage_trace blks)
#endif

#ifdef OXBOW_TRACK_TPUT
// Realtime throughput.
extern PF_EVT(COMMIT_DATA_IO);

#endif

#endif
