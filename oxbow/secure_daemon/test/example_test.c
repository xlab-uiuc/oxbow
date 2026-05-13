#include "config.h"
#include "test_global.h"
#include <stdio.h>

// int main(int argc, char **argv)
int main()
{
	load_secure_daemon_configs();
	print_secure_daemon_configs();
}