#!/bin/bash
#set -xe
PKG_CONFIG_DIR="${SPDK_INSTALL}/lib/pkgconfig"
BUILD_DIR="build"

if [ "$1" = "re" ]; then
	rm -rf $BUILD_DIR
fi

if [ ! -d $BUILD_DIR ]; then
	# meson setup $BUILD_DIR -Dpkg_config_path=$PKG_CONFIG_DIR -Db_asneeded=false
	meson setup $BUILD_DIR -Dpkg_config_path="$PKG_CONFIG_DIR"
	# meson init --name oxbow_libfs -f --build
	# meson init --name oxbow_libfs -l c -f src
fi

# meson compile -C $BUILD_DIR
meson compile -vC $BUILD_DIR
#sudo "$OXBOW_ROOT"/dep_pkgs/bin/ninja test -C $BUILD_DIR
