#include "fs.h"
#include "fs/sefs/sefs.h"
#include "buffer_head.h"
#include "oxbow_debug.h"

#include <fcntl.h>
#include <stddef.h>
#include <stdint.h>

/*
 * Search the extent which contain the target block.
 * Retrun the first unused file index if not found.
 * Return -1 if it is out of range.
 * TODO: use binary search.
 */
uint32_t sefs_ext_search(struct sefs_ei_block *index, uint32_t iblock)
{
	uint32_t i;
	for (i = 0; i < SEFS_MAX_EXTENTS; i++) {
		uint32_t block = index->extents[i].iblock;
		uint32_t len = index->extents[i].len;
		if (index->extents[i].bno == 0 ||
		    (iblock >= block && iblock < block + len))
			return i;
	}
	return -1;
}

#define MATCH_EXTENT(blocks)                                                   \
	((blocks) == 1	  ? 1 :                                                \
	 (blocks) <= 8	  ? 8 :                                                \
	 (blocks) <= 32	  ? 32 :                                               \
	 (blocks) <= 128  ? 128 :                                              \
	 (blocks) <= 512  ? 512 :                                              \
	 (blocks) <= 2048 ? 2048 :                                             \
	 (blocks) <= 8192 ? 8192 :                                             \
			    32768)

static int alloc_file_block(struct inode *inode, struct buffer_head *bh, u32 nr,
			    u32 ei)
{
	struct sefs_ei_block *index;
	struct sefs_extent *exts;
	sector_t bno;
	size_t blks;

	sefs_debug("[%s] start", __func__);

	index = (struct sefs_ei_block *)bh->b_data;
	exts = index->extents;

	if (ei > 3) /* if we found contiguous pattern allocate more blocks */
		if (exts[ei - 1].len == MATCH_EXTENT(nr))
			if (exts[ei - 2].len == MATCH_EXTENT(nr)) {
				if (exts[ei - 3].len == MATCH_EXTENT(nr)) {
					if (exts[ei - 4].len ==
					    MATCH_EXTENT(nr))
						nr *= 4;
				}
			}

	blks = ei ? max(exts[ei - 1].len, MATCH_EXTENT(nr)) : MATCH_EXTENT(nr);

	bno = get_free_blocks(inode->i_sb, blks);
	if (!bno)
		return -1;

	lock_buffer(bh);
	exts[ei].bno = bno;
	exts[ei].len = blks;
	if (ei && (exts[ei].bno != 0))
		exts[ei].iblock = exts[ei - 1].iblock + exts[ei - 1].len;

	mark_buffer_dirty_inode(bh, inode);
	unlock_buffer(bh);

	sefs_debug("[%s] ei(%d) index(%d) bno,len (%d,%d)", __func__, ei,
		   exts[ei].iblock, bno, blks);
	return 0;
}

static int get_next_ei_block(struct inode *inode, struct buffer_head **bh)
{
	struct sefs_ei_block *index;
	off_t last;
	baddr_t bno;

	sefs_debug("[%s]", __func__);

	index = (struct sefs_ei_block *)(*bh)->b_data;
	if (index->next != 0) {
		brelse(*bh);
		*bh = sb_bread(inode->i_sb, index->next);
		if (!(*bh)) {
			oxb_error("fail read next ei_block");
			return -1;
		}

		return 0;
	}

	/* allocate next ei_block  */
	last = index->extents[SEFS_MAX_EXTENTS - 1].iblock +
	       index->extents[SEFS_MAX_EXTENTS - 1].len;
	bno = get_free_blocks(inode->i_sb, 1);
	if (!bno) {
		brelse(*bh);
		return -1;
	}

	lock_buffer(*bh);
	index->next = bno;
	mark_buffer_dirty_inode(*bh, inode);
	unlock_buffer(*bh);
	brelse(*bh);

	*bh = sb_balloc(inode->i_sb, bno);
	if (!*bh)
		return -1;

	/* scrub ei_block */
	memset((*bh)->b_data, 0, SEFS_BLOCK_SIZE);

	/* index from previous block's last */
	index = (struct sefs_ei_block *)(*bh)->b_data;
	index->extents[0].iblock = last;

	mark_buffer_dirty_inode(*bh, inode);

	return 0;
}

