#ifndef _OXBOW_DEBUG_H_
#define _OXBOW_DEBUG_H_
#include "log.h"

/* Enable print messages */
#define PRINT_OXBOW_INFO
// #define PRINT_OXBOW_DEBUG
// #define PRINT_LIBFS_DEBUG
// #define PRINT_POSIX_DEBUG
// #define PRINT_INODE_DEBUG
// #define PRINT_RW_DEBUG
// #define PRINT_MSG_DEBUG
// #define PRINT_BIO_DEBUG
// #define PRINT_SHM_DEBUG
// #define PRINT_SPINNING_DEBUG

// printf color set.
#define PRINT_COLOR
#ifdef PRINT_COLOR
#define ANSI_COLOR_RED "\x1b[0;31m"
#define ANSI_COLOR_RED_BOLD "\x1b[1;31m"
#define ANSI_COLOR_GREEN "\x1b[0;32m"
#define ANSI_COLOR_GREEN_BOLD "\x1b[1;32m"
#define ANSI_COLOR_YELLOW "\x1b[0;33m"
#define ANSI_COLOR_YELLOW_BOLD "\x1b[1;33m"
#define ANSI_COLOR_BLUE "\x1b[0;34m"
#define ANSI_COLOR_BLUE_BOLD "\x1b[1;34m"
#define ANSI_COLOR_MAGENTA "\x1b[0;35m"
#define ANSI_COLOR_MAGENTA_BOLD "\x1b[1;35m"
#define ANSI_COLOR_CYAN "\x1b[0;36m"
#define ANSI_COLOR_CYAN_BOLD "\x1b[1;36m"
#define ANSI_COLOR_BRIGHT_RED "\x1b[0;91m"
#define ANSI_COLOR_BRIGHT_RED_BOLD "\x1b[1;91m"
#define ANSI_COLOR_BRIGHT_GREEN "\x1b[0;92m"
#define ANSI_COLOR_BRIGHT_GREEN_BOLD "\x1b[1;92m"
#define ANSI_COLOR_BRIGHT_YELLOW "\x1b[0;93m"
#define ANSI_COLOR_BRIGHT_YELLOW_BOLD "\x1b[1;93m"
#define ANSI_COLOR_BRIGHT_BLUE "\x1b[0;94m"
#define ANSI_COLOR_BRIGHT_BLUE_BOLD "\x1b[1;94m"
#define ANSI_COLOR_BRIGHT_MAGENTA "\x1b[0;95m"
#define ANSI_COLOR_BRIGHT_MAGENTA_BOLD "\x1b[1;95m"
#define ANSI_COLOR_BRIGHT_CYAN "\x1b[0;96m"
#define ANSI_COLOR_BRIGHT_CYAN_BOLD "\x1b[1;96m"
#define ANSI_COLOR_RESET "\x1b[0m"
#else
#define ANSI_COLOR_RED
#define ANSI_COLOR_RED_BOLD
#define ANSI_COLOR_GREEN
#define ANSI_COLOR_GREEN_BOLD
#define ANSI_COLOR_YELLOW
#define ANSI_COLOR_YELLOW_BOLD
#define ANSI_COLOR_BLUE
#define ANSI_COLOR_BLUE_BOLD
#define ANSI_COLOR_MAGENTA
#define ANSI_COLOR_MAGENTA_BOLD
#define ANSI_COLOR_CYAN
#define ANSI_COLOR_CYAN_BOLD
#define ANSI_COLOR_BRIGHT_RED
#define ANSI_COLOR_BRIGHT_RED_BOLD
#define ANSI_COLOR_BRIGHT_GREEN
#define ANSI_COLOR_BRIGHT_GREEN_BOLD
#define ANSI_COLOR_BRIGHT_YELLOW
#define ANSI_COLOR_BRIGHT_YELLOW_BOLD
#define ANSI_COLOR_BRIGHT_BLUE
#define ANSI_COLOR_BRIGHT_BLUE_BOLD
#define ANSI_COLOR_BRIGHT_MAGENTA
#define ANSI_COLOR_BRIGHT_MAGENTA_BOLD
#define ANSI_COLOR_BRIGHT_CYAN
#define ANSI_COLOR_BRIGHT_CYAN_BOLD
#define ANSI_COLOR_RESET
#endif

// struct timespec ts;
// long a, b, c, d, end;
// a = b = c = d = end = 0;
// clock_gettime(CLOCK_MONOTONIC, &ts);
// end = ts.tv_nsec;
// printf("[%s] a %04ld \n", __func__, (end - a) / 1000);
// printf("[%s] b %04ld \n", __func__, (end - b) / 1000);
// printf("[%s] c %04ld \n", __func__, (end - c) / 1000);

#ifdef PRINT_OXBOW_DEBUG
#define oxb_debug(...) log_debug(__VA_ARGS__)
#else
#define oxb_debug(...)                                                         \
	do {                                                                   \
	} while (0)
#endif
#if defined(PRINT_OXBOW_INFO) || defined(PRINT_OXBOW_DEBUG)
#define oxb_info(...) log_info(__VA_ARGS__)
#else
#define oxb_info(...)                                                          \
	do {                                                                   \
	} while (0)
