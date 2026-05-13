/*
 * Copyright (c) 2013 Grzegorz Kostka (kostka.grzegorz@gmail.com)
 *
 *
 * HelenOS:
 * Copyright (c) 2012 Martin Sucha
 * Copyright (c) 2012 Frantisek Princ
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
 * @file  ext4_super.h
 * @brief Superblock operations.
 */

#include "ext4_inode.h"
#include <pthread.h>

#include <ext4_config.h>
#include <ext4_types.h>
#include <ext4_misc.h>
#include <ext4_errno.h>
#include <ext4_debug.h>

#include <ext4_super.h>
#include <ext4_crc32.h>
#include <ext4_fs.h> // due to ext4_iget()
#include <ext4_blockdev.h> // due to ext4_set_blockdev_info()

#include "buffer_head.h"
#include "oxbow_debug.h"
#include "config.h"
#include "journal.h"
#include <time.h>

uint32_t ext4_block_group_cnt(struct ext4_sblock *s)
{
	uint64_t blocks_count = ext4_sb_get_blocks_cnt(s);
	uint32_t blocks_per_group = ext4_get32(s, blocks_per_group);

	uint32_t block_groups_count =
		(uint32_t)(blocks_count / blocks_per_group);

	if (blocks_count % blocks_per_group)
		block_groups_count++;

	return block_groups_count;
}

uint32_t ext4_blocks_in_group_cnt(struct ext4_sblock *s, uint32_t bgid)
{
	uint32_t block_group_count = ext4_block_group_cnt(s);
	uint32_t blocks_per_group = ext4_get32(s, blocks_per_group);
	uint64_t total_blocks = ext4_sb_get_blocks_cnt(s);

	if (bgid < block_group_count - 1)
		return blocks_per_group;

	return (uint32_t)(total_blocks -
			  ((block_group_count - 1) * blocks_per_group));
}

uint32_t ext4_inodes_in_group_cnt(struct ext4_sblock *s, uint32_t bgid)
{
	uint32_t block_group_count = ext4_block_group_cnt(s);
	uint32_t inodes_per_group = ext4_get32(s, inodes_per_group);
	uint32_t total_inodes = ext4_get32(s, inodes_count);

	if (bgid < block_group_count - 1)
		return inodes_per_group;

	return (total_inodes - ((block_group_count - 1) * inodes_per_group));
}

#if CONFIG_META_CSUM_ENABLE
static uint32_t ext4_sb_csum(struct ext4_sblock *s)
{
	return ext4_crc32c(EXT4_CRC32_INIT, s,
			   offsetof(struct ext4_sblock, checksum));
}
#else
#define ext4_sb_csum(...) 0
#endif

static bool ext4_sb_verify_csum(struct ext4_sblock *s)
{
	if (!ext4_sb_feature_ro_com(s, EXT4_FRO_COM_METADATA_CSUM))
		return true;

	if (s->checksum_type != to_le32(EXT4_CHECKSUM_CRC32C))
		return false;

	return s->checksum == to_le32(ext4_sb_csum(s));
}

static void ext4_sb_set_csum(struct ext4_sblock *s)
{
	if (!ext4_sb_feature_ro_com(s, EXT4_FRO_COM_METADATA_CSUM))
		return;

	s->checksum = to_le32(ext4_sb_csum(s));
}

int ext4_sb_write(struct ext4_blockdev *bdev, struct ext4_sblock *s)
{
	ext4_sb_set_csum(s);

	ext4_fs_rdlock(g_bdev);

	return ext4_block_writebytes(bdev, EXT4_SUPERBLOCK_OFFSET, s,
				     EXT4_SUPERBLOCK_SIZE);

	ext4_fs_unlock(g_bdev);
}

int ext4_sb_read(struct ext4_blockdev *bdev, struct ext4_sblock *s)
{
	return ext4_block_readbytes(bdev, EXT4_SUPERBLOCK_OFFSET, s,
				    EXT4_SUPERBLOCK_SIZE);
}

