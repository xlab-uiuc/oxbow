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
 * @file  ext4_inode.c
 * @brief Inode handle functions
 */

#include <ext4_config.h>
#include <ext4_types.h>
#include <ext4_misc.h>
#include <ext4_errno.h>
#include <ext4_debug.h>

#include <ext4_inode.h>
#include <ext4_super.h>

#include <ext4_fs.h>
#include <ext4_dir.h>

#include <ext4.h>

#include "fs.h"
#include "oxbow_debug.h"

#include <stdio.h>
#include <ext4_types.h>
#include <ext4_fs.h>
#include <ext4_blockdev.h>
#include <ext4_trans.h>
#include <string.h>

/**
 * @brief Print the contents of an inode block containing 4 inodes.
 * 
 * @param fs Pointer to the filesystem structure.
 * @param block_num Physical block number to print.
 * @return int Standard error code.
 */
int print_inode_block(ext4_fsblk_t block_num)
{
	struct ext4_block block;
	struct ext4_fs *fs = g_fs;
	int rc;
	uint16_t inode_size = ext4_get16(&fs->sb, inode_size);
	uint32_t inode_count = EXT4_INODE_BLOCK_SIZE / inode_size;

	rc = ext4_trans_block_get(fs->bdev, &block, block_num, NULL);
	if (rc != EOK) {
		return rc;
	}

	printf("Inode Block at Physical Block Number: %" PRIu64 " (0x%" PRIx64
	       ")\n",
	       block_num, block_num);

	for (uint32_t i = 0; i < inode_count; i++) {
		struct ext4_inode *inode =
			(struct ext4_inode *)((char *)block.data +
					      i * inode_size);

		printf("Inode %d:\n", i);
		printf("  Mode: %u (0x%x)\n", inode->mode, inode->mode);
		printf("  UID: %u (0x%x)\n", inode->uid, inode->uid);
		printf("  Size (low): %u (0x%x)\n", inode->size_lo,
		       inode->size_lo);
		printf("  Access Time: %u (0x%x)\n", inode->access_time,
		       inode->access_time);
		printf("  Change Inode Time: %u (0x%x)\n",
		       inode->change_inode_time, inode->change_inode_time);
		printf("  Modification Time: %u (0x%x)\n",
		       inode->modification_time, inode->modification_time);
		printf("  Deletion Time: %u (0x%x)\n", inode->deletion_time,
		       inode->deletion_time);
		printf("  GID: %u (0x%x)\n", inode->gid, inode->gid);
		printf("  Links Count: %u (0x%x)\n", inode->links_count,
		       inode->links_count);
		printf("  Blocks Count (low): %u (0x%x)\n",
		       inode->blocks_count_lo, inode->blocks_count_lo);
		printf("  Flags: %u (0x%x)\n", inode->flags, inode->flags);
		printf("  Unused OSD1: %u (0x%x)\n", inode->unused_osd1,
		       inode->unused_osd1);

		for (int j = 0; j < EXT4_INODE_BLOCKS; j++) {
			printf("  Block Pointer[%d]: %u (0x%x)\n", j,
			       inode->blocks[j], inode->blocks[j]);
		}

		printf("  Generation: %u (0x%x)\n", inode->generation,
		       inode->generation);
		printf("  File ACL (low): %u (0x%x)\n", inode->file_acl_lo,
		       inode->file_acl_lo);
		printf("  Size (high): %u (0x%x)\n", inode->size_hi,
		       inode->size_hi);
		printf("  Obsoleted Fragment Address: %u (0x%x)\n",
		       inode->obso_faddr, inode->obso_faddr);

		printf("  Extra Inode Size: %u (0x%x)\n", inode->extra_isize,
		       inode->extra_isize);
		printf("  Checksum (high): %u (0x%x)\n", inode->checksum_hi,
		       inode->checksum_hi);
		printf("  Extra Change Time: %u (0x%x)\n", inode->ctime_extra,
		       inode->ctime_extra);
		printf("  Extra Modification Time: %u (0x%x)\n",
		       inode->mtime_extra, inode->mtime_extra);
		printf("  Extra Access Time: %u (0x%x)\n", inode->atime_extra,
		       inode->atime_extra);
		printf("  File Creation Time: %u (0x%x)\n", inode->crtime,
		       inode->crtime);
		printf("  Extra File Creation Time: %u (0x%x)\n",
		       inode->crtime_extra, inode->crtime_extra);
		printf("  Version (high): %u (0x%x)\n", inode->version_hi,
		       inode->version_hi);
		printf("\n");
	}

	rc = ext4_block_set(fs->bdev, &block);
	return rc;
}

