#!/bin/bash
set -euo pipefail

SCRIPT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
VM_DIR="${OXBOW_AE_VM_DIR:-$HOME/oxbow/oxbow-ae-vm}"
DAEMONIZE=0
STOP=0
USE_EMULATED_NVME=1
PASSTHROUGH_BDFS=()
QEMU_GDB_PORT="${QEMU_GDB_PORT:-1235}"

print_usage() {
	echo "Usage: $(basename "$0") [--dir <vm_dir>] [--daemonize] [--stop]
       $(basename "$0") [--dir <vm_dir>] [--passthrough <bdf>] [--no-emulated-nvme]"
}

load_vm_env() {
	local env_file="$VM_DIR/vm.env"

	if [ ! -f "$env_file" ]; then
		echo "$env_file does not exist. Run tools/qemu/create_vm.sh first."
		exit 1
	fi

	# shellcheck disable=SC1090
	source "$env_file"
}

stop_vm() {
	local pidfile="$VM_DIR/qemu.pid"

	if [ ! -f "$pidfile" ]; then
		echo "No pidfile: $pidfile"
		exit 0
	fi

	local pid
	pid=$(cat "$pidfile")
	if kill -0 "$pid" >/dev/null 2>&1; then
		kill "$pid"
		echo "Stopped QEMU pid $pid"
	else
		echo "Removing stale pidfile: $pidfile"
	fi
	rm -f "$pidfile"
}

while [ $# -gt 0 ]; do
	case "$1" in
	--dir)
		VM_DIR="$2"
		shift 2
		;;
	--daemonize)
		DAEMONIZE=1
		shift
		;;
	--stop)
		STOP=1
		shift
		;;
	--passthrough)
		PASSTHROUGH_BDFS+=("$2")
		shift 2
		;;
	--no-emulated-nvme)
		USE_EMULATED_NVME=0
		shift
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

VM_DIR="${VM_DIR%/}"
load_vm_env

if [ "$STOP" -eq 1 ]; then
	stop_vm
	exit 0
fi

if [ ! -e /dev/kvm ] || [ ! -r /dev/kvm ] || [ ! -w /dev/kvm ]; then
	echo "Cannot access /dev/kvm. Add the current user to the kvm group and log in again."
	exit 1
fi

PIDFILE="$VM_DIR/qemu.pid"
SERIAL_LOG="$VM_DIR/qemu-serial.log"

if [ -f "$PIDFILE" ]; then
	old_pid=$(cat "$PIDFILE")
	if kill -0 "$old_pid" >/dev/null 2>&1; then
		echo "QEMU is already running with pid $old_pid"
		exit 1
	fi
	rm -f "$PIDFILE"
fi

cmd=(
	qemu-system-x86_64
	-name oxbow-ae-vm
	-machine q35,accel=kvm
	-cpu host
	-smp "$QEMU_CPUS"
	-m "$QEMU_MEM"
	-drive "file=$ROOT_IMG,format=qcow2,if=virtio"
	-drive "file=$SEED_ISO,format=raw,if=virtio,media=cdrom,readonly=on"
	-nic "user,model=virtio-net-pci,hostfwd=tcp:127.0.0.1:$SSH_PORT-:22"
	-display none
	-pidfile "$PIDFILE"
)

if [ "$USE_EMULATED_NVME" -eq 1 ]; then
	cmd+=(
		-drive "file=$NVME_SECURE_IMG,format=raw,if=none,id=nvme_secure,cache=none,aio=threads"
		-device "nvme,drive=nvme_secure,serial=$SECURE_NVME_SERIAL"
		-drive "file=$NVME_DEVFS_IMG,format=raw,if=none,id=nvme_devfs,cache=none,aio=threads"
		-device "nvme,drive=nvme_devfs,serial=$DEVFS_NVME_SERIAL"
	)
fi

for bdf in "${PASSTHROUGH_BDFS[@]}"; do
	"$SCRIPT_DIR/bind_vfio_driver.sh" "$bdf"
	cmd+=(-device "vfio-pci,host=$bdf")
done

if [ "$DAEMONIZE" -eq 1 ]; then
	cmd+=(-serial "file:$SERIAL_LOG" -monitor none -daemonize)
else
	cmd+=(-serial mon:stdio)
fi

echo "SSH port: $SSH_PORT"
echo "Starting QEMU..."
exec "${cmd[@]}"
