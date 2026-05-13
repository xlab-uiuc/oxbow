#include <stdio.h>
#include <unistd.h>
#include <string.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include "test_global.h"
#include "oxbow_libfs.h"

#define BUF_SIZE 4096
#define STR_SIZE 4083
#define TEST_FILE_NAME "file_basic.file"

#include <time.h>

int main()
{
	int fd1, fd2, fd3;
	int bytes, ret = 0;
	char buffer[BUF_SIZE], str[STR_SIZE];
	int write_count = 0;

	/*
		 ret = mkdir("/mlfs/", 0600);

		 if (ret < 0) {
		 perror("mkdir\n");
		 return 1;
		 }

		 printf("--- mkdir\n");

		 fd1 = creat("/mlfs/testfile", 0600);

		 if (fd1 < 0) {
		 perror("creat");
		 return 1;
		 }

		 write(fd1, "Hello World\n", 12);
		 close(fd1);

		 printf("--- creat/write/close\n");

		 fd2 = open("/mlfs/testfile", O_RDONLY, 0600);

		 if (fd2 < 0) {
		 perror("open without O_CREAT");
		 return 1;
		 }

		 bytes = read(fd2, buffer, BUF_SIZE);

		 if (bytes < 0) {
		 perror("read");
		 return 1;
		 }

		 buffer[11] = 0;
		 printf("read from file: %s\n", buffer);
		 close(fd2);
		 unlink("testfile");

		 printf("--- open(RDONLY)/read/unlink/close\n");
	*/

	printf("[File basic test] open the file\n");
	/* Create test file. */
	fd3 = open(TEST_DIR "/" TEST_FILE_NAME, O_RDWR | O_CREAT, 0600);

	if (fd3 < 0) {
		perror("open with O_CREAT");
		return 1;
	}

	printf("[File basic test] write '%s' the file\n", TEST_FILE_NAME);
	ret = write(fd3, TEST_FILE_NAME, 200);

	if (ret < 0) {
		perror(":write failed");
		return 1;
	}

	printf("[File basic test] close the file\n");

	ret = close(fd3);

	if (ret < 0) {
		perror("close failed");
		return 1;
	}

	printf("File_Basic is going to be paused for 3 seconds.\n");

	sleep(3);

	fd3 = open(TEST_DIR "/" TEST_FILE_NAME, O_RDWR | O_CREAT, 0600);

	if (fd3 < 0) {
		perror("open with O_CREAT");
		return 1;
	}

	bytes = read(fd3, buffer, BUF_SIZE);

	printf("Read data (%d bytes) from fd3(%d): %s\n", bytes, fd3, buffer);

	lseek(fd3, 0, SEEK_SET);

	if (bytes > 0) {
		sscanf(buffer, "%s %d\n", str, &write_count);
		memset(buffer, 0, BUF_SIZE);
		sprintf(buffer, "%s %d\n", str, ++write_count);
	} else {
		sprintf(buffer, "file-write-O_CREATE 0\n");
	}

	printf("new data: %s\n", buffer);

	write(fd3, buffer, BUF_SIZE);

	close(fd3);
	sleep(1);
	//unlink("testfile");

	printf("--- open(CREAT)/read/write/close\n");

	fd3 = open(TEST_DIR "/" TEST_FILE_NAME, O_RDWR, 0600);
	lseek(fd3, 0, SEEK_SET);

	sprintf(buffer, "This should be not printed\n");
	bytes = read(fd3, buffer, BUF_SIZE);
	printf("Read data (%d bytes) (in-memory) from fd3: %s\n", bytes,
	       buffer);
	close(fd3);

	printf("--- open(CREAT)/read again/close\n");

	return 0;
}