void print_ext4_inode(struct ext4_inode_ref *inode_ref)
{
	struct ext4_inode *inode = inode_ref->inode;

	printf("\nlwext4 Inode(%d) Block:\n", inode_ref->index);

	// Print mode with file type
	uint16_t mode = inode->mode;
	printf("  Mode: 0x%x (", mode);
	if ((mode & EXT4_INODE_MODE_TYPE_MASK) == EXT4_INODE_MODE_FIFO)
		printf("FIFO");
	else if ((mode & EXT4_INODE_MODE_TYPE_MASK) == EXT4_INODE_MODE_CHARDEV)
		printf("Character Device");
	else if ((mode & EXT4_INODE_MODE_TYPE_MASK) ==
		 EXT4_INODE_MODE_DIRECTORY)
		printf("Directory");
	else if ((mode & EXT4_INODE_MODE_TYPE_MASK) == EXT4_INODE_MODE_BLOCKDEV)
		printf("Block Device");
	else if ((mode & EXT4_INODE_MODE_TYPE_MASK) == EXT4_INODE_MODE_FILE)
		printf("Regular File");
	else if ((mode & EXT4_INODE_MODE_TYPE_MASK) == EXT4_INODE_MODE_SOFTLINK)
		printf("Symbolic Link");
	else if ((mode & EXT4_INODE_MODE_TYPE_MASK) == EXT4_INODE_MODE_SOCKET)
		printf("Socket");
	else
		printf("Unknown");
	printf("), UID: %u, GID: %u, Links: %u\n", inode->uid, inode->gid,
	       inode->links_count);

	printf("  Size: %u|%u, Blocks: %u, Flags: 0x%x\n", inode->size_hi,
	       inode->size_lo, inode->blocks_count_lo, inode->flags);
	printf("  Times (a/c/m/d): %u/%u/%u/%u\n", inode->access_time,
	       inode->change_inode_time, inode->modification_time,
	       inode->deletion_time);

	printf("  Block Pointers:");
	for (int j = 0; j < EXT4_INODE_BLOCKS; j++) {
		printf(" %u", inode->blocks[j]);
	}
	printf("\n");

	printf("  Generation: %u, File ACL: %u\n", inode->generation,
	       inode->file_acl_lo);
	printf("  Extra - Size: %u, Checksum: %u\n", inode->extra_isize,
	       inode->checksum_hi);
	printf("  Extra Times (c/m/a): %u/%u/%u\n", inode->ctime_extra,
	       inode->mtime_extra, inode->atime_extra);
	printf("  Creation: %u (extra: %u), Version: %u\n", inode->crtime,
	       inode->crtime_extra, inode->version_hi);
}

/**@brief  Compute number of bits for block count.
 * @param block_size Filesystem block_size
 * @return Number of bits
 */
static uint32_t ext4_inode_block_bits_count(uint32_t block_size)
{
	uint32_t bits = 8;
	uint32_t size = block_size;

	do {
		bits++;
		size = size >> 1;
	} while (size > 256);

	return bits;
}

uint32_t ext4_inode_get_mode(struct ext4_sblock *sb, struct ext4_inode *inode)
{
	uint32_t v = to_le16(inode->mode);

	if (ext4_get32(sb, creator_os) == EXT4_SUPERBLOCK_OS_HURD) {
		v |= ((uint32_t)to_le16(inode->osd2.hurd2.mode_high)) << 16;
	}

	return v;
}

void ext4_inode_set_mode(struct ext4_sblock *sb, struct ext4_inode *inode,
			 uint32_t mode)
{
	inode->mode = to_le16((mode << 16) >> 16);

	if (ext4_get32(sb, creator_os) == EXT4_SUPERBLOCK_OS_HURD)
		inode->osd2.hurd2.mode_high = to_le16(mode >> 16);
}

uint32_t ext4_inode_get_uid(struct ext4_inode *inode)
{
	return to_le32(inode->uid);
}

void ext4_inode_set_uid(struct ext4_inode *inode, uint32_t uid)
{
	inode->uid = to_le32(uid);
}

uint64_t ext4_inode_get_size(struct ext4_sblock *sb, struct ext4_inode_ref *ref)
{
	struct ext4_inode *inode = ref->inode;
	struct inode *vfs_inode = &ref->vfs_inode;
	bool is_load; // Exclude when the data is loaded from ext4_inode to generic inode.

	uint64_t v = to_le32(inode->size_lo);

	if ((ext4_get32(sb, rev_level) > 0) &&
	    (ext4_inode_is_type(sb, inode, EXT4_INODE_MODE_FILE)))
		v |= ((uint64_t)to_le32(inode->size_hi)) << 32;

#ifndef MKFS
	// ext4's value may be outdated because vfs_inode's updates are not
	// applied to ext4's. (Oxbow)

	is_load = vfs_inode->i_size == 0; // TOCHECK: can be other than 0?

	if (!is_load && v != vfs_inode->i_size) {
		// Update ext4's value.
		ext4_inode_set_size(ref->inode, vfs_inode->i_size);
		v = vfs_inode->i_size;
	};
#endif

	return v;
}

