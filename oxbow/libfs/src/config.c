#include "config.h"
#include <stdio.h>
#include <stdlib.h>

#define LOAD_CONFIG_STR(var) g_libfs_conf.var = getenv(#var);
#define LOAD_CONFIG_INT(var) g_libfs_conf.var = get_val_from_env(#var);
#define PRINT_CONFIG_STR(var)                                                  \
	do {                                                                   \
		printf(#var "=%s\n", g_libfs_conf.var);                        \
	} while (0)
#define PRINT_CONFIG_INT(var)                                                  \
	do {                                                                   \
		printf(#var "=%d\n", g_libfs_conf.var);                        \
	} while (0)

struct libfs_config g_libfs_conf = { 0 };

static int get_val_from_env(char *conf_name)
{
	return getenv(conf_name) ? atoi(getenv(conf_name)) : 0; // default is 0
}

void load_libfs_configs(void)
{
	LOAD_CONFIG_INT(dirty_list_size);
	// LOAD_CONFIG_STR(XXX);
}

void print_libfs_configs(void)
{
	printf("---- LIBFS Configurations ----\n");
	PRINT_CONFIG_INT(dirty_list_size);
	// PRINT_CONFIG_STR(XXX);
	printf("--------------------------------------\n");
}
