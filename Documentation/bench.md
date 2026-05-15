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

This benchmark is different from the [uFS microbenchmark](#ufs-microbenchmark)
described in the paper.

### Compile

```shell
cd bench/micro
./build.sh
```

### Run the benchmark

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

For more details on installing and configuring NFS, refer to the relevant NFS
documentation.

## uFS Microbenchmark

This is the microbenchmark used in the Oxbow paper (OSDI'26).

```shell
cd uFS
git checkout oxbow
```

The `uFS/cfs` directory contains the source code for `uFS` (a file system),
which is not required to run the microbenchmarks. Therefore, you can ignore
failures to fetch its submodules.

### Build

```shell
cd scripts
./cmpl_bench.sh oxbow micro
```

### Configure the run

In `scripts/config.sh`, you can configure the benchmark. For example:

```shell
RUN_LATENCY # 0 to measure throughput, 1 to measure latency.
UFSBENCH_WORKLOADS # Select workloads to run.
UFSBENCH_IOSIZE # I/O size.
UFSBENCH_FILESIZE # Per-process file size.
UFSBENCH_ENABLE_PERF # Enable perf.
...
```

### Run

The script assumes the tmux pane layout described in
[Run Oxbow with scripts](deploy-on-testbed.md#run-oxbow-with-scripts).
Therefore, run the script from the bottom-left pane of that tmux session.

```shell
cd scripts
./run_bench.sh microbench oxbow
```

### Check results

If the benchmark runs successfully, the results are stored in the `DATA`
directory. The symbolic link `DATA_microbench_oxbow_latest` points to the most
recent result directory.

Use `parse_all.sh` to parse the results:

```shell
cd scripts

# Oxbow results:
./parse_all.sh micro oxbow ../DATA/DATA_microbench_oxbow_latest
```

The parsed result files are created in the directory where the results are stored
(e.g., `DATA_microbench_oxbow_latest`).

### Run Ext4

Use the same scripts, but pass `ext4` or `ext4dj` instead of `oxbow`.

```shell
./cmpl_bench.sh ext4 micro # Build.
./run_bench.sh microbench ext4dj # Run Ext4 with data journaling.
./parse_all.sh micro ext4 ../DATA/DATA_microbench_ext4dj_latest
```