void ext4_inode_set_size(struct ext4_inode *inode, uint64_t size)
{
	// struct ext4_inode *inode = ref->inode;
	// struct inode *vfs_inode = &ref->vfs_inode;

	inode->size_lo = to_le32((size << 32) >> 32);
	inode->size_hi = to_le32(size >> 32);

	// #ifndef MKFS
	// TOCHECK: Do we need to update generic inode's i_size? (Oxbow)
	// For regular file, the size must be changed already.
	// For directory, oxbow inode size is updated after calling this
	// function. So, we don't need to update generic inode's i_size here.
	//
	// vfs_inode->i_size = size;
	// illufs_set_i_size(vfs_inode);
	// #endif
}

uint32_t ext4_inode_get_csum(struct ext4_sblock *sb, struct ext4_inode *inode)
{
	uint16_t inode_size = ext4_get16(sb, inode_size);
	uint32_t v = to_le16(inode->osd2.linux2.checksum_lo);

	if (inode_size > EXT4_GOOD_OLD_INODE_SIZE)
		v |= ((uint32_t)to_le16(inode->checksum_hi)) << 16;

	return v;
}

void ext4_inode_set_csum(struct ext4_sblock *sb, struct ext4_inode *inode,
			 uint32_t checksum)
{
	uint16_t inode_size = ext4_get16(sb, inode_size);
	inode->osd2.linux2.checksum_lo = to_le16((checksum << 16) >> 16);

	if (inode_size > EXT4_GOOD_OLD_INODE_SIZE)
		inode->checksum_hi = to_le16(checksum >> 16);
}

uint32_t ext4_inode_get_access_time(struct ext4_inode *inode)
{
	return to_le32(inode->access_time);
}
void ext4_inode_set_access_time(struct ext4_inode *inode, uint32_t time)
{
	inode->access_time = to_le32(time);
}

uint32_t ext4_inode_get_change_inode_time(struct ext4_inode *inode)
{
	return to_le32(inode->change_inode_time);
}
void ext4_inode_set_change_inode_time(struct ext4_inode *inode, uint32_t time)
{
	inode->change_inode_time = to_le32(time);
}

uint32_t ext4_inode_get_modif_time(struct ext4_inode *inode)
{
	return to_le32(inode->modification_time);
}

void ext4_inode_set_modif_time(struct ext4_inode *inode, uint32_t time)
{
	inode->modification_time = to_le32(time);
}

uint32_t ext4_inode_get_del_time(struct ext4_inode *inode)
{
	return to_le32(inode->deletion_time);
}

void ext4_inode_set_del_time(struct ext4_inode *inode, uint32_t time)
{
	inode->deletion_time = to_le32(time);
}

uint32_t ext4_inode_get_gid(struct ext4_inode *inode)
{
	return to_le32(inode->gid);
}
void ext4_inode_set_gid(struct ext4_inode *inode, uint32_t gid)
{
	inode->gid = to_le32(gid);
}

uint16_t ext4_inode_get_links_cnt(struct ext4_inode *inode)
{
	return to_le16(inode->links_count);
}
void ext4_inode_set_links_cnt(struct ext4_inode *inode, uint16_t cnt)
{
	inode->links_count = to_le16(cnt);
}

uint64_t ext4_inode_get_blocks_count(struct ext4_sblock *sb,
				     struct ext4_inode *inode)
{
	uint64_t cnt = to_le32(inode->blocks_count_lo);

	if (ext4_sb_feature_ro_com(sb, EXT4_FRO_COM_HUGE_FILE)) {
		/* 48-bit field */
		cnt |= (uint64_t)to_le16(inode->osd2.linux2.blocks_high) << 32;

		if (ext4_inode_has_flag(inode, EXT4_INODE_FLAG_HUGE_FILE)) {
			uint32_t block_count = ext4_sb_get_block_size(sb);
			uint32_t b = ext4_inode_block_bits_count(block_count);
			return cnt << (b - 9);
		}
	}

	return cnt;
}

int ext4_inode_set_blocks_count(struct ext4_sblock *sb,
				struct ext4_inode *inode, uint64_t count)
{
	/* 32-bit maximum */
	uint64_t max = 0;
	max = ~max >> 32;

