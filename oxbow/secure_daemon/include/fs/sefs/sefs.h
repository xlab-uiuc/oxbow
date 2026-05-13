#ifndef SEFS_H
#define SEFS_H

#include "fs/fs.h"
#include "common/fs/sefs/sefs.h" /* used in macro */
#include "oxbow_debug.h"
#include <stdint.h>

#define SEFS_ROOT_INO 1 // Should be same with OXBOW_ROOT_INO in oxbow_kernel.h
#define SEFS_FS_MAX_SIZE (2048UL * (1UL << 30)) /* 2 TiB */

#define SEFS_MAGIC 0x012E0BAF
#define SEFS_BLOCK_SIZE (1UL << 12) /* 4 KiB */

/* max extent per one ei_block */
#define SEFS_MAX_EXTENTS                                                       \
	((SEFS_BLOCK_SIZE - sizeof(uint64_t)) / sizeof(struct sefs_extent))

#define SEFS_MAX_FILESIZE OXBOW_MAX_FILE_SIZE

#define SEFS_NAME_MAX OXBOW_FILENAME_MAX

/* max block for directory entry per ext */
#define SEFS_DENT_BLOCKS 8

/* max directory entry per one block */
#define SEFS_D_INFO_ENT_MAX                                                    \
	((SEFS_BLOCK_SIZE - sizeof(uint32_t)) / sizeof(struct d_info_ent))

#define SEFS_DENT_PER_BLOCK                                                    \
	((SEFS_BLOCK_SIZE - sizeof(uint32_t)) / sizeof(struct sefs_dirent))

#define SEFS_DENT_PER_EXT (SEFS_DENT_PER_BLOCK * SEFS_DENT_BLOCKS)

#define SEFS_DENT_MAX (SEFS_DENT_PER_EXT * SEFS_D_INFO_ENT_MAX)

#define SEFS_INODES_PER_BLOCK (SEFS_BLOCK_SIZE / sizeof(struct sefs_inode))

// The number of bits in a block. (aka. the number of blocks represented by bits
// in a page.)
#define BITMAP_BLK_NR (SEFS_BLOCK_SIZE * 8)

#define SEFS_INO_BLOCK(ino) ((ino) / SEFS_INODES_PER_BLOCK + 1)
#define SEFS_INO_SHIFT(ino) ((ino) % SEFS_INODES_PER_BLOCK)

// TODO: Need to be updated to include staging area.
/*
 * sefs partition layout
 * +---------------+
 * |  superblock   |  1 block
 * +---------------+
 * |  inode store  |  sb->nr_istore_blocks blocks
 * +---------------+
 * | ifree bitmap  |  sb->nr_ifree_blocks blocks
 * +---------------+
 * | bfree bitmap  |  sb->nr_bfree_blocks blocks
 * +---------------+
 * |    data       |
 * |      blocks   |  rest of the blocks
 * +---------------+
 *  (if journal on)   below area is for journaling
 * +---------------+
 * |  j_superblock |  1 block
 * +---------------+
 * | j_transaction |
 * |      blocks   |  rest of the blocks
 * +---------------+
 * |  s_superblock |  1 block
 * +---------------+
 * | s_transaction |
 * |      blocks   |  rest of the blocks
 * +---------------+
 */

struct sefs_inode {
	__le16 i_mode; /* File mode */
	__le16 i_uid; /* Owner id */
	__le16 i_gid; /* Group id */
	__le16 i_nlink; /* Hard links count */
	__le64 i_size; /* Size in bytes */
	__le32 i_ctime; /* Inode change time */
	__le32 i_atime; /* Access time */
	__le32 i_mtime; /* Modification time */
	__le32 i_blocks; /* Block count */
	__le64 ei_block; /* Block with list of extents for this file */
	__le32 i_data[8]; /* store symlink content */
};

