/*
 * Copyright (c) 2015 Grzegorz Kostka (kostka.grzegorz@gmail.com)
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

#include <ext4_config.h>
#include <ext4_blockdev.h>
#include <ext4_errno.h>

#include "oxbow_debug.h"
#include "buffer_head.h"

/**********************BLOCKDEV INTERFACE**************************************/
static int blockdev_open(struct ext4_blockdev *bdev);
static int blockdev_bread(struct ext4_blockdev *bdev, void *buf,
			  uint64_t blk_id, uint32_t blk_cnt);
static int blockdev_bwrite(struct ext4_blockdev *bdev, const void *buf,
			   uint64_t blk_id, uint32_t blk_cnt);
static int blockdev_close(struct ext4_blockdev *bdev);
static int blockdev_lock(struct ext4_blockdev *bdev);
static int blockdev_unlock(struct ext4_blockdev *bdev);

/******************************************************************************/
// EXT4_BLOCKDEV_STATIC_INSTANCE(blockdev, OXBOW_BLOCK_SIZE, 0, blockdev_open,
// 			      blockdev_bread, blockdev_bwrite, blockdev_close,
// 			      blockdev_lock, blockdev_unlock);
//
// Expand macro for easy debugging. NOTE: To change, modify the above macro and
// regenerate it.
static uint8_t blockdev_ph_bbuf[((4096UL))];
static struct ext4_blockdev_iface blockdev_iface = {
	.open = blockdev_open,
	.bread = blockdev_bread,
	.bwrite = blockdev_bwrite,
	.close = blockdev_close,
	// .lock = blockdev_lock,
	.lock = 0, // TOCHECK: Not used?
	// .unlock = blockdev_unlock,
	.unlock = 0, // TOCHECK: Not used?
	.ph_bsize = (4096UL),
	.ph_bcnt = 0,
	.ph_bbuf = blockdev_ph_bbuf,
};
static struct ext4_blockdev blockdev = {
	.bdif = &blockdev_iface,
	.part_offset = 0,
	.part_size = (0) * ((4096UL)),
};

struct ext4_blockdev *g_bd = &blockdev;

/******************************************************************************/
static int blockdev_open(struct ext4_blockdev *bdev)
{
	// Do nothing. Block layer is initialized in the Oxbow's Secure Daemon.
	// Also, some variables are initialized after filling the super block by
	// ext4_block_set_info() function.
	blockdev.part_offset = 0;

	return EOK;
}

/******************************************************************************/

static int blockdev_bread(struct ext4_blockdev *bdev, void *buf,
			  uint64_t blk_id, uint32_t blk_cnt)
{
	// Do buffered read.
	struct buffer_head *bh;

	// TOCHECK: Is there any request for multiple blocks?
	oxbow_assert(blk_cnt == 1);

	// FIXME: Passing global instance (g_bdev). Oxbow supports only one
	// super block.
	bh = sb_bread(g_bdev->sb, blk_id);
	if (!bh)
		return ENOMEM;

	// Copy data to the buffer. The src data is cached (buffer cache).
	memcpy(buf, bh->b_data, bdev->bdif->ph_bsize);

	// TOCHECK: Is it efficient? Or, do we need to release is later in the
	// ext4 file system layer?
	brelse(bh);

	return EOK;
}

/******************************************************************************/
static int blockdev_bwrite(struct ext4_blockdev *bdev, const void *buf,
			   uint64_t blk_id, uint32_t blk_cnt)
{
	// Make a buffer dirty. (Write to the cache and do not flush.)
	// Note that, all data writes are done in the LibFS.
	int ret;

	// TOCHECK: Is there any request for multiple blocks?
	oxbow_assert(blk_cnt == 1);

	// FIXME: Passing global instance (g_bdev). Oxbow supports only one
	// super block.
	ret = sb_bwrite(g_bdev->sb, buf, blk_id);
	if (ret != 0) {
		fs_error("sb_write failed");
		return EIO;
	}

	return EOK;
}

/******************************************************************************/
static int blockdev_close(struct ext4_blockdev *bdev)
{
	// Do nothing. Block layer is destroyed in the Oxbow's Secure Daemon.
	// return EIO;
	return EOK;
}

static int blockdev_lock(struct ext4_blockdev *bdev)
{
	/*blockdev_lock: skeleton*/
	return EIO;
}

static int blockdev_unlock(struct ext4_blockdev *bdev)
{
	/*blockdev_unlock: skeleton*/
	return EIO;
}

/******************************************************************************/
struct ext4_blockdev *blockdev_get(void)
{
	return &blockdev;
}
/******************************************************************************/

void ext4_block_set_info(uint64_t block_count)
{
	blockdev.bdif->ph_bcnt = block_count;
	blockdev.part_size = block_count * blockdev.bdif->ph_bsize;
}

static uint64_t ext4_lba2pba(struct ext4_blockdev *bdev, uint64_t lba)
{
	uint64_t pba;

	pba = (lba * bdev->lg_bsize + bdev->part_offset) / bdev->bdif->ph_bsize;

	return pba;
}

struct buffer_head *blockdev_bread_noread(struct ext4_blockdev *bdev,
					  uint64_t lba, bool *is_new)
{
	return sb_balloc(g_bdev->sb, ext4_lba2pba(bdev, lba));
}
