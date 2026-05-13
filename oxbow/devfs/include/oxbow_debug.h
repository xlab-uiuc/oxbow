#ifndef _OXBOW_DEBUG_H_
#define _OXBOW_DEBUG_H_
#include <sys/syscall.h>
#include <unistd.h>
#include <string.h>
#include <ctype.h>

// Overwrite ENABLE_PRINT of log.c library.
#define ENABLE_PRINT 1
#include "log.h"

#define PRINT_OXBOW_INFO
// #define PRINT_OXBOW_DEBUG
// #define PRINT_MSG_INFO
// #define PRINT_MSG_DEBUG
#define PRINT_COMMIT_INFO
// #define PRINT_COMMIT_DEBUG
#define PRINT_CKPT_INFO
// #define PRINT_CKPT_DEBUG
// #define PRINT_STORAGE_ENGINE_INFO
// #define PRINT_STORAGE_ENGINE_DEBUG
// #define PRINT_BIO_DEBUG // Refer to bio.h
// #define PRINT_MSG_DEBUG
// #define PRINT_STORAGE_ENGINE_DEBUG
// #define PRINT_BIO_DEBUG // Print bio_list.

// #define PRINT_DUMP_BLK // Dump blocks.

/* Dump data blocks in rdma (or shm) buffer. */
// #define DEBUG_HEXDUMP_JNL_DATA_PAGES

/* Dump a whole transaction. */
// #define DEBUG_PRINT_JNL_TX

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

#define d_trace(...) log_trace(__VA_ARGS__)
#define d_debug(...) log_debug(__VA_ARGS__)

#ifdef PRINT_OXBOW_DEBUG
#define oxb_trace(...) log_trace(__VA_ARGS__)
#define oxb_debug(...) log_debug(__VA_ARGS__)
#else
#define oxb_trace(...)                                                         \
	do {                                                                   \
	} while (0)
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
#define oxb_warn(...) log_warn(__VA_ARGS__)
#define oxb_error(...) log_error(__VA_ARGS__)
#define oxb_fatal(...) log_fatal(__VA_ARGS__)

#ifdef PRINT_MSG_DEBUG
#define msg_trace(...) log_trace(__VA_ARGS__)
#define msg_debug(...) log_debug(__VA_ARGS__)
#else
#define msg_trace(...)                                                         \
	do {                                                                   \
	} while (0)
#define msg_debug(...)                                                         \
	do {                                                                   \
	} while (0)
#endif
#if defined(PRINT_MSG_INFO) || defined(PRINT_MSG_DEBUG)
#define msg_info(...) log_info(__VA_ARGS__)
#else
#define msg_info(...)                                                          \
	do {                                                                   \
	} while (0)
#endif
#define msg_warn(...) log_warn(__VA_ARGS__)
#define msg_error(...) log_error(__VA_ARGS__)
#define msg_fatal(...) log_fatal(__VA_ARGS__)

#ifdef PRINT_STORAGE_ENGINE_DEBUG
#define se_trace(...) log_trace(__VA_ARGS__)
#define se_debug(...) log_debug(__VA_ARGS__)
#else
#define se_trace(...)                                                          \
	do {                                                                   \
	} while (0)
#define se_debug(...)                                                          \
	do {                                                                   \
	} while (0)
#endif
#if defined(PRINT_STORAGE_ENGINE_INFO) || defined(PRINT_STORAGE_ENGINE_DEBUG)
#define se_info(...) log_info(__VA_ARGS__)
#else
#define se_info(...)                                                           \
	do {                                                                   \
	} while (0)
#endif
#define se_warn(...) log_warn(__VA_ARGS__)
#define se_error(...) log_error(__VA_ARGS__)
#define se_fatal(...) log_fatal(__VA_ARGS__)

#ifdef PRINT_COMMIT_DEBUG
#define commit_trace(...) log_trace(__VA_ARGS__)
#define commit_debug(...) log_debug(__VA_ARGS__)
#else
#define commit_trace(...)                                                      \
	do {                                                                   \
	} while (0)
#define commit_debug(...)                                                      \
	do {                                                                   \
	} while (0)
#endif
#if defined(PRINT_COMMIT_INFO) || defined(PRINT_COMMIT_DEBUG)
#define commit_info(...) log_info(__VA_ARGS__)
#else
#define commit_info(...)                                                       \
	do {                                                                   \
	} while (0)
#endif
#define commit_warn(...) log_warn(__VA_ARGS__)
#define commit_error(...) log_error(__VA_ARGS__)
#define commit_fatal(...) log_fatal(__VA_ARGS__)

#ifdef PRINT_CKPT_DEBUG
#define ckpt_trace(...) log_trace(__VA_ARGS__)
#define ckpt_debug(...) log_debug(__VA_ARGS__)
#else
#define ckpt_trace(...)                                                        \
	do {                                                                   \
	} while (0)
#define ckpt_debug(...)                                                        \
	do {                                                                   \
	} while (0)
#endif
#if defined(PRINT_CKPT_INFO) || defined(PRINT_CKPT_DEBUG)
#define ckpt_info(...) log_info(__VA_ARGS__)
#else
#define ckpt_info(...)                                                         \
	do {                                                                   \
	} while (0)
#endif
#define ckpt_warn(...) log_warn(__VA_ARGS__)
#define ckpt_error(...) log_error(__VA_ARGS__)
#define ckpt_fatal(...) log_fatal(__VA_ARGS__)
/**
 * @brief Hexdump a page of memory for debugging purposes
 * 
 * @param data Pointer to the data to dump
 * @param size Size of the data to dump (typically PAGE_SIZE)
 * @param label Label to print before the hexdump
 * @param offset_start Starting offset for display (for partial dumps)
 */
static inline void hexdump_page(const void *data, size_t size,
				const char *label, size_t offset_start)
{
	const unsigned char *bytes = (const unsigned char *)data;
	size_t i, j;

	if (!data || size == 0) {
		printf("[HEXDUMP] %s: NULL data or zero size\n", label);
		return;
	}

	// Limit dump size to avoid excessive output (max 128 bytes for debugging)
	size_t dump_size = (size > 128) ? 128 : size;

	printf("[HEXDUMP] %s (showing %zu/%zu bytes):\n", label, dump_size,
	       size);

	for (i = 0; i < dump_size; i += 16) {
		char hex_line[128] = { 0 };
		char ascii_line[32] = { 0 };
		char full_line[256] = { 0 };

		// Build hex values
		for (j = 0; j < 16; j++) {
			if (i + j < dump_size) {
				sprintf(hex_line + strlen(hex_line), "%02x ",
					bytes[i + j]);
			} else {
				sprintf(hex_line + strlen(hex_line), "   ");
			}
		}

		// Build ASCII representation
		for (j = 0; j < 16 && (i + j) < dump_size; j++) {
			unsigned char c = bytes[i + j];
			ascii_line[j] = isprint(c) ? c : '.';
		}
		ascii_line[j] = '\0';

		// Combine into one line and print
		snprintf(full_line, sizeof(full_line), "%08zx: %s |%s|",
			 offset_start + i, hex_line, ascii_line);
		printf("%s\n", full_line);
	}

	if (size > dump_size) {
		printf("[HEXDUMP] ... (truncated, %zu more bytes)\n",
		       size - dump_size);
	}
}

/**
 * @brief Simplified hexdump for quick debugging
 * 
 * @param data Pointer to the data to dump
 * @param size Size of the data to dump
 * @param label Label to identify the dump
 */
static inline void quick_hexdump(const void *data, size_t size,
				 const char *label)
{
	hexdump_page(data, size, label, 0);
}

#endif
