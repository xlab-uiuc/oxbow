#include "oxbow_debug.h"
#include <stdio.h>
#include <pthread.h>
#include "msg_op.h"
#include "fs.h"
#include "msg.h"
// #include "oxbow_libfs.h"
#include "msg_handler.h"
#include "config.h"
#include <signal.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <sys/fcntl.h>
#include "profile_libfs.h"
#include "common/dirty_mgmt.h"
// Secure Daemon CM socket path.
#define RPC_SHMEM_SERVER_CM_PATH oxbow_secure_daemon_shmem_cm

// Client of RPC SHMEM channel to Secure Daemon.
struct rpc_ch_info *g_rpc_sd_client = NULL;

// Profiling events.
PF_TL_EVT(a_evt_write);
PF_TL_EVT(aa_before_sh_inode_lock_wait);
PF_TL_EVT(aa_sh_inode_lock_wait);
PF_TL_EVT(ab_journal_start);
PF_TL_EVT(aba_msg_send_add_jnl);
PF_TL_EVT(ac_page_lock_bit);
PF_TL_EVT(aca_page_lock_wait);
PF_TL_EVT(ad_write_memcpy);
PF_TL_EVT(ae_journal_handle_end);
PF_TL_EVT(b_evt_fsync);
PF_TL_EVT(b_evt_fsync_shm);
PF_TL_EVT(b_evt_fsync_msg);

PF_TL_EVT(shm_connection);
PF_TL_EVT(shma_loop);
PF_TL_EVT(shmb_open);
PF_TL_EVT(shmc_mmap);

// PF_TL_EVT(close_a);
// PF_TL_EVT(close_aa_munmap);
// PF_TL_EVT(close_ab_sysclose);

PF_EVT(libfs_write_memcpy);

static void print_stat_info(const struct stat *stat_buf)
{
	if (!stat_buf) {
		printf("stat_buf is NULL\n");
		return;
	}

	printf("Inode Number (st_ino): %ld\n", stat_buf->st_ino);
	printf("Number of Hard Links (st_nlink): %ld\n", stat_buf->st_nlink);
	printf("File Mode (st_mode): %o\n", stat_buf->st_mode);
	printf("User ID of Owner (st_uid): %d\n", stat_buf->st_uid);
	printf("Group ID of Owner (st_gid): %d\n", stat_buf->st_gid);
	printf("Total Size (st_size): %ld bytes\n", stat_buf->st_size);
	printf("Block Size (st_blksize): %ld bytes\n", stat_buf->st_blksize);
	printf("Number of Blocks Allocated (st_blocks): %ld\n",
	       stat_buf->st_blocks);
}

