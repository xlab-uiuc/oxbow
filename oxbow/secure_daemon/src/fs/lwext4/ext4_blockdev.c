/*
 * Copyright (c) 2013 Grzegorz Kostka (kostka.grzegorz@gmail.com)
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * - Redistributions of source code must retain the above copyright
 *   notice, this list of conditions and the following disclaimer.
 * - Redistributions in binary form must reproduce the above copyright
 *   notice, this list of conditions and the following disclaimer in the
 *   documentation and/or other materials provided with the distribution.
 * - The name of the author may not be used to endorse or promote products
 *   derived from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
 * OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
 * IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
 * NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
 * THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

/** @addtogroup lwext4
 * @{
 */
/**
 * @file  ext4_blockdev.c
 * @brief Block device module. (The bcache is removed for Oxbow file system.)
 */

#include <ext4_config.h>
#include <ext4_types.h>
#include <ext4_misc.h>
#include <ext4_errno.h>
#include <ext4_debug.h>

#include <ext4_blockdev.h>
#include <ext4_fs.h>
#include <ext4_journal.h>

#include <string.h>
#include <stdlib.h>

#include "buffer_head.h"
#include "oxbow_debug.h"

static __attribute__((unused)) void ext4_bdif_lock(struct ext4_blockdev *bdev)
{
	if (!bdev->bdif->lock)
		return;

	int r = bdev->bdif->lock(bdev);
	ext4_assert(r == EOK);
}

static __attribute__((unused)) void ext4_bdif_unlock(struct ext4_blockdev *bdev)
{
	if (!bdev->bdif->unlock)
		return;

	int r = bdev->bdif->unlock(bdev);
	ext4_assert(r == EOK);
}

static int ext4_bdif_bread(struct ext4_blockdev *bdev, void *buf,
			   uint64_t blk_id, uint32_t blk_cnt)
{
	// ext4_bdif_lock(bdev); // Not used.
	int r = bdev->bdif->bread(bdev, buf, blk_id, blk_cnt);
	bdev->bdif
		->bread_ctr++; // It might be incorrect. TODO: Require atomic operation.
	// ext4_bdif_unlock(bdev); // Not used.
	return r;
}

static int ext4_bdif_bwrite(struct ext4_blockdev *bdev, const void *buf,
			    uint64_t blk_id, uint32_t blk_cnt)
{
	// ext4_bdif_lock(bdev); // Not used.
	int r = bdev->bdif->bwrite(bdev, buf, blk_id, blk_cnt);
	bdev->bdif
		->bwrite_ctr++; // It might be incorrect. TODO: Require atomic operation.
	// ext4_bdif_unlock(bdev); // Not used.
	return r;
}

int ext4_block_init(struct ext4_blockdev *bdev)
{
	int rc;
	ext4_assert(bdev);
	ext4_assert(bdev->bdif);
	ext4_assert(bdev->bdif->open && bdev->bdif->close &&
		    bdev->bdif->bread && bdev->bdif->bwrite);

	if (bdev->bdif->ph_refctr) {
		bdev->bdif->ph_refctr++;
		return EOK;
	}

	/*Low level block init*/
	rc = bdev->bdif->open(bdev);
	if (rc != EOK)
		return rc;

	bdev->bdif->ph_refctr = 1;
	return EOK;
}

int ext4_block_bind_bcache(struct ext4_blockdev *bdev, struct ext4_bcache *bc)
{
	ext4_assert(bdev && bc);
	bdev->bc = bc;
	bc->bdev = bdev;
	return EOK;
}

void ext4_block_set_lb_size(struct ext4_blockdev *bdev, uint32_t lb_bsize)
{
	/*Logical block size has to be multiply of physical */
	ext4_assert(!(lb_bsize % bdev->bdif->ph_bsize));

	bdev->lg_bsize = lb_bsize;
	bdev->lg_bcnt = bdev->part_size / lb_bsize;
}

int ext4_block_fini(struct ext4_blockdev *bdev)
{
	ext4_assert(bdev);

	if (!bdev->bdif->ph_refctr)
		return EOK;

	bdev->bdif->ph_refctr--;
	if (bdev->bdif->ph_refctr)
		return EOK;

	/*Low level block fini*/
	return bdev->bdif->close(bdev);
}

int ext4_block_flush_buf(struct ext4_blockdev *bdev, struct ext4_buf *buf)
{
	int r;
	struct ext4_bcache *bc = bdev->bc;

	if (ext4_bcache_test_flag(buf, BC_DIRTY) &&
	    ext4_bcache_test_flag(buf, BC_UPTODATE)) {
		r = ext4_blocks_set_direct(bdev, buf->data, buf->lba, 1);
		if (r) {
			if (buf->end_write) {
				bc->dont_shake = true;
				buf->end_write(bc, buf, r, buf->end_write_arg);
				bc->dont_shake = false;
			}

			return r;
		}

		ext4_bcache_remove_dirty_node(bc, buf);
		ext4_bcache_clear_flag(buf, BC_DIRTY);
		if (buf->end_write) {
			bc->dont_shake = true;
			buf->end_write(bc, buf, r, buf->end_write_arg);
			bc->dont_shake = false;
		}
	}
	return EOK;
}

