#include "shmem.h"
#include "oxbow_debug.h"

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/shm.h>
#include <sys/types.h>

/**
 * @brief Create a shm seg.
 * 
 * @param key shm_key
 * @param size in byte.
 * @return int 
 */
int create_shm_seg(key_t key, uint64_t size, int perm)
{
	int shmid, err_num;

	// Create shared memory segment
	shmid = shmget(key, size, IPC_CREAT | perm);
	if (shmid < 0) {
		err_num = errno;
		if (err_num == EINVAL) {
			oxb_error(
				"There exists a segment with the same key and different size."
				" key=0x%lx size=%lu Please remove that segment using a command:"
				" ipcrm -m <shmid>",
				key, size);
		} else {
			oxb_error(
				"Creating shared memory segment (shmget) failed. Errno=%s",
				strerror(errno));
		}
		return -1;
	}

	oxb_debug("shmget: shmid=%d", shmid);
	return shmid;
}

/**
 * @brief Get the shm seg that has already been created.
 * 
 * @param key shm_key.
 * @param size 
 * @return int shm_id.
 */
int get_shm_seg(key_t key, uint64_t size)
{
	int shmid, err_num;

	shmid = shmget(key, size, 0666);
	if (shmid < 0) {
		err_num = errno;
		if (err_num == EINVAL) {
			oxb_error(
				"There exists a segment with the same key and different size."
				" key=0x%lx size=%lu Please check the segment using a command: `ipcs`"
				" and remove that segment using a command: `ipcrm -m <shmid>`",
				key, size);
		} else {
			oxb_error(
				"Getting shared memory segment (shmget) failed. Errno=%s",
				strerror(errno));
		}
		return -1;
	}

	oxb_debug("shmget: shmid=%d", shmid);
	return shmid;
}

void remove_shm_seg(int shmid)
{
	int ret;

	// Remove shared memory segment
	ret = shmctl(shmid, IPC_RMID, NULL);
	if (ret == -1)
		oxb_error("Removing shared memory segment (shmctl) failed.");
}

char *attach_shm_seg(int shmid)
{
	char *shmaddr;

	// Attach shared memory segment
	shmaddr = shmat(shmid, NULL, 0);
	if (shmaddr == (char *)-1) {
		oxb_error(
			"Attaching shared memory segment (shmat) failed. errno=%d",
			errno);
		return NULL;
	}
	return shmaddr;
}

void detach_shm_seg(char *shmaddr)
{
	int ret;

	// Detach shared memory segment
	ret = shmdt(shmaddr);
	if (ret == -1)
		oxb_error(
			"Detaching shared memory segment (shmdt) failed. errno=%d",
			errno);
}
