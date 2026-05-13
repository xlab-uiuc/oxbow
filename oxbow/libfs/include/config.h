#ifndef _CONFIG_H_
#define _CONFIG_H_

struct libfs_config {
	int dirty_list_size; // TODO: Not used. to be deleted.
};

extern struct libfs_config g_libfs_conf;

void load_libfs_configs(void);
void print_libfs_configs(void);

#endif
