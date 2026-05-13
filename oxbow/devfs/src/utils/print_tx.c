#include "utils/print_tx.h"
#include "common/sjd.h"
#include "journal.h"
#include <stdio.h>
#include <string.h>
#include <endian.h>
#include <ctype.h>
#include <time.h>
#include "storage_engine.h"
#include "common/oxbow.h"
#include "common/journal_op.h"
#include "common/fs/lwext4/lwext4.h"

// EXT4 Group Descriptor structure (32-byte version)
struct ext4_group_desc {
	uint32_t bg_block_bitmap_lo; /* Blocks bitmap block */
	uint32_t bg_inode_bitmap_lo; /* Inodes bitmap block */
	uint32_t bg_inode_table_lo; /* Inodes table block */
	uint16_t bg_free_blocks_count_lo; /* Free blocks count */
	uint16_t bg_free_inodes_count_lo; /* Free inodes count */
	uint16_t bg_used_dirs_count_lo; /* Directories count */
	uint16_t bg_flags; /* EXT4_BG_flags (INODE_UNINIT, etc) */
	uint32_t bg_exclude_bitmap_lo; /* Exclude bitmap for snapshots */
	uint16_t bg_block_bitmap_csum_lo; /* crc32c(s_uuid+grp_num+bbitmap) LE */
	uint16_t bg_inode_bitmap_csum_lo; /* crc32c(s_uuid+grp_num+ibitmap) LE */
	uint16_t bg_itable_unused_lo; /* Unused inodes count */
	uint16_t bg_checksum; /* crc16(sb_uuid+group+desc) */
} __attribute__((packed));

// Constants for SEFS filesystem detection
#define SEFS_MAGIC 0x53454653 // "SEFS" in ASCII (estimated from code analysis)
#define SEFS_BLOCK_SIZE 4096 // Standard block size
#define SEFS_INODES_PER_BLOCK 64 // Estimated from mkfs.c patterns
#define SEFS_ROOT_INO 1 // Root inode number (from mkfs.c)

// Constants for EXT4 extent detection
#define EXT4_EXTENT_MAGIC 0xF30A // From ext4_extent.c

// EXT4 extent header structure (from ext4_extent.c)
struct ext4_extent_header {
	uint16_t magic;
	uint16_t entries_count; /* Number of valid entries */
	uint16_t max_entries_count; /* Capacity of store in entries */
	uint16_t depth; /* Has tree real underlying blocks? */
	uint32_t generation; /* generation of the tree */
} __attribute__((packed));

// EXT4 extent structure (from ext4_extent.c)
struct ext4_extent {
	uint32_t first_block; /* First logical block extent covers */
	uint16_t block_count; /* Number of blocks covered by extent */
	uint16_t start_hi; /* High 16 bits of physical block */
	uint32_t start_lo; /* Low 32 bits of physical block */
} __attribute__((packed));

// EXT4 extent index structure (from ext4_extent.c)
struct ext4_extent_index {
	uint32_t first_block; /* Index covers logical blocks from 'block' */
	uint32_t leaf_lo; /* Low 32 bits of physical block */
	uint16_t leaf_hi; /* High 16 bits of physical block */
	uint16_t padding;
} __attribute__((packed));

// Function declarations for data block type detection
static int is_ext4_inode_block(const unsigned char *data);
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

// Address-based block type prediction
static const char *predict_block_type_by_address(baddr_t addr);
static int validate_block_content(baddr_t addr, const unsigned char *data,
				  const char *predicted_type);

static void dump_hex(const unsigned char *data, size_t size)
{
	for (size_t i = 0; i < size; i++) {
		if (i % 16 == 0) {
			printf("%08lx: ", i);
		}
		printf("%02x", data[i]);
		if ((i + 1) % 2 == 0)
			printf(" ");
		if ((i + 1) % 16 == 0) {
			printf("  ");
			// Print ASCII representation
			for (size_t j = i - 15; j <= i; j++) {
				if (data[j] >= 32 && data[j] <= 126)
					printf("%c", data[j]);
				else
					printf(".");
			}
			printf("\n");
		}
	}
	// Print remaining bytes if any
	if (size % 16 != 0) {
		size_t remaining = 16 - (size % 16);
		for (size_t i = 0; i < remaining; i++) {
			printf("  ");
			if ((i + 1) % 2 == 0)
				printf(" ");
		}
		printf("  ");
		for (size_t i = size - (size % 16); i < size; i++) {
			if (data[i] >= 32 && data[i] <= 126)
				printf("%c", data[i]);
			else
				printf(".");
		}
		printf("\n");
	}
}

static void set_bvec(struct bio *bio, int bvec_id, char *buf, uint32_t n_blks)
{
	bio->bi_io_vec[bvec_id].bv_buf = buf;
	bio->bi_io_vec[bvec_id].bv_len = (size_t)n_blks * PAGE_SIZE;
	bio->total_size += bio->bi_io_vec[bvec_id].bv_len;
}

static int read_block_from_disk(baddr_t baddr, unsigned char *buf)
{
	struct bio_list *bl;
	struct bio *bio;

	bl = alloc_bl();
	if (!bl) {
		fprintf(stderr, "Failed to allocate bio_list\n");
		return -1;
	}

	bio = alloc_bio_n_bvecs(baddr, 1);
	if (!bio) {
		fprintf(stderr, "Failed to allocate bio\n");
		free_bl(bl);
		return -1;
	}

	set_bvec(bio, 0, (char *)buf, 1);
	bio_list_add(bl, bio);

	if (se_request_dispatch_io_sync(bl, 1) != 0) {
		fprintf(stderr,
			"se_request_dispatch_io_sync failed for baddr %lu\n",
			baddr);
		free_bl(bl);
		return -1;
	}

	free_bl(bl);
	return 0;
}

static void print_etag(etag_t *tag)
{
	printf("  start: %lu, cnt: %u\n", tag->start, tag->cnt);
}

static void dump_stage_trace_block(const unsigned char *data)
{
	struct journal_stage_trace_block *stb =
		(struct journal_stage_trace_block *)data;
	printf("\nStage Trace Block:\n");
	printf("Header:\n");
	printf("  blocktype: %u\n", stb->h.blocktype);
	printf("  nr: %u\n", stb->h.nr);
	printf("Transaction list:\n");
	for (uint32_t i = 0; i < stb->h.nr; i++) {
		printf("  tx_list[%u]: %lu\n", i, stb->tx_list[i]);
	}
}

static void dump_journal_extent_tag_block(const unsigned char *data)
{
	struct journal_extent_tag_block *etb =
		(struct journal_extent_tag_block *)data;
	printf("\nJournal Extent Tag Block:\n");
	printf("Header:\n");
	printf("  blocktype: %u\n", etb->h.blocktype);
	printf("  nr: %u\n", etb->h.nr);
	printf("Tags:\n");
	for (uint32_t i = 0; i < etb->h.nr; i++) {
		printf("  [%u] ", i);
		print_etag(&etb->tags[i]);
	}
}

static void dump_journal_descriptor_block(const unsigned char *data)
{
	struct journal_descriptor_block *jdb =
		(struct journal_descriptor_block *)data;
	printf("\nJournal Descriptor Block:\n");
	printf("Header:\n");
	printf("  blocktype: %u\n", jdb->h.blocktype);
	printf("  transaction_id: %u\n", jdb->h.transaction_id);
	printf("  nr_etag_blks: %u\n", jdb->h.nr_etag_blks);
	printf("  nr_stage_trace_blks: %u\n", jdb->h.nr_stage_trace_blks);
	printf("  nr_tags: %u\n", jdb->h.nr_tags);
	printf("  meta_start_baddr: %u\n", jdb->h.meta_start_baddr);
	printf("Tags:\n");
	for (uint32_t i = 0; i < jdb->h.nr_tags; i++) {
		printf("  [%u] ", i);
		print_etag(&jdb->tags[i]);
	}
}

static void dump_commit_block(const unsigned char *data)
{
	struct commit_header *ch = (struct commit_header *)data;
	printf("\nCommit Block:\n");
	printf("Header:\n");
	printf("  blocktype: %u\n", ch->common.h_blocktype);
	printf("  transaction_id: %u\n", ch->common.h_txid);
	printf("  commit_sec: %lu\n", ch->h_commit_sec);
	printf("  commit_nsec: %u\n", ch->h_commit_nsec);

	// Convert timestamp to human readable format
	time_t commit_time = (time_t)ch->h_commit_sec;
	struct tm *tm_info = localtime(&commit_time);
	char time_buffer[64];

	if (tm_info != NULL) {
		strftime(time_buffer, sizeof(time_buffer), "%Y-%m-%d %H:%M:%S",
			 tm_info);
		printf("  commit_time: %s.%09u (local time)\n", time_buffer,
		       ch->h_commit_nsec);

		// Also show UTC time
		tm_info = gmtime(&commit_time);
		if (tm_info != NULL) {
			strftime(time_buffer, sizeof(time_buffer),
				 "%Y-%m-%d %H:%M:%S", tm_info);
			printf("  commit_time_utc: %s.%09u UTC\n", time_buffer,
			       ch->h_commit_nsec);
		}
	} else {
		printf("  commit_time: <invalid timestamp>\n");
	}
}

