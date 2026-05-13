#include "oxbow.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "config.h"
#include "rpc.h"
#include "msg.h"
#include "msg_op.h"
#include "msg_handler.h"
#include "profile_devfs.h"
#include "journal.h"

#define RPC_SHMEM_DEVFS_CM_PATH oxbow_devfs_shmem_cm

void handle_secure_daemon_msg(struct msg_data *msg_body,
			      struct msg_data *resp_data)
{
	int ret;
	uint32_t tx_id;

	switch (msg_body->op) {
	case MSG_FETCH_DATA:
		ret = handle_fetch_data(msg_body->devfs_fetch_data.df_id,
					msg_body->devfs_fetch_data.req_id,
					msg_body->devfs_fetch_data.buf_id,
					msg_body->devfs_fetch_data.size);

		// Fill response msg.
		resp_data->op = MSG_FETCH_DATA;
		resp_data->sd_fetch_data_resp.df_id =
			msg_body->devfs_fetch_data.df_id;
		resp_data->sd_fetch_data_resp.buf_id =
			msg_body->devfs_fetch_data.buf_id;

		if (ret < 0) {
			log_error(
				"Failed to handle FETCH_DATA. df_id=%d buf_id=%d",
				msg_body->devfs_fetch_data.df_id,
				msg_body->devfs_fetch_data.buf_id);
			resp_data->rc = MSG_RC_ERR; // error.
		} else
			resp_data->rc = MSG_RC_OK; // success.

		break;

	case MSG_BG_JOURNAL:
		ret = handle_journal(msg_body->devfs_journal.df_id,
				     msg_body->devfs_journal.md_buf_id,
				     msg_body->devfs_journal.md_size,
				     msg_body->devfs_journal.total_data_size,
				     msg_body->devfs_journal.n_fetch_reqs,
				     &tx_id);

		// Fill response msg.
		resp_data->op = MSG_BG_JOURNAL;
		resp_data->sd_journal_resp.df_id =
			msg_body->devfs_journal.df_id;
		resp_data->sd_journal_resp.tx_id = tx_id;
		resp_data->sd_journal_resp.tx_p = msg_body->devfs_journal.tx_p;

		if (ret < 0) {
			log_error("Failed to handle journal. tx_id=%u", tx_id);
			resp_data->rc = MSG_RC_ERR; // error.
		} else {
			resp_data->rc = MSG_RC_OK; // success.
			resp_data->sd_journal_resp.md_buf_id =
				msg_body->devfs_journal.md_buf_id;
		}

		break;

	case MSG_STG_CKPT: {
		uint32_t nr_blks_freed = 0;

		ret = handle_stg_ckpt(
			msg_body->devfs_stg_ckpt.stage_total_blks,
			msg_body->devfs_stg_ckpt.nr_blks_to_free,
			msg_body->devfs_stg_ckpt.is_sync,
			&nr_blks_freed);

		resp_data->op = MSG_STG_CKPT;
		resp_data->sd_stg_ckpt_resp.nr_blks_freed = nr_blks_freed;

		if (ret < 0) {
			log_error("Failed to handle stage ckpt.");
			resp_data->rc = MSG_RC_ERR;
		} else {
			resp_data->rc = MSG_RC_OK;
		}

		break;
	}

	default:
		log_error("Unknown message operation: %d", msg_body->op);
	}
}

