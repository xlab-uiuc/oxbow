#include "fs.h"
#include "oxbow_debug.h"
#include "time_stat.h"
#include "profile_libfs.h"
#include <string.h>

PF_TL_EVT(c_evt_read);
PF_TL_EVT(ca_memcpy);
PF_TL_EVT(ca_mmap);

/**
 * @brief common read functions, calculate size and pos.
 * 
 * @return ssize_t (how much bytes be read)
 */
ssize_t libfs_read(struct lfs_file *f, char *buf, size_t count, loff_t *pos)
{
	// struct lfs_inode *inode;
	// atomic_char *page_bits;
	size_t size, pg_idx, page_start, ofs_in_page;
	size_t starg_pg_idx = *pos / PAGE_SIZE;
	size_t end_pg_idx = (*pos + count - 1) / PAGE_SIZE;
	size_t bytes_to_copy, bytes_remain, read_bytes;
	size_t cur_ofs = *pos;
	size_t buf_ofs = 0;

	PF_TL_START(c_evt_read);

	// inode = f->f_inode;

	PF_TL_START(ca_mmap);
	// shared_inode_rdlock(inode); Is it required?
	size = get_oxbow_isize(f);
	if (size < (size_t)*pos) {
		log_error("pos is over size: size %lu, pos %lu", size, *pos);
		return -1;
	}
	read_bytes = *pos + count > size ? size - *pos : count;
	bytes_remain = read_bytes;

	if (f->data == NULL) {
		if (file_do_mmap(f) < 0) {
			log_error("failed to do mmap");
			return -1;
		}
	}
	PF_TL_END(ca_mmap);

	// log_debug("[%s] at(0x%lx) read(%lu) pos(%lu) f_size(%lu)", __func__,
	// 	  (char *)f->data + *pos, bytes_remain, *pos, size);

	for (pg_idx = starg_pg_idx; pg_idx < end_pg_idx + 1; pg_idx++) {
		page_start = pg_idx * PAGE_SIZE;
		ofs_in_page = cur_ofs - page_start;
		bytes_to_copy = PAGE_SIZE - ofs_in_page;
		if (bytes_to_copy > bytes_remain)
			bytes_to_copy = bytes_remain;

		// page_bits = lock_page_bit(inode, pg_idx);
		PF_TL_START(ca_memcpy);
		if (memcpy(buf + buf_ofs, (char *)f->data + cur_ofs,
			   bytes_to_copy) == NULL) {
			log_error("[%s] memcpy failed", __func__);
			// unlock_page_bit(page_bits, pg_idx);
			// shared_inode_unlock(inode);
			return -1; // goto err
		}
		PF_TL_END(ca_memcpy);

		// unlock_page_bit(page_bits, pg_idx);

		bytes_remain -= bytes_to_copy;
		cur_ofs += bytes_to_copy;
		buf_ofs += bytes_to_copy;
	}

	*pos += read_bytes;

	PF_TL_END(c_evt_read);

	return read_bytes;
}
int prefault = 0;
/**
 * @brief common write functions. fine-grained locking for managing file.
 * 
 * @return ssize_t (how many bytes are written)
 */
