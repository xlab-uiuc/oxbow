#ifndef _MSG_OP_H_
#define _MSG_OP_H_
#include "oxbow.h"
#include <sched.h>
#include <stdint.h>
#include "oxbow_debug.h"
#include <sys/types.h>

#define msg_req(format, ...)                                                   \
	msg_info(ANSI_COLOR_RED                                                \
		 "[MSG_REQ] " format ANSI_COLOR_RESET __VA_OPT__(, )           \
			 __VA_ARGS__)
#define msg_resp(format, ...)                                                  \
	msg_info(ANSI_COLOR_BLUE                                               \
		 "[MSG_RESP] " format ANSI_COLOR_RESET __VA_OPT__(, )          \
			 __VA_ARGS__)

/*
 * To add a new msg operation,
 * 1) Add MSG_XX_YYY to `enum msg_op`. XX: Target, YY: Operation
 * 2) Add `struct msg_data_XX_YYY`.
 * 3) Add `struct msg_data_XX_YYY XX_YYY` to `msg_data`.
 */

enum msg_op {
	MSG_INIT = 1,
	MSG_EXIT,
	MSG_FSYNC,
	// MSG_GETSHM,
	// MSG_INO_FSYNC,
	MSG_SYNC,
	// MSG_FSTAT,
	// MSG_INO_FSTAT,
	MSG_FALLOC,
	MSG_FTRUNC,
	MSG_ADD_JOURNAL,
	MSG_BG_JOURNAL,
	MSG_FETCH_DATA,
	MSG_STG_CKPT,
	// Add here...
};

enum msg_rc {
	MSG_RC_ERR = 0,
	MSG_RC_OK = 1,
} __attribute__((packed));

struct msg_data_devfs_journal {
	int df_id;
	int md_buf_id;
	uint64_t md_size; // meta data (desc and tags blks) size in byte.
	uint64_t total_data_size;
	int n_fetch_reqs; // Total FETCH_DATA requests sent previously (not including this one).
	char *tx_p; // Secure daemon's tx pointer. Need to be freed by Secure Daemon at callback.
	uint32_t tx_id; // To print.
} __attribute__((packed));

struct msg_data_devfs_fetch_data {
	int df_id;
	int req_id;
	int buf_id;
	uint64_t size;
} __attribute__((packed));

struct msg_data_sd_journal_resp {
	int df_id;
	uint32_t tx_id; // tx_id of the completed commit.
	int md_buf_id;
	char *tx_p; // Secure daemon's tx pointer. Need to be freed by Secure Daemon at callback.
} __attribute__((packed));

struct msg_data_sd_init_proc_resp {
	char key[12];
	int index;
} __attribute__((packed));
struct msg_data_sd_fetch_data_resp {
	int df_id;
	int buf_id;
} __attribute__((packed));

// struct msg_data_sd_getshm_resp {
// 	int daemon_fd;
// 	int *ret;
// } __attribute__((packed));

struct msg_data_sd_fstat_resp {
	uint64_t st_ino;
	uint64_t st_nlink; // hard link nr
	uint32_t st_mode;
	uint32_t st_uid;
	uint32_t st_gid;
	// dev_t st_rdev;
	int64_t st_size; // in bytes
	// time_t st_atime;
	// time_t st_mtime;
	// time_t st_ctime;
	int32_t st_blksize; // file system specific block size
	int32_t st_blocks; // allocated block nr
	char *stat_buf; // Client's stat buffer.
} __attribute__((packed));

/* Secure Daemon -> DevFS: request ckpt on the stage area. */
struct msg_data_devfs_stg_ckpt {
	uint32_t stage_total_blks; /* Total number of blocks in the stage area. */
	uint32_t nr_blks_to_free; /* Desired number of blocks to free. */
	uint8_t is_sync; /* 1: synchronous, 0: asynchronous. */
} __attribute__((packed));

/* DevFS -> Secure Daemon: ckpt response. */
struct msg_data_sd_stg_ckpt_resp {
	uint32_t nr_blks_freed; /* Actual number of blocks freed. */
} __attribute__((packed));

// Add more...

/** The size of `struct msg_data` should be less than `msgdata_size` of the RPC channel. */
struct msg_data {
	enum msg_op op; /* common */
	int rc; /* daemon send */
	union {
		/* libfs send reqeust to daemon */
		struct {
			pid_t pid;
		} init;
		struct {
			void *daemon_inode_va;
		} fsync;
		struct {
			int pid;
			int fd;
			void *ret;
			void *daemon_inode_va;
		} getshm;
		// struct {
		// 	unsigned long ino;
		// } ino_fsync;
		struct {
			void *daemon_inode_va;
		} add_journal;
		// struct {
		// 	void *daemon_inode_va;
		// 	char *stat_buf; // Client's stat buffer.
		// } fstat;
		// struct {
		// 	unsigned long ino;
		// 	char *stat_buf; // Client's stat buffer.
		// } ino_fstat;
		struct {
			void *daemon_inode_va;
		} fallocate;
		struct {
			void *daemon_inode_va;
		} ftrucate;

		/* secure daemon send response message to libfs */
		struct {
			struct msg_data_sd_init_proc_resp sd_init_resp;
		};
		struct {
			struct msg_data_sd_fstat_resp sd_fstat_resp;
		};

		/* Secure daemon send request to DevFS */
		struct {
			struct msg_data_devfs_journal devfs_journal;
		};

