#include "fs.h"
#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <stdio.h>
#include <fcntl.h>
#include "msg_op.h"
#include "rpc.h"
#include "global.h"
#include "config.h"
#include "data_fetcher.h"
#include "fs/journal.h"
#include "thpool.h"
#include "msg.h"
#include "libfs/msg_handler.h"
#include "io_dispatcher.h"
#include "io/nvme.h"
#include "oxbow_debug.h"
#include "profile_secure_daemon.h"
#include "utils/jlock_profile.h"

// To avoid deadlock and consequent requeuing of fsync request.
#define FSYNC_BOUNCE

/*
 * Maximum number of bounced fsync requests released per drain call.
 *
 * Run 4 (§8.4 in plans/2026-04-20-scalability-bottleneck-analysis.md)
 * showed that releasing the full idle headroom in one shot creates a
 * thundering-herd race for `j->lock` (§3.4) and regresses 64-app
 * throughput by 5–16% on write workloads. Capping the burst limits the
 * number of fsyncs that can collide on `j->lock` simultaneously.
 *
 * Tuning knob: 1 reproduces the original drain-1 behavior (no convoy
 * but resurrects the §8.2.1 10-app dip). 2–3 keeps the dip mostly
 * resolved while keeping the convoy small. Setting it >= pool size
 * recovers the full drain-to-capacity behavior of Run 4.
 */
#define FSYNC_BOUNCE_DRAIN_CAP 1

struct free_th_arg {
	struct rpc_ch_info *rpc_cli_ch;
	int msgbuf_id;
};

#define MSG_DATA(name, __op, __rc)                                             \
	struct msg_data name = { .op = __op, .rc = __rc }

// Client of RPC to DevFS.
struct rpc_ch_info *g_rpc_devfs_client = NULL;

threadpool msg_async_thpool;

threadpool rpc_libfs_handler_thpool; // SHMEM
threadpool rpc_devfs_handler_thpool; // RDMA

/* Forward declaration for requeue */
void rpc_shmem_libfs_handler(void *arg);

#ifdef FSYNC_BOUNCE
/* =================== FSYNC BOUNCE QUEUE (delayed requeue) =================== */
struct fsync_bounce_node {
	void *arg;
	struct fsync_bounce_node *next;
};
static struct fsync_bounce_node *g_fsync_bounce_head;
static struct fsync_bounce_node *g_fsync_bounce_tail;
static pthread_spinlock_t g_fsync_bounce_lock;

static inline void fsync_bounce_enqueue(void *arg)
{
	struct fsync_bounce_node *n =
		(struct fsync_bounce_node *)malloc(sizeof(*n));
	if (!n) {
		oxb_error("failed to allocate fsync_bounce_node");

		/* Allocation failed; fallback to immediate requeue to avoid request loss. */
		thpool_add_work(rpc_libfs_handler_thpool,
				rpc_shmem_libfs_handler, arg);
		return;
	}
	n->arg = arg;
	n->next = NULL;

	pthread_spin_lock(&g_fsync_bounce_lock);
	if (g_fsync_bounce_tail)
		g_fsync_bounce_tail->next = n;
	else
		g_fsync_bounce_head = n;
	g_fsync_bounce_tail = n;
	pthread_spin_unlock(&g_fsync_bounce_lock);
}

static inline void *fsync_bounce_try_dequeue(void)
{
	void *arg = NULL;
	pthread_spin_lock(&g_fsync_bounce_lock);
	if (g_fsync_bounce_head) {
		struct fsync_bounce_node *n = g_fsync_bounce_head;
		g_fsync_bounce_head = n->next;
		if (!g_fsync_bounce_head)
			g_fsync_bounce_tail = NULL;
		arg = n->arg;
		free(n);
	}
	pthread_spin_unlock(&g_fsync_bounce_lock);
	return arg;
}

