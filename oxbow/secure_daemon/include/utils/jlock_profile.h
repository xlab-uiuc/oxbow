#ifndef _JLOCK_PROFILE_H_
#define _JLOCK_PROFILE_H_
/*
 * Lightweight contention measurement for `journal_control_ctx::lock`
 * (a.k.a. j->lock, see §3.4 in
 * plans/2026-04-20-scalability-bottleneck-analysis.md).
 *
 * Toggle at compile time via JLOCK_PROFILE in
 * include/profile_secure_daemon.h.
 *
 * When JLOCK_PROFILE is defined, `jlock_acquire()`:
 *   - bumps `g_jlock_total` on every call (relaxed atomic).
 *   - tries `pthread_spin_trylock` first; if it fails, bumps
 *     `g_jlock_contended` and falls back to `pthread_spin_lock`.
 *   - bracket the whole acquire path with `__rdtsc()` and accumulate
 *     the observed cycles into `g_jlock_wait_cycles`.
 *   `jlock_profile_init()` calibrates the TSC and zeros the counters
 *   at daemon start; `jlock_profile_print()` (called from
 *   `exit_secure_daemon()`) dumps cumulative stats to stdout.
 *
 * When JLOCK_PROFILE is NOT defined, `jlock_acquire()` inlines to a
 * plain `pthread_spin_lock()` (zero overhead) and the init/print
 * functions are no-op stubs.
 *
 * Cost on the uncontended path with the macro on: 2 atomic adds + 1
 * trylock + 2 rdtsc (~30–40 ns total). For a fsync that is multiple
 * ms long this is negligible. We keep `pthread_spin_unlock` plain in
 * either build — only the acquire side is instrumented because
 * lock-wait time is what indicates contention.
 *
 * Hold time is intentionally not measured here; that would require
 * matching unlock instrumentation and TLS state. If we ever need it,
 * extend with a `jlock_release()` and TLS-stashed acquire timestamp.
 */

#include <pthread.h>

#include "profile_secure_daemon.h"

void jlock_profile_init(void);
void jlock_profile_print(void);

#ifdef JLOCK_PROFILE

#include <stdatomic.h>
#include <stdint.h>
#include <x86intrin.h>

extern atomic_uint_fast64_t g_jlock_total;
extern atomic_uint_fast64_t g_jlock_contended;
extern atomic_uint_fast64_t g_jlock_wait_cycles;

static inline void jlock_acquire(pthread_spinlock_t *lock)
{
	uint64_t start = __rdtsc();
	int rc = pthread_spin_trylock(lock);
	if (rc != 0)
		pthread_spin_lock(lock);
	uint64_t got = __rdtsc();

	atomic_fetch_add_explicit(&g_jlock_total, 1, memory_order_relaxed);
	if (rc != 0)
		atomic_fetch_add_explicit(&g_jlock_contended, 1,
					  memory_order_relaxed);
	atomic_fetch_add_explicit(&g_jlock_wait_cycles, got - start,
				  memory_order_relaxed);
}

#else /* !JLOCK_PROFILE */

static inline void jlock_acquire(pthread_spinlock_t *lock)
{
	pthread_spin_lock(lock);
}

#endif /* JLOCK_PROFILE */

#endif /* _JLOCK_PROFILE_H_ */
