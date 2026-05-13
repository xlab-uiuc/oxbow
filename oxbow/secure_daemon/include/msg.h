#ifndef _MSG_H_
#define _MSG_H_
#include "msg_op.h"
#include "rpc.h"

// Client of RPC RDMA channel to Secure Daemon.
extern struct rpc_ch_info *g_rpc_devfs_client;

int init_msg(void);
void exit_msg(void);
void msg_send_libfs_resp(struct rpc_resp_param *, int pid);
void msg_send_libfs_basic_resp(struct rpc_resp_param *resp_param, int op,
			       int success);
void msg_send_libfs_fstat_resp(struct rpc_resp_param *resp_param,
			       struct msg_data *msg_body);
void msg_send_libfs_ino_fstat_resp(struct rpc_resp_param *resp_param,
				   struct msg_data *msg_body);
void msg_send_devfs_journal(int df_id, int md_buf_id, uint64_t md_size,
			   uint64_t total_data_size, int n_fetch_reqs, char *tx,
			   uint32_t tid);
void msg_send_devfs_fetch_data(int df_id, int req_id, int buf_id,
			       uint64_t size);
void msg_send_devfs_stg_ckpt(uint32_t stage_total_blks,
			     uint32_t nr_blks_to_free, uint8_t is_sync);
#endif
