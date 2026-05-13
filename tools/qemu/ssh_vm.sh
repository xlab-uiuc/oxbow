#!/bin/bash
set -euo pipefail

VM_DIR="${OXBOW_AE_VM_DIR:-$HOME/oxbow/oxbow-ae-vm}"

print_usage() {
	echo "Usage: $(basename "$0") [--dir <vm_dir>]"
}

while [ $# -gt 0 ]; do
	case "$1" in
	--dir)
		VM_DIR="$2"
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

VM_DIR="${VM_DIR%/}"
VM_ENV="$VM_DIR/vm.env"
if [ ! -f "$VM_ENV" ]; then
	echo "$VM_ENV does not exist. Run tools/qemu/create_vm.sh first."
	exit 1
fi

# shellcheck disable=SC1090
source "$VM_ENV"

exec ssh \
	-o StrictHostKeyChecking=no \
	-o UserKnownHostsFile="$VM_DIR/known_hosts" \
	-i "$SSH_KEY" \
	-p "$SSH_PORT" \
	"$VM_USER@127.0.0.1"
