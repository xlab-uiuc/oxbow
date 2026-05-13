#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <pthread.h>
#include <semaphore.h>
#include <stdatomic.h>
#include <ctype.h>

#include "config.h"
#include "io_dispatcher.h"
#include "io/sd_bio.h"

#define BYTES_PER_PAGE 4096
#define CHUNK_PAGES BIO_MAX_VECS
#define CHUNK_BYTES (BYTES_PER_PAGE * CHUNK_PAGES)
#define CHUNKS_PER_THREAD 256 /* total per thread: 16MB when CHUNK_PAGES=16 */
#define PER_THREAD_TOTAL_BYTES (CHUNK_BYTES * CHUNKS_PER_THREAD)
#define START_BLK_ADDR 1
#define NUM_THREADS 8

struct test_thread_ctx {
	int tid;
	char *wbuf;
	char *rbuf;
	baddr_t start_blk;
	unsigned long pattern_offset;
	sem_t wr_done;
	atomic_int wr_inflight;
	sem_t rd_done;
	atomic_int rd_inflight;
	int result; // 0 on success, non-zero on failure
};

static void fill_pattern_with_offset(char *buf, size_t bytes, unsigned long offset)
{
	for (size_t i = 0; i < bytes; i++)
		buf[i] = '0' + ((i + offset) % 10);
}

static int verify_pattern_with_offset(const char *buf, size_t bytes, unsigned long offset)
{
	for (size_t i = 0; i < bytes; i++) {
		if (buf[i] != (char)('0' + ((i + offset) % 10)))
			return -1;
	}
	return 0;
}

/* Print a compact hexdump window around the mismatch location */
static void hexdump_around_index(const char *buf, size_t total_bytes, size_t index)
{
	/* Show 2 lines of 16 bytes centered on the mismatching line */
	size_t line = index / 16;
	size_t start = (line >= 1) ? (line - 1) * 16 : 0;
	size_t end = ((line + 2) * 16 <= total_bytes) ? (line + 2) * 16 : total_bytes;

	for (size_t base = start; base < end; base += 16) {
		fprintf(stderr, "  %08zu: ", base);
		for (size_t j = 0; j < 16; j++) {
			size_t p = base + j;
			if (p < total_bytes)
				fprintf(stderr, "%02x ", (unsigned char)buf[p]);
			else
				fprintf(stderr, "   ");
		}
		fprintf(stderr, " |");
		for (size_t j = 0; j < 16; j++) {
			size_t p = base + j;
			unsigned char ch = (p < total_bytes) ? (unsigned char)buf[p] : 0;
			fprintf(stderr, "%c", (p < total_bytes && isprint(ch)) ? (char)ch : '.');
		}
		fprintf(stderr, "|\n");
	}
}

/* Scan buffer and print detailed information about mismatches */
static void report_mismatch_details(const char *buf, size_t bytes,
					unsigned long pattern_offset, baddr_t start_blk)
{
	const size_t max_reports = 16; /* limit noisy output */
	size_t mismatch_count = 0;

	for (size_t i = 0; i < bytes; i++) {
		char expected = (char)('0' + ((i + pattern_offset) % 10));
		char actual = buf[i];
		if (actual == expected)
			continue;

		mismatch_count++;
		if (mismatch_count <= (ssize_t)max_reports) {
			size_t page_index = i / BYTES_PER_PAGE;
			size_t offset_in_page = i % BYTES_PER_PAGE;
			size_t chunk_index = i / CHUNK_BYTES;
			size_t offset_in_chunk = i % CHUNK_BYTES;
			baddr_t lba = start_blk + (baddr_t)page_index;

			fprintf(stderr,
				"Mismatch #%zu: byte=%zu chunk=%zu(+%zu) page=%zu(+%zu) LBA=%lu expected=0x%02x('%c') actual=0x%02x('%c')\n",
				(size_t)mismatch_count,
				i,
				chunk_index, offset_in_chunk,
				page_index, offset_in_page,
				(unsigned long)lba,
				(unsigned char)expected, isprint((unsigned char)expected) ? expected : '.',
				(unsigned char)actual, isprint((unsigned char)actual) ? actual : '.');

			hexdump_around_index(buf, bytes, i);
		}
	}

	fprintf(stderr, "Total mismatches: %zu out of %zu bytes\n",
		(size_t)mismatch_count, bytes);

	if (mismatch_count > (ssize_t)max_reports)
		fprintf(stderr, "(Only first %zu mismatches shown)\n", max_reports);
}

// removed: per-bio semaphore end_io

static void bio_end_io_write(void *args)
{
	struct bio *bio = args;
	struct test_thread_ctx *ctx = (struct test_thread_ctx *)bio->bi_private;
	if (atomic_fetch_sub(&ctx->wr_inflight, 1) == 1)
		sem_post(&ctx->wr_done);
	if (bio->bi_io_vec)
		free(bio->bi_io_vec);
	free(bio);
}

static void bio_end_io_read(void *args)
{
	struct bio *bio = args;
	struct test_thread_ctx *ctx = (struct test_thread_ctx *)bio->bi_private;
	if (atomic_fetch_sub(&ctx->rd_inflight, 1) == 1)
		sem_post(&ctx->rd_done);
	if (bio->bi_io_vec)
		free(bio->bi_io_vec);
	free(bio);
}