void rpc_rdma_server_handler(void *arg)
{
	struct msg_handler_param *param;
	struct rpc_msg *msg;
	struct rpc_ch_info rpc_ch = { 0 };
	struct rpc_resp_param resp_param;
	struct msg_data *msg_body;
	struct msg_data resp_data = { 0 };

	param = (struct msg_handler_param *)arg;
	msg = param->msg;

	oxb_debug(
		"[RPC_SERVER] received from Client %d: seqn=%lu sem_addr(rdma channel only)=%lx data=%s",
		param->client_id, msg->header.seqn, (uint64_t)msg->header.sem,
		msg->data);

	/* Handle message. */
	msg_body = (struct msg_data *)msg->data;
	print_req_msg_data(msg_body);

	handle_secure_daemon_msg(msg_body, &resp_data);

	print_resp_msg_data(&resp_data);

	// Fill response params.
	rpc_ch.ch_cb = param->ch_cb;
	rpc_ch.msgbuf_bitmap = NULL;

	// ch_cb is passed as a parameter because it is different for each client.
	rpc_ch.ch_type = RPC_CH_RDMA;

	resp_param = (struct rpc_resp_param){ .rpc_ch = &rpc_ch,
					      .client_rpc_ch_addr =
						      msg->header.client_rpc_ch,
					      .data = (char *)&resp_data,
					      .sem = (sem_t *)msg->header.sem,
					      .client_id = 0, // Not used.
					      .msgbuf_id = param->msgbuf_id,
					      .seqn = msg->header.seqn };

	/* Send reply to the client. */
	send_rpc_response_to_client(&resp_param);

	free(msg);
	free(param);
}

// When HOST_JOURNALING is set. (handle secure daemon's msg)
void rpc_shmem_secure_daemon_handler(void *arg)
{
	struct msg_handler_param *param;
	struct rpc_msg *msg;
	struct rpc_ch_info rpc_ch = { 0 };
	struct msg_data resp_data = { 0 };
	struct rpc_resp_param resp_param;
	struct msg_data *msg_body;

	param = (struct msg_handler_param *)arg;
	msg = param->msg;

	msg_body = (struct msg_data *)msg->data;

	msg_debug(
		"[RPC SHMEM SERVER] sender_cid(%d) seqn(%lu) sem_addr(%lx) op(%d)",
		param->client_id, msg->header.seqn, (uint64_t)msg->header.sem,
		msg_body->op);

	print_req_msg_data(msg_body);

	handle_secure_daemon_msg(msg_body, &resp_data);

	print_resp_msg_data(&resp_data);

	// Fill response params.
	rpc_ch.ch_cb = param->ch_cb;
	rpc_ch.msgbuf_bitmap = NULL;

	// ch_cb is passed as a parameter because it is different for each client.
	rpc_ch.ch_type = RPC_CH_SHMEM;

	resp_param = (struct rpc_resp_param){ .rpc_ch = &rpc_ch,
					      .client_rpc_ch_addr =
						      msg->header.client_rpc_ch,
					      .data = (char *)&resp_data,
					      .sem = NULL,
					      .client_id = param->client_id,
					      .msgbuf_id = param->msgbuf_id,
					      .seqn = msg->header.seqn };

	/* Send reply to the client. */
	send_rpc_response_to_client(&resp_param);

	free(msg);
	free(param);
}

void on_secure_daemon_connected(void *arg)
{
	reset_jnr_ctx();
}

void on_secure_daemon_disconnected(void *arg)
{
	// TOOD: Do checkpoint to update the recent state of superblock.
	// For now, just reset the indices when a client is re-connected for experiments.
}

int init_rpc(void)
{
	int ret;

#ifdef HOST_JOURNALING
	ret = init_rpc_server(RPC_CH_SHMEM,
			      "/tmp/" XSTR(RPC_SHMEM_CM_PATH_DAEMON_DEVFS), 0,
			      sizeof(struct msg_data),
			      rpc_shmem_secure_daemon_handler, worker_thpool,
			      on_secure_daemon_connected, NULL,
			      on_secure_daemon_disconnected, NULL,
			      RPC_SHMEM_SEED_DAEMON_DEVFS);
	oxb_info("RPC server (SHMEM) is running.");
#else
	ret = init_rpc_server(RPC_CH_RDMA, NULL, g_devfs_conf.rpc_rdma_port,
			      sizeof(struct msg_data), rpc_rdma_server_handler,
			      worker_thpool, on_secure_daemon_connected, NULL,
			      on_secure_daemon_disconnected, NULL, 0);

	oxb_info("RPC server (RDMA) is running.");
#endif
	if (ret < 0) {
		log_error("RPC server initialization failed.");
		return ret;
	}

	return 0;
}

int init_msg(void)
{
	int ret;

	ret = init_rpc();
	if (ret < 0) {
		log_error("Failed to initialize MSG module.");
		return -1;
	}

	return 0;
}

void exit_msg(void)
{
	// Do something.
}
