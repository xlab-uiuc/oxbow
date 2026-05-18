# Oxbow DevFS README

This document describes how to run DevFS. DevFS can run on both host
(`Host-journaling` mode) and Smart
device (Bluefield 2 DPU). Please follow the instructions where you want to run
DevFS (i.e., on the host for `Host-journaling` mode or on the device).

## Prerequisite

We assume RDMA connection between the host and the Bluefield DPU is set
correctly, which might require appropriate RDMA driver (e.g., OFED_MLNX RDMA).

## Source code path

Benchmark scripts ([bench.md](../../Documentation/bench.md)) assume that the
host and DPU source trees are located at the same path. Therefore, place the
device-side source code at the same path as the host-side source code.

## Set environment variables

```shell
source set_env.sh
```

## Compile DevFS library

### Static configurations (To be fixed)

For now, you have to set the number of I/O thread in the
`devfs/src/common/storage_engine/se_nvmf_fast.c`. There are four threads by default.

```c
	env_opts->core_mask = "0xf"; // -c core mask. 4 cores
```

### Build dependencies

LibFS and DevFS share a library directory. You only need to compile the
libraries once if you run DevFS on the host.

If you are on the device, you don't need to build the `syscall_intercept`
library. Pass `-d` to skip it.

```shell
cd oxbow/devfs/lib
./install.sh -d
```

### Copy `oxbow_kernel.h`

This header defines common constants used by Oxbow components. Since the full
kernel source tree is not needed, copy only this file as shown below.

```shell
# From the host (assuming the kernel source code has been cloned):
scp oxbow/linux-kernel/include/uapi/linux/oxbow/oxbow_kernel.h <dpu_ipaddr>:<oxbow_root>/oxbow/linux-kernel/include/uapi/linux/oxbow/
```

### Hugepages and device binding for SPDK library

Run the following script to set up SPDK huge page memory.

```shell
scripts/device/setup_spdk.sh
```

### Build DevFS

(Currently, Oxbow supports only `lwext4` file system. `sefs` is not tested.)

~~Select a file system by changing the followings:~~

1. ~~`filesystem` variable in the `oxbow/devfs/meson.build` file~~

~~Note that you alse need to change the following when compiling Secure Daemon:~~

1. ~~`filesystem` variable in the `oxbow/secure_daemon/meson.build` file~~
2. ~~`filesystem` variable in the `oxbow/secure_daemon/secure_daemon_conf.sh` file~~


```shell
cd oxbow/devfs
./build.sh
# Or, to rebuild:
# ./build.sh re
```

It builds the DevFS and tests (`oxbow/devfs/build/test/`).


## Configure DevFS

Check the file `devfs_conf.sh`. Set proper values for the configurations. You
can overwrite the configurations in `devfs_conf.sh` by writing your own local
configuration named as `myconf.sh` in the same directory.

An example of `oxbow/devfs/myconf.sh` file:

```shell
#!/bin/bash
# Configs in this file overwrites configs in devfs_conf.sh
export rpc_rdma_ip_addr="192.168.14.113" # host address.
export pcie_nvme_addr="0000:XX:00.0" # host ssd pcie address.
export nvmf_ip_addr="192.168.14.113" # host address.
```

These values must be configured correctly.

## Run DevFS

```shell
cd oxbow/devfs
./run.sh build/devfs
```

## Run tests

```shell
cd oxbow/devfs
# sudo "$OXBOW_ROOT"/dep_pkgs/bin/meson test -C build # Not supported yet.

# Or, run each of them manually.
./run.sh build/test/TEST_PROG
```

## Development

### Formatter

You can use the auto format feature of VSCode. Please use `oxbow/devfs/.clang-format` file. This file is from the Linux kernel source code.
