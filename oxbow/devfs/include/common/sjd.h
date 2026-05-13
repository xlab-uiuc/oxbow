#ifndef _SJD_H_
#define _SJD_H_

#include "kerncompat.h"
#include "global.h"
#include "oxbow.h"
#include <linux/fs.h>
#include <stdio.h>
#include <semaphore.h>

// #define SJD_MAGIC 0x012E0BAF

enum j_block_type {
	SUPER_BLK = 1, // 1: Journal super block.
	DESC_BLK, // 2: Descriptor block.
	COMMIT_BLK, // 3: Commit block.
	ETAG_BLK, // 4: Extent tag block.
	STAGE_TRACE_BLK, // 5: Stage trace block.
	STAGE_DESC_BLK, // 6: Stage descriptor block.
};

/* Common header. */
struct journal_header {
	uint32_t h_magic;
	uint32_t h_blocktype; // enum j_block_type
	uint32_t h_txid; // h_sequence in JBD2. FIXME: Do we need it?
} __attribute__((packed));
struct journal_superblock {
	struct journal_header common;

	/* Static information describing the journal */
	uint32_t block_size; /* journal device blocksize only OXBOW_BLOCK_SIZE is support for now. */
	baddr_t first; /* first block number of log ( > 0) */
	uint64_t maxlen; /* total number of blocks of log (=maxlen in JBD2) */

	/* Dynamic information describing the current state of the log */
	uint32_t tx_id; /* first commit tx ID expected in log (s_sequence in JBD2) */

	baddr_t start; /* Current block number of start of log.
			* Updated with log.tail when flushing to the disk.
			* Set to 0 on shutdown. if we encounter other values
			* on init time, recovery is required.
			*/

	// TODO: We don't need to store head_blk. We can scan the whole journal
	// on recovery to get the head block number.
	// The file system is terminated without an error, all the committed
	// transactions have already been checkpointed. It means head is the
	// first block.
	// baddr_t head_blk; /* blocknr of the next available block (end+1). Updated with log.head when flushing to the disk. */

	/* Error value, as set by jbd2_journal_abort(). */
	uint32_t err_no;

	uint32_t max_transaction; /* Limit of journal blocks per trans.*/
	uint32_t max_trans_data; /* Limit of data blocks per trans. */

	// uint8_t s_checksum_type; /* checksum type */
	// uint8_t s_padding2[3];

	// uint32_t s_checksum; /* crc32c(superblock) */

	//	// TODO: replace it with DevFS's journal block.
	//	/* Static information describing the journal */
	//	u32 s_blocksize; /* journal device blocksize */
	//	u32 s_maxlen; /* total blocks in journal file */
	//	u32 s_first; /* first block of log information */
	//
	//	/* Dynamic information describing the current state of the log */
	//	u32 s_sequence; /* first commit ID expected in log */
	//	u32 s_start; /* blocknr of start of log */
	//
	//	u32 s_max_transaction; /* Limit of journal blocks per trans.*/
	//	u32 s_max_trans_data; /* Limit of data blocks per trans. */
	//	u32 s_num_fc_blks; /* Number of fast commit blocks */

} __attribute__((aligned(OXBOW_BLOCK_SIZE)));

struct staging_superblock {
	u32 magic;
	u32 blk_size;
	u32 block_nr; /* total number of blocks */
	baddr_t start; /* staging starts from this block */
	baddr_t end;
	/* checkpoint starts from this block */ // TOCHECK: Is it used?

} __attribute__((aligned(OXBOW_BLOCK_SIZE)));

/**
 * @note "cnt = 0" means special purpose block
 *  	  i. "start = 0", fsync(stage) tracing block
 */
typedef struct __attribute__((packed)) {
	baddr_t start; /* start lba */
	u32 cnt;
} etag_t; // A tag that represents an extent.

