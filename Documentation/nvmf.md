# NVMe-oF configuration

You need to configure NVMe-oF to use `nvmf` storage engine.
Please refer to the following documents for basic configuration of RDMA and NVMe-oF.

- [Howto configur NVMe over fabrics](https://enterprise-support.nvidia.com/s/article/howto-configure-nvme-over-fabrics)
- [Howto configure NVMe-oF target offload](https://enterprise-support.nvidia.com/s/article/howto-configure-nvme-over-fabrics--nvme-of--target-offload)

## Terminology

- Target server: Where the nvme storage device is installed. In Oxbow, the host
  is a target server of NVMe-oF.
- Client (a.k.a. initiator): It sends I/O requests to the target server. In
  Oxbow, SmartNIC is a client.

## Prerequisites

### Drivers and hardware specification

#### Server

- Ubuntu 23.04
- Linux Kernel 6.2.10 (Oxbow kernel)
- MLNX_OFED_LINUX-23.07-0.5.0.0-ubuntu23.04-x86_64

Install MLNX_OFED driver as below. If an error occurs, do `sudo ./uninstall.sh` first.

```shell
sudo ./mlnxofedinstall --add-kernel-support --with-nvmf
sudo reboot
```

**You must disable IOMMU for NVMe-oF target offload.** Add `intel_iommu=off` to the
kernel parameters.

Also, it is recommended to reserve memory for the target offload. For example,
`mem=176128M` restricts kernel's memory use up to 172GB. The higher memory can be
used buffers for an offloaded NVMe-oF target. You have to adjust the values in
the `config_nvmf_server.sh` script:

```shell
# Please refer to comments in the source file.
sudo modprobe nvmet-rdma offload_mem_start=0x2b80000000 offload_mem_size=16384 offload_buffer_size=1024
```

#### Client

Bluefield 2 is used as a client. We use
`DOCA_2.2.0_BSP_4.2.0_Ubuntu_22.04-2.23-07.prod.bfb` to install Ubuntu to
Bluefield 2 DPU. It includes MLNX_OFED driver of version 23.07-0.5.0.
If you want to install MLNX_OFED driver, add `--with-nvmf` flag just as we
installed to the target server.

### RDMA configuration

The RDMA network should be configured between the server and the client. You can
test the RDMA connection as below.

From one side,

```shell
ib_write_bw -d mlx5_0 -i 1 -a -F --report_gbits
```

from the other side,

```shell
ib_write_bw -d mlx5_0 -i 1 -a -F --report_gbits <server_ip_addr>
```

In this example, server's ip address of RDMA interface is 192.168.13.113 and
client's address is 192.168.14.114.

### `nvme-cli` installation on client

`nvmecli` is required to configure the client.

```shell
git clone https://github.com/linux-nvme/nvme-cli.git
cd nvme-cli
```

Install `nvme-cli` referring to its `README.md` file.

### Build SPDK with RDMA

```shell
scripts/pkgdep.sh --rdma
configure --with-rdma
# And build it.
```

## Configure NVMe-oF

### Server-side (Host)

You can use `scripts/host/nvme-of/configure_nvmf_server.sh` to configure your target
server. Please set the correct values for the variables in the script. You have
to set `RDMA_IP_ADDR` and `NVME_DEV` properly. Default values can be used for
the other variables.

```shell
...
### You may want to change the followings.
HW_OFFLOAD=1 # NVMe-oF offload to RDMA NIC.
NVME_SUBSYSTEM_NAME="oxbow-nvmf"
NAMESPACE="1"
PORT_DIR="1"
PORT_NUM="4420"

# Set proper path to nvme-cli.
NVME_BIN="$TOOLS/bin/nvme" # Use binary in this project.
#NVME_BIN="nvme" # If nvme-cli is installed.

# Set manually.
RDMA_IP_ADDR="192.168.14.113" # Target server IP (Host IP).
# Or,
# Read from devfs_conf.sh file. TODO: After creating global config file.
# RDMA_IP_ADDR=$(
# 	source $DEVFS/devfs_conf.sh
# 	echo "$nvmf_ip_addr"
# )

...

# Set manually.
# NVME_DEV="/dev/nvme1n1"
# Or,
# Parse it.
# The device in our testbed is Samsung PM1735. However, nvme list shows the
# ...1733... as its Subsystem-NQN.
temp=$(sudo $NVME_BIN list -v | grep 1733 | xargs | cut -d ' ' -f 3)
NVME_DEV=$(sudo $NVME_BIN list -v | grep $temp | grep dev | xargs | cut -d ' ' -f 1)
echo "NVMe device name: $NVME_DEV"
...
```

You can create and destroy a NVMe-oF target.

```shell
# To create.
./configure_nvmf_server.sh

# To destroy. Disconnect client first.
./configure_nvmf_server.sh -d
```

### Client-side (SmartNIC)

Use `scripts/device/nvme-of/configure_nvmf_client.sh` to discover, connect to, and
disconnect from the target server.  
*As it sources the variables of `config_nvmf_server.sh`, make sure that the
same `config_nvmf_server.sh` file is used for both the host and the SmartNIC.*

```shell
# To discover.
./configure_nvmf_client.sh -l

# To connect.
./configure_nvmf_client.sh

# To disconnect
./configure_nvmf_client.sh -d
```
