#include <stddef.h>
#include <stdio.h>
#include <signal.h>
#include <errno.h>
#include <string.h>
#include <stdlib.h>
#include <pthread.h>
#include <stdatomic.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include "global.h"
#include "list.h"
#include "thpool.h"
#include "profile.h"
#include "oxbow_debug.h"
#include <unistd.h>

struct pf_thpool_list_entry {
	threadpool thpool;
	void (*print_fn)(void *);
	void (*reset_fn)(void *);
	struct list_head list;
};

LIST_HEAD(g_pf_threads);
pthread_spinlock_t g_pf_threads_lock;
__thread struct pf_thread_evt_lists *g_curr_thread_evt_lists = NULL;

#define MAX_EVENTS_PER_THREAD 100 // Set enough number.

static struct pf_thread_evt_lists *get_curr_thread_evt_lists(void)
{
	struct pf_thread_evt_lists *thread_lists;

	if (g_curr_thread_evt_lists)
		return g_curr_thread_evt_lists;

	// Allocate new thread event lists structure
	thread_lists = calloc(1, sizeof(struct pf_thread_evt_lists));
	if (!thread_lists) {
		oxb_error("Failed to allocate thread event lists");
		return NULL;
	}

	thread_lists->tid = get_tid();
	thread_lists->evts =
		calloc(MAX_EVENTS_PER_THREAD, sizeof(struct pf_evt));
	if (!thread_lists->evts) {
		oxb_error("Failed to allocate events array");
		free(thread_lists);
		return NULL;
	}
	atomic_init(&thread_lists->evt_id, 0);
	INIT_LIST_HEAD(&thread_lists->list);

	// Add to global list
	pthread_spin_lock(&g_pf_threads_lock);
	list_add_tail(&thread_lists->list, &g_pf_threads);
	pthread_spin_unlock(&g_pf_threads_lock);

	g_curr_thread_evt_lists = thread_lists;
	return thread_lists;
}

// Communicate with user via signals (+ value.)
// Signal handler.
void pf_sig_handler(int signo, siginfo_t *sip, void *ptr)
{
	UNUSED2(signo, ptr);

	switch (sip->si_value.sival_int) {
	case 1: // Reset events.
		pf_reset_evt_lists();
		break;

	case 2: // Print event stats.
		pf_print_evt_lists();
		break;

	default:
		oxb_error("Unknown value:%d", sip->si_value.sival_int);
		break;
	}
}

int setup_pf_sig_handlers(int signo)
{
	struct sigaction sigact = { 0 };
	int rc;

	sigemptyset(&sigact.sa_mask);
	sigact.sa_sigaction = pf_sig_handler;
	sigact.sa_flags =
		SA_SIGINFO; // An int value will be delivered with the signal.
	rc = sigaction(signo, &sigact, NULL);
	if (rc < 0) {
		oxb_error("sigaction (signo: %d) failed, errno %d (%s)", signo,
			  errno, strerror(errno));
		return -1;
	}

	return 0;
}

/**
 * @brief 
 * 
 * @param signo A signal number to use to trigger callback functions.
 */
void pf_init(int signo)
{
	int rc;

	// Initialize spinlock
	pthread_spin_init(&g_pf_threads_lock, PTHREAD_PROCESS_PRIVATE);

	// Register sighandler.
	rc = setup_pf_sig_handlers(signo);
	if (rc != 0) {
		oxb_error("Failed to setup signal handlers.");
		exit(EXIT_FAILURE);
	}
}

void pf_exit(void)
{
	// Destroy spinlock
	pthread_spin_destroy(&g_pf_threads_lock);
}

/**
 * @brief Add an event to the thread's event array.
 * 
 * @param evt_name Name of the event.
 * @return struct pf_evt* 
 */
struct pf_evt *pf_add_evt(const char *evt_name)
{
	struct pf_thread_evt_lists *thread_lists = get_curr_thread_evt_lists();
	if (!thread_lists) {
		oxb_error("[Error] Failed to get thread event lists\n");
		return NULL;
	}

	uint16_t curr_id = atomic_fetch_add(&thread_lists->evt_id, 1);
	if (curr_id >= MAX_EVENTS_PER_THREAD) {
		oxb_error("[Error] Maximum events per thread reached\n");
		atomic_fetch_sub(&thread_lists->evt_id, 1);
		return NULL;
	}