static struct bio *build_single_bio(blk_opf_t opf, char *buf,
					 baddr_t start_blk, size_t len)
{
	struct bio *bio;

	// Use one vector per page for clear semantics with current nvme.c logic
	unsigned int nr_vecs = len / BYTES_PER_PAGE;
	if (len % 4096)
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

static void *thread_main(void *arg)
{
	struct test_thread_ctx *ctx = (struct test_thread_ctx *)arg;
	struct bio *wbio = NULL;
	struct bio *rbio = NULL;
	int rc;

	// Fill full write buffer for this thread
	fill_pattern_with_offset(ctx->wbuf, PER_THREAD_TOTAL_BYTES, ctx->pattern_offset);

	// Submit all write chunks without per-chunk waits
	atomic_store(&ctx->wr_inflight, CHUNKS_PER_THREAD);
	for (size_t c = 0; c < CHUNKS_PER_THREAD; c++) {
		char *chunk_w = ctx->wbuf + c * CHUNK_BYTES;
		baddr_t chunk_lba = ctx->start_blk + (baddr_t)(c * CHUNK_PAGES);

		wbio = build_single_bio(REQ_OP_WRITE, chunk_w, chunk_lba, CHUNK_BYTES);
		if (!wbio) {
			fprintf(stderr, "[T%d] Failed to build write bio (chunk %zu).\n", ctx->tid, c);
			ctx->result = 1;
			return NULL;
		}
		wbio->bi_private = ctx;
		wbio->end_io = bio_end_io_write;
		iod_submit_bio(REQ_OP_WRITE, wbio);
	}
	// Wait until all writes complete
	sem_wait(&ctx->wr_done);

	printf("[iod_test] All write completed\n");

	// Submit all read chunks, then wait once for completion
	atomic_store(&ctx->rd_inflight, CHUNKS_PER_THREAD);
	for (size_t c = 0; c < CHUNKS_PER_THREAD; c++) {
		char *chunk_r = ctx->rbuf + c * CHUNK_BYTES;
		baddr_t chunk_lba = ctx->start_blk + (baddr_t)(c * CHUNK_PAGES);

		rbio = build_single_bio(REQ_OP_READ, chunk_r, chunk_lba, CHUNK_BYTES);
		if (!rbio) {
			fprintf(stderr, "[T%d] Failed to build read bio (chunk %zu).\n", ctx->tid, c);
			ctx->result = 1;
			return NULL;
		}
		rbio->bi_private = ctx;
		rbio->end_io = bio_end_io_read;
		iod_submit_bio(REQ_OP_READ, rbio);
	}
	sem_wait(&ctx->rd_done);

	printf("[iod_test] All read completed\n");

	// Verify entire buffer
	rc = verify_pattern_with_offset(ctx->rbuf, PER_THREAD_TOTAL_BYTES, ctx->pattern_offset);
	if (rc) {
		fprintf(stderr, "[T%d] Data mismatch. Showing details...\n", ctx->tid);
		report_mismatch_details(ctx->rbuf, PER_THREAD_TOTAL_BYTES, ctx->pattern_offset, ctx->start_blk);
		ctx->result = 1;
		return NULL;
	}

	ctx->result = 0;
	return NULL;
}

int main(void)
{
	int ret;
	pthread_t threads[NUM_THREADS];
	struct test_thread_ctx ctxs[NUM_THREADS];
	int any_fail = 0;

	printf("IOD SUBMIT BIO TEST\n");

	load_secure_daemon_configs();
	print_secure_daemon_configs();

	ret = init_io_dispatcher();
	if (ret < 0) {
		fprintf(stderr, "Failed to initialize IO Dispatcher.\n");
		return -1;
	}

	// Per-run shift so data pattern differs each execution
	struct timespec ts;
	clock_gettime(CLOCK_REALTIME, &ts);
	unsigned long run_shift = (unsigned long)((ts.tv_nsec ^ ts.tv_sec) % 10);

	// Launch threads
	for (int i = 0; i < NUM_THREADS; i++) {
		ctxs[i].tid = i;
		ctxs[i].wbuf = calloc(1, PER_THREAD_TOTAL_BYTES);
		ctxs[i].rbuf = calloc(1, PER_THREAD_TOTAL_BYTES);
		if (!ctxs[i].wbuf || !ctxs[i].rbuf) {
			fprintf(stderr, "Allocation failed for thread %d.\n", i);
			return -1;
		}

		// Separate each thread's LBA range
		baddr_t stride_blks = (PER_THREAD_TOTAL_BYTES / BYTES_PER_PAGE) + 128; // extra gap
		ctxs[i].start_blk = START_BLK_ADDR + (baddr_t)i * stride_blks;
		ctxs[i].pattern_offset = (run_shift + (unsigned long)((i * 3) % 10)) % 10;
		sem_init(&ctxs[i].wr_done, 0, 0);
		sem_init(&ctxs[i].rd_done, 0, 0);
		ctxs[i].result = 1;

		if (pthread_create(&threads[i], NULL, thread_main, &ctxs[i]) != 0) {
			fprintf(stderr, "Failed to create thread %d.\n", i);
			return -1;
		}
	}

	for (int i = 0; i < NUM_THREADS; i++) {
		pthread_join(threads[i], NULL);
		if (ctxs[i].result != 0)
			any_fail = 1;
		sem_destroy(&ctxs[i].wr_done);
		sem_destroy(&ctxs[i].rd_done);
		free(ctxs[i].wbuf);
		free(ctxs[i].rbuf);
	}

	if (any_fail) {
		fprintf(stderr, "Test failed.\n");
		return 1;
	}

	printf("Test passed.\n");
	return 0;
}


