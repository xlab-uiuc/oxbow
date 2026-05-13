#include <sys/mman.h>
#include "msg_handler.h"
#include "msg_op.h"
#include "fs.h"
#include <unistd.h>

// int handle_open_resp(struct msg_data *msg_body)
// {
// 	return 0;
// }

// int handle_close_resp(struct msg_data *msg_body)
// {
// 	// TODO:
// 	// Ref counter.
// 	// If ref cnt == 0, dettach and remove shm.
// 	// decrease_file_ref_cnt(f);

// 	// I think don't have to handle close_resp
// }
