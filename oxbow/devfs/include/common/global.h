#ifndef _GLOBAL_H_
#define _GLOBAL_H_

#include <stdlib.h>
#include <stdio.h>
#include <sched.h>
#include <time.h>
#include <sys/prctl.h>
#include <linux/prctl.h>
#include "oxbow_debug.h"
// Global things not related to Oxbow file system.

/* Global variables */
// #define CHECK_PREFIX 1
#define CHECK_FD 1
#define g_fd_start 1000000
#define ENABLE_ASSERT 1

#if (defined(__i386__) || defined(__x86_64__))
#define GDB_TRAP __asm__("int $3;");
#elif (defined(__aarch64__))
#define GDB_TRAP __asm__(".inst 0xd4200000"); // FIXME Not working correctly?
// #define GDB_TRAP __builtin_trap();
#else
#error "Not supported architecture."
#endif

void _panic(void);
#define panic(str)                                                             \
	do {                                                                   \
		fprintf(stdout, "%s:%d %s(): %s\n", __FILE__, __LINE__,        \
			__func__, str);                                        \
		fflush(stdout);                                                \
		GDB_TRAP;                                                      \
		_panic();                                                      \
	} while (0)

#define stringize(s) #s
#define XSTR(s) stringize(s)

// TODO: Disable assertion in release mode.
#ifdef ENABLE_ASSERT
#if defined __STDC_VERSION__ && __STDC_VERSION__ >= 199901L
#define oxbow_assert(a)                                                        \
	do {                                                                   \
		if (0 == (a)) {                                                \
			fprintf(stderr,                                        \
				"Assertion failed: %s, "                       \
				"%s(), %d at \'%s\'\n",                        \
				__FILE__, __func__, __LINE__, XSTR(a));        \
			GDB_TRAP;                                              \
			abort();                                               \
		}                                                              \
	} while (0)
#else
#define oxbow_assert(a)                                                        \
	do {                                                                   \
		if (0 == (a)) {                                                \
			fprintf(stderr,                                        \
				"Assertion failed: %s, "                       \
				"%d at \'%s\'\n",                              \
				__FILE__, __LINE__, XSTR(a));                  \
			GDB_TRAP;                                              \
			abort();                                               \
		}                                                              \
	} while (0)
#endif
#else

#define oxbow_assert(a)                                                        \
	do {                                                                   \
	} while (0)
#endif

#if (defined(__i386__) || defined(__x86_64__))
#include <immintrin.h>
#endif

/*
 * Busy-wait backoff helpers
 *
 * Use these in tight polling loops to reduce contention and improve fairness.
 *
 * Tuning guide:
 * - OXBOW_SPIN_PAUSE_ITERS: stay in-CPU with pause/yield hints for a few µs
 * - OXBOW_SPIN_YIELD_ITERS: after this, yield the timeslice once
 * - OXBOW_SPIN_SLEEP_NS: short sleep to avoid run-queue thrash for longer waits
 */
#ifndef OXBOW_SPIN_PAUSE_ITERS
#define OXBOW_SPIN_PAUSE_ITERS 1024U
#endif

#ifndef OXBOW_SPIN_YIELD_ITERS
#define OXBOW_SPIN_YIELD_ITERS 8192U
#endif

#ifndef OXBOW_SPIN_SLEEP_NS
#define OXBOW_SPIN_SLEEP_NS 50000L /* 50 microseconds */
#endif

static inline void oxbow_cpu_relax(void)
{
#if (defined(__i386__) || defined(__x86_64__))
	_mm_pause();
#elif (defined(__aarch64__))
	__asm__ __volatile__("yield" ::: "memory");
#else
	/* Fallback: compiler barrier */
	__asm__ __volatile__("" ::: "memory");
#endif
}

/*
 * Adaptive backoff based on spin count.
 * Call this once per unsuccessful iteration in a polling loop.
 */
static inline void oxbow_spin_backoff(unsigned int spins)
{
	if (spins < OXBOW_SPIN_PAUSE_ITERS) {
		/* Very short wait: stay hot, reduce SMT contention */
		oxbow_cpu_relax();
		return;
	}

	if (spins < OXBOW_SPIN_YIELD_ITERS) {
		/* Give scheduler a chance to run the producer */
		sched_yield();
		return;
	}

	/* Longer-than-expected wait: brief sleep to reduce run-queue churn */
	struct timespec ts = { .tv_sec = 0, .tv_nsec = OXBOW_SPIN_SLEEP_NS };
	(void)nanosleep(&ts, NULL);
}

#define BUG_ON(condition, message)                                             \
	do {                                                                   \
		if (condition) {                                               \
			log_error(message);                                    \
			abort();                                               \
		}                                                              \
	} while (0)

#if 0
#define BUG_ON(...)                                                            \
	do {                                                                   \
	} while (0)
#endif

// Use this macro to surpress unused warnings.
#define UNUSED1(z) (void)(z)
#define UNUSED2(y, z) UNUSED1(y), UNUSED1(z)
#define UNUSED3(x, y, z) UNUSED1(x), UNUSED2(y, z)
#define UNUSED4(b, x, y, z) UNUSED2(b, x), UNUSED2(y, z)
#define UNUSED5(a, b, x, y, z) UNUSED2(a, b), UNUSED3(x, y, z)

#define VA_NUM_ARGS_IMPL(_1, _2, _3, _4, _5, N, ...) N
#define VA_NUM_ARGS(...) VA_NUM_ARGS_IMPL(__VA_ARGS__, 5, 4, 3, 2, 1)

#define ALL_UNUSED_IMPL_(nargs) UNUSED##nargs
#define ALL_UNUSED_IMPL(nargs) ALL_UNUSED_IMPL_(nargs)
#define ALL_UNUSED(...) ALL_UNUSED_IMPL(VA_NUM_ARGS(__VA_ARGS__))(__VA_ARGS__)

#ifndef PAGE_SIZE
#define PAGE_SIZE 4096
#define PAGE_SHIFT 12
#define PAGE_ALIGN(addr)                                                       \
	(((uintptr_t)(addr) + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1))
#define PAGE_INDEX(pos) ((pos) / PAGE_SIZE)

#endif

static inline void hex_dump(const char *data, size_t size)
{
	printf("Hex Dump (%zu bytes):\n", size);
	printf("Offset    0  1  2  3  4  5  6  7  8  9  A  B  C  D  E  F    0123456789ABCDEF\n");
	printf("--------  -----------------------------------------------    ----------------\n");

	for (size_t i = 0; i < size; i += 16) {
		// Print offset
		printf("%08zx  ", i);

		// Print hex values
		for (size_t j = 0; j < 16; j++) {
			if (i + j < size)
				printf("%02x ", (unsigned char)data[i + j]);
			else
				printf("   ");
		}

		// Print ASCII representation
		printf("   ");
		for (size_t j = 0; j < 16; j++) {
			if (i + j < size) {
				unsigned char c = (unsigned char)data[i + j];
				// Print printable characters, replace others with dots
				printf("%c", (c >= 32 && c <= 126) ? c : '.');
			} else {
				printf(" ");
			}
		}
		printf("\n");
	}
}

static inline void get_thread_name(char *thread_name)
{
#if defined(__linux__)
	int ret = prctl(PR_GET_NAME, thread_name);
	if (ret != 0) {
		oxb_error("prctl PR_GET_NAME failed");
	}
#else
	oxb_error("get_thread_name(): prctl is not supported on this system.");
#endif
}

#endif