	if (count <= max) {
		inode->blocks_count_lo = to_le32((uint32_t)count);
		inode->osd2.linux2.blocks_high = 0;
		ext4_inode_clear_flag(inode, EXT4_INODE_FLAG_HUGE_FILE);

		return EOK;
	}

	/* Check if there can be used huge files (many blocks) */
	if (!ext4_sb_feature_ro_com(sb, EXT4_FRO_COM_HUGE_FILE))
		return EINVAL;

	/* 48-bit maximum */
	max = 0;
	max = ~max >> 16;

	if (count <= max) {
		inode->blocks_count_lo = to_le32((uint32_t)count);
		inode->osd2.linux2.blocks_high =
			to_le16((uint16_t)(count >> 32));
		ext4_inode_clear_flag(inode, EXT4_INODE_FLAG_HUGE_FILE);
	} else {
		uint32_t block_count = ext4_sb_get_block_size(sb);
		uint32_t block_bits = ext4_inode_block_bits_count(block_count);

		ext4_inode_set_flag(inode, EXT4_INODE_FLAG_HUGE_FILE);
		count = count >> (block_bits - 9);
		inode->blocks_count_lo = to_le32((uint32_t)count);
		inode->osd2.linux2.blocks_high =
			to_le16((uint16_t)(count >> 32));
	}

	return EOK;
}

uint32_t ext4_inode_get_flags(struct ext4_inode *inode)
{
	return to_le32(inode->flags);
}
void ext4_inode_set_flags(struct ext4_inode *inode, uint32_t flags)
{
	inode->flags = to_le32(flags);
}

uint32_t ext4_inode_get_generation(struct ext4_inode *inode)
{
	return to_le32(inode->generation);
}
void ext4_inode_set_generation(struct ext4_inode *inode, uint32_t gen)
{
	inode->generation = to_le32(gen);
}

uint16_t ext4_inode_get_extra_isize(struct ext4_sblock *sb,
				    struct ext4_inode *inode)
{
	uint16_t inode_size = ext4_get16(sb, inode_size);
	if (inode_size > EXT4_GOOD_OLD_INODE_SIZE)
		return to_le16(inode->extra_isize);
	else
		return 0;
}

void ext4_inode_set_extra_isize(struct ext4_sblock *sb,
				struct ext4_inode *inode, uint16_t size)
{
	uint16_t inode_size = ext4_get16(sb, inode_size);
	if (inode_size > EXT4_GOOD_OLD_INODE_SIZE)
		inode->extra_isize = to_le16(size);
}

uint64_t ext4_inode_get_file_acl(struct ext4_inode *inode,
				 struct ext4_sblock *sb)
{
	uint64_t v = to_le32(inode->file_acl_lo);

	if (ext4_get32(sb, creator_os) == EXT4_SUPERBLOCK_OS_LINUX)
		v |= (uint32_t)to_le16(inode->osd2.linux2.file_acl_high) << 16;

	return v;
}

void ext4_inode_set_file_acl(struct ext4_inode *inode, struct ext4_sblock *sb,
			     uint64_t acl)
{
	inode->file_acl_lo = to_le32((acl << 32) >> 32);

	if (ext4_get32(sb, creator_os) == EXT4_SUPERBLOCK_OS_LINUX)
		inode->osd2.linux2.file_acl_high =
			to_le16((uint16_t)(acl >> 32));
}

uint32_t ext4_inode_get_direct_block(struct ext4_inode *inode, uint32_t idx)
{
	return to_le32(inode->blocks[idx]);
}
void ext4_inode_set_direct_block(struct ext4_inode *inode, uint32_t idx,
				 uint32_t block)
{
	inode->blocks[idx] = to_le32(block);
}

uint32_t ext4_inode_get_indirect_block(struct ext4_inode *inode, uint32_t idx)
{
	return to_le32(inode->blocks[idx + EXT4_INODE_INDIRECT_BLOCK]);
}

void ext4_inode_set_indirect_block(struct ext4_inode *inode, uint32_t idx,
				   uint32_t block)
{
	inode->blocks[idx + EXT4_INODE_INDIRECT_BLOCK] = to_le32(block);
}

uint32_t ext4_inode_get_dev(struct ext4_inode *inode)
{
	uint32_t dev_0, dev_1;
	dev_0 = ext4_inode_get_direct_block(inode, 0);
	dev_1 = ext4_inode_get_direct_block(inode, 1);

	if (dev_0)
		return dev_0;
	else
		return dev_1;
}

