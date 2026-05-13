#!/bin/bash

TARGETS=(
  # "dep_pkgs/meson"
  # "dep_pkgs/ninja"
  "oxbow/libfs/lib/BitArray"
  "oxbow/libfs/lib/c-thread-pool"
  "oxbow/libfs/lib/data_fetcher"
  "oxbow/libfs/lib/log.c"
  "oxbow/libfs/lib/oxbow-rpc"
  "oxbow/libfs/lib/rdma-core"
  "oxbow/libfs/lib/spdk"
  "oxbow/libfs/lib/syscall_intercept"
)

for target in "${TARGETS[@]}"; do
  git submodule update --init --recursive "$target"
done
