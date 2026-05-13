#ifndef _PROFILE_SECURE_DAEMON_H_
#define _PROFILE_SECURE_DAEMON_H_
#include "profile.h"

// Use SIGRTMIN. (SIGUSR1 is used by filebench, SIGUSR2 is used by thpool library.)
#define PF_SIGNAL SIGRTMIN

/*
 * Enable lightweight contention measurement on `journal_control_ctx::lock`
 * (j->lock). When defined, jlock_acquire() wraps each acquire site with a
 * trylock + rdtsc bracket and accumulates atomic counters for total /
 * contended / wait_cycles; cumulative stats print to stdout from
 * exit_secure_daemon(). When NOT defined, jlock_acquire() inlines to a
 * plain pthread_spin_lock() with zero overhead.
 *
 * See oxbow/secure_daemon/include/utils/jlock_profile.h and §8.6 of
 * plans/2026-04-20-scalability-bottleneck-analysis.md.
 */
// #define JLOCK_PROFILE

#ifdef OXBOW_PROFILE
/* Events. */
// Thread local events.
// extern PF_TL_EVT(evt_fetch_data); // Per RDMA server thread.

// Global events.

// fsync path.
extern PF_TL_EVT(a_evt_sd_fsync);
extern PF_TL_EVT(ab__evt_sd_stg);
extern PF_TL_EVT(ac___evt_sd_stg_build_tx);
extern PF_TL_EVT(
	ad____evt_wait_shi_lock_fg); // shi lock wait time at fsync path.
extern PF_TL_EVT(ae____evt_stage_inode);
extern PF_TL_EVT(af_____evt_inode_alloc); // inode alloc time at fsync path.
extern PF_TL_EVT(ah____evt_inode_gather_dirty);
extern PF_TL_EVT(ai_____evt_gather_dirty_clear_bit);
extern PF_TL_EVT(aj_____evt_gather_dirty_bits);
extern PF_TL_EVT(aja_____evt_add_range_dirty);
// extern PF_TL_EVT(k______evt_gather_dirty_main_loop);
// extern PF_TL_EVT(l______evt_add_range_dirty);
extern PF_TL_EVT(am____evt_stage_file_data);
extern PF_TL_EVT(an_____evt_stage_fill_index_blk);
extern PF_TL_EVT(ao_____evt_stage_fill_tags);
extern PF_TL_EVT(ap_____evt_stage_fix_blocks);
extern PF_TL_EVT(aq_____evt_stage_fill_data);
extern PF_TL_EVT(ar___evt_sd_stg_io_tx);
extern PF_TL_EVT(as___evt_sd_stg_io_desc);
extern PF_TL_EVT(at___evt_sd_stg_io_commit);
extern PF_TL_EVT(au___evt_sd_stg_io_async_wait);

// read path.
extern PF_TL_EVT(b_evt_readahead);
extern PF_TL_EVT(b_evt_readpage);
extern PF_TL_EVT(ba_mpage_readpage);
extern PF_TL_EVT(baa_get_blocks);
extern PF_TL_EVT(bb_mpage_readahead);
extern PF_TL_EVT(bbca_get_blocks_ra);
extern PF_TL_EVT(bba_ra_cache_consume);
extern PF_TL_EVT(bbb_ra_check_shm);
extern PF_TL_EVT(bbc_ra_issue_bio);
extern PF_TL_EVT(bbcb_alloc_init_bio);
extern PF_TL_EVT(bbcc_submit_bio);
/* Read-path IOCTL costs (profiling) */
extern PF_TL_EVT(e002rca_ioctl_ra_end);
extern PF_TL_EVT(e004_ioctl_read_end);
extern PF_TL_EVT(bb1_ra_cache_consume);

// manager
extern PF_TL_EVT(fm_poll);
extern PF_TL_EVT(fm_read);
extern PF_TL_EVT(fm_revive);
extern PF_TL_EVT(fm_fdmap);