bool ext4_sb_check(struct ext4_sblock *s)
{
	if (ext4_get16(s, magic) != EXT4_SUPERBLOCK_MAGIC)
		return false;

	if (ext4_get32(s, inodes_count) == 0)
		return false;

	if (ext4_sb_get_blocks_cnt(s) == 0)
		return false;

	if (ext4_get32(s, blocks_per_group) == 0)
		return false;

	if (ext4_get32(s, inodes_per_group) == 0)
		return false;

	if (ext4_get16(s, inode_size) < 128)
		return false;

	if (ext4_get32(s, first_inode) < 11)
		return false;

	if (ext4_sb_get_desc_size(s) < EXT4_MIN_BLOCK_GROUP_DESCRIPTOR_SIZE)
		return false;

	if (ext4_sb_get_desc_size(s) > EXT4_MAX_BLOCK_GROUP_DESCRIPTOR_SIZE)
		return false;

	if (!ext4_sb_verify_csum(s))
		return false;

	return true;
}

static inline int is_power_of(uint32_t a, uint32_t b)
{
	while (1) {
		if (a < b)
			return 0;
		if (a == b)
			return 1;
		if ((a % b) != 0)
			return 0;
		a = a / b;
	}
}

bool ext4_sb_sparse(uint32_t group)
{
	if (group <= 1)
		return 1;

	if (!(group & 1))
		return 0;

	return (is_power_of(group, 7) || is_power_of(group, 5) ||
		is_power_of(group, 3));
}

bool ext4_sb_is_super_in_bg(struct ext4_sblock *s, uint32_t group)
{
	if (ext4_sb_feature_ro_com(s, EXT4_FRO_COM_SPARSE_SUPER) &&
	    !ext4_sb_sparse(group))
		return false;
	return true;
}

static uint32_t ext4_bg_num_gdb_meta(struct ext4_sblock *s, uint32_t group)
{
	uint32_t dsc_per_block =
		ext4_sb_get_block_size(s) / ext4_sb_get_desc_size(s);

	uint32_t metagroup = group / dsc_per_block;
	uint32_t first = metagroup * dsc_per_block;
	uint32_t last = first + dsc_per_block - 1;

	if (group == first || group == first + 1 || group == last)
		return 1;
	return 0;
}

static uint32_t ext4_bg_num_gdb_nometa(struct ext4_sblock *s, uint32_t group)
{
	if (!ext4_sb_is_super_in_bg(s, group))
		return 0;
	uint32_t dsc_per_block =
		ext4_sb_get_block_size(s) / ext4_sb_get_desc_size(s);

	uint32_t db_count =
		(ext4_block_group_cnt(s) + dsc_per_block - 1) / dsc_per_block;

	if (ext4_sb_feature_incom(s, EXT4_FINCOM_META_BG))
		return ext4_sb_first_meta_bg(s);

	return db_count;
}

uint32_t ext4_bg_num_gdb(struct ext4_sblock *s, uint32_t group)
{
	uint32_t dsc_per_block =
		ext4_sb_get_block_size(s) / ext4_sb_get_desc_size(s);
	uint32_t first_meta_bg = ext4_sb_first_meta_bg(s);
	uint32_t metagroup = group / dsc_per_block;

	if (!ext4_sb_feature_incom(s, EXT4_FINCOM_META_BG) ||
	    metagroup < first_meta_bg)
		return ext4_bg_num_gdb_nometa(s, group);

	return ext4_bg_num_gdb_meta(s, group);
}

uint32_t ext4_num_base_meta_clusters(struct ext4_sblock *s,
				     uint32_t block_group)
{
	uint32_t num;
	uint32_t dsc_per_block =
		ext4_sb_get_block_size(s) / ext4_sb_get_desc_size(s);

	num = ext4_sb_is_super_in_bg(s, block_group);

	if (!ext4_sb_feature_incom(s, EXT4_FINCOM_META_BG) ||
	    block_group < ext4_sb_first_meta_bg(s) * dsc_per_block) {
		if (num) {
			num += ext4_bg_num_gdb(s, block_group);
			num += ext4_get16(s, s_reserved_gdt_blocks);
		}
	} else {
		num += ext4_bg_num_gdb(s, block_group);
	}

	uint32_t clustersize = 1024 << ext4_get32(s, log_cluster_size);
	uint32_t cluster_ratio = clustersize / ext4_sb_get_block_size(s);
	uint32_t v =
		(num + cluster_ratio - 1) >> ext4_get32(s, log_cluster_size);

	return v;
}