int ext4_block_flush_lba(struct ext4_blockdev *bdev, uint64_t lba)
{
	int r = EOK;
	struct ext4_buf *buf;
	struct ext4_block b;
	buf = ext4_bcache_find_get(bdev->bc, &b, lba);
	if (buf) {
		r = ext4_block_flush_buf(bdev, buf);
		ext4_bcache_free(bdev->bc, &b);
	}
	return r;
}

int ext4_block_cache_shake(struct ext4_blockdev *bdev)
{
	int r = EOK;
	struct ext4_buf *buf;
	if (bdev->bc->dont_shake)
		return EOK;

	bdev->bc->dont_shake = true;

	while (!RB_EMPTY(&bdev->bc->lru_root) &&
	       ext4_bcache_is_full(bdev->bc)) {
		buf = ext4_buf_lowest_lru(bdev->bc);
		ext4_assert(buf);
		if (ext4_bcache_test_flag(buf, BC_DIRTY)) {
			r = ext4_block_flush_buf(bdev, buf);
			if (r != EOK)
				break;
		}

		ext4_bcache_drop_buf(bdev->bc, buf);
	}
	bdev->bc->dont_shake = false;
	return r;
}

// XXX: Need to fix. Restore to the original definition.
int ext4_block_get_noread(struct ext4_blockdev *bdev, struct ext4_block *b,
			  uint64_t lba, struct list_head *head __unused)
{
	bool is_new;
	int r;

	ext4_assert(bdev && b);

	if (!bdev->bdif->ph_refctr) {
		log_error("[%s]", __func__);
		return EIO;
	}

	if (!(lba < bdev->lg_bcnt)) {
		log_error("[%s]", __func__);
		return ENXIO;
	}

	b->lb_id = lba;

	r = ext4_bcache_alloc(bdev->bc, b, &is_new);
	if (r != EOK)
		return r;

	if (!b->data)
		return ENOMEM;

	return EOK;
}

int ext4_block_get(struct ext4_blockdev *bdev, struct ext4_block *b,
		   uint64_t lba, struct list_head *head __unused)
{
	int r;
	struct buffer_head *bh;

	// Read cache first. Alloc one, if cache miss.
	r = ext4_block_get_noread(bdev, b, lba, head);
	if (r != EOK) {
		log_error("[%s]", __func__);
		return r;
	}

	bh = b->buf;
	if (!buffer_new(bh)) {
		/* Buffer is from cache.
		 * Reading from physical device is not required */
		return EOK;
	}

	// Get data from disk.
	r = ext4_bcache_fill_buffer(bh);
	// r = ext4_blocks_get_direct(bdev, b->data, lba, 1);
	if (r != EOK) {
		fs_error("get block failed. lba: %llu", lba);
		b->lb_id = 0;
		return r;
	}

	return EOK;
}

//  In Oxbow, it makes the buffer dirty and return.
int ext4_block_set(struct ext4_blockdev *bdev, struct ext4_block *b)
{
	int r;
	struct buffer_head *bh;

	ext4_assert(bdev && b);
	ext4_assert(b->buf);

	bh = b->buf;

	if (!bdev->bdif->ph_refctr)
		return EIO;

	if (buffer_dirty(bh)) {
		// OPTIMIZE: If the following function only makes bh dirty, we
		// don't need to even call it.

		// Pass NULL as a buffer because data is in the buffer.
		r = ext4_blocks_set_direct(bdev, NULL, b->lb_id, 1);
		if (r != EOK) {
			fs_error("set block failed. lba=%llu pba=%luu",
				 b->lb_id, bh->b_blocknr);
			return r;
		}
	}

	// Put a buffer that was gotten in ext4_block_get.
	// TOCHECK: brelse here generates double freeing. Where is the right location?
	// brelse(bh);

	// TOCHECK: Is it correct?
	// Reset the ext4 buffer.
	// (Done in the ext4_bcache_free in original lwext4 source code.)
	b->lb_id = 0;
	b->data = 0;

	return EOK;

	// return ext4_bcache_free(bdev->bc, b);
}

int ext4_blocks_get_direct(struct ext4_blockdev *bdev, void *buf, uint64_t lba,
			   uint32_t cnt)
{
	uint64_t pba;
	uint32_t pb_cnt;

	ext4_assert(bdev && buf);

	pba = (lba * bdev->lg_bsize + bdev->part_offset) / bdev->bdif->ph_bsize;
	pb_cnt = bdev->lg_bsize / bdev->bdif->ph_bsize;

	return ext4_bdif_bread(bdev, buf, pba, pb_cnt * cnt);
}

int ext4_blocks_set_direct(struct ext4_blockdev *bdev, const void *buf,
			   uint64_t lba, uint32_t cnt)
{
	uint64_t pba;
	uint32_t pb_cnt;

	ext4_assert(bdev);

	pba = (lba * bdev->lg_bsize + bdev->part_offset) / bdev->bdif->ph_bsize;
	pb_cnt = bdev->lg_bsize / bdev->bdif->ph_bsize;

	fs_trace("[%s] lba(%d) -> pba(%d) cnt(%d)", __func__, lba, pba, cnt);

	return ext4_bdif_bwrite(bdev, buf, pba, pb_cnt * cnt);
}