	struct pf_evt *evt = &thread_lists->evts[curr_id];
	evt->evt_name = strdup(evt_name);
	if (!evt->evt_name) {
		oxb_error("[Error] Failed to allocate event name");
		atomic_fetch_sub(&thread_lists->evt_id, 1);
		return NULL;
	}

	evt->id = curr_id;
	evt->count = 0;
	evt->time_sum = 0.0;
	evt->used = 0;
	evt->last_output_count = 0;
	memset(&evt->start_time, 0, sizeof(struct timespec));
	memset(&evt->end_time, 0, sizeof(struct timespec));
	memset(&evt->last_output_time, 0, sizeof(struct timespec));

	return evt;
}

/**
 * @brief Reset a global event.
 * 
 * @param evt_name Name of the event.
 * @return struct pf_evt* 
 */
struct pf_evt *pf_reset_global_evt(const char *evt_name, struct pf_evt *evt)
{
	evt->evt_name = strdup(evt_name);
	if (!evt->evt_name) {
		oxb_error("[Error] Failed to allocate event name");
		return NULL;
	}

	evt->id = 0;
	evt->count = 0;
	evt->time_sum = 0.0;
	evt->used = 0;
	evt->last_output_count = 0;
	memset(&evt->start_time, 0, sizeof(struct timespec));
	memset(&evt->end_time, 0, sizeof(struct timespec));
	memset(&evt->last_output_time, 0, sizeof(struct timespec));
	pthread_spin_init(&evt->lock, PTHREAD_PROCESS_PRIVATE);
	return evt;
}

/**
 * @brief Reset all events for all threads
 */
void pf_reset_evt_lists(void)
{
	struct pf_thread_evt_lists *thread_lists;
	struct pf_evt *evt;

	pthread_spin_lock(&g_pf_threads_lock);
	list_for_each_entry (thread_lists, &g_pf_threads, list) {
		uint16_t curr_id = atomic_load(&thread_lists->evt_id);

		// Reset each event in the array
		for (uint16_t i = 0; i < curr_id; i++) {
			evt = &thread_lists->evts[i];
			evt->count = 0;
			evt->time_sum = 0.0;
			evt->used = 0;
			evt->last_output_count = 0;
			memset(&evt->start_time, 0, sizeof(struct timespec));
			memset(&evt->end_time, 0, sizeof(struct timespec));
			memset(&evt->last_output_time, 0, sizeof(struct timespec));
		}
	}
	pthread_spin_unlock(&g_pf_threads_lock);
}

// Keep the comparison function
static int compare_events(const void *a, const void *b)
{
	const struct pf_evt *evt_a = (const struct pf_evt *)a;
	const struct pf_evt *evt_b = (const struct pf_evt *)b;

	if (!evt_a->evt_name)
		return 1;
	if (!evt_b->evt_name)
		return -1;

	return strcmp(evt_a->evt_name, evt_b->evt_name);
}

/**
 * @brief Print all events for all threads, with events sorted within each thread
 */
