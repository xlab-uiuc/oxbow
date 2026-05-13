#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <errno.h>
#include "oxbow.h"
#include "oxbow_debug.h"
#include "bio.h"
#include "storage_engine.h" // For se_read function
#include "utils/sync_device.h"

// Maximum buffer size (512MB)
#define MAX_BUFFER_SIZE (512 * 1024 * 1024)

baddr_t sync_dev_ssb_baddr;
uint64_t sync_dev_ssb_nr_blks;
baddr_t sync_dev_fs_area_start_baddr;
uint64_t sync_dev_fs_area_nr_blks;

/**
 * @brief Set the N-th bvec of a contiguous bio.
 * 
 * @param bio 
 * @param buf 
 * @param n_blks 
 */
static void set_bvec(struct bio *bio, int bvec_id, char *buf, uint32_t n_blks)
{
	oxbow_assert(bvec_id < bio->bi_vcnt);
	oxbow_assert(buf != NULL);

	// ckpt_info("bio=0x%lx bvec_id=%d buf=0x%lx n_blks=%u", (uintptr_t)bio,
	// 	  bvec_id, (uintptr_t)buf, n_blks);

	bio->bi_io_vec[bvec_id].bv_buf = buf;
	bio->bi_io_vec[bvec_id].bv_len = nblks_to_bytes(n_blks);
	bio->total_size += bio->bi_io_vec[bvec_id].bv_len;
}

/**
 * Helper function to add bio to bio_list
 */
static void add_bio_to_bl(struct bio_list *bl, char *buf, baddr_t start,
			  uint32_t n_blks)
{
	struct bio *bio;
	size_t size_copied;
	size_t size_to_copy;
	size_t size_total;
	uint32_t n_blks_to_copy;
	baddr_t cur_start;
	char *buf_p;

	size_total = nblks_to_bytes(n_blks);
	buf_p = buf;
	cur_start = start;

	for (size_copied = 0; size_copied < size_total;
	     size_copied += BIO_MAX_BIO_SIZE) {
		bio = alloc_bio_n_bvecs(cur_start, 1);
		bio_list_add(bl, bio);

		size_to_copy = min(size_total - size_copied, BIO_MAX_BIO_SIZE);
		n_blks_to_copy = bytes_to_nblks(size_to_copy);

		set_bvec(bio, 0, buf_p, n_blks_to_copy);

		cur_start += n_blks_to_copy;
		buf_p += size_to_copy;
	}
}

/**
 * Reads blocks from the device and saves them to a file.
 * Limits buffer size to 512MB and processes blocks in multiple iterations if needed.
 * 
 * @param start_baddr Starting block address to read from
 * @param nr_blks Number of blocks to read
 * @param filename Name of the file to write the blocks to
 * 
 * @return 0 on success, negative value on error
 */
int dump_blocks_to_file(baddr_t start_baddr, uint64_t nr_blks,
			const char *filename)
{
	struct bio_list *bl;
	char *buf;
	FILE *fp;
	uint64_t max_blks_per_iter = MAX_BUFFER_SIZE / OXBOW_BLOCK_SIZE;
	uint64_t processed_blks = 0;
	uint64_t blks_to_process;
	baddr_t current_baddr;
	int ret = 0;

	buf = malloc(MAX_BUFFER_SIZE);
	if (!buf) {
		oxb_error("Failed to allocate memory for buffer (size: %lu MB)",
			  MAX_BUFFER_SIZE / (1024 * 1024));
		return -ENOMEM;
	}

	// Open output file
	fp = fopen(filename, "wb");
	if (!fp) {
		oxb_error("Failed to open file %s for writing: %s", filename,
			  strerror(errno));
		free(buf);
		return -errno;
	}

	// Process blocks in chunks of max_blks_per_iter
	current_baddr = start_baddr;
	while (processed_blks < nr_blks) {
		// Calculate number of blocks to process in this iteration
		blks_to_process =
			(nr_blks - processed_blks < max_blks_per_iter) ?
				(nr_blks - processed_blks) :
				max_blks_per_iter;

		size_t buffer_size = blks_to_process * OXBOW_BLOCK_SIZE;

		// Create and initialize bio_list
		bl = alloc_bl();
		if (!bl) {
			oxb_error("Failed to allocate bio_list");
			free(buf);
			fclose(fp);
			return -ENOMEM;
		}

		// Add bio to bio_list
		add_bio_to_bl(bl, buf, current_baddr, blks_to_process);

		// Perform read operation
		while (se_request_dispatch_io_sync(bl, 1) == -1) {
			oxb_warn("Dispatch read I/O failed.");
			usleep(1);
		}

		// Write buffer to file
		if (fwrite(buf, 1, buffer_size, fp) != buffer_size) {
			oxb_error("Failed to write to file %s: %s", filename,
				  strerror(errno));
			ret = -EIO;

			// Free resources for this iteration
			free_bl(bl);
			free(buf);
			fclose(fp);
			return ret;
		}

		// Free resources for this iteration
		free_bl(bl);

		// Update progress
		processed_blks += blks_to_process;
		current_baddr += blks_to_process;

		printf("Progress: %lu/%lu blocks dumped to %s (%.2f%%)\n",
		       processed_blks, nr_blks, filename,
		       (double)processed_blks * 100.0 / nr_blks);
	}

	printf("Successfully dumped all %lu blocks (start-last baddr: %lu-%lu) to %s\n",
	       nr_blks, start_baddr, start_baddr + nr_blks - 1, filename);

	fclose(fp);
	free(buf);
	return 0;
}

