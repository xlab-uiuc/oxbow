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
 * @file  ext4_types.h
 * @brief Ext4 data structure definitions.
 */

#ifndef EXT4_TYPES_H_
#define EXT4_TYPES_H_

#ifdef __cplusplus
extern "C" {
#endif

#include <ext4_config.h>
#include <ext4_blockdev.h>
#include <misc/tree.h>

#include <stddef.h>
#include <stdint.h>
#include <sys/stat.h>

#include "common/fs/lwext4/lwext4.h"
/*
 * Types of blocks.
 */
typedef uint32_t ext4_lblk_t;
typedef uint64_t ext4_fsblk_t;


#define EXT4_CHECKSUM_CRC32C 1

#define EXT4_SUPERBLOCK_SIZE 1024

#define EXT4_SUPERBLOCK_OS_LINUX 0
#define EXT4_SUPERBLOCK_OS_HURD 1

/*
 * Misc. filesystem flags
 */
#define EXT4_SUPERBLOCK_FLAGS_SIGNED_HASH 0x0001
#define EXT4_SUPERBLOCK_FLAGS_UNSIGNED_HASH 0x0002
#define EXT4_SUPERBLOCK_FLAGS_TEST_FILESYS 0x0004
/*
 * Filesystem states
 */
#define EXT4_SUPERBLOCK_STATE_VALID_FS 0x0001  /* Unmounted cleanly */
#define EXT4_SUPERBLOCK_STATE_ERROR_FS 0x0002  /* Errors detected */
#define EXT4_SUPERBLOCK_STATE_ORPHAN_FS 0x0004 /* Orphans being recovered */

/*
 * Behaviour when errors detected
 */
#define EXT4_SUPERBLOCK_ERRORS_CONTINUE 1 /* Continue execution */
#define EXT4_SUPERBLOCK_ERRORS_RO 2       /* Remount fs read-only */
#define EXT4_SUPERBLOCK_ERRORS_PANIC 3    /* Panic */
#define EXT4_SUPERBLOCK_ERRORS_DEFAULT EXT4_ERRORS_CONTINUE

/*
 * Compatible features
 */
#define EXT4_FCOM_DIR_PREALLOC 0x0001
#define EXT4_FCOM_IMAGIC_INODES 0x0002
#define EXT4_FCOM_HAS_JOURNAL 0x0004
#define EXT4_FCOM_EXT_ATTR 0x0008
#define EXT4_FCOM_RESIZE_INODE 0x0010
#define EXT4_FCOM_DIR_INDEX 0x0020

/*
 * Read-only compatible features
 */
#define EXT4_FRO_COM_SPARSE_SUPER 0x0001
#define EXT4_FRO_COM_LARGE_FILE 0x0002
#define EXT4_FRO_COM_BTREE_DIR 0x0004
#define EXT4_FRO_COM_HUGE_FILE 0x0008
#define EXT4_FRO_COM_GDT_CSUM 0x0010
#define EXT4_FRO_COM_DIR_NLINK 0x0020
#define EXT4_FRO_COM_EXTRA_ISIZE 0x0040
#define EXT4_FRO_COM_QUOTA 0x0100
#define EXT4_FRO_COM_BIGALLOC 0x0200
#define EXT4_FRO_COM_METADATA_CSUM 0x0400

/*
 * Incompatible features
 */
#define EXT4_FINCOM_COMPRESSION 0x0001
#define EXT4_FINCOM_FILETYPE 0x0002
#define EXT4_FINCOM_RECOVER 0x0004     /* Needs recovery */
#define EXT4_FINCOM_JOURNAL_DEV 0x0008 /* Journal device */
#define EXT4_FINCOM_META_BG 0x0010
#define EXT4_FINCOM_EXTENTS 0x0040 /* extents support */
#define EXT4_FINCOM_64BIT 0x0080
#define EXT4_FINCOM_MMP 0x0100
#define EXT4_FINCOM_FLEX_BG 0x0200
#define EXT4_FINCOM_EA_INODE 0x0400	 /* EA in inode */
#define EXT4_FINCOM_DIRDATA 0x1000	  /* data in dirent */
#define EXT4_FINCOM_BG_USE_META_CSUM 0x2000 /* use crc32c for bg */
#define EXT4_FINCOM_LARGEDIR 0x4000	 /* >2GB or 3-lvl htree */
#define EXT4_FINCOM_INLINE_DATA 0x8000      /* data in inode */

/*
 * EXT2 supported feature set
 */
#define EXT2_SUPPORTED_FCOM 0x0000

#define EXT2_SUPPORTED_FINCOM                                   \
	(EXT4_FINCOM_FILETYPE | EXT4_FINCOM_META_BG)

#define EXT2_SUPPORTED_FRO_COM                                  \
	(EXT4_FRO_COM_SPARSE_SUPER |                            \
	 EXT4_FRO_COM_LARGE_FILE)

/*
 * EXT3 supported feature set
 */
#define EXT3_SUPPORTED_FCOM (EXT4_FCOM_DIR_INDEX)

#define EXT3_SUPPORTED_FINCOM                                 \
	(EXT4_FINCOM_FILETYPE | EXT4_FINCOM_META_BG)

#define EXT3_SUPPORTED_FRO_COM                                \
	(EXT4_FRO_COM_SPARSE_SUPER | EXT4_FRO_COM_LARGE_FILE)

/*
 * EXT4 supported feature set
 */
#define EXT4_SUPPORTED_FCOM (EXT4_FCOM_DIR_INDEX)

#define EXT4_SUPPORTED_FINCOM                              \
	(EXT4_FINCOM_FILETYPE | EXT4_FINCOM_META_BG |      \
	 EXT4_FINCOM_EXTENTS | EXT4_FINCOM_FLEX_BG |       \
	 EXT4_FINCOM_64BIT)

#define EXT4_SUPPORTED_FRO_COM                             \
	(EXT4_FRO_COM_SPARSE_SUPER |                       \
	 EXT4_FRO_COM_METADATA_CSUM |                      \
	 EXT4_FRO_COM_LARGE_FILE | EXT4_FRO_COM_GDT_CSUM | \
	 EXT4_FRO_COM_DIR_NLINK |                          \
	 EXT4_FRO_COM_EXTRA_ISIZE | EXT4_FRO_COM_HUGE_FILE)

/*Ignored features:
 * RECOVER - journaling in lwext4 is not supported
 *           (probably won't be ever...)
 * MMP - multi-mout protection (impossible scenario)
 * */
#define EXT_FINCOM_IGNORED                                 \
	EXT4_FINCOM_RECOVER | EXT4_FINCOM_MMP

#if 0
/*TODO: Features incompatible to implement*/
#define EXT4_SUPPORTED_FINCOM
                     (EXT4_FINCOM_INLINE_DATA)

/*TODO: Features read only to implement*/
#define EXT4_SUPPORTED_FRO_COM
                     EXT4_FRO_COM_BIGALLOC |\
                     EXT4_FRO_COM_QUOTA)