/*
 * Drain bounced fsync requests into idle worker slots, bounded by both:
 *   (1) idle worker count (`alive - working`, stale snapshot)
 *   (2) FSYNC_BOUNCE_DRAIN_CAP (anti-convoy cap; see Run 4 / §8.4)
 *
 * Called at the tail of every message handler, so the calling worker is
 * still counted in thpool's `working`; `idle = alive - working` therefore
 * excludes self and self picks the next jobqueue entry after we return.
 *
 * thpool counters are lock-free volatile reads, so each snapshot is
 * inherently stale. Re-sampling per iteration keeps us in step with
 * workers we just woke (their `working++` lands before our next read)
 * and with concurrent drainers (other handlers finishing at the same
 * instant), bounding any over-dispatch to O(concurrent_drainers) extra
 * jobqueue entries — harmless since jobqueue is unbounded.
 *
 * The FSYNC_BOUNCE_DRAIN_CAP bound is the anti-convoy fix from Run 5:
 * cap=1 reproduces the original drain-1 behavior (no convoy but the
 * §8.2.1 10-app dip returns); uncapped (cap=infinity) is Run 4's
 * drain-to-capacity, which exposes §3.4 `j->lock` thundering herd and
 * regressed 64-app throughput by 5–16%. A small cap keeps the dip-fix
 * while bounding the burst that hits `j->lock`.
 */
static inline void fsync_bounce_drain_to_capacity(void)
{
	int dispatched = 0;

	while (dispatched < FSYNC_BOUNCE_DRAIN_CAP) {
		int alive = thpool_num_threads_alive(rpc_libfs_handler_thpool);
		int working =
			thpool_num_threads_working(rpc_libfs_handler_thpool);

		if (alive <= 1 || working >= alive)
			break;

		void *barg = fsync_bounce_try_dequeue();
		if (!barg)
			break;

		thpool_add_work(rpc_libfs_handler_thpool,
				rpc_shmem_libfs_handler, barg);
		dispatched++;
	}
}
#else
static inline void fsync_bounce_drain_to_capacity(void)
{
}
#endif

void handle_devfs_msg(struct msg_data *msg_body)
{
	switch (msg_body->op) {
	case MSG_BG_JOURNAL:

		// Free the Data Fetcher buffer (metadata)
		df_free_buffer(g_df_ctxs[msg_body->sd_journal_resp.df_id],
			       msg_body->sd_journal_resp.md_buf_id);

		// clear the waiting dirty list
		clear_waiting_dirty_lists((journal_tx *)msg_body->sd_journal_resp.tx_p);

		// Free tx and df_id.
		free_transaction((journal_tx *)msg_body->sd_journal_resp.tx_p);

		jlock_acquire(&g_journal_ctx->lock);

		/* check consistency? */
		jnl_debug("mrc_tx_id updated: %u -> %u",
			  g_journal_ctx->mrc_tx_id,
			  msg_body->sd_journal_resp.tx_id);

		g_journal_ctx->mrc_tx_id = msg_body->sd_journal_resp.tx_id;

		pthread_spin_unlock(&g_journal_ctx->lock);

		break;

	case MSG_FETCH_DATA:
		// Free the Data Fetcher buffer.
		df_free_buffer(g_df_ctxs[msg_body->sd_fetch_data_resp.df_id],
			       msg_body->sd_fetch_data_resp.buf_id);

		break;

	case MSG_STG_CKPT:
		// Reclaim stage area blocks freed by checkpoint.
		stage_free_blks(g_journal_ctx,
				msg_body->sd_stg_ckpt_resp.nr_blks_freed);
		atomic_store(&g_journal_ctx->stg_ckpt_in_flight, false);
		break;

	default:
		log_error("Unknown message operation: %d", msg_body->op);
	}
}

/*
 * RPC shared memory channel (server) is used to communicate with LibFSes.
 * RPC RDMA channel (client) is used to communicate with DevFS.
 */

// RPC RDMA client Handler. It is called in a worker thread.
void rpc_rdma_client_handler(void *arg)
{
	struct msg_handler_param *param;
	struct rpc_msg *msg;
	struct msg_data *msg_body;
	sem_t *sem;

	param = (struct msg_handler_param *)arg;
	msg = param->msg;

	msg_body = (struct msg_data *)msg->data;
	sem = (sem_t *)param->msg->header.sem;
	msg_debug("[MSG RDMA CLIENT] received: seqn=%lu sem_addr=%lx\n",
		  msg->header.seqn, (uint64_t)msg->header.sem);

	print_resp_msg_data(msg_body);

	// Do not allow any error for now.
	oxbow_assert(msg_body->rc == MSG_RC_OK);

	handle_devfs_msg(msg_body);

	// post sema to resume the requesting thread.
	if (sem)
		sem_post(sem);

	free(msg);
	free(param);
}