// Called in the requester thread.
void rpc_shmem_secure_daemon_handler(void *arg)
{
	struct rpc_msg *msg;
	struct msg_data *msg_body;

	msg = (struct rpc_msg *)arg;

	l_debug("(RPC shmem client handler) received: seqn=%lu data=%s",
		msg->header.seqn, msg->data);

	msg_body = (struct msg_data *)msg->data;

	switch (msg_body->op) {
	case MSG_INIT:
		if (msg_body->rc) {
			void *ptr;
			int shm_fd;

			l_debug("LIBFS init notified to daemon");
			shm_fd = shm_open(msg_body->sd_init_resp.key, O_RDWR,
					  0666);
			if (shm_fd < 0) {
				perror("failed to shm_open");
				goto exit;
			}
			ptr = mmap(0,
				   sizeof(struct shm_dfd) * OXBOW_MAX_OPEN_FILE,
				   PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd,
				   0);
			if (ptr == MAP_FAILED) {
				perror("failed to get shm_mmap");
				close(shm_fd);
				goto exit;
			}
			// register to global key_mmap
			init_libfs_fdt_key_map(ptr,
					       msg_body->sd_init_resp.index);
		} else {
		exit:
			oxb_error("LIBFS init failed, you may mount first");
			exit(-1);
		}
		break;
	case MSG_EXIT:
		l_debug("LIBFS exit given to daemon");
		break;

	case MSG_FSYNC:
		if (msg_body->rc) {
			l_debug("FSYNC succeeded.");
		} else {
			log_error("FSYNC failed in Secure Daemon.");
			// TODO: handle error case. (E.g., request again.)
		}
		break;

		// case MSG_GETSHM:
		// 	if (msg_body->rc) {
		// 		l_debug("MSG_GETSHM succeeded.");
		// 		*(msg_body->sd_getshm_resp.ret) =
		// 			msg_body->sd_getshm_resp.daemon_fd;
		// 	} else {
		// 		log_error("FSYNC failed in Secure Daemon.");
		// 		// TODO: handle error case. (E.g., request again.)
		// 	}
		// 	break;

		// case MSG_INO_FSYNC:
		// 	if (msg_body->rc) {
		// 		l_debug("(INO)FSYNC succeeded.");
		// 	} else {
		// 		log_error("FSYNC failed in Secure Daemon.");
		// 		// TODO: handle error case. (E.g., request again.)
		// 	}
		// 	break;

	case MSG_SYNC:
		if (msg_body->rc) {
			l_debug("SYNC succeeded.");
		} else {
			log_error("SYNC failed in Secure Daemon.");
			// TODO: handle error case. (E.g., request again.)
		}
		break;

	case MSG_ADD_JOURNAL: {
		if (msg_body->rc) {
			l_debug("ADD_JOURNAL succeeded.");
		} else {
			log_error("ADD_JOURNAL failed in Secure Daemon.");
			// TODO: handle error case. (E.g., request again.)
		}
		break;
	}

		// case MSG_FSTAT:
		// 	if (msg_body->rc) {
		// 		struct stat *stat_buf =
		// 			(struct stat *)msg_body->sd_fstat_resp.stat_buf;
		// 		msg_debug("FSTAT succeeded. %p", stat_buf);
		// 		stat_buf->st_ino = msg_body->sd_fstat_resp.st_ino;
		// 		stat_buf->st_nlink = msg_body->sd_fstat_resp.st_nlink;
		// 		stat_buf->st_mode = msg_body->sd_fstat_resp.st_mode;
		// 		stat_buf->st_uid = msg_body->sd_fstat_resp.st_uid;
		// 		stat_buf->st_gid = msg_body->sd_fstat_resp.st_gid;
		// 		stat_buf->st_size = msg_body->sd_fstat_resp.st_size;
		// 		stat_buf->st_blksize =
		// 			msg_body->sd_fstat_resp.st_blksize;
		// 		stat_buf->st_blocks = msg_body->sd_fstat_resp.st_blocks;
		// 		// print_stat_info(stat_buf);
		// 	} else {
		// 		log_error("FSTAT failed in Secure Daemon.");
		// 		// TODO: handle error case. (E.g., request again.)
		// 	}
		// 	break;

		// case MSG_INO_FSTAT:
		// 	if (msg_body->rc) {
		// 		struct stat *stat_buf =
		// 			(struct stat *)msg_body->sd_fstat_resp.stat_buf;
		// 		stat_buf->st_ino = msg_body->sd_fstat_resp.st_ino;
		// 		stat_buf->st_nlink = msg_body->sd_fstat_resp.st_nlink;
		// 		stat_buf->st_mode = msg_body->sd_fstat_resp.st_mode;
		// 		stat_buf->st_uid = msg_body->sd_fstat_resp.st_uid;
		// 		stat_buf->st_gid = msg_body->sd_fstat_resp.st_gid;
		// 		stat_buf->st_size = msg_body->sd_fstat_resp.st_size;
		// 		stat_buf->st_blksize =
		// 			msg_body->sd_fstat_resp.st_blksize;
		// 		stat_buf->st_blocks = msg_body->sd_fstat_resp.st_blocks;
		// 		// print_stat_info(stat_buf);
		// 		l_debug("INO_FSTAT succeeded.");
		// 	} else {
		// 		log_error("INO_FSTAT failed in Secure Daemon.");
		// 		// TODO: handle error case. (E.g., request again.)
		// 	}
		// 	break;

	default:
		log_error("Unknown message operation: %d", msg_body->op);
	}
}

