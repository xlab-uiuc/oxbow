#include "fs.h"
#include "buffer_head.h"
#include "kernfs.h"
#include "fs/journal.h"
#include "oxbow_debug.h"
#include "config.h"
#include "utils/sync_device.h"

#include <pthread.h>
#include <sys/mman.h>
#include <string.h>

struct super_block *g_super_block = NULL; /* for exiting */

int init_filesystem(const char *fsname)
{
	struct super_block *sb;

	sb = alloc_super_block();
	if (!sb)
		goto err;

	if (init_block_device())
		goto err;

	g_bdev->sb = sb;
	sb->bdev = g_bdev;

	if (init_inode()) /* should do before indexing operation */
		goto free;

	if (sb->journal && !g_sd_conf.bg_journaling) {
		oxb_error(
			"Config mismatch: bg_journaling: superblock=%d secure_daemon_config=%d",
			sb->journal, g_sd_conf.bg_journaling);
		goto free;
	}

#ifndef VM_ENV_NO_DEVFS
	// It should be called before background journaling thread is created. (fill_super)
	if (g_sd_conf.bg_journaling) {
		if (init_data_fetcher()) {
			log_error("Failed to initialize Data Fetcher.");
			goto free;
		}
	}
#endif

	if (!init_fs) {
		log_error("no matching fill_super");
		goto exit_df;
	}

	if (init_fs(sb)) {
		log_error("Init file system failed.");
		goto exit_df;
	}

#ifdef USE_NVME_STORAGE_ENGINE
	sync_dev_ssb_baddr = sb->s_op->get_stage_sb_baddr(sb->s_fs_info);
	sync_dev_ssb_nr_blks =
		sb->s_op->get_nr_stage_log_blocks(sb->s_fs_info) +
		1; // +1 for superblock.
	sync_dev_fs_area_start_baddr =
		sb->s_op->get_fs_area_start_baddr(sb->s_fs_info);
	sync_dev_fs_area_nr_blks =
		sb->s_op->get_nr_fs_area_blocks(sb->s_fs_info);
#endif

	g_super_block = sb;
	log_info("init_filesystem done, filesystem => %s", fsname);
	return 0;

exit_df:
#ifndef VM_ENV_NO_DEVFS
	if (g_sd_conf.bg_journaling)
		exit_data_fetcher();
#endif
free:
	free(sb);
err:
	return -1;
}

int exit_filesystem(void)
{
	struct super_block *sb = NULL;
	int ret;

	log_info("[%s]", __func__);

	ret = -1;
	sb = g_super_block;
	if (!sb) {
		log_error("No global superblock");
		goto ret;
	}

	if (sb)
		sync_fs();

	// File system specific exit function.
	if (exit_fs)
		exit_fs();

	if (bh_allfree(sb))
		goto ret;

	free(sb->bdev);

	inode_all_free(sb);

	// It is initialized in init_manager. However, we unmap it here, after
	// finishing sync_fs().
	if (sb->auth_mmap && sb->auth_mmap_len) {
		if (munmap(sb->auth_mmap, sb->auth_mmap_len) < 0)
			perror("exit_filesystem auth_mmap munmap fail");
		sb->auth_mmap = NULL;
		sb->auth_mmap_len = 0;
	}

#ifndef VM_ENV_NO_DEVFS
	if (g_sd_conf.bg_journaling)
		exit_data_fetcher();
#endif

ret:
	return ret;
}
