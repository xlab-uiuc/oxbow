#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <string.h>
#include <endian.h>
#include <sys/stat.h>
#include "sjd.h"
#include "fs/sefs/sefs.h"
#include "ext4.h"
#include "ext4_inode.h"

// Add extent block structures and constants
#define EXT4_EXTENT_MAGIC 0xF30A

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

struct ext4_extent_idx {
    uint32_t first_block;
    uint32_t leaf_lo;
    uint16_t leaf_hi;
    uint16_t unused;
};

void print_usage(const char *prog_name) {
    fprintf(stderr, "Usage: %s -b block_addr [-D device] [-s|-d|-t|-j|-J|-i|-e|-g|-l|-S|-T|-x|-c]\n", prog_name);
    fprintf(stderr, "Options:\n");
    fprintf(stderr, "  -b block_addr   Block address to dump\n");
    fprintf(stderr, "  -D device       Block device to read from (default: /dev/nvme0n1)\n");
    fprintf(stderr, "  -s              Print as stage_trace_block\n");
    fprintf(stderr, "  -d              Print as stage_descriptor_block\n");
    fprintf(stderr, "  -t              Print as journal_extent_tag_block\n");
    fprintf(stderr, "  -j              Print as journal_descriptor_block\n");
    fprintf(stderr, "  -J              Print as journal_superblock\n");
    fprintf(stderr, "  -i              Print as sefs_inode block\n");
    fprintf(stderr, "  -e              Print as ext4_inode block\n");
    fprintf(stderr, "  -g              Print as ext4_bgroup block\n");
    fprintf(stderr, "  -l              Print as lwext4 directory block\n");
    fprintf(stderr, "  -S              Print as ext4 superblock\n");
    fprintf(stderr, "  -T              Print as staging superblock\n");
    fprintf(stderr, "  -x              Print as lwext4 extent block\n");
    fprintf(stderr, "  -c              Print as stage_commit_block\n");
    exit(1);
}

