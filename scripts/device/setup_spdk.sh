#!/bin/bash
# set -xve

if [ -z "$OXBOW_ENV_SOURCED" ]; then
	echo "Do source set_env.sh first."
	exit
fi

printUsage() {
	echo "Usage: $(basename $0) [-r]
	without option : Set up SPDK.
        -r : reset."
}

RESET=""

while getopts "r?h" opt; do
	case $opt in
	r)
		RESET="reset"
		;;
	h | ?)
		printUsage
		exit 2
		;;
	esac
done

# Increase the number of hugepages. (Should be enough to support data fetcher buffer. Cf. oxbow.h)
sudo -E bash -c "${SPDK}/dpdk/usertools/dpdk-hugepages.py -p 2048K --setup 6G"

# Get pcie nvme addr.
# There is only one nvme-of device.
CMD="sudo $SPDK/scripts/setup.sh $RESET"
echo $CMD
sudo -E bash -c "$CMD"
