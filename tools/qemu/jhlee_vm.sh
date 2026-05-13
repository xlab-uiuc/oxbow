#!/bin/bash
# set -x
QEMU_SSH_PORT=5555
QEMU_GDB_PORT=1235
TOTAL_MEM="80G"

# CONFIG: Set propler image files.
ROOT_IMG="/data/vm_imgs/ubuntu-2204.img"
DATA_IMG="/data/vm_imgs/data.img"

# Set NVME_PCIE_ADDR_FULL To use a passthrough device.
# Note that intel_iommu=on should be passed to host kernel parameter.
NVME_PCIE_ADDR_FULL="0000:86:00.0" # Set proper PCIE address for your NVMe device.
NVME_PCIE_ADDR="0000:86:00.0"
# NVME_PCIE_ADDR_FULL="0000:d8:00.0"
# NVME_PCIE_ADDR_FULL="0000:af:00.0"
# NVME_PCIE_ADDR_FULL="0000:04:00.0"
#if [ -n "$NVME_PCIE_ADDR_FULL" ];then
	# Bind device to vfio-pci driver.
	# To reset the device run: ./bind_vfio_driver.sh $NVME_PCIE_ADDR_FULL -r
#	./bind_vfio_driver.sh $NVME_PCIE_ADDR_FULL
#fi

# if [ -n "$NVME_PCIE_ADDR_FULL" ];then
# 	# Bind device to vfio-pci driver.
# 	# To reset the device run: ./bind_vfio_driver.sh $NVME_PCIE_ADDR_FULL -r
# 	./bind_vfio_driver.sh $NVME_PCIE_ADDR_FULL

# 	PASS_THROUGH_NVME=" -device vfio-pci,host=$NVME_PCIE_ADDR_FULL "
# else
# 	PASS_THROUGH_NVME=""
# fi

# To emulate NVMe device set NVME_IMG.
# NVME_IMG="/data/vm_imgs/nvme-dev.img"
NVME_IMG_1="/mnt/drive/nvme1.img"
NVME_IMG_2="/mnt/drive/nvme2.img"
if [ -n "$NVME_IMG_1" ];then
	# EMULATE_NVME_FILE=" -drive file=$NVME_IMG,if=none,id=nvm -device nvme,serial=deadbeef,drive=nvm "
	EMULATE_NVME_FILE=$(cat << EOF
-drive file=$NVME_IMG_1,format=raw,if=none,id=nvm1
-device nvme,drive=nvm1,serial=1234
-drive file=$NVME_IMG_2,format=raw,if=none,id=nvm2
-device nvme,drive=nvm2,serial=1235
EOF
)
else
	EMULATE_NVME_FILE=""
fi

# To use emulate NVMe device with a partition, set NVME_PART.
# NVME_PART_1="/dev/nvme2n1p1"
# NVME_PART_2="/dev/nvme2n1p2"
if [ -n "$NVME_PART_1" ];then
	EMULATE_NVME_PART=$(cat << EOF
-drive file=${NVME_PART_1},if=none,id=nvme1,format=raw
-device nvme,drive=nvme1,serial=nvme-disk1
-drive file=${NVME_PART_2},if=none,id=nvme2,format=raw
-device nvme,drive=nvme2,serial=nvme-disk2
EOF
)
else
	EMULATE_NVME_PART=""
fi

# To boot with kernel in the host. (FIXME Do we need initrd?)
# "-initrd /boot/initrd.img-6.2.10-Oxbow+ "
SIMPLE_KERNEL_TEST=""
# SIMPLE_KERNEL_TEST=" -kernel \"$OXBOW_KERNEL/arch/x86/boot/bzImage\" -append \"root=/dev/sda rw console=ttyS0 selinux=0\" "

# If you want kernel debug enable this
# GDB=""
GDB=" -gdb tcp::$QEMU_GDB_PORT "

# if [[ "${BASH_SOURCE[0]}" == "${0}" ]]; then # script is executed directly.
# 	qemu-system-x86_64 \
# 		-cpu host \
# 		-smp cpus=28 \
# 		-m "$TOTAL_MEM" \
# 		-drive file=$ROOT_IMG,index=0,media=disk,format=qcow2 \
# 		-drive file=$DATA_IMG,index=1,media=disk,format=qcow2 \
# 		$PASS_THROUGH_NVME \
# 		$EMULATE_NVME \
# 		-net nic -net user,hostfwd=tcp::$QEMU_SSH_PORT-:22 \
# 		--enable-kvm \
# 		--nographic \
# 		-serial mon:stdio \
# 		$GDB \
# 		# -append "root=/dev/sda rw console=ttyS0 single" \
# 		# -vnc :0 \
# 		# -hda vm_imgs/qemu-image.img \
# fi

if [[ "${BASH_SOURCE[0]}" == "${0}" ]]; then # script is executed directly.
	qemu-system-x86_64 -kernel "$OXBOW_KERNEL"/arch/x86/boot/bzImage \
		-initrd /boot/initrd.img-6.2.10-oxbow+ \
		--enable-kvm \
		-cpu host \
		-smp cpus=28 \
		-m "$TOTAL_MEM" \
		-drive file=vm_imgs/qemu-image.qcow2,index=0,media=disk,format=qcow2 \
		-device vfio-pci,host=$NVME_PCIE_ADDR \
		-append "root=/dev/sda rw console=ttyS0 selinux=0" \
		-net nic -net user,hostfwd=tcp::$QEMU_SSH_PORT-:22 \
		--enable-kvm \
		--nographic \
		-serial mon:stdio \
		$GDB \
		# -append "root=/dev/sda rw console=ttyS0 single" \
		# -vnc :0 \
		# -hda vm_imgs/qemu-image.img \
		# -device intel-iommu \ # To use SPDK in guest OS.
fi

