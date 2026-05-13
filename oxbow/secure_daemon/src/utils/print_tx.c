#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <arpa/inet.h>
#include <endian.h>
#include <ctype.h>

#include "utils/print_tx.h"
#include "fs/journal.h"
#include "fs/fs.h"
#include "buffer_head.h"
#include "common/sjd.h"
#include "common/oxbow.h"

// For EXT4
#include "fs/lwext4/ext4_config.h"
#include "fs/lwext4/ext4_types.h"
#include "fs/lwext4/ext4_super.h"
#include "fs/lwext4/ext4_misc.h"
#include "fs/lwext4/ext4_block_group.h"
#include "fs/lwext4/ext4_inode.h"
#include "fs/lwext4/ext4_dir.h"
#include "fs/lwext4/ext4_dir_idx.h"
#include "fs/lwext4/ext4_extent.h"
#include "fs/lwext4/ext4.h"
#include "fs/lwext4/ext4_balloc.h"

// For SEFS
#include "fs/sefs/sefs.h"

#define EXT4_EXTENT_MAGIC 0xF30A
#define SEFS_FILENAME_LEN 255
#define EXT4_INODE_FLAG_EXTENTS 0x00080000

struct ext4_extent_header {
    uint16_t magic;
    uint16_t entries_count;
    uint16_t max_entries_count;
    uint16_t depth;
    uint32_t generation;
};

struct ext4_extent {
    uint32_t first_block;
    uint16_t block_count;
    uint16_t start_hi;
    uint32_t start_lo;
};

struct sefs_dir_entry {
	uint32_t d_ino;
	char d_name[SEFS_FILENAME_LEN];
};

enum {
	FS_TYPE_UNKNOWN,
	FS_TYPE_EXT4,
	FS_TYPE_SEFS,
};

static struct super_block *g_sb;
static void *g_sb_info; // This will be either sefs_sb_info or ext4_sblock.
static int g_fs_type;

// Function declarations for data block type detection
static int is_ext4_directory_block(const unsigned char *data);
static int is_ext4_extent_block(const unsigned char *data);
static int is_ext4_group_descriptor_block(baddr_t target_addr);
static int is_ext4_block_bitmap_block(baddr_t target_addr);
static int is_ext4_inode_bitmap_block(baddr_t target_addr);
static int is_ext4_inode_table_block(baddr_t target_addr);
static int is_ext4_reserved_gdt_block(baddr_t target_addr);
static int is_sefs_inode_block(const unsigned char *data);
static int is_sefs_directory_block(const unsigned char *data);
static int is_inode_block(const unsigned char *data);
static int is_directory_block(const unsigned char *data);
static const char *get_data_block_type(const unsigned char *data,
				       baddr_t target_addr);
static void dump_hex(const unsigned char *data, size_t size);

static int should_dump(baddr_t baddr, enum j_block_type btype,
		       const char *data_block_type_str, uint64_t baddr_to_dump,
		       uint32_t types_to_dump)
{
	if (baddr_to_dump != 0) {
		return (baddr == baddr_to_dump);
	}

	if (types_to_dump == 0) {
		return 0; // Don't dump anything if no flags are set
	}

	// Check for journal metadata types
	if (btype != 0) {
		return (types_to_dump & (1 << btype));
	}

	// Handle Data blocks (btype == 0)

	// First, check for specific type matches ONLY if the type is known.
	if (data_block_type_str && strcmp(data_block_type_str, "Unknown Data") != 0) {
		// Filesystem-wide data block flag
		if ((types_to_dump & DUMP_EXT4_BLOCKS) &&
		    strstr(data_block_type_str, "EXT4"))
			return 1;
		if ((types_to_dump & DUMP_SEFS_BLOCKS) &&
		    strstr(data_block_type_str, "SEFS"))
			return 1;

		// Generic type data block flag
		if ((types_to_dump & DUMP_DIRECTORY_BLOCKS) &&
		    strstr(data_block_type_str, "Directory"))
			return 1;
		if ((types_to_dump & DUMP_INODE_BLOCKS) &&
		    (strstr(data_block_type_str, "Inode Table") ||
		     strstr(data_block_type_str, "Inode Store")))
			return 1;

		// Specific type data block flags
		if ((types_to_dump & DUMP_EXT4_DIRECTORY_BLOCKS) &&
		    strcmp(data_block_type_str, "EXT4 Directory") == 0)
			return 1;
		if ((types_to_dump & DUMP_SEFS_DIRECTORY_BLOCKS) &&
		    strcmp(data_block_type_str, "SEFS Directory") == 0)
			return 1;
		if ((types_to_dump & DUMP_SEFS_INODE_BLOCKS) &&
		    strcmp(data_block_type_str, "SEFS Inode Store") == 0)
			return 1;
		if ((types_to_dump & DUMP_EXT4_BLOCK_BITMAP) &&
		    strcmp(data_block_type_str, "EXT4 Block Bitmap") == 0)
			return 1;
		if ((types_to_dump & DUMP_EXT4_INODE_BITMAP) &&
		    strcmp(data_block_type_str, "EXT4 Inode Bitmap") == 0)
			return 1;
		if ((types_to_dump & DUMP_EXT4_INODE_TABLE) &&
		    strcmp(data_block_type_str, "EXT4 Inode Table") == 0)
			return 1;
		if ((types_to_dump & DUMP_EXT4_RESERVED_GDT) &&
		    strcmp(data_block_type_str, "EXT4 Reserved GDT") == 0)
			return 1;
		if ((types_to_dump & DUMP_EXT4_EXTENT_BLOCKS) &&
		    strcmp(data_block_type_str, "EXT4 Extent Tree Block") == 0)
			return 1;
	}

	// For unknown types, or known types that didn't match a specific flag,
	// only dump if the generic DUMP_DATA_BLOCKS flag is set.
	return (types_to_dump & DUMP_DATA_BLOCKS) ? 1 : 0;
}

static void print_etag(etag_t *tag)
{
	printf("    tag: start_lba=%-10lu, count=%u\n", tag->start, tag->cnt);
}