void ext4_inode_set_dev(struct ext4_inode *inode, uint32_t dev)
{
	if (dev & ~0xFFFF)
		ext4_inode_set_direct_block(inode, 1, dev);
	else
		ext4_inode_set_direct_block(inode, 0, dev);
}

uint32_t ext4_inode_type(struct ext4_sblock *sb, struct ext4_inode *inode)
{
	return (ext4_inode_get_mode(sb, inode) & EXT4_INODE_MODE_TYPE_MASK);
}

bool ext4_inode_is_type(struct ext4_sblock *sb, struct ext4_inode *inode,
			uint32_t type)
{
	return ext4_inode_type(sb, inode) == type;
}

bool ext4_inode_has_flag(struct ext4_inode *inode, uint32_t f)
{
	return ext4_inode_get_flags(inode) & f;
}

void ext4_inode_clear_flag(struct ext4_inode *inode, uint32_t f)
{
	uint32_t flags = ext4_inode_get_flags(inode);
	flags = flags & (~f);
	ext4_inode_set_flags(inode, flags);
}

void ext4_inode_set_flag(struct ext4_inode *inode, uint32_t f)
{
	uint32_t flags = ext4_inode_get_flags(inode);
	flags = flags | f;
	ext4_inode_set_flags(inode, flags);
}

bool ext4_inode_can_truncate(struct ext4_sblock *sb, struct ext4_inode *inode)
{
	if ((ext4_inode_has_flag(inode, EXT4_INODE_FLAG_APPEND)) ||
	    (ext4_inode_has_flag(inode, EXT4_INODE_FLAG_IMMUTABLE)))
		return false;

	if ((ext4_inode_is_type(sb, inode, EXT4_INODE_MODE_FILE)) ||
	    (ext4_inode_is_type(sb, inode, EXT4_INODE_MODE_DIRECTORY)) ||
	    (ext4_inode_is_type(sb, inode, EXT4_INODE_MODE_SOFTLINK)))
		return true;

	return false;
}

struct ext4_extent_header *
ext4_inode_get_extent_header(struct ext4_inode *inode)
{
	return (struct ext4_extent_header *)inode->blocks;
}

/**
 * @brief 
 * 
 * @param dir Parent
 * @param fname Filename
 * @return struct inode* Inode of child.
 */
static struct inode *lwext4_lookup(struct inode *dir, const char *fname)
{
	struct ext4_dir_search_result result;
	struct ext4_inode_ref *ref;
	int ret;
	uint32_t child_inum;
	struct inode *child_ino;

	ref = EXT4_INODE(dir);
	ret = ext4_dir_find_entry(&result, ref, fname, strlen(fname));
	if (ret != EOK) {
		fs_debug("Lookup failed. ret=%d", ret);
		return NULL;
	}

	child_inum = ext4_dir_en_get_inode(result.dentry);
	child_ino = ext4_iget(dir->i_sb, child_inum);
	if (child_ino == NULL) {
		fs_error("Get inode failed. ino=%u ret=%d", child_inum, ret);
		return NULL;
	}

	return child_ino;
}

/**
 * @brief Set inode content and unlock it..
 * 
 * @param sb 
 * @param inode A new inode allocated in the caller.
 * @param mode 
 */
static int ext4_set_new_inode(struct super_block *sb, struct inode *inode,
			      mode_t mode)
{
	// struct buffer_head *bh;
	// unsigned long bno;
	struct ext4_sblock *esb;
	struct ext4_inode_ref *ref;

	/* Check mode before doing anything to avoid undoing everything */
	if (!S_ISDIR(mode) && !S_ISREG(mode) && !S_ISLNK(mode)) {
		log_error("File type not supported");
		return -EINVAL;
	}

	ref = EXT4_INODE(inode);

	/* Check if inodes are available */
	esb = EXT4_SB(sb);
	if (esb->free_inodes_count == 0 ||
	    ext4_sb_get_free_blocks_cnt(esb) == 0)
		return -ENOSPC;

	// /* Get a free block for this new inode's index */
	// bno = get_free_blocks(sb, 1);
	// if (!bno)
	// 	goto err;

	// /* Scrub ei_block for new file/directory  */
	// bh = sb_bread(sb, bno, &inode->i_private);
	// if (!bh)
	// 	goto put_block;

	// memset((char *)bh->b_data, 0, ext4_sb_get_block_size(esb));
	// set_buffer_dirty(bh); /* ei_block */
	// brelse(bh);

	/* Initialize inode */
	inode->i_blocks = 1;
	if (S_ISDIR(mode)) {
		inode->i_size = ext4_inode_get_size(esb, ref);
		inode->i_fop = &lwext4_dir_ops;
		inode->i_op = &lwext4_dir_inode_ops;
		inode->i_nlink = 2; /* due to . itself .. is for directory */
	} else if (S_ISREG(mode)) {
		inode->i_size = 0;
		inode->i_op = &lwext4_inode_ops;
		inode->i_mapping->a_ops = &lwext4_aops;
		inode->i_nlink = 1;
	} else
		oxb_error("unknown mode");

	/* daemon must know the mode */
	inode->i_mode = mode;
	// inode->i_ctime = inode->i_atime = inode->i_mtime = current_time(inode);

	unlock_new_inode(inode); // TOCHECK: Do we need it?
	return 0;

// put_block:
// 	put_blocks(sb, bno, 1);
err:
	// iput(inode);

	return -1;
}

