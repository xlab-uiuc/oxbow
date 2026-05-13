#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <linux/fs.h>
#include <sys/types.h>
#include <unistd.h>

#include "fs/sefs/sefs.h"
#include "common/sjd.h"

int has_journal = SEFS_FEATURE_HAS_JOURNAL;

struct superblock {
	struct sefs_sb_info info;
} __attribute__((aligned(SEFS_BLOCK_SIZE)));

/* Returns ceil(a/b) */
static inline uint32_t idiv_ceil(uint32_t a, uint32_t b)
{
	uint32_t ret = a / b;
	if (a % b)
		return ret + 1;
	return ret;
}

/**
 * @brief Naive configuration for write superblock
 */
static struct superblock *write_superblock(int fd, struct stat *fstats)
{
	struct superblock *sb = malloc(sizeof(struct superblock));
	if (!sb)
		return NULL;
	// Total number of blocks.
	uint32_t nr_blocks = fstats->st_size / SEFS_BLOCK_SIZE;
	uint32_t nr_journal_blocks = nr_blocks / 5;
	if (!(has_journal & SEFS_FEATURE_HAS_JOURNAL))
		nr_journal_blocks = 0;

	// Total number of inodes. (not block)
	uint32_t nr_inodes = nr_blocks - nr_journal_blocks;
	nr_inodes /= 64; // 256KiB per inode, 16KiB is default of ext4
	uint32_t mod = nr_inodes % SEFS_INODES_PER_BLOCK;
	if (mod)
		nr_inodes += SEFS_INODES_PER_BLOCK - mod;

	// Total number of inode blocks.
	uint32_t nr_istore_blocks = idiv_ceil(nr_inodes, SEFS_INODES_PER_BLOCK);
	// # of inode free block bitmap blocks.
	uint32_t nr_ifree_blocks = idiv_ceil(nr_inodes, SEFS_BLOCK_SIZE * 8);
	// # of block free block bitmap blocks.
	uint32_t nr_bfree_blocks = idiv_ceil(nr_blocks, SEFS_BLOCK_SIZE * 8);
	uint32_t nr_data_blocks = nr_blocks - nr_istore_blocks -
				  nr_ifree_blocks - nr_bfree_blocks -
				  2 * nr_journal_blocks -
				  1 /*Super block*/; // Remains

	printf("[INFO] Data area %lu GiB\n"
	       "[INFO] Journal area %lu GiB / Staging area %lu GiB\n",
	       ((unsigned long)nr_data_blocks * PAGE_SIZE) >> 30,
	       ((unsigned long)nr_journal_blocks * PAGE_SIZE) >> 30,
	       ((unsigned long)nr_journal_blocks * PAGE_SIZE) >> 30);

	memset(sb, 0, sizeof(struct superblock));
	sb->info = (struct sefs_sb_info){
		.magic = htole32(SEFS_MAGIC),
		.nr_blocks = htole32(nr_blocks),
		.nr_inodes = htole32(nr_inodes),
		.nr_istore_blocks = htole32(nr_istore_blocks),
		.nr_ifree_blocks = htole32(nr_ifree_blocks),
		.nr_bfree_blocks = htole32(nr_bfree_blocks),
		.nr_free_inodes = htole32(nr_inodes - 1),
		.nr_free_blocks = htole32(nr_data_blocks - 1),
		.fs_features = htole32(has_journal),
		.journal_sb = htole32(nr_blocks - 2 * nr_journal_blocks),
		.nr_journal_blocks = htole32(nr_journal_blocks - 1),
		.staging_sb = htole32(nr_blocks - nr_journal_blocks),
		.nr_staging_blocks = htole32(nr_journal_blocks - 1),
	};

	int ret = write(fd, sb, sizeof(struct superblock));
	if (ret != sizeof(struct superblock)) {
		free(sb);
		return NULL;
	}

	printf("\nSuperblock: (%ld)\n"
	       "\tmagic=%#x\n"
	       "\tnr_blocks=%u\n"
	       "\tnr_inodes=%u (istore=%u blocks)\n"
	       "\tnr_ifree_blocks=%u\n"
	       "\tnr_bfree_blocks=%u\n"
	       "\tnr_free_inodes=%u\n"
	       "\tnr_free_blocks=%u\n"
	       "\tnr_journal_blocks=%u\n"
	       "\tnr_journal_sb=%lu\n"
	       "\tnr_staging_sb=%lu\n",

	       sizeof(struct superblock), sb->info.magic, sb->info.nr_blocks,
	       sb->info.nr_inodes, sb->info.nr_istore_blocks,
	       sb->info.nr_ifree_blocks, sb->info.nr_bfree_blocks,
	       sb->info.nr_free_inodes, sb->info.nr_free_blocks,
	       nr_journal_blocks, sb->info.journal_sb, sb->info.staging_sb);

