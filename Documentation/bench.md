# Run Benchmarks

## Source environment variables

The environment variables must be sourced before running benchmarks. If you have
not already done so, source them as follows:

```shell
source set_env.sh
```

## Download benchmarks

Run the following script to clone the benchmark root repository into `bench/`.

```shell
scripts/download_bench.sh
```

You can download all benchmarks at once or download each benchmark individually.

```shell
cd bench
# Download all benchmarks.
git submodule update --recursive --init -j 4

# Or download one benchmark at a time (micro is an example path).
git submodule update --recursive --init micro
```

Reference: `bench/README.md`.

## Microbenchmark

This benchmark is different from [uFS microbenchmark](#ufs-microbench) that is
described in the paper.

### Compile

```shell
cd bench/micro
./build.sh
```

### Run

You must pass `$OXBOW_PREFIX` (for example, `/oxbow` as defined in `set_env.sh`)
as the file path prefix. Refer to the usage information for other options. For
example, to run the latency benchmark:

```shell
cd oxbow/libfs
./run.sh $BENCH_MICRO/build/lat_micro -d $OXBOW_PREFIX -s sw 256M 4K 1
```

or

```shell
cd bench/micro
$LIBFS/run.sh build/lat_micro -d $OXBOW_PREFIX -s sw 256M 4K 1
```

To run the throughput benchmark:

```shell
cd oxbow/libfs
./run.sh $BENCH_MICRO/build/tput_micro -d $OXBOW_PREFIX -s ap 1G 4K 1
```

or

```shell
cd bench/micro
$LIBFS/run.sh build/tput_micro -d $OXBOW_PREFIX -s ap 1G 4K 1
```

## Mount for automation

The scripts for running the experiments described in the paper check the state of
Secure Daemon and DevFS using files in the NFS-mounted directory.

(For OSDI 2026 Artifact Evaluation reviewers: this is already configured in the
provided testbed.)

### Host

Configure an NFS directory at `/mnt/oxbow_flag`.

For example, add the following line to `/etc/exports`.

```shell
/mnt/oxbow_flag *(rw,no_root_squash,sync,insecure,no_subtree_check,no_wdelay)
```

Then run `sudo exportfs -rv` to apply the changes.

### Device

Mount the NFS directory at `/mnt/oxbow_flag`. For example:

```shell
sudo mount -t nfs libra06-rdma2:/mnt/oxbow_flag /mnt/oxbow_flag
```

For more details on installing and configuring NFS, refer to external materials.

## uFS Microbench

(To be updated soon.)
