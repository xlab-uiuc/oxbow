#!/bin/bash

### For checking this file is sourced.
export OXBOW_ENV_SOURCED=1

### Set directory paths.
export OXBOW_ROOT=$PWD
export DEP_PKG=$OXBOW_ROOT/dep_pkgs
export SECURE_DAEMON=$OXBOW_ROOT/oxbow/secure_daemon
export SECURE_DAEMON_BUILD=$SECURE_DAEMON/build
export LIBFS=$OXBOW_ROOT/oxbow/libfs
export LIBFS_BUILD=$LIBFS/build
export DEVFS=$OXBOW_ROOT/oxbow/devfs
export DEVFS_BUILD=$DEVFS/build
export OXBOW_KERNEL=$OXBOW_ROOT/oxbow/linux-kernel
export SYSCALL_INTERCEPT_INSTALL=$LIBFS/lib/syscall_intercept/install
export SPDK=$LIBFS/lib/spdk
export SPDK_INSTALL=$SPDK/build
export TOOLS=$OXBOW_ROOT/tools
export BENCH=$OXBOW_ROOT/bench
export BENCH_MICRO=$BENCH/micro
export BENCH_FILEBENCH=$BENCH/filebench
export BENCH_LEVELDB=$BENCH/leveldb
export BENCH_YCSBCC=$BENCH/YCSB-CC
export BENCH_YCSBCPP=$BENCH/YCSB-cpp
export BENCH_UFS=$BENCH/uFS
export SCRIPTS=$OXBOW_ROOT/scripts

# nfs flags for experiments. It should be same as the one in include/common/oxbow.h.
export EXP_FLAG_DIR=/mnt/oxbow_flag
sudo -E bash -c "mkdir -p $EXP_FLAG_DIR; chown -R 777 $EXP_FLAG_DIR"

##### Host configuration #####
if [ ! $(uname -m) = "aarch64" ]; then

# export NVME_PCIE_ADDR='0000:00:04.0'
export NVME_PCIE_ADDR='0000:d8:00.0' # libra06
# export NVME_PCIE_ADDR_EXT4='0000:d8:00.3' # libra06 use VF
export NVME_PCIE_ADDR_EXT4='0000:d8:00.0' # libra06 use PF

### NVME_BIN can be set by env variable.
# Ex) NVME_BIN=/path/to/nvme/bin ./setup_vf.sh
#
if [ -z "${NVME_BIN:-}" ]; then
  # Install `nvme-cli` and set the proper bin path if `nvme` command is not found.
  if [ -x "$TOOLS/bin/nvme" ]; then
    NVME_BIN=$TOOLS/bin/nvme
  else
    NVME_BIN=$(command -v nvme || true)
  fi
  if [ -z "$NVME_BIN" ]; then
    NVME_BIN=$TOOLS/bin/nvme
  fi
fi

# The device in our testbed is Samsung PM1735. However, nvme list shows the
# ...1733... as its Subsystem-NQN.
export NVME_DEV_NAME=$(sudo $NVME_BIN list -v | grep $NVME_PCIE_ADDR | xargs | awk '{print $NF}')
export NVME_DEV_NAME_EXT4=$(sudo $NVME_BIN list -v | grep $NVME_PCIE_ADDR_EXT4 | xargs | awk '{print $NF}')
# The other solution with PCIe address.
# Or set it manually.
#NVME_DEV_NAME=nvme1

fi # aarch64

# Oxbow constants.
export OXBOW_PREFIX="/oxbow"

# Include package bin directory to PATH.
export PATH=$DEP_PKG/bin:$TOOLS/bin:$PATH

# Include mkfs for simplefs
export PATH=$OXBOW_ROOT/tools/mkfs:$PATH

# Only used for qemu setup (i.e., setup_spdk_qemu.sh)
export NUMA_NODE=0

SYS=$(gcc -dumpmachine)
if [ "$SYS" = "aarch64-linux-gnu" ]; then
	export PINNING="" # There is only one socket in the SmartNIC.
else
	# export PINNING="numactl -N${NUMA_NODE} -m${NUMA_NODE}" ## CHANGE THIS VALUE. [OXBOW_CONFIG_RUN]
	export PINNING="" # There is only one socket in the SmartNIC.
fi
