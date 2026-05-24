#!/bin/bash
# set -xve
TOTAL_HUGEMEM_SIZE=2048 # in MB.
SPDK_HUGENODE=${SPDK_HUGENODE:-1}

if [ -z "$OXBOW_ENV_SOURCED" ]; then
	echo "Do source set_env.sh first."
	exit
fi

printUsage() {
	echo "Usage: $(basename $0) [-r] [--host-journaling]
	without option : Set up SPDK.
        -r : reset.
        --host-journaling : Configuration for host journaling mode. It includes DevFS NVMe VF and uses 8192 MB hugepages."
}

RESET=""
HOST_JOURNALING=""

while [ "$#" -gt 0 ]; do
	case "$1" in
	-r)
		RESET="reset"
		;;
	--host-journaling)
		HOST_JOURNALING="true"
		TOTAL_HUGEMEM_SIZE=8192
		;;
	-h | --help)
		printUsage
		exit 0
		;;
	*)
		printUsage
		exit 2
		;;
	esac
	shift
done

# Set hugepages.
# Increase the number of hugepages.
# sudo -E bash -c "${SPDK}/dpdk/usertools/dpdk-hugepages.py -p 2048K -n ${NUMA_NODE} --setup 16G"

# Get pcie nvme addr. pcie_nvme_addr may contain one or more BDFs
# (whitespace-separated) to cover multiple SR-IOV VFs of the same NVMe
# namespace. SPDK's setup.sh natively supports a space-separated list
# in PCI_ALLOWED, but the /sys unbind sysfs only accepts one BDF per
# write, so we iterate.
source $SECURE_DAEMON/secure_daemon_conf.sh
secure_daemon_pcie_nvme_addr="$pcie_nvme_addr"

devfs_pcie_nvme_addr=""
if [ -n "$HOST_JOURNALING" ]; then
	source $DEVFS/devfs_conf.sh
	devfs_pcie_nvme_addr="$pcie_nvme_addr"
fi

pcie_nvme_addr="$secure_daemon_pcie_nvme_addr $devfs_pcie_nvme_addr"
# CMD="sudo PCI_ALLOWED=$pcie_nvme_addr HUGEMEM=16384 HUGE_EVEN_ALLOC=yes $SPDK/scripts/setup.sh $RESET"

# Sometimes nvme prevents spdk driver binding.
for addr in $pcie_nvme_addr; do
	# Some VFs have no nvme driver bound; ignore failure in that case.
	sudo bash -c "echo $addr > /sys/bus/pci/drivers/nvme/unbind" 2>/dev/null || true
done

# Keep SPDK DMA memory on the storage-local NUMA node. CLEAR_HUGE removes
# stale allocations on other nodes before SPDK setup allocates HUGENODE.
CMD=(sudo -E env "PCI_ALLOWED=$pcie_nvme_addr" "HUGEMEM=$TOTAL_HUGEMEM_SIZE" "HUGENODE=$SPDK_HUGENODE" "CLEAR_HUGE=yes" "$SPDK/scripts/setup.sh")
if [ -n "$RESET" ]; then
	CMD+=("$RESET")
fi
printf "%q " "${CMD[@]}"
echo
"${CMD[@]}"