static void dump_journal_superblock(const unsigned char *data)
{
	struct journal_superblock *sb = (struct journal_superblock *)data;

	printf("\nJournal Superblock:\n");
	printf("  magic: 0x%x\n", sb->common.h_magic);
	printf("  blocktype: %u\n", sb->common.h_blocktype);
	printf("  txid: %u\n", sb->common.h_txid);
	printf("  block_size: %u\n", sb->block_size);
	printf("  maxlen (total # of blocks): %lu (%lu GB)\n", sb->maxlen,
	       sb->maxlen >> (30 - OXBOW_BLOCK_SIZE_SHIFT));
	printf("  first block: %lu\n", sb->first);
	printf("  tx_id (seqn): %u\n", sb->tx_id);
	printf("  log start block: %lu\n", sb->start);
	printf("  err_no: %u\n", sb->err_no);
	printf("  max_transaction: %u\n", sb->max_transaction);
	printf("  max_trans_data: %u\n", sb->max_trans_data);
}

static void dump_ext4_inode_table_block(const unsigned char *data,
					baddr_t target_addr)
{
	if (!g_sb_static) {
		printf("  EXT4 Inode Table Block (no superblock)\n");
		return;
	}

	struct ext4_sblock *ext4_sb = (struct ext4_sblock *)g_sb_static;
	if (ext4_sb->magic != EXT4_SUPERBLOCK_MAGIC) {
		printf("  EXT4 Inode Table Block\n");
		return;
	}

	uint32_t inode_size = ext4_sb->inode_size;
	uint32_t inodes_per_block = PAGE_SIZE / inode_size;

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

		if (i_mode != 0) {
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

			printf("    Inode #%u: %s %s uid=%u gid=%u size=%u links=%u blocks=%u",
			       i + 1, type_desc, perms, i_uid, i_gid, i_size,
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
		}
	}

	printf("    Summary: %u allocated / %u total inodes in this block\n",
	       allocated_count, inodes_per_block);
}

static int should_dump(baddr_t baddr, enum j_block_type btype,
		       uint64_t baddr_to_dump, uint32_t types_to_dump)
{
	if (baddr_to_dump != (uint64_t)-1) {
		return baddr_to_dump == baddr;
	}
	if (types_to_dump & (1 << btype)) {
		return 1;
	}
	return 0;
}

/**
 * @brief Try to detect if the block contains EXT4 inode data
 * @param data Pointer to block data
 * @return 1 if likely EXT4 inode block, 0 otherwise
 */
static int is_ext4_inode_block(const unsigned char *data)
{
	// For EXT4: Check for inode structure patterns
	// EXT4 inodes have specific field patterns at fixed offsets
	uint16_t *mode = (uint16_t *)(data);
	uint32_t *size = (uint32_t *)(data + 4);

	// Check if mode field looks like a valid file mode
	// Valid modes typically have certain bits set
	if ((*mode & 0xF000) != 0) { // File type bits should be set
		switch (*mode & 0xF000) {
		case 0x1000: // FIFO
		case 0x2000: // Character device
		case 0x4000: // Directory
		case 0x6000: // Block device
		case 0x8000: // Regular file
		case 0xA000: // Symbolic link
		case 0xC000: // Socket
			return 1;
		}
	}

	// Additional heuristic: check if size field is reasonable
	if (*size <
	    (1ULL << 40)) { // Less than 1TB seems reasonable for file size
		return 1;
	}

	return 0;
}

/**
 * @brief Try to detect if the block contains SEFS inode data
 * @param data Pointer to block data
 * @return 1 if likely SEFS inode block, 0 otherwise
 */
static int is_sefs_inode_block(const unsigned char *data)
{
	// SEFS inodes: analyze based on actual mkfs.c structure
	// Check for multiple inodes in the block (SEFS_INODES_PER_BLOCK per block)

	int valid_inodes = 0;
	const int estimated_inode_size =
		SEFS_BLOCK_SIZE / SEFS_INODES_PER_BLOCK; // ~64 bytes per inode

	for (int i = 0; i < SEFS_INODES_PER_BLOCK &&
			(i * estimated_inode_size + 8) < SEFS_BLOCK_SIZE;
	     i++) {
		const unsigned char *inode_ptr =
			data + (i * estimated_inode_size);

		// SEFS inode structure fields (based on standard Unix inode layout)
		uint16_t *mode = (uint16_t *)(inode_ptr); // i_mode
		uint32_t *size = (uint32_t *)(inode_ptr + 4); // i_size
		uint32_t *blocks =
			(uint32_t *)(inode_ptr +
				     8); // i_blocks (estimated offset)

		// Check if this looks like a valid inode
		if (*mode != 0) {
			// Check if mode field has valid file type bits
			uint16_t file_type = *mode & 0xF000;
			switch (file_type) {
			case 0x1000: // FIFO
			case 0x2000: // Character device
			case 0x4000: // Directory
			case 0x6000: // Block device
			case 0x8000: // Regular file
			case 0xA000: // Symbolic link
			case 0xC000: // Socket
				// Check for reasonable size values (SEFS may be more conservative)
				if (*size < (1ULL
					     << 30) && // Less than 1GB for SEFS files
				    (*blocks == 0 ||
				     *blocks <
					     1000000)) { // Reasonable block count
					valid_inodes++;
				}
				break;
			}
		}
	}

	// If at least 1 valid inode found, consider it an inode block
	return valid_inodes > 0;
}

/**
 * @brief Generic inode block detection (tries both EXT4 and SEFS)
 * @param data Pointer to block data
 * @return 1 if likely inode block, 0 otherwise
 */
static int is_inode_block(const unsigned char *data)
{
	return is_ext4_inode_block(data) || is_sefs_inode_block(data);
}

/**
 * @brief Check if target address is EXT4 group descriptor block
 * @param target_addr Block address to check
 * @return 1 if it's group descriptor block, 0 otherwise
 */
static int is_ext4_group_descriptor_block(baddr_t target_addr)
{
	if (!g_j_ops || !g_j_ops->get_fs_area_start_baddr) {
		return 0;
	}

	// Check if this looks like EXT4 by checking magic number
	struct ext4_sblock *ext4_sb = (struct ext4_sblock *)g_sb_static;
	if (ext4_sb->magic != EXT4_SUPERBLOCK_MAGIC) {
		return 0;
	}

	baddr_t fs_start = g_j_ops->get_fs_area_start_baddr(g_sb_static);

	// Use correct mkfs.c logic
	uint32_t block_size = 4096; // OXBOW_BLOCK_SIZE
	uint32_t blocks_per_group = ext4_sb->blocks_per_group;
	uint32_t blocks_count = ext4_sb->blocks_count_lo;
	uint32_t bg_count =
		(blocks_count + blocks_per_group - 1) / blocks_per_group;
	uint32_t desc_size = ext4_sb->desc_size;
	if (desc_size == 0)
		desc_size = 32; // Default EXT4 descriptor size

	// Calculate first_data_block
	uint32_t first_data_block = (block_size > 1024) ? 0 : 1;
	uint32_t descs_per_block = block_size / desc_size;
	uint32_t gdt_blocks =
		(bg_count + descs_per_block - 1) / descs_per_block;

	// Group descriptor table starts after superblock
	baddr_t gdt_start = fs_start + first_data_block + 1;

	// Check if target_addr is within group descriptor table range
	return (target_addr >= gdt_start &&
		target_addr < gdt_start + gdt_blocks);
}

/**
 * @brief Try to detect if the block contains EXT4 extent tree data
 * @param data Pointer to block data
 * @return 1 if likely EXT4 extent block, 0 otherwise
 */
static int is_ext4_extent_block(const unsigned char *data)
{
	// Check for EXT4 extent magic number at the beginning
	struct ext4_extent_header *eh = (struct ext4_extent_header *)data;

	// Convert from little endian and check magic
	uint16_t magic = le16toh(eh->magic);
	if (magic != EXT4_EXTENT_MAGIC) {
		return 0;
	}

	// Additional validation: check if entries_count is reasonable
	uint16_t entries = le16toh(eh->entries_count);
	uint16_t max_entries = le16toh(eh->max_entries_count);
	uint16_t depth = le16toh(eh->depth);

	// Basic sanity checks
	if (entries > max_entries) {
		return 0;
	}

	// Max entries should be reasonable for a block
	if (max_entries == 0 || max_entries > 1000) {
		return 0;
	}

	// Depth should be reasonable (usually 0-4 for extent trees)
	if (depth > 10) {
		return 0;
	}

	return 1;
}

