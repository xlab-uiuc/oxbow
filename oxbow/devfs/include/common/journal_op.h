#ifndef _JOURNAL_OP_H_
#define _JOURNAL_OP_H_

#include "oxbow.h"

struct journal_operations {
	uint32_t (*get_sb_offset)(void); // Offset of superblock within a block.
	baddr_t (*get_journal_sb_baddr)(void *superblock);
	uint32_t (*get_nr_journal_log_blocks)(
		void *superblock); // NOT including journal super block.
	baddr_t (*get_stage_sb_baddr)(void *superblock);
	baddr_t (*first_stage_log_blk)(void *superblock);
	baddr_t (*last_stage_log_blk)(void *superblock);
	uint32_t (*get_nr_stage_log_blocks)(
		void *superblock); // NOT including stage super block.
	baddr_t (*get_fs_area_start_baddr)(void *superblock);
	uint32_t (*get_nr_fs_area_blocks)(
		void *superblock); // including super block.

	// Functions for determining inode block regions
	baddr_t (*get_inode_region_start)(void *superblock);
	baddr_t (*get_inode_region_end)(void *superblock);
	baddr_t (*get_data_region_start)(void *superblock);
	baddr_t (*get_data_region_end)(void *superblock);
};

extern struct journal_operations *g_j_ops;

void init_journal_operations(struct journal_operations *ops);

#endif
