#!/bin/bash
if [ -z "$OXBOW_ENV_SOURCED" ]; then
	echo "Do source set_env.sh first."
	exit
fi

printUsage() {
	echo "Usage: $(basename $0) -b BLOCK_INDEX [-r] [-s]
	-b BLOCK_INDEX : Specify the block index to dump (default: 0)
	-r : Bind SPDK after dumping
	-s : Unbind SPDK before dumping
	-h : Display this help message"
}

INDEX=0
UNBIND_SPDK=0
BIND_SPDK=0
DEV_PATH="/dev/nvmeXnY" # Set proper path.
PCIE_ADDR="0000:00:XX.Y" # Set proper pcie address.

while getopts "b:rs?h" opt; do
	case $opt in
	b)
		INDEX=$OPTARG
		;;
	r)
		BIND_SPDK=1
		;;
	s)
		UNBIND_SPDK=1
		;;
	h | ?)
		printUsage
		exit 2
		;;
	esac
done

if [ "$UNBIND_SPDK" -eq 1 ];then
	$SCRIPTS/host/setup_spdk.sh -r
fi

echo "Reading block $INDEX."

## Read to file.
sudo dd if=$DEV_PATH of=block.bin bs=4096 count=1 skip=$INDEX
# sudo dd if=$SECURE_DAEMON/lwext4.img of=block.bin bs=4096 count=1 skip=$INDEX

## dump to stdout.
# hexdump -C block.bin
sudo xxd block.bin

sudo rm -rf block.bin

if [ "$BIND_SPDK" -eq 1 ];then
	sudo bash -c "echo $PCIE_ADDR > /sys/bus/pci/drivers/nvme/unbind"
	$SCRIPTS/host/setup_spdk.sh
fi
