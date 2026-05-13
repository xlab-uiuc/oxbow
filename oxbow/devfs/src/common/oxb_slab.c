#include "oxb_slab.h"
#include "common/profile.h"
#include "common/global.h"
#include "log.h"
#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>
#include <threads.h>
#include <stdio.h>

PF_TL_EVT(x001_slab_alloc);
PF_TL_EVT(x002_slab_grow);
PF_TL_EVT(x003_slab_waiting_grow);

// #define OXB_SLAB_VALIDATE_PTR

#define INITIAL_SLAB_COUNT 8192 // 8192 slabs = 32MB
#define DEFAULT_ALIGNMENT 8
#define MAX_SLAB_SIZE 4096 // 4KB max slab size

// Minimum number of objects in a slab (optimized for 4KB objects. e.g., struct inode_range_dirty)
#define MIN_OBJECTS_PER_SLAB 1

// Round up to the nearest multiple of alignment
static size_t align_up(size_t size, size_t alignment)
{
	return (size + alignment - 1) & ~(alignment - 1);
}

int oxb_slab_init(oxb_slab_t *slab, size_t obj_size)
{
#ifdef OXB_SLAB_VALIDATE_PTR
	log_warn("OXB_SLAB_VALIDATE_PTR is defined. Disable it for performance.");
#endif

	if (!slab || obj_size == 0) {
		return -1;
	}

	// Ensure object size is at least as large as a pointer for free list
	if (obj_size < sizeof(oxb_slab_obj_t)) {
		obj_size = sizeof(oxb_slab_obj_t);
	}

	// Align object size
	obj_size = align_up(obj_size, DEFAULT_ALIGNMENT);

	// Calculate slab size
	size_t target_objs = MIN_OBJECTS_PER_SLAB;
	size_t slab_size = obj_size * target_objs;
	if (slab_size > MAX_SLAB_SIZE) {
		slab_size = MAX_SLAB_SIZE;
	}
	size_t objs_per_slab = slab_size / obj_size;
	if (objs_per_slab == 0) {
		slab_size = obj_size;
		objs_per_slab = 1;
	}

	// Initialize slab structure
	memset(slab, 0, sizeof(oxb_slab_t)); // Zeroing for safety
	slab->obj_size = obj_size;
	slab->objs_per_slab = objs_per_slab;
	slab->slab_size = slab_size;

	atomic_store(&slab->num_slabs, 0);
	atomic_store(&slab->slabs_capacity, INITIAL_SLAB_COUNT);

	// Initialize free_list with NULL pointer and version 0
	oxb_free_list_head_t initial_head = { .obj = NULL, .version = 0 };
	atomic_store(&slab->free_list, initial_head);

	// Initialize the mutex
	if (mtx_init(&slab->lock, mtx_plain) != thrd_success) {
		return -1;
	}

	// Allocate array of slab pointers
	size_t initial_capacity = atomic_load(&slab->slabs_capacity);
	slab->slabs = malloc(sizeof(void *) * initial_capacity);
	if (!slab->slabs) {
		mtx_destroy(&slab->lock);
		return -1;
	}
	// malloc doesn't zero memory, so zero it for VALIDATE_PTR
	memset(slab->slabs, 0, sizeof(void *) * initial_capacity);

	// Prefill: allocate all initial slabs and push their objects to
	// free_list
	size_t slabs_allocated = 0;
	for (size_t i = 0; i < initial_capacity; i++) {
		void *new_slab = malloc(slab->slab_size);
		if (!new_slab) {
			// Allocation failed; leave the allocator in a usable
			// (partially prefilled) state
			log_error("Failed to allocate slab %lu of %lu.", i,
				  initial_capacity);
			break;
		}

		slab->slabs[i] = new_slab;
		slabs_allocated++;

		// Push every object in this slab onto the lock-free free_list
		char *obj_ptr = (char *)new_slab;
		for (size_t j = 0; j < slab->objs_per_slab; j++) {
			oxb_slab_obj_t *obj = (oxb_slab_obj_t *)obj_ptr;
			oxb_free_list_head_t old_head;
			oxb_free_list_head_t new_head;
			new_head.obj = obj;

			do {
				old_head = atomic_load_explicit(&slab->free_list, memory_order_relaxed);
				obj->next = old_head.obj;
				new_head.version = old_head.version + 1;
			} while (!atomic_compare_exchange_weak_explicit(
				&slab->free_list, &old_head, new_head,
				memory_order_release, memory_order_relaxed));

			obj_ptr += slab->obj_size;
		}
	}

	atomic_store_explicit(&slab->num_slabs, slabs_allocated, memory_order_release);

	if (slabs_allocated < initial_capacity) {
		panic("Failed to allocate and prefill all initial slabs.");
		// return -1;
	}

	return 0;
}

void oxb_slab_destroy(oxb_slab_t *slab)
{
	if (!slab) {
		return;
	}

	// Acquire lock to prevent race with grow or validate_ptr
	mtx_lock(&slab->lock);

	// Free all slabs
	size_t num_slabs = atomic_load(&slab->num_slabs);
	for (size_t i = 0; i < num_slabs; i++) {
		free(slab->slabs[i]);
	}

	void *slabs_ptr = slab->slabs;

	// Release lock, then destroy mutex and free memory
	mtx_unlock(&slab->lock);
	mtx_destroy(&slab->lock);

	free(slabs_ptr);

	// Reset the structure
	memset(slab, 0, sizeof(oxb_slab_t));
}