// RPC SHMEM client handler. It is used when running DevFS on the host.
// (HOST_JOURNALING is enabled.)
void rpc_shmem_devfs_handler(void *arg)
{
	struct rpc_msg *msg;
	struct msg_data *msg_body;

	msg = (struct rpc_msg *)arg;

	msg_debug("(RPC shmem client handler) received: seqn=%lu data=%s",
		  msg->header.seqn, msg->data);

	msg_body = (struct msg_data *)msg->data;

	print_resp_msg_data(msg_body);

	handle_devfs_msg(msg_body);
}

static void fill_rpc_resp_param(struct rpc_resp_param *resp,
				struct rpc_ch_info *rpc_ch, struct rpc_msg *msg,
				struct msg_handler_param *param)
{
	rpc_ch->ch_type = RPC_CH_SHMEM;
	resp->rpc_ch = rpc_ch;
	resp->client_rpc_ch_addr = msg->header.client_rpc_ch;
	resp->data = NULL;
	resp->sem = NULL;
	resp->client_id = param->client_id;
	resp->msgbuf_id = param->msgbuf_id;
	resp->seqn = msg->header.seqn;
}

/** Handle messages sent from LibFSes. */
void rpc_shmem_libfs_handler(void *arg)
{
	struct msg_handler_param *param;
	struct rpc_msg *msg;
	struct rpc_ch_info rpc_ch = { 0 };
	struct rpc_resp_param resp_param;
	struct msg_data *msg_body;
	int ret;
	int success;
	int alive;
	int working;

	param = (struct msg_handler_param *)arg;
	msg = param->msg;

	msg_body = (struct msg_data *)msg->data;

	// msg_debug(
	// 	"[RPC SHMEM SERVER] sender_cid(%d) seqn(%lu) sem_addr(%lx) op(%d)",
	// 	param->client_id, msg->header.seqn, (uint64_t)msg->header.sem,
	// 	msg_body->op);

	print_req_msg_data(msg_body);

	rpc_ch.ch_cb = param->ch_cb;
	rpc_ch.msgbuf_bitmap = NULL;

	switch (msg_body->op) {
	case MSG_INIT:
		msg_debug("[MSG_INIT]");

		fill_rpc_resp_param(&resp_param, &rpc_ch, msg, param);
		msg_send_libfs_resp(&resp_param, msg_body->init.pid);
		break;

	case MSG_EXIT:
		// TODO : do some free
		msg_debug("[MSG_EXIT]");
		success = 1;

		fill_rpc_resp_param(&resp_param, &rpc_ch, msg, param);
		msg_send_libfs_basic_resp(&resp_param, MSG_EXIT, success);
		/* After finishing a job, drain bounced fsyncs into all idle worker slots. */
		fsync_bounce_drain_to_capacity();
		break;

	case MSG_FSYNC:
#ifdef FSYNC_BOUNCE
		/* Bounce fsync only when there is no idle thread (working == alive).
		* Delayed requeue: enqueue into a bounce queue and return immediately. */
		alive = thpool_num_threads_alive(rpc_libfs_handler_thpool);
		working = thpool_num_threads_working(rpc_libfs_handler_thpool);
		if (alive > 1 && working == alive) {
			oxb_warn(
				"Bouncing fsync (idle=0) (alive=%d, working=%d)",
				alive, working);
			fsync_bounce_enqueue(arg);
			return;
		}
#endif

		msg_info("[MSG_FSYNC] %lx", msg_body->fsync.daemon_inode_va);

		PF_TL_START(a_evt_sd_fsync);

		ret = handle_fsync(msg_body);

		success = ret < 0 ? 0 : 1;

		fill_rpc_resp_param(&resp_param, &rpc_ch, msg, param);
		msg_send_libfs_basic_resp(&resp_param, MSG_FSYNC, success);
		msg_debug("[MSG_FSYNC] done %lx",
			  msg_body->fsync.daemon_inode_va);
		PF_TL_END(a_evt_sd_fsync);
		/* After finishing a job, drain bounced fsyncs into all idle worker slots. */
		fsync_bounce_drain_to_capacity();
		break;

		// case MSG_GETSHM:
		// 	msg_debug("[MSG_FSYNC]");

		// 	ret = handle_fsync(msg_body);

		// 	success = ret < 0 ? 0 : 1;

		// 	fill_rpc_resp_param(&resp_param, &rpc_ch, msg, param);
		// 	msg_send_libfs_basic_resp(&resp_param, MSG_FSYNC, success);
		// 	msg_debug("[MSG_GETSHM] done");
		// 	break;

		// case MSG_INO_FSYNC:
		// 	msg_debug("[MSG_INO_FSYNC]");

		// 	PF_TL_START(a_evt_sd_fsync);

		// 	ret = handle_ino_fsync(msg_body);

		// 	success = ret < 0 ? 0 : 1;

		// 	fill_rpc_resp_param(&resp_param, &rpc_ch, msg, param);
		// 	msg_send_libfs_basic_resp(&resp_param, MSG_INO_FSYNC, success);

		// 	PF_TL_END(a_evt_sd_fsync);

		// 	break;

	case MSG_SYNC:
		msg_debug("[MSG_SYNC]");
		ret = handle_sync();
		success = ret < 0 ? 0 : 1;

		fill_rpc_resp_param(&resp_param, &rpc_ch, msg, param);
		msg_send_libfs_basic_resp(&resp_param, MSG_SYNC, success);
		/* After finishing a job, drain bounced fsyncs into all idle worker slots. */
		fsync_bounce_drain_to_capacity();
		break;

		// case MSG_FSTAT:
		// 	msg_debug("[MSG_FSTAT]");

		// 	// inode virtual address should be qualified.
		// 	fill_rpc_resp_param(&resp_param, &rpc_ch, msg, param);
		// 	msg_send_libfs_fstat_resp(&resp_param, msg_body);
		// 	ret = 0;

		// 	break;

		// case MSG_INO_FSTAT:
		// 	msg_debug("[MSG_INO_FSTAT]");

		// 	// inode virtual address should be qualified.
		// 	fill_rpc_resp_param(&resp_param, &rpc_ch, msg, param);
		// 	msg_send_libfs_ino_fstat_resp(&resp_param, msg_body);
		// 	ret = 0;

		// 	break;

	case MSG_FALLOC:
		msg_debug("[MSG_FALLOCATE]");

		ret = handle_fallocate(msg_body);

		success = ret < 0 ? 0 : 1;

		fill_rpc_resp_param(&resp_param, &rpc_ch, msg, param);
		msg_send_libfs_basic_resp(&resp_param, MSG_FALLOC, success);
		/* After finishing a job, drain bounced fsyncs into all idle worker slots. */
		fsync_bounce_drain_to_capacity();

		break;

	case MSG_FTRUNC:
		msg_debug("[MSG_FTRUNCATE]");

		ret = handle_ftruncate(msg_body);

		success = ret < 0 ? 0 : 1;

		fill_rpc_resp_param(&resp_param, &rpc_ch, msg, param);
		msg_send_libfs_basic_resp(&resp_param, MSG_FTRUNC, success);
		/* After finishing a job, drain bounced fsyncs into all idle worker slots. */
		fsync_bounce_drain_to_capacity();

		break;

	case MSG_ADD_JOURNAL:
		// msg_debug("[MSG_ADD_JOURNAL]");
		ret = handle_add_journal(msg_body);
		success = ret == 0 ? 1 : 0;

		fill_rpc_resp_param(&resp_param, &rpc_ch, msg, param);
		msg_send_libfs_basic_resp(&resp_param, MSG_ADD_JOURNAL,
					  success);
		/* After finishing a job, drain bounced fsyncs into all idle worker slots. */
		fsync_bounce_drain_to_capacity();

		// msg_debug("[MSG_ADD_JOURNAL] done");
		break;

	default:
		log_error("Unknown message operation: %d", msg_body->op);
	}

	free(msg);
	free(param);
}

