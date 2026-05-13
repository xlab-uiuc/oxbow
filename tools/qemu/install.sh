#!/bin/bash
QEMU_VERSION="7.2.1"

# Install required packages
sudo apt install -y libglib2.0-dev libpixman-1-dev libslirp-dev

# Download qemu source code.
wget https://download.qemu.org/qemu-$QEMU_VERSION.tar.xz
tar xvJf qemu-$QEMU_VERSION.tar.xz

# Build qemu.
cd qemu-$QEMU_VERSION || exit
./configure --disable-rbd --target-list=x86_64-softmmu
make -j

# Add binary.
mkdir -p ../../bin
ln -sf ../qemu/qemu-$QEMU_VERSION/build/qemu-system-x86_64 ../../bin
ln -sf ../qemu/qemu-$QEMU_VERSION/build/qemu-img ../../bin
