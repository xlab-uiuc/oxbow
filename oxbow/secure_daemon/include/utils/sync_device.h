#ifndef __DUMP_TO_FILE_H__
#define __DUMP_TO_FILE_H__
#include "oxbow.h"

void sync_device_init(void);
void set_ssb_baddr(baddr_t baddr);
void set_ssb_nr_blks(uint64_t nr_blks);

extern baddr_t sync_dev_ssb_baddr; // staging super block address.
extern uint64_t sync_dev_ssb_nr_blks; // staging super block size.
extern baddr_t sync_dev_fs_area_start_baddr; // file system area start address.
extern uint64_t sync_dev_fs_area_nr_blks; // file system area size including super block.

#endif
