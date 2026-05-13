#include <stdio.h>
#include <stdlib.h>
#include "utils/print_tx.h"
#include "journal.h"

int main(int argc, char **argv)
{
	printf("Testing print_tx data block type detection\n");
	printf("==========================================\n\n");

	printf("Available dump options:\n");
	printf("  DUMP_SUPER_BLK        (0x%02x) - Dump journal superblocks\n",
	       DUMP_SUPER_BLK);
	printf("  DUMP_DESC_BLK         (0x%02x) - Dump descriptor blocks\n",
	       DUMP_DESC_BLK);
	printf("  DUMP_COMMIT_BLK       (0x%02x) - Dump commit blocks\n",
	       DUMP_COMMIT_BLK);
	printf("  DUMP_ETAG_BLK         (0x%02x) - Dump extent tag blocks\n",
	       DUMP_ETAG_BLK);
	printf("  DUMP_STAGE_TRACE_BLK  (0x%02x) - Dump stage trace blocks\n",
	       DUMP_STAGE_TRACE_BLK);
	printf("  DUMP_STAGE_DESC_BLK   (0x%02x) - Dump stage descriptor blocks\n",
	       DUMP_STAGE_DESC_BLK);
	printf("  DUMP_DATA_BLOCKS      (0x%02x) - Dump all data blocks\n",
	       DUMP_DATA_BLOCKS);
	printf("  DUMP_DIRECTORY_BLOCKS (0x%02x) - Dump directory blocks only\n",
	       DUMP_DIRECTORY_BLOCKS);
	printf("  DUMP_INODE_BLOCKS     (0x%02x) - Dump inode blocks only\n",
	       DUMP_INODE_BLOCKS);
	printf("  DUMP_EXT4_EXTENT_BLOCKS (0x%02x) - Dump EXT4 extent blocks only\n",
	       DUMP_EXT4_EXTENT_BLOCKS);
	printf("\n  Filesystem-specific options:\n");
	printf("  DUMP_EXT4_BLOCKS           (0x%02x) - Dump EXT4-specific blocks\n",
	       DUMP_EXT4_BLOCKS);
	printf("  DUMP_SEFS_BLOCKS           (0x%02x) - Dump SEFS-specific blocks\n",
	       DUMP_SEFS_BLOCKS);
	printf("  DUMP_EXT4_DIRECTORY_BLOCKS (0x%02x) - Dump EXT4 directory blocks\n",
	       DUMP_EXT4_DIRECTORY_BLOCKS);
	printf("  DUMP_SEFS_DIRECTORY_BLOCKS (0x%02x) - Dump SEFS directory blocks\n",
	       DUMP_SEFS_DIRECTORY_BLOCKS);
	printf("  DUMP_SEFS_INODE_BLOCKS     (0x%02x) - Dump SEFS inode blocks\n",
	       DUMP_SEFS_INODE_BLOCKS);
	printf("  DUMP_SEFS_SUPERBLOCKS      (0x%02x) - Dump SEFS superblocks\n",
	       DUMP_SEFS_SUPERBLOCKS);
	printf("\n  EXT4 filesystem structure options:\n");
	printf("  DUMP_EXT4_BLOCK_BITMAP     (0x%02x) - Dump EXT4 block bitmap blocks\n",
	       DUMP_EXT4_BLOCK_BITMAP);
	printf("  DUMP_EXT4_INODE_BITMAP     (0x%02x) - Dump EXT4 inode bitmap blocks\n",
	       DUMP_EXT4_INODE_BITMAP);
	printf("  DUMP_EXT4_INODE_TABLE      (0x%02x) - Dump EXT4 inode table blocks\n",
	       DUMP_EXT4_INODE_TABLE);
	printf("  DUMP_EXT4_RESERVED_GDT     (0x%02x) - Dump EXT4 reserved GDT blocks\n",
	       DUMP_EXT4_RESERVED_GDT);
	printf("\n");

	printf("Usage examples:\n");
	printf("  To dump all blocks: print_tx(ce, -1, 0xFFFF);\n");
	printf("  To dump only directory blocks: print_tx(ce, -1, DUMP_DIRECTORY_BLOCKS);\n");
	printf("  To dump only inode blocks: print_tx(ce, -1, DUMP_INODE_BLOCKS);\n");
	printf("  To dump only EXT4 extent blocks: print_tx(ce, -1, DUMP_EXT4_EXTENT_BLOCKS);\n");
	printf("  To dump descriptors + directories: print_tx(ce, -1, DUMP_DESC_BLK | DUMP_DIRECTORY_BLOCKS);\n");
	printf("  To dump specific block address: print_tx(ce, 12345, 0);\n");
	printf("  To dump only EXT4 blocks: print_tx(ce, -1, DUMP_EXT4_BLOCKS);\n");
	printf("  To dump only SEFS directory blocks: print_tx(ce, -1, DUMP_SEFS_DIRECTORY_BLOCKS);\n");
	printf("  To dump EXT4 inode table: print_tx(ce, -1, DUMP_EXT4_INODE_TABLE);\n");
	printf("  To dump EXT4 inode table + SEFS inodes: print_tx(ce, -1, DUMP_EXT4_INODE_TABLE | DUMP_SEFS_INODE_BLOCKS);\n");
	printf("  To dump SEFS superblocks: print_tx(ce, -1, DUMP_SEFS_SUPERBLOCKS);\n");
	printf("\n");

	printf("Data block type detection capabilities:\n");
	printf("  - Detects EXT4 directory entry blocks by parsing directory entries\n");
	printf("  - Detects EXT4 inode blocks by checking file mode and size fields\n");
	printf("  - Detects SEFS directory entry blocks (similar structure to EXT4)\n");
	printf("  - Detects SEFS inode blocks (based on mkfs.c analysis with accurate structure)\n");
	printf("  - Detects SEFS superblocks by magic number validation\n");
	printf("  - Identifies unallocated/zero blocks\n");
	printf("  - Identifies text/data blocks with printable content\n");
	printf("  - Shows directory entry details (inode, name, type for EXT4/generic for SEFS)\n");
	printf("  - Shows inode details (mode, file type, size) with filesystem-specific formatting\n");
	printf("  - Shows SEFS superblock layout and region information\n");
	printf("  - Supports different inode sizes for different filesystems (EXT4: 128B, SEFS: ~64B)\n");
	printf("\n");

	printf("NEW: Address-based block type prediction and validation:\n");
	printf("  - Uses transaction tags to extract target block addresses\n");
	printf("  - Predicts block type based on filesystem layout (inode vs data regions)\n");
	printf("  - Validates content against address-based prediction\n");
	printf("  - Detects data consistency bugs when content doesn't match expected type\n");
	printf("  - Shows filesystem region information for debugging\n");
	printf("  - Works with both EXT4 and SEFS filesystems via journal_operations\n");
	printf("  - Output format: 'baddr X: Data Block #Y (content: Z) -> target=0xABC (predicted: DEF) [VALID/ERROR]'\n");
	printf("  - When validation fails, shows detailed error information and region boundaries\n");
	printf("\n");

	return 0;
}