// Allocate struct ext4_inode_ref and initialize it.
struct inode *ext4_alloc_inode(struct super_block *sb)
{
	UNUSED(sb);

	d_trace("[%s]", __func__);

	struct ext4_inode_ref *ci = malloc(sizeof(struct ext4_inode_ref));
	if (!ci)
		return NULL;

	// Fill generic inode info.
	memset(ci, 0, sizeof(struct ext4_inode_ref));
	inode_init_always(sb, &ci->vfs_inode);
	ci->vfs_inode.free_inode = sb->s_op->free_inode;

	return &ci->vfs_inode;
}

/* it must destroy the inode  TODO; */
void ext4_destroy_inode(struct inode *inode)
{
	struct ext4_inode_ref *ci = EXT4_INODE(inode);
	i_debug("[%s] inode(%d)", __func__, inode->i_ino);
#ifdef OXBOW_USER_READAHEAD
	if (inode->ra_cache_buf)
		free(inode->ra_cache_buf);
	if (inode->ra_cache_index)
		free(inode->ra_cache_index);
	if (inode->ra_cache_valid)
		free(inode->ra_cache_valid);
#endif /* OXBOW_USER_READAHEAD */
	free(inode->i_mapping);
	free(ci);
}

void ext4_free_inode(struct inode *inode)
{
	struct ext4_inode_ref *ci = EXT4_INODE(inode);
	int ret;
	i_debug("[%s] inode(%d)", __func__, inode->i_ino);
	ret = ext4_fs_free_inode(ci);
	if (ret != EOK) {
		fs_error("Free inode failed. ret=%d", ret);
	}
}

static int ext4_write_inode(struct inode *inode)
{
	struct ext4_inode *disk_inode;
	// struct buffer_head *bh;
	// uint64_t ino = inode->i_ino;
	struct ext4_inode_ref *new_ref;
	// struct ext4_inode_ref old_ref;
	struct ext4_sblock *sb;
	int ret = -1;
	struct buffer_head *bh;

	fs_debug("[%s] inode(%d)", __func__, inode->i_ino);

	new_ref = EXT4_INODE(inode);
	sb = &new_ref->fs->sb;
	// bh = new_ref->block.buf;

	/* Read inode from disk and initialize */
	// It reads inode block from the ssd.
	// TOCHECK: Shouldn't there be the inode block in the buffer cache? Any
	// cases that the inode block is not in the buffer cache?
	// ret = ext4_fs_get_inode_ref(new_ref->fs, ino, &old_ref);
	// if (ret != EOK) {
	// 	fs_error("failed to get inode ref (read inode from disk). ret=%d", ret);
	// 	return -EIO;
	// }

	disk_inode = (*new_ref).inode;
	disk_inode->mode = mode_k2e(inode->i_mode);
	disk_inode->uid = inode->i_uid;
	disk_inode->gid = inode->i_gid;
	ext4_inode_set_size(disk_inode, inode->i_size);
	ext4_inode_set_blocks_count(sb, disk_inode, inode->i_blocks);
	disk_inode->links_count = inode->i_nlink;

	// TOCHECK: Do we need to update times as in the ext4_iget?

	// Update checksum, mark buffer dirty, and put inode ref.
	// FIXME: Do we need this?
	ret = ext4_fs_put_inode_ref(new_ref);
	if (ret != EOK) {
		fs_error(
			"failed to put inode (write inode to disk/journal). ret=%d",
			ret);
	}

	// Flush changes to ssd (or journal).
	bh = (struct buffer_head *)new_ref->block.buf;

	/* If using journal, do not directly store to disk. */
	if (inode->i_state & I_USE_JOURNAL) {
		if (!buffer_injournal(bh)) {
			oxb_debug(
				"[%s] inode=%u blknr=%llu journal=on (add to journal)",
				__func__, inode->i_ino, bh->b_blocknr);
			ret = add_journal_inode(bh);
		} else
			ret = 0;
	} else {
		d_debug("[%s] inode=%u blknr=%llu journal=off (sync directly)",
			__func__, inode->i_ino, bh->b_blocknr);
		mark_buffer_dirty(bh);
		ret = sync_dirty_buffer(bh);
	}

	inode->i_state &= ~I_DIRTY;

	return ret;
}