static inline void print_sefs_inode(const struct sefs_inode *inode
				    __attribute__((unused)))
{
#ifdef PRINT_DUMP_BLK
	printf("SEFS Inode:\n");
	printf("  mode: 0x%x\n", le16_to_cpu(inode->i_mode));
	printf("  uid: %u\n", le16_to_cpu(inode->i_uid));
	printf("  gid: %u\n", le16_to_cpu(inode->i_gid));
	printf("  nlink: %u\n", le16_to_cpu(inode->i_nlink));
	printf("  size: %llu\n", le64_to_cpu(inode->i_size));
	printf("  ctime: %u\n", le32_to_cpu(inode->i_ctime));
	printf("  atime: %u\n", le32_to_cpu(inode->i_atime));
	printf("  mtime: %u\n", le32_to_cpu(inode->i_mtime));
	printf("  blocks: %u\n", le32_to_cpu(inode->i_blocks));
	printf("  ei_block: %llu\n", le64_to_cpu(inode->ei_block));

	// Print i_data array
	printf("  i_data:\n");
	for (int i = 0; i < 8; i++) {
		printf("    [%d]: %u\n", i, le32_to_cpu(inode->i_data[i]));
	}
#endif
}

#define SEFS_FEATURE_HAS_JOURNAL 0x0004

struct sefs_inode_info {
	union {
		uint64_t ei_block; /* Block with list of extents for this file */
		uint64_t d_info_block;
	};
	uint32_t i_data[8];
	struct inode vfs_inode;
};

struct sefs_extent {
	uint64_t bno; /* first physical block extent covers */
	uint32_t iblock; /* first logical block extent covers */
	uint16_t len; /* number of blocks covered by extent */
} __attribute__((packed));

struct sefs_ei_block {
	uint64_t next; /* regular file : next ei_block */
	struct sefs_extent extents[SEFS_MAX_EXTENTS];
} __attribute__((aligned(SEFS_BLOCK_SIZE)));

/**
 * @brief Directory blocks metadata.
 * 
 */
struct sefs_dirent {
	uint64_t ino;
	char name[SEFS_NAME_MAX];
};
struct sefs_dir_block {
	uint32_t nr_files; // max value is SEFS_DENT_PER_BLOCK
	struct sefs_dirent files[SEFS_DENT_PER_BLOCK];
} __attribute__((aligned(SEFS_BLOCK_SIZE)));

struct d_info_ent {
	uint32_t nr_files; // max value is SEFS_DENT_PER_EXT
	struct sefs_extent d_ext;
};
struct sefs_d_info_block {
	uint32_t nr_files; // max value is SEFS_DENT_MAX
	struct d_info_ent ents[SEFS_D_INFO_ENT_MAX];
} __attribute__((aligned(SEFS_BLOCK_SIZE)));

/* inode.c */
struct inode *sefs_iget(struct super_block *, unsigned long);

extern const struct inode_operations sefs_dir_inode_operation;
extern const struct inode_operations sefs_inode_operation;

/* super.c */
int sefs_fill_super(struct super_block *);

/* dir.c */
extern const struct file_operations sefs_dir_ops;

/* Getters for superbock and inode */
#define SEFS_SB(sb) ((struct sefs_sb_info *)sb->s_fs_info)
#define SEFS_INODE(inode)                                                      \
	(container_of(inode, struct sefs_inode_info, vfs_inode))

#ifdef DAEMON_DEBUG
#define print_file_type(inode)                                                 \
	do {                                                                   \
		if (S_ISDIR((inode)->i_mode))                                  \
			sefs_debug("[%s] directory", __func__);                \
		else if (S_ISREG((inode)->i_mode))                             \
			sefs_debug("[%s] regular file", __func__);             \
                                                                               \
		else                                                           \
			oxb_error("unknown type");                             \
	} while (0);
#else
#define print_file_type(inode)                                                 \
	do {                                                                   \
	} while (0)
#endif

#endif
