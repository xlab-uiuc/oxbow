#!/bin/bash
set -euo pipefail

print_usage() {
	echo "Usage: $(basename "$0") <device_name>"
	echo "Example: $0 nvmeXnY"
	echo "         $0 /dev/nvmeXnY"
}

if [ "${1:-}" = "-h" ] || [ "${1:-}" = "--help" ]; then
	print_usage
	exit 0
fi

if [ -z "${OXBOW_ENV_SOURCED:-}" ]; then
	echo "Do source set_env.sh first."
	exit
fi

# Check if device name argument is provided
if [ -z "${1:-}" ]; then
	print_usage
	exit 1
fi

# Get pcie nvme addr.
source "${SECURE_DAEMON}/secure_daemon_conf.sh"

###### If the above parsing does not work, you can just simply set it MANUALLY.
nvme_dev_name="${1#/dev/}"

nvme_dev="/dev/$nvme_dev_name"

echo "Target device info: $nvme_dev"

# MKFS.
if [ "$filesystem" == "sefs" ]; then
	opts=""
	CMD="${SECURE_DAEMON}/build/mkfs.sefs $opts $nvme_dev"

elif [ "$filesystem" == "lwext4" ]; then
	opts="-b 4096 -i" # 4K block size and -i for device.

	### Currently, ext4 mkfs depends on many source codes of the secure daemon.
	### So, we include all the library that required to run Secure daemon here
	### just like run.sh does. (e.g. spdk)
	#
	# For SPDK shared library. Refer to SPDK README file.
	LIB_PATH_SPDK="$SPDK_INSTALL/lib/:$SPDK/dpdk/build/lib/"
	ldconfig -v -n "$SPDK_INSTALL/lib" >/dev/null

	# blocksize=4096
	CMD="LD_LIBRARY_PATH=$LIB_PATH_SPDK ${SECURE_DAEMON_BUILD}/mkfs.lwext4 -b 4096 -i $nvme_dev"

else
	echo "Unsupported filesystem: $filesystem"
	exit
fi

echo $CMD
sudo -E bash -c "$CMD"

echo "mkfs done."