/**
  * @brief get blocks lba from indexing block, if not exist allocate
  *
  * @param iblock (logical block index in file)
  * @param create (if > 0, then nr of creating blocks)
  * @return int (success on 0, not found or failed to alloc return -1)
  */
static int sefs_get_block(struct inode *inode, struct buffer_extent *be,
			  sector_t iblock, u32 create)
{
	struct sefs_ei_block *index;
	struct buffer_head *bh;
	uint32_t ei;
	sector_t n_iblock;
	u32 n_create;
	int ret;

	BUG_ON(S_ISDIR(inode->i_mode), "not expected");

	ret = -1;
	/* If block number exceeds filesize, fail */
	if (iblock > (SEFS_MAX_FILESIZE - 1) / PAGE_SIZE) {
		oxb_error("index too big %d", iblock);
		goto ret;
	}

	bh = sb_bread(inode->i_sb, SEFS_INODE(inode)->ei_block);
	if (!bh)
		goto ret;

search:
	/* read first index block */
	index = (struct sefs_ei_block *)bh->b_data;
	ei = sefs_ext_search(index, iblock);
	if ((int)ei == -1) {
		if (get_next_ei_block(inode, &bh)) {
			oxb_error("fail to get ei_block");
			goto ret;
		}
		goto search;
	}

	if (!create) { /* Read case */
		if ((iblock == 0 && index->extents[ei].len != 0) ||
		    index->extents[ei].bno != 0)
			goto done;
		oxb_error(
			"inode(%lu) failed to find block iblock %d ei %d / ei_len %d/ bno %d",
			inode->i_ino, iblock, ei, index->extents[ei].len,
			index->extents[ei].bno);
		goto brelse_index;
	}

	/* this extent contains all what we want, may not expected */
	if (iblock + create <=
	    (index->extents[ei].iblock + index->extents[ei].len)) {
		// sefs_debug("unusual case");
		goto done;
	}

	/* Lucky! we just allocate new blocks in here */
	if (index->extents[ei].bno == 0) {
		if (alloc_file_block(inode, bh, create, ei)) {
			oxb_error("fail to alloc");
			goto brelse_index;
		}
		goto done;
	}

	/* complex routine, buffer contains iblock. But we have to alloc more */
	if (be) {
		be->iblock = index->extents[ei].iblock;
		be->lba = index->extents[ei].bno;
		be->nr = index->extents[ei].len;
	}

retry:
	n_iblock = index->extents[ei].iblock + index->extents[ei].len;
	n_create = create - (n_iblock - iblock);

cr_search:
	index = (struct sefs_ei_block *)bh->b_data;
	ei = sefs_ext_search(index, n_iblock);
	if ((int)ei == -1) {
		if (get_next_ei_block(inode, &bh)) {
			oxb_error("fail to get ei_block");
			goto ret;
		}
		goto cr_search;
	}

	/* allocate remain blocks */
	if (index->extents[ei].bno == 0) {
		if (alloc_file_block(inode, bh, n_create, ei)) {
			oxb_error("fail to alloc");
			goto brelse_index;
		}
	}

	/* if extents are allocated with small size, this would be challenging */
	if (n_iblock + n_create >
	    (index->extents[ei].iblock + index->extents[ei].len)) {
		sefs_debug("find next ei %d", ei);
		goto retry;
	}

	sefs_debug("[%s] index(%d) create blocks(%d)", __func__, iblock,
		   create);

	ret = 0;
	goto ret;
	/* ===============================================================  */

done:
	/* fill buffer_extent containing iblock */
	if (be) {
		be->iblock = index->extents[ei].iblock;
		be->lba = index->extents[ei].bno;
		be->nr = index->extents[ei].len;
	}
	ret = 0;

brelse_index:
	brelse(bh);
ret:
	return ret;
}

