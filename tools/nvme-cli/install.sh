#!/bin/bash

# Download nvme-cli.
git clone https://github.com/linux-nvme/nvme-cli.git
cd nvme-cli || exit
git checkout v2.5

# Build.
meson setup --force-fallback-for=libnvme .build
meson compile -C .build

# Add to bin.
mkdir -p ../../bin
ln -sf ../nvme-cli/nvme-cli/.build/nvme ../../bin
