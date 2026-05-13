#ifndef _MSG_HANDLER_H_
#define _MSG_HANDLER_H_
#include "msg_op.h"

int handle_fsync(struct msg_data *msg_body);
int handle_ino_fsync(struct msg_data *msg_body);
int handle_sync(void);
int handle_add_journal(struct msg_data *msg_body);
int handle_fallocate(struct msg_data *msg_body);
int handle_ftruncate(struct msg_data *msg_body);

#endif
