#include <stdio.h>
#include <stdlib.h>
#include "global.h"

void _panic()
{
	fflush(stdout);
	fflush(stderr);

	exit(-1);
}
