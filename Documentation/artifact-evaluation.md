# OSDI 2026 Artifact Evaluation

**This document will be updated soon.**

We provide two ways to evaluate Oxbow.

1. **Running in a virtual machine.** We provide instructions for running Oxbow
   in a virtual machine without dedicated devices. You can try running simple
   microbenchmarks. However, in this setup, the device component (D-Server or
   DevFS) runs on the host and writes to its own SSD. Hence, data must be copied
   between two different (virtual) SSDs to perform checkpointing, an operation
   that publishes data from a journal area to a file system area. Please refer to
   [Deploy Oxbow in a VM](deploy-in-vm.md) for the instructions.

2. **Running in the testbed.** We will make our testbed machine available during
   the AE period. You can deploy Oxbow on a real machine with an NVIDIA
   BlueField-2 DPU and run benchmarks there. To access the machine, follow the
   instructions in [README for Artifact Evaluation
   Reviewers](https://docs.google.com/document/d/e/2PACX-1vQv4cdO8hSZcd7KNu35hipq63KcVYA-DrLpTTg1FADdu13oBSjg1KqYNW7LpuBTmOX_eMDuK8bVBbli/pub).
   The instructions for deploying Oxbow in our testbed are described in [Deploy
   Oxbow on the AE Testbed](deploy-on-testbed.md).

## uFS Microbench - Throughput

This benchmark covers Figure 9 (Throughput), Figure 10 (CPU consumption), and
Table 1 (fsync latency).

(To be updated soon.)

## uFS Microbench - Latency

This benchmark covers Figure 8.

(To be updated soon.)

## More benchmarks will be added