/**
 * Layout for background journal transaction
 *
 * |          | < Data buf > |         < Metadata buf >           |       |        |
 * | Journal  | Data blocks  | Journal (dup)| Tag blocks or, ...  | Super | Commit |
 * | Desc blk |              | Desc blk     | Stage trace blocks  | Block | Block  |
 */

struct journal_descriptor_header {
	u32 blocktype; // enum j_block_type
	u32 transaction_id;
	u32 nr_etag_blks; // Total number of etags blocks in this transaction. Required for checkpointing.
	u32 nr_stage_trace_blks; // Total number of stage trace blocks in this transaction. Required for checkpointing.
	u32 nr_tags; // It is required because it can be less than JOURNAL_TAG_MAX.
	u32 meta_start_baddr; /* starting block address of metadata. (Used by DevFS)
		               * It is stored in the disk for recovery. Otherwise, we
		               * have to get the baddr scanning all the tags.
		               */
} __attribute__((packed));

#define JOURNAL_DESC_TAG_MAX                                                   \
	((PAGE_SIZE - sizeof(struct journal_descriptor_header)) /              \
	 sizeof(etag_t))

struct journal_descriptor_block {
	/* header */
	struct journal_descriptor_header h;
	/* normal tag */
	etag_t tags[JOURNAL_DESC_TAG_MAX];
} __attribute__((aligned(PAGE_SIZE)));

/**
 * @brief metadata block header (etag block, stage trace block)
 */
struct journal_meta_header {
	u32 blocktype; // enum j_block_type: to identify the type of this block.
	u32 nr; // # of tags (etag block) or # of stage Txs (stage trace block)
} __attribute__((packed));

#define JOURNAL_ETAG_MAX                                                       \
	((PAGE_SIZE - sizeof(struct journal_meta_header)) / sizeof(etag_t))

struct journal_extent_tag_block {
	struct journal_meta_header
		h; // nr: number of tags this etag block contains.
	etag_t tags[JOURNAL_ETAG_MAX];
} __attribute__((aligned(PAGE_SIZE)));

#define JOURNAL_TRACE_MAX                                                      \
	((PAGE_SIZE - sizeof(struct journal_meta_header)) / sizeof(baddr_t))
/**
 * @brief this block in transaction be identified by etag(0,0)
 */
struct journal_stage_trace_block {
	struct journal_meta_header
		h; // nr: how many fsync called before == # of stage Txs
	baddr_t tx_list[JOURNAL_TRACE_MAX]; /* Block address of the stage
					     * descriptor block in stage log.
					     */
} __attribute__((aligned(PAGE_SIZE)));

/**
 * @brief fsync journal related block structures.
 * 
 */
#define INODE_MAX_SIZE 256
struct journal_stage_header {
	char inode[INODE_MAX_SIZE]; /* disk inode */
	u32 blocktype;
	u64 ino;
	u32 mrc_tx_id; // most recently committed journal txid
	u32 nr_etag_blks; // Total number of etags blocks in this transaction. Required for checkpointing.
	u32 nr_tags; // Not total tag nr
	u32 next_etag_blk;
	u64 inode_baddr;
	u32 inode_index; // index in a block.
	u32 inode_size;

} __attribute__((packed));

#define STAGE_DESC_BLK_TAG_MAX                                                 \
	((PAGE_SIZE - sizeof(struct journal_stage_header)) / sizeof(etag_t))

struct stage_descriptor_block {
	struct journal_stage_header h;
	etag_t tags[STAGE_DESC_BLK_TAG_MAX];
} __attribute__((aligned(PAGE_SIZE)));

struct stage_commit_block {
	// # of waiting dirty data blocks stored. 0 means waiting dirty list is skipped.
	uint32_t waiting_dirty_data_blks[2];

	// # of waiting dirty IRD blocks stored. 0 means waiting dirty list is skipped.
	uint32_t waiting_dirty_ird_blks[2];
	char pad[PAGE_SIZE - sizeof(uint32_t) * 2 * 2];
} __attribute__((aligned(PAGE_SIZE)));

