#!/bin/bash
set -euo pipefail

SCRIPT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
REPO_DIR=$(cd "$SCRIPT_DIR/../.." && pwd)
VM_DIR="${OXBOW_AE_VM_DIR:-$HOME/oxbow/oxbow-ae-vm}"
CLEAN=0

print_usage() {
	echo "Usage: $(basename "$0") [--dir <vm_dir>] [--clean]"
}

while [ $# -gt 0 ]; do
	case "$1" in
	--dir)
		VM_DIR="$2"
		shift 2
		;;
	--clean)
		CLEAN=1
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
VM_ENV="$VM_DIR/vm.env"
if [ ! -f "$VM_ENV" ]; then
	echo "$VM_ENV does not exist. Run tools/qemu/create_vm.sh first."
	exit 1
fi

require_host_path() {
	local path="$1"
	local hint="$2"

	if [ -e "$path" ]; then
		return 0
	fi

	echo "Missing $path"
	echo "$hint"
	return 1
}

check_host_source_tree() {
	local missing=0

	require_host_path "$REPO_DIR/oxbow/linux-kernel/Makefile" \
		"Run git submodule update --recursive --init in the host checkout." || missing=1
	require_host_path "$REPO_DIR/oxbow/libfs/lib/spdk/README.md" \
		"Run git submodule update --recursive --init in the host checkout." || missing=1
	require_host_path "$REPO_DIR/dep_pkgs/meson/meson.py" \
		"Run git submodule update --recursive --init in the host checkout." || missing=1
	require_host_path "$REPO_DIR/bench/micro/build.sh" \
		"Prepare bench/micro in the host checkout before syncing into the VM." || missing=1

	if [ "$missing" -ne 0 ]; then
		echo "The VM path copies the host source tree as-is and does not clone repositories in the guest."
		exit 1
	fi
}

check_host_source_tree

# shellcheck disable=SC1090
source "$VM_ENV"

KNOWN_HOSTS="$VM_DIR/known_hosts"
SSH_OPTS=(
	-o BatchMode=yes
	-o StrictHostKeyChecking=no
	-o UserKnownHostsFile="$KNOWN_HOSTS"
	-i "$SSH_KEY"
	-p "$SSH_PORT"
)
SSH_TARGET="$VM_USER@127.0.0.1"

echo "Waiting for SSH on 127.0.0.1:$SSH_PORT..."
for _ in $(seq 1 120); do
	if ssh "${SSH_OPTS[@]}" "$SSH_TARGET" true >/dev/null 2>&1; then
		break
	fi
	sleep 2
done

if ! ssh "${SSH_OPTS[@]}" "$SSH_TARGET" true >/dev/null 2>&1; then
	echo "SSH is not ready. Make sure the VM is running."
	exit 1
fi

ssh "${SSH_OPTS[@]}" "$SSH_TARGET" \
	'if command -v cloud-init >/dev/null 2>&1; then cloud-init status --wait; fi'

ssh "${SSH_OPTS[@]}" "$SSH_TARGET" "mkdir -p /home/$VM_USER/oxbow.code"

RSYNC_SSH="ssh -o StrictHostKeyChecking=no -o UserKnownHostsFile=$KNOWN_HOSTS -i $SSH_KEY -p $SSH_PORT"
RSYNC_OPTS=(
	-a
	'--exclude=.git/'
	'--exclude=.git'
	'--exclude=qemu-serial.log'
	'--exclude=*.dump'
)

if [ "$CLEAN" -eq 1 ]; then
	RSYNC_OPTS+=(--delete)
fi

rsync "${RSYNC_OPTS[@]}" -e "$RSYNC_SSH" \
	"$REPO_DIR/" "$SSH_TARGET:/home/$VM_USER/oxbow.code/"

echo "Synced $REPO_DIR to /home/$VM_USER/oxbow.code"
