# QEMU VM tools

This directory contains helper scripts for running Oxbow in a QEMU/KVM VM.
For the setup guide, see `Documentation/vm.md`.

## `create_vm.sh`

Creates the VM working directory.

It downloads an Ubuntu 22.04 cloud image, creates a root qcow2 image, creates
two raw NVMe images, creates a cloud-init seed image, generates an SSH key, and
writes `vm.env`.

By default, the root qcow2 image is 20GB and each emulated NVMe image is 20GB.

Default output directory:

```shell
~/oxbow/oxbow-ae-vm
```

Common options:

```shell
--dir <vm_dir>
--force
```

The following environment variables can be used to change image sizes and VM
resources:

```shell
OXBOW_VM_ROOT_SIZE
OXBOW_VM_NVME_SIZE
OXBOW_VM_CPUS
OXBOW_VM_MEM
OXBOW_VM_IMAGE_URL
```

## `run_vm.sh`

Starts or stops the VM.

By default, it runs QEMU in the foreground and prints the serial console to the
terminal.

Common options:

```shell
--dir <vm_dir>
--daemonize
--stop
--passthrough <bdf>
--no-emulated-nvme
```

The default VM attaches two emulated NVMe devices using the serial numbers below:

```shell
oxbow-secure
oxbow-devfs
```

With `--passthrough`, the script calls `bind_vfio_driver.sh` and attaches the
given physical PCIe device with `vfio-pci`.

## `sync_repo.sh`

Copies the current repository to a running VM.

It reads `vm.env`, waits for SSH, waits for cloud-init to finish, and then runs
`rsync` to:

```shell
/home/oxbow/oxbow.code
```

Common options:

```shell
--dir <vm_dir>
--clean
```

`--clean` adds `--delete` to `rsync`.

Prepare submodules and benchmark source in the host tree before running this
script. The VM setup path copies the host checkout as-is and does not clone git
repositories in the guest. `sync_repo.sh` checks that the kernel submodule,
required nested submodules, and `bench/micro` are present before copying.

## `setup_vm.sh`

Sets up the synced VM guest from the host.

It reads `vm.env`, connects over SSH, waits for cloud-init, runs
`setup_vm_guest.sh`, installs the required Ubuntu build packages, builds and
installs the Oxbow kernel, reboots the VM, then builds Oxbow user-level
components and `bench/micro`.

The source tree must already have been synced with `sync_repo.sh`; this script
does not run `git clone` or `git submodule update` in the guest.

Common option:

```shell
--dir <vm_dir>
```

## `ssh_vm.sh`

Logs in to a running VM using the SSH key and port stored in `vm.env`.

Common option:

```shell
--dir <vm_dir>
```

## `setup_vm_guest.sh`

Applies the Oxbow configuration required inside the VM guest. This is called by
`setup_vm.sh`; it can also be run manually inside the guest.

It enables host journaling, switches CPU pinning to NUMA0, enables the Secure
Daemon `msg_ring` build flag to match the VM kernel config, changes the lwext4
mkfs layout to a small VM-sized layout, writes `secure_daemon/myconf.sh` and
`devfs/myconf.sh`, and creates `/mnt/oxbow_flag`. The VM mkfs layout is a 12GB
filesystem, a 3GB journal area, and a 3GB staging area.

Run it in the guest after syncing the source tree:

```shell
cd ~/oxbow.code
tools/qemu/setup_vm_guest.sh --yes
```

Common options:

```shell
--yes
--secure-bdf <bdf>
--devfs-bdf <bdf>
```

This script intentionally modifies the guest checkout for the VM path. It does
not change the default DPU/testbed configuration in the repository.

## `bind_vfio_driver.sh`

Binds a PCIe NVMe device to `vfio-pci`, or binds it back to the host `nvme`
driver with `-r`.

Examples:

```shell
tools/qemu/bind_vfio_driver.sh 0000:d8:00.0
tools/qemu/bind_vfio_driver.sh 0000:d8:00.0 -r
```

This script is only needed for NVMe passthrough.

## `install.sh`

Builds QEMU from source and links the built binaries under `bin/`.
