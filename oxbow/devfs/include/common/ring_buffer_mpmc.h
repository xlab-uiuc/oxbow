/* SPDX-License-Identifier: GPL-2.0 OR MIT */
#ifndef OXBOW_COMMON_RING_BUFFER_MPMC_H
#define OXBOW_COMMON_RING_BUFFER_MPMC_H 1

#include <stdbool.h>
#include <stddef.h>
#include <stdatomic.h>

/*
 * Lock-free bounded MPMC ring buffer using per-slot sequence numbers
 * (Vyukov-style). Capacity must be a power of two.
 */

struct ring_mpmc_slot {
	/* Slot sequence used for producer/consumer coordination */
	_Atomic size_t sequence;
	void *value;
};

struct ring_buffer_mpmc {
	/* Cold/read-mostly fields grouped first */
	struct ring_mpmc_slot
		*slots; /* array of slots (64B-aligned allocation) */
	size_t capacity; /* total slots; power of two */
	size_t mask; /* capacity - 1 */

	/* Hot counters placed on separate cache lines to avoid false sharing */
	_Atomic size_t enqueue_pos
		__attribute__((aligned(64))); /* global enqueue position */
	_Atomic size_t dequeue_pos
		__attribute__((aligned(64))); /* global dequeue position */
};

int ring_buffer_mpmc_create(struct ring_buffer_mpmc *rb, size_t capacity);
void ring_buffer_mpmc_destroy(struct ring_buffer_mpmc *rb);

/* Non-blocking operations. 0 on success, -EAGAIN if full/empty, -EINVAL on bad args. */
int ring_buffer_mpmc_try_enqueue(struct ring_buffer_mpmc *rb, void *elem);
int ring_buffer_mpmc_try_dequeue(struct ring_buffer_mpmc *rb, void **out);

/* Approximate helpers. */
size_t ring_buffer_mpmc_count(struct ring_buffer_mpmc *rb);
size_t ring_buffer_mpmc_space(struct ring_buffer_mpmc *rb);

#endif /* OXBOW_COMMON_RING_BUFFER_MPMC_H */


