#include "config.h"
#include "fs.h"
#include "fs/sefs/sefs.h"
#include "buffer_head.h"
#include "journal.h"
#include "oxbow_debug.h"
#include "linux/bitmap.h"
#include <pthread.h>

static struct inode *sefs_alloc_inode(struct super_block *sb)
{
	UNUSED(sb);

	sefs_debug("[%s]", __func__);

	struct sefs_inode_info *ci = malloc(sizeof(struct sefs_inode_info));
	if (!ci)
		return NULL;

	memset(ci, 0, sizeof(struct sefs_inode_info));
	inode_init_always(sb, &ci->vfs_inode);
	ci->vfs_inode.free_inode = sb->s_op->free_inode;
	return &ci->vfs_inode;
}

static void sefs_destroy_inode(struct inode *inode)
{
	struct super_block *sb;
	unsigned long ino;
	struct sefs_inode_info *ci = SEFS_INODE(inode);

	sb = inode->i_sb;
	ino = inode->i_ino;

	// free related buffer head here
#ifdef OXBOW_USER_READAHEAD
	if (inode->ra_cache_buf)
		free(inode->ra_cache_buf);
	if (inode->ra_cache_index)
		free(inode->ra_cache_index);
	if (inode->ra_cache_valid)
		free(inode->ra_cache_valid);
#endif /* OXBOW_USER_READAHEAD */
	free(inode->i_mapping);
	free(ci);

	// only for sefs (lwext4 do its own mechanism)
	put_inode(sb, ino);
}

static int sefs_write_inode(struct inode *inode)
{
	struct sefs_inode *disk_inode;
	struct buffer_head *bh;
	uint64_t ino = inode->i_ino;

	int ret = -1;

	sefs_debug("[%s] inode(%d)", __func__, inode->i_ino);

	// Fetch from the cache if it exists. If not, read from the disk.
	bh = sb_bread(inode->i_sb, SEFS_INO_BLOCK(ino));
	if (!bh)
		return -EIO;

	disk_inode = (struct sefs_inode *)bh->b_data;
	disk_inode += SEFS_INO_SHIFT(ino);
	disk_inode->i_mode = inode->i_mode;
	disk_inode->i_uid = inode->i_uid;
	disk_inode->i_gid = inode->i_gid;
	disk_inode->i_size = inode->i_size;
	disk_inode->i_blocks = inode->i_blocks;
	disk_inode->i_nlink = inode->i_nlink;
	disk_inode->ei_block = SEFS_INODE(inode)->ei_block;
	// false positive no lock
	memcpy(disk_inode->i_data, SEFS_INODE(inode)->i_data,
	       sizeof(disk_inode->i_data));

	/* If using journal, do not directly store to disk. */
	if (inode->i_state & I_USE_JOURNAL) {
		if (!buffer_injournal(bh)) {
			sefs_debug("[%s] inode(%d)", __func__, inode->i_ino);
			ret = add_journal_inode(bh);
		} else
			ret = 0;
	} else {
		mark_buffer_dirty(bh);
		ret = sync_dirty_buffer(bh);
	}

	inode->i_state &= ~I_DIRTY;
	brelse(bh);

	return ret;
}

static int sefs_stage_inode(struct inode *inode,
			    struct journal_stage_header *jsh)
{
	struct sefs_inode *stage_inode;
	uint64_t ino = inode->i_ino;

	stage_inode = (struct sefs_inode *)&jsh->inode;

	if (sizeof(*stage_inode) > 256) {
		oxb_error("stage support size over");
		return -1;
	}

	stage_inode->i_mode = inode->i_mode;
	stage_inode->i_uid = inode->i_uid;
	stage_inode->i_gid = inode->i_gid;
	stage_inode->i_size = inode->i_size;
	stage_inode->i_blocks = inode->i_blocks;
	// disk_inode->i_nlink = inode->i_nlink;
	stage_inode->ei_block = SEFS_INODE(inode)->ei_block;
	memcpy(stage_inode->i_data, SEFS_INODE(inode)->i_data,
	       sizeof(stage_inode->i_data));

	// inode block number and offset
	jsh->inode_baddr = SEFS_INO_BLOCK(ino);
	jsh->inode_index = SEFS_INO_SHIFT(ino);
	jsh->inode_size = sizeof(struct sefs_inode);

	stg_debug(
		"[%s] stage inode(%d) to desc_blk baddr=%llu index=%u size=%u",
		__func__, inode->i_ino, jsh->inode_baddr, jsh->inode_index,
		jsh->inode_size);

	print_sefs_inode(stage_inode);
	hex_dump((char *)stage_inode, sizeof(struct sefs_inode));

	return 0;
}