	return sb;
}

static int write_journal_super_block(int fd, struct superblock *sb)
{
	struct journal_superblock *jsb = malloc(sizeof(*jsb));
	if (!jsb)
		return -1;

	jsb->common.h_blocktype = SUPER_BLK;
	jsb->common.h_magic = sb->info.magic;
	jsb->common.h_txid = 0; // Not used in superblock.

	jsb->block_size = OXBOW_BLOCK_SIZE;
	jsb->first = sb->info.journal_sb + 1;
	jsb->maxlen = sb->info.nr_journal_blocks;

	jsb->tx_id = OXBOW_JOURNAL_INIT_TXID;
	jsb->start =
		0; // 0 on normal shutdown. Otherwise, recovery is required.

	// Not used for now.
	jsb->max_transaction = 0;
	jsb->max_trans_data = 0;
	jsb->err_no = 0;

	if (lseek(fd, (loff_t)sb->info.journal_sb * PAGE_SIZE, SEEK_SET) < 0)
		return -1;

	int ret = write(fd, jsb, sizeof(struct journal_superblock));
	if (ret != sizeof(struct journal_superblock)) {
		free(jsb);
		return -1;
	}
	printf("Journal superblock: wrote at %lu block\n",
	       (loff_t)sb->info.journal_sb);

	free(jsb);
	return 0;
}

static int write_stage_super_block(int fd, struct superblock *sb)
{
	struct staging_superblock *ssb = malloc(sizeof(*ssb));
	if (!ssb)
		return -1;

	ssb->magic = SEFS_MAGIC;
	ssb->blk_size = SEFS_BLOCK_SIZE;
	ssb->block_nr = sb->info.nr_staging_blocks;
	ssb->start = sb->info.staging_sb + 1;
	ssb->end = sb->info.nr_blocks; /* NOTE: It assumes the staging area
					* is located at the end of ssd.
					*/

	if (lseek(fd, (loff_t)sb->info.staging_sb * PAGE_SIZE, SEEK_SET) < 0)
		return -1;

	int ret = write(fd, ssb, sizeof(struct staging_superblock));
	if (ret != sizeof(struct staging_superblock)) {
		free(ssb);
		return -1;
	}
	printf("stage superblock: wrote at %lu block\n",
	       (loff_t)sb->info.staging_sb);

	free(ssb);
	return 0;
}

static int write_inode_store(int fd, struct superblock *sb)
{
	/* Allocate a zeroed block for inode store */
	char *block = malloc(SEFS_BLOCK_SIZE);
	if (!block)
		return -1;

	memset(block, 0, SEFS_BLOCK_SIZE);

	/* Root inode (inode 1) */
	struct sefs_inode *inode = (struct sefs_inode *)block;
	uint32_t first_data_block = 1 + le32toh(sb->info.nr_bfree_blocks) +
				    le32toh(sb->info.nr_ifree_blocks) +
				    le32toh(sb->info.nr_istore_blocks);

	/* Not use ino 0, since glibc readdir skip ino 0 and vfs avoid using it */
	inode += SEFS_ROOT_INO;
	inode->i_mode =
		htole32(S_IFDIR | S_IRUSR | S_IRGRP | S_IROTH | S_IWUSR |
			S_IWGRP | S_IXUSR | S_IXGRP | S_IXOTH);
	inode->i_uid = 0;
	inode->i_gid = 0;
	inode->i_size = htole32(SEFS_BLOCK_SIZE);
	inode->i_ctime = inode->i_atime = inode->i_mtime = htole32(0);
	inode->i_blocks = htole32(1);
	inode->i_nlink = htole32(2);
	inode->ei_block = htole32(first_data_block);

	int ret = write(fd, block, SEFS_BLOCK_SIZE);
	if (ret != SEFS_BLOCK_SIZE) {
		ret = -1;
		goto end;
	}

	/* Reset inode store blocks to zero */
	memset(block, 0, SEFS_BLOCK_SIZE);
	ret = pwrite(fd, block, SEFS_BLOCK_SIZE,
		     first_data_block * SEFS_BLOCK_SIZE);
	printf("root inode index block offset %d\n", first_data_block);
	if (ret != SEFS_BLOCK_SIZE) {
		ret = -1;
		goto end;
	}

	uint32_t i;
	for (i = 1; i < sb->info.nr_istore_blocks; i++) {
		ret = write(fd, block, SEFS_BLOCK_SIZE);
		if (ret != SEFS_BLOCK_SIZE) {
			ret = -1;
			goto end;
		}
	}
	ret = 0;

	printf("Inode store: wrote %d blocks\n"
	       "\tinode size = %ld B\n",
	       i, sizeof(struct sefs_inode));

end:
	free(block);
	return ret;
}

