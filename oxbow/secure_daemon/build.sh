#!/bin/bash -e
PKG_CONFIG_DIR="${SPDK_INSTALL}/lib/pkgconfig"
BUILD_DIR="build"

if [ "$1" = "re" ]; then
	rm -rf $BUILD_DIR
fi

if [ ! -d $BUILD_DIR ]; then
	meson setup $BUILD_DIR -Dpkg_config_path="$PKG_CONFIG_DIR"
fi
meson compile -vC $BUILD_DIR

# sudo "$OXBOW_ROOT"/dep_pkgs/bin/ninja test -C $BUILD_DIR
