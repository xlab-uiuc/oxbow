#!/bin/bash
# set -xv
#NVME_PCIE_ADDR="" ### Set proper PCIe address for your NVMe device. [OXBOW_CONFIG_HW]
NVME_PCIE_ADDR="0000:d8:00.1" ### Set proper PCIe address for your NVMe device. [OXBOW_CONFIG_HW]

# Launch gdb.
if [ -n "$1" ] && [ "$1" = '-d' ]; then
	sudo gdb --command="$DEVFS/gdb_scripts/mkfs_journal.gdb" --args "$DEVFS_BUILD"/mkfs_journal -D "$NVME_PCIE_ADDR"
else
	# For SPDK shared library. Refer to SPDK README file.
	LIB_PATH_SPDK="$SPDK_INSTALL/lib/:$SPDK/dpdk/build/lib/"
	ldconfig -v -n "$SPDK_INSTALL/lib" &>/dev/null
	sudo LD_LIBRARY_PATH="$LIB_PATH_SPDK" $PINNING "$DEVFS_BUILD"/mkfs_journal -D "$NVME_PCIE_ADDR"
fi
