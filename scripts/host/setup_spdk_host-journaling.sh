#!/bin/bash
# set -xve
# TOTAL_HUGEMEM_SIZE=16384 # in MB.
TOTAL_HUGEMEM_SIZE=12288 # in MB.
NUMA_NUM=$(lscpu | grep -i "NUMA node(s)" | awk '{print $NF}')

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

# Set hugepages.
# Increase the number of hugepages.
# sudo -E bash -c "${SPDK}/dpdk/usertools/dpdk-hugepages.py -p 2048K -n ${NUMA_NODE} --setup 16G"

# Get pcie nvme addr. Each of secure_daemon / devfs may set a
# whitespace-separated list of BDFs; we merge them and feed SPDK
# PCI_ALLOWED with the full list.
source $SECURE_DAEMON/secure_daemon_conf.sh
pcie_addr_1="$pcie_nvme_addr" ## Get secure_daemon address(es).

source $DEVFS/devfs_conf.sh
pcie_addr_2="$pcie_nvme_addr" ## Get devfs address.

# Sometimes nvme prevents spdk driver binding. Unbind every BDF
# individually because the sysfs unbind attribute accepts one entry
# per write.
for addr in $pcie_addr_1 $pcie_addr_2; do
	sudo bash -c "echo $addr > /sys/bus/pci/drivers/nvme/unbind" 2>/dev/null || true
done

for n in $(seq 0 $((NUMA_NUM - 1))); do
	CMD="sudo PCI_ALLOWED=\"$pcie_addr_1 $pcie_addr_2\" HUGEMEM=$TOTAL_HUGEMEM_SIZE HUGENODE=$n $SPDK/scripts/setup.sh $RESET"
	echo $CMD
	sudo -E bash -c "$CMD"
done
