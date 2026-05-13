#!/bin/bash
# DevFS configurations loaded as environment variables.
# This file defines default values.
# You can overwrite configuration by setting environment variable in run command.

export rpc_thread_num=8 # The number of RPC handler threads. (or main worker threads: rpc handler & io worker.)

export spdk_max_io_requests_in_qpair=512 # SPDK max number of I/O requests in a qpair.

# Per-qpair outstanding write inflight cap. Throttles burst dispatch so it
# stays within the device's sustained write capability. 0 = use default
# (spdk_max_io_requests_in_qpair / 2). If non-zero, the effective cap is
# min(this value, spdk_max_io_requests_in_qpair / 2).
export nvmf_max_inflight_per_qpair=96

export rpc_rdma_ip_addr="" # SecureDaemon ip addr.
export rpc_rdma_port=7174
export data_fetcher_rdma_port=7175
export data_fetcher_rdma_port_second=7176
export pcie_nvme_addr="" # ((WARN)) You must set proper PCIE address. Data can be lost. Used if storage engine = nvme.
export nvmf_ip_addr=""   # ((WARN)) You must set proper address. Data can be lost.
export nvmf_port=4420
export nvmf_subnqn="oxbow-nvmf"

# Only used when we do host background journaling and DevFS uses nvme instead of
# nvmf. (HOST_JOURNALING and USE_NVME_STORAGE_ENGINE is set)
# Otherwise, the number of worker threads is defined in the set_opts() function in se_nvmf.c.
# Note that, max is 7 due to 1 is dedicated to fsync in the secure daemon implementation.
export storage_engine_thread_num=3

### Example configuration in Libra06 ###
# export rpc_rdma_ip_addr="192.168.14.113"
# export pcie_nvme_addr="0000:d8:00.0"
# export nvmf_ip_addr="192.168.14.113"

### Configuration in Libra09 ###
# export rpc_rdma_ip_addr="192.168.14.115"
# export rpc_rdma_port=7174
# export pcie_nvme_addr="0000:d8:00.0" # ((WARN)) You must set proper PCIE address. Data can be lost.

# You can overwrite configs using myconf.sh file which is not tracked by git.
[ -e "${DEVFS}/myconf.sh" ] && source "${DEVFS}/myconf.sh" || true
