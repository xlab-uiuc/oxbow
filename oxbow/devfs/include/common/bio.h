#ifndef _BIO_H_
#define _BIO_H_

#include "linux/oxbow_kernel.h"
#include <stdatomic.h>
#include "oxbow.h"
#include "oxbow_debug.h"
#include <stdio.h>
#include <stdlib.h>

// Same as Linux's
#define BIO_MAX_VECS 16

/**
 * @brief It is the maximum size of a bio. If its size exceeds this value, SPDK
 * fails to allocate a request (qpair->free_req at nvme_allocate_request() in
 * lib/nvme/nvme_internal.)
 * It should match DATA_FETCHER_BUF_SIZE in oxbow.h.
 */
//64 MB is max but we choose smaller for pipelining.
#define BIO_MAX_BIO_SIZE (DATA_FETCHER_BUF_SIZE)
// #define BIO_MAX_BIO_SIZE (1024 * 1024 * 16UL) // 16 MB

/**
 * @brief Virtual memory address to block number mapping.
 * It is for sorting mappings. After sorting, we build
 * bio_list.
 */
struct mb_map {
	char *bv_buf; // Starting address of buffer.
	baddr_t block_num; // Block number.
	unsigned int n_blks; // Number of blocks.
};

/**
 * @brief A contiguous range of virtual memory addresses.
 * It means that a bio_vec entry corresponds to a buffer.
 * 
 */
struct bio_vec {
	char *bv_buf; // Starting address of buffer.
	size_t bv_len; // Number of bytes in the address range.
};

/**
 * @brief A contiguous range of physical storage blocks.
 * 
 */
struct bvec_iter {
	baddr_t bi_blk_no; /* Block number index (4KB granularity) */
	// unsigned int bi_size; /* residual I/O size in byte */ // Not used.
	// unsigned int bi_idx; /* current index into bi_io_vec */ // Not used.
	unsigned int bi_bvec_done; /* number of bytes completed in current bvec */
};

/**
 * @brief Block I/O request for a contiguous range of storage blocks.
 * It is a unit of thread parallelization. Namely, each thread is requested
 * to process one bio at a time.
 */
struct bio {
	struct bio *bi_next; /* request queue link (bio list) */
	struct bvec_iter
		bi_iter; // REFACTOR: We don't need iterator.  Only need bi_blk_no (start baddr)
	unsigned short bi_vcnt; /* how many bio_vec's */
	size_t total_size; /* Total IO size of all bvecs in this bio. */

	// FYI, in DevFS, there are two bio_vecs only if a wrap-around occurs in RDMA buffer.
	struct bio_vec *bi_io_vec; /* the actual vec array */
	atomic_int is_completed; /* For synchronization between
				  * a requesting thread and a worker thread.
				  * Set to 1 when all NVMe requests are completed.
				  * Set to 2 if error occurs.
				  */
};

static inline int is_completed(struct bio *bio)
{
	return atomic_load_explicit(&bio->is_completed, memory_order_acquire);
}

static inline void set_is_completed(struct bio *bio, int val)
{
	// printf("store bio:0x%lx\n", (unsigned long)bio);
	atomic_store_explicit(&bio->is_completed, val, memory_order_release);
}

/**
  * @brief Allocate a new bio. Set the start block number.
  * 
  * @param blk_no Start blk_no id.
  * @return struct bio* 
  */
static inline struct bio *alloc_bio(baddr_t blk_no)
{
	struct bio *bio;

	// OPTIMIZE: Adopt slab allocator.
	bio = calloc(1, sizeof(struct bio));

	bio->bi_vcnt = 0;
	bio->bi_iter.bi_bvec_done = 0;
	bio->bi_iter.bi_blk_no = blk_no;

	// OPTIMIZE: Adopt slab allocator.
	// Allocate large chunk at once to avoid repeated small
	// allocations.
	bio->bi_io_vec = malloc(sizeof(struct bio_vec) * BIO_MAX_VECS);

	atomic_init(&bio->is_completed, 0);

	return bio;
}

/**
 * @brief Allocate a bio with N bio_vecs. Note that a bio_vec can
 * represent a contiguous memory region.
 * 
 * @param blk_no Starting block address.
 * @param n_bio_vecs The number of bio_vecs to allocate.
 * @return struct bio* 
 */
static inline struct bio *alloc_bio_n_bvecs(baddr_t blk_no, int n_bio_vecs)
{
	struct bio *bio;

	// OPTIMIZE: Adopt slab allocator.
	bio = calloc(1, sizeof(struct bio));

	bio->bi_vcnt = n_bio_vecs;
	bio->bi_iter.bi_bvec_done = 0;
	bio->bi_iter.bi_blk_no = blk_no;
	bio->total_size = 0;

	// OPTIMIZE: Adopt slab allocator.
	// Allocate N bio_vecs.
	bio->bi_io_vec = malloc(sizeof(struct bio_vec) * n_bio_vecs);

	atomic_init(&bio->is_completed, 0);

	return bio;
}

static inline void free_bio(struct bio *bio)
{
	if (!bio)
		return;

	if (bio->bi_io_vec)
		free(bio->bi_io_vec);

	free(bio);
}

