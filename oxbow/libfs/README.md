# Oxbow LibFS README

## Compile LibFS library

```shell
cd oxbow/libfs
./build.sh
```

It builds the LibFS library and test programs that use LibFS (`oxbow/libfs/build/test/`).

## Run tests

```shell
# To execute one test program.
cd oxbow/libfs
./run.sh build/test/PROGRAM

# To run all the tests (not supported yet).
cd oxbow/libfs
meson test -C build
```

## Development

### Formatter

You can use the auto format feature of VSCode. Please use `oxbow/libfs/.clang-format` file. This file is from the Linux kernel source code.
