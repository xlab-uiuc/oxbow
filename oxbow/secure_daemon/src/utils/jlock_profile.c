#include "utils/jlock_profile.h"

#ifdef JLOCK_PROFILE

#include <stdio.h>
#include <time.h>
#include <unistd.h>

#include "oxbow_debug.h"

atomic_uint_fast64_t g_jlock_total;
atomic_uint_fast64_t g_jlock_contended;
atomic_uint_fast64_t g_jlock_wait_cycles;

/* TSC frequency in cycles-per-nanosecond. Set once during init. */
static double g_tsc_cycles_per_ns;

/*
 * Calibrate TSC against CLOCK_MONOTONIC by sampling both at two points
 * separated by ~100 ms. Modern x86 has invariant TSC, so a one-time
 * calibration is sufficient for the lifetime of the daemon.
 */
static void calibrate_tsc(void)
{
	struct timespec t1, t2;
	uint64_t c1, c2;

	clock_gettime(CLOCK_MONOTONIC, &t1);
	c1 = __rdtsc();
	usleep(100000); /* 100 ms */
	c2 = __rdtsc();
	clock_gettime(CLOCK_MONOTONIC, &t2);

	uint64_t ns = (uint64_t)(t2.tv_sec - t1.tv_sec) * 1000000000ULL +
		      (uint64_t)(t2.tv_nsec - t1.tv_nsec);
	uint64_t cycles = c2 - c1;

	if (ns == 0 || cycles == 0) {
		g_tsc_cycles_per_ns = 0.0;
		return;
	}
	g_tsc_cycles_per_ns = (double)cycles / (double)ns;
}

void jlock_profile_init(void)
{
	atomic_store_explicit(&g_jlock_total, 0, memory_order_relaxed);
	atomic_store_explicit(&g_jlock_contended, 0, memory_order_relaxed);
	atomic_store_explicit(&g_jlock_wait_cycles, 0, memory_order_relaxed);

	calibrate_tsc();

	oxb_info("[jlock_profile] initialized (TSC: %.3f cycles/ns); stats "
		 "will be printed on daemon exit",
		 g_tsc_cycles_per_ns);
}

void jlock_profile_print(void)
{
	uint64_t total =
		atomic_load_explicit(&g_jlock_total, memory_order_relaxed);
	uint64_t contended =
		atomic_load_explicit(&g_jlock_contended, memory_order_relaxed);
	uint64_t wait_cycles = atomic_load_explicit(&g_jlock_wait_cycles,
						    memory_order_relaxed);

	double wait_ns_total = 0.0;
	double avg_wait_per_acq_ns = 0.0;
	double avg_wait_per_contended_ns = 0.0;
	double contended_pct = 0.0;

	if (g_tsc_cycles_per_ns > 0.0) {
		wait_ns_total = (double)wait_cycles / g_tsc_cycles_per_ns;
		if (total > 0)
			avg_wait_per_acq_ns = wait_ns_total / (double)total;
		if (contended > 0)
			avg_wait_per_contended_ns =
				wait_ns_total / (double)contended;
	}
	if (total > 0)
		contended_pct = 100.0 * (double)contended / (double)total;

	printf("\n==================== [jlock_profile] ====================\n"
	       "  acquires           : %lu\n"
	       "  contended          : %lu (%.2f%%)\n"
	       "  wait_total         : %.0f us (%.3f s)\n"
	       "  avg wait per acq   : %.3f us\n"
	       "  avg wait per cont. : %.3f us\n"
	       "=========================================================\n\n",
	       (unsigned long)total, (unsigned long)contended, contended_pct,
	       wait_ns_total / 1000.0, wait_ns_total / 1e9,
	       avg_wait_per_acq_ns / 1000.0,
	       avg_wait_per_contended_ns / 1000.0);
	fflush(stdout);
}

#else /* !JLOCK_PROFILE */

void jlock_profile_init(void)
{
}

void jlock_profile_print(void)
{
}

#endif /* JLOCK_PROFILE */
