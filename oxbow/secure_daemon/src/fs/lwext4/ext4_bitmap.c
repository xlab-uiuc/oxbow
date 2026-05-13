/*
 * Copyright (c) 2013 Grzegorz Kostka (kostka.grzegorz@gmail.com)
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * - Redistributions of source code must retain the above copyright
 *   notice, this list of conditions and the following disclaimer.
 * - Redistributions in binary form must reproduce the above copyright
 *   notice, this list of conditions and the following disclaimer in the
 *   documentation and/or other materials provided with the distribution.
 * - The name of the author may not be used to endorse or promote products
 *   derived from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
 * OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
 * IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
 * NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
 * THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

/** @addtogroup lwext4
 * @{
 */
/**
 * @file  ext4_bitmap.c
 * @brief Block/inode bitmap allocator.
 */

#include <ext4_config.h>
#include <ext4_types.h>
#include <ext4_misc.h>
#include <ext4_errno.h>
#include <ext4_debug.h>

#include <ext4_bitmap.h>

void ext4_bmap_bits_free(uint8_t *bmap, uint32_t sbit, uint32_t bcnt)
{
	uint32_t i = sbit;

	while (i & 7) {

		if (!bcnt)
			return;

		ext4_bmap_bit_clr(bmap, i);

		bcnt--;
		i++;
	}
	sbit = i;
	bmap += (sbit >> 3);

#if CONFIG_UNALIGNED_ACCESS
	while (bcnt >= 32) {
		*(uint32_t *)bmap = 0;
		bmap += 4;
		bcnt -= 32;
		sbit += 32;
	}

	while (bcnt >= 16) {
		*(uint16_t *)bmap = 0;
		bmap += 2;
		bcnt -= 16;
		sbit += 16;
	}
#endif

	while (bcnt >= 8) {
		*bmap = 0;
		bmap += 1;
		bcnt -= 8;
		sbit += 8;
	}

	for (i = 0; i < bcnt; ++i) {
		ext4_bmap_bit_clr(bmap, i);
	}
}

int ext4_bmap_bit_find_clr(uint8_t *bmap, uint32_t sbit, uint32_t ebit,
			   uint32_t *bit_id)
{
	uint32_t i;
	uint32_t bcnt = ebit - sbit;

	i = sbit;

	while (i & 7) {

		if (!bcnt)
			return ENOSPC;

		if (ext4_bmap_is_bit_clr(bmap, i)) {
			*bit_id = sbit;
			return EOK;
		}

		i++;
		bcnt--;
	}

	sbit = i;
	bmap += (sbit >> 3);

#if CONFIG_UNALIGNED_ACCESS
	while (bcnt >= 32) {
		if (*(uint32_t *)bmap != 0xFFFFFFFF)
			goto finish_it;

		bmap += 4;
		bcnt -= 32;
		sbit += 32;
	}

	while (bcnt >= 16) {
		if (*(uint16_t *)bmap != 0xFFFF)
			goto finish_it;

		bmap += 2;
		bcnt -= 16;
		sbit += 16;
	}
finish_it:
#endif
	while (bcnt >= 8) {
		if (*bmap != 0xFF) {
			for (i = 0; i < 8; ++i) {
				if (ext4_bmap_is_bit_clr(bmap, i)) {
					*bit_id = sbit + i;
					return EOK;
				}
			}
		}

		bmap += 1;
		bcnt -= 8;
		sbit += 8;
	}

	for (i = 0; i < bcnt; ++i) {
		if (ext4_bmap_is_bit_clr(bmap, i)) {
			*bit_id = sbit + i;
			return EOK;
		}
	}

	return ENOSPC;
}

int ext4_bmap_bits_find_clr(uint8_t *bmap, uint32_t start_bit, uint32_t end_bit,
			    uint32_t n, uint32_t *bit_id)
{
	uint32_t consecutive_count = 0;
	uint32_t start_pos = 0;

	for (uint32_t i = start_bit; i < end_bit; i++) {
		if (!(bmap[i / 8] & (1 << (i % 8)))) {
			if (consecutive_count == 0) {
				start_pos = i;
			}
			consecutive_count++;
			if (consecutive_count == n) {
				*bit_id = start_pos;
				return EOK;
			}
		} else {
			consecutive_count = 0;
		}
	}

	return ENOSPC; // No consecutive cleared bits of length n found
}

void ext4_bmap_bits_set(uint8_t *bmap, uint32_t start_bit, uint32_t n)
{
#ifdef EXT4_BITMAP_BOUNDS_CHECK
	if (n > 0 && ((start_bit + n - 1) >> 3) >= EXT4_BMAP_MAX_BYTE) {
		fprintf(stderr,
			"\n*** BITMAP OOB: bits_set start=%u n=%u "
			"(last byte %u) bmap=%p ***\n",
			start_bit, n, (start_bit + n - 1) >> 3,
			(void *)bmap);
	}
#endif
	for (uint32_t i = 0; i < n; i++) {
		bmap[(start_bit + i) / 8] |= (1 << ((start_bit + i) % 8));
	}
}

bool ext4_bmap_is_bits_clr(uint8_t *bmap, uint32_t start_bit, uint32_t n)
{
#ifdef EXT4_BITMAP_BOUNDS_CHECK
	if (n > 0 && ((start_bit + n - 1) >> 3) >= EXT4_BMAP_MAX_BYTE) {
		fprintf(stderr,
			"\n*** BITMAP OOB: is_bits_clr start=%u n=%u "
			"(last byte %u) bmap=%p ***\n",
			start_bit, n, (start_bit + n - 1) >> 3,
			(void *)bmap);
	}
#endif
	for (uint32_t i = start_bit; i < start_bit + n; i++) {
		if (bmap[i / 8] & (1 << (i % 8))) {
			return 0; // Found a set bit, return false
		}
	}
	return 1; // All bits in the range are cleared
}

/**
 * @}
 */
