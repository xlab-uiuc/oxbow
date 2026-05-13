#!/bin/bash
set -e

if [ -z "$OXBOW_ENV_SOURCED" ]; then
	echo "Do source set_env.sh first."
	exit
fi

## To clone all of benchmarks.
# git clone --recurse-submodules -j8 git@github.com:casys-kaist-internal/oxbow.bench.git $BENCH
## Or, with old git,
# git clone git@github.com:casys-kaist-internal/oxbow.bench.git $BENCH
# cd $BENCH
# git submodule update --init --recursive

git clone git@github.com:casys-kaist-internal/oxbow.bench.git $BENCH
echo "Read $BENCH/README.md to download benchmarks."