static void dump_journal_descriptor_block(const unsigned char *data)
{
	struct journal_descriptor_block *jdb =
		(struct journal_descriptor_block *)data;
	printf("[Descriptor Block (type=%u)]\n", jdb->h.blocktype);
	printf("  tx_id: %u\n", jdb->h.transaction_id);
	printf("  nr_etag_blks: %u\n", jdb->h.nr_etag_blks);
	printf("  nr_stage_trace_blks: %u\n", jdb->h.nr_stage_trace_blks);
	printf("  nr_tags: %u\n", jdb->h.nr_tags);
	for (uint32_t i = 0; i < jdb->h.nr_tags; i++) {
		print_etag(&jdb->tags[i]);
	}
}

static void dump_journal_extent_tag_block(const unsigned char *data)
{
	struct journal_extent_tag_block *etb =
		(struct journal_extent_tag_block *)data;
	printf("[Extent Tag Block (type=%u)]\n", etb->h.blocktype);
	printf("  nr_tags: %u\n", etb->h.nr);
	for (uint32_t i = 0; i < etb->h.nr; i++) {
		print_etag(&etb->tags[i]);
	}
}

static void dump_stage_trace_block(const unsigned char *data)
{
	struct journal_stage_trace_block *stb =
		(struct journal_stage_trace_block *)data;
	printf("[Stage Trace Block (type=%u)]\n", stb->h.blocktype);
	printf("  nr_stage_txs: %u\n", stb->h.nr);
	for (uint32_t i = 0; i < stb->h.nr; i++) {
		printf("  staged_tx_desc_baddr[%u]: %lu\n", i, stb->tx_list[i]);
	}
}

static void dump_ext4_inode_table_block(const unsigned char *data,
					baddr_t target_addr)
{
	struct ext4_sblock *ext4_sb = (struct ext4_sblock *)g_sb_info;
	if (ext4_sb->magic != EXT4_SUPERBLOCK_MAGIC) {
		printf("  EXT4 Inode Table Block\n");
		return;
	}

	/* Compute correct inode numbering based on filesystem layout */
	uint32_t block_size = 1024U << le32toh(ext4_sb->log_block_size);
	uint32_t inode_size = le16toh(ext4_sb->inode_size);
	uint32_t inodes_per_block = block_size / inode_size;
	uint32_t blocks_per_group = le32toh(ext4_sb->blocks_per_group);
	uint32_t inodes_per_group = le32toh(ext4_sb->inodes_per_group);
	uint32_t first_data_block = le32toh(ext4_sb->first_data_block);
	uint16_t desc_size = le16toh(ext4_sb->desc_size);
	uint16_t reserved_gdt = le16toh(ext4_sb->s_reserved_gdt_blocks);

	/* Number of block groups and group descriptor blocks */
	uint64_t total_blocks = ext4_sb_get_blocks_cnt(ext4_sb);
	uint32_t groups_count =
		(uint32_t)((total_blocks + blocks_per_group - 1) / blocks_per_group);
	uint32_t dsc_per_block = desc_size ? (block_size / desc_size) : 0;
	uint32_t bg_desc_blocks = (dsc_per_block == 0)
					? 0
					: (groups_count + dsc_per_block - 1) / dsc_per_block;

	/* Identify block group of target block.
	 * mkfs places groups starting at: first_data_block + first_data_block + g*blocks_per_group
	 */
	uint64_t group_base0 = (uint64_t)first_data_block + (uint64_t)first_data_block;
	uint32_t group = 0;
	if (target_addr >= group_base0 && blocks_per_group > 0) {
		group = (uint32_t)((target_addr - group_base0) / blocks_per_group);
		if (group >= groups_count)
			group = groups_count ? (groups_count - 1) : 0;
	}

	/* Whether group has backup superblock (affects offsets) */
	int has_sb = ext4_sb_is_super_in_bg(ext4_sb, group) ? 1 : 0;

	/* Compute inode table start block for this group */
	uint64_t group_base = group_base0 + (uint64_t)group * blocks_per_group;
	uint64_t inode_table_start = group_base + bg_desc_blocks + 3ULL;
	if (has_sb) {
		inode_table_start += 1ULL + (uint64_t)reserved_gdt;
	}

	/* Offset of this block within inode table of its group */
	uint64_t block_index_in_table = 0;
	if (target_addr >= inode_table_start)
		block_index_in_table = (uint64_t)(target_addr - inode_table_start);

	/* Base inode number for this block */
	uint64_t group_inode_base = (uint64_t)group * inodes_per_group;
	uint64_t inode_offset_start = group_inode_base + block_index_in_table * inodes_per_block;

	// Since is_ext4_inode_table_block() already verified this is an inode table block,
	// we don't need to recalculate the exact position. Just show block-relative info.
	printf("  EXT4 Inode Table Block (%u bytes/inode, %u inodes max, baddr=%lu):\n",
	       inode_size, inodes_per_block, target_addr);

	// Count and show allocated inodes
	uint32_t allocated_count = 0;
	for (uint32_t i = 0; i < inodes_per_block; i++) {
		const unsigned char *inode_ptr = data + (i * inode_size);
		uint16_t i_mode = *(uint16_t *)(inode_ptr + 0);
		uint16_t i_uid = *(uint16_t *)(inode_ptr + 2);
		uint32_t i_size = *(uint32_t *)(inode_ptr + 4);
		uint16_t i_gid = *(uint16_t *)(inode_ptr + 24);
		uint16_t i_links = *(uint16_t *)(inode_ptr + 26);
		uint32_t i_blocks = *(uint32_t *)(inode_ptr + 28);
		uint32_t i_flags = *(uint32_t *)(inode_ptr + 32);

		if (i_mode != 0) {
			uint64_t inode_number = (uint64_t)(i + 1) + inode_offset_start;
			allocated_count++;

			// File type with description
			const char *type_desc;
			switch (i_mode & 0xF000) {
			case 0x1000:
				type_desc = "FIFO";
				break;
			case 0x2000:
				type_desc = "CHAR";
				break;
			case 0x4000:
				type_desc = "DIR";
				break;
			case 0x6000:
				type_desc = "BLOCK";
				break;
			case 0x8000:
				type_desc = "FILE";
				break;
			case 0xA000:
				type_desc = "LINK";
				break;
			case 0xC000:
				type_desc = "SOCK";
				break;
			default:
				type_desc = "UNK";
				break;
			}

			// Permissions string
			char perms[10];
			sprintf(perms, "%c%c%c%c%c%c%c%c%c",
				(i_mode & 0400) ? 'r' : '-',
				(i_mode & 0200) ? 'w' : '-',
				(i_mode & 0100) ? 'x' : '-',
				(i_mode & 0040) ? 'r' : '-',
				(i_mode & 0020) ? 'w' : '-',
				(i_mode & 0010) ? 'x' : '-',
				(i_mode & 0004) ? 'r' : '-',
				(i_mode & 0002) ? 'w' : '-',
				(i_mode & 0001) ? 'x' : '-');

			printf("    Inode #%lu: %s %s uid=%u gid=%u size=%u links=%u blocks=%u",
			       (unsigned long)inode_number, type_desc, perms, i_uid, i_gid, i_size,
			       i_links, i_blocks);

			// Show modification time
			uint32_t i_mtime = *(uint32_t *)(inode_ptr + 16);
			if (i_mtime != 0) {
				time_t mtime = (time_t)i_mtime;
				struct tm *tm_info = localtime(&mtime);
				if (tm_info) {
					printf(" mtime=%04d-%02d-%02d_%02d:%02d",
					       tm_info->tm_year + 1900,
					       tm_info->tm_mon + 1,
					       tm_info->tm_mday,
					       tm_info->tm_hour,
					       tm_info->tm_min);
				}
			}
			printf("\n");

			// Print extent tree if this inode uses extents
			if (i_flags & EXT4_INODE_FLAG_EXTENTS) {
				printf("      [Extent Tree Analysis for Inode #%lu]\n", (unsigned long)inode_number);
				
				// Create a temporary ext4_inode_ref structure for extent tree printing
				struct ext4_inode_ref temp_inode_ref;
				memset(&temp_inode_ref, 0, sizeof(temp_inode_ref));
				
				// Set up basic fields needed for extent tree analysis
				temp_inode_ref.inode = (struct ext4_inode *)inode_ptr;
				temp_inode_ref.index = (uint32_t)inode_number; // Inode number.
				temp_inode_ref.fs = g_fs; // Use global ext4_fs instance
				
				// Call extent tree printing function (validation is handled inside)
				ext4_print_extent_tree(&temp_inode_ref, "INODE_TABLE_DUMP");
			} else {
				printf("      (Uses indirect blocks, not extents)\n");
			}
		}
	}

	printf("    Summary: %u allocated / %u total inodes in this block\n",
	       allocated_count, inodes_per_block);
}

