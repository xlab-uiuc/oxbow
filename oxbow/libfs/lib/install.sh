#!/bin/bash

# Check env vars are set.
if [[ -z "${OXBOW_ROOT}" ]]; then
	echo "Set env variables first by executing 'source set_env.sh' at project root directory."
	exit 1
fi

# Install syscall_intercept
sudo apt install -y pkg-config libcapstone-dev cmake
(
	cd syscall_intercept || exit
	mkdir -p build
	mkdir -p install
	cd build || exit
	cmake -DCMAKE_INSTALL_PREFIX=$SYSCALL_INTERCEPT_INSTALL -DCMAKE_BUILD_TYPE=Release ..
	make
	make install
)

# Install spdk
# Refer to https://spdk.io/news/2019/05/06/nvme/ for the options: --enable-lto
(
	cd spdk || exit
	sudo ./scripts/pkgdep.sh --rdma
	./configure --with-shared --with-rdma # --enable-lto
	make -j "$(nproc)"
)
# --enable-lto <=== this option on ./configure make problem in VM
# --with-rdma is for target.
# sudo apt install -y libibverbs-dev librdmacm-dev
# spdk library not compiled on gcc-10 due to xor, recommend gcc-11

# If you are using ubuntu 24.04 LTS machine you will see ./scripts/pkgdep.sh not work
# You have to manually install
# I found these are nessesary
# sudo pip install pyelftools --break-system-packages
# sudo apt install libnuma-dev



# Install rdma-core
# [WARNING] If you build this in virtual machine, below packages can affect the VM images. See, github issue.
# sudo apt install build-essential cmake gcc libudev-dev libnl-3-dev libnl-route-3-dev ninja-build pkg-config valgrind python3-dev cython3 python3-docutils pandoc
(
	cd rdma-core || exit
	./build.sh
)

# Install BitArray library.
(
	cd BitArray || exit
	make
)

# Install Oxbow RPC
(
	cd oxbow-rpc/lib || exit
	./install.sh
	cd ..
	./build.sh
)

# Install Oxbow Data Fetcher.
(
	cd data_fetcher/lib || exit
	./install.sh
	cd ..
	./build.sh
)