int sefs_block_alloc(struct inode *inode, sector_t iblock)
{
	return mpage_block_alloc(inode, iblock);
}

void sefs_readpage(struct read_control *rc)
{
	mpage_readpage(rc);
}

void sefs_readahead(struct readahead_control *rac)
{
	mpage_readahead(rac);
}

static const struct address_space_operations sefs_aops = {
	.block_alloc = sefs_block_alloc,
	.readpage = sefs_readpage,
	.readahead = sefs_readahead,
};

/* Get inode block from disk and get inode struct */
struct inode *sefs_iget(struct super_block *sb, unsigned long ino)
{
	struct inode *inode;
	struct sefs_inode *cinode;
	struct buffer_head *bh;

	inode = NULL;
	if (ino >= SEFS_SB(sb)->nr_inodes) {
		log_error("inode is out of range.");
		goto ret;
	}

	/* Get a locked inode structure */
	inode = iget_locked(sb, ino);
	if (!inode)
		goto ret;

	/* If inode is in cache, return it */
	if (!(inode->i_state & I_NEW))
		return inode;

	/* Read inode from disk and initialize */
	bh = sb_bread(sb, SEFS_INO_BLOCK(ino));
	if (!bh) {
		log_error("failed to read inode from disk.");
		unlock_new_inode(inode);
		inode = NULL;
		goto ret;
	}

	cinode = (struct sefs_inode *)bh->b_data;
	cinode += SEFS_INO_SHIFT(ino);

	inode->i_ino = ino;
	inode->i_sb = sb;

	inode->i_mode = cinode->i_mode;
	BUG_ON(inode->i_mode == 0, "inode mode is 0");
	inode->i_size = cinode->i_size;
	inode->i_ctime.tv_sec = cinode->i_ctime;
	inode->i_ctime.tv_nsec = 0;
	inode->i_atime.tv_sec = cinode->i_atime;
	inode->i_atime.tv_nsec = 0;
	inode->i_mtime.tv_sec = cinode->i_mtime;
	inode->i_mtime.tv_nsec = 0;
	inode->i_blocks = cinode->i_blocks;
	inode->i_uid = cinode->i_uid;
	inode->i_gid = cinode->i_gid;
	inode->i_nlink = cinode->i_nlink;

	sefs_debug("[%s] inode(%d) size %d, uid,gid (%d,%d) type: [%s]",
		   __func__, inode->i_ino, inode->i_size, inode->i_uid,
		   inode->i_gid,
		   S_ISDIR(inode->i_mode) ?
			   "Directory" :
			   (S_ISREG(inode->i_mode) ? "Regular" : "Unknown"));

	if (S_ISDIR(inode->i_mode)) {
		SEFS_INODE(inode)->d_info_block = cinode->ei_block;
		inode->i_fop = &sefs_dir_ops;
		inode->i_op = &sefs_dir_inode_operation;
	} else if (S_ISREG(inode->i_mode)) {
		SEFS_INODE(inode)->ei_block = cinode->ei_block;
		inode->i_mapping->a_ops = &sefs_aops;
		inode->i_op = &sefs_inode_operation;
	} else
		log_error("i_mode %d, ino %d", inode->i_mode, ino);

	// else if (S_ISLNK(inode->i_mode)) {
	// 	strncpy(ci->i_data, cinode->i_data, sizeof(ci->i_data));
	// 	inode->i_link = ci->i_data;
	// 	inode->i_op = &symlink_inode_ops;
	// }
	brelse(bh);

	/* Unlock the inode to make it usable */
	unlock_new_inode(inode);
ret:
	return inode;
}

static struct inode *sefs_lookup(struct inode *dir, const char *fname)
{
	struct super_block *sb;
	struct buffer_head *bh, *bh2;
	struct sefs_d_info_block *d_info;
	struct sefs_dir_block *dblock;
	struct sefs_dirent *f;
	struct inode *ret;
	baddr_t bno;
	uint32_t ei, bi, fi;

