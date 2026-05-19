# IO thread pinning

Oxbow configures IO worker CPU pinning in
`oxbow/secure_daemon/include/common/cpu_pinning.h`.

## Current default

The current default is based on the testbed hardware (DPU and SSD are attached
to the NUMA1 node):

```c
#define OXB_PIN_MODE OXB_PIN_MODE_NUMA1_ONLY
```

This pins IO workers to NUMA1 primary CPUs first, then NUMA1 hyper-thread
siblings when more than 16 workers are used.

## Machine-specific constants

The CPU base values are hardcoded for the current dual-socket
testbed:

```c
#define OXB_NUMA0_BASE 0
#define OXB_NUMA1_BASE 16
#define OXB_NUMA0_HT_BASE 32
#define OXB_NUMA1_HT_BASE 48
```

Before running on a different machine, verify the CPU topology:

```shell
lscpu -e
```

Update these constants or choose another `OXB_PIN_MODE` if CPU numbering,
NUMA ownership, or hyper-thread sibling layout is different.

## Pinning modes

- `OXB_PIN_MODE_SPLIT_NUMA`: split workers across NUMA0 and NUMA1.
- `OXB_PIN_MODE_NUMA0_ONLY`: pin workers to NUMA0 primary CPUs first, then
  NUMA0 HT siblings after the first 16 workers.
- `OXB_PIN_MODE_NUMA1_ONLY`: pin workers to NUMA1 primary CPUs first, then
  NUMA1 HT siblings after the first 16 workers.

## Notes

- `OXB_PIN_MODE_NUMA1_ONLY` assumes NUMA1 exists.
- On machines without NUMA1, or with different CPU numbering, affinity setup can
  fail or workers can be pinned to unintended CPUs.
