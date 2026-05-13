#define _GNU_SOURCE
#include <sys/mount.h>
#include "kernfs.h"
#include <unistd.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <signal.h>
#include <mntent.h>
#include "config.h"
#include "secure_daemon.h"
#include "oxbow_debug.h"
#include "common/config.h"
#include "fs/fs.h"
#include "io/nvme.h"
#include "io_dispatcher.h"
#include "msg.h"
#include "profile_secure_daemon.h"
#include "common/dirty_mgmt.h"
#include "utils/sync_device.h"
#include "utils/jlock_profile.h"
#include "common/utils/exp_flag.h"

#ifdef OXBOW_IPC_MSG_RING
#define OXBOW_DAEMON_IPC_MODE_STR "msg_ring"
#else
#define OXBOW_DAEMON_IPC_MODE_STR "legacy"
#endif

// Profiling events.
PF_TL_EVT(a_evt_sd_fsync);
PF_TL_EVT(ab__evt_sd_stg);
PF_TL_EVT(ac___evt_sd_stg_build_tx);
PF_TL_EVT(ad____evt_wait_shi_lock_fg); // shi lock wait time at fsync path.
PF_TL_EVT(ae____evt_stage_inode);
PF_TL_EVT(af_____evt_inode_alloc); // inode alloc time at fsync path.
PF_TL_EVT(ah____evt_inode_gather_dirty);
PF_TL_EVT(ai_____evt_gather_dirty_clear_bit);
PF_TL_EVT(aj_____evt_gather_dirty_bits);
PF_TL_EVT(aja_____evt_add_range_dirty);
// PF_TL_EVT(k______evt_gather_dirty_main_loop);
// PF_TL_EVT(l______evt_add_range_dirty);
PF_TL_EVT(am____evt_stage_file_data);
PF_TL_EVT(an_____evt_stage_fill_index_blk);
PF_TL_EVT(ao_____evt_stage_fill_tags);
PF_TL_EVT(ap_____evt_stage_fix_blocks);
PF_TL_EVT(aq_____evt_stage_fill_data);
PF_TL_EVT(ar___evt_sd_stg_io_tx);
PF_TL_EVT(as___evt_sd_stg_io_desc);
PF_TL_EVT(at___evt_sd_stg_io_commit);
PF_TL_EVT(au___evt_sd_stg_io_async_wait);

PF_TL_EVT(open1_syscall_getfd);
PF_TL_EVT(open4_mmap);
PF_TL_EVT(open5_reopen_re);
PF_TL_EVT(open2_init_shm);
PF_TL_EVT(open3_ioctl_init_file);
PF_TL_EVT(open4_init_datapath);

PF_TL_EVT(b_evt_readahead);
PF_TL_EVT(b_evt_readpage);
PF_TL_EVT(ba_mpage_readpage);
PF_TL_EVT(baa_get_blocks);
PF_TL_EVT(bb_mpage_readahead);
PF_TL_EVT(bbca_get_blocks_ra);
PF_TL_EVT(bba_ra_cache_consume);
PF_TL_EVT(bbb_ra_check_shm);
PF_TL_EVT(bbc_ra_issue_bio);
PF_TL_EVT(bbcb_alloc_init_bio);
PF_TL_EVT(bbcc_submit_bio);
PF_TL_EVT(e002h_rd_thpool_add);

void pf_print_stats(void)
{
	printf("Print profiling stats.\n");
	printf(",=============== PROFILE GLOBAL ===============,,,,\n");
	PF_PRINT_HDR();
	// PF_PRINT_STATS(evt_example, "FSYNC", evt_example);
}

void pf_reset_stats(void)
{
	printf("Reset profiling stats.\n");
	// PF_RESET(evt_example);
}

