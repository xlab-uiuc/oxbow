#!/bin/bash
set -e
if [ -z "$OXBOW_ENV_SOURCED" ]; then
        echo "Do source set_env.sh first."
        exit
fi

# GDB_TTY="--tty /dev/pts/X"
GDB_TTY=""

# For SPDK shared library. Refer to SPDK README file.
#LIB_PATH_SPDK="$SPDK_INSTALL/lib/:$SPDK/dpdk/build/lib/"
#ldconfig -v -n "$SPDK_INSTALL/lib" > /dev/null
#LD_LIBRARY_PATH=$LIB_PATH_SPDK LD_PRELOAD=$LIBFS_BUILD/liboxbow_libfs.so $PINNING "$@"

# Load configurations as environment variables.
source $LIBFS/libfs_conf.sh

if [ -n "$1" ] && [ "$1" = '-d' ]; then # gdb.
        # sudo -E bash -c "gdb --command=\"$LIBFS/gdb_scripts/libfs.gdb\" -ex run --args ${*:2}"
        sudo -E bash -c "gdb --command=\"$LIBFS/gdb_scripts/libfs.gdb\" $GDB_TTY -ex run --args ${*:2}"
        # sudo -E bash -c "gdb --command=\"$LIBFS/gdb_scripts/libfs.gdb\" $GDB_TTY ${*:2}"

elif [ -n "$1" ] && [ "$1" = '-v' ]; then # valgrind.
        sudo -E bash -c "LD_PRELOAD=$LIBFS_BUILD/liboxbow_libfs.so valgrind --leak-check=full --track-origins=yes $*"

elif [ -n "$1" ] && [ "$1" = '-n' ]; then # niceness
        echo "niceness mode"
        sudo -E bash -c "LD_PRELOAD=$LIBFS_BUILD/liboxbow_libfs.so nice -n -20 $*"

elif [ -n "$1" ] && [ "$1" = '-m' ]; then # mkfs.

        # For SPDK shared library. Refer to SPDK README file.
        LIB_PATH_SPDK="$SPDK_INSTALL/lib/:$SPDK/dpdk/build/lib/"
        ldconfig -v -n "$SPDK_INSTALL/lib" >/dev/null

        # sudo -E bash -c "LD_LIBRARY_PATH=$LIB_PATH_SPDK LD_PRELOAD=$LIBFS_BUILD/liboxbow_libfs.so $PINNING $*"
        sudo -E bash -c "LD_LIBRARY_PATH=$LIB_PATH_SPDK LD_PRELOAD=$LIBFS_BUILD/liboxbow_libfs.so $*"
else
        sudo -E bash -c "LD_PRELOAD=$LIBFS_BUILD/liboxbow_libfs.so $PINNING $*"
fi