#endif


/* Inode table/bitmap not in use */
#define EXT4_BLOCK_GROUP_INODE_UNINIT 0x0001
/* Block bitmap not in use */
#define EXT4_BLOCK_GROUP_BLOCK_UNINIT 0x0002
/* On-disk itable initialized to zero */
#define EXT4_BLOCK_GROUP_ITABLE_ZEROED 0x0004

/*
 * Structure of a blocks group descriptor
 */
struct ext4_bgroup {
	uint32_t block_bitmap_lo;	    /* Blocks bitmap block */
	uint32_t inode_bitmap_lo;	    /* Inodes bitmap block */
	uint32_t inode_table_first_block_lo; /* Inodes table block */
	uint16_t free_blocks_count_lo;       /* Free blocks count */
	uint16_t free_inodes_count_lo;       /* Free inodes count */
	uint16_t used_dirs_count_lo;	 /* Directories count */
	uint16_t flags;		       /* EXT4_BG_flags (INODE_UNINIT, etc) */
	uint32_t exclude_bitmap_lo;    /* Exclude bitmap for snapshots */
	uint16_t block_bitmap_csum_lo; /* crc32c(s_uuid+grp_num+bbitmap) LE */
	uint16_t inode_bitmap_csum_lo; /* crc32c(s_uuid+grp_num+ibitmap) LE */
	uint16_t itable_unused_lo;     /* Unused inodes count */
	uint16_t checksum;	     /* crc16(sb_uuid+group+desc) */