/*
 * Return the first bit we found and clear the the following `len` consecutive
 * free bit(s) (set to 1) in a given in-memory bitmap spanning over multiple
 * blocks. Return 0 if no enough free bit(s) were found (we assume that the
 * first bit is never free because of the superblock and the root inode, thus
 * allowing us to use 0 as an error value).
 */
static uint32_t get_first_free_bits(unsigned long *freemap, unsigned long size,
				    uint32_t len)
{
	uint32_t bit, prev = 0, count = 0;
	for_each_set_bit (bit, freemap, size) {
		if (prev != bit - 1)
			count = 0;
		prev = bit;
		if (++count == len) {
			bitmap_clear(freemap, bit - len + 1, len);
			return bit - len + 1;
		}
	}
	return 0;
}

/* Mark the `len` bit(s) from i-th bit in freemap as free (i.e. 1) */
static int put_free_bits(unsigned long *freemap, unsigned long size, uint32_t i,
			 uint32_t len)
{
	/* i is greater than freemap size */
	if (i + len - 1 > size)
		return -1;

	bitmap_set(freemap, i, len);

	return 0;
}

static unsigned long sefs_get_free_inode(struct super_block *sb)
{
	struct sefs_sb_info *sbi = SEFS_SB(sb);
	struct buffer_head *bh;
	unsigned long ret, i;

	for (i = 0; i < sbi->nr_ifree_blocks; i++)
		if (sbi->ifree_bit_left[i] != 0)
			break;

	// [TODO] you can make it alloc when fragmentation exist
	if (i == sbi->nr_ifree_blocks) {
		oxb_error("no possible inodes");
		return 0;
	}

	bh = sb_bread(sb, sbi->ifree_first + i);
	if (!bh) {
		oxb_error("failed to read %d", sbi->ifree_first + i);
		return 0;
	}

	lock_buffer(bh);
	ret = get_first_free_bits((unsigned long *)bh->b_data, BITMAP_BLK_NR,
				  1);
	if (!ret) {
		oxb_error("no inode left!");
		unlock_buffer(bh);
		return ret;
	}
	ret += i * BITMAP_BLK_NR;
	mark_buffer_dirty_sb(bh, sb);
	unlock_buffer(bh);

	pthread_spin_lock(&sbi->lock);
	sbi->ifree_bit_left[i] -= 1;
	sbi->nr_free_inodes--;
	pthread_spin_unlock(&sbi->lock);

	sefs_debug("[%s] new ino(%d)", __func__, ret);

	return ret;
}

sector_t sefs_get_free_blocks(struct super_block *sb, uint32_t len)
{
	struct sefs_sb_info *sbi = SEFS_SB(sb);
	struct buffer_head *bh;
	uint32_t ret, i, old;

	BUG_ON(len > BITMAP_BLK_NR, "too big");

	for (i = 0; i < sbi->nr_bfree_blocks; i++)
		if (len <= sbi->bfree_bit_left[i])
			break;

	// [TODO] you can make it alloc when fragmentation exist
	if (i == sbi->nr_bfree_blocks) {
		oxb_error("no possible free blocks");
		return 0;
	}

	bh = sb_bread(sb, sbi->bfree_first + i);
	if (!bh)
		return 0;

	lock_buffer(bh);
	ret = get_first_free_bits((unsigned long *)bh->b_data, BITMAP_BLK_NR,
				  len);
	old = ret;
	ret += i * BITMAP_BLK_NR;
	mark_buffer_dirty_sb(bh, sb);
	unlock_buffer(bh);

	pthread_spin_lock(&sbi->lock);
	sbi->bfree_bit_left[i] -= len;
	sbi->nr_free_blocks -= len;
	pthread_spin_unlock(&sbi->lock);

	sefs_debug("[%s] new bno(%d) len(%d) from(%d) at(%d)", __func__, ret,
		   len, sbi->bfree_first + i, old);

	return ret;
}

/* Mark an inode as unused */
void sefs_put_inode(struct super_block *sb, unsigned long ino)
{
	struct sefs_sb_info *sbi = SEFS_SB(sb);
	struct buffer_head *bh;

	bh = sb_bread(sb, sbi->ifree_first + (ino / BITMAP_BLK_NR));
	if (!bh)
		return;

	lock_buffer(bh);
	if (put_free_bits((unsigned long *)bh->b_data, BITMAP_BLK_NR,
			  ino % BITMAP_BLK_NR, 1))
		oxb_error("free bit failed");
	mark_buffer_dirty_sb(bh, sb);
	unlock_buffer(bh);

	pthread_spin_lock(&sbi->lock);
	sbi->ifree_bit_left[ino / BITMAP_BLK_NR] += 1;
	sbi->nr_free_inodes++;
	pthread_spin_unlock(&sbi->lock);

	d_trace("[%s] ino(%d)", __func__, ino);
}

