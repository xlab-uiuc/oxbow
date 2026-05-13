#!/bin/bash
set -xve
(cd $SECURE_DAEMON && ./build.sh re) &&
	(cd $LIBFS && ./build.sh re) &&
	(cd $DEVFS && ./build.sh re)
