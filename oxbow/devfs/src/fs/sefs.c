#include "common/fs/sefs/sefs.h"
#include "journal_op.h"

baddr_t first_stage_log_blk(void *superblock)
{
	struct sefs_sb_info *info = (struct sefs_sb_info *)superblock;

	// TOCHECK: The next block of the stage super block.
	return info->staging_sb + 1;
}

baddr_t last_stage_log_blk(void *superblock)
{
	struct sefs_sb_info *info = (struct sefs_sb_info *)superblock;
	return info->staging_sb + info->nr_staging_blocks - 1;
}

uint32_t get_nr_stage_log_blocks(void *superblock)
{
	struct sefs_sb_info *info = (struct sefs_sb_info *)superblock;

	// TOCHECK: Except stage super block.
	return info->nr_staging_blocks - 1;
}

baddr_t get_journal_sb_baddr(void *superblock)
{
	struct sefs_sb_info *info = (struct sefs_sb_info *)superblock;

	printf("\tmagic=%#x\n"
	       "\tnr_blocks=%u\n"
	       "\tnr_inodes=%u (istore=%u blocks)\n"
	       "\tnr_ifree_blocks=%u\n"
	       "\tnr_bfree_blocks=%u\n"
	       "\tnr_free_inodes=%u\n"
	       "\tnr_free_blocks=%u\n"
	       //    "\tnr_journal_blocks=%u\n"
	       "\tnr_journal_sb=%lu\n"
	       "\tnr_staging_sb=%lu\n",

	       info->magic, info->nr_blocks, info->nr_inodes,
	       info->nr_istore_blocks, info->nr_ifree_blocks,
	       info->nr_bfree_blocks, info->nr_free_inodes,
	       info->nr_free_blocks, info->journal_sb, info->staging_sb);

	return info->journal_sb;
}

uint32_t get_nr_journal_log_blocks(void *superblock)
{
	struct sefs_sb_info *info = (struct sefs_sb_info *)superblock;

	return info->nr_journal_blocks;
}

baddr_t get_stage_sb_baddr(void *superblock)
{
	struct sefs_sb_info *info = (struct sefs_sb_info *)superblock;

	return info->staging_sb;
}

uint32_t get_sb_offset(void)
{
	return 0; // SEFS superblock is at block 0
}

baddr_t get_fs_area_start_baddr(void *superblock)
{
	return 0; // SEFS filesystem starts from block 0
}

uint32_t get_nr_fs_area_blocks(void *superblock)
{
	struct sefs_sb_info *info = (struct sefs_sb_info *)superblock;

	// Filesystem area ends where journal begins
	return info->journal_sb;
}

baddr_t sefs_get_inode_region_start(void *superblock)
{
	struct sefs_sb_info *info = (struct sefs_sb_info *)superblock;

	// Correct SEFS layout from mkfs.c:
	// Block 0: Superblock
	// Block 1+: Inode store blocks (immediately after superblock)
	return 1;
}

baddr_t sefs_get_inode_region_end(void *superblock)
{
	struct sefs_sb_info *info = (struct sefs_sb_info *)superblock;

	// Inode region ends after all inode store blocks
	baddr_t start = sefs_get_inode_region_start(superblock);
	return start + info->nr_istore_blocks - 1;
}

baddr_t sefs_get_data_region_start(void *superblock)
{
	struct sefs_sb_info *info = (struct sefs_sb_info *)superblock;

	// Data blocks start after: superblock + inode store + inode bitmap + block bitmap
	// Following mkfs.c sequence: superblock -> inode store -> ifree bitmap -> bfree bitmap -> data
	return 1 + info->nr_istore_blocks + info->nr_ifree_blocks +
	       info->nr_bfree_blocks;
}

baddr_t sefs_get_data_region_end(void *superblock)
{
	struct sefs_sb_info *info = (struct sefs_sb_info *)superblock;

	// Data region ends where journal begins
	return info->journal_sb - 1;
}

struct journal_operations sefs_journal_ops = {
	.get_sb_offset = get_sb_offset,
	.get_journal_sb_baddr = get_journal_sb_baddr,
	.get_nr_journal_log_blocks = get_nr_journal_log_blocks,
	.get_stage_sb_baddr = get_stage_sb_baddr,
	.first_stage_log_blk = first_stage_log_blk,
	.last_stage_log_blk = last_stage_log_blk,
	.get_nr_stage_log_blocks = get_nr_stage_log_blocks,
	.get_fs_area_start_baddr = get_fs_area_start_baddr,
	.get_nr_fs_area_blocks = get_nr_fs_area_blocks,
	.get_inode_region_start = sefs_get_inode_region_start,
	.get_inode_region_end = sefs_get_inode_region_end,
	.get_data_region_start = sefs_get_data_region_start,
	.get_data_region_end = sefs_get_data_region_end,
};

struct journal_operations *g_j_ops = &sefs_journal_ops;