int init_rpc(void)
{
	int ret;

	oxb_info("Initializing RPC module.");

#ifdef FSYNC_BOUNCE
	/* Init fsync bounce queue */
	g_fsync_bounce_head = g_fsync_bounce_tail = NULL;
	pthread_spin_init(&g_fsync_bounce_lock, PTHREAD_PROCESS_PRIVATE);
#endif

	// Init message handler thread pools.
	rpc_libfs_handler_thpool = iod_workers; // change to io workers
	// rpc_libfs_handler_thpool = thpool_init(g_sd_conf.rpc_shmem_thread_num,
	// 				       "rpc_shmem_handler");

	// Initialize RPC shmem (IPC) server. (<-> LibFS)
	ret = init_rpc_server(RPC_CH_SHMEM,
			      "/tmp/" XSTR(RPC_SHMEM_CM_PATH_LIBFS_DAEMON), 0,
			      sizeof(struct msg_data), rpc_shmem_libfs_handler,
			      rpc_libfs_handler_thpool, NULL, NULL, NULL, NULL,
			      RPC_SHMEM_SEED_LIBFS_DAEMON);
	if (ret < 0) {
		log_error("RPC shmem server initialization failed.");
		return ret;
	}
	sleep(1);
	oxb_info("RPC (shmem) server is running.");

#ifndef VM_ENV_NO_DEVFS
	// Initialize RPC RDMA client. (<-> DevFS)
	if (g_sd_conf.bg_journaling) {
#ifdef HOST_JOURNALING
		g_rpc_devfs_client = init_rpc_client(
			RPC_CH_SHMEM,
			"/tmp/" XSTR(RPC_SHMEM_CM_PATH_DAEMON_DEVFS), 0,
			sizeof(struct msg_data), rpc_shmem_devfs_handler, NULL,
			RPC_SHMEM_SEED_DAEMON_DEVFS);

		oxb_info(
			"RPC (to DevFS) client is connected to server (devfs): %s",
			"/tmp/" XSTR(RPC_SHMEM_CM_PATH_DAEMON_DEVFS));
#else
		// Init message handler thread pools.
		rpc_devfs_handler_thpool = thpool_init(
			g_sd_conf.rpc_rdma_thread_num, "rpc_devfs_handler");

		g_rpc_devfs_client = init_rpc_client(
			RPC_CH_RDMA, g_sd_conf.rpc_rdma_ip_addr,
			g_sd_conf.rpc_rdma_port, sizeof(struct msg_data),
			rpc_rdma_client_handler, rpc_devfs_handler_thpool, 0);

		oxb_info(
			"RPC (to DevFS) client is connected to server (devfs): %s:%d",
			g_sd_conf.rpc_rdma_ip_addr, g_sd_conf.rpc_rdma_port);
#endif

		if (!g_rpc_devfs_client) {
			log_error(
				"RPC (to DevFS) client initialization failed.");
			return -1;
		}

	}
#endif

	return 0;
}

