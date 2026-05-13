#!/bin/bash
# set -xve
set -e
if [ -z "$OXBOW_ENV_SOURCED" ]; then
	echo "Do source set_env.sh first."
	exit
fi

# Load configurations as environment variables.
source $SECURE_DAEMON/secure_daemon_conf.sh

mount() {
	sudo mount -t illufs dummy "$1"
}

umount() {
	sudo umount "$1"
}

if [ "$1" != '-k' ]; then
	# Clear shmem files except secure-daemon and devfs connection (oxbow_df_shm)
	echo "Clear files in /dev/shm/ except oxbow_df_shm."
	sudo find /dev/shm/ -mindepth 1 -exec rm -rf {} +
fi

if [ -n "$1" ] && [ "$1" = '-d' ]; then
	# You can pass the tty option to gdb. Ex) ./run.sh -d --tty /dev/ptx/X
	sudo -E bash -c "gdb --command=\"$SECURE_DAEMON/gdb_scripts/secure_daemon.gdb\" -ex run ${*:2} $SECURE_DAEMON_BUILD/secure_daemon"
	# sudo -E bash -c "gdb --command=\"$SECURE_DAEMON/gdb_scripts/secure_daemon.gdb\" ${*:2} $SECURE_DAEMON_BUILD/secure_daemon"
	# sudo -E bash -c "gdb --command=\"$SECURE_DAEMON/gdb_scripts/secure_daemon.gdb\" -ex run ${*:2} --args $SECURE_DAEMON_BUILD/mkfs.lwext4 -b 4096 -i lwext4.img" #### Warning!!!! data can be lost!!!!
	# sudo -E bash -c "gdb --command=\"$SECURE_DAEMON/gdb_scripts/secure_daemon.gdb\" -ex run ${*:2} --args $SECURE_DAEMON_BUILD/mkfs.lwext4 -b 4096 -i /dev/nvmeXnY" #### Warning!!!! data can be lost!!!!

	# To pass arguments to secure daemon.
	# sudo -E bash -c "gdb --command=\"$SECURE_DAEMON/gdb_scripts/secure_daemon.gdb\" --args $SECURE_DAEMON_BUILD/secure_daemon ${*:2}"

	# To call secure daemon bin explicitly.
	# sudo -E bash -c "gdb --command=\"$SECURE_DAEMON/gdb_scripts/secure_daemon.gdb\" --args ${*:2}"

# Background mode
elif [ -n "$1" ] && [ "$1" = '-b' ]; then # BACKGROUND
	echo Background mode
	# For SPDK shared library. Refer to SPDK README file.
	LIB_PATH_SPDK="$SPDK_INSTALL/lib/:$SPDK/dpdk/build/lib/"
	ldconfig -v -n "$SPDK_INSTALL/lib" >/dev/null

	# sudo -E bash -c "LD_LIBRARY_PATH=$LIB_PATH_SPDK $PINNING $SECURE_DAEMON_BUILD/secure_daemon"
	sudo -E bash -c "LD_LIBRARY_PATH=$LIB_PATH_SPDK $PINNING $SECURE_DAEMON_BUILD/secure_daemon &> /tmp/oxb_daemon.log &"

elif [ -n "$1" ] && [ "$1" = '-r' ]; then # RUN OTHER TESTS.
	# For SPDK shared library. Refer to SPDK README file.
	LIB_PATH_SPDK="$SPDK_INSTALL/lib/:$SPDK/dpdk/build/lib/"
	ldconfig -v -n "$SPDK_INSTALL/lib" >/dev/null
	sudo -E bash -c "LD_LIBRARY_PATH=$LIB_PATH_SPDK $PINNING ${*:2}"
	# sudo -E bash -c "gdb --command=\"$SECURE_DAEMON/gdb_scripts/secure_daemon.gdb\" ${*:2}"

elif [ -n "$1" ] && [ "$1" = '-k' ]; then # KILL
	# Kill
	sudo pkill -9 secure_daemon
	sleep 2
	sudo -E bash -c "umount $OXBOW_PREFIX"

else
	# For SPDK shared library. Refer to SPDK README file.
	LIB_PATH_SPDK="$SPDK_INSTALL/lib/:$SPDK/dpdk/build/lib/"
	ldconfig -v -n "$SPDK_INSTALL/lib" >/dev/null

	sudo -E bash -c "LD_LIBRARY_PATH=$LIB_PATH_SPDK $PINNING $SECURE_DAEMON_BUILD/secure_daemon"
	# sudo -E bash -c "LD_LIBRARY_PATH=$LIB_PATH_SPDK $PINNING $*"

fi
