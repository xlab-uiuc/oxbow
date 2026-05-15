# OSDI 2026 Artifact Evaluation

We provide two ways to evaluate Oxbow.

1. **Running in a virtual machine.** We provide instructions for running Oxbow
   in a virtual machine without dedicated devices. You can try running simple
   microbenchmarks. However, in this setup, the device component (D-Server or
   DevFS) runs on the host and writes to its own SSD. Therefore, data must be copied
   between two different (virtual) SSDs to perform checkpointing, an operation
   that publishes data from a journal area to a file system area. Please refer to
   [Deploy Oxbow in a VM](deploy-in-vm.md) for the instructions.

2. **Running in the testbed.** We will make our testbed machine available during
   the AE period. You can deploy Oxbow on a real machine with an NVIDIA
   BlueField-2 DPU and run benchmarks there. To access the machine, follow the
   instructions in [README for Artifact Evaluation
   Reviewers](https://docs.google.com/document/d/e/2PACX-1vQv4cdO8hSZcd7KNu35hipq63KcVYA-DrLpTTg1FADdu13oBSjg1KqYNW7LpuBTmOX_eMDuK8bVBbli/pub).
   The instructions for deploying Oxbow on our testbed are described in [Deploy
   Oxbow on the AE Testbed](deploy-on-testbed.md).

## uFS Microbenchmark

See [bench.md](bench.md) for instructions on running these benchmarks. This
section lists the configurations used to run the benchmarks as described in the
paper.

### Throughput

This benchmark covers the throughput microbenchmark (Figure 9), CPU consumption
(Figure 10), and fsync latency (Table 1).

Confirm that the `uFS/scripts/config.sh` file is set as follows.

```shell
RUN_LATENCY="0"
```

You can change the throughput microbenchmark configuration in the same file. The
configuration used in the paper is set by default.

```shell
if [ "$RUN_LATENCY" = "1" ]; then
        # Latency benchmark configurations:
        ...
else
        # Throughput benchmark configurations:
        export UFSBENCH_WORKLOADS="ADPS,WDPR,WDPS,RDPR,RDPS"

        export UFSBENCH_IOSIZE="4K"

        # Maximum number of concurrent benchmark applications.
        export UFSBENCH_NUMAPP="16"

        # File size (per file for throughput benchmarks).
        export UFSBENCH_FILESIZE=$((2 * 1024 * 1024 * 1024)) # 2GB (Throughput)
        ...
fi
```

### Latency

This benchmark covers the latency microbenchmark (Figure 8).

Confirm that the `uFS/scripts/config.sh` file is set as follows.

```shell
RUN_LATENCY="1"
```

You can change the latency microbenchmark configuration in the same file. The
configuration used in the paper is set by default.

```shell
if [ "$RUN_LATENCY" = "1" ]; then
        # Latency benchmark configurations:
        export UFSBENCH_WORKLOADS="ADPS_L,WDPS_L,WDPR_L,RDPR_L,RDPS_L"

        export UFSBENCH_IOSIZE="1K,4K,16K,64K,256K,512K"

        # Total file size for the latency benchmarks.
        export UFSBENCH_LAT_TOTAL_SIZE=$((1 * 1024 * 1024 * 1024)) # 1GB
        ...
else
        # Throughput benchmark configurations:
        ...
fi
```

## More benchmarks will be added
