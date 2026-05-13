#!/bin/bash
set -e

if [ -z "$OXBOW_ENV_SOURCED" ]; then
	echo "Do source set_env.sh first."
	exit
fi

nvme_bin="${TOOLS}/bin/nvme"

# Get pcie nvme addr.
source "${SECURE_DAEMON}/secure_daemon_conf.sh"

# We need to use physical function(PF) because nvme driver is not activated with VF.
#   Workaround 1: mkfs with PF.
#   Another solution: make mkfs.FILESYSTEM do I/O with SPDK.
# Currently let's replace the last character of the PICE address with '0'. It is
# a resonable solution because the last character indicates namespace.
#
# pcie_nvme_addr may be a whitespace-separated list of VF BDFs. All VFs
# of an SR-IOV namespace share the same PF, so take the first entry and
# derive the PF from it.
first_vf_pcie_addr="${pcie_nvme_addr%% *}"
pf_pcie_nvme_addr="${first_vf_pcie_addr%.?}.0"
# echo "PF: $pf_pcie_nvme_addr"

# The device in our testbed is Samsung PM1735. However, nvme list shows the
# ...1733... as its Subsystem-NQN.
nvme_dev_name=$(sudo $nvme_bin list -v | grep $pf_pcie_nvme_addr | xargs | grep -E --only-matching "nvme.n.")
# echo "Dev Name: $nvme_dev_name"

###### If the above parsing does not work, you can just simply set it MANUALLY.
#nvme_dev_name=nvme2n1

nvme_dev="/dev/$nvme_dev_name"

echo "Target device info: $nvme_dev $pf_pcie_nvme_addr"

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
