#!/bin/bash
set -e
if [ -z "$OXBOW_ENV_SOURCED" ]; then
	echo "Do source set_env.sh first."
	exit
fi

# Load configurations as environment variables.
source $DEVFS/devfs_conf.sh

if [ -n "$1" ] && [ "$1" = '-d' ]; then
	# You can pass the tty option to gdb. Ex) ./run.sh -d --tty /dev/ptx/X
	sudo -E bash -c "gdb --command=\"$DEVFS/gdb_scripts/devfs.gdb\" ${*:2} -ex run $DEVFS_BUILD/devfs"

	# To pass arguments to devfs.
	# sudo -E bash -c "gdb --command=\"$DEVFS/gdb_scripts/devfs.gdb\" --args $DEVFS_BUILD/devfs ${*:2}"

	# To call bin explicitly.
	#sudo -E bash -c "gdb --command=\"$DEVFS/gdb_scripts/devfs.gdb\" -ex \"tty /dev/pts/0\" -ex \"r\" --args ${*:2}"

elif [ -n "$1" ] && [ "$1" = '-r' ]; then # RUN OTHER TESTS.
	# For SPDK shared library. Refer to SPDK README file.
	LIB_PATH_SPDK="$SPDK_INSTALL/lib/:$SPDK/dpdk/build/lib/"
	ldconfig -v -n "$SPDK_INSTALL/lib" >/dev/null

	sudo -E bash -c "LD_LIBRARY_PATH=$LIB_PATH_SPDK $PINNING ${*:2}"
	# sudo -E bash -c "gdb --command=\"$DEVFS/gdb_scripts/devfs.gdb\" ${*:2}"

else

	# For SPDK shared library. Refer to SPDK README file.
	LIB_PATH_SPDK="$SPDK_INSTALL/lib/:$SPDK/dpdk/build/lib/"
	ldconfig -v -n "$SPDK_INSTALL/lib" >/dev/null

	sudo -E bash -c "LD_LIBRARY_PATH=$LIB_PATH_SPDK $PINNING $DEVFS_BUILD/devfs" # Run devfs.
	# sudo -E bash -c "LD_LIBRARY_PATH=$LIB_PATH_SPDK $PINNING $*" # To run other test files.
fi
