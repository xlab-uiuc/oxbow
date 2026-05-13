#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <signal.h>
#include "devfs.h"
#include "config.h"
#include "common/config.h"
#include "oxbow.h"
#include "msg_op.h"
#include "msg.h"
#include "journal.h"
#include "profile_devfs.h"
#include "utils/sync_device.h"
#include "common/utils/exp_flag.h"

int terminate = 0;
static atomic_bool flag_ckpt = false;

// Profiling events.
PF_TL_EVT(evt_fetch_data);
PF_TL_EVT(evt_bg_journal);
PF_TL_EVT(evt_fetch_meta);
PF_TL_EVT(evt_commit);
PF_TL_EVT(evt_nvmf_dispatch_fast);
PF_TL_EVT(evt_nvmf_poll_fast);

void pf_print_stats(void)
{
	printf("Print profiling stats.\n");
	printf(",=============== PROFILE GLOBAL ===============,,,,\n");
	PF_PRINT_HDR();
	PF_TL_PRINT_STATS("", evt_bg_journal);
	PF_TL_PRINT_STATS("", evt_fetch_meta);
	PF_TL_PRINT_STATS("", evt_commit);
	PF_TL_PRINT_STATS("", evt_nvmf_dispatch_fast);
	PF_TL_PRINT_STATS("", evt_nvmf_poll_fast);
}

void pf_reset_stats(void)
{
	printf("Reset profiling stats.\n");

	PF_TL_RESET(evt_bg_journal);
	PF_TL_RESET(evt_fetch_meta);
	PF_TL_RESET(evt_commit);
	PF_TL_RESET(evt_nvmf_dispatch_fast);
	PF_TL_RESET(evt_nvmf_poll_fast);
}

void ckpt_signal_handler(int signo)
{
	UNUSED(signo);

	// Cehckpoint all.
	oxb_info("Signal received. Set ckpt flag.");
	atomic_store(&flag_ckpt, true);
}

void init_devfs(void)
{
	load_devfs_configs();
	print_devfs_configs();

	load_common_configs();
	print_common_configs();

	terminate = 0;

	// Register signal handler for checkpointing.
	struct sigaction sa;
	sa.sa_handler = ckpt_signal_handler;
	sigemptyset(&sa.sa_mask);
	sa.sa_flags = 0;
	sigaction(SIGRTMIN + 1, &sa, NULL);

#ifdef USE_NVME_STORAGE_ENGINE
	sync_device_init(); // SIGRTMIN + 2
#endif

#ifdef OXBOW_PROFILE
	oxb_warn("Profiling is enabled. Disable it to measure performance.");
	pf_init(PF_SIGNAL);
#endif
#ifdef OXBOW_TRACK_TPUT
	PF_RESET(COMMIT_DATA_IO);
#endif
}

void exit_devfs(void)
{
	printf("Exiting devfs.\n");

	exit_msg();
	exit_journal();
#ifdef OXBOW_PROFILE
	// pf_print_stats();
	// pf_print_tl_stats();
	pf_exit();
#endif

	printf("Bye.\n");
	exit(EXIT_SUCCESS);
}

int start_devfs(void)
{
	int ret;

	log_info("Start DevFS.");

	init_devfs();

	// Itnitializes storage engine first than msg module.
	// Storage engine's io worker is used for RPC handler.
	ret = init_journal();
	if (ret < 0)
		return -1;

	ret = init_msg();
	if (ret < 0)
		return -1;

	oxb_info("All initializations are done. DevFS is running.");
	printf("\n");
	printf("   ██████╗ ██╗  ██╗██████╗  ██████╗ ██╗    ██╗███████╗███████╗\n");
	printf("  ██╔═══██╗╚██╗██╔╝██╔══██╗██╔═══██╗██║    ██║██╔════╝██╔════╝\n");
	printf("  ██║   ██║ ╚███╔╝ ██████╔╝██║   ██║██║ █╗ ██║█████╗  ███████╗\n");
	printf("  ██║   ██║ ██╔██╗ ██╔══██╗██║   ██║██║███╗██║██╔══╝  ╚════██║\n");
	printf("  ╚██████╔╝██╔╝ ██╗██████╔╝╚██████╔╝╚███╔███╔╝██║     ███████║\n");
	printf("   ╚═════╝ ╚═╝  ╚═╝╚═════╝  ╚═════╝  ╚══╝╚══╝ ╚═╝     ╚══════╝\n");
	printf("┌───────────┐      ┌─────────────────────┐      ╔═══════════════╗\n");
	printf("│   LibFS   │<────>│    Secure Daemon    │<────>║ >>  DevFS <<  ║\n");
	printf("│           │      │                     │      ║   ACTIVATED   ║\n");
	printf("└───────────┘      └─────────────────────┘      ╚═══════════════╝\n");

	EXP_FLAG_WRITE(FLAG_FILE_DEVFS_STATUS, "1");

	// It checks the flags periodically and handles them.
	while (terminate == 0) {
		if (atomic_load(&flag_ckpt)) {
			start_checkpoint((void *)(intptr_t)100);
			atomic_store(&flag_ckpt, false);
			oxb_info("Clear ckpt flag.");
		}
		sleep(1);
	}

	exit_devfs();

	EXP_FLAG_WRITE(FLAG_FILE_DEVFS_STATUS, "0");

	return 0;
}
