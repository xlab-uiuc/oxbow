#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <errno.h>
#include <math.h>
#include "oxbow.h"
#include "oxbow_debug.h"
#include "utils/sync_device.h"
#include "io_dispatcher.h"
#include "common/sjd.h"

// Maximum buffer size (16 pages) limited by BIO_MAX_VECS.
#define MAX_BUFFER_SIZE (16 * 4096)

baddr_t sync_dev_ssb_baddr;
uint64_t sync_dev_ssb_nr_blks;
baddr_t sync_dev_fs_area_start_baddr;
uint64_t sync_dev_fs_area_nr_blks;
static sem_t sync_dev_sem; // It assume all sync device process is single threaded.

/**
 * Callback function for read operations completion
 */
void bio_end_io(void *args)
{
	struct bio *bio = args;

	oxb_debug("IO operation completed for bio at block %lu", bio->bi_start);
	free(bio->bi_io_vec);
	free_bio(bio);

	// Resume the requesting thread.
	sem_post(&sync_dev_sem);
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
int dump_blocks_to_file(baddr_t start_baddr, uint64_t nr_blks, const char *filename)
{
	struct bio *bio = NULL;
	char *buf;
	FILE *fp;
	uint64_t max_blks_per_iter = MAX_BUFFER_SIZE / OXBOW_BLOCK_SIZE;
	uint64_t processed_blks = 0;
	uint64_t blks_to_process;
	baddr_t current_baddr;
	int ret = 0;
	double last_printed_percentage_dump = -10.0;

	sem_init(&sync_dev_sem, 0, 0);

	buf = malloc(MAX_BUFFER_SIZE);
	if (!buf) {
		oxb_error("Failed to allocate memory for buffer (size: %lu MB)", 
			MAX_BUFFER_SIZE / (1024 * 1024));
		return -ENOMEM;
	}

	// Open output file
	fp = fopen(filename, "wb");
	if (!fp) {
		oxb_error("Failed to open file %s for writing: %s", filename, strerror(errno));
		free(buf);
		return -errno;
	}

	// Process blocks in chunks of max_blks_per_iter
	current_baddr = start_baddr;
	while (processed_blks < nr_blks) {
		// Calculate number of blocks to process in this iteration
		blks_to_process = (nr_blks - processed_blks < max_blks_per_iter) ? 
			(nr_blks - processed_blks) : max_blks_per_iter;

		oxbow_assert(blks_to_process <= BIO_MAX_VECS);
		
		size_t buffer_size = blks_to_process * OXBOW_BLOCK_SIZE;

		// Create bio.
		if (!bio) {
			bio = alloc_bio(BIO_MAX_VECS, REQ_OP_READ);
			bio->bi_vcnt = 0;

			if (!bio) {
				oxb_error("Failed to allocate bio");
				free(buf);
				fclose(fp);
				return -ENOMEM;
			}

			bio->bi_start = current_baddr;
		}

		// Add each page as a bvec.
		for (uint64_t i = 0; i < blks_to_process; i++) {
			add_bvec(bio, buf + i * OXBOW_BLOCK_SIZE);
		}
	
		// Register free callback.
		bio->end_io = bio_end_io;
		bio->bi_vcnt = bio->bi_vtotal;

		// Read from device's staging area.
		iod_submit_bio_general(bio, 1);
		bio = NULL;

		// Wait for the bio to be done.
		sem_wait(&sync_dev_sem);

		// Dump block if it is the first block. (staging super block).
		if (processed_blks == 0) {
			struct staging_superblock *ssb = (struct staging_superblock *)buf;
			printf("------- Staging Super Block -------\n");
			printf("magic: 0x%x\n", ssb->magic);
			printf("block_size: %u\n", ssb->blk_size);
			printf("block_nr (total # of blocks): %u\n", ssb->block_nr);
			printf("start: %lu\n", ssb->start);
			printf("end: %lu\n", ssb->end);
			printf("-----------------------------------\n");
		}

		// Write buffer to file
		if (fwrite(buf, 1, buffer_size, fp) != buffer_size) {
			oxb_error("Failed to write to file %s: %s", filename, strerror(errno));
			ret = -EIO;
			
			// Free resources for this iteration
			free(buf);
			
			fclose(fp);
			return ret;
		}

		// Update progress
		processed_blks += blks_to_process;
		current_baddr += blks_to_process;

		// Print progress every 10% or at completion
		double current_percentage = (double)processed_blks * 100.0 / nr_blks;
		if (current_percentage >= last_printed_percentage_dump + 10.0 || processed_blks == nr_blks) {
			printf("Progress: %lu/%lu blocks dumped to %s (%.1f%%)\n",
			       processed_blks, nr_blks, filename,
			       current_percentage);
			if (processed_blks == nr_blks) {
				last_printed_percentage_dump = 100.0;
			} else {
				last_printed_percentage_dump = floor(current_percentage / 10.0) * 10.0;
			}
		}
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
int load_blocks_from_file(baddr_t start_baddr, uint64_t nr_blks, const char *filename)
{
	struct bio *bio = NULL;
	char *buf;
	FILE *fp;
	uint64_t max_blks_per_iter = MAX_BUFFER_SIZE / OXBOW_BLOCK_SIZE;
	uint64_t processed_blks = 0;
	uint64_t blks_to_process;
	baddr_t current_baddr;
	int ret = 0;
	double last_printed_percentage_load = -10.0;

	sem_init(&sync_dev_sem, 0, 0);

	buf = malloc(MAX_BUFFER_SIZE);
	if (!buf) {
		oxb_error("Failed to allocate memory for buffer (size: %lu MB)", 
			MAX_BUFFER_SIZE / (1024 * 1024));
		return -ENOMEM;
	}

	// Open input file
	fp = fopen(filename, "rb");
	if (!fp) {
		oxb_error("Failed to open file %s for reading: %s", filename, strerror(errno));
		free(buf);
		return -errno;
	}

	// Process blocks in chunks of max_blks_per_iter
	current_baddr = start_baddr;
	while (processed_blks < nr_blks) {
		// Calculate number of blocks to process in this iteration
		blks_to_process = (nr_blks - processed_blks < max_blks_per_iter) ? 
			(nr_blks - processed_blks) : max_blks_per_iter;

		oxbow_assert(blks_to_process <= BIO_MAX_VECS);
		
		size_t buffer_size = blks_to_process * OXBOW_BLOCK_SIZE;

		// Read from file into buffer
		size_t bytes_read = fread(buf, 1, buffer_size, fp);
		if (bytes_read != buffer_size) {
			if (feof(fp)) {
				oxb_error("Reached end of file before reading all requested blocks");
			} else {
				oxb_error("Failed to read from file %s: %s", filename, strerror(errno));
			}
			ret = -EIO;
			free(buf);
			fclose(fp);
			return ret;
		}

		// Create bio and add to bio_list for write operation
		if (!bio){
			bio = alloc_bio(BIO_MAX_VECS, REQ_OP_WRITE);
			bio->bi_vcnt = 0;

			if (!bio) {
				oxb_error("Failed to allocate bio");
				free(buf);
				fclose(fp);
				return -ENOMEM;
			}

			bio->bi_start = current_baddr;
		}

		// Add each page as a bvec.
		for (uint64_t i = 0; i < blks_to_process; i++) {
			add_bvec(bio, buf + i * OXBOW_BLOCK_SIZE);
		}

		// Register free callback.
		bio->end_io = bio_end_io;
		bio->bi_vcnt = bio->bi_vtotal;
		
		// Write to device's fs area.
		iod_submit_bio_general(bio, 0);
		bio = NULL;

		// Wait for the bio to be done.
		sem_wait(&sync_dev_sem);

		// Update progress
		processed_blks += blks_to_process;
		current_baddr += blks_to_process;

		// Print progress every 10% or at completion
		double current_percentage = (double)processed_blks * 100.0 / nr_blks;
		if (current_percentage >= last_printed_percentage_load + 10.0 || processed_blks == nr_blks) {
			printf("Progress: %lu/%lu blocks loaded from %s (%.1f%%)\n",
			       processed_blks, nr_blks, filename,
			       current_percentage);
			if (processed_blks == nr_blks) {
				last_printed_percentage_load = 100.0;
			} else {
				last_printed_percentage_load = floor(current_percentage / 10.0) * 10.0;
			}
		}
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
		dump_blocks_to_file(sync_dev_ssb_baddr, sync_dev_ssb_nr_blks, STAGE_AREA_FILE_NAME);
		break;

	case 2:
		// Copy device's file system area to secure daemon's file system area.
		// file system area: from block 0 to block (ssb_baddr -1), total
		// number of blocks == ssb_baddr.
		load_blocks_from_file(sync_dev_fs_area_start_baddr, sync_dev_fs_area_nr_blks, FS_AREA_FILE_NAME);
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
