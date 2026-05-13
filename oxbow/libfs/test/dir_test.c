#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>
#include <errno.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <string.h>
#include <dirent.h>
#include "test_global.h"

#define N_FILES 3
// #define N_FILES 900
#define SLEEP_TIME 1

#define TEST_DIR_PATH TEST_DIR "/test_dir"

void sync_dir(char *dir) {
	printf("sync_dir: %s\n", dir);

	printf("\tcall open(%s, O_DIRECTORY|O_RDONLY)\n", dir);
	int fd = open(dir, O_DIRECTORY | O_RDONLY);
	// int fd = open(dir, O_RDONLY);
	if (fd < 0) {
		perror("open failed.");
		exit(1);
	}
	printf("\topen succeeded: fd=%d\n", fd);
	printf("\tcall fsync(fd=%d)\n", fd);
	if (fsync(fd) < 0) {
		perror("fsync failed.");
		exit(1);
	}
	printf("\tfsync done\n");

	printf("\tcall close(fd=%d)\n", fd);
	if (close(fd) < 0) {
		perror("close failed.");
		exit(1);
	}
	printf("\tclose done\n");

	printf("sync_dir done\n");
}

void mk_dir(char *dir, mode_t mode) {
	int ret;

	printf("mkdir %s \n", dir);

	printf("call mkdir(%s, 0%o)\n", dir, mode);
	if ((ret = mkdir(dir, mode)) < 0) {
		if (errno == EEXIST)
			printf("directory already exists.\n");
		else {
			perror("mkdir failed.");
			exit(1);
		}
	}

	printf("mkdir succeeded\n");
	printf("mkdir done\n");
}

void unlink_file(const char *file_path) {

	printf("unlink %s\n", file_path);

	if (unlink(file_path) < 0) {
		perror("unlink");
		exit(1);
	}
	printf("unlink done\n");
}

void stat_file(const char *file_path)
{
	struct stat st;

	printf("lstat %s\n", file_path);
	if (lstat(file_path, &st) < 0) {
		perror("lstat failed");
		return;
	}

	printf("\tino=%lu mode=0%o size=%ld\n",
	       (unsigned long)st.st_ino,
	       st.st_mode,
	       (long)st.st_size);
}

void list_dir_entries(const char *dir_path) {
	printf("=== List directory entries: %s ===\n", dir_path);

	DIR *dir = opendir(dir_path);
	if (dir == NULL) {
		perror("opendir failed");
		exit(1);
	}

	struct dirent *entry;
	int count = 0;

	printf("Entries:\n");
	while ((entry = readdir(dir)) != NULL) {
		printf("\t%s\n", entry->d_name);
		count++;
	}

	if (closedir(dir) < 0) {
		perror("closedir failed");
		exit(1);
	}

	printf("Total entries: %d\n", count);
	printf("=== List directory entries done ===\n\n");
}


int main(void)
{
	printf("dir_test start in 2 seconds...\n");
	sleep(2);
	// struct stat buf;
	int i, ret = 0;

	/*
    if ((ret = lstat(TEST_DIR_PATH "/files/files1", &buf)) == 0) {
		printf("%s: inode number %lu", TEST_DIR_PATH "files/file1", buf.st_ino);
	}
	*/

	/*
    if ((ret = rmdir(TEST_DIR_PATH)) < 0 && errno != ENOENT) {
        perror("rmdir");
        exit(1);
    }
	*/

	// mkdir
	mk_dir(TEST_DIR_PATH, 0700);

	// Sync root directory.
	printf("=== Sync root directory: /oxbow/ ===\n");
	sync_dir("/oxbow/");

	// create a file.
	printf("creat " TEST_DIR_PATH "/file\n");
	// if ((ret = creat(TEST_DIR_PATH "/file", 0600)) < 0) {
	if ((ret = creat(TEST_DIR_PATH "/file", 0700)) < 0) {
		perror("open");
		exit(1);
	}
	printf("creat returned fd=%d\n", ret);
	printf("creat done\n\n");

	// Sync parent directory.
	printf("=== Sync parent directory: %s ===\n", TEST_DIR_PATH);
	sync_dir(TEST_DIR_PATH);

	// unlink the file.
	// unlink_file(TEST_DIR_PATH "/file");

	// mkdir another directory.
	mk_dir(TEST_DIR_PATH "/files", 0600);

	// Sync parent directory.
	printf("=== Sync parent directory: %s ===\n", TEST_DIR_PATH);
	sync_dir(TEST_DIR_PATH);


	// create multiple files.
	printf("create files\n");

	for (i = 0; i < N_FILES; i++) {
		char file_path[4096];
		memset(file_path, 0, 4096);

		sprintf(file_path, "%s%d", TEST_DIR_PATH "/files/file", i);
		printf("creat %s\n", file_path);
		if ((ret = creat(file_path, 0600)) < 0) {
			perror("open");
			exit(1);
		}
		printf("creat done: fd=%d\n", ret);
	}
	printf("create files done\n\n");

	// Sync files directory.
	printf("=== Sync files directory: %s/files ===\n", TEST_DIR_PATH);
	sync_dir(TEST_DIR_PATH "/files");

	// Stat each file directly. (Before unlink)
	printf("=== Stat files in directory: %s/files ===\n", TEST_DIR_PATH);
	for (i = 0; i < N_FILES; i++) {
		char file_path[4096];
		memset(file_path, 0, sizeof(file_path));

		sprintf(file_path, "%s%d", TEST_DIR_PATH "/files/file", i);
		stat_file(file_path);
	}
	printf("=== Stat files in directory done ===\n\n");

	// Unlink
	unlink_file(TEST_DIR_PATH "/files/file2");

	// Stat each file directly. (After unlink)
	printf("=== Stat files in directory: %s/files ===\n", TEST_DIR_PATH);
	for (i = 0; i < N_FILES; i++) {
		char file_path[4096];
		memset(file_path, 0, sizeof(file_path));

		sprintf(file_path, "%s%d", TEST_DIR_PATH "/files/file", i);
		stat_file(file_path);
	}
	printf("=== Stat files in directory done ===\n\n");

	// Create a file with the same name.
	printf("creat" TEST_DIR_PATH "/files/file2\n");
	if ((ret = creat(TEST_DIR_PATH "/files/file2", 0600)) < 0) {
		perror("open");
		exit(1);
	}
	printf("creat returned fd=%d\n", ret);
	printf("creat done\n");

	// Sync files directory.
	printf("=== Sync files directory: %s/files ===\n", TEST_DIR_PATH);
	sync_dir(TEST_DIR_PATH "/files");

	// Stat each file directly. (After recreate)
	printf("=== Stat files in directory: %s/files ===\n", TEST_DIR_PATH);
	for (i = 0; i < N_FILES; i++) {
		char file_path[4096];
		memset(file_path, 0, sizeof(file_path));

		sprintf(file_path, "%s%d", TEST_DIR_PATH "/files/file", i);
		stat_file(file_path);
	}
	printf("=== Stat files in directory done ===\n\n");

	// List directory entries.
	list_dir_entries(TEST_DIR_PATH);
	list_dir_entries(TEST_DIR_PATH "/files");

	printf("dir_test done.\n");

	printf("Exit in 3 seconds...\n");
	sleep(3);

	return 0;
}
