#include "oxbow_debug.h"
#define _GNU_SOURCE
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include "msg.h"
#include "msg_op.h"
#include "fs.h"
#include "thpool.h"
#include "common/profile.h"
// #include "oxbow.h"
#include "time_stat.h"

struct free_th_arg {
	struct rpc_ch_info *rpc_cli_ch;
	int msgbuf_id;
};

threadpool msg_async_thpool = NULL;

int init_msg(void)
{
	msg_async_thpool = thpool_init(2, "msg_async");
	if (!msg_async_thpool)
		return -1;
	l_debug("init_msg done");
	return 0;
}

/* Send message helper functions */

/**
 * @brief Send sync message to Secure Daemon. It returns after receiving a
 * response from the server.
 * 
 * @param data 
 */
static void send_sync_msg_to_secure_daemon(char *data)
{
	int msgbuf_id;
	struct rpc_req_param req_param;
	// struct msg_data *msg_data = data;
	req_param = (struct rpc_req_param){ .rpc_ch = g_rpc_sd_client,
					    .data = data,
					    .sem = NULL };

	print_req_msg_data((struct msg_data *)data);

	msgbuf_id = send_rpc_msg_to_server(&req_param);

	// if (msg_data->op == MSG_FSYNC)
	// log_debug("Sending RPC message:%d op%d, %lx", msgbuf_id, msg_data->op,
	// 	  msg_data->fsync.daemon_inode_va);
	// Resume after response arrives and handler callback function is called.
	wait_rpc_shmem_response(g_rpc_sd_client, msgbuf_id, 1);

	// if (msg_data->op == MSG_FSYNC)
	// log_debug("Sending RPC message:%d  op%d, done :%lx", msgbuf_id,
	// 	  msg_data->op, msg_data->fsync.daemon_inode_va);
}

/**
 * @brief Send async message to Secure Daemon. You have to manually call
 * `wait_rpc_shmem_response(rpc_cli_ch, msgbuf_id)` to free the rpc resources.
 * 
 * @param data Message body.
 * @return int msgbuf_id
 */
static int send_async_msg_to_secure_daemon(char *data)
{
	int msgbuf_id;
	struct rpc_req_param req_param;

	req_param = (struct rpc_req_param){ .rpc_ch = g_rpc_sd_client,
					    .data = data,
					    .sem = NULL };

	l_debug("Sending RPC message:%s", data);
	msgbuf_id = send_rpc_msg_to_server(&req_param);

	return msgbuf_id;
}

/**
 * @brief There are always responses and they should be handled. A message
 * buffer is freed during this process.
 * 
 * @param arg 
 */
static void free_msgbuf(void *arg)
{
	struct free_th_arg *ft_arg;
	int ret;

	ft_arg = (struct free_th_arg *)arg;

	ret = trywait_rpc_shmem_response(ft_arg->rpc_cli_ch, ft_arg->msgbuf_id,
					 1);
	if (ret < 0) {
		// Reschedule it.
		thpool_add_work(msg_async_thpool, free_msgbuf, (void *)ft_arg);
		return;
	}

	// msgbuf freed.
	free(arg);
}

/**
 * @brief Send a message to Secure Daemon and do not care of response. Another
 * dummy thread wait for response by calling `wait_rpc_shmem_response()`.
 * 
 * @param data Message body.
 */
static void send_msg_to_secure_daemon_nowait(char *data)
{
	struct free_th_arg *ft_arg;
	struct rpc_req_param req_param;
	int msgbuf_id;

	req_param = (struct rpc_req_param){ .rpc_ch = g_rpc_sd_client,
					    .data = data,
					    .sem = NULL };

	print_req_msg_data((struct msg_data *)data);

	msgbuf_id = send_rpc_msg_to_server(&req_param);

	ft_arg = calloc(1, sizeof(struct free_th_arg));
	ft_arg->rpc_cli_ch = g_rpc_sd_client;
	ft_arg->msgbuf_id = msgbuf_id;

	// Free msgbuf in another thread.
	// TODO: The pending reads need to be resumed.
	thpool_add_work(msg_async_thpool, free_msgbuf, (void *)ft_arg);
}

/*** Functions to send a message ***/
/*
 * Function name format:  msg_send_<target>_<operation>()
 * Targets
 * sd : Secure Daemon
 */

// #define MSG_DATA(name, __op, __fd)
// 	struct msg_data name = { .op = __op, .pid = libfs_tgid, .fd = __fd }

void msg_send_sd_init(void)
{
	struct msg_data init_msg;

	init_msg.op = MSG_INIT;
	init_msg.init.pid = getpid();

	l_debug("[%s] pid(%d)", __func__, getpid());

	send_sync_msg_to_secure_daemon((char *)&init_msg);
}

