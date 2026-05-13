#ifndef _PROFILE_H_
#define _PROFILE_H_
#include "list.h"
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdint.h>
#include <time.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <string.h>

// Enable profile. Comment out to disable.
// #define OXBOW_PROFILE
// If defined, the same contents are dumped to files in profile directory.
// #define PF_PRINT_TO_FILE

// If defined, real-time throughput is tracked.
// #define OXBOW_TRACK_TPUT

#ifndef get_tid
#define get_tid() syscall(__NR_gettid)
#endif

/** Profile event */
struct pf_evt {
	char *evt_name;
	uint32_t id; // each thread has different id for the same event name. Not used in global events.
	unsigned long count;
	double time_sum;
	struct timespec start_time;
	struct timespec end_time;
	int used; // for validation.
	int is_counter;
	struct timespec last_output_time; // For throttling output
	unsigned long last_output_count; // Count at last output
	pthread_spinlock_t lock; // Used for global events.
};

struct pf_thread_evt_lists {
	pthread_t tid; // Thread ID this list belongs to
	struct pf_evt *evts; // Array of events for this thread
	atomic_uint_fast16_t evt_id; // Number of events for this thread
	struct list_head list; // For linking in global list
};

// Global list head for thread-specific event lists
extern struct list_head g_pf_threads;
extern pthread_spinlock_t g_pf_threads_lock;

// Thread-local pointer to this thread's event lists
extern __thread struct pf_thread_evt_lists *g_curr_thread_evt_lists;

extern atomic_uint_fast16_t g_pf_evt_list_id;

static inline double get_duration(struct timespec *time_start,
				  struct timespec *time_end)
{
	double start_sec = (double)(time_start->tv_sec * 1000000000.0 +
				    (double)time_start->tv_nsec) /
			   1000000000.0;
	double end_sec = (double)(time_end->tv_sec * 1000000000.0 +
				  (double)time_end->tv_nsec) /
			 1000000000.0;
	return end_sec - start_sec;
}

static inline uint64_t get_duration_in_ns(struct timespec *time_start,
					  struct timespec *time_end)
{
	uint64_t start_sec =
		time_start->tv_sec * 1000000000 + (double)time_start->tv_nsec;
	uint64_t end_sec =
		time_end->tv_sec * 1000000000 + (double)time_end->tv_nsec;
	return end_sec - start_sec;
}

static inline double get_time(struct timespec *time)
{
	return (double)(time->tv_sec * 1000000000.0 + (double)time->tv_nsec) /
	       1000000000.0;
}

struct realtime_bw_stat {
	uint64_t bytes_until_now;
	struct timespec start_time;
	struct timespec end_time;
	pthread_spinlock_t lock;
	char *name;
};
typedef struct realtime_bw_stat rt_bw_stat;

/* Real-time queue depth statistics. Useful for spotting where requests
 * are piling up in a pipeline. Each check_rt_q() call samples the current
 * depth; once per second it prints average and max depth seen so far.
 */
struct realtime_q_stat {
	uint64_t depth_sum;
	uint64_t samples;
	uint32_t depth_max;
	uint64_t last_tag;
	struct timespec start_time;
	struct timespec end_time;
	pthread_spinlock_t lock;
	char *name;
};
typedef struct realtime_q_stat rt_q_stat;

static inline void init_rt_bw_stat(rt_bw_stat *stat, char *name)
{
	if (!stat) {
		printf("[Warn] Real time bandwidth stat is not allocated.\n");
		return;
	}
	stat->name = name;
	pthread_spin_init(&stat->lock, PTHREAD_PROCESS_PRIVATE);
}