	uint32_t block_bitmap_hi;	    /* Blocks bitmap block MSB */
	uint32_t inode_bitmap_hi;	    /* I-nodes bitmap block MSB */
	uint32_t inode_table_first_block_hi; /* I-nodes table block MSB */
	uint16_t free_blocks_count_hi;       /* Free blocks count MSB */
	uint16_t free_inodes_count_hi;       /* Free i-nodes count MSB */
	uint16_t used_dirs_count_hi;	 /* Directories count MSB */
	uint16_t itable_unused_hi;	   /* Unused inodes count MSB */
	uint32_t exclude_bitmap_hi;	  /* Exclude bitmap block MSB */
	uint16_t block_bitmap_csum_hi; /* crc32c(s_uuid+grp_num+bbitmap) BE */
	uint16_t inode_bitmap_csum_hi; /* crc32c(s_uuid+grp_num+ibitmap) BE */
	uint32_t reserved;	     /* Padding */
};


#define EXT4_MIN_BLOCK_GROUP_DESCRIPTOR_SIZE 32
#define EXT4_MAX_BLOCK_GROUP_DESCRIPTOR_SIZE 64

#define EXT4_MIN_BLOCK_SIZE 1024  /* 1 KiB */
#define EXT4_MAX_BLOCK_SIZE 65536 /* 64 KiB */
#define EXT4_REV0_INODE_SIZE 128

#define EXT4_INODE_BLOCK_SIZE 4096
#define EXT4_INODE_SIZE 256 // It was hardcoded in the original lwext4. There can be more somewhere else.

#define EXT4_INODE_DIRECT_BLOCK_COUNT 12
#define EXT4_INODE_INDIRECT_BLOCK EXT4_INODE_DIRECT_BLOCK_COUNT
#define EXT4_INODE_DOUBLE_INDIRECT_BLOCK (EXT4_INODE_INDIRECT_BLOCK + 1)
#define EXT4_INODE_TRIPPLE_INDIRECT_BLOCK (EXT4_INODE_DOUBLE_INDIRECT_BLOCK + 1)
#define EXT4_INODE_BLOCKS (EXT4_INODE_TRIPPLE_INDIRECT_BLOCK + 1)
#define EXT4_INODE_INDIRECT_BLOCK_COUNT                                        \
	(EXT4_INODE_BLOCKS - EXT4_INODE_DIRECT_BLOCK_COUNT)

#pragma pack(push, 1)

/*
 * Structure of an inode on the disk
 */
struct ext4_inode {
	uint16_t mode;		    /* File mode */
	uint16_t uid;		    /* Low 16 bits of owner uid */
	uint32_t size_lo;	   /* Size in bytes */
	uint32_t access_time;       /* Access time */
	uint32_t change_inode_time; /* I-node change time */
	uint32_t modification_time; /* Modification time */
	uint32_t deletion_time;     /* Deletion time */
	uint16_t gid;		    /* Low 16 bits of group id */
	uint16_t links_count;       /* Links count */
	uint32_t blocks_count_lo;   /* Blocks count */
	uint32_t flags;		    /* File flags */
	uint32_t unused_osd1;       /* OS dependent - not used in HelenOS */
	uint32_t blocks[EXT4_INODE_BLOCKS]; /* Pointers to blocks */
	uint32_t generation;		    /* File version (for NFS) */
	uint32_t file_acl_lo;		    /* File ACL */
	uint32_t size_hi;
	uint32_t obso_faddr; /* Obsoleted fragment address */

	union {
		struct {
			uint16_t blocks_high;
			uint16_t file_acl_high;
			uint16_t uid_high;
			uint16_t gid_high;
			uint16_t checksum_lo; /* crc32c(uuid+inum+inode) LE */
			uint16_t reserved2;
		} linux2;
		struct {
			uint16_t reserved1;
			uint16_t mode_high;
			uint16_t uid_high;
			uint16_t gid_high;
			uint32_t author;
		} hurd2;
	} osd2;