void msg_send_sd_exit(void)
{
	// MSG_DATA(m, MSG_EXIT, 0);

	// l_debug("[%s] tgid(%d)", __func__, libfs_tgid);

	// send_sync_msg_to_secure_daemon((char *)&m);
}

int msg_send_sd_fsync(void *daemon_inode_va)
{
	struct msg_data fsync_msg;

	fsync_msg.op = MSG_FSYNC;
	fsync_msg.fsync.daemon_inode_va = daemon_inode_va;

	// log_debug("[tid: %d] daemon_inode_va(%p)", get_tid(), daemon_inode_va);

	send_sync_msg_to_secure_daemon((char *)&fsync_msg);

	// log_debug("done");
	// TODO: What happend if fsync failed?

	return 0;
}

// int msg_send_sd_getshm(struct lfs_file *f, int *ret)
// {
// 	struct msg_data msg;

// 	msg.op = MSG_GETSHM;
// 	msg.getshm.pid = getpid();
// 	msg.getshm.fd = f->fd;
// 	msg.getshm.ret = ret;

// 	// log_debug("[tid: %d] daemon_inode_va(%p)", get_tid(), daemon_inode_va);

// 	send_sync_msg_to_secure_daemon((char *)&msg);

// 	// log_debug("done");
// 	// TODO: What happend if fsync failed?

// 	return 0;
// }

// int msg_send_sd_ino_fsync(unsigned long ino)
// {
// 	struct msg_data fsync_msg;

// 	fsync_msg.op = MSG_INO_FSYNC;
// 	fsync_msg.ino_fsync.ino = ino;

// 	send_sync_msg_to_secure_daemon((char *)&fsync_msg);

// 	// TODO: What happend if fsync failed?

// 	return 0;
// }

int msg_send_sd_add_journal(void *daemon_inode_va)
{
	struct msg_data aj_msg;

	aj_msg.op = MSG_ADD_JOURNAL;
	aj_msg.add_journal.daemon_inode_va = daemon_inode_va;
	// aj_msg.add_journal.tx_id = tx_id;

	send_msg_to_secure_daemon_nowait((char *)&aj_msg);

	return 0;
}

int msg_send_sd_sync(void)
{
	// MSG_DATA(m, MSG_SYNC, 0);

	// l_debug("[%s] tgid(%d)", __func__, libfs_tgid);

	// send_sync_msg_to_secure_daemon((char *)&m);

	return 0;
}

// int msg_send_sd_fstat(void *daemon_inode_va, void *__stat_buf)
// {
// 	struct msg_data msg;

// 	msg.op = MSG_FSTAT;
// 	msg.fstat.daemon_inode_va = daemon_inode_va;
// 	msg.fstat.stat_buf = __stat_buf;

// 	msg_debug("[%s] inode_va(%p) stat_buf:%p send | (tid:%d)", __func__,
// 		  daemon_inode_va, __stat_buf, get_tid());

// 	send_sync_msg_to_secure_daemon((char *)&msg);

// 	if (msg.rc < 0) {
// 		log_error("FSTAT failed");
// 		return -1;
// 	}

// 	return 0;
// }

// int msg_send_sd_ino_fstat(unsigned long ino, void *__stat_buf)
// {
// 	struct msg_data msg;

// 	msg.op = MSG_INO_FSTAT;
// 	msg.ino_fstat.ino = ino;
// 	msg.ino_fstat.stat_buf = __stat_buf;

// 	msg_debug("[%s] ino(%lu) | (tid:%d)", __func__, ino, get_tid());

// 	send_sync_msg_to_secure_daemon((char *)&msg);

// 	if (msg.rc < 0) {
// 		log_error("INO_FSTAT failed");
// 		return -1;
// 	}

// 	msg_debug("[%s] ino(%lu) done | (tid:%d)", __func__, ino, get_tid());

// 	return 0;
// }

int msg_send_sd_fallocate(void *daemon_inode_va)
{
	struct msg_data msg;
	int ret;

	msg.op = MSG_FALLOC;
	msg.fallocate.daemon_inode_va = daemon_inode_va;

	posix_debug("[tid: %d] daemon_inode_va(%p)", get_tid(),
		    daemon_inode_va);

	send_sync_msg_to_secure_daemon((char *)&msg);

	ret = msg.rc;
	return ret;
}

int msg_send_sd_ftruncate(void *daemon_inode_va)
{
	struct msg_data msg;
	int ret;

	msg.op = MSG_FTRUNC;
	msg.ftrucate.daemon_inode_va = daemon_inode_va;

	posix_debug("[tid: %d] daemon_inode_va(%p)", get_tid(),
		    daemon_inode_va);

	send_sync_msg_to_secure_daemon((char *)&msg);

	ret = msg.rc;
	return ret;
}