void dump_hex(const unsigned char *data, size_t size) {
    for (size_t i = 0; i < size; i++) {
        if (i % 16 == 0) {
            printf("%08lx: ", i);
        }
        printf("%02x", data[i]);
        if ((i + 1) % 2 == 0) printf(" ");
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
            if ((i + 1) % 2 == 0) printf(" ");
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

void print_etag(etag_t *tag) {
    printf("  start: %lu, cnt: %u\n", tag->start, tag->cnt);
}

void dump_stage_trace_block(const unsigned char *data) {
    struct journal_stage_trace_block *stb = (struct journal_stage_trace_block *)data;
    printf("\nStage Trace Block:\n");
    printf("Header:\n");
    printf("  blocktype: %u\n", stb->h.blocktype);
    printf("  nr: %u\n", stb->h.nr);
    printf("Transaction list:\n");
    for (uint32_t i = 0; i < stb->h.nr; i++) {
        printf("  tx_list[%u]: %lu\n", i, stb->tx_list[i]);
    }
}

void dump_stage_descriptor_block(const unsigned char *data) {
    struct stage_descriptor_block *sdb = (struct stage_descriptor_block *)data;
    printf("\nStage Descriptor Block:\n");
    printf("Header:\n");
    printf("  blocktype: %u\n", sdb->h.blocktype);
    printf("  inode: %.256s\n", sdb->h.inode);
    printf("  ino: %llu\n", sdb->h.ino);
    printf("  mrc_tx_id: %u\n", sdb->h.mrc_tx_id);
    printf("  nr_etag_blks: %u\n", sdb->h.nr_etag_blks);
    printf("  nr_tags: %u\n", sdb->h.nr_tags);
    printf("  next_etag_blk: %u\n", sdb->h.next_etag_blk);
    printf("  inode_baddr: %llu\n", sdb->h.inode_baddr);
    printf("  inode_index: %u\n", sdb->h.inode_index);
    printf("  inode_size: %u\n", sdb->h.inode_size);
    printf("Tags:\n");
    for (uint32_t i = 0; i < sdb->h.nr_tags; i++) {
        printf("  [%u] ", i);
        print_etag(&sdb->tags[i]);
    }
}

void dump_journal_extent_tag_block(const unsigned char *data) {
    struct journal_extent_tag_block *etb = (struct journal_extent_tag_block *)data;
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

void dump_journal_descriptor_block(const unsigned char *data) {
    struct journal_descriptor_block *jdb = (struct journal_descriptor_block *)data;
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

void dump_journal_superblock(const unsigned char *data) {
    struct journal_superblock *sb = (struct journal_superblock *)data;
    print_journal_sb(sb);
}

void dump_staging_superblock(const unsigned char *data) {
    struct staging_superblock *sb = (struct staging_superblock *)data;
    
    printf("\nStaging Superblock:\n");
    printf("  magic: 0x%x\n", le32toh(sb->magic));
    printf("  blk_size: %u\n", le32toh(sb->blk_size));
    printf("  block_nr (total number of blocks): %u\n", le32toh(sb->block_nr));
    printf("  start (staging starts from this block): %lu\n", le64toh(sb->start));
    printf("  end: %lu\n", le64toh(sb->end));
}

void dump_stage_commit_block(const unsigned char *data) {
    // Check if this is a valid commit block by looking at the first byte
    struct stage_commit_block *commit_blk = (struct stage_commit_block *)data;
    
    printf("\nStage Commit Block:\n");
    
    // Check if the first byte is 'C' which indicates a valid commit block
    if (commit_blk->pad[0] == 'C') {
        printf("  Status: Valid commit block (marked with 'C')\n");
    } else if (commit_blk->pad[0] == 0) {
        printf("  Status: Empty/unused commit block\n");
    } else {
        printf("  Status: Invalid or unrecognized commit block (first byte: 0x%02x '%c')\n", 
               commit_blk->pad[0], 
               (commit_blk->pad[0] >= 32 && commit_blk->pad[0] <= 126) ? commit_blk->pad[0] : '?');
    }
    
    // Show first few bytes for debugging
    printf("  First 16 bytes: ");
    for (int i = 0; i < 16; i++) {
        printf("%02x ", data[i]);
    }
    printf("\n");
}

void dump_sefs_inode_block(const unsigned char *data) {
    struct sefs_inode *inodes = (struct sefs_inode *)data;
    int inodes_per_block = SEFS_INODES_PER_BLOCK;

    printf("\nSEFS Inode Block (%d inodes per block):\n", inodes_per_block);
    
    for (int idx = 0; idx < inodes_per_block; idx++) {
        struct sefs_inode *inode = &inodes[idx];
        uint16_t mode = le16toh(inode->i_mode);
        
        // Skip printing if the inode appears unused (mode == 0)
        if (mode == 0) {
            printf("\nInode %d: unused (mode == 0)\n", idx);
            continue;
        }
        
        printf("\nInode %d:\n", idx);
        printf("  mode: %o\n", mode);
        printf("  uid: %u\n", le16toh(inode->i_uid));
        printf("  gid: %u\n", le16toh(inode->i_gid));
        printf("  nlink: %u\n", le16toh(inode->i_nlink));
        printf("  size: %lu bytes\n", le64toh(inode->i_size));
        printf("  ctime: %u\n", le32toh(inode->i_ctime));
        printf("  atime: %u\n", le32toh(inode->i_atime));
        printf("  mtime: %u\n", le32toh(inode->i_mtime));
        printf("  blocks: %u\n", le32toh(inode->i_blocks));
        printf("  ei_block: %lu\n", le64toh(inode->ei_block));
        printf("  i_data:");
        for (int i = 0; i < 8; i++) {
            printf(" %u", le32toh(inode->i_data[i]));
        }
        printf("\n");
        
        printf("  type: ");
        if (S_ISDIR(mode))
            printf("directory\n");
        else if (S_ISREG(mode))
            printf("regular file\n");
        else if (S_ISLNK(mode))
            printf("symbolic link\n");
        else
            printf("unknown\n");
    }
}

void dump_ext4_inode_block(const unsigned char *data) {
    printf("Lwext4 Inode Block\n");

    uint16_t inode_size = EXT4_INODE_SIZE;
    uint32_t inode_count = EXT4_INODE_BLOCK_SIZE / inode_size;
    for (uint32_t i = 0; i < inode_count; i++) {
        struct ext4_inode *inode = (struct ext4_inode *)((char *)data + i * inode_size);

        printf("\nInode %d (index=%u):\n", i+1, i);
        
        // Print mode with file type
        uint16_t mode = inode->mode;
        printf("  Mode: 0x%x (", mode);
        if ((mode & EXT4_INODE_MODE_TYPE_MASK) == EXT4_INODE_MODE_FIFO)
            printf("FIFO");
        else if ((mode & EXT4_INODE_MODE_TYPE_MASK) == EXT4_INODE_MODE_CHARDEV)
            printf("Character Device");
        else if ((mode & EXT4_INODE_MODE_TYPE_MASK) == EXT4_INODE_MODE_DIRECTORY)
            printf("Directory");
        else if ((mode & EXT4_INODE_MODE_TYPE_MASK) == EXT4_INODE_MODE_BLOCKDEV)
            printf("Block Device");
        else if ((mode & EXT4_INODE_MODE_TYPE_MASK) == EXT4_INODE_MODE_FILE)
            printf("Regular File");
        else if ((mode & EXT4_INODE_MODE_TYPE_MASK) == EXT4_INODE_MODE_SOFTLINK)
            printf("Symbolic Link");
        else if ((mode & EXT4_INODE_MODE_TYPE_MASK) == EXT4_INODE_MODE_SOCKET)
            printf("Socket");
        else
            printf("Unknown");
        printf("), UID: %u, GID: %u, Links: %u\n", 
               inode->uid, inode->gid, inode->links_count);

        printf("  Size: %u|%u, Blocks: %u, Flags: 0x%x\n",
               inode->size_hi, inode->size_lo, inode->blocks_count_lo, inode->flags);
        printf("  Times (a/c/m/d): %u/%u/%u/%u\n",
               inode->access_time, inode->change_inode_time,
               inode->modification_time, inode->deletion_time);
        
        printf("  Block Pointers:");
        for (int j = 0; j < EXT4_INODE_BLOCKS; j++) {
            printf(" %u", inode->blocks[j]);
        }
        printf("\n");

        printf("  Generation: %u, File ACL: %u\n", 
               inode->generation, inode->file_acl_lo);
        printf("  Extra - Size: %u, Checksum: %u\n",
               inode->extra_isize, inode->checksum_hi);
        printf("  Extra Times (c/m/a): %u/%u/%u\n",
               inode->ctime_extra, inode->mtime_extra, inode->atime_extra);
        printf("  Creation: %u (extra: %u), Version: %u\n",
               inode->crtime, inode->crtime_extra, inode->version_hi);
    }
}

void dump_ext4_bgroup_block(const unsigned char *data) {
    struct ext4_bgroup *bg = (struct ext4_bgroup *)data;
    
    printf("\nEXT4 Block Group Descriptor:\n");
    printf("  block_bitmap_lo: %u\n", le32toh(bg->block_bitmap_lo));
    printf("  inode_bitmap_lo: %u\n", le32toh(bg->inode_bitmap_lo));
    printf("  inode_table_first_block_lo: %u\n", le32toh(bg->inode_table_first_block_lo));
    printf("  free_blocks_count_lo: %u\n", le16toh(bg->free_blocks_count_lo));
    printf("  free_inodes_count_lo: %u\n", le16toh(bg->free_inodes_count_lo));
    printf("  used_dirs_count_lo: %u\n", le16toh(bg->used_dirs_count_lo));
    printf("  flags: 0x%x\n", le16toh(bg->flags));
    printf("  exclude_bitmap_lo: %u\n", le32toh(bg->exclude_bitmap_lo));
    printf("  block_bitmap_csum_lo: %u\n", le16toh(bg->block_bitmap_csum_lo));
    printf("  inode_bitmap_csum_lo: %u\n", le16toh(bg->inode_bitmap_csum_lo));
    printf("  itable_unused_lo: %u\n", le16toh(bg->itable_unused_lo));
    printf("  checksum: 0x%x\n", le16toh(bg->checksum));
    
    // 64-bit fields
    printf("  block_bitmap_hi: %u\n", le32toh(bg->block_bitmap_hi));
    printf("  inode_bitmap_hi: %u\n", le32toh(bg->inode_bitmap_hi));
    printf("  inode_table_first_block_hi: %u\n", le32toh(bg->inode_table_first_block_hi));
    printf("  free_blocks_count_hi: %u\n", le16toh(bg->free_blocks_count_hi));
    printf("  free_inodes_count_hi: %u\n", le16toh(bg->free_inodes_count_hi));
    printf("  used_dirs_count_hi: %u\n", le16toh(bg->used_dirs_count_hi));
    printf("  itable_unused_hi: %u\n", le16toh(bg->itable_unused_hi));
    printf("  exclude_bitmap_hi: %u\n", le32toh(bg->exclude_bitmap_hi));
    printf("  block_bitmap_csum_hi: %u\n", le16toh(bg->block_bitmap_csum_hi));
    printf("  inode_bitmap_csum_hi: %u\n", le16toh(bg->inode_bitmap_csum_hi));
    printf("  reserved: %u\n", le32toh(bg->reserved));
}

void dump_lwext4_dir_block(const unsigned char *data) {
    printf("\nLwext4 Directory Block:\n");
    printf("%-10s %-10s %-10s %-20s %-10s\n", 
           "Inode", "Rec_len", "Name_len", "Name", "Type");
    printf("--------------------------------------------------------\n");

    const struct ext4_dir_en *de = (const struct ext4_dir_en *)data;
    const unsigned char *addr_limit = data + PAGE_SIZE;

    while ((const unsigned char *)de < addr_limit) {
        uint32_t inode = le32toh(de->inode);
        uint16_t rec_len = le16toh(de->entry_len);
        uint8_t name_len = de->name_len;
        uint8_t file_type = de->in.inode_type;

        // Break if we reach an invalid entry
        if (rec_len == 0 || (const unsigned char *)de + rec_len > addr_limit)
            break;

        // Print entry information if it's a valid entry
        if (inode != 0) {
            char name_buf[256];
            memcpy(name_buf, de->name, name_len);
            name_buf[name_len] = '\0';

            const char *type_str;
            switch (file_type) {
                case EXT4_DE_UNKNOWN:   type_str = "Unknown"; break;
                case EXT4_DE_REG_FILE:  type_str = "File"; break;
                case EXT4_DE_DIR:       type_str = "Dir"; break;
                case EXT4_DE_CHRDEV:    type_str = "CharDev"; break;
                case EXT4_DE_BLKDEV:    type_str = "BlkDev"; break;
                case EXT4_DE_FIFO:      type_str = "FIFO"; break;
                case EXT4_DE_SOCK:      type_str = "Socket"; break;
                case EXT4_DE_SYMLINK:   type_str = "Symlink"; break;
                default:                type_str = "Unknown"; break;
            }

            printf("%-10u %-10u %-10u %-20s %-10s\n",
                   inode, rec_len, name_len, name_buf, type_str);
        }

        // Jump to next entry
        de = (const struct ext4_dir_en *)((const unsigned char *)de + rec_len);
    }
}

void dump_ext4_superblock(const unsigned char *data) {
    char *sb_start;

    sb_start = (char *)(data + 1024); // EXT4_SUPERBLOCK_OFFSET is 1024
    struct ext4_sblock *sb = (struct ext4_sblock *)sb_start;
    
    printf("\nEXT4 Superblock:\n");
    printf("  Inodes count: %u\n", le32toh(sb->inodes_count));
    printf("  Blocks count: %u\n", le32toh(sb->blocks_count_lo));
    printf("  Reserved blocks count: %u\n", le32toh(sb->reserved_blocks_count_lo));
    printf("  Free blocks count: %u\n", le32toh(sb->free_blocks_count_lo));
    printf("  Free inodes count: %u\n", le32toh(sb->free_inodes_count));
    printf("  First data block: %u\n", le32toh(sb->first_data_block));
    printf("  Block size: %u\n", 1024 << le32toh(sb->log_block_size));
    printf("  Fragments per group: %u\n", le32toh(sb->frags_per_group));
    printf("  Blocks per group: %u\n", le32toh(sb->blocks_per_group));
    printf("  Inodes per group: %u\n", le32toh(sb->inodes_per_group));
    printf("  Mount time: %u\n", le32toh(sb->mount_time));
    printf("  Write time: %u\n", le32toh(sb->write_time));
    printf("  Mount count: %u\n", le16toh(sb->mount_count));
    printf("  Max mount count: %u\n", le16toh(sb->max_mount_count));
    printf("  Magic signature: 0x%x\n", le16toh(sb->magic));
    printf("  File system state: 0x%x\n", le16toh(sb->state));
    printf("  Error handling: 0x%x\n", le16toh(sb->errors));
    printf("  Minor revision level: 0x%x\n", le16toh(sb->minor_rev_level));
    printf("  Last check time: %u\n", le32toh(sb->last_check_time));
    printf("  Check interval: %u\n", le32toh(sb->check_interval));
    printf("  Creator OS: 0x%x\n", le32toh(sb->creator_os));
    printf("  Revision level: %u\n", le32toh(sb->rev_level));
    printf("  Default uid for reserved blocks: %u\n", le16toh(sb->def_resuid));
    printf("  Default gid for reserved blocks: %u\n", le16toh(sb->def_resgid));
    
    // EXT4 specific fields
    printf("  First non-reserved inode: %u\n", le32toh(sb->first_inode));
    printf("  Inode size: %u\n", le16toh(sb->inode_size));
    printf("  Block group number of this superblock: %u\n", le16toh(sb->block_group_index));
    printf("  Compatible feature flags: 0x%x\n", le32toh(sb->features_compatible));
    printf("  Incompatible feature flags: 0x%x\n", le32toh(sb->features_incompatible));
    printf("  Read-only compatible feature flags: 0x%x\n", le32toh(sb->features_read_only));
    printf("  Volume ID: ");
    for (int i = 0; i < 16; i++) {
        printf("%02x", sb->uuid[i]);
    }
    printf("\n");
    printf("  Volume name: %.16s\n", sb->volume_name);
    printf("  Last mounted directory: %.64s\n", sb->last_mounted);
    printf("  Algorithm bitmap: 0x%x\n", le32toh(sb->algorithm_usage_bitmap));
    printf("  Blocks count (high 32-bits): %u\n", le32toh(sb->blocks_count_hi));
    printf("  Reserved blocks count (high 32-bits): %u\n", le32toh(sb->reserved_blocks_count_hi));
    printf("  Free blocks count (high 32-bits): %u\n", le32toh(sb->free_blocks_count_hi));
    printf("  Minimum extra inode size: %u\n", le16toh(sb->min_extra_isize));
    printf("  Wanted extra inode size: %u\n", le16toh(sb->want_extra_isize));
    printf("  Flags: 0x%x\n", le32toh(sb->flags));
    printf("  Checksum: 0x%x\n", le32toh(sb->checksum));
}

void dump_lwext4_extent_block(const unsigned char *data) {
    const struct ext4_extent_header *eh = (const struct ext4_extent_header *)data;
    
    printf("\n=== EXTENT TREE DEBUG: EXTENT_TREE_BLOCK_DUMP ===\n");
    
    // Validate magic number
    if (le16toh(eh->magic) != EXT4_EXTENT_MAGIC) {
        printf("Error: Invalid magic number 0x%x (expected 0x%x)\n", 
               le16toh(eh->magic), EXT4_EXTENT_MAGIC);
        return;
    }
    
    uint16_t entries = le16toh(eh->entries_count);
    uint16_t max_entries = le16toh(eh->max_entries_count);
    uint16_t depth = le16toh(eh->depth);
    uint32_t generation = le32toh(eh->generation);
    
    if (entries > max_entries) {
        printf("Error: entries_count (%u) > max_entries_count (%u)\n", entries, max_entries);
        return;
    }
    
    printf("Extent Tree Structure:\n");
    printf("Extent Header (depth %u):\n", depth);
    printf("  magic: 0x%x\n", le16toh(eh->magic));
    printf("  entries: %u / %u\n", entries, max_entries);
    printf("  depth: %u\n", depth);
    printf("  generation: %u\n", generation);
    
    if (depth == 0) {
        // Leaf node - contains actual extents
        const struct ext4_extent *extent = (const struct ext4_extent *)(data + sizeof(struct ext4_extent_header));
        
        for (int i = 0; i < entries; i++) {
            uint32_t first_block = le32toh(extent[i].first_block);
            uint16_t block_count = le16toh(extent[i].block_count);
            uint16_t start_hi = le16toh(extent[i].start_hi);
            uint32_t start_lo = le32toh(extent[i].start_lo);
            uint64_t start_addr = ((uint64_t)start_hi << 32) | start_lo;
            
            printf("  Extent %d: [%u-%u] -> [%lu-%lu] (len=%u)\n",
                   i, first_block, first_block + block_count - 1,
                   start_addr, start_addr + block_count - 1, block_count);
        }
    } else {
        // Internal node - contains indexes to other extent tree nodes
        const struct ext4_extent_idx *idx = (const struct ext4_extent_idx *)(data + sizeof(struct ext4_extent_header));
        
        for (int i = 0; i < entries; i++) {
            uint32_t first_block = le32toh(idx[i].first_block);
            uint32_t leaf_lo = le32toh(idx[i].leaf_lo);
            uint16_t leaf_hi = le16toh(idx[i].leaf_hi);
            uint64_t leaf_addr = ((uint64_t)leaf_hi << 32) | leaf_lo;
            
            printf("Index %d: [%u+] -> %lu\n", i, first_block, leaf_addr);
        }
    }
    
    printf("========================================\n");
}

int main(int argc, char *argv[]) {
    int opt;
    unsigned long block_addr = 0;
    int dump_type = 0;  // 0: hex only, 1: stage_trace, 2: stage_desc, 3: extent_tag, 4: journal_desc, 5: journal_superblock, 6: sefs_inode, 7: ext4_inode, 8: ext4_bgroup, 9: lwext4_dir, 10: ext4_superblock, 11: staging_superblock, 12: lwext4_extent, 13: stage_commit
    unsigned char block_data[PAGE_SIZE];
    const char *device = "/dev/nvme0n1";  // default device
    
    while ((opt = getopt(argc, argv, "b:D:sdtjJieglSTxc")) != -1) {
        switch (opt) {
            case 'b':
                block_addr = strtoul(optarg, NULL, 0);
                break;
            case 'D':
                device = optarg;
                break;
            case 's':
                dump_type = 1;
                break;
            case 'd':
                dump_type = 2;
                break;
            case 't':
                dump_type = 3;
                break;
            case 'j':
                dump_type = 4;
                break;
            case 'J':
                dump_type = 5;
                break;
            case 'i':
                dump_type = 6;
                break;
            case 'e':
                dump_type = 7;
                break;
            case 'g':
                dump_type = 8;
                break;
            case 'l':
                dump_type = 9;
                break;
            case 'S':
                dump_type = 10;
                break;
            case 'T':
                dump_type = 11;
                break;
            case 'x':
                dump_type = 12;
                break;
            case 'c':
                dump_type = 13; // New case for stage_commit_block
                break;
            default:
                print_usage(argv[0]);
        }
    }

    // if (block_addr == 0) {
    //     print_usage(argv[0]);
    // }

    // Open the block device
    int fd = open(device, O_RDONLY);
    if (fd < 0) {
        fprintf(stderr, "Failed to open block device %s: ", device);
        perror("");
        return 1;
    }

    // Seek to the block address
    if (lseek(fd, block_addr * PAGE_SIZE, SEEK_SET) < 0) {
        perror("Failed to seek to block");
        close(fd);
        return 1;
    }

    // Read the block
    if (read(fd, block_data, PAGE_SIZE) != PAGE_SIZE) {
        perror("Failed to read block");
        close(fd);
        return 1;
    }

    close(fd);

    // Print hex dump
    printf("Hex dump of block %lu:\n", block_addr);
    dump_hex(block_data, PAGE_SIZE);

    // Print structured data if requested
    switch (dump_type) {
        case 1:
            dump_stage_trace_block(block_data);
            break;
        case 2:
            dump_stage_descriptor_block(block_data);
            break;
        case 3:
            dump_journal_extent_tag_block(block_data);
            break;
        case 4:
            dump_journal_descriptor_block(block_data);
            break;
        case 5:
            dump_journal_superblock(block_data);
            break;
        case 6:
            dump_sefs_inode_block(block_data);
            break;
        case 7:
            dump_ext4_inode_block(block_data);
            break;
        case 8:
            dump_ext4_bgroup_block(block_data);
            break;
        case 9:
            dump_lwext4_dir_block(block_data);
            break;
        case 10:
            dump_ext4_superblock(block_data);
            break;
        case 11:
            dump_staging_superblock(block_data);
            break;
        case 12:
            dump_lwext4_extent_block(block_data);
            break;
        case 13:
            dump_stage_commit_block(block_data);
            break;
    }

    return 0;
}