int ext4_block_writebytes(struct ext4_blockdev *bdev, uint64_t off,
			  const void *buf, uint32_t len)
{
	uint64_t block_idx;
	uint32_t blen;
	uint32_t unalg;
	int r = EOK;

	const uint8_t *p = (void *)buf;

	ext4_assert(bdev && buf);

	if (!bdev->bdif->ph_refctr)
		return EIO;

	if (off + len > bdev->part_size)
		return EINVAL; /*Ups. Out of range operation*/

	block_idx = ((off + bdev->part_offset) / bdev->bdif->ph_bsize);

	/*OK lets deal with the first possible unaligned block*/
	unalg = (off & (bdev->bdif->ph_bsize - 1));
	if (unalg) {
		uint32_t wlen = (bdev->bdif->ph_bsize - unalg) > len ?
					len :
					(bdev->bdif->ph_bsize - unalg);

		r = ext4_bdif_bread(bdev, bdev->bdif->ph_bbuf, block_idx, 1);
		if (r != EOK)
			return r;

		memcpy(bdev->bdif->ph_bbuf + unalg, p, wlen);
		r = ext4_bdif_bwrite(bdev, bdev->bdif->ph_bbuf, block_idx, 1);
		if (r != EOK)
			return r;

		p += wlen;
		len -= wlen;
		block_idx++;
	}

	/*Aligned data*/
	blen = len / bdev->bdif->ph_bsize;
	if (blen != 0) {
		r = ext4_bdif_bwrite(bdev, p, block_idx, blen);
		if (r != EOK)
			return r;

		p += bdev->bdif->ph_bsize * blen;
		len -= bdev->bdif->ph_bsize * blen;

		block_idx += blen;
	}

	/*Rest of the data*/
	if (len) {
		r = ext4_bdif_bread(bdev, bdev->bdif->ph_bbuf, block_idx, 1);
		if (r != EOK)
			return r;

		memcpy(bdev->bdif->ph_bbuf, p, len);
		r = ext4_bdif_bwrite(bdev, bdev->bdif->ph_bbuf, block_idx, 1);
		if (r != EOK)
			return r;
	}

	return r;
}

int ext4_block_readbytes(struct ext4_blockdev *bdev, uint64_t off, void *buf,
			 uint32_t len)
{
	uint64_t block_idx;
	uint32_t blen;
	uint32_t unalg;
	int r = EOK;

	uint8_t *p = (void *)buf;

	ext4_assert(bdev && buf);

	if (!bdev->bdif->ph_refctr)
		return EIO;

	// We do not check boundary for the first superblock load. At that time
	// part_size is not set yet.
	if (off != EXT4_SUPERBLOCK_OFFSET && off + len > bdev->part_size)
		return EINVAL; /*Ups. Out of range operation*/

	block_idx = ((off + bdev->part_offset) / bdev->bdif->ph_bsize);

	/*OK lets deal with the first possible unaligned block*/
	unalg = (off & (bdev->bdif->ph_bsize - 1));
	if (unalg) {
		uint32_t rlen = (bdev->bdif->ph_bsize - unalg) > len ?
					len :
					(bdev->bdif->ph_bsize - unalg);

		r = ext4_bdif_bread(bdev, bdev->bdif->ph_bbuf, block_idx, 1);
		if (r != EOK)
			return r;

		memcpy(p, bdev->bdif->ph_bbuf + unalg, rlen);

		p += rlen;
		len -= rlen;
		block_idx++;
	}

	/*Aligned data*/
	blen = len / bdev->bdif->ph_bsize;

	if (blen != 0) {
		r = ext4_bdif_bread(bdev, p, block_idx, blen);
		if (r != EOK)
			return r;

		p += bdev->bdif->ph_bsize * blen;
		len -= bdev->bdif->ph_bsize * blen;

		block_idx += blen;
	}

	/*Rest of the data*/
	if (len) {
		r = ext4_bdif_bread(bdev, bdev->bdif->ph_bbuf, block_idx, 1);
		if (r != EOK)
			return r;

		memcpy(p, bdev->bdif->ph_bbuf, len);
	}

	return r;
}

int ext4_block_cache_flush(struct ext4_blockdev *bdev)
{
	while (!SLIST_EMPTY(&bdev->bc->dirty_list)) {
		int r;
		struct ext4_buf *buf = SLIST_FIRST(&bdev->bc->dirty_list);
		ext4_assert(buf);
		r = ext4_block_flush_buf(bdev, buf);
		if (r != EOK)
			return r;
	}
	return EOK;
}

int ext4_block_cache_write_back(struct ext4_blockdev *bdev, uint8_t on_off)
{
	if (on_off)
		bdev->cache_write_back++;

	if (!on_off && bdev->cache_write_back)
		bdev->cache_write_back--;

	if (bdev->cache_write_back)
		return EOK;

	/*Flush data in all delayed cache blocks*/
	return ext4_block_cache_flush(bdev);
}

/**
 * @}
 */