#endif
#define oxb_warn(...)                                                                   \
	do {                                                                            \
		fflush(stdout); /* Adjust as needed, e.g., to a specific file stream */ \
		log_warn(__VA_ARGS__);                                                  \
		fflush(stdout); /* Adjust as needed, e.g., to a specific file stream */ \
	} while (0)
#define oxb_error(...) log_error(__VA_ARGS__)
#define oxb_fatal(...) log_fatal(__VA_ARGS__)

#ifdef PRINT_LIBFS_DEBUG
// #define l_trace(...) log_trace(__VA_ARGS__)
#define l_trace(...)                                                           \
	do {                                                                   \
	} while (0)
// #define l_debug(...) log_debug(__VA_ARGS__)
#define l_debug(...)                                                                    \
	do {                                                                            \
		fflush(stdout); /* Adjust as needed, e.g., to a specific file stream */ \
		log_debug(__VA_ARGS__);                                                 \
		fflush(stdout); /* Adjust as needed, e.g., to a specific file stream */ \
	} while (0)

#define libfs_info(...) log_info(__VA_ARGS__)
#define libfs_warn(...) log_warn(__VA_ARGS__)
#else
#define l_trace(...)                                                           \
	do {                                                                   \
	} while (0)
#define l_debug(...)                                                           \
	do {                                                                   \
	} while (0)
#define libfs_info(...) log_info(__VA_ARGS__)
#define libfs_warn(...) log_warn(__VA_ARGS__)
#endif

#ifdef PRINT_POSIX_DEBUG
#define posix_trace(...) log_trace(__VA_ARGS__)
#define posix_debug(...)                                                                \
	do {                                                                            \
		fflush(stdout); /* Adjust as needed, e.g., to a specific file stream */ \
		log_debug(__VA_ARGS__);                                                 \
		fflush(stdout); /* Adjust as needed, e.g., to a specific file stream */ \
	} while (0)
#else
#define posix_trace(...)                                                       \
	do {                                                                   \
	} while (0)
#define posix_debug(...)                                                       \
	do {                                                                   \
	} while (0)
#endif

#ifdef PRINT_INODE_DEBUG
#define i_debug(...)                                                                    \
	do {                                                                            \
		fflush(stdout); /* Adjust as needed, e.g., to a specific file stream */ \
		log_debug(__VA_ARGS__);                                                 \
		fflush(stdout); /* Adjust as needed, e.g., to a specific file stream */ \
	} while (0)
#else
#define i_debug(...)                                                           \
	do {                                                                   \
	} while (0)
#endif

#ifdef PRINT_RW_DEBUG
#define rw_debug(...)                                                                   \
	do {                                                                            \
		fflush(stdout); /* Adjust as needed, e.g., to a specific file stream */ \
		log_debug(__VA_ARGS__);                                                 \
		fflush(stdout); /* Adjust as needed, e.g., to a specific file stream */ \
	} while (0)
#else
#define rw_debug(...)                                                          \
	do {                                                                   \
	} while (0)
#endif

#ifdef PRINT_MSG_DEBUG
#define msg_trace(...) log_trace(__VA_ARGS__)
#define msg_debug(...) log_debug(__VA_ARGS__)
#define msg_info(...) log_info(__VA_ARGS__)
#define msg_warn(...) log_warn(__VA_ARGS__)
#define msg_error(...) log_error(__VA_ARGS__)
#define msg_fatal(...) log_fatal(__VA_ARGS__)
#else
#define msg_trace(...)                                                         \
	do {                                                                   \
	} while (0)
#define msg_debug(...)                                                         \
	do {                                                                   \
	} while (0)
#define msg_info(...)                                                          \
	do {                                                                   \
	} while (0)
#define msg_warn(...)                                                          \
	do {                                                                   \
	} while (0)
#define msg_error(...) log_error(__VA_ARGS__)
#define msg_fatal(...) log_fatal(__VA_ARGS__)
#endif

#ifdef PRINT_SHM_DEBUG
#define shm_trace(...) log_trace(__VA_ARGS__)
#define shm_debug(...)                                                                  \
	do {                                                                            \
		fflush(stdout); /* Adjust as needed, e.g., to a specific file stream */ \
		log_debug(__VA_ARGS__);                                                 \
		fflush(stdout); /* Adjust as needed, e.g., to a specific file stream */ \
	} while (0)
#else
#define shm_trace(...)                                                         \
	do {                                                                   \
	} while (0)
#define shm_debug(...)                                                         \
	do {                                                                   \
	} while (0)
#endif

#ifdef PRINT_SPINNING_DEBUG
#define spin_dbg(...)                                                                   \
	do {                                                                            \
		fflush(stdout); /* Adjust as needed, e.g., to a specific file stream */ \
		log_debug(__VA_ARGS__);                                                 \
		fflush(stdout); /* Adjust as needed, e.g., to a specific file stream */ \
	} while (0)
#else
#define spin_dbg(...)                                                          \
	do {                                                                   \
	} while (0)
#endif

#endif