static inline void check_rt_bw(rt_bw_stat *stat, uint64_t sent_bytes)
{
	if (!stat) {
		printf("[Warn] Real time bandwidth stat is not allocated.\n");
		return;
	}

	pthread_spin_lock(&stat->lock);

	if (stat->start_time.tv_sec == 0) { // First run.
		clock_gettime(CLOCK_MONOTONIC, &stat->start_time);
	}

	clock_gettime(CLOCK_MONOTONIC, &stat->end_time);

	if (get_duration(&stat->start_time, &stat->end_time) > 1.0) {
		printf("[RT_BW %s]: %lu MB/s\n", stat->name,
		       stat->bytes_until_now >> 20);

		clock_gettime(CLOCK_MONOTONIC, &stat->start_time);
		stat->bytes_until_now = 0;
	}

	stat->bytes_until_now += sent_bytes;

	pthread_spin_unlock(&stat->lock);
}

static inline void init_rt_q_stat(rt_q_stat *stat, char *name)
{
	if (!stat) {
		printf("[Warn] Real time queue stat is not allocated.\n");
		return;
	}
	stat->name = name;
	stat->depth_sum = 0;
	stat->samples = 0;
	stat->depth_max = 0;
	stat->last_tag = 0;
	stat->start_time.tv_sec = 0;
	stat->start_time.tv_nsec = 0;
	stat->end_time.tv_sec = 0;
	stat->end_time.tv_nsec = 0;
	pthread_spin_init(&stat->lock, PTHREAD_PROCESS_PRIVATE);
}

static inline void check_rt_q(rt_q_stat *stat, uint32_t depth, uint64_t tag)
{
	double dur;

	if (!stat) {
		printf("[Warn] Real time queue stat is not allocated.\n");
		return;
	}

	pthread_spin_lock(&stat->lock);

	if (stat->start_time.tv_sec == 0) { /* First run. */
		clock_gettime(CLOCK_MONOTONIC, &stat->start_time);
	}

	clock_gettime(CLOCK_MONOTONIC, &stat->end_time);

	stat->depth_sum += depth;
	stat->samples++;
	if (depth > stat->depth_max)
		stat->depth_max = depth;
	stat->last_tag = tag;

	dur = get_duration(&stat->start_time, &stat->end_time);
	if (dur > 1.0 && stat->samples > 0) {
		double avg = (double)stat->depth_sum / (double)stat->samples;

		/* Also print the current sampled depth for easier
		 * real-time understanding of queue occupancy.
		 * Use a fixed-width name field so that the closing ']' column
		 * is aligned across different RT_Q types, and fixed-width
		 * numeric fields so Depth/cur/avg/max/tag columns line up.
		 */
		printf("[RT_Q %-20s]: Depth: cur=%4u avg=%6.1f max=%8u tag=%12llu\n",
		       stat->name, depth, avg, stat->depth_max,
		       (unsigned long long)stat->last_tag);

		clock_gettime(CLOCK_MONOTONIC, &stat->start_time);
		stat->depth_sum = 0;
		stat->samples = 0;
		stat->depth_max = 0;
	}

	pthread_spin_unlock(&stat->lock);
}

struct pf_evt *pf_reset_global_evt(const char *evt_name, struct pf_evt *evt);

#ifdef OXBOW_TRACK_TPUT

/* Global events. */
#define PF_EVT(event) struct pf_evt event

/**
 * @brief Thread-unsafe.
 * 
 */