/* Dump details of an EXT4 extent tree block (both leaf and index nodes) */
static void dump_ext4_extent_tree_block(const unsigned char *data,
					   baddr_t target_addr)
{
	const struct ext4_extent_header *eh =
		(const struct ext4_extent_header *)data;

	if (le16toh(eh->magic) != EXT4_EXTENT_MAGIC) {
		printf("  [Invalid EXT4 Extent Tree Block]\n");
		dump_hex(data, 128);
		return;
	}

	uint16_t entries = le16toh(eh->entries_count);
	uint16_t max_entries = le16toh(eh->max_entries_count);
	uint16_t depth = le16toh(eh->depth);
	uint32_t generation = le32toh(eh->generation);

	printf("  EXT4 Extent Tree Block (baddr=%lu)\n", target_addr);
	printf("    header: magic=0x%04x, entries=%u/%u, depth=%u, gen=%u\n",
	       le16toh(eh->magic), entries, max_entries, depth, generation);

	if (entries == 0) {
		printf("    (no entries)\n");
		return;
	}

	if (depth == 0) {
		/* Leaf node: array of extents */
		const struct ext4_extent *ext =
			(const struct ext4_extent *)(data + sizeof(struct ext4_extent_header));
		for (uint16_t i = 0; i < entries; i++) {
			uint32_t first_block = le32toh(ext[i].first_block);
			uint16_t block_count = le16toh(ext[i].block_count);
			uint64_t start = ((uint64_t)le16toh(ext[i].start_hi) << 32) |
					(uint64_t)le32toh(ext[i].start_lo);
			printf("    extent[%u]: lblock=%u, len=%u, start=%lu\n",
			       i, first_block, block_count, (unsigned long)start);
		}
	} else {
		/* Index node: array of index entries */
		struct ext4_extent_idx_local {
			uint32_t block;    /* first logical block that this index covers */
			uint32_t leaf_lo;  /* low 32 bits of physical block of the child */
			uint16_t leaf_hi;  /* high 16 bits of physical block of the child */
			uint16_t unused;
		};

		const struct ext4_extent_idx_local *idx =
			(const struct ext4_extent_idx_local *)(data + sizeof(struct ext4_extent_header));
		for (uint16_t i = 0; i < entries; i++) {
			uint32_t lblock = le32toh(idx[i].block);
			uint64_t child = ((uint64_t)le16toh(idx[i].leaf_hi) << 32) |
					 (uint64_t)le32toh(idx[i].leaf_lo);
			printf("    index[%u]: lblock=%u -> child_phys_block=%lu\n",
			       i, lblock, (unsigned long)child);
		}
	}
}