	sefs_debug("[%s] file(%s)", __func__, fname);

	ret = NULL;
	sb = dir->i_sb;

	bh = sb_bread(sb, SEFS_INODE(dir)->d_info_block);
	if (!bh)
		return ERR_PTR(-EIO);

	d_info = (struct sefs_d_info_block *)bh->b_data;

	if (d_info->nr_files == 0) {
		brelse(bh);
		return ret;
	}

	/* Search for the file in directory */
	for (ei = 0; ei < SEFS_D_INFO_ENT_MAX; ei++) {
		if (d_info->ents[ei].nr_files == 0)
			continue;

		bno = d_info->ents[ei].d_ext.bno;
		/* Iterate blocks in extent */
		for (bi = 0; bi < SEFS_DENT_BLOCKS; bi++) {
			bh2 = sb_bread(sb, bno + bi);
			if (!bh2) {
				log_error("fail to read");
				goto search_end;
			}

			dblock = (struct sefs_dir_block *)bh2->b_data;
			if (dblock->nr_files == 0) {
				brelse(bh2);
				continue;
			}

			/* Search file in ei_block */
			for (fi = 0; fi < SEFS_DENT_PER_BLOCK; fi++) {
				f = &dblock->files[fi];

				if (!strncmp(f->name, fname, SEFS_NAME_MAX)) {
					ret = sefs_iget(dir->i_sb, f->ino);
					brelse(bh2);
					goto search_end;
				}
			}
			brelse(bh2);
			bh2 = NULL;
		}
	}

search_end:
	brelse(bh);

	/* Update directory access time */
	// dir->i_atime = current_time(dir);
	// mark_inode_dirty(dir);

	sefs_debug("[%s] file(%s) %s", __func__, fname, ret ? "found" : "none");
	return ret;
}

/* Create a new inode in dir */
static struct inode *sefs_new_inode(struct inode *dir, mode_t mode)
{
	struct inode *inode;
	struct super_block *sb;
	struct buffer_head *bh;
	unsigned long ino, bno;

	/* Check mode before doing anything to avoid undoing everything */
	if (!S_ISDIR(mode) && !S_ISREG(mode) && !S_ISLNK(mode)) {
		log_error("File type not supported");
		return ERR_PTR(-EINVAL);
	}

	/* Check if inodes are available */
	sb = dir->i_sb;
	if (SEFS_SB(sb)->nr_free_inodes == 0 ||
	    SEFS_SB(sb)->nr_free_blocks == 0)
		return ERR_PTR(-ENOSPC);

	/* Get a new free inode */
	ino = get_free_inode(sb);
	if (!ino)
		return ERR_PTR(-ENOSPC);

	inode = iget_locked(sb, ino);
	if (IS_ERR(inode))
		goto put_ino;

	/* Get a free block for this new inode's index */
	bno = get_free_blocks(sb, 1);
	if (!bno)
		goto put_inode;

	/* Scrub ei_block for new file/directory  */
	bh = sb_bread(sb, bno);
	if (!bh)
		goto put_inode;
	memset((char *)bh->b_data, 0, SEFS_BLOCK_SIZE);
	mark_buffer_dirty_inode(bh, inode); /* ei_block */
	brelse(bh);

	/* Initialize inode */
	inode->i_blocks = 1;
	if (S_ISDIR(mode)) {
		SEFS_INODE(inode)->d_info_block = bno;
		inode->i_size = SEFS_BLOCK_SIZE;
		inode->i_fop = &sefs_dir_ops;
		inode->i_op = &sefs_dir_inode_operation;
		inode->i_nlink = 2; /* due to . itself .. is for directory */
	} else if (S_ISREG(mode)) {
		SEFS_INODE(inode)->ei_block = bno;
		inode->i_size = 0;
		inode->i_op = &sefs_inode_operation;
		inode->i_mapping->a_ops = &sefs_aops;
		inode->i_nlink = 1;
	} else
		oxb_error("unknown mode");

