#ifndef _SHMEM_H_
#define _SHMEM_H_
#include <stdint.h>
#include <sys/types.h>

// Arbitrary value.
#define READONLY_SHM_KEY_BASE 0x100000
#define WRITABLE_SHM_KEY_BASE 0x10000

int create_shm_seg(key_t key, uint64_t size, int perm);
int get_shm_seg(key_t key, uint64_t size);
void remove_shm_seg(int shmid);
char *attach_shm_seg(int shmid);
void detach_shm_seg(char *shmaddr);
#endif