int init_rpc(void)
{
	// int max_msgdata_size;
	l_debug("Initializing RPC client.");

	g_rpc_sd_client = init_rpc_client(
		RPC_CH_SHMEM, "/tmp/" XSTR(RPC_SHMEM_CM_PATH_LIBFS_DAEMON), 0,
		sizeof(struct msg_data), rpc_shmem_secure_daemon_handler, NULL,
		RPC_SHMEM_SEED_LIBFS_DAEMON);
	if (g_rpc_sd_client == NULL) {
		printf("init_rpc_client failed.\n");
		return -1;
	}

	// Check whether msg size exceeds rpc library support.
	// (cf. RPC_MSG_BUF_SIZE of oxbow-rpc library)
	// max_msgdata_size = get_max_msgdata_size(g_rpc_sd_client);
	// libfs_info(
	// 	"MAX supported RPC msgdata_size is %d and sizeof(struct msg_data) is %lu",
	// 	max_msgdata_size, sizeof(struct msg_data));
	// oxbow_assert((int)sizeof(struct msg_data) <= max_msgdata_size);

	libfs_info("RPC client is connected to Secure Daemon.");

	return 0;
}

void destroy_rpc(struct rpc_ch_info *rpc_cli_ch)
{
	destroy_rpc_client(rpc_cli_ch);
}

void exit_libfs_signal(int sig)
{
	libfs_info("[%s] %d", __func__, sig);
	msg_send_sd_exit();
	libfs_info("[%s] done", __func__);

	// Terminate the process.
	raise(SIGTERM);
}

__attribute__((constructor)) void init_libfs(void)
{
	pid_t current_pid;

	current_pid = getpid();

	/* Skip the first init_libfs() call.
	 *
	 * For some reason, this function is called twice when the program
	 * starts. This is a workaround to avoid duplicate initialization.
	 *
	 * Additionally, initialization in the first call does not work (Don't
	 * know why yet.)
	 */
	// if (!getenv("LIBFS_SKIP_FIRST_INIT")) {
	// 	libfs_info("Skip the first init_libfs() call.");

	// 	// Mark as initialized in the environment variable.
	// 	setenv("LIBFS_SKIP_FIRST_INIT", "1", 1);
	// 	return;
	// }

	libfs_info("Initializing libfs. pid=%d", current_pid);

	load_libfs_configs();
	print_libfs_configs();

	if (init_rpc() < 0)
		return;

	if (init_msg() < 0)
		return;

	msg_send_sd_init();

	// minimal vfs like stuff...
	init_libfs_fdt();
	lfs_inode_hash_init();

	if (signal(SIGINT, exit_libfs_signal) == SIG_ERR) {
		perror("Can't catch SIGINT");
		return;
	};

	init_dirty_mgmt();

#ifdef OXBOW_PROFILE
	oxb_warn("Profiling enabled. Disable it to measure performance.");
	pf_init(PF_SIGNAL);
#endif

#ifdef OXBOW_TRACK_TPUT
	PF_RESET(libfs_write_memcpy);
#endif

	libfs_info("Initializing libfs completed.");
}

__attribute__((destructor)) void exit_libfs(void)
{
	// msg_send_sd_exit();

#ifdef OXBOW_PROFILE
	// Print stats before exit.
	pf_print_evt_lists();

	pf_exit();
#endif

	exit_dirty_mgmt_libfs();

	// destroy_libfs_inode_table();
	lfs_inode_hash_destory();

	libfs_info("[%s] bye!", __func__);
}