static int write_ifree_blocks(int fd, struct superblock *sb)
{
	char *block = malloc(SEFS_BLOCK_SIZE);
	if (!block)
		return -1;

	uint64_t *ifree = (uint64_t *)block;

	/* Set all bits to 1 */
	memset(ifree, 0xff, SEFS_BLOCK_SIZE);

	/* First ifree block, containing first used inode */
	ifree[0] = htole64(0xfffffffffffffffc);
	int ret = write(fd, ifree, SEFS_BLOCK_SIZE);
	if (ret != SEFS_BLOCK_SIZE) {
		ret = -1;
		goto end;
	}

	/* All ifree blocks except the one containing 2 first inodes */
	ifree[0] = 0xffffffffffffffff;
	uint32_t i;
	for (i = 1; i < le32toh(sb->info.nr_ifree_blocks); i++) {
		ret = write(fd, ifree, SEFS_BLOCK_SIZE);
		if (ret != SEFS_BLOCK_SIZE) {
			ret = -1;
			goto end;
		}
	}
	ret = 0;

	printf("Ifree blocks: wrote %d blocks\n", i);

end:
	free(block);

	return ret;
}

static int write_bfree_blocks(int fd, struct superblock *sb)
{
	int ret;
	uint32_t j, i = 0;

	uint32_t nr_used = le32toh(sb->info.nr_istore_blocks) +
			   le32toh(sb->info.nr_ifree_blocks) +
			   le32toh(sb->info.nr_bfree_blocks) + 2;

	char *block = malloc(SEFS_BLOCK_SIZE);
	if (!block)
		return -1;
	uint64_t *bfree = (uint64_t *)block;

	/*
     * First blocks (incl. sb + istore + ifree + bfree + 1 used block)
     * we suppose it won't go further than the first block
     */

	/* case for metadata is not covered by 1 bfree bitmap block */
	memset(bfree, 0, SEFS_BLOCK_SIZE);
	while (nr_used > BITMAP_BLK_NR) {
		ret = write(fd, bfree, SEFS_BLOCK_SIZE);
		if (ret != SEFS_BLOCK_SIZE) {
			ret = -1;
			goto end;
		}
		nr_used -= BITMAP_BLK_NR;
		i++;
	}

	memset(bfree, 0xff, SEFS_BLOCK_SIZE);
	j = 0;
	while (nr_used) {
		uint64_t line = 0xffffffffffffffff;
		for (uint64_t mask = 0x1; mask; mask <<= 1) {
			line &= ~mask;
			nr_used--;
			if (!nr_used)
				break;
		}
		bfree[j] = htole64(line);
		j++;
	}
	i++;
	ret = write(fd, bfree, SEFS_BLOCK_SIZE);
	if (ret != SEFS_BLOCK_SIZE) {
		ret = -1;
		goto end;
	}

	/* other blocks */
	memset(bfree, 0xff, SEFS_BLOCK_SIZE);
	for (; i < le32toh(sb->info.nr_bfree_blocks); i++) {
		ret = write(fd, bfree, SEFS_BLOCK_SIZE);
		if (ret != SEFS_BLOCK_SIZE) {
			ret = -1;
			goto end;
		}
	}
	ret = 0;

	printf("Bfree blocks: wrote %d blocks\n", i);
end:
	free(block);
	return ret;
}

static int write_data_blocks(int fd, struct superblock *sb)
{
	/* FIXME: unimplemented */
	return 0;
}

