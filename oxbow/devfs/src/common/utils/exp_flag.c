#include "common/utils/exp_flag.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <fcntl.h>

int exp_flag_create_and_write(const char *path, const char *value)
{
	if (!path || !value) {
		return -1;
	}

	FILE *file = fopen(path, "w");
	if (!file) {
		return -1;
	}

	size_t value_len = strlen(value);
	size_t written = fwrite(value, 1, value_len, file);

	fclose(file);

	if (written != value_len) {
		return -1;
	}

	return 0;
}

int exp_flag_read(const char *path, char *buffer, size_t buffer_size)
{
	if (!path || !buffer || buffer_size == 0) {
		return -1;
	}

	FILE *file = fopen(path, "r");
	if (!file) {
		return -1;
	}

	// Clear buffer
	memset(buffer, 0, buffer_size);

	// Read file content
	size_t read_bytes = fread(buffer, 1, buffer_size - 1, file);
	fclose(file);

	if (read_bytes == 0 && ferror(file)) {
		return -1;
	}

	// Remove trailing newline if present
	if (read_bytes > 0 && buffer[read_bytes - 1] == '\n') {
		buffer[read_bytes - 1] = '\0';
	}

	return 0;
}

bool exp_flag_check_value(const char *path, const char *expected_value)
{
	if (!path || !expected_value) {
		return false;
	}

	char buffer[256];
	if (exp_flag_read(path, buffer, sizeof(buffer)) != 0) {
		return false;
	}

	return strcmp(buffer, expected_value) == 0;
}

int exp_flag_wait_for_value(const char *path, const char *expected_value,
			    uint32_t timeout_ms)
{
	if (!path || !expected_value) {
		return -1;
	}

	struct timeval start_time, current_time;
	gettimeofday(&start_time, NULL);

	while (1) {
		// Check if the file has the expected value
		if (exp_flag_check_value(path, expected_value)) {
			return 0;
		}

		// Check timeout
		if (timeout_ms > 0) {
			gettimeofday(&current_time, NULL);
			uint32_t elapsed_ms =
				(current_time.tv_sec - start_time.tv_sec) *
					1000 +
				(current_time.tv_usec - start_time.tv_usec) /
					1000;

			if (elapsed_ms >= timeout_ms) {
				return -1; // Timeout
			}
		}

		// Small delay to avoid busy waiting
		usleep(10000); // 10ms delay
	}

	return -1;
}