// Background journaling.
extern PF_TL_EVT(c_bg_jnl);
extern PF_TL_EVT(ca_build_tx);
extern PF_TL_EVT(caa_wait_ref_down);
extern PF_TL_EVT(cab_snapshot_meta);
extern PF_TL_EVT(cac_snapshot_file);
extern PF_TL_EVT(caca_jtx_reg_jnl_meta);
extern PF_TL_EVT(cacaa_shinode_lock_wait);
extern PF_TL_EVT(cacab_reg_inode);
extern PF_TL_EVT(cacabs_alloc_blocks);
extern PF_TL_EVT(cacabt_write_inode);
extern PF_TL_EVT(cacabu_write_inode);
extern PF_TL_EVT(cacabv_dirty_blocks);
extern PF_TL_EVT(cacad_gather_dirty);
extern PF_TL_EVT(cacadf_gather_dirty_clear_bit);
extern PF_TL_EVT(cacadg_gather_dirty_bits);
extern PF_TL_EVT(cacadga_add_range_dirty);
extern PF_TL_EVT(cacb_jtx_reg_jnl_stage);
extern PF_TL_EVT(cacc_jtx_reg_jnl_file);
extern PF_TL_EVT(caccm_copy_data_blks);
extern PF_TL_EVT(caccmi_data_copy);
extern PF_TL_EVT(caccmj_unlock_pagebit);
extern PF_TL_EVT(caccmk_request_df);
extern PF_TL_EVT(caccml_tx_alloc_df_buf);
extern PF_TL_EVT(caccn_idx_lock_wait);
extern PF_TL_EVT(cacco_get_blocks);
extern PF_TL_EVT(cad_snapshot_inode);
extern PF_TL_EVT(cb_commit);
extern PF_TL_EVT(cc_alloc_df);

// memcpy in building journal tx.
extern PF_TL_EVT(d_meta_copy);

// I/O.
extern PF_TL_EVT(e001_nvme_rd_submit_bh);
extern PF_TL_EVT(e002_nvme_rd_submit_bio);
extern PF_TL_EVT(e002a_nvme_seq_acquire);
extern PF_TL_EVT(e002a1_nvme_seq_check_free);
extern PF_TL_EVT(e002a2_nvme_seq_mark_busy);
extern PF_TL_EVT(e002a3_nvme_seq_qdepth_sample);
extern PF_TL_EVT(e002b_nvme_spdk_submit);
extern PF_TL_EVT(e002c_nvme_poll_loop);
extern PF_TL_EVT(e002d_nvme_poll_once);
extern PF_TL_EVT(e002rb_nvme_copy_from_spdk);
extern PF_TL_EVT(e002f_rd_enqueue);
extern PF_TL_EVT(e002rc_nvme_end_io);
extern PF_TL_EVT(e002h_rd_thpool_add);
extern PF_TL_EVT(e002r_read_complete);

// Misc.
extern PF_TL_EVT(x001_slab_alloc);
extern PF_TL_EVT(x002_slab_grow);
extern PF_TL_EVT(x003_slab_waiting_grow);

// FS lock.
extern PF_TL_EVT(y1_fs_wrlockup_time);
extern PF_TL_EVT(y2_fs_rdlock_wait);
extern PF_TL_EVT(y3_fs_wrlock_wait);

// Counters. (Total number of blocks processed.)
extern PF_TL_EVT(z01_jnl_nblks_data);
extern PF_TL_EVT(z02_jnl_nblks_sb_meta);
extern PF_TL_EVT(z03_jnl_nblks_inode_meta);
extern PF_TL_EVT(z04_jnl_nblks_inode);
extern PF_TL_EVT(z05_jnl_nblks_dirent);
extern PF_TL_EVT(z06_stg_n_trace_blks);
extern PF_TL_EVT(z07_stg_tx_cnt); // # of txs staged.
extern PF_TL_EVT(z08_stg_n_inodes); // # of inodes staged.
extern PF_TL_EVT(z09_stg_idx_tags); // # of stage tags for index blocks.
extern PF_TL_EVT(z10_stg_idx_nblks); // # of blks for index blocks.
extern PF_TL_EVT(z11_stg_data_tags); // # of stage tags for data blocks.
extern PF_TL_EVT(z12_stg_data_nblks); // # of blks for data blocks.

// Open path file worker init.
extern PF_TL_EVT(open1_syscall_getfd);
extern PF_TL_EVT(open4_mmap);
extern PF_TL_EVT(open5_reopen_re);
extern PF_TL_EVT(open2_init_shm);
extern PF_TL_EVT(open3_ioctl_init_file);
extern PF_TL_EVT(open4_init_datapath);

extern PF_TL_EVT(z13_total_etag_nblks); // # of total etag blocks.

extern PF_TL_EVT(z14_msg_add_journal); // # of handling add_journal msgs.
#endif

#ifdef OXBOW_TRACK_TPUT
// Real-time throughput.
extern PF_EVT(SYNC_memcpy_data);
extern PF_EVT(SYNC_io_data);
extern PF_EVT(JNL_memcpy_data);
extern PF_EVT(JNL_dma_copy);
extern PF_EVT(RA_KNL_REQ);
extern PF_EVT(RA_USER_REQ);
#endif

#endif
