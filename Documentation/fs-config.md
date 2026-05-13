# File system configuration

## Lwext4 configurations

- Total file system area size can be adjusted in `ext4_mkfs.c` by changing
  values of `EXT4_TOTAL_PARTITION_SIZE` and `EXT4_NR_JOURNAL_BLOCKS`.
  Smaller size reduces the number of inodes and mkfs time.

## Journaling configurations (Not tested yet)

To enable Oxbow's journaling, change the followings:

In `oxbow/secure_daemon/secure_daemon_conf.sh`,

```shell
export device_journaling=1 # 1: enable, 0: disable
```

Currently, Oxbow supports data journaling only.