	uint16_t extra_isize;
	uint16_t checksum_hi;	/* crc32c(uuid+inum+inode) BE */
	uint32_t ctime_extra; /* Extra change time (nsec << 2 | epoch) */
	uint32_t mtime_extra; /* Extra Modification time (nsec << 2 | epoch) */
	uint32_t atime_extra; /* Extra Access time (nsec << 2 | epoch) */
	uint32_t crtime;      /* File creation time */
	uint32_t
	    crtime_extra;    /* Extra file creation time (nsec << 2 | epoch) */
	uint32_t version_hi; /* High 32 bits for 64-bit version */
};

#pragma pack(pop)

#define EXT4_INODE_MODE_FIFO 0x1000
#define EXT4_INODE_MODE_CHARDEV 0x2000
#define EXT4_INODE_MODE_DIRECTORY 0x4000
#define EXT4_INODE_MODE_BLOCKDEV 0x6000
#define EXT4_INODE_MODE_FILE 0x8000
#define EXT4_INODE_MODE_SOFTLINK 0xA000
#define EXT4_INODE_MODE_SOCKET 0xC000
#define EXT4_INODE_MODE_TYPE_MASK 0xF000

/*
 * Inode flags
 */
#define EXT4_INODE_FLAG_SECRM 0x00000001     /* Secure deletion */
#define EXT4_INODE_FLAG_UNRM 0x00000002      /* Undelete */
#define EXT4_INODE_FLAG_COMPR 0x00000004     /* Compress file */
#define EXT4_INODE_FLAG_SYNC 0x00000008      /* Synchronous updates */
#define EXT4_INODE_FLAG_IMMUTABLE 0x00000010 /* Immutable file */
#define EXT4_INODE_FLAG_APPEND 0x00000020  /* writes to file may only append */
#define EXT4_INODE_FLAG_NODUMP 0x00000040  /* do not dump file */
#define EXT4_INODE_FLAG_NOATIME 0x00000080 /* do not update atime */

/* Compression flags */
#define EXT4_INODE_FLAG_DIRTY 0x00000100
#define EXT4_INODE_FLAG_COMPRBLK                                               \
	0x00000200			   /* One or more compressed clusters */
#define EXT4_INODE_FLAG_NOCOMPR 0x00000400 /* Don't compress */
#define EXT4_INODE_FLAG_ECOMPR 0x00000800  /* Compression error */

#define EXT4_INODE_FLAG_INDEX 0x00001000  /* hash-indexed directory */
#define EXT4_INODE_FLAG_IMAGIC 0x00002000 /* AFS directory */
#define EXT4_INODE_FLAG_JOURNAL_DATA                                           \
	0x00004000			  /* File data should be journaled */
#define EXT4_INODE_FLAG_NOTAIL 0x00008000 /* File tail should not be merged */
#define EXT4_INODE_FLAG_DIRSYNC                                                \
	0x00010000 /* Dirsync behaviour (directories only) */
#define EXT4_INODE_FLAG_TOPDIR 0x00020000    /* Top of directory hierarchies */
#define EXT4_INODE_FLAG_HUGE_FILE 0x00040000 /* Set to each huge file */
#define EXT4_INODE_FLAG_EXTENTS 0x00080000   /* Inode uses extents */
#define EXT4_INODE_FLAG_EA_INODE 0x00200000  /* Inode used for large EA */
#define EXT4_INODE_FLAG_EOFBLOCKS 0x00400000 /* Blocks allocated beyond EOF */
#define EXT4_INODE_FLAG_RESERVED 0x80000000  /* reserved for ext4 lib */

// #define EXT4_INODE_ROOT_INDEX 2
#define EXT4_INODE_ROOT_INDEX EXT4_ROOT_INO


#define EXT4_DIRECTORY_FILENAME_LEN 255

/**@brief   Directory entry types. */
enum { EXT4_DE_UNKNOWN = 0,
       EXT4_DE_REG_FILE,
       EXT4_DE_DIR,
       EXT4_DE_CHRDEV,
       EXT4_DE_BLKDEV,
       EXT4_DE_FIFO,
       EXT4_DE_SOCK,
       EXT4_DE_SYMLINK };