	/* daemon must know the mode */
	inode->i_mode = mode;
	BUG_ON(inode->i_mode == 0, "inode mode is 0");
	// inode->i_ctime = inode->i_atime = inode->i_mtime = current_time(inode);

	unlock_new_inode(inode);
	return inode;

put_inode:
	// iput(inode);
put_ino:
	put_inode(sb, ino);

	return inode;
}

static int sefs_create(struct inode *dir, struct inode **ret_i, mode_t mode,
		       const char *fname)
{
	struct super_block *sb;
	struct inode *inode;
	struct sefs_d_info_block *d_info;
	struct sefs_dir_block *dblock;
	struct buffer_head *bh, *bh2;
	int ret, alloc, bno, ei, bi, fi, i;

	alloc = false;
	ret = -1;

	sb = dir->i_sb;
	bh = sb_bread(sb, SEFS_INODE(dir)->d_info_block);
	if (!bh)
		return ret;
	d_info = (struct sefs_d_info_block *)bh->b_data;

	/* directory has a limit on this version */
	if (d_info->nr_files > SEFS_DENT_MAX - 1) {
		oxb_error("[%s] dir(%d) is full", __func__, dir->i_ino);
		return ret;
	}

	/* Get a new free inode */
	inode = sefs_new_inode(dir, mode);
	if (IS_ERR(inode)) {
		ret = PTR_ERR(inode);
		goto end;
	}
	print_file_type(inode);
	*ret_i = inode; /* inode can be seen to userfaultfd */

	/* search extent first, which directory has a enough room for new */
	for (ei = 0; ei < (int)SEFS_D_INFO_ENT_MAX; ei++) {
		if (d_info->ents[ei].d_ext.bno == 0) {
			bno = get_free_blocks(sb, SEFS_DENT_BLOCKS);
			if (!bno) {
				oxb_error("fail to get free block");
				ret = -ENOSPC;
				goto iput;
			}

			lock_buffer(bh);
			d_info->ents[ei].d_ext.bno = bno;
			d_info->ents[ei].d_ext.len = SEFS_DENT_BLOCKS;
			d_info->ents[ei].d_ext.iblock = ei * SEFS_DENT_BLOCKS;
			mark_buffer_dirty_inode(bh, dir);
			unlock_buffer(bh);

			alloc = true;
			/* scrub directory entries */
			for (i = 0; i < SEFS_DENT_BLOCKS; i++) {
				bh2 = sb_bread(sb, bno + i);
				memset(bh2->b_data, 0, SEFS_BLOCK_SIZE);
				mark_buffer_dirty_inode(bh2, dir);
				brelse(bh2);
			}
		}

		if (d_info->ents[ei].nr_files < SEFS_DENT_PER_EXT)
			break;
	}

	if (ei == SEFS_D_INFO_ENT_MAX) {
		oxb_error("[%s] inconsistent");
		goto iput;
	}

	bno = d_info->ents[ei].d_ext.bno;
	for (bi = 0; bi < SEFS_DENT_BLOCKS; bi++) {
		bh2 = sb_bread(sb, bno + bi);
		if (!bh2)
			goto put_block;

		dblock = (struct sefs_dir_block *)bh2->b_data;

		/* found the target block */
		if (dblock->nr_files < SEFS_DENT_PER_BLOCK)
			break;

		brelse(bh2);
	}

	if (bi == SEFS_DENT_BLOCKS) {
		oxb_error("[%s] inconsistent");
		goto iput;
	}

	for (fi = 0; fi < (int)SEFS_DENT_PER_BLOCK; fi++) {
		if (dblock->files[fi].ino == 0) {
			lock_buffer(bh2);
			dblock->files[fi].ino = inode->i_ino;
			strncpy(dblock->files[fi].name, fname, SEFS_NAME_MAX);
			dblock->nr_files++;
			unlock_buffer(bh2);

			lock_buffer(bh);
			d_info->ents[ei].nr_files++;
			d_info->nr_files++;
			unlock_buffer(bh);
			break;
		}
	}

	if (fi == SEFS_DENT_PER_BLOCK) {
		oxb_error("[%s] inconsistent");
		goto iput;
	}

	mark_buffer_dirty_inode(bh2, dir); /* dirent block */
	mark_buffer_dirty_inode(bh, dir); /* directory d_info_block */
	brelse(bh2);
	brelse(bh);

	/* Update stats and mark dir and new inode dirty */
	mark_inode_dirty(inode);
	// dir->i_mtime = dir->i_atime = dir->i_ctime = current_time(dir);
	if (S_ISDIR(mode))
		dir->i_nlink++;
	mark_inode_dirty(dir);

	return 0;

put_block:
	if (alloc && d_info->ents[ei].d_ext.bno) {
		put_blocks(sb, d_info->ents[ei].d_ext.bno, SEFS_DENT_BLOCKS);
		memset(&d_info->ents[ei].d_ext, 0, sizeof(struct d_info_ent));
	}
iput:
	put_blocks(sb, SEFS_INODE(inode)->ei_block, 1);
	put_inode(sb, inode->i_ino);
	// iput(inode);
end:
	log_error("error %d", ret);
	brelse(bh);
	return ret;
}

