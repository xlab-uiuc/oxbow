# Deploy Oxbow in a VM

This document describes how to use a VM for evaluation.

The default VM configuration uses:

- Ubuntu 22.04 cloud image as the root disk.
- Two QEMU NVMe devices.
  - `oxbow-secure`: device for Secure Daemon.
  - `oxbow-devfs`: device for DevFS.
- SSH port forwarding from host port `5555` to guest port `22`.
- Guest user `oxbow`.
- 20GB root disk and two 20GB emulated NVMe devices by default.

The default VM configuration also requires:

- Around 60 GB of storage for the VM root disk and two emulated QEMU NVMe devices.

## Host prerequisites

Install QEMU and utilities on the host.

```shell
sudo apt update
sudo apt install -y qemu-system-x86 qemu-utils cloud-image-utils rsync openssh-client
```

Make sure KVM is available.

```shell
ls -l /dev/kvm
```

If the current user cannot access `/dev/kvm`, add it to the `kvm` group and log
in again.

```shell
sudo usermod -a -G kvm $USER
```

## Prepare the repository

Before creating the VM, clone everything.

```shell
source set_env.sh
git submodule update --recursive --init

scripts/download_bench.sh
git -C "$BENCH" submodule update --recursive --init micro
```

## Create VM images

Run the following command from the Oxbow repository.

```shell
tools/qemu/create_vm.sh --dir ~/oxbow/oxbow-ae-vm
```

This will create all necessary VM images, get the guest OS, and write
the default QEMU configuration for you.

```shell
~/oxbow/oxbow-ae-vm/vm.env
~/oxbow/oxbow-ae-vm/oxbow-ae-root.qcow2
~/oxbow/oxbow-ae-vm/nvme-secure.raw
~/oxbow/oxbow-ae-vm/nvme-devfs.raw
~/oxbow/oxbow-ae-vm/seed.iso
~/oxbow/oxbow-ae-vm/ssh/oxbow-ae
~/oxbow/oxbow-ae-vm/ssh/oxbow-ae.pub
```

If the directory already contains VM images, the script exits without changing
them. To recreate the VM images, use `--force`.

```shell
tools/qemu/create_vm.sh --dir ~/oxbow/oxbow-ae-vm --force
```

## Run the VM

```shell
tools/qemu/run_vm.sh --dir ~/oxbow/oxbow-ae-vm
```

Please keep this terminal open (you might want to use `tmux`).

## Sync the repository

Use another terminal after starting the VM. The following command waits for SSH,
waits for cloud-init to finish, and copies the current repository to the guest.

```shell
tools/qemu/sync_repo.sh --dir ~/oxbow/oxbow-ae-vm
```

The default destination is:

```shell
/home/oxbow/oxbow.code
```

You only need to sync once if no update is required.

The default sync does not delete files that exist only in the guest. To make the
guest tree match the host tree, use `--clean`.

```shell
tools/qemu/sync_repo.sh --dir ~/oxbow/oxbow-ae-vm --clean
```

## Set up the VM guest

Run the setup helper from the host. It connects to the guest over SSH, applies
the VM configuration, builds and installs the Oxbow kernel, reboots
the VM, and then builds Oxbow and the microbenchmark.

```shell
tools/qemu/setup_vm.sh --dir ~/oxbow/oxbow-ae-vm
```

## Login to the VM

Use the SSH helper.

```shell
tools/qemu/ssh_vm.sh --dir ~/oxbow/oxbow-ae-vm
```

Then load Oxbow environment variables.

```shell
cd ~/oxbow.code
source set_env.sh
```

## Compile Oxbow

See [README.md](../README.md) for compilation instructions.

Before compiling, configure smaller file system layouts in
`oxbow/secure_daemon/src/fs/lwext4/ext4_mkfs.c`. For example, use a 12 GB file
system area (`EXT4_TOTAL_PARTITION_SIZE`) and 3 GB each for the journaling and
staging areas (`EXT4_NR_JOURNAL_BLOCKS`), for a total of 18 GB.

Additionally, to adjust the VM root disk size or QEMU NVMe device size,
configure them in `create_vm.sh`:

```shell
ROOT_SIZE="${OXBOW_VM_ROOT_SIZE:-20G}"
NVME_SIZE="${OXBOW_VM_NVME_SIZE:-20G}"
```

## Prepare QEMU NVMe devices

The VM uses two emulated QEMU NVMe devices. The root-level `mkfs_qemu.sh` script
formats both devices and sets them up for SPDK.

```shell
./mkfs_qemu.sh --auto
```

This is destructive. It formats both QEMU NVMe devices:

- `oxbow-secure`
- `oxbow-devfs`

For the VM path, `tools/qemu/setup_vm.sh` changes the lwext4 mkfs layout in the
guest checkout to use a 12GB filesystem, a 3GB journal area, and a 3GB staging
area. This fits in the default 20GB emulated NVMe images.

## Running microbenchmarks

You will need to use separate terminals for DevFS, Secure Daemon, and benchmark
commands. Using `tmux` is recommended.

The following helper creates `tmux` panes, load environment variables, starts
DevFS, Secure Daemon, and opens a benchmark pane.

```shell
cd ~/oxbow.code
./uFSBench_tmux_qemu.sh
```

For benchmarks, see `Documentation/bench.md`. You can only run
microbenchmarks in the VM.

Run `ap` first:

```shell
# In ~/oxbow.code/oxbow/libfs
./run.sh "$BENCH_MICRO/build/tput_micro" -d "$OXBOW_PREFIX" -s ap 4K 4K 1
```

Some microbenchmark workloads (`sw`, `rw`, `sr`, and `rr`) need a checkpointed
base image. With DevFS and Secure Daemon running, use:

```shell
# In ~/oxbow.code
scripts/host/host_journaling_ckpt.sh
```

After each dump or load signal, wait until the corresponding DevFS or Secure
Daemon terminal reports that the operation is complete. Then press enter in the
checkpoint script. Do not press enter repeatedly: the dump and load operations
copy large regions and must finish in order.

Then run a small latency read test.

```shell
cd ~/oxbow.code
source set_env.sh
cd oxbow/libfs
./run.sh "$BENCH_MICRO/build/lat_micro" -d "$OXBOW_PREFIX" -s sr 4K 4K 1
```

To exercise the other microbenchmark modes, use the same pattern after creating
or checkpointing the corresponding base image.

```shell
./run.sh "$BENCH_MICRO/build/tput_micro" -d "$OXBOW_PREFIX" -s sw 4K 4K 1
./run.sh "$BENCH_MICRO/build/tput_micro" -d "$OXBOW_PREFIX" -s rw 4K 4K 1
./run.sh "$BENCH_MICRO/build/tput_micro" -d "$OXBOW_PREFIX" -s sr 4K 4K 1
./run.sh "$BENCH_MICRO/build/tput_micro" -d "$OXBOW_PREFIX" -s rr 4K 4K 1
```

## Advanced usage

For QEMU script details, see `tools/qemu/README.md`.
