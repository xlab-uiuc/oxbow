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

KNOWN_HOSTS="$VM_DIR/known_hosts"
SSH_OPTS=(
	-o BatchMode=yes
	-o StrictHostKeyChecking=no
	-o UserKnownHostsFile="$KNOWN_HOSTS"
	-i "$SSH_KEY"
	-p "$SSH_PORT"
)
SSH_TARGET="$VM_USER@127.0.0.1"

wait_for_ssh() {
	echo "Waiting for SSH on 127.0.0.1:$SSH_PORT..."
	for _ in $(seq 1 180); do
		if ssh "${SSH_OPTS[@]}" "$SSH_TARGET" true >/dev/null 2>&1; then
			return 0
		fi
		sleep 2
	done

	echo "SSH is not ready. Make sure the VM is running."
	exit 1
}

remote_bash() {
	ssh "${SSH_OPTS[@]}" "$SSH_TARGET" 'bash -se'
}

wait_for_ssh

echo "Waiting for cloud-init..."
remote_bash <<'REMOTE'
if command -v cloud-init >/dev/null 2>&1; then
	cloud-init status --wait
fi
REMOTE

echo "Checking synced source tree..."
remote_bash <<'REMOTE'
set -euo pipefail
cd ~/oxbow.code

missing=0
require_path() {
	local path="$1"
	if [ ! -e "$path" ]; then
		echo "Missing $path"
		missing=1
	fi
}

require_path oxbow/linux-kernel/Makefile
require_path oxbow/libfs/lib/spdk/README.md
require_path dep_pkgs/meson/meson.py
require_path bench/micro/build.sh

if [ "$missing" -ne 0 ]; then
	echo "Prepare submodules and bench/micro on the host, then run tools/qemu/sync_repo.sh again."
	exit 1
fi
REMOTE

echo "Applying VM guest configuration..."
remote_bash <<'REMOTE'
set -euo pipefail
cd ~/oxbow.code
tools/qemu/setup_vm_guest.sh --yes
REMOTE

echo "Installing build dependencies and building the Oxbow kernel..."
remote_bash <<'REMOTE'
set -euo pipefail
cd ~/oxbow.code
set +e +u +o pipefail
source set_env.sh
set -euo pipefail

sudo apt-get update
sudo DEBIAN_FRONTEND=noninteractive apt-get install -y \
	bc bison build-essential cmake cython3 dwarves flex gcc git \
	libacl1-dev libcapstone-dev libelf-dev libncurses-dev \
	libnl-3-dev libnl-route-3-dev libnuma-dev libssl-dev libudev-dev \
	ninja-build nvme-cli pandoc pciutils pkg-config python3 python3-dev \
	python3-docutils python3-pip re2c tmux valgrind

cd oxbow/linux-kernel
cp ../vm_config .config
make olddefconfig
make -j"$(nproc)"
sudo make modules_install install
REMOTE

echo "Rebooting the VM into the Oxbow kernel..."
ssh "${SSH_OPTS[@]}" "$SSH_TARGET" 'sudo reboot' >/dev/null 2>&1 || true
sleep 5
wait_for_ssh

echo "Checking the Oxbow kernel..."
remote_bash <<'REMOTE'
set -euo pipefail
uname -r
grep illufs /proc/filesystems
REMOTE

echo "Building Oxbow user-level components and microbenchmark..."
remote_bash <<'REMOTE'
set -euo pipefail
cd ~/oxbow.code
set +e +u +o pipefail
source set_env.sh
set -euo pipefail

(cd dep_pkgs && ./install.sh)
(cd oxbow/libfs/lib && ./install.sh)
./rebuild.sh

if [ ! -d "$BENCH_MICRO" ]; then
	echo "$BENCH_MICRO does not exist."
	echo "Prepare the benchmark repository on the host before running sync_repo.sh."
	exit 1
fi

(cd "$BENCH_MICRO" && ./build.sh)
REMOTE

echo "VM setup completed."
