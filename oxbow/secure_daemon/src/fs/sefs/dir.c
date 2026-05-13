#include "fs.h"
#include "fs/sefs/sefs.h"
#include "buffer_head.h"

static int sefs_iterate(struct inode *dir, unsigned long pos, char **names,
			int *name_len, unsigned long **inos)
{
	struct super_block *sb;
	struct buffer_head *bh, *bh2;
	struct sefs_d_info_block *eblock;
	struct sefs_dir_block *dblock;
	struct sefs_dirent *f;
	char *name;
	baddr_t bno;
	uint32_t i, ei, bi, fi;
	int nr;

	/* Check that dir is a directory */
	if (!S_ISDIR(dir->i_mode))
		return -ENOTDIR;

	/*
     * Check that ctx->pos is not bigger than what we can handle (including
     * . and ..)
     */
	if (pos > SEFS_DENT_MAX + 2) {
		oxb_info("pos over max sefs dent");
		return 0;
	}

	sb = dir->i_sb;

	/* Read the directory index block on disk */
	bh = sb_bread(sb, SEFS_INODE(dir)->d_info_block);
	if (!bh)
		return -EIO;

	eblock = (struct sefs_d_info_block *)bh->b_data;
	nr = eblock->nr_files;
	if (nr == 0)
		return nr;

	*inos = malloc(nr * sizeof(unsigned long));
	if (!*inos) {
		oxb_error("malloc fail");
		return -ENOMEM;
	}

	ei = (pos - 2) / SEFS_DENT_PER_EXT;
	bi = (pos - 2) % SEFS_DENT_PER_EXT / SEFS_DENT_PER_BLOCK;
	fi = (pos - 2) % SEFS_DENT_PER_BLOCK;

	/* Iterate over the index block and commit subfiles */
	for (; ei < SEFS_MAX_EXTENTS; ei++) {
		bno = eblock->ents[ei].d_ext.bno;
		if (bno == 0)
			break;

		/* Iterate over blocks in one extent */
		for (; bi < SEFS_DENT_BLOCKS; bi++) {
			bh2 = sb_bread(sb, bno + bi);
			if (!bh2) {
				oxb_error("fail bread");
				goto err_ifree;
			}
			dblock = (struct sefs_dir_block *)bh2->b_data;
			if (dblock->files[0].ino == 0)
				break;

			/* Iterate every file in one block */
			for (; fi < SEFS_DENT_PER_BLOCK; fi++) {
				f = &dblock->files[fi];
				if (f->ino)
					*name_len += strlen(f->name) + 1;
			}
			brelse(bh2);
		}
	}

	*names = malloc(*name_len);
	if (!*names) {
		oxb_error("malloc fail");
		goto err_ifree;
	}
	memset(*names, 0, *name_len);
	name = *names;

	i = 0;
	ei = (pos - 2) / SEFS_DENT_PER_EXT;
	/* Iterate over the index block and commit subfiles */
	for (; ei < SEFS_MAX_EXTENTS; ei++) {
		bno = eblock->ents[ei].d_ext.bno;
		if (bno == 0) {
			sefs_debug("[%s] ei(%d)", __func__, ei);
			break;
		}

		bi = (pos - 2) % SEFS_DENT_PER_EXT / SEFS_DENT_PER_BLOCK;
		/* Iterate over blocks in one extent */
		for (; bi < SEFS_DENT_BLOCKS; bi++) {
			bh2 = sb_bread(sb, bno + bi);
			if (!bh2) {
				oxb_error("fail bread");
				goto err_nfree;
			}
			dblock = (struct sefs_dir_block *)bh2->b_data;
			if (dblock->files[0].ino == 0) {
				sefs_debug("[%s] ei(%d) bi(%d)", __func__, ei,
					   bi);
				break;
			}

			fi = (pos - 2) % SEFS_DENT_PER_BLOCK;
			/* Iterate every file in one block */
			for (; fi < SEFS_DENT_PER_BLOCK; fi++) {
				f = &dblock->files[fi];
				if (f->ino) {
					sefs_debug("[%s] found(%s) ino(%d)",
						   __func__, f->name, f->ino);
					(*inos)[i] = f->ino;
					i++;
					strcpy(name, f->name);
					name += strlen(f->name) + 1;
				}
			}
			brelse(bh2);
		}
	}

	return nr;

err_nfree:
	free(*names);
err_ifree:
	free(*inos);
	brelse(bh);
	return -EIO;
}

const struct file_operations sefs_dir_ops = {
	.iterate_shared = sefs_iterate,
};