static void success_starting(void)
{
	printf("\n");
	printf("   ██████╗ ██╗  ██╗██████╗  ██████╗ ██╗    ██╗███████╗███████╗\n");
	printf("  ██╔═══██╗╚██╗██╔╝██╔══██╗██╔═══██╗██║    ██║██╔════╝██╔════╝\n");
	printf("  ██║   ██║ ╚███╔╝ ██████╔╝██║   ██║██║ █╗ ██║█████╗  ███████╗\n");
	printf("  ██║   ██║ ██╔██╗ ██╔══██╗██║   ██║██║███╗██║██╔══╝  ╚════██║\n");
	printf("  ╚██████╔╝██╔╝ ██╗██████╔╝╚██████╔╝╚███╔███╔╝██║     ███████║\n");
	printf("   ╚═════╝ ╚═╝  ╚═╝╚═════╝  ╚═════╝  ╚══╝╚══╝ ╚═╝     ╚══════╝\n");
	printf("┌───────────┐      ╔═════════════════════╗      ┌───────────────┐\n");
	printf("│   LibFS   │<────>║ >> Secure Daemon << ║<────>│     DevFS     │\n");
	printf("│           │      ║      ACTIVATED      ║      │   ACTIVATED   │\n");
	printf("└───────────┘      ╚═════════════════════╝      └───────────────┘\n");
}

int init_secure_daemon(void)
{
	oxb_info("Initializing Secure Daemon");

	load_secure_daemon_configs();
	print_secure_daemon_configs();
	if (validate_secure_daemon_configs() < 0)
		return -1;

	load_common_configs();
	print_common_configs();

#ifdef USE_NVME_STORAGE_ENGINE
	sync_device_init(); // SIGRTMIN + 2
#endif

	jlock_profile_init(); // SIGRTMIN + 1

#ifdef OXBOW_PROFILE
	oxb_warn("Profiling enabled. Disable it to measure performance.");
	pf_init(PF_SIGNAL);
#endif

#ifdef OXBOW_TRACK_TPUT
	// Init global events. (throughput tracking)
	PF_RESET(SYNC_memcpy_data);
	PF_RESET(SYNC_io_data);
	PF_RESET(JNL_memcpy_data);
	PF_RESET(JNL_dma_copy);
	PF_RESET(RA_KNL_REQ);
	PF_RESET(RA_USER_REQ);
#endif
	if (init_dir_workers() < 0)
		return -1;

	if (init_file_workers() < 0)
		return -1;

	return 0;
}

void exit_secure_daemon(void)
{
	oxb_info("Exiting Secure Daemon.");

	jlock_profile_print();

#ifdef OXBOW_PROFILE
	pf_print_evt_lists();
	pf_exit();
#endif
}

static int is_oxbow_mounted(void)
{
	FILE *mounts;
	struct mntent *mnt;

	mounts = setmntent("/proc/mounts", "r");
	if (!mounts) {
		perror("setmntent");
		return -1;
	}

	/* Must umount before starting daemon */
	while ((mnt = getmntent(mounts)) != NULL) {
		if (strcmp(mnt->mnt_dir, OXBOW_PREFIX) == 0) {
			endmntent(mounts);
			return 1;
		}
	}

	endmntent(mounts);
	return 0;
}

static int exit_switch = 0;
static int drop_switch = 0;

void handle_sigint(int sig __attribute__((unused)))
{
	exit_switch = 1;
}

// #define SIG_DROP_CACHE (SIGRTMIN + 4)

// void handle_sigdropcache(int sig)
// {
// 	drop_switch = 1;
// }

