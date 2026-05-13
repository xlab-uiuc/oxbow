#include "test_global.h"
#include "oxbow_libfs.h"
#include "common/linux/oxbow_kernel.h"
#include "common/global.h"

#include <stdio.h>
#include <unistd.h>
#include <string.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>

int file_basic_operation_test(void);
int open_latency_test(int times);
int sq_read_test(int io_size);
int sq_write_test(int io_size);
int fsync_lat_test(int io_size);
int mkdir_test(void);

#define TEST_FILE_SIZE (1000UL * 1024UL * 1024UL) /* 1GB */