static inline int is_jnr_desc_blk(char *blk)
{
	struct journal_descriptor_header *h;
	h = (struct journal_descriptor_header *)blk;

	if (h->blocktype == DESC_BLK)
		return true;
	return false;
}

static inline int is_stage_trace_blk(char *blk)
{
	struct journal_meta_header *h;
	h = (struct journal_meta_header *)blk;

	if (h->blocktype == STAGE_TRACE_BLK)
		return true;
	return false;
}

static inline int is_etag_blk(char *blk)
{
	struct journal_meta_header *h;
	h = (struct journal_meta_header *)blk;

	if (h->blocktype == ETAG_BLK)
		return true;
	return false;
}

static inline void print_blk_type(enum j_block_type t)
{
	if (t == SUPER_BLK)
		printf("super_blk");
	else if (t == DESC_BLK)
		printf("desc_blk");
	else if (t == COMMIT_BLK)
		printf("commit_blk");
	else
		printf("Unknown block type: %u", t);
}

static inline void print_journal_sb(struct journal_superblock *sb
				    __attribute__((unused)))
{
	// #ifdef PRINT_DUMP_BLK
	printf("------- Journal super block -------\n");
	printf("common.h_magic: %x\n", sb->common.h_magic);
	printf("common.h_blocktype: ");
	print_blk_type(sb->common.h_blocktype);
	printf("\n");
	printf("common.h_txid: %u\n", sb->common.h_txid);

	printf("bock_size: %u\n", sb->block_size);
	printf("maxlen (total # of blocks): %lu (%lu GB)\n", sb->maxlen,
	       sb->maxlen >> (30 - OXBOW_BLOCK_SIZE_SHIFT));
	printf("first block: %lu\n", sb->first);

	printf("tx_id (seqn): %u\n", sb->tx_id);
	printf("log start block: %lu\n", sb->start);

	printf("err_no: %u\n", sb->err_no);

	printf("max_transaction: %u\n", sb->max_transaction);
	printf("max_trans_data: %u\n", sb->max_trans_data);

	printf("-----------------------------------\n");
	// #endif
}

static inline void print_stage_trace_block(struct journal_stage_trace_block *stb
					   __attribute__((unused)))
{
#ifdef PRINT_DUMP_BLK
	uint32_t i;

	printf("------- Stage Trace Block (type=%u) -------\n",
	       stb->h.blocktype);
	printf("# of stage Txs: %u\n", stb->h.nr);

	for (i = 0; i < stb->h.nr; i++) {
		printf("tx_list[%u]: %lu\n", i, stb->tx_list[i]);
	}

	printf("-------------------------------\n");
#endif
}

static inline void print_stage_descriptor_block(struct stage_descriptor_block *sdb)
{
#ifdef PRINT_DUMP_BLK
	uint32_t i;

	printf("------- Stage Descriptor Block (type=%u) -------\n",
	       sdb->h.blocktype);
	printf("ino: %lu\n", sdb->h.ino);
	printf("mrc_tx_id: %u\n", sdb->h.mrc_tx_id);
	printf("nr_etag_blks: %u\n", sdb->h.nr_etag_blks);
	printf("nr_tags: %u\n", sdb->h.nr_tags);
	printf("next_etag_blk: %u\n", sdb->h.next_etag_blk);
	printf("inode_baddr: %lu\n", sdb->h.inode_baddr);
	printf("inode_index: %u\n", sdb->h.inode_index);
	printf("inode_size: %u\n", sdb->h.inode_size);

	printf("Tags:\n");
	for (i = 0; i < sdb->h.nr_tags; i++) {
		printf("  tag[%u]: start: %lu, cnt: %u\n", i, 
		       sdb->tags[i].start, sdb->tags[i].cnt);
	}

	printf("-------------------------------\n");
#endif
}

#endif
