#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>
#include <errno.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <string.h>
#include "test_global.h"

#define N_FILES 1000

int main(int argc, char **argv)
{
	struct stat buf;
	int i, ret = 0;

	/*
    if ((ret = lstat(TEST_DIR "/files/files1", &buf)) == 0) {
		printf("%s: inode number %lu", TEST_DIR "files/file1", buf.st_ino);
	}
	*/

	/*
    if ((ret = rmdir(TEST_DIR)) < 0 && errno != ENOENT) {
        perror("rmdir");
        exit(1);
    }
	*/

	if ((ret = mkdir(TEST_DIR, 0700)) < 0) {
		perror("mkdir");
		exit(1);
	}

	if ((ret = creat(TEST_DIR "/file", 0600)) < 0) {
		perror("open");
		exit(1);
	}

	if ((ret = unlink(TEST_DIR "/file")) < 0) {
		perror("unlink");
		exit(1);
	}

	if ((ret = mkdir(TEST_DIR "/files", 0600)) < 0) {
		perror("open");
		exit(1);
	}

	for (i = 0; i < N_FILES; i++) {
		char file_path[4096];
		memset(file_path, 0, 4096);

		sprintf(file_path, "%s%d", TEST_DIR "/files/file", i);
		if ((ret = creat(file_path, 0600)) < 0) {
			perror("open");
			exit(1);
		}
	}

	if ((ret = unlink(TEST_DIR "/files/file2")) < 0) {
		perror("unlink");
		exit(1);
	}

	if ((ret = creat(TEST_DIR "/files/file2", 0600)) < 0) {
		perror("open");
		exit(1);
	}

	return 0;
}