void pf_print_evt_lists(void)
{
	struct pf_thread_evt_lists *thread_lists;
	struct pf_evt *evt;
	struct pf_evt *thread_events = NULL;

	printf("\n,=============== PROFILE EVENT LISTS ===============,,,,\n");
	PF_TL_PRINT_HDR();

	pthread_spin_lock(&g_pf_threads_lock);
	list_for_each_entry (thread_lists, &g_pf_threads, list) {
		uint16_t curr_id = atomic_load(&thread_lists->evt_id);
		if (curr_id == 0) // No events in this thread.
			continue;

		// Allocate temporary array for this thread's events
		thread_events = calloc(curr_id, sizeof(struct pf_evt));
		if (!thread_events) {
			oxb_error(
				"Failed to allocate memory for sorting thread events");
			continue;
		}

		// Copy this thread's events to temporary array
		for (uint16_t i = 0; i < curr_id; i++) {
			memcpy(&thread_events[i], &thread_lists->evts[i],
			       sizeof(struct pf_evt));
		}

		// Sort this thread's events
		qsort(thread_events, curr_id, sizeof(struct pf_evt),
		      compare_events);

		// Print thread header
		printf("\n Thread ID: %lu\n", (unsigned long)thread_lists->tid);

		// Print sorted events for this thread
		for (uint16_t i = 0; i < curr_id; i++) {
			if (thread_events[i]
				    .evt_name) { // Only print valid events
				evt = &thread_events[i];
				PF_TL_PRINT_STATS(
					"  ",
					evt); // Added indent for thread events
			}
		}

		free(thread_events);
	}
	pthread_spin_unlock(&g_pf_threads_lock);

	// Print summary of events with the same name
	printf("\n,=============== SUMMARY OF EVENTS ===============,,,,\n");
	PF_TL_PRINT_HDR();

	pthread_spin_lock(&g_pf_threads_lock);
	struct pf_evt *all_events = NULL;
	size_t total_events = 0;

	// First, count total events
	list_for_each_entry (thread_lists, &g_pf_threads, list) {
		total_events += atomic_load(&thread_lists->evt_id);
	}

	if (total_events > 0) {
		// Allocate array for all events
		all_events = calloc(total_events, sizeof(struct pf_evt));
		if (!all_events) {
			oxb_error(
				"Failed to allocate memory for event summary");
			pthread_spin_unlock(&g_pf_threads_lock);
			return;
		}

		// Copy all events to the array
		size_t idx = 0;
		list_for_each_entry (thread_lists, &g_pf_threads, list) {
			uint16_t curr_id = atomic_load(&thread_lists->evt_id);
			for (uint16_t i = 0; i < curr_id; i++) {
				if (thread_lists->evts[i].evt_name) {
					memcpy(&all_events[idx],
					       &thread_lists->evts[i],
					       sizeof(struct pf_evt));
					idx++;
				}
			}
		}

		// Sort all events
		qsort(all_events, idx, sizeof(struct pf_evt), compare_events);

		// Print summed stats for events with the same name
		const char *curr_name = NULL;
		uint64_t total_count = 0;
		double total_time = 0.0;

		for (size_t i = 0; i < idx; i++) {
			if (!all_events[i].evt_name)
				continue;

			if (!curr_name ||
			    strcmp(curr_name, all_events[i].evt_name) != 0) {
				// Print previous event's total if exists
				if (curr_name) {
					struct pf_evt sum_evt;
					sum_evt.evt_name = (char *)curr_name;
					sum_evt.id = 0;
					sum_evt.count = total_count;
					sum_evt.time_sum = total_time;
					sum_evt.used = 1;
					sum_evt.is_counter = 0;
					memset(&sum_evt.start_time, 0,
					       sizeof(struct timespec));
					memset(&sum_evt.end_time, 0,
					       sizeof(struct timespec));
					struct pf_evt *evt_ptr = &sum_evt;
					PF_TL_PRINT_STATS("  ", evt_ptr);
				}
				// Start new event
				curr_name = all_events[i].evt_name;
				total_count = all_events[i].count;
				total_time = all_events[i].time_sum;
			} else {
				// Add to current event's total
				total_count += all_events[i].count;
				total_time += all_events[i].time_sum;
			}
		}

		// Print last event's total
		if (curr_name) {
			struct pf_evt sum_evt;
			sum_evt.evt_name = (char *)curr_name;
			sum_evt.id = 0;
			sum_evt.count = total_count;
			sum_evt.time_sum = total_time;
			sum_evt.used = 1;
			sum_evt.is_counter = 0;
			memset(&sum_evt.start_time, 0, sizeof(struct timespec));
			memset(&sum_evt.end_time, 0, sizeof(struct timespec));
			struct pf_evt *evt_ptr = &sum_evt;
			PF_TL_PRINT_STATS("  ", evt_ptr);
		}

		free(all_events);
	}
	pthread_spin_unlock(&g_pf_threads_lock);

#ifdef PF_PRINT_TO_FILE
	pf_dump_evt_lists_to_files();
#endif
}

/**
 * @brief Track throughput in MB/s based on number of 4KB blocks processed
 * 
 * @param evt Profiling event to update
 * @param num_blocks Number of 4KB blocks processed in this call
 * @return double Calculated throughput in MB/s
 */
