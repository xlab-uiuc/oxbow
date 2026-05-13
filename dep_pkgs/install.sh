#!/bin/bash
sudo apt install build-essential python3 re2c libacl1-dev
mkdir -p bin

(
	cd meson || exit
	git checkout 0.63.3
	ln -s ../meson/meson.py ../bin/meson
)

(
	cd ninja || exit
	git checkout v1.11.1
	./configure.py --bootstrap
	mkdir build-cmake
	cmake -Bbuild-cmake
	ln -s ../ninja/ninja ../bin/ninja
)