/**
 * Reads blocks from a file and writes them to the device.
 * Limits buffer size to 512MB and processes blocks in multiple iterations if needed.
 * 
 * @param start_baddr Starting block address to write to
 * @param nr_blks Number of blocks to write
 * @param filename Name of the file to read the blocks from
 * 
 * @return 0 on success, negative value on error
 */
int load_blocks_from_file(baddr_t start_baddr, uint64_t nr_blks,
			  const char *filename)
{
	struct bio_list *bl;
	char *buf;
	FILE *fp;
	uint64_t max_blks_per_iter = MAX_BUFFER_SIZE / OXBOW_BLOCK_SIZE;
	uint64_t processed_blks = 0;
	uint64_t blks_to_process;
	baddr_t current_baddr;
	int ret = 0;

	buf = malloc(MAX_BUFFER_SIZE);
	if (!buf) {
		oxb_error("Failed to allocate memory for buffer (size: %lu MB)",
			  MAX_BUFFER_SIZE / (1024 * 1024));
		return -ENOMEM;
	}

	// Open input file
	fp = fopen(filename, "rb");
	if (!fp) {
		oxb_error("Failed to open file %s for reading: %s", filename,
			  strerror(errno));
		free(buf);
		return -errno;
	}

	// Process blocks in chunks of max_blks_per_iter
	current_baddr = start_baddr;
	while (processed_blks < nr_blks) {
		// Calculate number of blocks to process in this iteration
		blks_to_process =
			(nr_blks - processed_blks < max_blks_per_iter) ?
				(nr_blks - processed_blks) :
				max_blks_per_iter;

		size_t buffer_size = blks_to_process * OXBOW_BLOCK_SIZE;

		// Read from file into buffer
		size_t bytes_read = fread(buf, 1, buffer_size, fp);
		if (bytes_read != buffer_size) {
			if (feof(fp)) {
				oxb_error(
					"Reached end of file before reading all requested blocks");
			} else {
				oxb_error("Failed to read from file %s: %s",
					  filename, strerror(errno));
			}
			ret = -EIO;
			free(buf);
			fclose(fp);
			return ret;
		}

		// Create and initialize bio_list
		bl = alloc_bl();
		if (!bl) {
			oxb_error("Failed to allocate bio_list");
			free(buf);
			fclose(fp);
			return -ENOMEM;
		}

		// Add bio to bio_list
		add_bio_to_bl(bl, buf, current_baddr, blks_to_process);

		// Perform write operation
		while (se_request_dispatch_io_sync(bl, 0) == -1) {
			oxb_warn("Dispatch write I/O failed.");
			usleep(1);
		}

		// Free resources for this iteration
		free_bl(bl);

		// Update progress
		processed_blks += blks_to_process;
		current_baddr += blks_to_process;

		printf("Progress: %lu/%lu blocks loaded from %s (%.1f%%)\n",
		       processed_blks, nr_blks, filename,
		       (double)processed_blks * 100.0 / nr_blks);
	}

	printf("Successfully loaded all %lu blocks (start-last baddr: %lu-%lu) from %s\n",
	       nr_blks, start_baddr, start_baddr + nr_blks - 1, filename);

	fclose(fp);
	free(buf);
	return 0;
}

// Communicate with user via signals (+ value.)
// Signal handler.
void sync_device_sig_handler(int signo, siginfo_t *sip, void *ptr)
{
	UNUSED2(signo, ptr);

	switch (sip->si_value.sival_int) {
	case 1:
		// Copy secure daemon's staging area to device's staging area.
		load_blocks_from_file(sync_dev_ssb_baddr, sync_dev_ssb_nr_blks,
				      STAGE_AREA_FILE_NAME);
		break;

	case 2:
		// Copy device's file system area to secure daemon's file system area.
		// file system area: from block 0 to block (ssb_baddr -1), total
		// number of blocks == ssb_baddr.
		dump_blocks_to_file(sync_dev_fs_area_start_baddr,
				    sync_dev_fs_area_nr_blks,
				    FS_AREA_FILE_NAME);
		break;

	default:
		oxb_error("Unknown value:%d", sip->si_value.sival_int);
		break;
	}
}

int sync_device_setup_sig_handler(int signo)
{
	struct sigaction sigact = { 0 };
	int rc;

	sigemptyset(&sigact.sa_mask);
	sigact.sa_sigaction = sync_device_sig_handler;
	sigact.sa_flags =
		SA_SIGINFO; // An int value will be delivered with the signal.
	rc = sigaction(signo, &sigact, NULL);
	if (rc < 0) {
		oxb_error("sigaction (signo: %d) failed, errno %d (%s)", signo,
			  errno, strerror(errno));
		return -1;
	}

	return 0;
}

void sync_device_init(void)
{
	sync_device_setup_sig_handler(SIGRTMIN + 2);
}