#define PF_RESET(event)                                                        \
	do {                                                                   \
		pf_reset_global_evt(#event, &event);                           \
	} while (0)

#define PF_START(event)                                                                \
	do {                                                                           \
		pthread_spin_lock(&(event).lock);                                      \
		if ((event).used) {                                                    \
			printf("START_TIMER: timer already used. [tid:%lu %s():%d]\n", \
			       get_tid(), __func__, __LINE__);                         \
		}                                                                      \
		clock_gettime(CLOCK_MONOTONIC, &(event).start_time);                   \
		(event).used = 1;                                                      \
		pthread_spin_unlock(&(event).lock);                                    \
	} while (0)

#define PF_END(event)                                                          \
	do {                                                                   \
		pthread_spin_lock(&(event).lock);                              \
		if (!(event).used) {                                           \
			printf("END_TIMER: timer not started. [%s():%d]\n",    \
			       __func__, __LINE__);                            \
		}                                                              \
		clock_gettime(CLOCK_MONOTONIC, &(event).end_time);             \
		(event).time_sum +=                                            \
			get_duration(&(event).start_time, &(event).end_time);  \
		(event).count++;                                               \
		(event).used = 0;                                              \
		pthread_spin_unlock(&(event).lock);                            \
	} while (0)

#define PF_PRINT_HDR()                                                         \
	do {                                                                   \
		printf(",%-30s, %12s, %10s, %6s, %6s,\n", "evt_name", "usec",  \
		       "count", "avg", "%");                                   \
	} while (0)

#define PF_PRINT_FMT ",%-30s, %12.2f, %10lu, %6.2f, %6.2f %%,\n"
#define PF_PRINT_STATS(event, desc, event_denom)                               \
	printf(PF_PRINT_FMT, desc, event.time_sum * 1000000.0, event.count,    \
	       event.time_sum * 1000000.0 / event.count,                       \
	       (event.time_sum * 1000000.0) * 100 /                            \
		       (event_denom.time_sum * 1000000.0))

#define PF_PRINT_START_TIME(event, desc)                                       \
	printf("%-30s %lu %8.4f\n", desc, get_tid(),                           \
	       get_time(&(event).start_time))

#define PF_PRINT_END_TIME(event, desc)                                         \
	printf("%-30s %lu %8.4f\n", desc, get_tid(),                           \
	       get_time(&(event).end_time))

/**
 * @brief It requires to init using PF_RESET() first.
 * 
 */
#define PF_TRACK_TPUT(event, num_blocks)                                       \
	do {                                                                   \
		pthread_spin_lock(&(event.lock));                              \
		(event).used = 1;                                              \
		(event).is_counter = 1;                                        \
		pf_track_throughput(&(event), num_blocks);                     \
		pthread_spin_unlock(&(event.lock));                            \
	} while (0)
#else
// Declare it to prevent formatting issue.
#define PF_EVT(event) struct pf_evt event
#define PF_RESET(event)
#define PF_START(event)
#define PF_END(event)
#define PF_PRINT_HDR()
#define PF_PRINT_MP_HDR()
#define PF_PRINT_FMT
// event_denom: denominator.
#define PF_PRINT_STATS(event, desc, event_denom)
#define PF_PRINT_FMT_MP
#define PF_PRINT_MP_STATS(event, desc)
#define PF_PRINT_START_TIME(event, desc)
#define PF_PRINT_END_TIME(event, desc)
#define PF_TRACK_TPUT(event, num_blocks)
#endif

#ifdef OXBOW_PROFILE
/* Thread local events. */
#define PF_TL_EVT(event) __thread struct pf_evt *event

// Free event. PF_TL_START will re-alloc.
#define PF_TL_RESET(event)                                                     \
	do {                                                                   \
		if ((event)) {                                                 \
			free(event);                                           \
		}                                                              \
	} while (0)

#define PF_TL_START(event)                                                     \
	do {                                                                   \
		if (!(event)) {                                                \
			event = pf_add_evt(#event);                            \
		}                                                              \
                                                                               \
		if ((event)->used) {                                           \
			printf("PF_START: evt already used. [tid:%lu "         \
			       "%s():%d]\n",                                   \
			       get_tid(), __func__, __LINE__);                 \
		}                                                              \
		clock_gettime(CLOCK_MONOTONIC, &(event)->start_time);          \
		(event)->used = 1;                                             \
		(event)->is_counter = 0;                                       \
	} while (0)

#define PF_TL_END(event)                                                       \
	do {                                                                   \
		if (!event || !(event)->used) {                                \
			printf("PF_END: evt not started. [%s():%d]\n",         \
			       __func__, __LINE__);                            \
		}                                                              \
		clock_gettime(CLOCK_MONOTONIC, &(event)->end_time);            \
		(event)->time_sum += get_duration(&(event)->start_time,        \
						  &(event)->end_time);         \
		(event)->count++;                                              \
		(event)->used = 0;                                             \
	} while (0)

#define PF_TL_CNT(event, cnt)                                                  \
	do {                                                                   \
		if (!(event)) {                                                \
			event = pf_add_evt(#event);                            \
		}                                                              \
                                                                               \
		(event)->used = 1;                                             \
		(event)->is_counter = 1;                                       \
		(event)->count += cnt;                                         \
	} while (0)

#define PF_TL_PRINT_HDR()                                                      \
	do {                                                                   \
		printf(",%-32s, %12s, %10s, %6s,\n", "evt_name", "usec",       \
		       "count", "avg");                                        \
	} while (0)

#define PF_TL_PRINT_FMT ",%s%-32s, %12.2f, %10lu, %6.2f,\n"
#define PF_TL_PRINT_STATS(indent, event)                                       \
	do {                                                                   \
		if (event) {                                                   \
			printf(PF_TL_PRINT_FMT, indent, event->evt_name,       \
			       event->time_sum * 1000000.0, event->count,      \
			       event->time_sum * 1000000.0 / event->count);    \
		}                                                              \
	} while (0)

#define PF_TL_FPRINT_HDR(fp)                                                   \
	do {                                                                   \
		fprintf((fp), ",%-32s, %12s, %10s, %6s,\n", "evt_name",        \
			"usec", "count", "avg");                               \
	} while (0)

#define PF_TL_FPRINT_FMT ",%s%-32s, %12.2f, %10lu, %6.2f,\n"
#define PF_TL_FPRINT_STATS(fp, indent, event)                                  \
	do {                                                                   \
		if (event &&                                                   \
		    (event)->count > 0) { /* Avoid division by zero */         \
			fprintf((fp), PF_TL_FPRINT_FMT, indent,                \
				event->evt_name, event->time_sum * 1000000.0,  \
				event->count,                                  \
				event->time_sum * 1000000.0 / event->count);   \
		} else if (event) { /* Handle case where count is 0 */         \
			fprintf((fp), PF_TL_FPRINT_FMT, indent,                \
				event->evt_name, event->time_sum * 1000000.0,  \
				event->count, 0.0);                            \
		}                                                              \
	} while (0)

#define PF_TL_TRACK_TPUT(event, num_blocks)                                    \
	do {                                                                   \
		if (!(event)) {                                                \
			event = pf_add_evt(#event);                            \
		}                                                              \
		(event)->used = 1;                                             \
		(event)->is_counter = 1;                                       \
		pf_track_throughput(event, num_blocks);                        \
	} while (0)

#else /* OXBOW_PROFILE */
// Declare it to prevent formatting issue.
#define PF_TL_EVT(event) __thread struct pf_evt *event

#define PF_TL_RESET(event)
#define PF_TL_START(event)
#define PF_TL_END(event)
#define PF_TL_CNT(event, cnt)
#define PF_TL_PRINT_HDR()
#define PF_TL_PRINT_STATS(indent, event)

#define PF_TL_FPRINT_HDR(fp)                                                   \
	do {                                                                   \
	} while (0)
#define PF_TL_FPRINT_STATS(fp, indent, event)                                  \
	do {                                                                   \
	} while (0)

#define PF_TL_TRACK_TPUT(event, num_blocks)                                    \
	do {                                                                   \
	} while (0)

#endif /* OXBOW_PROFILE */

void pf_init(int signo);
void pf_exit(void);
void pf_reset_stats(void);
void pf_print_tl_stats(void);
void pf_print_stats(void);
int setup_pf_sig_handlers(int signo);
void pf_print_all_stats(void);
void pf_reset_all_stats(void);
void pf_add_evt_list(const char *evt_name);

/* Callback function to print global events. */
extern void (*g_print_evt_stats)(void);

struct pf_evt *pf_add_evt(const char *evt_name);

void pf_reset_evt_lists(void);
void pf_print_evt_lists(void);
void pf_dump_evt_lists_to_files(void);
double pf_track_throughput(struct pf_evt *evt, uint64_t num_blocks);

#endif
