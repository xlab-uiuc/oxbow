#include "common/fs/lwext4/lwext4.h"
#include "journal_op.h"
#include "common/oxbow.h"

baddr_t first_stage_log_blk(void *superblock)
{
	struct ext4_sblock *info = (struct ext4_sblock *)superblock;

	// TOCHECK: The next block of the stage super block.
	return info->ox_staging_sb + 1;
}

baddr_t last_stage_log_blk(void *superblock)
{
	struct ext4_sblock *info = (struct ext4_sblock *)superblock;
	return info->ox_staging_sb + info->ox_nr_staging_blocks;
}

uint32_t get_nr_stage_log_blocks(void *superblock)
{
	struct ext4_sblock *info = (struct ext4_sblock *)superblock;

	// TOCHECK: Except stage super block.
	return info->ox_nr_staging_blocks - 1;
}

baddr_t get_journal_sb_baddr(void *superblock)
{
	struct ext4_sblock *info = (struct ext4_sblock *)superblock;

	return info->ox_journal_sb;
}

uint32_t get_nr_journal_log_blocks(void *superblock)
{
	struct ext4_sblock *info = (struct ext4_sblock *)superblock;

	return info->ox_nr_journal_blocks;
}

baddr_t get_stage_sb_baddr(void *superblock)
{
	struct ext4_sblock *info = (struct ext4_sblock *)superblock;

	return info->ox_staging_sb;
}

uint32_t get_sb_offset(void)
{
	return EXT4_SUPERBLOCK_OFFSET;
}

baddr_t get_fs_area_start_baddr(void *superblock)
{
	return OXBOW_SUPER_BLOCK_NR; // It starts from 0.
}

uint32_t get_nr_fs_area_blocks(void *superblock)
{
	struct ext4_sblock *info = (struct ext4_sblock *)superblock;

	// journal area follows after the file system area.
	return info->ox_journal_sb - OXBOW_SUPER_BLOCK_NR;
}

baddr_t get_inode_region_start(void *superblock)
{
	struct ext4_sblock *info = (struct ext4_sblock *)superblock;

	// Use correct ext4_mkfs.c logic to calculate inode table start
	uint32_t block_size = OXBOW_BLOCK_SIZE; // 4096
	uint32_t blocks_per_group = info->blocks_per_group;
	uint32_t blocks_count = info->blocks_count_lo;
	uint32_t bg_count =
		(blocks_count + blocks_per_group - 1) / blocks_per_group;
	uint32_t desc_size = info->desc_size;
	if (desc_size == 0)
		desc_size = 32; // Default EXT4 descriptor size

	// Calculate first_data_block (same logic as ext4_mkfs.c)
	uint32_t first_data_block = (block_size > 1024) ? 0 : 1;
	uint32_t descs_per_block = block_size / desc_size;
	uint32_t gdt_blocks =
		(bg_count + descs_per_block - 1) / descs_per_block;

	// For first block group only (block group 0) - following ext4_mkfs.c logic
	uint64_t bg_start_block =
		first_data_block + 0 * blocks_per_group; // Group 0
	uint32_t blk_off = 0;

	// Adjust for group descriptor blocks and superblock
	blk_off += gdt_blocks;
	if (true) { // Group 0 always has superblock
		bg_start_block++; // Skip superblock
		uint16_t reserved_gdt_blocks = info->s_reserved_gdt_blocks;
		blk_off += reserved_gdt_blocks;
	}

	baddr_t fs_start = get_fs_area_start_baddr(superblock);
	baddr_t inode_table_start = fs_start + bg_start_block + blk_off + 3;

	return inode_table_start;
}

baddr_t get_inode_region_end(void *superblock)
{
	struct ext4_sblock *info = (struct ext4_sblock *)superblock;

	// Calculate end of inode table for single block group
	uint32_t inodes_per_group = info->inodes_per_group;
	uint32_t inode_size = info->inode_size;
	uint32_t block_size = OXBOW_BLOCK_SIZE; // Use consistent block size
	uint32_t inode_blocks_per_group =
		(inodes_per_group * inode_size + block_size - 1) / block_size;

	// Single block group assumption: only one inode table
	return get_inode_region_start(superblock) + inode_blocks_per_group - 1;
}

baddr_t get_data_region_start(void *superblock)
{
	// Data blocks start after inode tables
	return get_inode_region_end(superblock) + 1;
}

baddr_t get_data_region_end(void *superblock)
{
	struct ext4_sblock *info = (struct ext4_sblock *)superblock;

	// Data region ends where journal begins
	return info->ox_journal_sb - 1;
}

struct journal_operations lwext4_journal_ops = {
	.get_sb_offset = get_sb_offset,
	.get_journal_sb_baddr = get_journal_sb_baddr,
	.get_nr_journal_log_blocks = get_nr_journal_log_blocks,
	.get_stage_sb_baddr = get_stage_sb_baddr,
	.first_stage_log_blk = first_stage_log_blk,
	.last_stage_log_blk = last_stage_log_blk,
	.get_nr_stage_log_blocks = get_nr_stage_log_blocks,
	.get_fs_area_start_baddr = get_fs_area_start_baddr,
	.get_nr_fs_area_blocks = get_nr_fs_area_blocks,
	.get_inode_region_start = get_inode_region_start,
	.get_inode_region_end = get_inode_region_end,
	.get_data_region_start = get_data_region_start,
	.get_data_region_end = get_data_region_end,
};

struct journal_operations *g_j_ops = &lwext4_journal_ops;