static int ext4_stage_inode(struct inode *inode,
			    struct journal_stage_header *jsh)
{
	struct ext4_inode *dst_inode, *src_inode;
	uint64_t ino = inode->i_ino;
	struct ext4_sblock *sb;
	baddr_t inode_baddr;
	uint32_t inode_offset;

	src_inode = EXT4_INODE(inode)->inode;

	dst_inode = (struct ext4_inode *)&jsh->inode;

	// Copy stale inode first.
	memcpy(dst_inode, src_inode, sizeof(struct ext4_inode));

	if (sizeof(*dst_inode) > 256) {
		oxb_error("stage support size over");
		return -1;
	}

	sb = &EXT4_INODE(inode)->fs->sb;

	// Update inode info with generic inode.
	dst_inode->mode = mode_k2e(inode->i_mode);
	dst_inode->uid = inode->i_uid;
	dst_inode->gid = inode->i_gid;
	ext4_inode_set_size(dst_inode, inode->i_size);
	ext4_inode_set_blocks_count(sb, dst_inode, inode->i_blocks);
	dst_inode->links_count =
		inode->i_nlink; // Why commented out in sefs_stage_inode()?

	// inode block number and offset
	ext4_fs_get_inode_blk_addr_offset(EXT4_INODE(inode)->fs, ino,
					  &inode_baddr, &inode_offset);
	jsh->inode_baddr = inode_baddr;
	jsh->inode_index = inode_offset;
	jsh->inode_size = EXT4_INODE_SIZE;

	stg_debug(
		"[%s] (ino=%u) inode_baddr: %lu, inode_index: %u inode_size: %u",
		__func__, inode->i_ino, jsh->inode_baddr, jsh->inode_index,
		jsh->inode_size);

	return 0;
}

static int ext4_fill_journal_sb(struct super_block *sb)
{
	struct journal_control_ctx *j;
	struct ext4_sblock *esb = EXT4_SB(sb);
	struct buffer_head *bh;
	struct journal_superblock *journal_sb;
	struct staging_superblock *stage_sb;

	j = sb->journal;
	bh = sb_bread(sb, esb->ox_journal_sb);
	if (!bh) {
		d_debug("read fail");
		return -1;
	}
	journal_sb = (struct journal_superblock *)bh->b_data;

	j->j_transaction_sequence = journal_sb->tx_id;

	brelse(bh);

	bh = sb_bread(sb, esb->ox_staging_sb);
	if (!bh) {
		d_debug("read fail");
		return -1;
	}
	stage_sb = (struct staging_superblock *)bh->b_data;
	j->stage_start = stage_sb->start;
	j->stage_tail = stage_sb->start;
	j->stage_gap_start = 0;
	j->stage_end = stage_sb->end;
	j->stage_total = stage_sb->block_nr;
	atomic_init(&j->stg_ckpt_in_flight, false);

	fs_info("[Fill journal SB] stagesb start %u tail %u end %u total %u",
		j->stage_start, j->stage_tail, j->stage_end, j->stage_total);

	brelse(bh);
	return 0;
}

baddr_t ext4_get_stage_sb_baddr(void *superblock)
{
	struct ext4_sblock *info = (struct ext4_sblock *)superblock;

	return info->ox_staging_sb;
}

uint32_t ext4_get_nr_stage_log_blocks(void *superblock)
{
	struct ext4_sblock *info = (struct ext4_sblock *)superblock;

	// TOCHECK: Except stage super block.
	return info->ox_nr_staging_blocks - 1;
}

baddr_t ext4_get_fs_area_start_baddr(void *superblock)
{
	return OXBOW_SUPER_BLOCK_NR;  // It starts from 0.
}