static int get_target_address_for_data_block(
	struct journal_descriptor_block *jdb, char *meta_start,
	size_t data_block_index, baddr_t *target_addr)
{
	size_t block_count = 0;
	char *cur = meta_start;

	for (uint32_t i = 0; i < jdb->h.nr_tags; i++) {
		if (data_block_index >= block_count &&
		    data_block_index < block_count + jdb->tags[i].cnt) {
			*target_addr =
				jdb->tags[i].start + (data_block_index - block_count);
			return 0;
		}
		block_count += jdb->tags[i].cnt;
	}

	cur += OXBOW_BLOCK_SIZE;

	/* After the descriptor block, there may be stage trace blocks and extent tag blocks.
	 * Scan up to (nr_etag_blks + nr_stage_trace_blks) meta blocks and process only
	 * ETAG_BLK blocks for data block mapping. */
	uint32_t blocks_to_scan = jdb->h.nr_etag_blks + jdb->h.nr_stage_trace_blks;
	uint32_t etag_blocks_seen = 0;
	for (uint32_t i = 0; i < blocks_to_scan && etag_blocks_seen < jdb->h.nr_etag_blks; i++) {
		uint32_t blocktype = *(uint32_t *)cur;
		if (blocktype == ETAG_BLK) {
			struct journal_extent_tag_block *etb =
				(struct journal_extent_tag_block *)cur;
			for (uint32_t j = 0; j < etb->h.nr; j++) {
				if (data_block_index >= block_count &&
				    data_block_index < block_count + etb->tags[j].cnt) {
					*target_addr = etb->tags[j].start +
					       (data_block_index - block_count);
					return 0;
				}
				block_count += etb->tags[j].cnt;
			}
			etag_blocks_seen++;
		}
		cur += OXBOW_BLOCK_SIZE;
	}

	return -1;
}

void print_jnl_tx(journal_tx *tx, uint64_t baddr_to_dump,
	      uint32_t types_to_dump)
{
	if (!tx) {
		printf("print_tx: journal_tx is NULL\n");
		return;
	}

	g_sb = tx->sb;
	if (g_sb->s_magic == htole32(SEFS_MAGIC)) {
		g_fs_type = FS_TYPE_SEFS;
		g_sb_info = g_sb->s_fs_info;
	} else if (g_sb->s_magic == htole16(EXT4_SUPERBLOCK_MAGIC)) {
		g_fs_type = FS_TYPE_EXT4;
		g_sb_info = g_sb->s_fs_info;
	} else {
		printf("print_tx: Unknown filesystem type (magic: 0x%x)\n", (unsigned int)g_sb->s_magic);
		return;
	}

	printf("\n--- Printing Journal Transaction ID: %u ---\n", tx->tid);
	printf("  State: %d, Ref Count: %u\n", tx->state, atomic_load(&tx->ref));
	printf("  Data Fetcher ID: %d\n", tx->df_id);
	printf("  Metadata Buffer: %p (id: %d), Data Buffer: %p (id: %d)\n",
	       tx->df_md_rdma_buf, tx->df_md_buf_id, tx->df_rdma_buf, tx->df_buf_id);
	printf("  Total Data Blocks: %u, Sent: %zu, In Buffer: %zu\n",
	       tx->used_blk_cnt, tx->nr_sent_blks, tx->used_blk_cnt - tx->nr_sent_blks);
	printf("  * Note that only blocks in an in-memory buffer are printed. (Data already been sent to the devfs is not printed.)\n");
	printf("------------------------------------------------\n");

	// Dump metadata blocks
	char *meta_buf = tx->df_md_rdma_buf;
	char *meta_cur = meta_buf;
	size_t meta_size = tx->md_cur - meta_buf;
	uint32_t n_meta_blks = meta_size / OXBOW_BLOCK_SIZE;

	printf("\n--- Metadata Blocks (total %u blocks) ---\n", n_meta_blks);
	if (n_meta_blks > 0) {
		for (uint32_t i = 0; i < n_meta_blks; i++) {
			uint32_t blocktype =
				*(uint32_t *)meta_cur; // All meta blocks have type at start

			if (!should_dump(0, (enum j_block_type)blocktype, NULL,
					 baddr_to_dump, types_to_dump)) {
				meta_cur += OXBOW_BLOCK_SIZE;
				continue;
			}

			printf("\nMeta Block %u: ", i);
			switch (blocktype) {
			case DESC_BLK:
				dump_journal_descriptor_block(
					(const unsigned char *)meta_cur);
				break;
			case ETAG_BLK:
				dump_journal_extent_tag_block(
					(const unsigned char *)meta_cur);
				break;
			case STAGE_TRACE_BLK:
				dump_stage_trace_block(
					(const unsigned char *)meta_cur);
				break;
			default:
				printf("\nUnknown metadata block type: %u\n", blocktype);
				dump_hex((const unsigned char *)meta_cur, OXBOW_BLOCK_SIZE);
			}
			meta_cur += OXBOW_BLOCK_SIZE;
		}
	} else {
		printf("  No metadata blocks in buffer.\n");
	}

	// Dump data blocks
	char *data_buf = tx->df_rdma_buf;
	size_t n_data_blks_in_buf = tx->used_blk_cnt - tx->nr_sent_blks;

	printf("\n--- Data Blocks (total %zu blocks) ---\n", n_data_blks_in_buf);
	if (n_data_blks_in_buf > 0 && tx->desc_blk) {
		for (size_t i = 0; i < n_data_blks_in_buf; i++) {
			size_t overall_data_idx = tx->nr_sent_blks + i;
			baddr_t target_addr;
			const unsigned char *block_data =
				(const unsigned char *)data_buf + i * OXBOW_BLOCK_SIZE;

			if (get_target_address_for_data_block(
				    tx->desc_blk, meta_buf, overall_data_idx,
				    &target_addr) != 0) {
				printf("\nData Block (in-buffer index %zu, overall %zu): Could not find mapping to baddr\n", i, overall_data_idx);
				continue;
			}

			if (baddr_to_dump != 0 && target_addr != baddr_to_dump)
				continue;

			const char *block_type_str =
				get_data_block_type(block_data, target_addr);

			// printf("(baddr = %lu) block_type_str: %s\n", target_addr, block_type_str);

			if (!should_dump(target_addr, 0, block_type_str,
					 baddr_to_dump, types_to_dump))
				continue;

			printf("\nData Block (in-buffer index %zu, overall %zu, baddr %lu): Type: %s\n",
			       i, overall_data_idx, target_addr, block_type_str);

			// Show detailed information for specific block types
			if (strcmp(block_type_str, "EXT4 Inode Table") == 0) {
				dump_ext4_inode_table_block(block_data, target_addr);
			} else if (strcmp(block_type_str, "EXT4 Extent Tree Block") == 0) {
				dump_ext4_extent_tree_block(block_data, target_addr);
			}

			dump_hex(block_data, 128);
		}
	} else {
		printf("  No data blocks in buffer or descriptor block missing.\n");
	}
	printf("\n--- End of Transaction Dump ---\n\n");
}

