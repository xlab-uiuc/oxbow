#include "shm.h"
#include <sys/mman.h>
#include <string.h>

inline void print_page_bits(char n)
{
	printf("This 1byte contains: 0b");
	for (int i = 7; i >= 0; i--) { // 8bit
		printf("%d", (n >> i) & 1);
	}
	printf("\n");
}

void shm_page_set_bit(uint8_t *page, int bit_pos)
{
	if (bit_pos < 0 || bit_pos >= (int)BITS_IN_PAGE) {
		oxb_error("wrong pos %d\n", bit_pos);
		return;
	}

	int byte_idx = bit_pos / BITS_IN_BYTE;
	int bit_idx = bit_pos % BITS_IN_BYTE;

	page[byte_idx] |= (1 << bit_idx);
}

void shm_page_unset_bit(uint8_t *page, int bit_pos)
{
	if (bit_pos < 0 || bit_pos >= (int)BITS_IN_PAGE) {
		oxb_error("wrong pos %d\n", bit_pos);
		return;
	}

	int byte_idx = bit_pos / BITS_IN_BYTE;
	int bit_idx = bit_pos % BITS_IN_BYTE;

	page[byte_idx] &= ~(1 << bit_idx);
}

/* WARN! Not use this function on PEB */
int shm_page_check_bit(uint8_t *page, int bit_pos)
{
	if (bit_pos < 0 || bit_pos >= (int)BITS_IN_PAGE) {
		oxb_error("wrong pos %d\n", bit_pos);
		return 0;
	}

	int byte_idx = bit_pos / BITS_IN_BYTE;
	int bit_idx = bit_pos % BITS_IN_BYTE;

	return page[byte_idx] & (1 << bit_idx);
}

atomic_char *get_page_bits(char *shm_start, size_t pg_idx, int bit_type)
{
	char *ptr;
	loff_t dl_bmap_idx, byte_ofs;

	ptr = shm_start;

	/* First, find a dirty-locking bitmap block */
	dl_bmap_idx = pg_idx * PAGE_SIZE / PEB_COVERAGE;
	ptr += ((PDB_DIRTY_GROUP_NR + PDB_ALLOC_CHECK_NR + 1 + dl_bmap_idx) *
		PAGE_SIZE);

	/* Calculate the byte offset in one drity-locking bitmap block */
	byte_ofs = (pg_idx % NR_PAGES_IN_PEB) / BITS_IN_BYTE;
	if (bit_type == DIRTYBIT)
		byte_ofs += 0;
	else if (bit_type == LOCKBIT)
		byte_ofs += LOCKBIT_START_INDEX;
	else if (bit_type == UPTODATEBIT)
		byte_ofs += UPTODATEBIT_START_INDEX;
	else if (bit_type == SKIPREAD_BIT)
		byte_ofs += SKIPREAD_START_INDEX;
	else
		log_error("bit type %d not supported", bit_type);

	/* We get the byte that contains our pages */
	ptr += byte_ofs;
	return (atomic_char *)ptr;
}

void unlock_page_bit(atomic_char *bits, size_t pg_idx)
{
	char mask = ~(1 << (pg_idx % BITS_IN_BYTE));
	atomic_fetch_and(bits, mask);
	// print_page_bits(*bits);
}

/* if not exist in shm, allocating shm and zeroing */
int shm_alloc_check(struct shm_shared_state *header, size_t pg_idx)
{
	char *shm_ptr;
	loff_t alc_bmap_pos;

	shm_ptr = (char *)header;
	shm_ptr += PAGE_SIZE;

	// First check whether the page is already allocated
	alc_bmap_pos = pg_idx * PAGE_SIZE / PEB_COVERAGE;
	if (shm_page_check_bit((uint8_t *)shm_ptr, alc_bmap_pos))
		return 0;

	// If not, allocate the page and initiating the states
	shm_page_set_bit((uint8_t *)shm_ptr, alc_bmap_pos);

	// log_debug("pages %ld~%ld dirty/locking allocated", pg_idx,
	// 	  pg_idx + NR_PAGES_IN_PEB);
	shm_ptr += (alc_bmap_pos + 2) * PAGE_SIZE;
	memset(shm_ptr, 0, PAGE_SIZE);
	if (mlock(shm_ptr, PAGE_SIZE))
		return -1;

	return 0;
}
