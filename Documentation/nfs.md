# NFS Configuration for Communication between Host and SmartNIC(devfs)

Benchmark scripts use NFS to communicate with `devfs` in SmartNIC.

## Host Configuration

An NFS server is configured in the host. First, create a directory to expose to
the NFS client.

```shell
sudo mkdir /mnt/oxbow_flag
sudo chmod 777 /mnt/oxbow_flag
```

Install NFS server.

```shell
sudo apt update
sudo apt install nfs-kernel-server
```

Add the following line to the `/etc/exports`.

```
/mnt/oxbow_flag *(rw,no_root_squash,sync,insecure,no_subtree_check,no_wdelay)
```

Apply the change.

```shell
sudo exportfs -rv
```

## Device-side (SmartNIC) Configuration

Install NFS client.

```shell
sudo apt update
sudo apt install nfs-common
```

Create a directory and mount NFS on it.

```shell
sudo mkdir /mnt/oxbow_flag
sudo chmod 777 /mnt/oxbow_flag
sudo mount -t nfs <nfs_server_address>:/mnt/oxbow_flag /mnt/oxbow_flag
```

<!-- ### Optional

You can add the following line to `/etc/fstab` to make the system automatically
mount the NFS share at boot.

```shell
<nfs_server_address>:/mnt/oxbow_flag /mnt/oxbow_flag nfs rw,relatime,exec,nofail 0 0
``` -->

## Troubleshoot

If mount command is not successful, check the NFS server status.

```shell
sudo systemctl status nfs-kernel-server
```

If it is disabled, enable it.

```shell
sudo systemctl start nfs-kernel-server
```
