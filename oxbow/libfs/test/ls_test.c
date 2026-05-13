#include <stdio.h>
#include <stdlib.h>
#include <dirent.h>
#include <errno.h>
#include <string.h>

/* Simple ls-like utility for testing: prints directory entries. */
static void print_dir_entries(const char *dir_path)
{
	DIR *dir;
	struct dirent *entry;

	dir = opendir(dir_path);
	if (dir == NULL) {
		fprintf(stderr, "opendir failed for '%s': %s\n",
			dir_path, strerror(errno));
		exit(1);
	}

	while ((entry = readdir(dir)) != NULL) {
		printf("%s\n", entry->d_name);
	}

	if (closedir(dir) < 0) {
		fprintf(stderr, "closedir failed for '%s': %s\n",
			dir_path, strerror(errno));
		exit(1);
	}
}

int main(int argc, char *argv[])
{
	if (argc != 2) {
		fprintf(stderr, "Usage: %s <directory_path>\n", argv[0]);
		return 1;
	}

	print_dir_entries(argv[1]);

	return 0;
}



