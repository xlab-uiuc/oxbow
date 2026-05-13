#!/bin/bash
# Script to configure NVMe-oF target server.
#set -xe
set -e

if [ -z "$OXBOW_ENV_SOURCED" ]; then
	echo "Do source set_env.sh first."
	exit
fi

### You may want to change the followings.
HW_OFFLOAD=1 # NVMe-oF offload to RDMA NIC.
NVME_SUBSYSTEM_NAME="oxbow-nvmf"
NAMESPACE="1"
PORT_DIR="1"
PORT_NUM="4420"

# Set proper path to nvme-cli.
NVME_BIN="$TOOLS/bin/nvme" # Use binary in this project.
#NVME_BIN="nvme" # If nvme-cli is installed.

# Set manually.
# RDMA_IP_ADDR="192.168.xx.xxx" # Target server IP (Host IP).
# Or,
# Read from devfs_conf.sh file.
RDMA_IP_ADDR=$(
	source $DEVFS/devfs_conf.sh
	echo "$nvmf_ip_addr"
)

### Path variable. Do not modify it.
SUBSYSTEM_PATH="/sys/kernel/config/nvmet/subsystems/$NVME_SUBSYSTEM_NAME"
NAMESPACE_PATH="$SUBSYSTEM_PATH/namespaces/$NAMESPACE"
PORT_PATH="/sys/kernel/config/nvmet/ports/$PORT_DIR"

DESTROY=0

print_usage() {
	echo "Usage: $0 [-d]
	without option : Setup nvme-of target.
        -d : Destroy."
}

if [[ "${BASH_SOURCE[0]}" == "${0}" ]]; then # script is executed directly.

	while getopts "d?h" opt; do
		case $opt in
		d)
			DESTROY=1
			;;
		h | ?)
			print_usage
			exit 2
			;;
		esac
	done

	if [[ $DESTROY = 1 ]]; then

		# Delete port.
		sudo rm $PORT_PATH/subsystems/$NVME_SUBSYSTEM_NAME
		sudo rmdir $PORT_PATH

		# Disable and delete namespace
		sudo bash -c "echo 0 > $NAMESPACE_PATH/enable"
		sudo rmdir $NAMESPACE_PATH

		# Delete subsystem.
		sudo rmdir $SUBSYSTEM_PATH
		exit
	fi

	if [[ $HW_OFFLOAD = 1 ]]; then
		# Load kernel modules.
		if sudo lsmod | grep -q nvme; then
			echo "nvme module is loaded. Reload it with a parameter."
			sudo modprobe -rv nvmet_rdma
			sudo modprobe -rv nvmet
			sudo modprobe -rv nvme # To set num_p2p_queues
		else
			echo "nvme module is not loaded."
		fi

		sudo modprobe nvme num_p2p_queues=2 # We only have one nvmf process: DevFS.
		sudo modprobe nvmet
		# sudo modprobe nvmet-rdma

		# Reserve memory for buffer to get better performance.
		#
		# You need to set mem=XXXM kernel parameter to limit the memory
		# that kernel uses. For example, kernel uses upto 78GB
		# (mem=79872M) and nvmf uses from 80GB (0x1400000000).
		#
		# Another example, kernel uses upto 172 GB (mem=176128M) and
		# nvmf uses from 174 GB (0x2b80000000).
		#
		# Another example, kernel uses upto 104 GB (mem=106496M) and
		# nvmf uses from 106 GB (0x1a80000000).
		#
		# Allocates <offload_buffer_size> for each offload context
		# (total chunks N=<offload_mem_size>/<offload_buffer_size>).
		# Here, 16 GB in total. (1GB for each chunck and there are 16 chuncks.)
		# sudo modprobe nvmet-rdma offload_mem_start=0x2b80000000 offload_mem_size=16384 offload_buffer_size=1024
		# sudo modprobe nvmet-rdma offload_mem_start=0x1a80000000 offload_mem_size=16384 offload_buffer_size=1024
		sudo modprobe nvmet-rdma offload_mem_start=0x1400000000 offload_mem_size=16384 offload_buffer_size=1024
		# sudo modprobe nvmet-rdma offload_mem_start=0x1400000000 offload_mem_size=2048 offload_buffer_size=512

		# Create nvmet-rdma subsystem.
		sudo mkdir $SUBSYSTEM_PATH
		sudo bash -c "echo 1 > $SUBSYSTEM_PATH/attr_allow_any_host"

		# Make subsystem to offload subsystem.
		sudo bash -c "echo 1 > $SUBSYSTEM_PATH/attr_offload"

	else
		# Load kernel modules.
		sudo modprobe nvmet
		sudo modprobe nvmet-rdma

		# Create nvmet-rdma subsystem.
		sudo mkdir $SUBSYSTEM_PATH
		sudo bash -c "echo 1 > $SUBSYSTEM_PATH/attr_allow_any_host"

	fi

	# Set manually.
	# NVME_DEV="/dev/nvme1n1"
	# Or,
	# Parse it.
	# The device in our testbed is Samsung PM1735. However, nvme list shows the
	# ...1733... as its Subsystem-NQN.
	temp=$(sudo $NVME_BIN list -v | grep 1733 | xargs | cut -d ' ' -f 3)
	NVME_DEV=$(sudo $NVME_BIN list -v | grep $temp | grep dev | xargs | cut -d ' ' -f 1)
	echo "NVMe device name: $NVME_DEV"

	# Create namespace and enable it.
	sudo mkdir $NAMESPACE_PATH
	sudo bash -c "echo -n $NVME_DEV > $NAMESPACE_PATH/device_path"
	sudo bash -c "echo 1 > $NAMESPACE_PATH/enable"

	# Create port and configure it.
	sudo mkdir $PORT_PATH
	sudo bash -c "echo $RDMA_IP_ADDR > $PORT_PATH/addr_traddr"
	sudo bash -c "echo rdma > $PORT_PATH/addr_trtype"
	sudo bash -c "echo $PORT_NUM > $PORT_PATH/addr_trsvcid"
	sudo bash -c "echo ipv4 > $PORT_PATH/addr_adrfam"

	# Create link.
	sudo ln -s $SUBSYSTEM_PATH $PORT_PATH/subsystems/$NVME_SUBSYSTEM_NAME

	echo 'Check dmesg output: dmesg | grep "enabling port"'
	sudo dmesg | grep "enabling port"

fi
