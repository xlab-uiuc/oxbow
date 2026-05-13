#!/bin/bash
# Script to configure client (NVMe-oF initiator).
#set -xe
set -e
source $SCRIPTS/host/nvme-of/config_nvmf_server.sh

DISCONNECT=0
# DEV_PATH=""
LIST=0

print_usage() {
	echo "Usage: $0 [-d | -l]
	without option : Connect.
        -d : Disconnect the device.
	-l : List."
	# echo "Usage: $0 [-d <nvme device path> | -l]
	# -d <nvme device path> : Disconnect the device. (ex: /dev/nvme0n1)
}

if [[ "${BASH_SOURCE[0]}" == "${0}" ]]; then # script is executed directly.

	while getopts "dl?h" opt; do
		case $opt in
		d)
			DISCONNECT=1
			# DEV_PATH="$OPTARG"
			;;
		l)
			LIST=1
			;;
		h | ?)
			print_usage
			exit 2
			;;
		esac
	done

	# Load kernel module.
	sudo modprobe nvme # For NVMe-oF target offloading.
	sudo modprobe nvme-rdma

	if [ $DISCONNECT = 1 ]; then
		CMD="sudo $NVME_BIN disconnect -n $NVME_SUBSYSTEM_NAME"
		# sudo nvme disconnect -d "$DEV_PATH"  # Not working in our testbed.
		echo $CMD
		$CMD
		exit

	elif [ $LIST = 1 ]; then
		CMD="sudo $NVME_BIN discover -t rdma -a $RDMA_IP_ADDR -s $PORT_NUM"
		echo $CMD
		$CMD
		exit

	else
		# Connect to the target server.
		CMD="sudo $NVME_BIN connect -t rdma -n $NVME_SUBSYSTEM_NAME -a $RDMA_IP_ADDR -s $PORT_NUM"
		echo $CMD
		$CMD

		# Confirm the connection.
		lsblk
	fi
fi