/* Mark len block(s) as unused */
void sefs_put_blocks(struct super_block *sb, sector_t bno, uint32_t len)
{
	struct sefs_sb_info *sbi = SEFS_SB(sb);
	struct buffer_head *bh;

	bh = sb_bread(sb, sbi->bfree_first + (bno / BITMAP_BLK_NR));
	if (!bh)
		return;

	lock_buffer(bh);
	if (put_free_bits((unsigned long *)bh->b_data, BITMAP_BLK_NR,
			  bno % BITMAP_BLK_NR, len))
		oxb_error("free bit failed");
	mark_buffer_dirty_sb(bh, sb);
	unlock_buffer(bh);

	pthread_spin_lock(&sbi->lock);
	sbi->bfree_bit_left[bno / BITMAP_BLK_NR] += len;
	sbi->nr_free_blocks += len;
	pthread_spin_unlock(&sbi->lock);

	d_debug("[%s] bno(%d) len(%d) from(%d) at(%d done)", __func__, bno, len,
		sbi->bfree_first + (bno / BITMAP_BLK_NR), bno % BITMAP_BLK_NR);
}

static int sefs_fill_journal_sb(struct super_block *sb)
{
	struct journal_control_ctx *j;
	struct sefs_sb_info *sbi = SEFS_SB(sb);
	struct buffer_head *bh;
	struct journal_superblock *journal_sb;
	struct staging_superblock *stage_sb;

	j = sb->journal;
	bh = sb_bread(sb, sbi->journal_sb);
	if (!bh) {
		sefs_debug("read fail");
		return -1;
	}
	journal_sb = (struct journal_superblock *)bh->b_data;

	j->j_transaction_sequence = journal_sb->tx_id;

	brelse(bh);

	bh = sb_bread(sb, sbi->staging_sb);
	if (!bh) {
		sefs_debug("read fail");
		return -1;
	}
	stage_sb = (struct staging_superblock *)bh->b_data;
	j->stage_start = stage_sb->start;
	j->stage_tail = stage_sb->start;
	j->stage_gap_start = 0;
	j->stage_end = stage_sb->end;
	j->stage_total = stage_sb->block_nr;
	atomic_init(&j->stg_ckpt_in_flight, false);

	oxb_info("[Fill journal SB] stagesb start %u tail %u end %u total %u",
		 j->stage_start, j->stage_tail, j->stage_end, j->stage_total);

	brelse(bh);
	return 0;
}

const struct super_operations sefs_super_ops = {
	.iget = sefs_iget,
	.alloc_inode = sefs_alloc_inode,
	.destroy_inode = sefs_destroy_inode,
	.free_inode = NULL,
	.write_inode = sefs_write_inode,
	.stage_inode = sefs_stage_inode,
	// .put_super = [TODO]
	// .sync_fs = [TODO]
	.get_free_blocks = sefs_get_free_blocks,
	.get_free_inode = sefs_get_free_inode,
	.put_blocks = sefs_put_blocks,
	.put_inode = sefs_put_inode,
	// journal related
	.fill_journal_sb = sefs_fill_journal_sb,
};

static int count_zero_bits(unsigned long *bmap)
{
	unsigned long *p;
	int ret;

	ret = SEFS_BLOCK_SIZE * 8;
	for (p = bmap; p < bmap + (PAGE_SIZE / sizeof(*p)); p++)
		ret -= __builtin_ctzll(*p);

	return ret;
}