static struct inode *sefs_remove_dirent(struct inode *dir, const char *name)
{
	struct super_block *sb;
	struct inode *inode;
	struct buffer_head *bh, *bh2;
	struct sefs_d_info_block *d_info;
	struct sefs_dir_block *dblock;
	baddr_t bno;
	int ei, bi, fi;

	bh = bh2 = NULL;
	inode = NULL;
	sb = dir->i_sb;

	bh = sb_bread(sb, SEFS_INODE(dir)->d_info_block);
	if (!bh)
		return inode;

	d_info = (struct sefs_d_info_block *)bh->b_data;

	for (ei = 0; ei < SEFS_D_INFO_ENT_MAX; ei++) {
		bno = d_info->ents[ei].d_ext.bno;
		if (bno == 0)
			break;

		if (d_info->ents[ei].nr_files == 0)
			continue;

		for (bi = 0; bi < SEFS_DENT_BLOCKS; bi++) {
			bh2 = sb_bread(sb, bno + bi);
			if (!bh2) {
				oxb_error("failed to read");
				goto release_bh;
			}

			dblock = (struct sefs_dir_block *)bh2->b_data;

			if (dblock->nr_files == 0) {
				brelse(bh2);
				continue;
			}

			/* Remove file from parent directory */
			for (fi = 0; fi < (int)SEFS_DENT_PER_BLOCK; fi++) {
				if (!strcmp(dblock->files[fi].name, name)) {
					inode = sefs_iget(
						sb, dblock->files[fi].ino);

					if (!inode) {
						brelse(bh2);
						goto release_bh;
					}

					lock_buffer(bh);
					dblock->files[fi].ino = 0;
					dblock->nr_files--;
					mark_buffer_dirty_inode(bh, dir);
					unlock_buffer(bh);

					lock_buffer(bh2);
					d_info->ents[ei].nr_files--;
					d_info->nr_files--;
					mark_buffer_dirty_inode(bh2, dir);
					unlock_buffer(bh2);
					break;
				}
			}
			brelse(bh2);
		}
	}

release_bh:
	brelse(bh);
	if (inode) {
		sefs_debug("[%s] inode(%d) is %s name(%s)", __func__,
			   inode->i_ino, S_ISDIR(inode->i_mode) ? "DIR" : "REG",
			   name);
	}
	return inode;
}