uint32_t ext4_get_nr_fs_area_blocks(void *superblock)
{
	struct ext4_sblock *info = (struct ext4_sblock *)superblock;

	// journal area follows after the file system area.
	return info->ox_journal_sb - OXBOW_SUPER_BLOCK_NR;
}

const struct super_operations lwext4_super_ops = {
	.iget = ext4_iget,
	.alloc_inode = ext4_alloc_inode,
	.destroy_inode = ext4_destroy_inode,
	.free_inode = ext4_free_inode,
	.write_inode = ext4_write_inode,
	.stage_inode = ext4_stage_inode,
	.get_free_blocks = NULL,
	.get_free_inode = NULL,
	.put_blocks = NULL,
	.put_inode = NULL,
	// journal related
	.fill_journal_sb = ext4_fill_journal_sb,
	// retrieving metadata
	.get_stage_sb_baddr = ext4_get_stage_sb_baddr,
	.get_nr_stage_log_blocks = ext4_get_nr_stage_log_blocks,
	.get_fs_area_start_baddr = ext4_get_fs_area_start_baddr,
	.get_nr_fs_area_blocks = ext4_get_nr_fs_area_blocks,
};

static void ext4_set_blockdev_info(struct ext4_sblock *esb, uint64_t block_cnt)
{
	uint32_t bsize;

	ext4_block_set_info(block_cnt);

	bsize = ext4_sb_get_block_size(esb);
	ext4_block_set_lb_size(g_bd, bsize);
}

int ext4_fill_super(struct super_block *sb)
{
	struct buffer_head *bh;
	struct ext4_sblock *disk_sb, *sbi;
	struct inode *root;
	int ret = -1;
	struct ext4_sblock *esb;

	/** Fill Oxbow general superblock. **/
	/* init superblock */
	sb->s_magic = EXT4_SUPERBLOCK_MAGIC;
	sb->s_maxbytes =
		OXBOW_MAX_FILE_SIZE; // FIXME: It depends on the SEFS_FILE_MAX
	sb->inodes_per_blk = (OXBOW_BLOCK_SIZE / sizeof(struct ext4_inode));
	sb->filename_max = OXBOW_FILENAME_MAX;
	sb->max_subfiles = UINT_MAX; // FIXME: Not used?
	sb->s_op = &lwext4_super_ops;

	bh = sb_bread(sb, OXBOW_SUPER_BLOCK_NR);
	if (!bh) {
		log_error("failed to read superblock");
		return -EIO;
	}

	disk_sb = (struct ext4_sblock *)(bh->b_data + EXT4_SUPERBLOCK_OFFSET);
	if (disk_sb->magic != EXT4_SUPERBLOCK_MAGIC) {
		log_error("wrong magic number %lx", disk_sb->magic);
		goto release;
	}

	sbi = calloc(1, sizeof(struct ext4_sblock));
	if (!sbi) {
		log_error("out of memory for sb_info");
		goto release;
	}

	sb->s_fs_info = sbi;
	// Copy on-disk superblock to in-memory superblock.
	memcpy(sbi, disk_sb, sizeof(struct ext4_sblock));

	sb->nr_inodes = sbi->inodes_count;
	sb->nr_blocks = ext4_sb_get_blocks_cnt(sbi);
	sb->s_bh = bh;
	brelse(bh);

	// Set the block device info correctly with a superblock data.
	esb = EXT4_SB(sb);
	ext4_set_blockdev_info(esb, sb->nr_blocks);

#ifndef VM_ENV_NO_DEVFS
	if (g_sd_conf.bg_journaling) {
		/* sb->journal is allocated here */
		sb->journal = malloc(sizeof(*sb->journal));
		if (!sb->journal) {
			oxb_error("malloc fail");
			goto free_sbi;
		}
		if (init_journal(sb)) {
			oxb_error("journal init fail");
			goto free_sbi;
		}
	}
#endif

	/** Alloc root directory inode **/
	root = ext4_iget(sb, EXT4_ROOT_INO);
	if (!root)
		goto free_sbi;

	sb->s_root = root;

	ret = 0; /* success */
	goto release;

free_sbi:
	free(sbi);
	if (sb->journal)
		free(sb->journal);
release:
	return ret;
}

/**
 * @}
 */