/**
 * @brief Create a new directory entry. It assumes there is no entry with the same name.
 * 
 * @param dir 
 * @param ret_i Created inode is returned.
 * @param mode 
 * @param fname 
 * @return int 
 */
static int lwext4_create(struct inode *dir, struct inode **ret_i, mode_t mode,
			 const char *fname)
{
	struct inode *child_inode = NULL;
	struct super_block *sb;
	struct ext4_inode_ref *parent_ref;
	struct ext4_inode_ref *child_ref = NULL;
	int ret = -1;

	fs_trace("[%s] parent=%p (ino=%lu) mode=%d fname=%s", __func__, dir,
		 dir->i_ino, mode, fname);

	sb = dir->i_sb;
	parent_ref = EXT4_INODE(dir);

	/* ext4 allocation */
	ret = ext4_fs_alloc_inode_ox(g_fs, sb, &child_inode,
				     get_filetype(mode));
	if (ret != EOK) {
		fs_error("Allocate inode failed. ret=%d", ret);
		/*
		 * On failure, ext4_fs_alloc_inode_ox has already rolled back
		 * any on-disk allocator state and dropped in-core inodes as
		 * needed, so there is nothing for us to free here.
		 */
		return -1;
	}

	child_ref = EXT4_INODE(child_inode);

	ext4_fs_inode_blocks_init(g_fs, child_ref);

	/*Link with parent dir.*/
	// TOCHECK: fname is correct? or full path is required?
	ret = ext4_link_(parent_ref, child_ref, fname, strlen(fname), false);
	if (ret != EOK) {
		fs_error("Add to parent directory failed. ret=%d", ret);
		goto free_generic_inode;
	}

	// Set generic inode content with ext4 inode.
	ret = ext4_set_new_inode(sb, child_inode, mode);
	if (ret != 0) {
		fs_error("Set generic inode failed..");
		goto free_generic_inode;
	}

	child_inode->i_ino = child_ref->index;

	*ret_i = child_inode;

	// TOCHECK: Is it duplicated with the generic dirty flag?
	// Maybe we need to excluding writeback from this function.
	ext4_fs_put_inode_ref(child_ref);

	/* Update stats and mark dir and new inode dirty */
	ext4_mark_inode_dirty(child_ref);
	// dir->i_mtime = dir->i_atime = dir->i_ctime = current_time(dir);
	if (S_ISDIR(mode))
		dir->i_nlink++;
	ext4_mark_inode_dirty(parent_ref);

	// log_debug(
	// 	"[%s] Success: parent=%p (ino=%lu) child=%p (ino=%lu) mode=%d fname=%s, size(%lu)",
	// 	__func__, dir, dir->i_ino, child_inode, child_inode->i_ino,
	// 	mode, fname, dir->i_size);

	return 0;

free_generic_inode:
	if (child_ref)
		ext4_fs_put_inode_ref(child_ref);
	if (child_inode)
		iput(child_inode);
	return ret;
}

