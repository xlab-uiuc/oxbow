#ifndef _MSG_H_
#define _MSG_H_
#include "fs.h"
#include "msg_op.h"
#include "rpc.h"

int init_msg(void);
void msg_send_sd_init(void);
void msg_send_sd_exit(void);
int msg_send_sd_fsync(void *);
// int msg_send_sd_getshm(struct lfs_file *, int *ret);
int msg_send_sd_ino_fsync(unsigned long ino);
int msg_send_sd_add_journal(void *);
int msg_send_sd_sync(void);
int msg_send_sd_fstat(void *, void *stat_buf);
int msg_send_sd_ino_fstat(unsigned long ino, void *__stat_buf);
int msg_send_sd_fallocate(void *);
int msg_send_sd_ftruncate(void *);

#endif