/**
 * @brief Get lwext4 file type from mode.
 * 
 * @param mode
 * @return int 
 */
static inline int get_filetype(mode_t mode)
{
	if (S_ISREG(mode))
		return EXT4_DE_REG_FILE;
	else if (S_ISDIR(mode))
		return EXT4_DE_DIR;
	else if (S_ISCHR(mode))
		return EXT4_DE_CHRDEV;
	else if (S_ISBLK(mode))
		return EXT4_DE_BLKDEV;
	else if (S_ISFIFO(mode))
		return EXT4_DE_FIFO;
	else if (S_ISSOCK(mode))
		return EXT4_DE_SOCK;
	else if (S_ISLNK(mode))
		return EXT4_DE_SYMLINK;
	else
		return EXT4_DE_UNKNOWN;

}

/**
 * Convert kernel mode to lwext4 mode
 * @param kernel_mode Kernel mode bits (S_IFMT, S_IRUSR, etc.)
 * @return lwext4 mode bits
 */
static inline uint32_t mode_k2e(uint32_t kernel_mode)
{
    uint32_t lwext4_mode = 0;
    
    // Convert file type
    switch (kernel_mode & S_IFMT) {
        case S_IFREG:
            lwext4_mode |= EXT4_INODE_MODE_FILE;
            break;
        case S_IFDIR:
            lwext4_mode |= EXT4_INODE_MODE_DIRECTORY;
            break;
        case S_IFLNK:
            lwext4_mode |= EXT4_INODE_MODE_SOFTLINK;
            break;
        case S_IFCHR:
            lwext4_mode |= EXT4_INODE_MODE_CHARDEV;
            break;
        case S_IFBLK:
            lwext4_mode |= EXT4_INODE_MODE_BLOCKDEV;
            break;
        case S_IFIFO:
            lwext4_mode |= EXT4_INODE_MODE_FIFO;
            break;
        case S_IFSOCK:
            lwext4_mode |= EXT4_INODE_MODE_SOCKET;
            break;
    }

    // Convert permissions (lower 12 bits are identical)
    lwext4_mode |= (kernel_mode & 0xFFF);
    return lwext4_mode;
}

/**
 * Convert lwext4 mode to kernel mode
 * @param lwext4_mode lwext4 mode bits
 * @return kernel mode bits
 */
static inline uint32_t mode_e2k(uint32_t lwext4_mode)
{
    uint32_t kernel_mode = 0;
    
    // Convert file type
    switch (lwext4_mode & EXT4_INODE_MODE_TYPE_MASK) {
        case EXT4_INODE_MODE_FILE:
            kernel_mode |= S_IFREG;
            break;
        case EXT4_INODE_MODE_DIRECTORY:
            kernel_mode |= S_IFDIR;
            break;
        case EXT4_INODE_MODE_SOFTLINK:
            kernel_mode |= S_IFLNK;
            break;
        case EXT4_INODE_MODE_CHARDEV:
            kernel_mode |= S_IFCHR;
            break;
        case EXT4_INODE_MODE_BLOCKDEV:
            kernel_mode |= S_IFBLK;
            break;
        case EXT4_INODE_MODE_FIFO:
            kernel_mode |= S_IFIFO;
            break;
        case EXT4_INODE_MODE_SOCKET:
            kernel_mode |= S_IFSOCK;
            break;
    }

    // Convert permissions (lower 12 bits are identical)
    kernel_mode |= (lwext4_mode & 0xFFF);
    return kernel_mode;
}

#define EXT4_DIRENTRY_DIR_CSUM 0xDE

#pragma pack(push, 1)

union ext4_dir_en_internal {
	uint8_t name_length_high; /* Higher 8 bits of name length */
	uint8_t inode_type;       /* Type of referenced inode (in rev >= 0.5) */
};

/**
 * Linked list directory entry structure
 */
struct ext4_dir_en {
	uint32_t inode;	/* I-node for the entry */
	uint16_t entry_len; /* Distance to the next directory entry */
	uint8_t name_len;   /* Lower 8 bits of name length */