static int lwext4_unlink(struct inode *dir, const char *name)
{
	int r;
	struct ext4_dir_search_result result;
	struct ext4_inode_ref *parent, *child;
	struct inode *child_inode;
	uint32_t child_ino;

	fs_trace("[%s] parent ino=%lu name=%s", __func__, dir->i_ino, name);

	parent = EXT4_INODE(dir);

	/* Find child inode ref. */
	r = ext4_dir_find_entry(&result, parent, name, strlen(name));
	if (r != EOK) {
		fs_error("Find dir entry(%s) failed. ret=%d", name, r);
		return -1;
	}

	// get inode number from dentry.
	child_ino = ext4_dir_en_get_inode(result.dentry);

	// get inode ref. (Search cache first in ext4_iget.)
	child_inode = ext4_iget(dir->i_sb, child_ino);
	if (child_inode == NULL) {
		fs_error("Get inode failed. ino=%u ret=%d", child_ino, r);
		return -1;
	}
	child = EXT4_INODE(child_inode);

	// /* Truncate */
	// if (inode_type != EXT4_INODE_MODE_DIRECTORY)
	// 	r = ext4_trunc_inode(mp, child.index, 0);
	// else
	// 	r = ext4_trunc_dir(mp, &act, &child);

	r = ext4_unlink_(parent, child, name, strlen(name));
	if (r != EOK) {
		fs_error("Unlink failed. ret=%d", r);
		goto err;
	}

	/* Handle generic metadata. */
	child_inode->i_nlink--;
	if (S_ISDIR(child_inode->i_mode)) {
		dir->i_nlink--;
		child_inode->i_nlink--;
		ext4_mark_inode_dirty(parent);
	}

	if (child_inode->i_nlink >= 1) {
		ext4_mark_inode_dirty(child);
		iput(child_inode);
		return 0;
	}

	/* Now inode become zero link count. Defer actual free to kernel evict time. */
	if (child_inode->i_nlink != 0)
		log_error("inode(%lu) nlink race condition on unlink",
			  child_ino);

	/* Persist metadata and mark deleted; then defer actual free by evicting */
	ext4_mark_inode_dirty(child);
	evict_inode(child_inode);
	return 0;

err:
	iput(child_inode);
	return -1;
}

static int lwext4_rename_new(struct inode *dir, const char *name,
			     unsigned long ino)
{
	struct ext4_inode_ref *dir_iref, *child_iref;
	struct inode *child;
	int ret;

	fs_trace("[%s] dir ino=%lu name=%s ino=%lu", __func__,
		dir->i_ino, name, ino);

	dir_iref = EXT4_INODE(dir);
	/*
	 * If target name already exists in this directory, emulate Linux
	 * rename semantics: remove the existing entry first (overwrite),
	 * then link the new inode under this name.
	 */
	{
		struct ext4_dir_search_result res;

		ret = ext4_dir_find_entry(&res, dir_iref, name, strlen(name));
		if (ret == EOK) {
			uint32_t existing_ino =
				ext4_dir_en_get_inode(res.dentry);

			ext4_dir_destroy_result(dir_iref, &res);

			if (existing_ino == ino) {
				/* Rename onto itself; nothing to do on NEW side. */
				fs_debug(
					"[lwext4_rename_new] rename to same inode: "
					"name=%s dir_ino=%lu ino=%lu",
					name, dir->i_ino, ino);
				return 0;
			}

			fs_debug(
				"[lwext4_rename_new] overwrite existing name=%s "
				"dir_ino=%lu old_ino=%" PRIu32 " new_ino=%lu",
				name, dir->i_ino, existing_ino, ino);

			/* Remove existing target entry (and possibly the inode). */
			ret = lwext4_unlink(dir, name);
			if (ret < 0) {
				fs_error("[lwext4_rename_new] failed to unlink "
					 "existing name=%s dir_ino=%lu",
					 name, dir->i_ino);
				return ret;
			}
		}
	}

	child = ext4_iget(dir->i_sb, ino);
	if (child == NULL) {
		fs_error("Get inode failed. ino=%lu", ino);
		return -1;
	}
	child_iref = EXT4_INODE(child);

	ret = ext4_link_(dir_iref, child_iref, name, strlen(name), false);
	if (ret != EOK) {
		fs_error("Add to parent directory failed. ret=%d", ret);
		// oxbow_assert(false); // Not handled yet.
	}
	ext4_fs_put_inode_ref(child_iref);

	if (S_ISDIR(child->i_mode))
		dir->i_nlink++;

	ext4_mark_inode_dirty(dir_iref);
	return 0;
}

static int lwext4_rename_old(struct inode *dir, const char *name,
			     unsigned long ino)
{
	struct ext4_dir_search_result result;
	struct ext4_inode_ref *dir_iref, *child_iref;
	struct inode *child;
	int ret;

	fs_trace("[%s] dir ino=%lu name=%s ino=%lu", __func__, dir->i_ino, name,
		 ino);

	dir_iref = EXT4_INODE(dir);
	ret = ext4_dir_find_entry(&result, dir_iref, name, strlen(name));
	if (ret != EOK) {
		fs_error("Find dir entry(%s) failed. ret=%d", name, ret);
		return -1;
	}

	if (ext4_dir_en_get_inode(result.dentry) != ino) {
		fs_error("inode number mismatch req(%lu) found(%lu)", ino,
			 ext4_dir_en_get_inode(result.dentry));
		return -1;
	}

	// get inode ref. (Search cache first in ext4_iget.)
	child = ext4_iget(dir->i_sb, ino);
	if (child == NULL) {
		fs_error("Get inode failed. ino=%u ret=%d", ino, ret);
		return -1;
	}

	child_iref = EXT4_INODE(child);

	ret = ext4_dir_remove_entry(dir_iref, name, strlen(name));
	if (ret != EOK) {
		ext4_fs_put_inode_ref(child_iref); // TOCHECK: is it needed?
		fs_error("Unlink failed. ret=%d", ret);
		return -1;
	}

	ext4_fs_put_inode_ref(child_iref);
	if (S_ISDIR(child->i_mode)) {
		dir->i_nlink--;
		ext4_mark_inode_dirty(dir_iref);
	}

	return 0;
}

