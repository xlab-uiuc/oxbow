#ifndef _EXP_FLAG_H_
#define _EXP_FLAG_H_

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

/* Enable exp_flag.
 * To communicate between devfs and secure daemon through files for running
 * experiments.
 * /mnt/oxbow_flag should be mounted as a NFS share.
 */
#define EXP_FLAG

/**
 * Create a file at the given path and write a string value to it.
 * If the file already exists, it will be overwritten.
 * 
 * @param path The file path to create
 * @param value The string value to write to the file
 * @return 0 on success, -1 on failure
 */
int exp_flag_create_and_write(const char *path, const char *value);

/**
 * Read a string value from the file at the given path.
 * 
 * @param path The file path to read from
 * @param buffer Buffer to store the read string
 * @param buffer_size Size of the buffer
 * @return 0 on success, -1 on failure
 */
int exp_flag_read(const char *path, char *buffer, size_t buffer_size);

/**
 * Wait until the file at the given path contains the expected value.
 * This function will continuously read the file until the value matches.
 * 
 * @param path The file path to monitor
 * @param expected_value The value to wait for
 * @param timeout_ms Timeout in milliseconds (0 for no timeout)
 * @return 0 on success, -1 on timeout or error
 */
int exp_flag_wait_for_value(const char *path, const char *expected_value,
			    uint32_t timeout_ms);

/**
 * Check if the file exists and contains the expected value.
 * 
 * @param path The file path to check
 * @param expected_value The value to check for
 * @return true if file exists and contains expected value, false otherwise
 */
bool exp_flag_check_value(const char *path, const char *expected_value);

/* Macros for conditional exp_flag operations */
#ifdef EXP_FLAG
#define EXP_FLAG_WRITE(path, value) exp_flag_create_and_write(path, value)
#define EXP_FLAG_WAIT(path, value, timeout_ms) exp_flag_wait_for_value(path, value, timeout_ms)
#else
#define EXP_FLAG_WRITE(path, value) do {} while(0)
#define EXP_FLAG_WAIT(path, value, timeout_ms) (0)
#endif

#endif /* _EXP_FLAG_H_ */
