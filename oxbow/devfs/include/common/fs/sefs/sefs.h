#ifndef _COMMON_FS_SEFS_H_
#define _COMMON_FS_SEFS_H_

#include "oxbow.h"
#include <stdint.h>
#include <pthread.h>

struct sefs_sb_info {
	uint32_t magic; /* Magic number */

	uint32_t nr_blocks; /* Total number of blocks (incl sb & inodes) */
	uint32_t nr_inodes; /* Total number of inodes */

	uint32_t nr_istore_blocks; /* Number of inode store blocks */
	uint32_t nr_ifree_blocks; /* Number of inode free bitmap blocks */
	uint32_t nr_bfree_blocks; /* Number of block free bitmap blocks */

	uint32_t nr_free_inodes; /* Number of free inodes */
	uint32_t nr_free_blocks; /* Number of free blocks */

	uint32_t fs_features;

	// REFACTOR: We can define a generic journal attribute.
	/* For journaling and staging */
	baddr_t journal_sb; /* background journal superblock */
	uint32_t nr_journal_blocks;
	baddr_t staging_sb; /* staging superblock */
	uint32_t nr_staging_blocks;

	/* upper must match with on-disk superblock */
	pthread_spinlock_t lock;
	uint32_t ifree_first; /* first block index of inode free bitmap (LBA) */
	uint32_t *ifree_bit_left; /* free bitmaps infos about left bits */
	uint32_t bfree_first; /* first block index of block free bitmap (LBA) */
	uint32_t *bfree_bit_left; /* free bitmaps infos about left bits */
};

#endif
