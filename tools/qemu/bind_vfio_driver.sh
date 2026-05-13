#!/bin/bash
set -e
#
# Usage: ./bind_vfio_driver.sh <nvme_addr> [-r]
#  -r : reset (bind to nvme.)

if [ $# -lt 1 ]; then
	echo "Usage: ./bind_vfio_driver.sh <nvme_pci_addr> [-r]"
	echo "  -r : reset (bind to nvme dirver)."
	echo " Example: ./bind_vfio_driver.sh 0000:0X:00.0"
	echo "          ./bind_vfio_driver.sh 0000:0X:00.0 -r"
	exit
fi

pci_addr_full=$1
pci_addr=$(echo $pci_addr_full | cut -c 6-)

# Get vendor id and device code.
vendor_device=$(lspci -vvnn | grep $pci_addr | grep -oP "\[.{4}\:.{4}\]" | tr -d [ | tr -d ])
vendor_id=$(echo $vendor_device | cut -d ':' -f1)
device_id=$(echo $vendor_device | cut -d ':' -f2)

# echo $vendor_id $device_id

if [ "$2" = "-r" ]; then
	### Reset mode: bind to nvme driver.
	echo "Bind device $pci_addr_full to nvme driver."

	# Unbind from vfio-pci driver.
	sudo bash -c "echo $pci_addr_full > /sys/bus/pci/drivers/vfio-pci/unbind"

	# Bind to the nvme driver.
	sudo bash -c "echo $pci_addr_full > /sys/bus/pci/drivers/nvme/bind"

	## Another way.
	# sudo bash -c "echo 1 > /sys/bus/pci/devices/$pci_addr_full/remove"
	# sudo bash -c "echo 1 > /sys/bus/pci/rescan"
else
	### Bind to vfio-pci driver.
	echo "Bind device $pci_addr_full to vfio-pci driver"

	# Unbind from nvme driver.
	sudo bash -c "echo $pci_addr_full > /sys/bus/pci/drivers/nvme/unbind"

	# Bind to the vfio-pci driver.
	sudo bash -c "echo $vendor_id $device_id > /sys/bus/pci/drivers/vfio-pci/new_id"
	sudo bash -c "echo $pci_addr_full > /sys/bus/pci/drivers/vfio-pci/bind"

fi