static int lwext4_get_block(struct inode *inode, struct buffer_extent *be,
			    sector_t iblock, u32 create)
{
	ext4_fsblk_t fblk;
	struct ext4_inode_ref *ref;
	uint32_t allocated, n_blks_to_create;
	struct buffer_extent final_be;
	sector_t iblock_to_create;
	int ret;

	BUG_ON(S_ISDIR(inode->i_mode), "not expected");

	ref = EXT4_INODE(inode);

	allocated = 0;
	iblock_to_create = iblock;

	// No create case.
	if (!create) {
		ret = ext4_fs_get_inode_dblk_idxs(ref, iblock, &fblk, 0, be);

		if (ret != EOK) {
			fs_error("Getting data block (no create) failed.");
			return ret;
		}

		fs_trace("[%s] index=%d fblk(returned)=%lu", __func__, iblock,
			 fblk);

		if (be) {
			fs_trace("      be => iblock=%lu lba=%lu nr=%u",
				 be->iblock, be->lba, be->nr);

			oxbow_assert(be->nr >= 1);
		}

		return 0;
	}

	// Create case.
	while (allocated < create) {
		// TODO: We have to consider how to return multiple buffer
		// extents. For now, only support one allocation.
		oxbow_assert(allocated == 0);

		n_blks_to_create = create - allocated;

		fs_trace(
			"Buffer extent to alloc: iblock=%lu nr=%u (total_nr_to_create=%u)",
			iblock_to_create, n_blks_to_create, create);

		ret = ext4_fs_get_inode_dblk_idxs(ref, iblock_to_create, &fblk,
						  n_blks_to_create, be);

		if (ret != EOK) {
			fs_error("Getting data block (create) failed.");
			return ret;
		}

		// `be` can be NULL.
		if (!be)
			goto skip_be_update;

		fs_trace("Buffer extent allocated: iblock=%lu lba=%lu nr=%u",
			 be->iblock, be->lba, be->nr);

		// If it is the first run.
		if (allocated == 0) {
			final_be.iblock = be->iblock;
			final_be.lba = be->lba;
			final_be.nr = be->nr;

		} else {
			// Check the continuity of the allocated physical blocks.
			// TMP: disabled
			// oxbow_assert(final_be.lba + final_be.nr == be->lba);

			// be's nr does not need to be the same as the requested nr.
		}

	skip_be_update:
		allocated += n_blks_to_create;
		iblock_to_create += n_blks_to_create;
	}

	fs_trace(
		"[%s] ino=%lu index=%d create_blocks requested=%d allocated=%d",
		__func__, inode->i_ino, iblock, create, allocated);

	// Set be to the final values (to the first be).
	if (be) {
		be->iblock = final_be.iblock;
		be->lba = final_be.lba;
		be->nr = final_be.nr;
		fs_trace("         => iblock=%lu lba=%lu nr=%u", be->iblock,
			 be->lba, be->nr);

		oxbow_assert(be->lba == fblk);
	}

	return 0;
}

int lwext4_block_alloc(struct inode *inode, sector_t iblock)
{
	return mpage_block_alloc(inode, iblock);
}

void lwext4_readpage(struct read_control *rc)
{
	mpage_readpage(rc);
}

void lwext4_readahead(struct readahead_control *rac)
{
	mpage_readahead(rac);
}

const struct inode_operations lwext4_dir_inode_ops = {
	.lookup = lwext4_lookup,
	.create = lwext4_create,
	.unlink = lwext4_unlink,
	.rename_new = lwext4_rename_new,
	.rename_old = lwext4_rename_old,
	.get_blocks = lwext4_get_block,
};

const struct inode_operations lwext4_inode_ops = {
	.get_blocks = lwext4_get_block,
};

const struct address_space_operations lwext4_aops = {
	.block_alloc = lwext4_block_alloc,
	.readpage = lwext4_readpage,
	.readahead = lwext4_readahead,
};

/**
 * @}
 */
