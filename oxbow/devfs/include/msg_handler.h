#ifndef _MSG_HANDLER_H_
#define _MSG_HANDLER_H_
#include <stddef.h>
#include <stdint.h>

int handle_fetch_data(int df_id, int req_id, int buf_id, size_t size);
int handle_journal(int df_id, int md_buf_id, size_t md_size,
		   size_t total_data_size, int n_fetch_reqs, uint32_t *tx_id);
int handle_stg_ckpt(uint32_t stage_total_blks, uint32_t nr_blks_to_free,
		    uint8_t is_sync, uint32_t *nr_blks_freed);

#endif
