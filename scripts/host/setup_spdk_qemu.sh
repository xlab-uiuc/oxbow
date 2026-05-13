#!/bin/bash
set -euo pipefail

SECURE_SERIAL="oxbow-secure"
DEVFS_SERIAL="oxbow-devfs"
RESET=0
PCIE_ADDRS=()
STATE_FILE="/tmp/oxbow_qemu_nvme_bdfs"

print_usage() {
	echo "Usage: $(basename "$0") [--auto] [--reset|-r]
       $(basename "$0") [--pcie <bdf>]... [--reset|-r]"
}

if [ "${1:-}" = "-h" ] || [ "${1:-}" = "--help" ]; then
	print_usage
	exit 0
fi

if [ -z "${OXBOW_ENV_SOURCED:-}" ]; then
	echo "Do source set_env.sh first."
	exit 1
fi

while [ $# -gt 0 ]; do
	case "$1" in
	--auto)
		shift
		;;
	-r | --reset)
		RESET=1
		shift
		;;
	--pcie)
		PCIE_ADDRS+=("$2")
		shift 2
		;;
	-h | --help)
		print_usage
		exit 0
		;;
	*)
		print_usage
		exit 2
		;;
	esac
done

find_bdf_by_serial() {
	local wanted="$1"
	local serial

	for ctrl_path in /sys/class/nvme/nvme*; do
		[ -e "$ctrl_path/serial" ] || continue
		serial=$(tr -d '[:space:]' <"$ctrl_path/serial")
		if [ "$serial" = "$wanted" ]; then
			basename "$(readlink -f "$ctrl_path/device")"
			return 0
		fi
	done

	return 1
}

read_state_bdfs() {
	local bdf

	[ -f "$STATE_FILE" ] || return 1
	while read -r bdf; do
		[ -n "$bdf" ] || continue
		[ -e "/sys/bus/pci/devices/$bdf" ] || continue
		PCIE_ADDRS+=("$bdf")
	done <"$STATE_FILE"

	[ "${#PCIE_ADDRS[@]}" -gt 0 ]
}

write_state_bdfs() {
	printf "%s\n" "${PCIE_ADDRS[@]}" >"$STATE_FILE"
}

current_driver() {
	local bdf="$1"

	if [ -e "/sys/bus/pci/devices/$bdf/driver" ]; then
		basename "$(readlink -f "/sys/bus/pci/devices/$bdf/driver")"
	fi
}

unbind_current_driver() {
	local bdf="$1"
	local driver

	driver=$(current_driver "$bdf" || true)
	if [ -n "$driver" ]; then
		echo "$bdf" | sudo tee "/sys/bus/pci/drivers/$driver/unbind" >/dev/null
	fi
}

bind_to_driver() {
	local bdf="$1"
	local driver="$2"

	sudo modprobe "$driver"
	unbind_current_driver "$bdf"
	echo "$driver" | sudo tee "/sys/bus/pci/devices/$bdf/driver_override" >/dev/null
	echo "$bdf" | sudo tee "/sys/bus/pci/drivers/$driver/bind" >/dev/null
	echo "" | sudo tee "/sys/bus/pci/devices/$bdf/driver_override" >/dev/null
}

if [ "${#PCIE_ADDRS[@]}" -eq 0 ]; then
	if [ "$RESET" -eq 1 ] && read_state_bdfs; then
		:
	else
		secure_bdf=$(find_bdf_by_serial "$SECURE_SERIAL") || {
			echo "Cannot find QEMU NVMe controller with serial $SECURE_SERIAL"
			exit 1
		}
		devfs_bdf=$(find_bdf_by_serial "$DEVFS_SERIAL") || {
			echo "Cannot find QEMU NVMe controller with serial $DEVFS_SERIAL"
			exit 1
		}
		PCIE_ADDRS+=("$secure_bdf" "$devfs_bdf")
	fi
fi

write_state_bdfs

if [ "$RESET" -eq 1 ]; then
	for bdf in "${PCIE_ADDRS[@]}"; do
		echo "Bind $bdf to nvme"
		bind_to_driver "$bdf" nvme
	done
	exit 0
fi

sudo -E "$SPDK/dpdk/usertools/dpdk-hugepages.py" -p 2048K -n "$NUMA_NODE" --setup 8G

for bdf in "${PCIE_ADDRS[@]}"; do
	echo "Bind $bdf to uio_pci_generic"
	bind_to_driver "$bdf" uio_pci_generic
done
