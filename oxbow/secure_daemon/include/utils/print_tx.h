#ifndef _PRINT_TX_H_
#define _PRINT_TX_H_

#include "common/sjd.h"
#include "fs.h"
#include <stdint.h>

// Bitmask for block types to dump, using enum j_block_type values
#define DUMP_SUPER_BLK (1 << SUPER_BLK)
#define DUMP_DESC_BLK (1 << DESC_BLK)
#define DUMP_COMMIT_BLK (1 << COMMIT_BLK)
#define DUMP_ETAG_BLK (1 << ETAG_BLK)
#define DUMP_STAGE_TRACE_BLK (1 << STAGE_TRACE_BLK)
#define DUMP_STAGE_DESC_BLK (1 << STAGE_DESC_BLK)

// Additional bitmasks for data block types (using higher bits)
#define DUMP_DATA_BLOCKS (1 << 7) // Dump all data blocks
#define DUMP_DIRECTORY_BLOCKS (1 << 8) // Dump directory blocks specifically
#define DUMP_INODE_BLOCKS (1 << 9) // Dump inode blocks specifically

// Filesystem-specific dump options
#define DUMP_EXT4_BLOCKS (1 << 10) // Dump EXT4-specific blocks
#define DUMP_SEFS_BLOCKS (1 << 11) // Dump SEFS-specific blocks
#define DUMP_EXT4_DIRECTORY_BLOCKS (1 << 12) // Dump EXT4 directory blocks
#define DUMP_SEFS_DIRECTORY_BLOCKS (1 << 14) // Dump SEFS directory blocks
#define DUMP_SEFS_INODE_BLOCKS (1 << 15) // Dump SEFS inode blocks
#define DUMP_SEFS_SUPERBLOCKS (1 << 16) // Dump SEFS superblocks

// EXT4 filesystem structure dump options
#define DUMP_EXT4_BLOCK_BITMAP (1 << 17) // Dump EXT4 block bitmap blocks
#define DUMP_EXT4_INODE_BITMAP (1 << 18) // Dump EXT4 inode bitmap blocks
#define DUMP_EXT4_INODE_TABLE (1 << 19) // Dump EXT4 inode table blocks
#define DUMP_EXT4_RESERVED_GDT (1 << 20) // Dump EXT4 reserved GDT blocks
#define DUMP_EXT4_EXTENT_BLOCKS (1 << 21) // Dump EXT4 extent tree blocks

/**
 * @brief 
 * 
 * @param tx 
 * @param dump_baddr If a specific block address is provided, dump that block.
 * @param dump_block_types Bitmask of block types to dump.
 */
void print_jnl_tx(journal_tx *tx, uint64_t dump_baddr,
	      uint32_t dump_block_types);

/**
 * @brief 
 * 
 * @param tx 
 * @param inode Associated inode for extent tree printing
 * @param types_to_dump Bitmask of block types to dump.
 */
void print_stage_tx(stage_tx *tx, struct inode *inode, uint32_t types_to_dump);

#endif /* _PRINT_TX_H_ */