static void dump_hex(const unsigned char *data, size_t size)
{
	for (size_t i = 0; i < size; i++) {
		if (i != 0 && i % 16 == 0)
			printf("\n    %04zx: ", i);
		printf("%02x ", data[i]);
	}
	printf("\n");
}

static int is_ext4_group_descriptor_block(baddr_t target_addr)
{
	if (g_fs_type != FS_TYPE_EXT4) return 0;
	struct ext4_sblock *ext4_sb = (struct ext4_sblock *)g_sb_info;

	uint32_t block_size = ext4_sb_get_block_size(ext4_sb);
	uint32_t blocks_per_group = le32toh(ext4_sb->blocks_per_group);
	uint32_t first_data_block = le32toh(ext4_sb->first_data_block);
	uint16_t desc_size = ext4_sb_get_desc_size(ext4_sb);
	if (desc_size == 0 || blocks_per_group == 0) return 0;
	uint32_t desc_per_block = block_size / desc_size;
	uint64_t total_blocks = ext4_sb_get_blocks_cnt(ext4_sb);
	uint32_t groups_count = (uint32_t)((total_blocks + blocks_per_group - 1) / blocks_per_group);
	uint32_t gdt_blocks = (groups_count + desc_per_block - 1) / desc_per_block;

	uint64_t gdt_start = (uint64_t)first_data_block + 1ULL;

	return (target_addr >= gdt_start && target_addr < gdt_start + gdt_blocks);
}

static int is_ext4_extent_block(const unsigned char *data)
{
	const struct ext4_extent_header *eh =
		(const struct ext4_extent_header *)data;
	
	// Check magic number
	if (le16toh(eh->magic) != EXT4_EXTENT_MAGIC) {
		return 0;
	}
	
	// More strict validation
	if (le16toh(eh->depth) > 5) { // Depth should be reasonable
		return 0;
	}
	if (le16toh(eh->entries_count) > le16toh(eh->max_entries_count)) {
		return 0;
	}
	return 1;
}

static int is_ext4_directory_block(const unsigned char *data)
{
//     const struct ext4_dir_en *entry = (const struct ext4_dir_en *)data;
//     if (entry->name_len == 1 && strncmp(entry->name, ".", 1) == 0 && le16toh(entry->entry_len) > 0) {
//         const struct ext4_dir_en *entry2 = (const struct ext4_dir_en *)(data + le16toh(entry->entry_len));
//         if (entry2->name_len == 2 && strncmp(entry2->name, "..", 2) == 0 && le16toh(entry2->entry_len) > 0) {
//             return 1;
//         }
//     }
//     return 0;

    	// For EXT4: Directory entries have specific structure
	// Each entry starts with: inode_number(4) + rec_len(2) + name_len(1) + file_type(1)

	uint32_t *inode_no = (uint32_t *)data;
	uint16_t *rec_len = (uint16_t *)(data + 4);
	uint8_t *name_len = (uint8_t *)(data + 6);
	uint8_t *file_type = (uint8_t *)(data + 7);

	// Check first directory entry
	if (*inode_no != 0 && // Inode number should be non-zero
	    *rec_len >= 8 && // Record length should be at least 8 bytes
	    *rec_len <= 4096 && // But not larger than block size
	    *name_len <= 255 && // Name length should be reasonable
	    *file_type <= 7) { // File type should be valid (0-7 in EXT4)

		// Additional check: see if the name starts after the header
		char *name = (char *)(data + 8);
		if (*name_len > 0 && isprint(name[0])) {
			return 1;
		}

		// Check for common directory entries like "." and ".."
		if (*name_len == 1 && name[0] == '.') {
			return 1;
		}
		if (*name_len == 2 && name[0] == '.' && name[1] == '.') {
			return 1;
		}
	}

	return 0;
}

static int is_sefs_directory_block(const unsigned char *data)
{
//     const struct sefs_dir_entry *entry = (const struct sefs_dir_entry *)data;
//     if (strlen(entry->d_name) == 1 && strncmp(entry->d_name, ".", 1) == 0) {
//         entry++;
//          if (strlen(entry->d_name) == 2 && strncmp(entry->d_name, "..", 2) == 0) {
//              return 1;
//          }
//     }
//     return 0;
    	// SEFS directory structure: similar to EXT4 but potentially simpler
	// Entry format: inode_number(4) + rec_len(2) + name_len(1) + [padding] + name

	uint32_t *inode_no = (uint32_t *)data;
	uint16_t *rec_len = (uint16_t *)(data + 4);
	uint8_t *name_len = (uint8_t *)(data + 6);

	// Basic SEFS directory entry validation
	if (*inode_no != 0 && // Inode number should be non-zero
	    *rec_len >= 8 && // Record length should be at least 8 bytes
	    *rec_len <= SEFS_BLOCK_SIZE && // But not larger than block size
	    *name_len <= 255) { // Name length should be reasonable

		// Check if the name starts after the header (offset 8 for SEFS)
		char *name = (char *)(data + 8);
		if (*name_len > 0) {
			// Validate first character is printable
			if (isprint(name[0]) || name[0] == '.') {
				return 1;
			}
		}

		// Check for common directory entries like "." and ".."
		if (*name_len == 1 && name[0] == '.') {
			return 1;
		}
		if (*name_len == 2 && name[0] == '.' && name[1] == '.') {
			return 1;
		}

		// SEFS-specific validation: check for root inode reference
		if (*inode_no == SEFS_ROOT_INO) {
			return 1;
		}

		// Additional SEFS pattern: check if multiple entries look valid
		if (*rec_len >= 12 && (*rec_len + 12) < SEFS_BLOCK_SIZE) {
			uint32_t *next_inode = (uint32_t *)(data + *rec_len);
			uint16_t *next_rec_len =
				(uint16_t *)(data + *rec_len + 4);

			if (*next_inode != 0 && *next_rec_len >= 8 &&
			    *next_rec_len <= SEFS_BLOCK_SIZE) {
				return 1; // Multiple valid entries found
			}
		}
	}

	return 0;
}

