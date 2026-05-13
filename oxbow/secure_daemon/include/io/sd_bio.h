#ifndef _SD_BIO_H_
#define _SD_BIO_H_

#include "buffer_head.h"
#include "linux/oxbow_kernel.h"
#include <stdatomic.h>
#include "oxbow.h"
#include "oxbow_debug.h"
#include <stdio.h>
#include <stdlib.h>

typedef __u32 __bitwise blk_opf_t;

#define REQ_OP_BITS 8
#define REQ_OP_MASK (__force blk_opf_t)((1 << REQ_OP_BITS) - 1)
#define REQ_FLAG_BITS 24

/**
 * enum req_op - Operations common to the bio and request structures.
 * We use 8 bits for encoding the operation, and the remaining 24 for flags.
 *
 * The least significant bit of the operation number indicates the data
 * transfer direction:
 *
 *   - if the least significant bit is set transfers are TO the device
 *   - if the least significant bit is not set transfers are FROM the device
 *
 * If a operation does not transfer data the least significant bit has no
 * meaning.
 */
enum req_op {
	/* read sectors from the device */
	REQ_OP_READ = (__force blk_opf_t)0,
	/* write sectors to the device */
	REQ_OP_WRITE = (__force blk_opf_t)1,
	/* flush the volatile write cache */
	REQ_OP_FLUSH = (__force blk_opf_t)2,
	/* discard sectors */
	REQ_OP_DISCARD = (__force blk_opf_t)3,
	/* securely erase sectors */
	REQ_OP_SECURE_ERASE = (__force blk_opf_t)5,
	/* write the zero filled sector many times */
	REQ_OP_WRITE_ZEROES = (__force blk_opf_t)9,
	/* Open a zone */
	REQ_OP_ZONE_OPEN = (__force blk_opf_t)10,
	/* Close a zone */
	REQ_OP_ZONE_CLOSE = (__force blk_opf_t)11,
	/* Transition a zone to full */
	REQ_OP_ZONE_FINISH = (__force blk_opf_t)12,
	/* write data at the current zone write pointer */
	REQ_OP_ZONE_APPEND = (__force blk_opf_t)13,
	/* reset a zone write pointer */
	REQ_OP_ZONE_RESET = (__force blk_opf_t)15,
	/* reset all the zone present on the device */
	REQ_OP_ZONE_RESET_ALL = (__force blk_opf_t)17,

	/* Driver private requests */
	REQ_OP_DRV_IN = (__force blk_opf_t)34,
	REQ_OP_DRV_OUT = (__force blk_opf_t)35,

	REQ_OP_LAST = (__force blk_opf_t)36,
};

/*
 * Max number of pages a single BIO can cover in secure_daemon.
 *
 * NOTE:
 *  - Each entry is a PAGE_SIZE chunk backed by SHM.
 *  - For read I/O, nvme_rd_submit_bio() further splits a BIO into
 *    segments that respect g_max_nvme_max_io_size, so setting this
 *    larger than the NVMe max I/O size is safe.
 *  - Larger values reduce the number of BIOs and ILLUFS_IOCTL_RA_END
 *    calls for sequential readahead and can increase effective NVMe
 *    queue depth.
 */
#define BIO_MAX_VECS 64 // OPTIMIZE: 16 or 32 or 64?

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
 * @brief Block I/O request for a contiguous range of storage blocks.
 * It is a unit of thread parallelization. Namely, each thread is requested
 * to process one bio at a time.
 */
struct bio {
	// struct bio *bi_next; /* request queue link (bio list) */
	void (*end_io)(void *);
	baddr_t bi_start; // bi_iter.bi_blk_no
	unsigned short bi_vcnt; /* how many bio_vec's, nr_pages, [variables] */
	unsigned short bi_vtotal; /* total nr of bio_vecs, fixed after issue bio*/

	/* for read operations (page fault) */
	int daemon_fd;
	unsigned long page_index;
	union {
		kaddr_t ractl;
		kaddr_t folio;
	};

	blk_opf_t bi_opf;
	void *bi_private; // buffer_head
	struct bio_vec *bi_io_vec; /* the actual vec array */
	unsigned int flags;	/* bio flags (e.g., user-level RA) */
};

/* bio->flags bits */
#define BIO_FLAG_USER_RA	0x1u

struct bio *alloc_bio(unsigned short nr_vecs, blk_opf_t);
void free_bio(struct bio *);
void add_bvec(struct bio *, char *buf);
bool bio_full(struct bio *);
#endif
