#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <malloc.h>

#include <stdatomic.h>

#include "common/global.h"
#include "common/ring_buffer_mpmc.h"

static inline bool is_power_of_two_size(size_t v)
{
	return v > 1 && (v & (v - 1)) == 0;
}

int ring_buffer_mpmc_create(struct ring_buffer_mpmc *rb, size_t capacity)
{
	size_t i;

	if (!rb || !is_power_of_two_size(capacity))
		return -EINVAL;

	rb->capacity = capacity;
	rb->mask = capacity - 1;
	atomic_store(&rb->enqueue_pos, 0);
	atomic_store(&rb->dequeue_pos, 0);

	/* 64-byte aligned slots allocation to start on a cache line boundary */
	void *mem = NULL;
	if (posix_memalign(&mem, 64, capacity * sizeof(*rb->slots)) != 0)
		return -ENOMEM;
	memset(mem, 0, capacity * sizeof(*rb->slots));
	rb->slots = (struct ring_mpmc_slot *)mem;

	for (i = 0; i < capacity; ++i) {
		atomic_store(&rb->slots[i].sequence, i);
		rb->slots[i].value = 0;
	}
	return 0;
}

void ring_buffer_mpmc_destroy(struct ring_buffer_mpmc *rb)
{
	if (!rb)
		return;
	free(rb->slots);
	rb->slots = 0;
}

int ring_buffer_mpmc_try_enqueue(struct ring_buffer_mpmc *rb, void *elem)
{
	size_t pos;
	struct ring_mpmc_slot *slot;
	size_t seq;
	intptr_t diff;

	if (!rb)
		return -EINVAL;

	for (;;) {
		pos = atomic_load_explicit(&rb->enqueue_pos,
					   memory_order_relaxed);
		slot = &rb->slots[pos & rb->mask];
		seq = atomic_load_explicit(&slot->sequence,
					   memory_order_acquire);
		diff = (intptr_t)seq - (intptr_t)pos;
		if (diff == 0) {
			if (atomic_compare_exchange_weak_explicit(
				    &rb->enqueue_pos, &pos, pos + 1,
				    memory_order_relaxed,
				    memory_order_relaxed)) {
				break;
			}
		} else if (diff < 0) {
			return -EAGAIN; /* full */
		} else {
			/* Another producer advanced; retry */
			oxbow_cpu_relax();
		}
	}

	slot->value = elem;
	/* Publish slot with sequence = pos + 1 */
	atomic_store_explicit(&slot->sequence, pos + 1, memory_order_release);
	return 0;
}

int ring_buffer_mpmc_try_dequeue(struct ring_buffer_mpmc *rb, void **out)
{
	size_t pos;
	struct ring_mpmc_slot *slot;
	size_t seq;
	intptr_t diff;
	void *val;

	if (!rb || !out)
		return -EINVAL;

	for (;;) {
		pos = atomic_load_explicit(&rb->dequeue_pos,
					   memory_order_relaxed);
		slot = &rb->slots[pos & rb->mask];
		seq = atomic_load_explicit(&slot->sequence,
					   memory_order_acquire);
		diff = (intptr_t)seq - (intptr_t)(pos + 1);
		if (diff == 0) {
			if (atomic_compare_exchange_weak_explicit(
				    &rb->dequeue_pos, &pos, pos + 1,
				    memory_order_relaxed,
				    memory_order_relaxed)) {
				break;
			}
		} else if (diff < 0) {
			return -EAGAIN; /* empty */
		} else {
			/* Another consumer advanced; retry */
			oxbow_cpu_relax();
		}
	}

	val = slot->value;
	/* Mark slot available for next round: sequence = pos + rb->capacity */
	atomic_store_explicit(&slot->sequence, pos + rb->capacity,
			      memory_order_release);
	*out = val;
	return 0;
}

size_t ring_buffer_mpmc_count(struct ring_buffer_mpmc *rb)
{
	size_t enq =
		atomic_load_explicit(&rb->enqueue_pos, memory_order_relaxed);
	size_t deq =
		atomic_load_explicit(&rb->dequeue_pos, memory_order_relaxed);
	return enq - deq;
}

size_t ring_buffer_mpmc_space(struct ring_buffer_mpmc *rb)
{
	return rb->capacity - 1 - ring_buffer_mpmc_count(rb);
}