// (Cold Path) Safely grow the slab array using a lock
static bool oxb_slab_grow(oxb_slab_t *slab)
{
	PF_TL_START(x003_slab_waiting_grow);

	log_warn("[%s] Waiting for the lock. Current nr_slabs: %lu", __func__,
		 atomic_load(&slab->num_slabs));

	// Protect the slabs array from grow, destroy, and validate_ptr
	mtx_lock(&slab->lock);

	PF_TL_END(x003_slab_waiting_grow);

	// Thundering herd prevention: another thread might have grown while we waited for the lock
	oxb_free_list_head_t current_head =
		atomic_load_explicit(&slab->free_list, memory_order_relaxed);
	if (current_head.obj) {
		mtx_unlock(&slab->lock);
		return true;
	}

	PF_TL_START(x002_slab_grow);

    /* no-op: success flag removed */
	size_t current_num_slabs = atomic_load(&slab->num_slabs);
	size_t current_capacity = atomic_load(&slab->slabs_capacity);

	// Check if we need to resize the slabs array
	if (current_num_slabs >= current_capacity) {
		size_t new_capacity = current_capacity * 2;
		void **new_slabs =
			realloc(slab->slabs, sizeof(void *) * new_capacity);
		if (!new_slabs) {
			goto cleanup; // Must release the lock
		}
		slab->slabs = new_slabs;
		atomic_store(&slab->slabs_capacity, new_capacity);

		log_warn("[%s] Doubling the slab capacity from %lu to %lu",
			 __func__, current_capacity, new_capacity);
	}

	// Allocate a new slab
	void *new_slab = malloc(slab->slab_size);
	if (!new_slab) {
		goto cleanup; // Must release the lock
	}

	// *First*, write the new slab to the slabs array
	slab->slabs[current_num_slabs] = new_slab;
	// *Then*, increment num_slabs to allow other threads (validate_ptr) to access it
	atomic_store_explicit(&slab->num_slabs, current_num_slabs + 1,
			      memory_order_release);

	// Release the lock immediately to allow other threads to continue alloc/free
	mtx_unlock(&slab->lock);

	// (Lock-Free) Add objects from the new slab to the lock-free free_list
	char *obj_ptr = (char *)new_slab;
	for (size_t i = 0; i < slab->objs_per_slab; i++) {
		oxb_slab_obj_t *obj = (oxb_slab_obj_t *)obj_ptr;

		oxb_free_list_head_t old_head;
		oxb_free_list_head_t new_head;
		new_head.obj = obj;

		do {
			// relaxed load is fine (CAS is the sync point)
			old_head = atomic_load_explicit(&slab->free_list,
							memory_order_relaxed);
			obj->next = old_head.obj;
			new_head.version =
				old_head.version +
				1; // Increment version (prevents ABA)
		} while (!atomic_compare_exchange_weak_explicit(
			&slab->free_list, &old_head, new_head,
			memory_order_release, memory_order_relaxed));

		obj_ptr += slab->obj_size;
	}

	PF_TL_END(x002_slab_grow);

	return true; // Success

cleanup:
	// Release lock on failure
	mtx_unlock(&slab->lock);
	return false;
}


// [Performance] Object initialization (zeroing) is the caller's responsibility.
void *oxb_slab_alloc(oxb_slab_t *slab)
{
	if (!slab) {
		return NULL;
	}

	PF_TL_START(x001_slab_alloc);

	oxb_free_list_head_t old_head;
	oxb_free_list_head_t new_head;

	// (Hot Path, Lock-Free)
	for (;;) {
		// Read pointer and version together to prevent ABA
		old_head = atomic_load_explicit(&slab->free_list,
						memory_order_acquire);

		// If free list is empty, try to grow the slab (Cold Path)
		if (!old_head.obj) {
			if (!oxb_slab_grow(slab)) {
				panic("Failed to grow slab.");
				// return NULL;
			}
			continue; // Retry
		}

		// Prepare the new head (next object, version + 1)
		new_head.obj = old_head.obj->next;
		new_head.version = old_head.version + 1;

		// 128-bit CAS (ABA-safe)
		if (atomic_compare_exchange_weak_explicit(
			    &slab->free_list, &old_head, new_head,
			    memory_order_acquire, memory_order_relaxed)) {
			break; // Success
		}
		// Loop and retry if CAS fails (another thread won)
	}

	PF_TL_END(x001_slab_alloc);

	return old_head.obj;
}

void oxb_slab_free(oxb_slab_t *slab, void *ptr)
{
	if (!slab || !ptr) {
		return;
	}

#ifdef OXB_SLAB_VALIDATE_PTR
	// (Debug Path) Validation must be synced with grow, so use the lock
	mtx_lock(&slab->lock);
	bool valid_ptr = false;
	size_t num_slabs = atomic_load(&slab->num_slabs);
	for (size_t i = 0; i < num_slabs; i++) {
		char *slab_start = (char *)slab->slabs[i];
		if (!slab_start)
			continue; // Might still be allocating
		char *slab_end = slab_start + slab->slab_size;
		if ((char *)ptr >= slab_start && (char *)ptr < slab_end) {
			if (((char *)ptr - slab_start) % slab->obj_size == 0) {
				valid_ptr = true;
				break;
			}
		}
	}
	mtx_unlock(&slab->lock);

	if (!valid_ptr) {
		// Pointer is not from this slab allocator
		return;
	}
#endif

	// (Hot Path, Lock-Free)
	oxb_slab_obj_t *obj = (oxb_slab_obj_t *)ptr;
	oxb_free_list_head_t old_head;
	oxb_free_list_head_t new_head;
	new_head.obj = obj;

	do {
		// relaxed load is fine (CAS is the sync point)
		old_head = atomic_load_explicit(&slab->free_list,
						memory_order_relaxed);
		obj->next = old_head.obj;
		new_head.version = old_head.version +
				   1; // Increment version (prevents ABA)
	} while (!atomic_compare_exchange_weak_explicit(
		&slab->free_list, &old_head, new_head, memory_order_release,
		memory_order_relaxed));
}
