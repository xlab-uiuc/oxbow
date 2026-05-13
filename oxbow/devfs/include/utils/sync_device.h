#ifndef __SYNC_DEVICE_H__
#define __SYNC_DEVICE_H__
#include "oxbow.h"

void sync_device_init(void);

extern baddr_t sync_dev_ssb_baddr; // staging super block address.
extern uint64_t sync_dev_ssb_nr_blks; // staging super block size.
extern baddr_t sync_dev_fs_area_start_baddr; // file system area start address.
extern uint64_t sync_dev_fs_area_nr_blks; // file system area size.
#endif
