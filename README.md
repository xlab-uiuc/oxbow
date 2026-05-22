# Oxbow File System

**Oxbow File System** adopts a coordinated architecture for multi-components
across user, kernel, and device spaces. The file system is presented at the 20th
USENIX Symposium on Operating Systems Design and Implementation (OSDI 2026).
Please find the paper here: [Oxbow: A Coordinated Architecture for Multi-component File Systems](https://www.usenix.org/conference/osdi26/technical-sessions).

Basically, Oxbow is designed to run with a computational storage device which
has a SoC -- general-purpose CPU and dedicated memory -- in it. Since there are
only a few experimental devices at this moment, we emulate the computational
storage using a DPU (a smartNIC).

Additionally, you can try running Oxbow with `Host-journaling` mode that
executes device-side component on the host. With this mode, you can run Oxbow
without any specific hardware in a virtual machine with two -- real or emulated
with two files -- NVMe SSDs.

Please check [Host-journaling](Documentation/host-journaling.md) for
`Host-journaling` setup.

**Notice to OSDI 2026 AE reviewers**:
See the [Artifact Evaluation document](Documentation/artifact-evaluation.md).

## 0. Different names used in the source code

Some components of Oxbow have different names from those described in the paper.
Check the names used in the source code below:

- `secure_daemon`: Described as `H-Server` in the paper.
- `devfs`: Described as `D-Server` in the paper.
- `libfs`: Described as `oxLib` in the paper.

## 1. Software and hardware requirements (Tested environment)

### Host requirements

Software:

- Ubuntu 23.04
- Linux kernel v6.2.10 (Oxbow kernel)

Hardware:

- DPU (Data Processing Unit, BlueField-2) for computational storage emulation
- SR-IOV and namespace sharing support on SSD

### DPU software requirements

- Ubuntu 22.04.3 LTS
- Kernel: 5.15.0-1021-bluefield

We assume that the RDMA connection between the host and the DPU has been configured correctly with the appropriate RDMA drivers.

## 2. Get the source code

### 2.1 Host

```shell
git clone https://github.com/xlab-uiuc/oxbow.git
git submodule update --recursive --init
```

### 2.2 DPU

```shell
git clone https://github.com/xlab-uiuc/oxbow.git
cd oxbow
scripts/device/submodule_init.sh
```

## 3. Compile Kernel FS

### 3.1 Configure kernel

> If you experience a hang during reboot with "Loading RAM disk" message,
> do `make oldconfig` with your working config instead.

Linux kernel source code is in `oxbow/linux-kernel` directory.

```shell
make x86_64_defconfig
```

To build kernel image for qemu:

```shell
make kvm_guest.config	# If failed (in older kernel) do: make kvmconfig
```

Additionally, you can set/unset the following configurations via `make menuconfig` or directly modifying the `.config` file.

Debug configurations might be needed:

```shell
CONFIG_KGDB=y
CONFIG_DEBUG_INFO=y
CONFIG_GDB_SCRIPTS=y
CONFIG_DEBUG_INFO_DWARF4=y
CONFIG_RANDOMIZE_BASE=n  # You can also disable it by adding 'nokaslr' kernel parameter.
CONFIG_USERFAULTFD=y # You can disable it. Not used in the recent version.
```

Set the Oxbow configurations as below:

```shell
CONFIG_OXBOW_ILLUFS=y
# CONFIG_OXBOW_IPC_MSG_RING is not set
CONFIG_OXBOW_SHM_GUP_CACHE=y
# CONFIG_OXBOW_SHM_GUP_COUNTER is not set
CONFIG_OXBOW_ILLUFS_TASK_WRITE_FAULT_CONTEXT=y
```


### 3.2 Build kernel

```shell
make -j
```

### 3.3 Required kernel parameter

The following kernel parameters are required:
- `intel_iommu=on`: to set up nvme-of.
- `memmap=` or `mem=`: to reserve higher memory for nvme-of offload buffer. You can set
  proper value considering your machine's resource. Also, you have to modify the
  script file, `scripts/host/nvmf-of/config_nvmf_server.sh` accordingly.
- `pci=realloc`: to set up nvme-sriov.
- `nvme_core.multipath=N`: to set up nvme-sriov.

Add to the `/etc/default/grub` as an example below and `sudo update-grub`.

```shell
GRUB_CMDLINE_LINUX_DEFAULT="...<other params>... intel_iommu=on iommu=pt memmap=16G\\\$80G pci=realloc nvme_core.multipath=N"
```

### 3.4 Install kernel

```shell
sudo make modules_install install
```

Restart the machine with the installed kernel.


## 4. Prepare to run Oxbow with Tmux sessions

You can easily set up the machine using `uFSBench_tmux.sh` or
`uFSBench_tmux_host-journaling.sh` scripts. These scripts set environment
variables and set up the machine for CSD
emulation -- hence, you can skip [5. Setting environment variables](#5-setting-environment-variables) and
[6. Emulating Smart SSD with Smart NIC](#6-emulating-smart-ssd-with-smart-nic).
In a virtual machine, use `uFSBench_qemu.sh` instead -- you don't need to set up
a smart device. The scripts are useful for running benchmarks as well (See
[artifact-evaluation](Documentation/artifact-evaluation.md) document).

## 5. Setting environment variables

Load environment variables to use Oxbow:

```shell
cd oxbow
source set_env.sh
```

Most of the scripts require environment variables set properly. Check
`set_env.sh` file and set the correct values based on your system.

Especially, check the following variables:
```shell
export NVME_PCIE_ADDR='0000:d8:00.0' # SSD to be used by Oxbow.
export NVME_PCIE_ADDR_EXT4='0000:d8:00.0' # SSD to be used by Ext4.
```

The following variables are set automatically but make sure that correct names are assigned.
```shell
export NVME_DEV_NAME=
export NVME_DEV_NAME_EXT4=
```


## 6. Emulating Smart SSD with Smart NIC

***You can skip this section if you runs Oxbow in `Host-journaling` mode.**  
****Use the script, `scripts/host/setup_machine_all.sh` which do the steps
in this section.**

---

Basic idea is that we will make the host and the device (SmartNIC) share a NVMe
namespace. It is feasible if the followings are supported by hardware.

- SR-IOV support by SSD
- Namespace sharing by SSD
- NVMe-oF is supported by SSD and SmartNIC

We create a VF (Virtual Function, a.k.a. secondary controller) and attach both
the PF (Physical function, a.k.a. primary controller) and the VF to the same
namespace. The host (Secure Daemon) accesses the namespace via VF using SPDK
library and the SmartNIC (DevFS) accesses via PF over NVMe-oF connection.


**\* The order of configurations below is important. The first step reloads
`nvme` driver and it resets all related configuration. It can cause the device
to enter an abnormal state. Additionally, `setup.sh` script of SPDK also changes
a driver from `nvme` to `vfio-pci` which can change the mapping between PCIe
addresses and device names.**

1. Set up NVMe-oF server (host) using PF and connect the client (SmartNIC).
2. Set up VF.
3. Set up SPDK to use the VF.

### 6.1. Set up NVMe-oF (PF)

Refer to the [`Documentation/nvmf.md`](Documentation/nvmf.md).

### 6.2. Set up VF

```shell
scripts/host/setup_sriov_vf.sh
```

Make sure that `NVME_DEV_NAME` has been parsed correctly in the script,
`scripts/host/nvme-sriov/setup_vf.sh`, which is called in `setup_sriov_vf.sh`.
This script parses `NVME_DEV_NAME` assuming that your SSD is `PM1735`. If you
are using other device, you have to set the proper name in the script.

Refer to the `scripts/host/nvme-sriov/README.me` for the details.

### 6.3. Set up SPDK to use VF

Set `pcie_nvme_addr` to the PCIe address of the created VF in
`oxbow/secure_daemon/secure_daemon_conf.sh`. For example, the first VF's is
`0000:d8:00.1` (where `0000:d8:00.0` is PF's).

```shell
$ lspci
...
d8:00.0 Non-Volatile memory controller: Samsung Electronics Co Ltd NVMe SSD Controller PM173X
d8:00.1 Non-Volatile memory controller: Samsung Electronics Co Ltd NVMe SSD Controller PM173X
d8:00.2 Non-Volatile memory controller: Samsung Electronics Co Ltd NVMe SSD Controller PM173X
```

Run `scripts/host/setup_spdk.sh` to set up huge pages and bind a driver for SPDK
library. `-r` option resets the configuration.


## 7. Compile user-level components

There are three user-level components,

that run on the host:

- Secure Daemon: User-level daemon (a file server process).
- LibFS: Library for applications.

that runs on the smart device:

- DevFS: In-device daemon.

### 7.1 Install libraries

```shell
cd oxbow/libfs/lib
./install.sh
```

### 7.2 Compile LibFS

```shell
cd oxbow/libfs
./build.sh
# Or, to rebuild:
# ./build.sh re
```

### 7.3 Compile Secure Daemon

(Currently, Oxbow supports only `lwext4` file system. `sefs` is not tested.)

~~Select a file system by changing the followings:~~

1. ~~`filesystem` variable in the `oxbow/secure_daemon/meson.build` file~~
2. ~~`filesystem` variable in the `oxbow/secure_daemon/secure_daemon_conf.sh` file~~

~~Note that you also need to change the following when compiling DevFS:~~

1. ~~`filesystem` variable in the `oxbow/devfs/meson.build` file~~

Set the appropriate values for the Oxbow layout. See [Format Oxbow](#9-format-oxbow).

And build it.

```shell
cd oxbow/secure_daemon
./build.sh
# Or, to rebuild:
# ./build.sh re
```

### 7.4 Compile DevFS

Refer to [`oxbow/devfs/README.md`](oxbow/devfs/README.md).

## 8. Configure Oxbow

### 8.1 Configure Secure Daemon

Set proper values in `oxbow/secure_daemon/secure_daemon_conf.sh`.
Note that you can create an untracked configuration file,
`oxbow/secure_daemon/myconf.sh`, that overwrites the configurations in
`secure_daemon_conf.sh`.

An example of `myconf.sh`:
```shell
#!/bin/bash
# Configs in this file overwrites configs in secure_daemon_conf.sh
export pcie_nvme_addr="0000:XX:00.1 0000:XX:00.2 0000:XX:00.3"
# export rpc_rdma_ip_addr="192.168.14.113" # host address for host journaling
export rpc_rdma_ip_addr="192.168.14.114" # device address for device journaling
```

It is required to set the proper values for `pcie_nvme_addr` and
`rpc_rdma_ip_addr` to run Secure Daemon.

Additionally, configure NUMA binding and CPU pinning in
`oxbow/secure_daemon/include/common/cpu_pinning.h`.
See [IO Thread Pinning](Documentation/io-thread-pinning.md).

### 8.2 Configure DevFS

Refer to [`oxbow/devfs/README.md`](oxbow/devfs/README.md).

### 8.3 Configure LibFS

There is a file `oxbow/libfs/libfs_conf.sh` for the libfs configuration, but you
don't need to modify it.

## 9 Format Oxbow

Configure the size of the Oxbow layout in `oxbow/secure_daemon/src/fs/ext4_mkfs.c`.
The following two constants determine the layout.

```c
/* Example to set up: 12GB filesystem + 3GB journal + 3GB staging. */

// 18 GB. Total size of the Oxbow layout.
#define EXT4_TOTAL_PARTITION_SIZE (18UL * 1024 * 1024 * 1024)

// 3 GB. Size of the journaling and staging areas.
#define EXT4_NR_JOURNAL_BLOCKS ((3UL * 1024 * 1024 * 1024) / 4096)
```

Rebuild the components to apply the changes.

Then format the layout with the script:

```shell
# Do mkfs with this script. (The script assumes SR-IOV setup.)
scripts/host/mkfs.sh
```

~~Or, do mkfs manually: `sudo build/sefs_mkfs /dev/nvmeXnY`.~~

## 10. How to run Oxbow file system

You need at least three terminals. Using tmux is recommended -- you can launch
the required session with `uFSBench_tmux.sh` or
`uFSBench_tmux_host-journaling.sh`. Refer to
[deploy-on-testbed.md](Documentation/deploy-on-testbed.md#run-oxbow-with-scripts) as well.

The order matters: start DevFS first then Secure Daemon.

### 10.2 Run DevFS

Refer to [`oxbow/devfs/README.md`](oxbow/devfs/README.md).

### 10.3 Run Secure Daemom

```shell
cd oxbow/secure_daemon
./run.sh

##############################
### Ready to use Oxbow FS. ###
##############################

## You can exit daemon by pressing ctrl+c.
#
## Make sure to umount /oxbow after terminating Secure Daemon.
sudo umount /oxbow
```

### (Optional) Tracking Kernel (IlluFS) message

```shell
## check kernel log (optional)
$ sudo dmesg --follow
```

### Run application built with LibFS

Run a test program.

```shell
cd oxbow/libfs

# Run program (e.g. benchmark)
./run.sh build/test/file_basic
```

## Run benchmark

Refer to [`Documentation/bench.md`](Documentation/bench.md).

## Other configurations

Refer to [`Documentation/fs-config.md`](Documentation/fs-config.md).

## Troubleshooting

### SPDK does not enumerate my device

NVMe device is not probed. `setup.sh` script of SPDK prompts as below.

```shell
0000:d9:00.0 (144d a80a): Active devices: data@nvme1n1, so not binding PCI dev
```

You have to wipe out the partition table from the device. `wipefs` command will do.

```shell
sudo wipefs -a <device_name: ex) /dev/nvme1n0>
```

The device should not be displayed with `blkid`.

### SPDK does not attach my device

NVMe device is not attached. An example of error message is as below.

```shell
...
16:12:28 DEBUG ../src/common/storage_engine/se_nvme.c:61: Attaching to 0000:d8:00.0
[2023-08-21 16:12:28.591811] pci.c:1016:spdk_pci_device_claim: *ERROR*: could not open /var/tmp/spdk_pci_lock_0000:d8:00.0
[2023-08-21 16:12:28.591835] nvme_pcie.c: 888:nvme_pcie_ctrlr_construct: *ERROR*: could not claim device 0000:d8:00.0 (Permission denied)
[2023-08-21 16:12:28.591846] nvme.c: 677:nvme_ctrlr_probe: *ERROR*: Failed to construct NVMe controller for SSD: 0000:d8:00.0
EAL: Requested device 0000:d8:00.0 cannot be used
...
```

Use SPDK's `setup.sh` script to clean up the device.

```shell
cd lib/spdk
sudo scripts/setup.sh cleanup
```

### SPDK setup script does not unbind the device from the existing driver (nvme)

For example, when you run the script, `scripts/host/setup_spdk.sh`, you will see
the following messages for the correct PCIe address.

```shell
sudo PCI_ALLOWED=0000:00:XX.0 /home/yulistic/data/oxbow.code/oxbow/libfs/lib/spdk/scripts/setup.sh
0000:00:XX.0 (c0a9 5415): Active devices: data@nvme0n1, so not binding PCI dev
INFO: Requested 1024 hugepages but 4096 already allocated on node0
```

Unbind the device from the driver manually with the following command.

```shell
# As a root account:
echo 0000:00:XX.0 > /sys/bus/pci/drivers/nvme/unbind

# Or,
# Using sudo:
sudo bash -c "echo 0000:00:XX.0 > /sys/bus/pci/drivers/nvme/unbind"
```