static int is_directory_block(const unsigned char *data)
{
	if (g_fs_type == FS_TYPE_EXT4) {
		return is_ext4_directory_block(data);
	} else if (g_fs_type == FS_TYPE_SEFS) {
		return is_sefs_directory_block(data);
	}
	return 0;
}

static int is_ext4_block_bitmap_block(baddr_t target_addr) {
    if (g_fs_type != FS_TYPE_EXT4) return 0;
	struct ext4_sblock *ext4_sb = (struct ext4_sblock *)g_sb_info;

	uint32_t block_size = ext4_sb_get_block_size(ext4_sb);
	uint32_t blocks_per_group = le32toh(ext4_sb->blocks_per_group);
	uint32_t inodes_per_group = le32toh(ext4_sb->inodes_per_group);
	UNUSED(inodes_per_group);
	uint32_t first_data_block = le32toh(ext4_sb->first_data_block);
	uint16_t desc_size = ext4_sb_get_desc_size(ext4_sb);
	uint32_t dsc_per_block = (desc_size == 0) ? 0 : (block_size / desc_size);
	uint64_t total_blocks = ext4_sb_get_blocks_cnt(ext4_sb);
	uint32_t groups_count = blocks_per_group ?
		(uint32_t)((total_blocks + blocks_per_group - 1) / blocks_per_group) : 0;
	uint32_t bg_desc_blocks = (dsc_per_block == 0) ? 0 :
		(uint32_t)((groups_count * desc_size + block_size - 1) / block_size);

	uint64_t group_base0 = (uint64_t)first_data_block + (uint64_t)first_data_block;
	if (blocks_per_group == 0 || target_addr < group_base0) return 0;
	uint32_t group = (uint32_t)((target_addr - group_base0) / blocks_per_group);
	uint64_t group_base = group_base0 + (uint64_t)group * blocks_per_group;
	int has_sb = ext4_sb_is_super_in_bg(ext4_sb, group) ? 1 : 0;
	uint16_t reserved_gdt = le16toh(ext4_sb->s_reserved_gdt_blocks);

	uint64_t block_bitmap_addr = group_base + bg_desc_blocks + 1ULL;
	if (has_sb)
		block_bitmap_addr += 1ULL + (uint64_t)reserved_gdt;

	return target_addr == block_bitmap_addr;
}

static int is_ext4_inode_bitmap_block(baddr_t target_addr) {
    if (g_fs_type != FS_TYPE_EXT4) return 0;
	struct ext4_sblock *ext4_sb = (struct ext4_sblock *)g_sb_info;
	uint32_t block_size = ext4_sb_get_block_size(ext4_sb);
	uint32_t blocks_per_group = le32toh(ext4_sb->blocks_per_group);
	uint32_t first_data_block = le32toh(ext4_sb->first_data_block);
	uint16_t desc_size = ext4_sb_get_desc_size(ext4_sb);
	uint32_t dsc_per_block = (desc_size == 0) ? 0 : (block_size / desc_size);
	uint64_t total_blocks = ext4_sb_get_blocks_cnt(ext4_sb);
	uint32_t groups_count = blocks_per_group ?
		(uint32_t)((total_blocks + blocks_per_group - 1) / blocks_per_group) : 0;
	uint32_t bg_desc_blocks = (dsc_per_block == 0) ? 0 :
		(uint32_t)((groups_count * desc_size + block_size - 1) / block_size);

	uint64_t group_base0 = (uint64_t)first_data_block + (uint64_t)first_data_block;
	if (blocks_per_group == 0 || target_addr < group_base0) return 0;
	uint32_t group = (uint32_t)((target_addr - group_base0) / blocks_per_group);
	uint64_t group_base = group_base0 + (uint64_t)group * blocks_per_group;
	int has_sb = ext4_sb_is_super_in_bg(ext4_sb, group) ? 1 : 0;
	uint16_t reserved_gdt = le16toh(ext4_sb->s_reserved_gdt_blocks);

	uint64_t inode_bitmap_addr = group_base + bg_desc_blocks + 2ULL;
	if (has_sb)
		inode_bitmap_addr += 1ULL + (uint64_t)reserved_gdt;

	return target_addr == inode_bitmap_addr;
}

static int is_ext4_inode_table_block(baddr_t target_addr) {
    if (g_fs_type != FS_TYPE_EXT4) return 0;
	struct ext4_sblock *ext4_sb = (struct ext4_sblock *)g_sb_info;
	uint32_t block_size = ext4_sb_get_block_size(ext4_sb);
	uint32_t blocks_per_group = le32toh(ext4_sb->blocks_per_group);
	uint32_t inodes_per_group = le32toh(ext4_sb->inodes_per_group);
	uint32_t first_data_block = le32toh(ext4_sb->first_data_block);
	uint16_t desc_size = ext4_sb_get_desc_size(ext4_sb);
	uint32_t dsc_per_block = (desc_size == 0) ? 0 : (block_size / desc_size);
	uint64_t total_blocks = ext4_sb_get_blocks_cnt(ext4_sb);
	uint32_t groups_count = blocks_per_group ?
		(uint32_t)((total_blocks + blocks_per_group - 1) / blocks_per_group) : 0;
	uint32_t bg_desc_blocks = (dsc_per_block == 0) ? 0 :
		(uint32_t)((groups_count * desc_size + block_size - 1) / block_size);

	uint64_t group_base0 = (uint64_t)first_data_block + (uint64_t)first_data_block;
	if (blocks_per_group == 0 || target_addr < group_base0) return 0;
	uint32_t group = (uint32_t)((target_addr - group_base0) / blocks_per_group);
	uint64_t group_base = group_base0 + (uint64_t)group * blocks_per_group;
	int has_sb = ext4_sb_is_super_in_bg(ext4_sb, group) ? 1 : 0;
	uint16_t reserved_gdt = le16toh(ext4_sb->s_reserved_gdt_blocks);

	uint64_t inode_table_start = group_base + bg_desc_blocks + 3ULL;
	if (has_sb)
		inode_table_start += 1ULL + (uint64_t)reserved_gdt;

	uint32_t inode_table_blocks = (inodes_per_group * le16toh(ext4_sb->inode_size) + block_size - 1) / block_size;
	if (inode_table_blocks == 0 && inodes_per_group > 0) inode_table_blocks = 1;

	return (target_addr >= inode_table_start && target_addr < inode_table_start + inode_table_blocks);
}