void exit_rpc(void)
{
	oxb_info("Exiting RPC module.");

	oxb_info(" - Exiting RPC LibFS handler thpool.");

	/* Stop scheduling new read pump jobs so that the shared threadpool
	 * can drain cleanly during shutdown.
	 */
	nvme_stop_rd_workers();

	thpool_wait(rpc_libfs_handler_thpool);
	thpool_destroy(rpc_libfs_handler_thpool);

#ifndef VM_ENV_NO_DEVFS
#ifndef HOST_JOURNALING
	thpool_wait(rpc_devfs_handler_thpool);
	thpool_destroy(rpc_devfs_handler_thpool);
	oxb_info(" - Exiting RPC DevFS handler thpool.");
#endif
#endif
}

int init_msg(void)
{
	int ret;

	ret = init_rpc();
	if (ret < 0) {
		log_error("Failed to initialize MSG module.");
		return -1;
	}

	msg_async_thpool = thpool_init(2, "msg_async");

	return 0;
}

void exit_msg(void)
{
	oxb_info("Exiting MSG module (rpc).");
	exit_rpc();
}

/**
 * @brief Send a message to DevFS and do not care of response.
 * 
 * @param data Message body.
 */
static void send_msg_to_devfs_nowait(char *data)
{
	struct rpc_req_param req_param;

	req_param = (struct rpc_req_param){ .rpc_ch = g_rpc_devfs_client,
					    .data = data,
					    .sem = NULL };

	send_rpc_msg_to_server(&req_param);

	// Return without waiting.
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
 * @brief Send a message to DevFS and do not care of response.
 * 
 * @param data Message body.
 */
static void send_msg_to_devfs_nowait_free_buf(char *data)
{
	struct rpc_req_param req_param;
	struct free_th_arg *ft_arg;
	int msgbuf_id;

	req_param = (struct rpc_req_param){ .rpc_ch = g_rpc_devfs_client,
					    .data = data,
					    .sem = NULL };

	msgbuf_id = send_rpc_msg_to_server(&req_param);

	ft_arg = calloc(1, sizeof(struct free_th_arg));
	ft_arg->rpc_cli_ch = g_rpc_devfs_client;
	ft_arg->msgbuf_id = msgbuf_id;

	// Free msgbuf in another thread.
	thpool_add_work(msg_async_thpool, free_msgbuf, (void *)ft_arg);
}

/**
 * @brief Send a message to DevFS.
 * 
 * @param data Message body.
 * @param sem 
 */
static void send_msg_to_devfs_sync(char *data)
{
	struct rpc_req_param req_param;
	sem_t sem;

	sem_init(&sem, 0, 0);

	req_param = (struct rpc_req_param){ .rpc_ch = g_rpc_devfs_client,
					    .data = data,
					    .sem = &sem };

	send_rpc_msg_to_server(&req_param);

	// Waiting server response.
	sem_wait(&sem);
}

/**
 * @brief Send a message to DevFS. You have to call sem_wait() manually.
 * 
 * @param data Message body.
 * @param sem 
 * @return int  msgbuf_id is returned.
 */
static int send_msg_to_devfs_async(char *data, sem_t *sem)
{
	struct rpc_req_param req_param;
	int ret;

	sem_init(sem, 0, 0);

	req_param = (struct rpc_req_param){ .rpc_ch = g_rpc_devfs_client,
					    .data = data,
					    .sem = sem };

	ret = send_rpc_msg_to_server(&req_param);

	return ret;
}

/**
 * @brief
 * 
 * @param df_id Data Fetcher ID.
 * @param md_buf_id Data Fetcher buffer ID for metadata (desc and tags blocks).
 * @param md_size Size of Data Fetcher buffer for metadata (desc and tags blocks).
 * @param total_size Total size of the data.
 * @param tx Pointer of the transaction to be freed in the callback.
 */
void msg_send_devfs_journal(int df_id, int md_buf_id, uint64_t md_size,
			   uint64_t total_data_size, int n_fetch_reqs, char *tx,
			   uint32_t tid)
{
	struct msg_data m = { 0 };

	m.op = MSG_BG_JOURNAL;

	m.devfs_journal.df_id = df_id;
	m.devfs_journal.md_buf_id = md_buf_id;
	m.devfs_journal.md_size = md_size;
	m.devfs_journal.total_data_size = total_data_size;
	m.devfs_journal.n_fetch_reqs = n_fetch_reqs;
	m.devfs_journal.tx_p = (char *)tx;
	m.devfs_journal.tx_id = tid;
	print_req_msg_data(&m);

#ifdef HOST_JOURNALING
	send_msg_to_devfs_nowait_free_buf((char *)&m);
#else
	send_msg_to_devfs_nowait((char *)&m);
#endif
}

/**
 * @brief 
 * 
 * @param df_id Data Fetcher ID.
 * @param req_id Id of a chunk in a transaction.
 * @param buf_id Id of the Data Fetcher buffer.
 * @param size Size of data to be fetched.
 */
void msg_send_devfs_fetch_data(int df_id, int req_id, int buf_id,
			       uint64_t size)
{
	struct msg_data m = { 0 };

	m.op = MSG_FETCH_DATA;

	m.devfs_fetch_data.df_id = df_id;
	m.devfs_fetch_data.req_id = req_id;
	m.devfs_fetch_data.buf_id = buf_id;
	m.devfs_fetch_data.size = size;

	print_req_msg_data(&m);

#ifdef HOST_JOURNALING
	send_msg_to_devfs_nowait_free_buf((char *)&m);
#else
	send_msg_to_devfs_nowait((char *)&m);
#endif
}

/**
 * @brief Send a stage checkpoint request to DevFS.
 *
 * @param stage_total_blks Total number of blocks in the stage area.
 * @param nr_blks_to_free Desired number of blocks to free.
 * @param is_sync 1: synchronous (blocks until done), 0: asynchronous.
 */
void msg_send_devfs_stg_ckpt(uint32_t stage_total_blks,
			     uint32_t nr_blks_to_free, uint8_t is_sync)
{
	struct msg_data m = { 0 };

	m.op = MSG_STG_CKPT;

	m.devfs_stg_ckpt.stage_total_blks = stage_total_blks;
	m.devfs_stg_ckpt.nr_blks_to_free = nr_blks_to_free;
	m.devfs_stg_ckpt.is_sync = is_sync;

	print_req_msg_data(&m);

	if (is_sync)
		send_msg_to_devfs_sync((char *)&m);
	else
		send_msg_to_devfs_nowait((char *)&m);
}

/* Function name format: msg_send_<target>_<op>[_resp]() */

/* Targets */
/* libfs: LibFS */
void msg_send_libfs_basic_resp(struct rpc_resp_param *resp_param, int op,
			       int success)
{
	MSG_DATA(m, op, success);

	resp_param->data = (char *)&m;

	print_resp_msg_data(&m);

	send_rpc_response_to_client(resp_param);
}

void msg_send_libfs_resp(struct rpc_resp_param *resp_param, int pid)
{
	MSG_DATA(m, MSG_INIT, 1);
	int ret;

	ret = get_proc_fd_map_ent(pid, m.sd_init_resp.key,
				  &m.sd_init_resp.index);
	if (ret)
		m.rc = 0;

	resp_param->data = (char *)&m;

	print_resp_msg_data(&m);

	send_rpc_response_to_client(resp_param);
}

// void msg_send_libfs_shm_resp(struct rpc_resp_param *resp_param,
// 			     struct msg_data *msg_body)
// {
// 	struct inode *inode;
// 	MSG_DATA(m, MSG_GETSHM, 1);

// 	log_info("fstat ino(%lu)", msg_body->ino_fstat.ino);

// 	// may be need locking later
// 	m.sd_getshm_resp.daemon_fd = inode->;
// 	m.sd_fstat_resp.st_nlink = inode->i_nlink;

// 	// print_inode_state(inode);
// }

// void msg_send_libfs_fstat_resp(struct rpc_resp_param *resp_param,
// 			       struct msg_data *msg_body)
// {
// 	struct inode *inode;
// 	MSG_DATA(m, MSG_FSTAT, 1);

// 	log_info("fstat ino(%lu)", msg_body->ino_fstat.ino);

// 	inode = msg_body->fstat.daemon_inode_va;
// 	if (!inode) {
// 		oxb_error("no inode");
// 		m.rc = 0;
// 	} else {
// 		// may be need locking later
// 		m.sd_fstat_resp.st_ino = inode->i_ino;
// 		m.sd_fstat_resp.st_nlink = inode->i_nlink;
// 		m.sd_fstat_resp.st_mode = inode->i_mode;
// 		m.sd_fstat_resp.st_uid = inode->i_uid;
// 		m.sd_fstat_resp.st_gid = inode->i_gid;
// 		m.sd_fstat_resp.st_size = inode->i_size;
// 		m.sd_fstat_resp.st_blksize = OXBOW_BLOCK_SIZE;
// 		m.sd_fstat_resp.st_blocks = inode->i_blocks;
// 		m.sd_fstat_resp.stat_buf = msg_body->fstat.stat_buf;
// 		m.rc = 1;
// 		// print_inode_state(inode);
// 	}

// 	resp_param->data = (char *)&m;

// 	send_rpc_response_to_client(resp_param);
// }

// void msg_send_libfs_ino_fstat_resp(struct rpc_resp_param *resp_param,
// 				   struct msg_data *msg_body)
// {
// 	struct inode *inode;
// 	MSG_DATA(m, MSG_INO_FSTAT, 1);

// 	log_info("ino_fstat ino(%lu)", msg_body->ino_fstat.ino);

// 	inode = iget_locked(NULL, msg_body->ino_fstat.ino);
// 	if (!inode) {
// 		oxb_error("no inode(%lu)", msg_body->ino_fstat.ino);
// 		m.rc = 0;
// 	} else {
// 		if (inode->i_state & I_NEW) {
// 			oxb_error("race inode(%lu)", inode->i_ino);
// 			m.rc = 0;
// 			goto ret;
// 		}
// 		// may be need locking later
// 		m.sd_fstat_resp.st_ino = inode->i_ino;
// 		m.sd_fstat_resp.st_nlink = inode->i_nlink;
// 		m.sd_fstat_resp.st_mode = inode->i_mode;
// 		m.sd_fstat_resp.st_uid = inode->i_uid;
// 		m.sd_fstat_resp.st_gid = inode->i_gid;
// 		m.sd_fstat_resp.st_size = inode->i_size;
// 		m.sd_fstat_resp.st_blksize = OXBOW_BLOCK_SIZE;
// 		m.sd_fstat_resp.st_blocks = inode->i_blocks;
// 		m.sd_fstat_resp.stat_buf = msg_body->ino_fstat.stat_buf;
// 		m.rc = 1;
// 		// print_inode_state(inode);
// 	}

// ret:
// 	resp_param->data = (char *)&m;

// 	send_rpc_response_to_client(resp_param);
// 	if (inode)
// 		iput(inode);
// }
