#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <pthread.h>
#include <semaphore.h>
#include <stdatomic.h>
#include <signal.h>

#include "config.h"
#include "profile_secure_daemon.h"
#include "io_dispatcher.h"
#include "io/sd_bio.h"

#define BYTES_PER_PAGE		4096
#define CHUNK_PAGES		BIO_MAX_VECS
#define CHUNK_BYTES		(BYTES_PER_PAGE * CHUNK_PAGES)

/* Per-thread total read volume: 2 GiB = 2 * 1024^3 bytes. */
#define PER_THREAD_TOTAL_BYTES	(2UL * 1024UL * 1024UL * 1024UL)

/* Must divide evenly for clean chunking; with BIO_MAX_VECS=32 or 64 this holds. */
#define CHUNKS_PER_THREAD	(PER_THREAD_TOTAL_BYTES / CHUNK_BYTES)
#define NUM_THREADS		8

struct bw_thread_ctx {
	int			tid;
	char			*rbuf;
	baddr_t			start_blk;
	sem_t			rd_done;
	atomic_int		rd_inflight;
	int			result;
};

static pthread_barrier_t g_start_barrier;

static struct bio *build_single_bio(blk_opf_t opf, char *buf,
				    baddr_t start_blk, size_t len)
{
	struct bio *bio;
	unsigned int nr_vecs = len / BYTES_PER_PAGE;

	if (len % BYTES_PER_PAGE)
		nr_vecs++;

	bio = alloc_bio(nr_vecs, opf);
	if (!bio)
		return NULL;

	bio->bi_start = start_blk;
	bio->bi_vcnt = nr_vecs;
	bio->bi_vtotal = nr_vecs;

	for (unsigned int i = 0; i < nr_vecs; i++) {
		bio->bi_io_vec[i].bv_buf = buf + (i * BYTES_PER_PAGE);
		bio->bi_io_vec[i].bv_len = BYTES_PER_PAGE;
	}

	return bio;
}

static void bio_end_io_read_bw(void *args)
{
	struct bio *bio = args;
	struct bw_thread_ctx *ctx = (struct bw_thread_ctx *)bio->bi_private;

	if (atomic_fetch_sub(&ctx->rd_inflight, 1) == 1)
		sem_post(&ctx->rd_done);

	if (bio->bi_io_vec)
		free(bio->bi_io_vec);
	free(bio);
}

static void *thread_main(void *arg)
{
	struct bw_thread_ctx *ctx = (struct bw_thread_ctx *)arg;
	size_t total_bytes = (size_t)CHUNK_BYTES * (size_t)CHUNKS_PER_THREAD;
	struct bio *rbio = NULL;

	/* Wait for all threads (and main) to be ready. */
	pthread_barrier_wait(&g_start_barrier);

	atomic_store(&ctx->rd_inflight, CHUNKS_PER_THREAD);

	for (size_t c = 0; c < CHUNKS_PER_THREAD; c++) {
		char *chunk_r = ctx->rbuf + c * CHUNK_BYTES;
		baddr_t chunk_lba = ctx->start_blk +
				    (baddr_t)(c * CHUNK_PAGES);

		rbio = build_single_bio(REQ_OP_READ, chunk_r,
					chunk_lba, CHUNK_BYTES);
		if (!rbio) {
			fprintf(stderr,
				"[T%d] Failed to build read bio (chunk %zu).\n",
				ctx->tid, c);
			ctx->result = 1;
			return NULL;
		}

		rbio->bi_private = ctx;
		rbio->end_io = bio_end_io_read_bw;
		iod_submit_bio(REQ_OP_READ, rbio);
	}

	/* Wait until all reads complete. */
	sem_wait(&ctx->rd_done);

	/* We do not verify data – this test is for pure throughput. */
	(void)total_bytes;
	ctx->result = 0;
	return NULL;
}

int main(void)
{
	int ret;
	pthread_t threads[NUM_THREADS];
	struct bw_thread_ctx ctxs[NUM_THREADS];
	size_t per_thread_bytes = (size_t)PER_THREAD_TOTAL_BYTES;
	size_t total_bytes = per_thread_bytes * (size_t)NUM_THREADS;
	struct timespec ts_start, ts_end;
	double elapsed_sec;
	double mb;
	double mbps;
	int any_fail = 0;

	printf("NVMe read throughput test via io_dispatcher/nvme layer\n");
	printf("  NUM_THREADS       = %d\n", NUM_THREADS);
	printf("  CHUNK_PAGES       = %d\n", CHUNK_PAGES);
	printf("  CHUNKS_PER_THREAD = %d\n", CHUNKS_PER_THREAD);

	load_secure_daemon_configs();
	print_secure_daemon_configs();

#ifdef OXBOW_PROFILE
	/* Initialize profiling infra so PF_TL_* in shared library are safe. */
	pf_init(PF_SIGNAL);
#endif

	ret = init_io_dispatcher();
	if (ret < 0) {
		fprintf(stderr, "Failed to initialize IO Dispatcher.\n");
		return -1;
	}

	/* Barrier for all worker threads + main. */
	pthread_barrier_init(&g_start_barrier, NULL, NUM_THREADS + 1);

	for (int i = 0; i < NUM_THREADS; i++) {
		ctxs[i].tid = i;
		ctxs[i].rbuf = calloc(1, per_thread_bytes);
		if (!ctxs[i].rbuf) {
			fprintf(stderr,
				"Allocation failed for thread %d (size=%zu).\n",
				i, per_thread_bytes);
			return -1;
		}

		/* Separate each thread's LBA range to avoid overlap. */
		baddr_t stride_blks =
			(per_thread_bytes / BYTES_PER_PAGE) + 128;
		ctxs[i].start_blk = (baddr_t)1 +
				    (baddr_t)i * stride_blks;
		sem_init(&ctxs[i].rd_done, 0, 0);
		ctxs[i].result = 1;

		if (pthread_create(&threads[i], NULL,
				   thread_main, &ctxs[i]) != 0) {
			fprintf(stderr, "Failed to create thread %d.\n", i);
			return -1;
		}
	}

	clock_gettime(CLOCK_MONOTONIC, &ts_start);
	pthread_barrier_wait(&g_start_barrier);

	for (int i = 0; i < NUM_THREADS; i++) {
		pthread_join(threads[i], NULL);
		if (ctxs[i].result != 0)
			any_fail = 1;
		sem_destroy(&ctxs[i].rd_done);
		free(ctxs[i].rbuf);
	}

	clock_gettime(CLOCK_MONOTONIC, &ts_end);

	if (any_fail) {
		fprintf(stderr, "Throughput test failed.\n");
		return 1;
	}

	elapsed_sec = (ts_end.tv_sec - ts_start.tv_sec) +
		      (ts_end.tv_nsec - ts_start.tv_nsec) / 1e9;
	mb = (double)total_bytes / (1024.0 * 1024.0);
	mbps = mb / elapsed_sec;

	printf("Total bytes read : %.2f MiB\n", mb);
	printf("Elapsed time     : %.6f s\n", elapsed_sec);
	printf("Throughput       : %.2f MiB/s\n", mbps);

	return 0;
}


