# Host Journaling

You can run DevFS including the background journaling on the host by define
`HOST_JOURNALING` in the `oxbow.h` file.

If you use an SSD that supports namespace sharing, you can run Oxbow just in
the same way it runs with Smart Device. For Devfs,

- Use NVMe-oF (NVMF) without modification.
- Or, you can configure DevFS to use NVMe instead of NVMF by defining
  `USE_NVME_STORAGE_ENGINE` in the `oxbow.h` file.

Even if you don't have such SSD, you can still do performance experiment and
correctness validation using two different SSDs. However, they cannot be done
simultaneously.

For example, let's assume there are two SSDs, `0000:00:04.0` and `0000:00:05.0`
and Secure Daemon and DevFS use `0000:00:04.0` and `0000:00:05.0`, respectively.
You have to configure the proper PCIe addresses of the devices in
`secure_daemon_conf.sh` and `devfs_conf.sh`. Next, do `mkfs` both of the devices
to make them have the same layout (You can use the script below). Make sure that
they have the same layouts and block addresses (check the message prompted
during mkfs).

For performance, just run Oxbow. Secure Daemon and DevFS write to their own
devices and you will get performance results.

For correctness, after running the benchmark, do the followings:

1. Copy the FS area and staging area from Secure Daemon's device (`0000:00:04.0`) to DevFS's
device (`0000:00:05.0`).
2. Do checkpoint.
3. Copy FS area of DevFS's device (`0000:00:05.0`) to Secure Daemon's device
   (`0000:00:04.0`).
4. Kill and restart Secure Daemon.
5. Read the data (e.g., run read microbench), to validate the data.

There are scripts to do processes 1, 2, and 3:

```shell
## 1.
# Copy stage area of Secure Daemon's device to a file (STAGE_AREA_FILE_NAME in oxbow.h).
scripts/sync_device.sh -s stage -t secure_daemon
# Load stage area from the file to DevFS's device.
scripts/sync_device.sh -s stage -t devfs

## 2.
# Do checkpointing.
scripts/device/send_ckpt_signal.sh

## 3.
# Copy file system area of DevFS's device to a file (FS_AREA_FILE_NAME in oxbow.h).
scripts/sync_device.sh -s fs -t devfs
# Load file system area from the file to Secure Daemon's device.
scripts/sync_device.sh -s fs -t secure_daemon
```

## Required configuration

If you run Oxbow with an SSD that supports SR-IOV and namespace sharing, you
have to increase the size of hugepages for SPDK use. In
`scripts/host/setup_spdk.sh` file:

```shell
TOTAL_HUGEMEM_SIZE=8192 # in MB. For host journaling mode.
```

Next, one VF needs to be dedicated to DevFS running on the host. Refer to
`scripts/host/nvme-sriov/setup_vf.sh`. Configure DevFS to use that VF in
`oxbow/devfs/devfs_config.sh` or `oxbow/devfs/myconf.sh`.

```shell
# For example,
# export pcie_nvme_addr="0000:d8:00.0" # Device-journaling
export pcie_nvme_addr="0000:d8:00.4" # Host-journaling
```

## Things to know

- DevFS does not do polling when it runs on the host to save host CPU resources.
- For the correctness check, the block addresses of two devices should be the same. For example,
  the superblock, journal superblock, and staging area superblock should have the
  same block addresses. Note that, making two different partitions with one physical NVMe device and emulating each of them as
  an NVMe device with Qemu doesn't work as their starting (physical) block addresses
  are different. Instead, you can use two different files to emulate NVMe
  devices.

## Useful scripts

- `scripts/host/mkfs_qemu.sh`: To mkfs both devices. Set proper PCIe addresses
  in the file.
- `scripts/host/setup_spdk_qemu.sh`: To setup SPDK. Give proper device name
  (e.g., `/dev/nvmeXnY`).
- `mkfs_qemu.sh` in the project root directory: To do mkfs and SPDK managing at once.
- `scripts/sync_device.sh`: To copy data between two devices.
- `scripts/host/host_journaling_ckpt.sh`: It includes a whole checkpointing process with host journaling enabled.

For example,

```shell
scripts/host/mkfs_qemu.sh nvmeXn0
scripts/host/mkfs_qemu.sh nvmeXn1
```
```shell
# Set proper PCIe addr in the file. Cf) sudo lspci -D
# To bind drivers for SPDK:
scripts/host_setup_spdk_qemu.sh

# To restore the drivers to nvme, give -r option.
scripts/host_setup_spdk_qemu.sh -r
```
```shell
### To do checkpointing with host journaling.

# 1. Copy the stage area to the devfs's device.
scripts/sync_device.sh -s stage -t secure_daemon
scripts/sync_device.sh -s stage -t devfs

# 2. Do checkpointing.
scripts/device/send_ckpt_signal.sh

# 3. Copy the file system area to the secure daemon's device.
scripts/sync_device.sh -s fs -t devfs
scripts/sync_device.sh -s fs -t secure_daemon
```

## Troubleshooting

```shell
spdk_malloc fail: Cannot allocate memory
```

The number of storage engine threads should be set to lower value (e.g., 4) as
the resources are shared between Secure Daemon and DevFS.

---
Random write / random read may not work correctly. For now, sequential write and
read are tested. For example,

```shell
# Sequential write
./run.sh $BENCH_MICRO/build/lat_micro -d $OXBOW_PREFIX -s sw 200M 4K 1

# Sequential read (to validate data)
./run.sh $BENCH_MICRO/build/lat_micro -d $OXBOW_PREFIX -s sr 200M 4K 1
```
