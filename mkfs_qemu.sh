#!/bin/bash
set -euo pipefail

SECURE_SERIAL="oxbow-secure"
DEVFS_SERIAL="oxbow-devfs"
SECURE_DEV=""
DEVFS_DEV=""
YES=0

print_usage() {
	echo "Usage: $(basename "$0") [--auto] [--yes]
       $(basename "$0") [--secure-dev <dev>] [--devfs-dev <dev>] [--yes]"
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
	--yes)
		YES=1
		shift
		;;
	--secure-dev)
		SECURE_DEV="${2#/dev/}"
		shift 2
		;;
	--devfs-dev)
		DEVFS_DEV="${2#/dev/}"
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

find_dev_by_serial() {
	local wanted="$1"
	local ctrl serial dev

	for ctrl_path in /sys/class/nvme/nvme*; do
		[ -e "$ctrl_path/serial" ] || continue
		serial=$(tr -d '[:space:]' <"$ctrl_path/serial")
		if [ "$serial" != "$wanted" ]; then
			continue
		fi

		ctrl=$(basename "$ctrl_path")
		for dev_path in /sys/block/"$ctrl"n*; do
			[ -e "$dev_path" ] || continue
			dev=$(basename "$dev_path")
			echo "$dev"
			return 0
		done
	done

	return 1
}

find_bdf_by_dev() {
	local dev="${1#/dev/}"
	local ctrl

	if [[ "$dev" =~ ^(nvme[0-9]+)n[0-9]+ ]]; then
		ctrl="${BASH_REMATCH[1]}"
	else
		echo "Invalid NVMe device: $dev" >&2
		return 1
	fi

	basename "$(readlink -f "/sys/class/nvme/$ctrl/device")"
}

if [ -z "$SECURE_DEV" ]; then
	SECURE_DEV=$(find_dev_by_serial "$SECURE_SERIAL") || {
		echo "Cannot find QEMU NVMe device with serial $SECURE_SERIAL"
		exit 1
	}
fi

if [ -z "$DEVFS_DEV" ]; then
	DEVFS_DEV=$(find_dev_by_serial "$DEVFS_SERIAL") || {
		echo "Cannot find QEMU NVMe device with serial $DEVFS_SERIAL"
		exit 1
	}
fi

SECURE_BDF=$(find_bdf_by_dev "$SECURE_DEV")
DEVFS_BDF=$(find_bdf_by_dev "$DEVFS_DEV")

echo "Secure Daemon device: /dev/$SECURE_DEV ($SECURE_BDF)"
echo "DevFS device:         /dev/$DEVFS_DEV ($DEVFS_BDF)"

if [ "$YES" -ne 1 ]; then
	echo "This will format both devices."
	read -r -p "Type 'yes' to continue: " ans
	if [ "$ans" != "yes" ]; then
		echo "Abort."
		exit 1
	fi
fi

scripts/host/setup_spdk_qemu.sh --reset --pcie "$SECURE_BDF" --pcie "$DEVFS_BDF"
scripts/host/mkfs_qemu.sh "$SECURE_DEV"
scripts/host/mkfs_qemu.sh "$DEVFS_DEV"
scripts/host/setup_spdk_qemu.sh --pcie "$SECURE_BDF" --pcie "$DEVFS_BDF"

echo "QEMU NVMe devices are formatted and bound for SPDK."