	union ext4_dir_en_internal in;
	uint8_t name[]; /* Entry name */
};

/* Structures for indexed directory */

struct ext4_dir_idx_climit {
	uint16_t limit;
	uint16_t count;
};

struct ext4_dir_idx_dot_en {
	uint32_t inode;
	uint16_t entry_length;
	uint8_t name_length;
	uint8_t inode_type;
	uint8_t name[4];
};

struct ext4_dir_idx_rinfo {
	uint32_t reserved_zero;
	uint8_t hash_version;
	uint8_t info_length;
	uint8_t indirect_levels;
	uint8_t unused_flags;
};

struct ext4_dir_idx_entry {
	uint32_t hash;
	uint32_t block;
};

struct ext4_dir_idx_root {
	struct ext4_dir_idx_dot_en dots[2];
	struct ext4_dir_idx_rinfo info;
	struct ext4_dir_idx_entry en[];
};

struct ext4_fake_dir_entry {
	uint32_t inode;
	uint16_t entry_length;
	uint8_t name_length;
	uint8_t inode_type;
};

struct ext4_dir_idx_node {
	struct ext4_fake_dir_entry fake;
	struct ext4_dir_idx_entry entries[];
};

/*
 * This goes at the end of each htree block.
 */
struct ext4_dir_idx_tail {
	uint32_t reserved;
	uint32_t checksum;	/* crc32c(uuid+inum+dirblock) */
};

/*
 * This is a bogus directory entry at the end of each leaf block that
 * records checksums.
 */
struct ext4_dir_entry_tail {
	uint32_t reserved_zero1;	/* Pretend to be unused */
	uint16_t rec_len;		/* 12 */
	uint8_t reserved_zero2;	/* Zero name length */
	uint8_t reserved_ft;	/* 0xDE, fake file type */
	uint32_t checksum;		/* crc32c(uuid+inum+dirblock) */
};

#pragma pack(pop)

#define EXT4_DIRENT_TAIL(block, blocksize) \
	((struct ext4_dir_entry_tail *)(((char *)(block)) + ((blocksize) - \
					sizeof(struct ext4_dir_entry_tail))))

#define EXT4_ERR_BAD_DX_DIR (-25000)

#define EXT4_LINK_MAX 65000

#define EXT4_BAD_INO 2 // Changed from 1 to 2
#define EXT4_ROOT_INO 1 /* Changed from 2 to 1. Current Oxbow limitation: Root inode should be 1.
			 * Should be same with OXBOW_ROOT_INO in oxbow_kernel.h
			 */
#define EXT4_BOOT_LOADER_INO 5
#define EXT4_UNDEL_DIR_INO 6
#define EXT4_RESIZE_INO 7
#define EXT4_JOURNAL_INO 8

#define EXT4_GOOD_OLD_FIRST_INO 11
#define EXT_MAX_BLOCKS (ext4_lblk_t) (-1)
#define IN_RANGE(b, first, len)	((b) >= (first) && (b) <= (first) + (len) - 1)


/******************************************************************************/

/* EXT3 HTree directory indexing */
#define EXT2_HTREE_LEGACY 0
#define EXT2_HTREE_HALF_MD4 1
#define EXT2_HTREE_TEA 2
#define EXT2_HTREE_LEGACY_UNSIGNED 3
#define EXT2_HTREE_HALF_MD4_UNSIGNED 4
#define EXT2_HTREE_TEA_UNSIGNED 5

#define EXT2_HTREE_EOF 0x7FFFFFFFUL

#define EXT4_GOOD_OLD_INODE_SIZE	128

/*****************************************************************************/

/*
 * JBD stores integers in big endian.
 */

#define JBD_MAGIC_NUMBER 0xc03b3998U /* The first 4 bytes of /dev/random! */

/*
 * Descriptor block types:
 */

#define JBD_DESCRIPTOR_BLOCK	1
#define JBD_COMMIT_BLOCK	2
#define JBD_SUPERBLOCK		3
#define JBD_SUPERBLOCK_V2	4
#define JBD_REVOKE_BLOCK	5

