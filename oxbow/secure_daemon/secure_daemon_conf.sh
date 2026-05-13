#!/bin/bash
# Secure Daemon configurations loaded as environment variables.
# This file defines default values.
# You can overwrite configuration by setting environment variable in run command.
export rpc_rdma_ip_addr="" # DevFS ip addr.
export rpc_rdma_port=7174
export data_fetcher_rdma_port=7175
export data_fetcher_rdma_port_second=7176

# One or more PCIe BDFs (whitespace-separated). Multiple BDFs let the
# storage engine spread IO worker qpairs across several SR-IOV VFs of
# the same NVMe namespace, which is required to scale beyond a single
# VF's HW IO queue cap (~8 qpairs per VF). Up to MAX_NVME_VFS (4) entries.
# The same format is consumed by SPDK setup scripts (PCI_ALLOWED), so
# commas are rejected by the daemon's parser.
#   Examples:
#     "0000:d8:00.1"                       # single VF (legacy)
#     "0000:d8:00.1 0000:d8:00.2"          # two VFs, IO workers split evenly
export pcie_nvme_addr=""                 # ((WARN)) You must set proper PCIE address. Data can be lost.
#export pcie_nvme_addr="0000:00:04.0" QEMU VM setting

# export spdk_max_io_requests_in_qpair=512 # SPDK max number of I/O requests in a qpair. Not used in secure daemon.
export rpc_rdma_thread_num=4
export rpc_shmem_thread_num=8
export io_handler_thread_num=8 # Not used.
export snapshot_thread_num=4 # Number of threads for snapshot. One is used for metadata.
#  If spdk_zmalloc fail, then add more hugepages
#  sudo sh -c "echo 2048 > /sys/kernel/mm/hugepages/hugepages-2048kB/nr_hugepages"

# Total IO worker count; evenly distributed across BDFs in pcie_nvme_addr.
# e.g. pcie_nvme_addr="VF1 VF2" + storage_engine_thread_num=16
#   => 8 qpairs on VF1, 8 qpairs on VF2.
# e.g. pcie_nvme_addr="VF1 VF2" + storage_engine_thread_num=8
#   => 4 qpairs on VF1, 4 qpairs on VF2.
export storage_engine_thread_num=8

# Dedicated read worker pool size. Only used when OXBOW_RD_INLINE_SUBMIT
# is defined (default). Total qpair pool = storage_engine_thread_num +
# read_worker_thread_num. For NUMA1-only pinning on Libra06 (CPU 16..31,
# 16 cores), keep the sum <= 16.
#   Examples (read-heavy tuning):
#     iod=6 + read=10 -> reads favored, still NUMA1-local
#     iod=8 + read=8  -> balanced
#     iod=4 + read=12 -> read-extreme (only if writes are rare)
#
#     3-VF read-heavy D-2 profile:
#   	iod_workers  : tid 0..7   -> CPU 16..23 (NUMA1 primary)
#   	read_workers : tid 8..23  -> CPU 24..31 (NUMA1 primary)
#                              + CPU 48..55 (NUMA1 HT siblings)
export read_worker_thread_num=16

export filesystem="lwext4" # File system. Supported FS: sefs, lwext4
#export filesystem="sefs" # File system. Supported FS: sefs, lwext4
export journal_mode=3      # 1: no journaling, 2: metadata journaling, 3: data journaling #FIXME: not used yet.
export bg_journaling=1 # 1: enable, 0: disable

### Example configuration in Libra06 ###
# export pcie_nvme_addr="0000:d8:00.1"
# export pcie_nvme_addr="0000:d8:00.1 0000:d8:00.2"  # two VFs (whitespace-separated)
# export rpc_rdma_ip_addr="192.168.14.113"

# You can overwrite configs using myconf.sh file which is not tracked by git.
[ -e "${SECURE_DAEMON}/myconf.sh" ] && source "${SECURE_DAEMON}/myconf.sh" || true