ssize_t libfs_write(struct lfs_file *f, const char *buf, size_t count,
		   loff_t *pos)
{
	const off_t starg_pg_idx = *pos / PAGE_SIZE;
	const off_t end_pg_idx = (*pos + count - 1) / PAGE_SIZE;
	struct lfs_inode *inode = f->f_inode;
	atomic_char *page_lock = NULL;
	size_t f_size, page_start;
	unsigned int bytes_to_copy, bytes_remain = count;
	off_t f_last_page, ofs_in_page, cur_ofs = *pos;
	off_t pi, buf_ofs = 0;
	int journalled;
	bool size_updated = false;

	rw_debug("[%s] file(%s) fd:%d pos:%d, count=%lu | (tid %d)", __func__,
		 f->f_inode->path, f->fd, f->pos, count, get_tid());

	PF_TL_START(a_evt_write);

	PF_TL_START(aa_before_sh_inode_lock_wait);
	if (!inode->shm_header)
		init_shared_file_metadata(f);

	for (pi = starg_pg_idx; pi < end_pg_idx + 1; pi++) {
		page_start = pi * PAGE_SIZE;
		ofs_in_page = cur_ofs - page_start;
		bytes_to_copy = PAGE_SIZE - ofs_in_page;
		if (bytes_to_copy > bytes_remain)
			bytes_to_copy = bytes_remain;

		shm_alloc_check(inode->shm_header, pi);

		bytes_remain -= bytes_to_copy;
		cur_ofs += bytes_to_copy;
		buf_ofs += bytes_to_copy;
	}

	if (f->data == NULL) {
		if (file_do_mmap(f) < 0) {
			log_error("failed to do mmap");
			return -1;
		}
	}
	PF_TL_END(aa_before_sh_inode_lock_wait);

	PF_TL_START(aa_sh_inode_lock_wait);
	shared_inode_lock(inode);
	PF_TL_END(aa_sh_inode_lock_wait);

	shared_data_dirty(inode);

	PF_TL_START(ab_journal_start);
	if (atomic_load(&inode->shm_header->i_state) & SHM_USE_JOURNAL)
		journalled = libfs_journal_start(inode);
	PF_TL_END(ab_journal_start);

	f_size = get_oxbow_isize(f);
	if (*pos + count > f_size) {
		size_updated = true;
		// log_info("pos(%lu) count(%lu) size(%d)", *pos, count, f_size);
	}
	f_last_page = f_size ? (f_size - 1) / PAGE_SIZE : 0;

	// What if IO size is larger than 32MB...?
	if (count > PEB_COVERAGE) {
		off_t temp = 0;
		while (starg_pg_idx + temp < end_pg_idx) {
			dirty_page_group(inode, starg_pg_idx + temp);
			temp += PEB_COVERAGE / PAGE_SIZE;
		}
	} else {
		dirty_page_group(inode, starg_pg_idx);
		if (end_pg_idx != starg_pg_idx)
			dirty_page_group(inode, end_pg_idx);
	}

	bytes_remain = count;
	cur_ofs = *pos;
	buf_ofs = 0;

	for (pi = starg_pg_idx; pi < end_pg_idx + 1; pi++) {
		page_start = pi * PAGE_SIZE;
		ofs_in_page = cur_ofs - page_start;
		bytes_to_copy = PAGE_SIZE - ofs_in_page;
		if (bytes_to_copy > bytes_remain)
			bytes_to_copy = bytes_remain;

		PF_TL_START(ac_page_lock_bit);
		if (lock_page_bit(inode, pi, &page_lock)) {
			log_error("[%s] lock failed", __func__);
			shared_inode_unlock(inode);
			return -1; // goto err
		}
		PF_TL_END(ac_page_lock_bit);

		// does kernel give zeroed pages?
		if (pi > f_last_page || ((f_last_page == 0) && (f_size == 0))) {
			rw_debug("Set SKIPREAD bit. (kernel gives zeroed pages)");
			/* zero-behind-EOF: no disk read required */
			set_skipread_bit(inode, pi);
		} else if (bytes_to_copy == PAGE_SIZE) {
			rw_debug(
				"Set SKIPREAD bit. (full-page write) offset_in_page: %ld bytes_to_copy: %u",
				ofs_in_page, bytes_to_copy);

			/* Under page lock: set skip_read=1 for full-page, 0 for partial */
			set_skipread_bit(inode, pi);
		} else
			clear_skipread_bit(inode, pi);

		// prefault = *(int *)((char *)f->data + cur_ofs);

		PF_TL_START(ad_write_memcpy);
		if (memcpy((char *)f->data + cur_ofs, buf + buf_ofs,
			   bytes_to_copy) == NULL) {
			log_error("[%s] memcpy failed", __func__);
			unlock_page_bit(page_lock, pi);
			shared_inode_unlock(inode);
			return -1; // goto err
		}

		PF_TRACK_TPUT(libfs_write_memcpy,
			      bytes_to_copy >> OXBOW_BLOCK_SIZE_SHIFT);

		PF_TL_END(ad_write_memcpy);

		// After data is written to the page, the page is marked as
		// uptodate.
		// TOCHECK: Still unclear why this bit is necessary. To prevent
		// concurrent read I/O? Additionally, it is not cleared in the
		// current implementation.
		uptodate_page_bit(inode, pi);

		dirty_page_bit(inode, pi);
		unlock_page_bit(page_lock, pi);

		bytes_remain -= bytes_to_copy;
		cur_ofs += bytes_to_copy;
		buf_ofs += bytes_to_copy;
	}

	if (size_updated)
		set_oxbow_isize(f, *pos + count);

	if (journalled) {
		PF_TL_START(ae_journal_handle_end);
		libfs_journal_end(inode);
		PF_TL_END(ae_journal_handle_end);
	}

	shared_inode_unlock(inode);

	*pos += count;

	rw_debug("[%s] write(%d) size?%d", __func__, count, size_updated);

	PF_TL_END(a_evt_write);

	return count;
}