// struct bio {
// 	char *bv_buf; // Starting virtual memory address of buffer.
// 	uint64_t bv_len; // Number of bytes.
// 	baddr_t bv_baddr; // Starting block address of device.
// };

/*
 * (Comments from Linux kernel. It's deprecated.)
 * BIO list management for use by remapping drivers (e.g. DM or MD) and loop.
 *
 * A bio_list anchors a singly-linked list of bios chained through the bi_next
 * member of the bio.  The bio_list also caches the last list member to allow
 * fast access to the tail.
 */
struct bio_list {
	struct bio *head;
	struct bio *tail;
};

static inline int bio_list_empty(const struct bio_list *bl)
{
	return bl->head == NULL;
}

static inline void bio_list_init(struct bio_list *bl)
{
	bl->head = bl->tail = NULL;
}

#define BIO_EMPTY_LIST { NULL, NULL }

#define bio_list_for_each(bio, bl)                                             \
	for (bio = (bl)->head; bio; bio = bio->bi_next)

static inline unsigned bio_list_size(const struct bio_list *bl)
{
	unsigned sz = 0;
	struct bio *bio;

	bio_list_for_each (bio, bl)
		sz++;

	return sz;
}

static inline void bio_list_add(struct bio_list *bl, struct bio *bio)
{
	bio->bi_next = NULL;

	if (bl->tail)
		bl->tail->bi_next = bio;
	else
		bl->head = bio;

	bl->tail = bio;
}

static inline void bio_list_add_head(struct bio_list *bl, struct bio *bio)
{
	bio->bi_next = bl->head;

	bl->head = bio;

	if (!bl->tail)
		bl->tail = bio;
}

static inline void bio_list_merge(struct bio_list *bl, struct bio_list *bl2)
{
	if (!bl2->head)
		return;

	if (bl->tail)
		bl->tail->bi_next = bl2->head;
	else
		bl->head = bl2->head;

	bl->tail = bl2->tail;
}

static inline void bio_list_merge_head(struct bio_list *bl,
				       struct bio_list *bl2)
{
	if (!bl2->head)
		return;

	if (bl->head)
		bl2->tail->bi_next = bl->head;
	else
		bl->tail = bl2->tail;

	bl->head = bl2->head;
}

static inline struct bio *bio_list_peek(struct bio_list *bl)
{
	return bl->head;
}

static inline struct bio *bio_list_peek_tail(struct bio_list *bl)
{
	return bl->tail;
}

static inline struct bio *bio_list_pop(struct bio_list *bl)
{
	struct bio *bio = bl->head;

	if (bio) {
		bl->head = bl->head->bi_next;
		if (!bl->head)
			bl->tail = NULL;

		bio->bi_next = NULL;
	}

	return bio;
}

static inline struct bio *bio_list_get(struct bio_list *bl)
{
	struct bio *bio = bl->head;

	bl->head = bl->tail = NULL;

	return bio;
}

static inline struct bio_list *alloc_bl(void)
{
	struct bio_list *bl;

	bl = malloc(sizeof(struct bio_list));
	if (!bl) {
		oxb_error("malloc fail");
		return bl;
	}

	bio_list_init(bl);

	return bl;
}

static inline void free_bl(struct bio_list *bl)
{
	struct bio *bio;

	while (!bio_list_empty(bl)) {
		bio = bio_list_pop(bl);
		free_bio(bio);
	}

	free(bl);
}

typedef unsigned long pgoff_t;

#define MAX_PAGES_BIO 32
struct ra_bio {
	/* daemon */
	char *bvec;
	baddr_t bvec_lba;
	unsigned int nr_pages; // can't over MAX_PAGES_BIO
	void (*end_io)(struct ra_bio *);

	/* storage engine */
	char *spdk_buf;

	/* kernel */
	int async;
	int daemon_fd;
	union {
		kaddr_t mapping;
		kaddr_t folio;
	};
	pgoff_t index;
};

#ifdef PRINT_BIO_DEBUG
static inline void print_bio(struct bio *bio)
{
	int i;
	struct bio_vec *bvec;
	baddr_t blk_no;

	blk_no = bio->bi_iter.bi_blk_no;

	printf("\tbio: bv_cnt=%u\n", bio->bi_vcnt);

	for (i = 0; i < bio->bi_vcnt; i++) {
		bvec = &bio->bi_io_vec[i];
		printf("\t\tbvec: blk_no=%lu size=%lu (%u blks, %lu KB, %lu MB)\n",
		       blk_no, bvec->bv_len, bytes_to_nblks(bvec->bv_len),
		       bvec->bv_len >> 10, bvec->bv_len >> 20);
		blk_no += bytes_to_nblks(bvec->bv_len);
	}
}

static inline void print_bl(struct bio_list *bl)
{
	struct bio *bio;
	int i;

	i = 0;
	printf("Print Bio List:\n");
	bio_list_for_each (bio, bl) {
		i++;
		print_bio(bio);
	}
	printf("Total bio count: %d\n", i);
}
#else
static inline void print_bio(struct bio *bio)
{
	UNUSED1(bio);
}
static inline void print_bl(struct bio_list *bl)
{
	UNUSED1(bl);
}
#endif

#endif