double pf_track_throughput(struct pf_evt *evt, uint64_t num_blocks)
{
	if (!evt || !evt->used) {
		oxb_warn("[Warn] Invalid event for throughput tracking");
		return 0.0;
	}

	// Update the total block count
	evt->count += num_blocks;
	
	// Get current time
	struct timespec current_time;
	clock_gettime(CLOCK_MONOTONIC, &current_time);

	// Check if at least 1 second has passed since last output
	double time_since_last_output = 0.0;
	if (evt->last_output_time.tv_sec > 0) {
		time_since_last_output =
			(double)(current_time.tv_sec - evt->last_output_time.tv_sec) +
			(double)(current_time.tv_nsec - evt->last_output_time.tv_nsec) /
				1000000000.0;
	}

	// Calculate throughput using blocks since last output
	uint64_t blocks_since_last_output = evt->count - evt->last_output_count;
	
	// Calculate data size: blocks_since_last_output * 4KB converted to MB
	double data_size_mb = (double)(blocks_since_last_output * 4 * 1024) / (1024 * 1024);
	
	// Calculate throughput (MB/s) by dividing by the time since last output
	// If this is the first call, use the original time_diff
	double throughput = 0.0;
	if (evt->last_output_time.tv_sec == 0) {
		throughput = 0.0;
	} else {
		throughput = data_size_mb / time_since_last_output;
	}
	
	// Store additional info in the event
	evt->time_sum += time_since_last_output;

	// Only print if this is the first output or at least 1 second has passed
	if (evt->last_output_time.tv_sec == 0 ||
	    time_since_last_output >= 1.0) {
		printf("[Thput] (tid: %lu) %s: %.2f MB/s (%.2f MB in %.6f seconds, %lu blocks since last output, %lu total blocks)\n",
		       get_tid(),evt->evt_name, throughput, data_size_mb,
		       (evt->last_output_time.tv_sec == 0) ? 0 : time_since_last_output,
		       blocks_since_last_output, evt->count);

		// Update last output time and count
		evt->last_output_time = current_time;
		evt->last_output_count = evt->count;
	}

	return throughput;
}

/**
 * @brief Dump all events for all threads to separate files, with events sorted within each thread
 */
