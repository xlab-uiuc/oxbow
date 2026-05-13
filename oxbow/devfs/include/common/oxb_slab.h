#ifndef _OXB_SLAB_H_
#define _OXB_SLAB_H_

#include <stddef.h>
#include <stdbool.h>
#include <stdatomic.h>
#include <threads.h> // Added for mtx_t

typedef struct oxb_slab_obj {
	struct oxb_slab_obj *next;
} oxb_slab_obj_t;

/**
 * @brief Tagged pointer (for DCAS) to solve the ABA problem.
 * Bundles the object pointer (obj) and version counter (version) 
 * for 128-bit atomic operations.
 */
typedef struct {
	oxb_slab_obj_t *obj;
	size_t version;
} oxb_free_list_head_t;

typedef struct oxb_slab {
	size_t obj_size; /* Size of each object */
	size_t objs_per_slab; /* Number of objects per slab */
	size_t slab_size; /* Size of each slab */

	// 'slabs' array and 'num_slabs' are protected by 'lock'
	void **slabs; /* Array of slab pointers */
	atomic_size_t num_slabs; /* Number of slabs allocated */
	atomic_size_t slabs_capacity; /* Current capacity of slabs array */

	// Lock-free free list (ABA-proof)
	_Atomic(oxb_free_list_head_t) free_list;

	// Mutex for grow, destroy, and validate_ptr operations
	mtx_t lock;

} oxb_slab_t;

/**
 * Initialize a slab allocator
 * @param slab The slab allocator to initialize
 * @param obj_size The size of each object to be allocated
 * @return 0 on success.
 */
int oxb_slab_init(oxb_slab_t *slab, size_t obj_size);

/**
 * Destroy a slab allocator and free all memory
 * @param slab The slab allocator to destroy
 */
void oxb_slab_destroy(oxb_slab_t *slab);

/**
 * Allocate an object from the slab (Lock-Free)
 * @param slab The slab allocator
 * @return Pointer to the allocated object, NULL on failure
 */
void *oxb_slab_alloc(oxb_slab_t *slab);

/**
 * Free an object back to the slab (Lock-Free)
 * @param slab The slab allocator
 * @param ptr Pointer to the object to free
 */
void oxb_slab_free(oxb_slab_t *slab, void *ptr);

#endif