#pragma pack(push, 1)

/*
 * Standard header for all descriptor blocks:
 */
struct jbd_bhdr {
	uint32_t		magic;
	uint32_t		blocktype;
	uint32_t		sequence;
};

#pragma pack(pop)

/*
 * Checksum types.
 */
#define JBD_CRC32_CHKSUM   1
#define JBD_MD5_CHKSUM     2
#define JBD_SHA1_CHKSUM    3
#define JBD_CRC32C_CHKSUM  4

#define JBD_CRC32_CHKSUM_SIZE 4

#define JBD_CHECKSUM_BYTES (32 / sizeof(uint32_t))

#pragma pack(push, 1)

/*
 * Commit block header for storing transactional checksums:
 *
 * NOTE: If FEATURE_COMPAT_CHECKSUM (checksum v1) is set, the h_chksum*
 * fields are used to store a checksum of the descriptor and data blocks.
 *
 * If FEATURE_INCOMPAT_CSUM_V2 (checksum v2) is set, then the h_chksum
 * field is used to store crc32c(uuid+commit_block).  Each journal metadata
 * block gets its own checksum, and data block checksums are stored in
 * journal_block_tag (in the descriptor).  The other h_chksum* fields are
 * not used.
 *
 * If FEATURE_INCOMPAT_CSUM_V3 is set, the descriptor block uses
 * journal_block_tag3_t to store a full 32-bit checksum.  Everything else
 * is the same as v2.
 *
 * Checksum v1, v2, and v3 are mutually exclusive features.
 */

struct jbd_commit_header {
	struct jbd_bhdr header;
	uint8_t chksum_type;
	uint8_t chksum_size;
	uint8_t padding[2];
	uint32_t		chksum[JBD_CHECKSUM_BYTES];
	uint64_t		commit_sec;
	uint32_t		commit_nsec;
};

/*
 * The block tag: used to describe a single buffer in the journal
 */
struct jbd_block_tag3 {
	uint32_t		blocknr;	/* The on-disk block number */
	uint32_t		flags;	/* See below */
	uint32_t		blocknr_high; /* most-significant high 32bits. */
	uint32_t		checksum;	/* crc32c(uuid+seq+block) */
};

struct jbd_block_tag {
	uint32_t		blocknr;	/* The on-disk block number */
	uint16_t		checksum;	/* truncated crc32c(uuid+seq+block) */
	uint16_t		flags;	/* See below */
	uint32_t		blocknr_high; /* most-significant high 32bits. */
};

#pragma pack(pop)

/* Definitions for the journal tag flags word: */
#define JBD_FLAG_ESCAPE		1	/* on-disk block is escaped */
#define JBD_FLAG_SAME_UUID	2	/* block has same uuid as previous */
#define JBD_FLAG_DELETED	4	/* block deleted by this transaction */
#define JBD_FLAG_LAST_TAG	8	/* last tag in this descriptor block */

#pragma pack(push, 1)

/* Tail of descriptor block, for checksumming */
struct jbd_block_tail {
	uint32_t	checksum;
};

/*
 * The revoke descriptor: used on disk to describe a series of blocks to
 * be revoked from the log
 */
struct jbd_revoke_header {
	struct jbd_bhdr  header;
	uint32_t	 count;	/* Count of bytes used in the block */
};

/* Tail of revoke block, for checksumming */
struct jbd_revoke_tail {
	uint32_t		checksum;
};

#pragma pack(pop)

#define JBD_USERS_MAX 48
#define JBD_USERS_SIZE (UUID_SIZE * JBD_USERS_MAX)

#pragma pack(push, 1)

/*
 * The journal superblock.  All fields are in big-endian byte order.
 */