/**
 * @brief Try to detect if the block contains EXT4 directory entry data
 * @param data Pointer to block data
 * @return 1 if likely EXT4 directory block, 0 otherwise
 */
static int is_ext4_directory_block(const unsigned char *data)
{
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

/**
 * @brief Try to detect if the block contains SEFS directory entry data
 * @param data Pointer to block data
 * @return 1 if likely SEFS directory block, 0 otherwise
 */
static int is_sefs_directory_block(const unsigned char *data)
{
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
		if (*name_len > 0 && *name_len < 256) {
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

/**
 * @brief Generic directory block detection (tries both EXT4 and SEFS)
 * @param data Pointer to block data
 * @return 1 if likely directory block, 0 otherwise
 */
static int is_directory_block(const unsigned char *data)
{
	return is_ext4_directory_block(data) || is_sefs_directory_block(data);
}

/**
 * @brief Predict block type based on address using filesystem layout
 * @param addr Block address to check
 * @return String describing the predicted block type
 */
static const char *predict_block_type_by_address(baddr_t addr)
{
	if (!g_j_ops) {
		return "Unknown (no filesystem ops)";
	}

	// Check if address is in inode region
	if (g_j_ops->get_inode_region_start && g_j_ops->get_inode_region_end) {
		baddr_t inode_start =
			g_j_ops->get_inode_region_start(g_sb_static);
		baddr_t inode_end = g_j_ops->get_inode_region_end(g_sb_static);

		if (addr >= inode_start && addr <= inode_end) {
			return "Inode Block (by address)";
		}
	}

	// Check if address is in data region
	if (g_j_ops->get_data_region_start && g_j_ops->get_data_region_end) {
		baddr_t data_start =
			g_j_ops->get_data_region_start(g_sb_static);
		baddr_t data_end = g_j_ops->get_data_region_end(g_sb_static);

		if (addr >= data_start && addr <= data_end) {
			return "Data Block (by address)";
		}
	}

	// Check if address is in filesystem area
	if (g_j_ops->get_fs_area_start_baddr &&
	    g_j_ops->get_nr_fs_area_blocks) {
		baddr_t fs_start =
			g_j_ops->get_fs_area_start_baddr(g_sb_static);
		uint32_t fs_blocks =
			g_j_ops->get_nr_fs_area_blocks(g_sb_static);

		if (addr >= fs_start && addr < fs_start + fs_blocks) {
			return "Filesystem Block (by address)";
		}
	}

	return "Unknown Region (by address)";
}

/**
 * @brief Validate block content against predicted type
 * @param addr Block address
 * @param data Block content
 * @param predicted_type Predicted type from address
 * @return 1 if content matches prediction, 0 if mismatch, -1 if cannot validate
 */
static int validate_block_content(baddr_t addr, const unsigned char *data,
				  const char *predicted_type)
{
	const char *actual_type = get_data_block_type(data, addr);

	// If we predicted inode block, check if content looks like inode
	if (strstr(predicted_type, "Inode") != NULL) {
		if (is_inode_block(data)) {
			return 1; // Valid: address says inode, content looks like inode
		} else {
			return 0; // Invalid: address says inode, content doesn't look like inode
		}
	}

	// If we predicted data block, it could be directory or regular data
	if (strstr(predicted_type, "Data") != NULL) {
		if (is_directory_block(data) ||
		    strstr(actual_type, "Text/Data") != NULL ||
		    strstr(actual_type, "Unknown Data") != NULL) {
			return 1; // Valid: address says data, content looks like data/directory
		} else if (is_inode_block(data)) {
			return 0; // Invalid: address says data, but content looks like inode
		}
		// If content is unallocated/zero, it might be valid (newly allocated)
		if (strstr(actual_type, "Unallocated") != NULL) {
			return 1; // Valid: could be newly allocated data block
		}
	}

	// Cannot make a determination
	return -1;
}

/**
 * @brief Get the target address for a specific data block index based on transaction tags
 * @param jdb Journal descriptor block
 * @param meta_start Start of metadata blocks (etag blocks)
 * @param data_block_index Index of the data block (0-based)
 * @param target_addr Output parameter for target address
 * @return 1 if found, 0 if not found
 */
static int
get_target_address_for_data_block(struct journal_descriptor_block *jdb,
				  char *meta_start, size_t data_block_index,
				  baddr_t *target_addr)
{
	size_t current_index = 0;
	uint32_t i, j;
	etag_t *tags;
	uint32_t n_tags;

	// First, check tags in descriptor block
	n_tags = jdb->h.nr_tags;
	tags = jdb->tags;

	for (i = 0; i < n_tags; i++) {
		if (current_index + tags[i].cnt > data_block_index) {
			// This tag contains our data block
			*target_addr = tags[i].start +
				       (data_block_index - current_index);
			return 1;
		}
		current_index += tags[i].cnt;
	}

	// If not found in descriptor, check etag blocks
	char *cur = meta_start;
	for (i = 0; i < jdb->h.nr_etag_blks; i++) {
		if (is_etag_blk(cur)) {
			struct journal_extent_tag_block *etb =
				(struct journal_extent_tag_block *)cur;
			n_tags = etb->h.nr;
			tags = etb->tags;

			for (j = 0; j < n_tags; j++) {
				if (current_index + tags[j].cnt >
				    data_block_index) {
					// This tag contains our data block
					*target_addr = tags[j].start +
						       (data_block_index -
							current_index);
					return 1;
				}
				current_index += tags[j].cnt;
			}
		}
		cur += OXBOW_BLOCK_SIZE;
	}

	return 0; // Not found
}

/**
 * @brief Check if block might contain SEFS superblock
 * @param data Pointer to block data
 * @return 1 if likely SEFS superblock, 0 otherwise
 */
static int is_sefs_superblock(const unsigned char *data)
{
	// Check for SEFS magic number at the beginning of superblock
	uint32_t *magic = (uint32_t *)data;
	return (*magic == SEFS_MAGIC || *magic == htole32(SEFS_MAGIC));
}

/**
 * @brief Get a human-readable description of the data block type
 * @param data Pointer to block data
 * @param target_addr Target block address (0 if unknown)
 * @return String describing the block type
 */
static const char *get_data_block_type(const unsigned char *data,
				       baddr_t target_addr)
{
	// Check for filesystem superblocks first
	if (is_sefs_superblock(data)) {
		return "SEFS Superblock";
	}

	// Check for EXT4 special blocks first (address-based) - these should have priority
	if (target_addr > 0) {
		if (is_ext4_group_descriptor_block(target_addr)) {
			return "EXT4 Group Descriptor Block";
		} else if (is_ext4_block_bitmap_block(target_addr)) {
			return "EXT4 Block Bitmap Block";
		} else if (is_ext4_inode_bitmap_block(target_addr)) {
			return "EXT4 Inode Bitmap Block";
		} else if (is_ext4_inode_table_block(target_addr)) {
			return "EXT4 Inode Table Block";
		}

		// FIXME: Reserved GDT block is not used in OXBOW? I
		// confirmed that block 2 is used as an extent block.
		// else if (is_ext4_reserved_gdt_block(target_addr)) {
		// 	return "EXT4 Reserved GDT Block";
		// }
	}

	// Determine which filesystem region the target address is in (for remaining blocks)
	int is_in_inode_region = 0;
	int is_in_data_region = 0;

	if (target_addr > 0 && g_j_ops) {
		// Check inode region
		if (g_j_ops->get_inode_region_start &&
		    g_j_ops->get_inode_region_end) {
			baddr_t inode_start =
				g_j_ops->get_inode_region_start(g_sb_static);
			baddr_t inode_end =
				g_j_ops->get_inode_region_end(g_sb_static);
			is_in_inode_region = (target_addr >= inode_start &&
					      target_addr <= inode_end);
		}

		// Check data region
		if (g_j_ops->get_data_region_start &&
		    g_j_ops->get_data_region_end) {
			baddr_t data_start =
				g_j_ops->get_data_region_start(g_sb_static);
			baddr_t data_end =
				g_j_ops->get_data_region_end(g_sb_static);
			is_in_data_region = (target_addr >= data_start &&
					     target_addr <= data_end);
		}
	}

	// If in inode region, only check for inode blocks
	if (is_in_inode_region) {
		if (is_ext4_inode_block(data)) {
			return "EXT4 Inode Block";
		} else if (is_sefs_inode_block(data)) {
			return "SEFS Inode Block";
		}
	}
	// If in data region, check for directory blocks and extent blocks
	else if (is_in_data_region) {
		if (is_ext4_directory_block(data)) {
			return "EXT4 Directory Entry Block";
		} else if (is_ext4_extent_block(data)) {
			return "EXT4 Extent Block";
		} else if (is_sefs_directory_block(data)) {
			return "SEFS Directory Entry Block";
		} else {
			return "Data Block";
		}
	}
	// If region unknown, check for content-based blocks
	else {
		// Check content-based blocks
		if (is_ext4_directory_block(data)) {
			return "EXT4 Directory Entry Block";
		} else if (is_ext4_extent_block(data)) {
			return "EXT4 Extent Block";
		} else if (is_sefs_directory_block(data)) {
			return "SEFS Directory Entry Block";
		} else {
			return "ERROR: Unknown Region";
		}
	}

	// Check for other patterns

	// Check if it's mostly zeros (unallocated)
	int zero_count = 0;
	for (int i = 0; i < 64; i++) { // Check first 64 bytes
		if (data[i] == 0)
			zero_count++;
	}
	if (zero_count > 60) {
		return "Unallocated/Zero Block";
	}

	// Check if it looks like text data
	int printable_count = 0;
	for (int i = 0; i < 64; i++) {
		if (isprint(data[i]) || isspace(data[i])) {
			printable_count++;
		}
	}
	if (printable_count > 48) { // 75% printable
		return "Text/Data Block";
	}

	return "Unknown Data Block";
}

/**
 * @brief Check if target address is EXT4 block bitmap block
 * @param target_addr Block address to check
 * @return 1 if it's block bitmap block, 0 otherwise
 */
static int is_ext4_block_bitmap_block(baddr_t target_addr)
{
	if (!g_j_ops || !g_j_ops->get_fs_area_start_baddr) {
		return 0;
	}

	// Check if this looks like EXT4 by checking magic number
	struct ext4_sblock *ext4_sb = (struct ext4_sblock *)g_sb_static;
	if (ext4_sb->magic != EXT4_SUPERBLOCK_MAGIC) {
		return 0;
	}

	baddr_t fs_start = g_j_ops->get_fs_area_start_baddr(g_sb_static);

	// Use correct mkfs.c logic
	uint32_t block_size = 4096; // OXBOW_BLOCK_SIZE
	uint32_t blocks_per_group = ext4_sb->blocks_per_group;
	uint32_t blocks_count = ext4_sb->blocks_count_lo;
	uint32_t bg_count =
		(blocks_count + blocks_per_group - 1) / blocks_per_group;
	uint32_t desc_size = ext4_sb->desc_size;
	if (desc_size == 0)
		desc_size = 32;

	// Calculate first_data_block
	uint32_t first_data_block = (block_size > 1024) ? 0 : 1;
	uint32_t descs_per_block = block_size / desc_size;
	uint32_t gdt_blocks =
		(bg_count + descs_per_block - 1) / descs_per_block;

	// For first block group only (block group 0)
	uint64_t bg_start_block =
		first_data_block + 0 * blocks_per_group; // Group 0
	uint32_t blk_off = 0;

	// Adjust for group descriptor blocks and superblock
	blk_off += gdt_blocks;
	if (true) { // Group 0 always has superblock
		bg_start_block++; // Skip superblock
		uint16_t reserved_gdt_blocks = ext4_sb->s_reserved_gdt_blocks;
		blk_off += reserved_gdt_blocks;
	}

	baddr_t block_bitmap_addr = fs_start + bg_start_block + blk_off + 1;

	return (target_addr == block_bitmap_addr);
}

/**
 * @brief Check if target address is EXT4 inode bitmap block
 * @param target_addr Block address to check
 * @return 1 if it's inode bitmap block, 0 otherwise
 */
static int is_ext4_inode_bitmap_block(baddr_t target_addr)
{
	if (!g_j_ops || !g_j_ops->get_fs_area_start_baddr) {
		return 0;
	}

	// Check if this looks like EXT4 by checking magic number
	struct ext4_sblock *ext4_sb = (struct ext4_sblock *)g_sb_static;
	if (ext4_sb->magic != EXT4_SUPERBLOCK_MAGIC) {
		return 0;
	}

	baddr_t fs_start = g_j_ops->get_fs_area_start_baddr(g_sb_static);

	// Use correct mkfs.c logic
	uint32_t block_size = 4096; // OXBOW_BLOCK_SIZE
	uint32_t blocks_per_group = ext4_sb->blocks_per_group;
	uint32_t blocks_count = ext4_sb->blocks_count_lo;
	uint32_t bg_count =
		(blocks_count + blocks_per_group - 1) / blocks_per_group;
	uint32_t desc_size = ext4_sb->desc_size;
	if (desc_size == 0)
		desc_size = 32;

	// Calculate first_data_block
	uint32_t first_data_block = (block_size > 1024) ? 0 : 1;
	uint32_t descs_per_block = block_size / desc_size;
	uint32_t gdt_blocks =
		(bg_count + descs_per_block - 1) / descs_per_block;

	// For first block group only (block group 0)
	uint64_t bg_start_block =
		first_data_block + 0 * blocks_per_group; // Group 0
	uint32_t blk_off = 0;

	// Adjust for group descriptor blocks and superblock
	blk_off += gdt_blocks;
	if (true) { // Group 0 always has superblock
		bg_start_block++; // Skip superblock
		uint16_t reserved_gdt_blocks = ext4_sb->s_reserved_gdt_blocks;
		blk_off += reserved_gdt_blocks;
	}

	baddr_t inode_bitmap_addr = fs_start + bg_start_block + blk_off + 2;

	return (target_addr == inode_bitmap_addr);
}

/**
 * @brief Check if target address is EXT4 inode table block
 * @param target_addr Block address to check
 * @return 1 if it's inode table block, 0 otherwise
 */
static int is_ext4_inode_table_block(baddr_t target_addr)
{
	if (!g_j_ops || !g_j_ops->get_fs_area_start_baddr) {
		return 0;
	}

	// Check if this looks like EXT4 by checking magic number
	struct ext4_sblock *ext4_sb = (struct ext4_sblock *)g_sb_static;
	if (ext4_sb->magic != EXT4_SUPERBLOCK_MAGIC) {
		return 0;
	}

	baddr_t fs_start = g_j_ops->get_fs_area_start_baddr(g_sb_static);

	// Use correct mkfs.c logic
	uint32_t block_size = 4096; // OXBOW_BLOCK_SIZE
	uint32_t blocks_per_group = ext4_sb->blocks_per_group;
	uint32_t blocks_count = ext4_sb->blocks_count_lo;
	uint32_t bg_count =
		(blocks_count + blocks_per_group - 1) / blocks_per_group;
	uint32_t desc_size = ext4_sb->desc_size;
	if (desc_size == 0)
		desc_size = 32;

	// Calculate first_data_block
	uint32_t first_data_block = (block_size > 1024) ? 0 : 1;
	uint32_t descs_per_block = block_size / desc_size;
	uint32_t gdt_blocks =
		(bg_count + descs_per_block - 1) / descs_per_block;

	// Calculate inode table size
	uint32_t inodes_per_group = ext4_sb->inodes_per_group;
	uint32_t inode_size = ext4_sb->inode_size;
	uint32_t inode_table_blocks =
		(inodes_per_group * inode_size + block_size - 1) / block_size;

	// For first block group only (block group 0)
	uint64_t bg_start_block =
		first_data_block + 0 * blocks_per_group; // Group 0
	uint32_t blk_off = 0;

	// Adjust for group descriptor blocks and superblock
	blk_off += gdt_blocks;
	if (true) { // Group 0 always has superblock
		bg_start_block++; // Skip superblock
		uint16_t reserved_gdt_blocks = ext4_sb->s_reserved_gdt_blocks;
		blk_off += reserved_gdt_blocks;
	}

	baddr_t inode_table_start = fs_start + bg_start_block + blk_off + 3;
	baddr_t inode_table_end = inode_table_start + inode_table_blocks - 1;

	return (target_addr >= inode_table_start &&
		target_addr <= inode_table_end);
}

// FIXME: Reserved GDT block is not used in OXBOW? I
// confirmed that block 2 is used as an extent block.
/**
 * @brief Check if target address is EXT4 reserved GDT block
 * @param target_addr Block address to check
 * @return 1 if it's reserved GDT block, 0 otherwise
 */
static int is_ext4_reserved_gdt_block(baddr_t target_addr)
{
	if (!g_j_ops || !g_j_ops->get_fs_area_start_baddr) {
		return 0;
	}

	// Check if this looks like EXT4 by checking magic number
	struct ext4_sblock *ext4_sb = (struct ext4_sblock *)g_sb_static;
	if (ext4_sb->magic != EXT4_SUPERBLOCK_MAGIC) {
		return 0;
	}

	// Check if there are reserved GDT blocks
	uint16_t reserved_gdt_blocks = ext4_sb->s_reserved_gdt_blocks;
	if (reserved_gdt_blocks == 0) {
		return 0;
	}

	baddr_t fs_start = g_j_ops->get_fs_area_start_baddr(g_sb_static);

	// Use correct mkfs.c logic
	uint32_t block_size = 4096; // OXBOW_BLOCK_SIZE
	uint32_t blocks_per_group = ext4_sb->blocks_per_group;
	uint32_t blocks_count = ext4_sb->blocks_count_lo;
	uint32_t bg_count =
		(blocks_count + blocks_per_group - 1) / blocks_per_group;
	uint32_t desc_size = ext4_sb->desc_size;
	if (desc_size == 0)
		desc_size = 32; // Default EXT4 descriptor size

	// Calculate first_data_block
	uint32_t first_data_block = (block_size > 1024) ? 0 : 1;
	uint32_t descs_per_block = block_size / desc_size;
	uint32_t gdt_blocks =
		(bg_count + descs_per_block - 1) / descs_per_block;

	// For first block group only (block group 0)
	uint64_t bg_start_block =
		first_data_block + 0 * blocks_per_group; // Group 0
	uint32_t blk_off = 0;

	// Adjust for group descriptor blocks and superblock
	blk_off += gdt_blocks;
	if (true) { // Group 0 always has superblock
		bg_start_block++; // Skip superblock
		// Reserved GDT blocks come after regular GDT blocks within the group
		baddr_t reserved_gdt_start =
			fs_start + bg_start_block + gdt_blocks;
		baddr_t reserved_gdt_end =
			reserved_gdt_start + reserved_gdt_blocks - 1;

		return (target_addr >= reserved_gdt_start &&
			target_addr <= reserved_gdt_end);
	}

	return 0;
}

void print_jnl_tx(struct ckpt_list_entry *ce, uint64_t baddr_to_dump,
		  uint32_t types_to_dump)
{
	if (!ce) {
		printf("Invalid ckpt_list_entry.\n");
		return;
	}

	baddr_t start_baddr = ce->j_start_baddr;
	size_t num_blocks = ce->total_tx_size / PAGE_SIZE;
	uint32_t tx_id = ce->tx_meta ? ce->tx_meta->tx_id : 0;

	printf("\n--- Printing Transaction (tx_id: %u, df_id: %d) ---\n", tx_id,
	       ce->j_log->df_id);
	printf("Journal start baddr: %lu, size: %zu bytes (%zu blocks)\n",
	       start_baddr, ce->total_tx_size, num_blocks);

	// Print filesystem block ranges information
	if (g_j_ops && g_sb_static) {
		printf("\nFilesystem Block Ranges:\n");

		// Print superblock address
		if (g_j_ops->get_fs_area_start_baddr) {
			baddr_t fs_start =
				g_j_ops->get_fs_area_start_baddr(g_sb_static);
			printf("  Superblock: %lu\n",
			       fs_start); // Superblock is typically at the start of filesystem
		}

		// Print filesystem area information
		if (g_j_ops->get_fs_area_start_baddr &&
		    g_j_ops->get_nr_fs_area_blocks) {
			baddr_t fs_start =
				g_j_ops->get_fs_area_start_baddr(g_sb_static);
			uint32_t fs_blocks =
				g_j_ops->get_nr_fs_area_blocks(g_sb_static);
			printf("  Filesystem area: %lu - %lu (%u blocks)\n",
			       fs_start, fs_start + fs_blocks - 1, fs_blocks);
		}

		// Print block group information for EXT4
		// Check if this looks like EXT4 by checking magic number
		if (g_j_ops->get_fs_area_start_baddr) {
			baddr_t fs_start =
				g_j_ops->get_fs_area_start_baddr(g_sb_static);

			// Try to detect EXT4 superblock and show block group info
			struct ext4_sblock *ext4_sb =
				(struct ext4_sblock *)g_sb_static;
			if (ext4_sb->magic == EXT4_SUPERBLOCK_MAGIC) {
				// EXT4 filesystem detected - calculate correct layout
				uint32_t block_size = 4096; // OXBOW_BLOCK_SIZE
				uint32_t blocks_per_group =
					ext4_sb->blocks_per_group;
				uint32_t blocks_count =
					ext4_sb->blocks_count_lo;
				uint32_t bg_count =
					(blocks_count + blocks_per_group - 1) /
					blocks_per_group;
				uint32_t desc_size = ext4_sb->desc_size;
				if (desc_size == 0)
					desc_size =
						32; // Default EXT4 descriptor size

				// Calculate first_data_block (same logic as ext4_mkfs.c)
				uint32_t first_data_block =
					(block_size > 1024) ? 0 : 1;

				// Calculate group descriptor table info
				uint32_t descs_per_block =
					block_size / desc_size;
				uint32_t gdt_blocks =
					(bg_count + descs_per_block - 1) /
					descs_per_block;
				baddr_t gdt_start = fs_start +
						    first_data_block +
						    1; // After superblock

				// Calculate inode table size
				uint32_t inodes_per_group =
					ext4_sb->inodes_per_group;
				uint32_t inode_size = ext4_sb->inode_size;
				uint32_t inode_table_blocks =
					(inodes_per_group * inode_size +
					 block_size - 1) /
					block_size;

				printf("  EXT4 Filesystem Layout (block_size=%u, first_data_block=%u):\n",
				       block_size, first_data_block);
				printf("  Total Block Groups: %u\n", bg_count);
				printf("  Group Descriptor Table: %lu (%u blocks, %u bytes per desc)\n",
				       gdt_start, gdt_blocks, desc_size);

				// Show first block group layout using correct mkfs.c logic
				printf("  First Block Group (Group 0) layout:\n");

				// Calculate bg_start_block for group 0 (correcting the mkfs.c bug)
				// Original buggy code: aux_info->first_data_block + aux_info->first_data_block + i * info->blocks_per_group
				// Corrected: first_data_block + i * blocks_per_group
				uint64_t bg_start_block =
					first_data_block +
					0 * blocks_per_group; // Group 0
				uint32_t blk_off = 0;

				// Adjust for group descriptor blocks and superblock
				blk_off += gdt_blocks;
				if (true) { // Group 0 always has superblock (has_superblock logic)
					bg_start_block++; // Skip superblock
					uint16_t reserved_gdt_blocks =
						ext4_sb->s_reserved_gdt_blocks;
					blk_off += reserved_gdt_blocks;
				}

				// Calculate actual block addresses using mkfs.c logic
				baddr_t block_bitmap_addr =
					bg_start_block + blk_off + 1;
				baddr_t inode_bitmap_addr =
					bg_start_block + blk_off + 2;
				baddr_t inode_table_start =
					bg_start_block + blk_off + 3;
				baddr_t inode_table_end = inode_table_start +
							  inode_table_blocks -
							  1;

				printf("    Superblock: %lu (in filesystem area starting at %lu)\n",
				       fs_start + first_data_block, fs_start);
				printf("    Group Descriptor: %lu - %lu (%u blocks)\n",
				       gdt_start, gdt_start + gdt_blocks - 1,
				       gdt_blocks);

				// Show reserved GDT blocks if any
				uint16_t reserved_gdt_blocks =
					ext4_sb->s_reserved_gdt_blocks;
				if (reserved_gdt_blocks > 0) {
					baddr_t reserved_gdt_start =
						bg_start_block + gdt_blocks;
					baddr_t reserved_gdt_end =
						reserved_gdt_start +
						reserved_gdt_blocks - 1;
					printf("    Reserved GDT Blocks: %lu - %lu (%u blocks)\n",
					       reserved_gdt_start,
					       reserved_gdt_end,
					       reserved_gdt_blocks);
				}

				printf("    Block Bitmap: %lu\n",
				       block_bitmap_addr);
				printf("    Inode Bitmap: %lu\n",
				       inode_bitmap_addr);
				printf("    Inode Table: %lu - %lu (%u blocks, %u inodes)\n",
				       inode_table_start, inode_table_end,
				       inode_table_blocks, inodes_per_group);

				// Calculate data region start
				baddr_t data_region_start = inode_table_end + 1;
				printf("    Data Region: %lu+ (starts after inode table)\n",
				       data_region_start);

				// Note about the mkfs.c bug
				printf("    Note: Corrected bg_start_block calculation from mkfs.c\n");
				printf("          (mkfs.c has a bug: first_data_block is added twice)\n");
			}
		}

		// Print inode region information
		if (g_j_ops->get_inode_region_start &&
		    g_j_ops->get_inode_region_end) {
			baddr_t inode_start =
				g_j_ops->get_inode_region_start(g_sb_static);
			baddr_t inode_end =
				g_j_ops->get_inode_region_end(g_sb_static);
			printf("  Inode region: %lu - %lu (%lu blocks)\n",
			       inode_start, inode_end,
			       inode_end - inode_start + 1);
		}

		// Print data region information
		if (g_j_ops->get_data_region_start &&
		    g_j_ops->get_data_region_end) {
			baddr_t data_start =
				g_j_ops->get_data_region_start(g_sb_static);
			baddr_t data_end =
				g_j_ops->get_data_region_end(g_sb_static);
			printf("  Data region: %lu - %lu (%lu blocks)\n",
			       data_start, data_end, data_end - data_start + 1);
		}

		printf("\n");
	} else {
		printf("\nFilesystem Block Ranges: Not available (no filesystem ops or superblock)\n\n");
	}

	if (num_blocks < 2) {
		printf("Error: Transaction too small (must be at least 2 blocks).\n");
		return;
	}

	unsigned char block_data[PAGE_SIZE];
	baddr_t meta_start_baddr = 0;
	size_t data_block_count = 0;

	for (size_t i = 0; i < num_blocks; i++) {
		baddr_t current_baddr = start_baddr + i;
		if (read_block_from_disk(current_baddr, block_data) != 0) {
			fprintf(stderr,
				"Failed to read block %lu, aborting print_tx.\n",
				current_baddr);
			break;
		}

		if (i == 0) { // First block: must be descriptor
			if (!is_jnr_desc_blk((char *)block_data)) {
				struct journal_descriptor_header *h =
					(struct journal_descriptor_header *)
						block_data;
				enum j_block_type btype =
					(enum j_block_type)h->blocktype;
				printf("baddr %-10lu: Error: Expected Descriptor Block, but got type %u\n",
				       current_baddr, btype);
				dump_hex(block_data, PAGE_SIZE);
				printf("\n");
				break; // Stop processing this invalid transaction
			}
			printf("baddr %-10lu: Journal Descriptor Block (Validated)\n",
			       current_baddr);
			struct journal_descriptor_block *jdb =
				(struct journal_descriptor_block *)block_data;
			meta_start_baddr = jdb->h.meta_start_baddr;
			if (should_dump(current_baddr, DESC_BLK, baddr_to_dump,
					types_to_dump)) {
				dump_journal_descriptor_block(block_data);
			}
			printf("\n");
		} else if (i ==
			   num_blocks -
				   2) { // The block before commit: must be superblock
			struct journal_superblock *sb =
				(struct journal_superblock *)block_data;
			enum j_block_type btype =
				(enum j_block_type)sb->common.h_blocktype;

			if (btype != SUPER_BLK) {
				printf("baddr %-10lu: Error: Expected Superblock, but got type %u\n",
				       current_baddr, btype);
				dump_hex(block_data, PAGE_SIZE);
				printf("\n");
			} else {
				printf("baddr %-10lu: Journal Superblock (Validated)\n",
				       current_baddr);
				if (should_dump(current_baddr, SUPER_BLK,
						baddr_to_dump, types_to_dump)) {
					dump_journal_superblock(block_data);
				}
			}
			printf("\n");
		} else if (i == num_blocks - 1) { // Last block: must be commit
			struct commit_header *h =
				(struct commit_header *)block_data;
			enum j_block_type btype =
				(enum j_block_type)h->common.h_blocktype;

			if (btype != COMMIT_BLK) {
				printf("baddr %-10lu: Error: Expected Commit Block, but got type %u\n",
				       current_baddr, btype);
				dump_hex(block_data, PAGE_SIZE);
				printf("\n");
			} else {
				printf("baddr %-10lu: Commit Block (Validated)\n",
				       current_baddr);
				if (should_dump(current_baddr, COMMIT_BLK,
						baddr_to_dump, types_to_dump)) {
					dump_commit_block(block_data);
				}
			}
			printf("\n");
		} else { // Intermediate blocks
			if (current_baddr < meta_start_baddr) {
				data_block_count++;

				// Get target address for this data block from tags
				baddr_t target_addr = 0;
				const char *predicted_type = "Unknown";
				const char *validation_status = "";
				const char *block_type = NULL;
				int addr_found = 0;

				// Get metadata from first block (descriptor)
				struct journal_descriptor_block *jdb =
					(struct journal_descriptor_block
						 *)(((char *)ce->tx_meta) +
						    offsetof(struct tx_meta,
							     desc_blk));

				if (get_target_address_for_data_block(
					    jdb, ce->tx_meta->meta_start,
					    data_block_count - 1,
					    &target_addr)) {
					addr_found = 1;
					predicted_type =
						predict_block_type_by_address(
							target_addr);

					// Only validate if content type might conflict with prediction
					// Skip validation for regular data blocks as they don't need it
					block_type = get_data_block_type(
						block_data, target_addr);

					if (strstr(block_type, "Directory") !=
						    NULL ||
					    strstr(block_type, "Inode") !=
						    NULL ||
					    strstr(block_type, "Extent") !=
						    NULL ||
					    strstr(block_type,
						   "Group Descriptor") !=
						    NULL) {
						// Validate content against prediction
						int validation_result =
							validate_block_content(
								target_addr,
								block_data,
								predicted_type);
						if (validation_result == 1) {
							validation_status =
								" [VALID]";
						} else if (validation_result ==
							   0) {
							validation_status =
								" [VALIDATION ERROR: content doesn't match address prediction]";
						} else {
							validation_status =
								" [VALIDATION UNKNOWN]";
						}
					} else {
						// No validation needed for regular data blocks
						validation_status = "";
					}
				}

				// Try to determine the type of data block from content
				// (Note: block_type may have been set above for validation)
				if (!block_type) {
					block_type = get_data_block_type(
						block_data, target_addr);
				}

				// Check if we should dump this data block
				int should_dump_data =
					(baddr_to_dump != (uint64_t)-1 &&
					 baddr_to_dump == current_baddr) ||
					(types_to_dump & DUMP_DATA_BLOCKS) ||
					((types_to_dump &
					  DUMP_DIRECTORY_BLOCKS) &&
					 strstr(block_type, "Directory") !=
						 NULL) ||
					((types_to_dump & DUMP_INODE_BLOCKS) &&
					 strstr(block_type, "Inode") != NULL) ||
					((types_to_dump &
					  DUMP_EXT4_EXTENT_BLOCKS) &&
					 strstr(block_type, "Extent") !=
						 NULL) ||
					// EXT4-specific dump options
					((types_to_dump & DUMP_EXT4_BLOCKS) &&
					 strstr(block_type, "EXT4") != NULL) ||
					((types_to_dump &
					  DUMP_EXT4_DIRECTORY_BLOCKS) &&
					 strstr(block_type, "EXT4 Directory") !=
						 NULL) ||
					// EXT4 filesystem structure dump options
					((types_to_dump &
					  DUMP_EXT4_BLOCK_BITMAP) &&
					 strstr(block_type,
						"EXT4 Block Bitmap") != NULL) ||
					((types_to_dump &
					  DUMP_EXT4_INODE_BITMAP) &&
					 strstr(block_type,
						"EXT4 Inode Bitmap") != NULL) ||
					((types_to_dump &
					  DUMP_EXT4_INODE_TABLE) &&
					 strstr(block_type,
						"EXT4 Inode Table") != NULL) ||
					((types_to_dump &
					  DUMP_EXT4_RESERVED_GDT) &&
					 strstr(block_type,
						"EXT4 Reserved GDT") != NULL) ||
					// SEFS-specific dump options
					((types_to_dump & DUMP_SEFS_BLOCKS) &&
					 strstr(block_type, "SEFS") != NULL) ||
					((types_to_dump &
					  DUMP_SEFS_DIRECTORY_BLOCKS) &&
					 strstr(block_type, "SEFS Directory") !=
						 NULL) ||
					((types_to_dump &
					  DUMP_SEFS_INODE_BLOCKS) &&
					 strstr(block_type, "SEFS Inode") !=
						 NULL) ||
					((types_to_dump &
					  DUMP_SEFS_SUPERBLOCKS) &&
					 strstr(block_type,
						"SEFS Superblock") != NULL);

				if (should_dump_data) {
					// Show basic data block info
					if (addr_found) {
						printf("baddr %-10lu: Data Block #%zu (content: %s) -> target=%lu (predicted: %s)%s\n",
						       current_baddr,
						       data_block_count,
						       block_type, target_addr,
						       predicted_type,
						       validation_status);

						// If validation failed, provide more details
						if (strstr(validation_status,
							   "ERROR") != NULL) {
							printf("                    *** DATA CONSISTENCY BUG DETECTED ***\n");
							printf("                    Expected: %s (based on address %lu)\n",
							       predicted_type,
							       target_addr);
							printf("                    Actual content: %s\n",
							       block_type);

							// Show filesystem region information
							if (g_j_ops &&
							    g_j_ops->get_inode_region_start &&
							    g_j_ops->get_inode_region_end) {
								baddr_t inode_start =
									g_j_ops->get_inode_region_start(
										g_sb_static);
								baddr_t inode_end =
									g_j_ops->get_inode_region_end(
										g_sb_static);
								printf("                    Inode region: %lu - %lu\n",
								       inode_start,
								       inode_end);
							}

							if (g_j_ops &&
							    g_j_ops->get_data_region_start &&
							    g_j_ops->get_data_region_end) {
								baddr_t data_start =
									g_j_ops->get_data_region_start(
										g_sb_static);
								baddr_t data_end =
									g_j_ops->get_data_region_end(
										g_sb_static);
								printf("                    Data region: %lu - %lu\n",
								       data_start,
								       data_end);
							}
						}
					} else {
						printf("baddr %-10lu: Data Block #%zu (%s) -> target=unknown\n",
						       current_baddr,
						       data_block_count,
						       block_type);
					}

					printf("  Block type: %s\n",
					       block_type);

					// Handle different block types
					if (strstr(block_type,
						   "ERROR: Unknown Region") !=
					    NULL) {
						printf("    ERROR: Block address not in known filesystem region!\n");
						printf("    Block content (first 64 bytes):\n");
						dump_hex(block_data, 64);
					} else if (strcmp(block_type,
							  "Data Block") == 0) {
						printf("    Data block content (first 64 bytes):\n");
						dump_hex(block_data, 64);
					}

					// For directory blocks, try to show directory entries
					if (strstr(block_type, "Directory") !=
					    NULL) {
						printf("  Directory entries:\n");
						unsigned char *ptr = block_data;
						for (int entry = 0;
						     entry < 8 &&
						     ptr < block_data +
								     PAGE_SIZE -
								     8;
						     entry++) {
							uint32_t inode_no = *(
								uint32_t *)ptr;
							uint16_t rec_len = *(
								uint16_t *)(ptr +
									    4);
							uint8_t name_len = *(
								uint8_t *)(ptr +
									   6);

							if (inode_no == 0 ||
							    rec_len == 0 ||
							    rec_len > PAGE_SIZE)
								break;

							char name[256] = { 0 };
							if (name_len > 0 &&
							    name_len < 255) {
								memcpy(name,
								       ptr + 8,
								       name_len);
								name[name_len] =
									'\0';
							}

							// Show different info based on filesystem type
							if (strstr(block_type,
								   "EXT4") !=
							    NULL) {
								uint8_t file_type = *(
									uint8_t *)(ptr +
										   7);
								printf("    Entry %d: inode=%u, rec_len=%u, name_len=%u, type=%u, name='%s'\n",
								       entry,
								       inode_no,
								       rec_len,
								       name_len,
								       file_type,
								       name);
							} else if (strstr(block_type,
									  "SEFS") !=
								   NULL) {
								// SEFS has simpler directory structure (no file_type field)
								printf("    Entry %d: inode=%u, rec_len=%u, name_len=%u, name='%s'",
								       entry,
								       inode_no,
								       rec_len,
								       name_len,
								       name);

								// Add SEFS-specific indicators
								if (inode_no ==
								    SEFS_ROOT_INO) {
									printf(" [ROOT]");
								}
								printf(" (SEFS)\n");
							} else {
								printf("    Entry %d: inode=%u, rec_len=%u, name_len=%u, name='%s'\n",
								       entry,
								       inode_no,
								       rec_len,
								       name_len,
								       name);
							}

							ptr += rec_len;
							if (rec_len < 8)
								break; // Prevent infinite loop
						}
					}

					// For inode blocks, show some inode information
					// Skip EXT4 Inode Table blocks as they have dedicated dump function
					// Skip bitmap blocks as they don't contain actual inode data
					if (strstr(block_type, "Inode") !=
						    NULL &&
					    strstr(block_type,
						   "EXT4 Inode Table") ==
						    NULL &&
					    strstr(block_type, "Bitmap") ==
						    NULL) {
						printf("  Inode information:\n");

						// Different inode sizes for different filesystems
						int inode_size =
							128; // Default EXT4 inode size
						if (strstr(block_type,
							   "SEFS") != NULL) {
							// SEFS inode size based on mkfs.c analysis
							inode_size =
								SEFS_BLOCK_SIZE /
								SEFS_INODES_PER_BLOCK; // ~64 bytes per inode
						}

						for (int i = 0;
						     i < PAGE_SIZE / inode_size;
						     i++) {
							unsigned char *inode_ptr =
								block_data +
								(i *
								 inode_size);
							uint16_t mode =
								*(uint16_t *)
									inode_ptr;
							uint32_t size = *(
								uint32_t *)(inode_ptr +
									    4);

							if (mode != 0) {
								if (strstr(block_type,
									   "EXT4") !=
								    NULL) {
									printf("    EXT4 Inode %d: mode=0x%04x",
									       i,
									       mode);
								} else if (
									strstr(block_type,
									       "SEFS") !=
									NULL) {
									printf("    SEFS Inode %d: mode=0x%04x",
									       i,
									       mode);
								} else {
									printf("    Inode %d: mode=0x%04x",
									       i,
									       mode);
								}

								// Decode file type
								switch (mode &
									0xF000) {
								case 0x1000:
									printf(" (FIFO)");
									break;
								case 0x2000:
									printf(" (CHAR_DEV)");
									break;
								case 0x4000:
									printf(" (DIR)");
									break;
								case 0x6000:
									printf(" (BLOCK_DEV)");
									break;
								case 0x8000:
									printf(" (REG_FILE)");
									break;
								case 0xA000:
									printf(" (SYMLINK)");
									break;
								case 0xC000:
									printf(" (SOCKET)");
									break;
								default:
									printf(" (UNKNOWN)");
									break;
								}

								printf(", size=%u",
								       size);

								// Add filesystem-specific information
								if (strstr(block_type,
									   "SEFS") !=
								    NULL) {
									printf(" [SEFS specific fields available]");
								}

								printf("\n");
							}
						}
					}

					// For SEFS superblocks, show superblock information
					if (strstr(block_type,
						   "SEFS Superblock") != NULL) {
						printf("  SEFS Superblock information:\n");

						// Parse SEFS superblock (struct sefs_sb_info)
						uint32_t *magic =
							(uint32_t *)block_data;
						uint32_t *nr_blocks =
							(uint32_t *)(block_data +
								     4);
						uint32_t *nr_inodes =
							(uint32_t *)(block_data +
								     8);
						uint32_t *nr_istore_blocks =
							(uint32_t *)(block_data +
								     12);
						uint32_t *nr_ifree_blocks =
							(uint32_t *)(block_data +
								     16);
						uint32_t *nr_bfree_blocks =
							(uint32_t *)(block_data +
								     20);

						printf("    Magic: 0x%08x",
						       *magic);
						if (*magic == SEFS_MAGIC) {
							printf(" (SEFS)");
						}
						printf("\n");
						printf("    Total blocks: %u\n",
						       *nr_blocks);
						printf("    Total inodes: %u\n",
						       *nr_inodes);
						printf("    Inode store blocks: %u\n",
						       *nr_istore_blocks);
						printf("    Inode free bitmap blocks: %u\n",
						       *nr_ifree_blocks);
						printf("    Block free bitmap blocks: %u\n",
						       *nr_bfree_blocks);

						// Calculate layout information
						uint32_t inode_region_start =
							1 + *nr_ifree_blocks +
							*nr_bfree_blocks;
						uint32_t data_region_start =
							inode_region_start +
							*nr_istore_blocks;
						printf("    Layout: SB(0) -> Ifree(1-%u) -> Bfree(%u-%u) -> Inodes(%u-%u) -> Data(%u+)\n",
						       *nr_ifree_blocks,
						       1 + *nr_ifree_blocks,
						       1 + *nr_ifree_blocks +
							       *nr_bfree_blocks -
							       1,
						       inode_region_start,
						       inode_region_start +
							       *nr_istore_blocks -
							       1,
						       data_region_start);
					}

					// For EXT4 extent blocks, show extent information
					if (strstr(block_type, "EXT4 Extent") !=
					    NULL) {
						printf("  EXT4 Extent Block information:\n");

						struct ext4_extent_header *eh =
							(struct ext4_extent_header
								 *)block_data;

						uint16_t magic =
							le16toh(eh->magic);
						uint16_t entries = le16toh(
							eh->entries_count);
						uint16_t max_entries = le16toh(
							eh->max_entries_count);
						uint16_t depth =
							le16toh(eh->depth);
						uint32_t generation =
							le32toh(eh->generation);

						printf("    Magic: 0x%04x",
						       magic);
						if (magic ==
						    EXT4_EXTENT_MAGIC) {
							printf(" (EXT4_EXTENT)");
						}
						printf("\n");
						printf("    Entries: %u/%u\n",
						       entries, max_entries);
						printf("    Depth: %u\n",
						       depth);
						printf("    Generation: %u\n",
						       generation);

						// Show extent entries or index entries based on depth
						if (depth == 0) {
							// Leaf node - contains extents
							printf("    Extent entries (leaf node):\n");
							struct ext4_extent *extent =
								(struct ext4_extent
									 *)(block_data +
									    sizeof(struct ext4_extent_header));

							for (uint16_t i = 0;
							     i < entries &&
							     i < 10; // Limit output
							     i++) {
								uint32_t first_block = le32toh(
									extent[i]
										.first_block);
								uint16_t block_count = le16toh(
									extent[i]
										.block_count);
								uint16_t start_hi = le16toh(
									extent[i]
										.start_hi);
								uint32_t start_lo = le32toh(
									extent[i]
										.start_lo);
								uint64_t start =
									((uint64_t)
										 start_hi
									 << 32) |
									start_lo;

								printf("      [%u] first_block=%u, count=%u, start=%lu",
								       i,
								       first_block,
								       block_count,
								       start);

								// Check for unwritten flag
								if (block_count &
								    0x8000) {
									printf(" (unwritten)");
								}
								printf("\n");
							}
							if (entries > 10) {
								printf("      ... and %u more entries\n",
								       entries -
									       10);
							}
						} else {
							// Internal node - contains index entries
							printf("    Index entries (internal node):\n");
							struct ext4_extent_index *index =
								(struct ext4_extent_index
									 *)(block_data +
									    sizeof(struct ext4_extent_header));

							for (uint16_t i = 0;
							     i < entries &&
							     i < 10; // Limit output
							     i++) {
								uint32_t first_block = le32toh(
									index[i].first_block);
								uint32_t leaf_lo = le32toh(
									index[i].leaf_lo);
								uint16_t leaf_hi = le16toh(
									index[i].leaf_hi);
								uint64_t leaf =
									((uint64_t)
										 leaf_hi
									 << 32) |
									leaf_lo;

								printf("      [%u] first_block=%u, leaf=%lu\n",
								       i,
								       first_block,
								       leaf);
							}
							if (entries > 10) {
								printf("      ... and %u more entries\n",
								       entries -
									       10);
							}
						}
					}

					// For EXT4 block bitmap blocks, show bitmap information
					if (strstr(block_type,
						   "EXT4 Block Bitmap") !=
					    NULL) {
						printf("  EXT4 Block Bitmap information:\n");

						// Count free and used blocks
						uint32_t free_blocks = 0;
						uint32_t used_blocks = 0;

						for (int i = 0; i < PAGE_SIZE;
						     i++) {
							uint8_t byte =
								block_data[i];
							for (int bit = 0;
							     bit < 8; bit++) {
								if (byte &
								    (1
								     << bit)) {
									used_blocks++;
								} else {
									free_blocks++;
								}
							}
						}

						printf("    Total blocks tracked: %u\n",
						       free_blocks +
							       used_blocks);
						printf("    Free blocks: %u\n",
						       free_blocks);
						printf("    Used blocks: %u\n",
						       used_blocks);
						printf("    Usage: %.2f%%\n",
						       (double)used_blocks /
							       (free_blocks +
								used_blocks) *
							       100.0);

						// Show first few bits as example
						printf("    First 64 bits (blocks 0-63): ");
						for (int i = 0; i < 8; i++) {
							printf("%02x ",
							       block_data[i]);
						}
						printf("\n");
					}

					// For EXT4 inode bitmap blocks, show bitmap information
					if (strstr(block_type,
						   "EXT4 Inode Bitmap") !=
					    NULL) {
						printf("  EXT4 Inode Bitmap information:\n");

						// Count free and used inodes
						uint32_t free_inodes = 0;
						uint32_t used_inodes = 0;

						for (int i = 0; i < PAGE_SIZE;
						     i++) {
							uint8_t byte =
								block_data[i];
							for (int bit = 0;
							     bit < 8; bit++) {
								if (byte &
								    (1
								     << bit)) {
									used_inodes++;
								} else {
									free_inodes++;
								}
							}
						}

						printf("    Total inodes tracked: %u\n",
						       free_inodes +
							       used_inodes);
						printf("    Free inodes: %u\n",
						       free_inodes);
						printf("    Used inodes: %u\n",
						       used_inodes);
						printf("    Usage: %.2f%%\n",
						       (double)used_inodes /
							       (free_inodes +
								used_inodes) *
							       100.0);

						// Show first few bits as example
						printf("    First 64 bits (inodes 1-64): ");
						for (int i = 0; i < 8; i++) {
							printf("%02x ",
							       block_data[i]);
						}
						printf("\n");
					}

					// For EXT4 inode table blocks, show inode information
					if (strstr(block_type,
						   "EXT4 Inode Table") !=
					    NULL) {
						dump_ext4_inode_table_block(
							block_data,
							target_addr);
					}

					// For EXT4 reserved GDT blocks, show block information
					if (strstr(block_type,
						   "EXT4 Reserved GDT") !=
					    NULL) {
						printf("  EXT4 Reserved GDT Block information:\n");
						printf("    This block is reserved for future group descriptor table expansion\n");

						// Check if block is mostly zeros (typical for reserved blocks)
						uint32_t zero_count = 0;
						for (int i = 0; i < PAGE_SIZE;
						     i++) {
							if (block_data[i] ==
							    0) {
								zero_count++;
							}
						}

						printf("    Zero bytes: %u/%u (%.2f%%)\n",
						       zero_count, PAGE_SIZE,
						       (double)zero_count /
							       PAGE_SIZE *
							       100.0);

						if (zero_count < PAGE_SIZE) {
							printf("    First 64 non-zero bytes:\n");
							int shown = 0;
							for (int i = 0;
							     i < PAGE_SIZE &&
							     shown < 64;
							     i++) {
								if (block_data[i] !=
								    0) {
									if (shown % 16 ==
									    0) {
										printf("      ");
									}
									printf("%02x ",
									       block_data
										       [i]);
									if ((shown +
									     1) % 16 ==
									    0) {
										printf("\n");
									}
									shown++;
								}
							}
							if (shown % 16 != 0) {
								printf("\n");
							}
						}
					}

					// For EXT4 group descriptor blocks, show group information
					if (strstr(block_type,
						   "EXT4 Group Descriptor") !=
					    NULL) {
						printf("  EXT4 Group Descriptor information:\n");

						// Get EXT4 superblock info for calculations
						struct ext4_sblock *ext4_sb =
							(struct ext4_sblock *)
								g_sb_static;
						uint32_t desc_size =
							ext4_sb->desc_size;
						if (desc_size == 0)
							desc_size =
								32; // Default EXT4 descriptor size

						baddr_t fs_start =
							g_j_ops->get_fs_area_start_baddr(
								g_sb_static);
						baddr_t gdt_start =
							fs_start + 1;

						// Calculate which group descriptors are in this block
						uint32_t descs_per_block =
							PAGE_SIZE / desc_size;
						uint32_t block_offset =
							target_addr - gdt_start;
						uint32_t first_group =
							block_offset *
							descs_per_block;

						printf("    Descriptor size: %u bytes\n",
						       desc_size);
						printf("    Descriptors per block: %u\n",
						       descs_per_block);
						printf("    First group in this block: %u\n",
						       first_group);

						// Parse group descriptors in this block
						for (uint32_t i = 0;
						     i < descs_per_block &&
						     (i * desc_size +
						      desc_size) <= PAGE_SIZE;
						     i++) {
							struct ext4_group_desc *gd =
								(struct ext4_group_desc
									 *)(block_data +
									    i * desc_size);
							uint32_t group_num =
								first_group + i;

							// Only show non-zero descriptors
							if (gd->bg_block_bitmap_lo !=
								    0 ||
							    gd->bg_inode_bitmap_lo !=
								    0) {
								printf("    Group %u: Block bitmap=%u Inode bitmap=%u Inode table=%u Free blocks=%u Free inodes=%u Used dirs=%u Flags=0x%04x Checksum=0x%04x\n",
								       group_num,
								       gd->bg_block_bitmap_lo,
								       gd->bg_inode_bitmap_lo,
								       gd->bg_inode_table_lo,
								       gd->bg_free_blocks_count_lo,
								       gd->bg_free_inodes_count_lo,
								       gd->bg_used_dirs_count_lo,
								       gd->bg_flags,
								       gd->bg_checksum);
							}
						}
					}
					printf("\n");
				}
			} else { // Metadata block
				if (is_etag_blk((char *)block_data)) {
					printf("baddr %-10lu: Journal Extent Tag Block (Validated)\n",
					       current_baddr);
					if (should_dump(current_baddr, ETAG_BLK,
							baddr_to_dump,
							types_to_dump)) {
						dump_journal_extent_tag_block(
							block_data);
					}
					printf("\n");
				} else if (is_stage_trace_blk(
						   (char *)block_data)) {
					printf("baddr %-10lu: Journal Stage Trace Block (Validated)\n",
					       current_baddr);
					if (should_dump(current_baddr,
							STAGE_TRACE_BLK,
							baddr_to_dump,
							types_to_dump)) {
						dump_stage_trace_block(
							block_data);
					}
					printf("\n");
				} else {
					struct journal_meta_header *h =
						(struct journal_meta_header *)
							block_data;
					enum j_block_type btype =
						(enum j_block_type)h->blocktype;
					printf("baddr %-10lu: Error: Unexpected block type %u in metadata section\n",
					       current_baddr, btype);
					dump_hex(block_data, PAGE_SIZE);
					printf("\n");
				}
			}
		}
	}

	printf("--- End of Transaction (ID: %u) ---\n\n", tx_id);
}