static int is_ext4_reserved_gdt_block(baddr_t target_addr) {
    if (g_fs_type != FS_TYPE_EXT4) return 0;
    struct ext4_sblock *ext4_sb = (struct ext4_sblock *)g_sb_info;
    if (le16toh(ext4_sb->s_reserved_gdt_blocks) == 0) return 0;
    
    // Assuming 1 group
    uint32_t groups_count = 1;
	uint32_t block_size = 1024 << le32toh(ext4_sb->log_block_size);
	uint16_t desc_size = le16toh(ext4_sb->desc_size);
	if (desc_size == 0) return 0;
	uint32_t desc_per_block = block_size / desc_size;
	uint32_t gdt_blocks = (groups_count + desc_per_block - 1) / desc_per_block;
	
    uint64_t gdt_end = le32toh(ext4_sb->first_data_block) + 1 + gdt_blocks;
    
    if (target_addr >= gdt_end && target_addr < gdt_end + le16toh(ext4_sb->s_reserved_gdt_blocks)) {
        return 1;
    }
    
    return 0;
}

static const char *get_data_block_type(const unsigned char *data,
				       baddr_t target_addr)
{
	if (g_fs_type == FS_TYPE_EXT4) {
		// First check address-based block types (these have fixed locations)
		if (is_ext4_group_descriptor_block(target_addr)) return "EXT4 Group Descriptor";
		if (is_ext4_block_bitmap_block(target_addr)) return "EXT4 Block Bitmap";
		if (is_ext4_inode_bitmap_block(target_addr)) return "EXT4 Inode Bitmap";
		if (is_ext4_inode_table_block(target_addr)) return "EXT4 Inode Table";
		// Skip Reserved GDT check for now as it may conflict with actual data blocks
		// if (is_ext4_reserved_gdt_block(target_addr)) return "EXT4 Reserved GDT";
		
		// Then check content-based block types (these can be anywhere in data area)
		if (is_ext4_extent_block(data)) return "EXT4 Extent Tree Block";
		if (is_ext4_directory_block(data)) return "EXT4 Directory";
	} else if (g_fs_type == FS_TYPE_SEFS) {
		struct sefs_sb_info *sb = g_sb->s_fs_info;
		uint32_t istore_start = 1;
		uint32_t istore_end = istore_start + le32toh(sb->nr_istore_blocks);
		uint32_t ifree_start = istore_end;
		uint32_t ifree_end = ifree_start + le32toh(sb->nr_ifree_blocks);
		uint32_t bfree_start = ifree_end;
		uint32_t bfree_end = bfree_start + le32toh(sb->nr_bfree_blocks);

		// Address-based checks first (SEFS has clearer layout)
		if (target_addr >= istore_start && target_addr < istore_end) return "SEFS Inode Store";
		if (target_addr >= ifree_start && target_addr < ifree_end) return "SEFS Inode Bitmap";
		if (target_addr >= bfree_start && target_addr < bfree_end) return "SEFS Block Bitmap";
		
		// Content-based checks second
		if (is_sefs_directory_block(data)) return "SEFS Directory";
	}

	return "Unknown Data";
}


static void read_and_dump_stage_data_blocks(stage_tx *tx, struct inode *inode, uint32_t types_to_dump)
{
	if (!g_super_block) {
		printf("  Error: Global superblock not available\n");
		return;
	}

	// Set up global context like in print_jnl_tx
	g_sb = g_super_block;
	if (g_sb->s_magic == htole32(SEFS_MAGIC)) {
		g_fs_type = FS_TYPE_SEFS;
		g_sb_info = g_sb->s_fs_info;
	} else if (g_sb->s_magic == htole16(EXT4_SUPERBLOCK_MAGIC)) {
		g_fs_type = FS_TYPE_EXT4;
		g_sb_info = g_sb->s_fs_info;
	} else {
		printf("  Error: Unknown filesystem type (magic: 0x%x)\n", (unsigned int)g_sb->s_magic);
		return;
	}

	uint32_t total_data_blocks = 0;
	uint32_t dumped_blocks = 0;

	// Count total data blocks from all tags
	for (uint32_t i = 0; i < tx->desc_blk_copy.h.nr_tags; i++) {
		total_data_blocks += tx->desc_blk_copy.tags[i].cnt;
	}
	if (tx->ext_blk) {
		for (uint32_t i = 0; i < tx->ext_blk->h.nr; i++) {
			total_data_blocks += tx->ext_blk->tags[i].cnt;
		}
	}

	printf("\n--- Stage Data Blocks (total %u blocks) ---\n", total_data_blocks);

	// Read and dump data blocks from descriptor block tags
	for (uint32_t i = 0; i < tx->desc_blk_copy.h.nr_tags; i++) {
		etag_t *tag = &tx->desc_blk_copy.tags[i];
		
		for (uint32_t j = 0; j < tag->cnt; j++) {
			baddr_t target_addr = tag->start + j;
			struct buffer_head *bh = sb_bread(g_super_block, target_addr);
			if (!bh) {
				printf("  Error reading block at address %lu\n", target_addr);
				continue;
			}

			const unsigned char *block_data = (const unsigned char *)bh->b_data;
			const char *block_type_str = get_data_block_type(block_data, target_addr);

			if (!should_dump(target_addr, 0, block_type_str, 0, types_to_dump)) {
				brelse(bh);
				continue;
			}

			printf("\nData Block %u (baddr %lu): Type: %s\n", 
			       dumped_blocks + 1, target_addr, block_type_str);

			// Show detailed information for specific block types
			if (strcmp(block_type_str, "EXT4 Inode Table") == 0) {
				dump_ext4_inode_table_block(block_data, target_addr);
			} else if (strcmp(block_type_str, "EXT4 Extent Tree Block") == 0) {
				// Print the entire extent tree for this inode
				if (inode && g_fs_type == FS_TYPE_EXT4) {
					printf("      [Complete Extent Tree for Inode %lu]\n", inode->i_ino);
					
					// Create a temporary ext4_inode_ref structure for extent tree printing
					struct ext4_inode_ref *inode_ref;
					inode_ref = EXT4_INODE(inode);
					
					// Call extent tree printing function
					ext4_print_extent_tree(inode_ref, "EXTENT_TREE_BLOCK_DUMP(STAGE)");
				}
			}

			dump_hex(block_data, 128);
			dumped_blocks++;

			brelse(bh);
		}
	}

	// Read and dump data blocks from extent tag block if it exists
	if (tx->ext_blk) {
		for (uint32_t i = 0; i < tx->ext_blk->h.nr; i++) {
			etag_t *tag = &tx->ext_blk->tags[i];
			
			for (uint32_t j = 0; j < tag->cnt; j++) {
				baddr_t target_addr = tag->start + j;
				struct buffer_head *bh = sb_bread(g_super_block, target_addr);
				if (!bh) {
					printf("  Error reading block at address %lu\n", target_addr);
					continue;
				}

				const unsigned char *block_data = (const unsigned char *)bh->b_data;
				const char *block_type_str = get_data_block_type(block_data, target_addr);

				if (!should_dump(target_addr, 0, block_type_str, 0, types_to_dump)) {
					brelse(bh);
					continue;
				}

				printf("\nData Block %u (baddr %lu): Type: %s\n", 
				       dumped_blocks + 1, target_addr, block_type_str);

				// Show detailed information for specific block types
				if (strcmp(block_type_str, "EXT4 Inode Table") == 0) {
					dump_ext4_inode_table_block(block_data, target_addr);
				} else if (strcmp(block_type_str, "EXT4 Extent Tree Block") == 0) {
					// Print the entire extent tree for this inode
					if (inode && g_fs_type == FS_TYPE_EXT4) {
						printf("      [Complete Extent Tree for Inode %lu]\n", inode->i_ino);
						
						struct ext4_inode_ref *inode_ref;
						inode_ref = EXT4_INODE(inode);
						
						// Call extent tree printing function
						ext4_print_extent_tree(inode_ref, "EXTENT_TREE_BLOCK_DUMP(STAGE)");
					}
				}

				dump_hex(block_data, 128);
				dumped_blocks++;

				brelse(bh);
			}
		}
	}

	if (dumped_blocks == 0) {
		printf("  No data blocks matched dump criteria\n");
	} else {
		printf("  Dumped %u data blocks (of %u total)\n", dumped_blocks, total_data_blocks);
	}
}