		struct {
			struct msg_data_devfs_fetch_data devfs_fetch_data;
		};

		/* DevFS send response message to Secure Daemon */
		struct {
			struct msg_data_sd_journal_resp sd_journal_resp;
		};

		struct {
			struct msg_data_sd_fetch_data_resp sd_fetch_data_resp;
		};

		/* Secure daemon send ckpt request to DevFS */
		struct {
			struct msg_data_devfs_stg_ckpt devfs_stg_ckpt;
		};

		/* DevFS send ckpt response to Secure Daemon */
		struct {
			struct msg_data_sd_stg_ckpt_resp sd_stg_ckpt_resp;
		};

		// struct {
		// 	struct msg_data_sd_getshm_resp sd_getshm_resp;
		// };

		// Add here...
	} __attribute__((packed));
} __attribute__((packed)) __attribute__((aligned(8)));

static inline void print_req_msg_data(struct msg_data *md)
{
	switch (md->op) {
	case MSG_FSYNC:
		// case MSG_INO_FSYNC:
		// 	// msg_req("SD_FSYNC 0x%lx", md->fsync.daemon_inode_va);
		break;
		// case MSG_GETSHM:
		// 	break;

	case MSG_ADD_JOURNAL:
		msg_req("SD_ADD_JOURNAL 0x%lx", md->add_journal.daemon_inode_va);
		break;

	case MSG_BG_JOURNAL:
		msg_req("DEVFS_JOURNAL df_id=%d md_buf_id=%d md_size=%lu total_data_size=%lu n_fetch_req=%d tx_p=0x%lx tx_id=%u",
			md->devfs_journal.df_id, md->devfs_journal.md_buf_id,
			md->devfs_journal.md_size,
			md->devfs_journal.total_data_size,
			md->devfs_journal.n_fetch_reqs, md->devfs_journal.tx_p,
			md->devfs_journal.tx_id);
		break;

	case MSG_FETCH_DATA:
		msg_req("DEVFS_FETCH_DATA df_id=%d req_id=%d buf_id=%d size=%lu",
			md->devfs_fetch_data.df_id, md->devfs_fetch_data.req_id,
			md->devfs_fetch_data.buf_id, md->devfs_fetch_data.size);
		break;
	case MSG_INIT:
		msg_req("MSG_INIT pid=%d", md->init.pid);
		break;

	case MSG_EXIT:
		msg_req("MSG_EXIT");
		break;

	case MSG_SYNC:
		msg_req("MSG_SYNC");
		break;

		// case MSG_FSTAT:
		// 	msg_req("MSG_FSTAT");
		// 	break;

		// case MSG_INO_FSTAT:
		// 	msg_req("MSG_INO_FSTAT");
		// 	break;

	case MSG_FALLOC:
		msg_req("MSG_FALLOC");
		break;

	case MSG_FTRUNC:
		msg_req("MSG_FTRUNC");
		break;

	case MSG_STG_CKPT:
		msg_req("DEVFS_STG_CKPT total_blks=%u nr_blks_to_free=%u is_sync=%u",
			md->devfs_stg_ckpt.stage_total_blks,
			md->devfs_stg_ckpt.nr_blks_to_free,
			md->devfs_stg_ckpt.is_sync);
		break;

	default:
		msg_error("unknown case %d", md->op);
		break;
	}
}

static inline void print_resp_msg_data(struct msg_data *md)
{
	switch (md->op) {
	case MSG_INIT:
		msg_resp("LIBFS_INIT_RESP rc=%d key=%s index=%d", md->rc,
			 md->sd_init_resp.key, md->sd_init_resp.index);
		break;
	case MSG_FSYNC:
		msg_resp("LIBFS_FSYNC_RESP rc=%d", md->rc);
		break;

	case MSG_ADD_JOURNAL:
		msg_resp("LIBFS_ADD_JOURNAL_RESP rc=%d", md->rc);
		break;

	case MSG_BG_JOURNAL:
		msg_resp("SD_JOURNAL_RESP tx_id=%u md_buf_id=%d tx_p=0x%lx",
			 md->sd_journal_resp.tx_id,
			 md->sd_journal_resp.md_buf_id,
			 md->sd_journal_resp.tx_p);
		break;

	case MSG_FETCH_DATA:
		msg_resp("SD_FETCH_DATA_RESP buf_id=%d",
			 md->sd_fetch_data_resp.buf_id);
		break;
	case MSG_EXIT:
		msg_resp("MSG_EXIT_RESP rc=%d", md->rc);
		break;

	case MSG_SYNC:
		msg_resp("MSG_SYNC_RESP rc=%d", md->rc);
		break;

		// case MSG_FSTAT:
		// 	msg_resp("MSG_FSTAT_RESP rc=%d", md->rc);
		// 	break;

	case MSG_FALLOC:
		msg_resp("MSG_FALLOC_RESP rc=%d", md->rc);
		break;

	case MSG_FTRUNC:
		msg_resp("MSG_FTRUNC_RESP rc=%d", md->rc);
		break;

	case MSG_STG_CKPT:
		msg_resp("SD_STG_CKPT_RESP nr_blks_freed=%u",
			 md->sd_stg_ckpt_resp.nr_blks_freed);
		break;

	default:
		msg_error("unknown case %d", md->op);
		break;
	}
}
#endif