struct jbd_sb {
/* 0x0000 */
	struct jbd_bhdr header;

/* 0x000C */
	/* Static information describing the journal */
	uint32_t	blocksize;		/* journal device blocksize */
	uint32_t	maxlen;		/* total blocks in journal file */
	uint32_t	first;		/* first block of log information */

/* 0x0018 */
	/* Dynamic information describing the current state of the log */
	uint32_t	sequence;		/* first commit ID expected in log */
	uint32_t	start;		/* blocknr of start of log */

/* 0x0020 */
	/* Error value, as set by journal_abort(). */
	int32_t 	error_val;

/* 0x0024 */
	/* Remaining fields are only valid in a version-2 superblock */
	uint32_t	feature_compat; 	/* compatible feature set */
	uint32_t	feature_incompat; 	/* incompatible feature set */
	uint32_t	feature_ro_compat; 	/* readonly-compatible feature set */
/* 0x0030 */
	uint8_t 	uuid[UUID_SIZE];		/* 128-bit uuid for journal */

/* 0x0040 */
	uint32_t	nr_users;		/* Nr of filesystems sharing log */

	uint32_t	dynsuper;		/* Blocknr of dynamic superblock copy*/

/* 0x0048 */
	uint32_t	max_transaction;	/* Limit of journal blocks per trans.*/
	uint32_t	max_trandata;	/* Limit of data blocks per trans. */

/* 0x0050 */
	uint8_t 	checksum_type;	/* checksum type */
	uint8_t 	padding2[3];
	uint32_t	padding[42];
	uint32_t	checksum;		/* crc32c(superblock) */

/* 0x0100 */
	uint8_t 	users[JBD_USERS_SIZE];		/* ids of all fs'es sharing the log */

/* 0x0400 */
};

#pragma pack(pop)

#define JBD_SUPERBLOCK_SIZE sizeof(struct jbd_sb)

#define JBD_HAS_COMPAT_FEATURE(jsb,mask)					\
	((jsb)->header.blocktype >= to_be32(2) &&				\
	 ((jsb)->feature_compat & to_be32((mask))))
#define JBD_HAS_RO_COMPAT_FEATURE(jsb,mask)				\
	((jsb)->header.blocktype >= to_be32(2) &&				\
	 ((jsb)->feature_ro_compat & to_be32((mask))))
#define JBD_HAS_INCOMPAT_FEATURE(jsb,mask)				\
	((jsb)->header.blocktype >= to_be32(2) &&				\
	 ((jsb)->feature_incompat & to_be32((mask))))

#define JBD_FEATURE_COMPAT_CHECKSUM	0x00000001

#define JBD_FEATURE_INCOMPAT_REVOKE		0x00000001
#define JBD_FEATURE_INCOMPAT_64BIT		0x00000002
#define JBD_FEATURE_INCOMPAT_ASYNC_COMMIT	0x00000004
#define JBD_FEATURE_INCOMPAT_CSUM_V2		0x00000008
#define JBD_FEATURE_INCOMPAT_CSUM_V3		0x00000010

/* Features known to this kernel version: */
#define JBD_KNOWN_COMPAT_FEATURES	0
#define JBD_KNOWN_ROCOMPAT_FEATURES	0
#define JBD_KNOWN_INCOMPAT_FEATURES	(JBD_FEATURE_INCOMPAT_REVOKE|\
					 JBD_FEATURE_INCOMPAT_ASYNC_COMMIT|\
					 JBD_FEATURE_INCOMPAT_64BIT|\
					 JBD_FEATURE_INCOMPAT_CSUM_V2|\
					 JBD_FEATURE_INCOMPAT_CSUM_V3)

/*****************************************************************************/

#define EXT4_CRC32_INIT (0xFFFFFFFFUL)

/*****************************************************************************/

#ifdef __cplusplus
}
#endif


#if CONFIG_USE_USER_MALLOC

#define ext4_malloc  ext4_user_malloc
#define ext4_calloc  ext4_user_calloc
#define ext4_realloc ext4_user_realloc
#define ext4_free    ext4_user_free

#else

#define ext4_malloc  malloc
#define ext4_calloc  calloc
#define ext4_realloc realloc
#define ext4_free    free

#endif

/* Oxbow related. */
#define EXT4_SB(sb) ((struct ext4_sblock *)sb->s_fs_info)

// Get struct ext4_inode_ref from the struct inode.
#define EXT4_INODE(inode)                                                      \
	(container_of(inode, struct ext4_inode_ref, vfs_inode))

#endif /* EXT4_TYPES_H_ */

/**
 * @}
 */