int sefs_fill_super(struct super_block *sb)
{
	struct buffer_head *bh;
	struct sefs_sb_info *disk_sb, *sbi;
	struct inode *root;
	int i, idx, ret = -1;

	/* init superblock */
	sb->s_magic = SEFS_MAGIC;
	sb->s_maxbytes = SEFS_MAX_FILESIZE;
	sb->inodes_per_blk = SEFS_INODES_PER_BLOCK;
	sb->filename_max = SEFS_NAME_MAX;
	sb->max_subfiles = SEFS_DENT_MAX;
	sb->s_op = &sefs_super_ops;

	bh = sb_bread(sb, OXBOW_SUPER_BLOCK_NR);
	if (!bh) {
		log_error("failed to read superblock");
		return -EIO;
	}

	disk_sb = (struct sefs_sb_info *)bh->b_data;
	if (disk_sb->magic != SEFS_MAGIC) {
		log_error("wrong magic number %lx", disk_sb->magic);
		goto release;
	}

	sbi = calloc(1, sizeof(struct sefs_sb_info));
	if (!sbi) {
		log_error("out of memory for sb_info");
		goto release;
	}

	sb->s_fs_info = sbi;
	sbi->nr_blocks = disk_sb->nr_blocks;
	sbi->nr_inodes = disk_sb->nr_inodes;
	sbi->nr_istore_blocks = disk_sb->nr_istore_blocks;
	sbi->nr_ifree_blocks = disk_sb->nr_ifree_blocks;
	sbi->nr_bfree_blocks = disk_sb->nr_bfree_blocks;
	sbi->nr_free_inodes = disk_sb->nr_free_inodes;
	sbi->nr_free_blocks = disk_sb->nr_free_blocks;
	sbi->fs_features = disk_sb->fs_features;
	sbi->journal_sb = disk_sb->journal_sb;
	sbi->nr_journal_blocks = disk_sb->nr_journal_blocks;
	sbi->staging_sb = disk_sb->staging_sb;
	sbi->nr_staging_blocks = disk_sb->nr_staging_blocks;

	sbi->ifree_first = sbi->nr_istore_blocks + 1;
	sbi->bfree_first = sbi->ifree_first + sbi->nr_ifree_blocks;

	oxb_info("\n [Simple extent file system on Oxbow]\n"
		 "\tnr_blocks=%u\n"
		 "\tnr_inodes=%u (istore=%u blocks)\n"
		 "\tnr_ifree_blocks=%u\n"
		 "\tnr_bfree_blocks=%u\n"
		 "\tnr_free_inodes=%u\n"
		 "\tnr_free_blocks=%u\n"
		 "\tnr_journal_blocks=%u\n"
		 "\tnr_journal_sb=%u\n"
		 "\tnr_staging_sb=%u",

		 sbi->nr_blocks, sbi->nr_inodes, sbi->nr_istore_blocks,
		 sbi->nr_ifree_blocks, sbi->nr_bfree_blocks,
		 sbi->nr_free_inodes, sbi->nr_free_blocks,
		 sbi->nr_journal_blocks, sbi->journal_sb, sbi->staging_sb);

	if ((sbi->fs_features & SEFS_FEATURE_HAS_JOURNAL) &&
	    g_sd_conf.bg_journaling) {
		/* sb->journal is allocated here */
		sb->journal = malloc(sizeof(*sb->journal));
		if (!sb->journal) {
			oxb_error("malloc fail");
			goto free_sbi;
		}
		if (init_journal(sb)) {
			oxb_error("journal init fail");
			goto free_sbi;
		}
	}

	pthread_spin_init(&sbi->lock, PTHREAD_PROCESS_PRIVATE);

	sbi->ifree_bit_left =
		calloc(1, sizeof(uint32_t *) * sbi->nr_ifree_blocks);
	if (!sbi->ifree_bit_left) {
		oxb_error("calloc fail");
		goto free_sbi;
	}

	sbi->bfree_bit_left =
		calloc(1, sizeof(uint32_t *) * sbi->nr_bfree_blocks);
	if (!sbi->bfree_bit_left) {
		oxb_error("calloc fail");
		goto free_ibit_left;
	}

	sb->nr_inodes = sbi->nr_inodes;
	sb->nr_blocks = sbi->nr_blocks;
	sb->s_bh = bh;
	brelse(bh);

	if (sb_mballoc(sb, sbi->ifree_first,
		       sbi->nr_ifree_blocks + sbi->nr_bfree_blocks))
		goto free_bit_left;

	for (i = 0; (u32)i < sbi->nr_ifree_blocks; i++) {
		idx = sbi->ifree_first + i;

		bh = sb_bread(sb, idx);
		if (!bh) {
			oxb_error("failed to readfree bitmap");
			goto free_bit_left;
		}

		sbi->ifree_bit_left[i] =
			count_zero_bits((unsigned long *)bh->b_data);
		brelse(bh);
	}

	for (i = 0; (u32)i < sbi->nr_bfree_blocks; i++) {
		idx = sbi->bfree_first + i;

		bh = sb_bread(sb, idx);
		if (!bh) {
			oxb_error("failed to readfree bitmap");
			goto free_bit_left;
		}

		sbi->bfree_bit_left[i] =
			count_zero_bits((unsigned long *)bh->b_data);
		brelse(bh);
	}

	/* Alloc root directory inode */
	root = sefs_iget(sb, SEFS_ROOT_INO);
	if (!root)
		goto free_bit_left;

	sb->s_root = root;
	ret = 0; /* success */
	log_info("[%s] done", __func__);
	goto release;

/* ERROR */
free_ibit_left:
	free(sbi->ifree_bit_left);
free_bit_left:
	free(sbi->bfree_bit_left);
free_sbi:
	free(sbi);
	if (sb->journal)
		free(sb->journal);

release:
	return ret;
}

/* Currently, fill_super replaces the file system specific init function. */
int (*init_fs)(struct super_block *) = sefs_fill_super;
int (*exit_fs)(void) = NULL;