static int sefs_delete_dir(struct inode *dir)
{
	struct super_block *sb;
	struct sefs_d_info_block *d_info;
	struct buffer_head *bh;
	int bno, ei, bi;

	sefs_debug("[%s] dir(%d)", __func__, dir->i_ino);

	sb = dir->i_sb;
	bh = sb_bread(sb, SEFS_INODE(dir)->d_info_block);
	if (!bh)
		return -1;

	d_info = (struct sefs_d_info_block *)bh->b_data;
	if (d_info->nr_files) {
		oxb_warn("[%s] dir(%d) contains files(%d) but deleted",
			 __func__, dir->i_ino, d_info->nr_files);
	}

	for (ei = 0; ei < (int)SEFS_D_INFO_ENT_MAX; ei++) {
		bno = d_info->ents[ei].d_ext.bno;
		if (bno == 0)
			continue;

		for (bi = 0; bi < SEFS_DENT_BLOCKS; bi++)
			free_buffer_head_inode(bno + bi, dir);

		put_blocks(sb, bno, SEFS_DENT_BLOCKS);
	}

	brelse(bh);
	free_buffer_head_inode(SEFS_INODE(dir)->d_info_block, dir);
	put_blocks(sb, SEFS_INODE(dir)->d_info_block, 1);

	return 0;
}

static int sefs_unlink(struct inode *dir, const char *name)
{
	struct super_block *sb;
	struct inode *inode;
	struct buffer_head *bh, *bh2;
	struct sefs_ei_block *eblock;
	int ei, bi, ret = 0;
	baddr_t ei_block, next_ei;

	bh = bh2 = NULL;
	sb = dir->i_sb;

	inode = sefs_remove_dirent(dir, name);
	if (!inode) {
		oxb_error("[%s] dir(%d) can't find inode(%s)", __func__,
			  dir->i_ino, name);
		return -1;
	}

	// if (S_ISLNK(inode->i_mode))
	// 	goto clean_inode;

	if (S_ISDIR(inode->i_mode)) {
		dir->i_nlink--;
		inode->i_nlink--; // decrement twice 1: for . and ..
		mark_inode_dirty(dir);
	}

	if (inode->i_nlink > 1) {
		inode->i_nlink--; // decrement twice 2: for . and ..
		mark_inode_dirty(inode);
		return ret;
	}

	/* detach directory entries */
	if (S_ISDIR(inode->i_mode)) {
		sefs_delete_dir(inode);
		goto clean_inode;
	}

	/* reclaim file index block and data blocks */
	ei_block = SEFS_INODE(inode)->ei_block;

next:
	bh = sb_bread(sb, ei_block);
	if (!bh)
		goto clean_inode;

	eblock = (struct sefs_ei_block *)bh->b_data;
	next_ei = eblock->next;

	/* reclaim data blocks */
	for (ei = 0; ei < (int)SEFS_MAX_EXTENTS; ei++) {
		if (!eblock->extents[ei].bno)
			break;

		for (bi = 0; bi < eblock->extents[ei].len; bi++)
			free_buffer_head_inode(eblock->extents[ei].bno + bi,
					       inode);

		put_blocks(sb, eblock->extents[ei].bno,
			   eblock->extents[ei].len);
	}

	free_buffer_head_inode(ei_block, inode);
	put_blocks(sb, ei_block, 1);
	if (next_ei != 0) {
		ei_block = next_ei;
		goto next;
	}

clean_inode:
	/* Cleanup inode and mark dirty */
	inode_lock(inode);
	// inode->i_blocks = 0;
	SEFS_INODE(inode)->ei_block = 0;
	// inode->i_size = 0;
	// inode->i_uid = 0;
	// inode->i_gid = 0;
	// inode->i_mode = 0;
	// inode->i_nlink = 0;
	inode_unlock(inode);

	evict_inode(inode);

	// 	inode->i_ctime.tv_sec = inode->i_mtime.tv_sec = inode->i_atime.tv_sec =
	// 		0;
	sefs_debug("[%s] done", __func__);

	return ret;
}

const struct inode_operations sefs_dir_inode_operation = {
	.lookup = sefs_lookup,
	.create = sefs_create,
	.unlink = sefs_unlink,
};

const struct inode_operations sefs_inode_operation = {
	.get_blocks = sefs_get_block,
};