void print_stage_tx(stage_tx *tx, struct inode *inode, uint32_t types_to_dump) {
	if (!tx) {
		printf("print_stage_tx: stage_tx is NULL\n");
		return;
	}

	printf("\n=== Printing Stage Transaction (inode=%lu) ===\n", inode->i_ino);
	printf("  Inode: %lu\n", inode->i_ino);
	printf("  MRC TX ID: %u\n", tx->desc_blk_copy.h.mrc_tx_id);
	printf("  Start LBA: %lu\n", tx->start);
	printf("  Sequence Index: %d, Max: %d\n", tx->seq_idx, tx->seq_max);
	printf("  Blocks - Pending: %u, Issued: %u, Left: %u\n", 
	       tx->nr_pending, tx->nr_issued, tx->nr_blocks);
	printf("  Data Blocks Count: %lu\n", tx->data_blks_cnt);
	printf("  Descriptor Block Skipped: %s\n", tx->desc_blk_skipped ? "Yes" : "No");
	printf("--------------------------------------\n");

	// Dump descriptor block
	printf("\n--- Stage Descriptor Block (inode=%lu) ---\n", inode->i_ino);
	if (should_dump(0, STAGE_DESC_BLK, NULL, 0, types_to_dump)) {
		printf("Stage Descriptor Block:\n");
		printf("  Blocktype: %u\n", tx->desc_blk_copy.h.blocktype);
		printf("  Inode: %llu\n", tx->desc_blk_copy.h.ino);
		printf("  MRC TX ID: %u\n", tx->desc_blk_copy.h.mrc_tx_id);
		printf("  Nr Etag Blocks: %u\n", tx->desc_blk_copy.h.nr_etag_blks);
		printf("  Nr Tags: %u\n", tx->desc_blk_copy.h.nr_tags);
		printf("  Inode Block Address: %llu\n", tx->desc_blk_copy.h.inode_baddr);
		printf("  Inode Index: %u\n", tx->desc_blk_copy.h.inode_index);
		printf("  Inode Size: %u\n", tx->desc_blk_copy.h.inode_size);

		// Print tags in descriptor block
		for (uint32_t i = 0; i < tx->desc_blk_copy.h.nr_tags; i++) {
			print_etag(&tx->desc_blk_copy.tags[i]);
		}

		// Dump hex of descriptor block
		dump_hex((const unsigned char *)&tx->desc_blk_copy, sizeof(tx->desc_blk_copy));
	}

	// Dump extent tag block if it exists
	if (tx->ext_blk) {
		printf("\n--- Stage Extent Tag Block (inode=%lu) ---\n", inode->i_ino);
		if (should_dump(0, ETAG_BLK, NULL, 0, types_to_dump)) {
			printf("Extent Tag Block:\n");
			printf("  Blocktype: %u\n", tx->ext_blk->h.blocktype);
			printf("  Nr Tags: %u\n", tx->ext_blk->h.nr);

			// Print tags in extent tag block
			for (uint32_t i = 0; i < tx->ext_blk->h.nr; i++) {
				print_etag(&tx->ext_blk->tags[i]);
			}

			// Dump hex of extent tag block
			dump_hex((const unsigned char *)tx->ext_blk, sizeof(*tx->ext_blk));
		}
	} else {
		printf("  No extent tag block\n");
	}

	// Read and dump actual data blocks from storage
	read_and_dump_stage_data_blocks(tx, inode, types_to_dump);

	printf("\n=== End of Stage Transaction Dump (inode=%lu) ===\n\n", inode->i_ino);
}