static int parse_arguments(int argc, char **argv)
{
	int i, fd;

	printf("[OPTIONS] default Journal is ON\n");

	for (i = 1; i < argc; i++) {
		if (argv[i][0] == '-') {
			if (strcmp(argv[i], "-O") == 0) {
				i++;
				if (i == argc)
					return -1;
				if (strcmp(argv[i], "has_journal") == 0)
					has_journal = SEFS_FEATURE_HAS_JOURNAL;
				else if (strcmp(argv[i], "^has_journal") == 0) {
					printf("[OPTIONS] Journal off\n");
					has_journal = 0;
				} else
					return -1;
			} else if (strcmp(argv[i], "-J") == 0) {
				i++;
				if (i == argc)
					return -1;
			} else {
				fprintf(stderr,
					"Usage: -O [features]\n -J [journal options]\n");
				return -1;
			}
		} else {
			fd = open(argv[i], O_RDWR);
			if (fd == -1) {
				perror("open():");
				return -1;
			}
		}
	}

	return fd;
}

int main(int argc, char **argv)
{
	/* Open disk image */
	int fd = parse_arguments(argc, argv);
	if (fd < 0)
		return EXIT_FAILURE;

	/* Get image size */
	struct stat stat_buf;
	int ret = fstat(fd, &stat_buf);
	if (ret) {
		perror("fstat():");
		ret = EXIT_FAILURE;
		goto fclose;
	}

	/* Get block device size */
	if ((stat_buf.st_mode & S_IFMT) == S_IFBLK) {
		long int blk_size = 0;
		ret = ioctl(fd, BLKGETSIZE64, &blk_size);
		if (ret != 0) {
			perror("BLKGETSIZE64:");
			ret = EXIT_FAILURE;
			goto fclose;
		}
		stat_buf.st_size = blk_size;
		printf("[INFO] Device size is %lu GiB, SEFS maximum is %lu GiB\n",
		       stat_buf.st_size >> 30, SEFS_FS_MAX_SIZE >> 30);

		if (stat_buf.st_size > SEFS_FS_MAX_SIZE) {
			stat_buf.st_size = SEFS_FS_MAX_SIZE;
			printf("[INFO] Device size is over maximum, set to %lu GiB\n",
			       SEFS_FS_MAX_SIZE >> 30);
		}
	}

	/* Check if image is large enough */
	long int min_size = 100 * SEFS_BLOCK_SIZE;
	if (stat_buf.st_size <= min_size) {
		fprintf(stderr,
			"File is not large enough (size=%ld, min size=%ld)\n",
			stat_buf.st_size, min_size);
		ret = EXIT_FAILURE;
		goto fclose;
	}

	/* Write superblock (block 0) */
	struct superblock *sb = write_superblock(fd, &stat_buf);
	if (!sb) {
		perror("write_superblock():");
		ret = EXIT_FAILURE;
		goto fclose;
	}
	printf("write sb %ld\n", lseek(fd, 0, SEEK_CUR) / PAGE_SIZE);

	/* Write inode store blocks (from block 1) */
	ret = write_inode_store(fd, sb);
	if (ret) {
		perror("write_inode_store():");
		ret = EXIT_FAILURE;
		goto free_sb;
	}
	printf("write istore %ld\n", lseek(fd, 0, SEEK_CUR) / PAGE_SIZE);

	/* Write inode free bitmap blocks */
	ret = write_ifree_blocks(fd, sb);
	if (ret) {
		perror("write_ifree_blocks()");
		ret = EXIT_FAILURE;
		goto free_sb;
	}
	printf("write ifree %ld\n", lseek(fd, 0, SEEK_CUR) / PAGE_SIZE);

	/* Write block free bitmap blocks */
	ret = write_bfree_blocks(fd, sb);
	if (ret) {
		perror("write_bfree_blocks()");
		ret = EXIT_FAILURE;
		goto free_sb;
	}
	printf("write bfree %ld\n", lseek(fd, 0, SEEK_CUR) / PAGE_SIZE);

	/* Write data blocks */
	ret = write_data_blocks(fd, sb);
	if (ret) {
		perror("write_data_blocks():");
		ret = EXIT_FAILURE;
		goto free_sb;
	}

	if (has_journal == 0)
		goto free_sb;

	/* Write journal superblock */
	ret = write_journal_super_block(fd, sb);
	if (ret) {
		perror("write_journal_super_block():");
		ret = EXIT_FAILURE;
		goto free_sb;
	}

	/* Write staging superblock */
	ret = write_stage_super_block(fd, sb);
	if (ret) {
		perror("write_staging_super_block():");
		ret = EXIT_FAILURE;
		goto free_sb;
	}

free_sb:
	free(sb);
fclose:
	close(fd);

	return ret;
}