void pf_dump_evt_lists_to_files(void)
{
	struct pf_thread_evt_lists *thread_lists;
	struct pf_evt *evt;
	struct pf_evt *thread_events = NULL;
	FILE *fp = NULL;
	char filename[256];
	char time_subdir[256]; // To store profile/YYYYMMDD_HHMMSS
	const char *base_profile_dir = "/tmp/profile";
	pid_t pid = getpid(); // Get the process ID
	time_t now;
	struct tm *local_time;
	char time_str[20]; // For YYYYMMDD_HHMMSS format

	// Get current time and format it
	now = time(NULL);
	local_time = localtime(&now);
	if (local_time == NULL) {
		oxb_error("Failed to get local time: %s", strerror(errno));
		return;
	}
	strftime(time_str, sizeof(time_str), "%Y%m%d_%H%M%S", local_time);

	// Construct the full path for the timestamped subdirectory
	snprintf(time_subdir, sizeof(time_subdir), "%s/%s", base_profile_dir,
		 time_str);

	// Create base profile directory if it doesn't exist
	struct stat st = { 0 };
	if (stat(base_profile_dir, &st) == -1) {
		if (mkdir(base_profile_dir, 0755) == -1) {
			oxb_error("Failed to create directory %s: %s",
				  base_profile_dir, strerror(errno));
			return;
		}
	} else if (!S_ISDIR(st.st_mode)) {
		oxb_error("Path %s exists but is not a directory",
			  base_profile_dir);
		return;
	}

	// Create timestamped subdirectory
	memset(&st, 0, sizeof(st)); // Reset stat struct for the next check
	if (stat(time_subdir, &st) == -1) {
		if (mkdir(time_subdir, 0755) == -1) {
			oxb_warn("Failed to create directory %s: %s",
				  time_subdir, strerror(errno));
			return;
		}
	} else if (!S_ISDIR(st.st_mode)) {
		oxb_error("Path %s exists but is not a directory", time_subdir);
		return;
	}

	pthread_spin_lock(&g_pf_threads_lock);
	list_for_each_entry (thread_lists, &g_pf_threads, list) {
		uint16_t curr_id = atomic_load(&thread_lists->evt_id);
		if (curr_id == 0) // No events in this thread.
			continue;

		// Allocate temporary array for this thread's events
		thread_events = calloc(curr_id, sizeof(struct pf_evt));
		if (!thread_events) {
			oxb_error(
				"Failed to allocate memory for sorting thread events");
			continue; // Move to the next thread
		}

		// Copy this thread's events to temporary array
		for (uint16_t i = 0; i < curr_id; i++) {
			memcpy(&thread_events[i], &thread_lists->evts[i],
			       sizeof(struct pf_evt));
		}

		// Sort this thread's events
		qsort(thread_events, curr_id, sizeof(struct pf_evt),
		      compare_events);

		// Create filename for this thread within the timestamped directory
		snprintf(filename, sizeof(filename), "%s/pid%d_th%lu.csv",
			 time_subdir, pid, (unsigned long)thread_lists->tid);

		// Open file for writing
		fp = fopen(filename, "w");
		if (!fp) {
			oxb_error("Failed to open file %s for writing",
				  filename);
			free(thread_events);
			continue; // Move to the next thread
		}

		// Write thread header to file
		fprintf(fp,
			",=============== PROFILE EVENT LIST FOR THREAD %lu ===============,,,\n",
			(unsigned long)thread_lists->tid);
		PF_TL_FPRINT_HDR(fp);

		// Write sorted events for this thread to file
		for (uint16_t i = 0; i < curr_id; i++) {
			if (thread_events[i]
				    .evt_name) { // Only write valid events
				evt = &thread_events[i];
				PF_TL_FPRINT_STATS(fp, "  ", evt);
			}
		}

		free(thread_events);
		fclose(fp);
	}
	pthread_spin_unlock(&g_pf_threads_lock);

	// Dump summary of events to a file in the timestamped directory
	snprintf(filename, sizeof(filename), "%s/pid%d_summary.csv",
		 time_subdir, pid);
	fp = fopen(filename, "w");
	if (!fp) {
		oxb_error("Failed to open file %s for writing", filename);
		return;
	}

	fprintf(fp, ",=============== SUMMARY OF EVENTS ===============,,,\n");
	PF_TL_FPRINT_HDR(fp);

	pthread_spin_lock(&g_pf_threads_lock);
	struct pf_evt *all_events = NULL;
	size_t total_events = 0;

	// First, count total events
	list_for_each_entry (thread_lists, &g_pf_threads, list) {
		total_events += atomic_load(&thread_lists->evt_id);
	}

	if (total_events > 0) {
		// Allocate array for all events
		all_events = calloc(total_events, sizeof(struct pf_evt));
		if (!all_events) {
			oxb_error(
				"Failed to allocate memory for event summary");
			fclose(fp);
			pthread_spin_unlock(&g_pf_threads_lock);
			return;
		}

		// Copy all events to the array
		size_t idx = 0;
		list_for_each_entry (thread_lists, &g_pf_threads, list) {
			uint16_t curr_id = atomic_load(&thread_lists->evt_id);
			for (uint16_t i = 0; i < curr_id; i++) {
				if (thread_lists->evts[i].evt_name) {
					memcpy(&all_events[idx],
					       &thread_lists->evts[i],
					       sizeof(struct pf_evt));
					idx++;
				}
			}
		}

		// Sort all events
		qsort(all_events, idx, sizeof(struct pf_evt), compare_events);

		// Print summed stats for events with the same name
		const char *curr_name = NULL;
		uint64_t total_count = 0;
		double total_time = 0.0;

		for (size_t i = 0; i < idx; i++) {
			if (!all_events[i].evt_name)
				continue;

			if (!curr_name ||
			    strcmp(curr_name, all_events[i].evt_name) != 0) {
				// Print previous event's total if exists
				if (curr_name) {
					struct pf_evt sum_evt;
					sum_evt.evt_name = (char *)curr_name;
					sum_evt.id = 0;
					sum_evt.count = total_count;
					sum_evt.time_sum = total_time;
					sum_evt.used = 1;
					sum_evt.is_counter = 0;
					memset(&sum_evt.start_time, 0,
					       sizeof(struct timespec));
					memset(&sum_evt.end_time, 0,
					       sizeof(struct timespec));
					struct pf_evt *evt_ptr = &sum_evt;
					PF_TL_FPRINT_STATS(fp, "  ", evt_ptr);
				}
				// Start new event
				curr_name = all_events[i].evt_name;
				total_count = all_events[i].count;
				total_time = all_events[i].time_sum;
			} else {
				// Add to current event's total
				total_count += all_events[i].count;
				total_time += all_events[i].time_sum;
			}
		}

		// Print last event's total
		if (curr_name) {
			struct pf_evt sum_evt;
			sum_evt.evt_name = (char *)curr_name;
			sum_evt.id = 0;
			sum_evt.count = total_count;
			sum_evt.time_sum = total_time;
			sum_evt.used = 1;
			sum_evt.is_counter = 0;
			memset(&sum_evt.start_time, 0, sizeof(struct timespec));
			memset(&sum_evt.end_time, 0, sizeof(struct timespec));
			struct pf_evt *evt_ptr = &sum_evt;
			PF_TL_FPRINT_STATS(fp, "  ", evt_ptr);
		}

		free(all_events);
	}
	pthread_spin_unlock(&g_pf_threads_lock);

	fclose(fp);

	printf("Profiling results are saved in %s.\n", time_subdir);
}
