#ifndef _IO_DISPATCHER_H_
#define _IO_DISPATCHER_H_

#include "io/sd_bio.h"
#include "buffer_head.h"

extern struct thpool_ *iod_workers;
extern int g_nvme_qpair_buf_max_blk_nr;
extern struct ring_buffer_mpmc g_rd_bio_ring_buf;

int init_io_dispatcher(void);
void exit_io_dispatcher(void);
// int iod_add_io_req_to_bl(struct bio_list *bl, char *buf, baddr_t blk_no,
// 			 size_t len);
// void submit_mb_maps(struct mb_map *mb_mapping, uint64_t blk_cnt);
// void iod_sort_mb_maps(struct mb_map *mb_mapping, uint64_t mb_nr);

void submit_bh(blk_opf_t, struct buffer_head *);
void submit_bio(blk_opf_t, struct bio *);
void iod_submit_bio(blk_opf_t, struct bio *);
void iod_submit_bio_general(struct bio *bio, bool is_read);
void iod_enqueue_bio(struct bio *bio);

struct stage_aio *alloc_stage_aio(void);

#endif
