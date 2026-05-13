#include "io/sd_bio.h"
#include "oxbow_debug.h"
#include <sys/user.h>

/**
  * @brief Allocate a new bio. Set the start block number.
  * 
  * @param blk_no Start blk_no id.
  * @return struct bio* 
  */
struct bio *alloc_bio(unsigned short nr_vecs, blk_opf_t opf)
{
	struct bio *bio;

	if (nr_vecs > BIO_MAX_VECS) {
		oxb_error("too many: %d > %d", nr_vecs, BIO_MAX_VECS);
		return NULL;
	}

	// OPTIMIZE: Adopt slab allocator.
	bio = malloc(sizeof(struct bio));
	if (!bio) {
		perror("malloc fail");
		return NULL;
	}
	bio->bi_vtotal = 0;
	bio->bi_vcnt = 0;
	bio->daemon_fd = -1;
	bio->page_index = 0;
	bio->ractl = 0;
	bio->bi_start = 0;
	bio->bi_private = NULL;
	bio->bi_io_vec = NULL;
	bio->flags = 0;

	bio->bi_io_vec = malloc(nr_vecs * sizeof(struct bio_vec));
	if (!bio->bi_io_vec) {
		perror("malloc fail");
		free(bio);
		return NULL;
	}

	bio->bi_opf = opf;

	return bio;
}

void free_bio(struct bio *bio)
{
	free(bio);
}

void add_bvec(struct bio *bio, char *buf)
{
	// oxb_debug("bio(%p) buf: 0x%lx, bi_vcnt:%d", bio, buf, bio->bi_vtotal);
	oxbow_assert(bio->bi_vtotal < BIO_MAX_VECS);
	bio->bi_io_vec[bio->bi_vtotal].bv_buf = buf;
	bio->bi_vtotal++;
}

bool bio_full(struct bio *bio)
{
	return bio->bi_vtotal == BIO_MAX_VECS;
}
