#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>
#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <stdint.h>

#define COPY_BUFSIZE 4096

static void
usage(const char *prog)
{
	fprintf(stderr, "Usage: %s <src_path> <dst_path>\n", prog);
	fprintf(stderr, "  src_path: file to read from (e.g., /oxbow/testfile)\n");
	fprintf(stderr, "  dst_path: path under /tmp to dump file contents\n");
}

static int
dump_file(const char *src_path, const char *dst_path)
{
	int src_fd = -1;
	int dst_fd = -1;
	char *buf = NULL;
	ssize_t nread;
	off_t total_written = 0;
	int ret = -1;

	printf("dump_file: src=%s dst=%s\n", src_path, dst_path);

	printf("  open src: open(%s, O_RDONLY)\n", src_path);
	src_fd = open(src_path, O_RDONLY);
	if (src_fd < 0) {
		perror("open src failed");
		goto out;
	}
	printf("  open src done: fd=%d\n", src_fd);

	printf("  open dst: open(%s, O_WRONLY|O_CREAT|O_TRUNC, 0666)\n",
	    dst_path);
	dst_fd = open(dst_path, O_WRONLY | O_CREAT | O_TRUNC, 0666);
	if (dst_fd < 0) {
		perror("open dst failed");
		goto out;
	}
	printf("  open dst done: fd=%d\n", dst_fd);

	buf = malloc(COPY_BUFSIZE);
	if (buf == NULL) {
		perror("malloc failed");
		goto out;
	}

	printf("  start copy loop (buffer size=%d bytes)\n", COPY_BUFSIZE);
	for (;;) {
		nread = read(src_fd, buf, COPY_BUFSIZE);
		if (nread == 0) {
			/* EOF */
			break;
		}
		if (nread < 0) {
			if (errno == EINTR)
				continue;
			perror("read failed");
			goto out;
		}

		ssize_t nwritten_total = 0;
		while (nwritten_total < nread) {
			ssize_t nwritten;

			nwritten = write(dst_fd, buf + nwritten_total,
			    (size_t)(nread - nwritten_total));
			if (nwritten < 0) {
				if (errno == EINTR)
					continue;
				perror("write failed");
				goto out;
			}
			nwritten_total += nwritten;
			total_written += nwritten;
		}
	}

	printf("  total bytes written: %jd\n", (intmax_t)total_written);

	printf("  fsync dst: fsync(fd=%d)\n", dst_fd);
	if (fsync(dst_fd) < 0) {
		perror("fsync failed");
		goto out;
	}

	ret = 0;

out:
	if (buf != NULL)
		free(buf);

	if (dst_fd >= 0) {
		printf("  close dst: close(fd=%d)\n", dst_fd);
		if (close(dst_fd) < 0) {
			perror("close dst failed");
			ret = -1;
		}
	}

	if (src_fd >= 0) {
		printf("  close src: close(fd=%d)\n", src_fd);
		if (close(src_fd) < 0) {
			perror("close src failed");
			ret = -1;
		}
	}

	if (ret == 0)
		printf("dump_file done successfully\n");
	else
		printf("dump_file failed\n");

	return ret;
}

int
main(int argc, char *argv[])
{
	const char *src_path;
	const char *dst_path;
	int ret;

	if (argc != 3) {
		usage(argv[0]);
		return 1;
	}

	src_path = argv[1];
	dst_path = argv[2];

	printf("dump_file_test start\n");

	ret = dump_file(src_path, dst_path);

	printf("dump_file_test done (ret=%d)\n", ret);

	return (ret == 0) ? 0 : 1;
}