void start_secure_daemon(void)
{
	if (is_oxbow_mounted()) {
		oxb_error("umount before starting new oxbow daemon");
		return;
	}

	// FIXME: Implement graceful shutdown on SIGINT.
	/* register for exiting secure daemon */
	if (signal(SIGINT, handle_sigint) == SIG_ERR) {
		perror("Can't catch SIGINT");
		return;
	};

	// if (signal(SIG_DROP_CACHE, handle_sigdropcache) == SIG_ERR) {
	// 	perror("Can't catch SIG_DROP_CACHE");
	// 	return;
	// };

	int ret;

	oxb_info("Start Oxbow Secure Daemon.");
	oxb_info("[IPC_MODE] secure_daemon build mode=%s",
		 OXBOW_DAEMON_IPC_MODE_STR);

	ret = init_secure_daemon();
	if (ret < 0) {
		log_error("Failed to initialize Secure Daemon.");
		goto err1;
	}

	ret = init_io_dispatcher();
	if (ret < 0) {
		log_error("Failed to initialize IO Dispatcher.");
		goto err2;
	}

	ret = init_msg();
	if (ret < 0) {
		log_error("Failed to initialize MSG module.");
		goto err3;
	}

	/* metadata shared with libfs process */
	ret = init_shm_system();
	if (ret < 0) {
		log_error("Failed to initialize shared memory system.");
		goto err4;
	}

	// Initialize the dirty page management
	ret = init_dirty_mgmt();
	if (ret) {
		log_error("Failed to initialize dirty manager.");
		goto err5;
	}

	ret = init_filesystem(g_sd_conf.filesystem);
	if (ret < 0) {
		log_error("Failed to initialize filesystem.");
		goto err6;
	}

	/* Adjust Illufs Readahead / IO Window */
	// NOTE: Kernel only issues 32-page-readahead at a time.
	// The remaining pages are prefetched by user-level readahead in secure_daemon.
	//
	// Default (no option): ra_pages=32, io_pages=128
	// 2 x default: ra_pages=64, io_pages=256
	// 4 x default: ra_pages=128, io_pages=512 (current — larger RA window so
	//              each kernel RA event produces more BIOs per round; helps
	//              D-2 read_workers fill per-qpair queue depth via
	//              per-epoll-round batching).
	//
	ret = mount("dummy", OXBOW_PREFIX, "illufs", 0, "");
	// ret = mount("dummy", OXBOW_PREFIX, "illufs", 0, "ra_pages=64,io_pages=256");
	// ret = mount("dummy", OXBOW_PREFIX, "illufs", 0, "ra_pages=128,io_pages=512");
	if (ret < 0) {
		perror("mount");
		log_error("Failed to mount illufs.");
		goto err7;
	}
	oxb_info("Mounting Oxbow filesystem at %s", OXBOW_PREFIX);

	if (init_fs_manager()) {
		log_error("init_fs_manager failed.");
		goto err6;
	}

	if (start_file_dispatcher() < 0) {
		log_error("start_file_dispatcher failed.");
		goto err6;
	}

	if (init_root_worker(g_super_block) < 0) {
		log_error("init_root_worker failed.");
		goto err6;
	}

	EXP_FLAG_WRITE(FLAG_FILE_DAEMON_STATUS, "1");

	usleep(50000);

	oxb_info("Secure Daemon is running\n"
		 "\t\t\t\t[Exit] ==> Press Ctrl+C (or SIGINT)");

	success_starting();

	while (1) {
		/* When secure daemon get do something?
			1. libfs do open : send msg to make shared memory.
				=> get fd or file name, make IO thread.
		*/
		sleep(1);
		if (exit_switch) {
			oxb_info("Exit secure daemon");
			break;
		}
		if (drop_switch) {
			oxb_info("Drop cache Not implemented yet)");
			drop_switch = 0;
		}
	}
	LOG_GLOBAL_NVME_STATS();
	nvme_ra_stats_dump();
	LOG_GLOBAL_WORKER_STATS();
	mpage_ra_stats_dump();
	log_shm_table();

	exit_manager();
	exit_file_workers();
err7:
	exit_filesystem();
err6:
	exit_shm_system();
err5:
	exit_dirty_mgmt();
err4:
	exit_msg();
err3:
	exit_io_dispatcher();
err2:
	exit_secure_daemon();
err1:
	EXP_FLAG_WRITE(FLAG_FILE_DAEMON_STATUS, "0");

	return;
}
