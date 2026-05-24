# IO thread pinning

Oxbow configures IO worker CPU pinning in
`oxbow/secure_daemon/include/common/cpu_pinning.h`.

## Current default

Secure Daemon and DevFS have separate pinning policies so host-journaling mode
can run both processes on the host without overlapping IO worker CPUs.

```c
#define OXB_SD_PIN_MODE OXB_PIN_MODE_NUMA1_ONLY
#define OXB_DEVFS_PIN_MODE OXB_PIN_MODE_NUMA0_ONLY
```

With the current testbed CPU numbering, this means:

- Secure Daemon IO workers use NUMA1 primary CPUs first (`16..31`), then NUMA1
  hyper-thread siblings (`48..63`) when more than 16 workers are used.
- DevFS IO workers use NUMA0 primary CPUs first (`0..15`), then NUMA0
  hyper-thread siblings (`32..47`) when more than 16 workers are used. However,
  DevFS typically uses up to 8 cores.

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

Update these constants or choose different `OXB_SD_PIN_MODE` /
`OXB_DEVFS_PIN_MODE` values if CPU numbering, NUMA ownership, or hyper-thread
sibling layout is different.

## Pinning modes

- `OXB_PIN_MODE_SPLIT_NUMA`: split workers across NUMA0 and NUMA1.
- `OXB_PIN_MODE_NUMA0_ONLY`: pin workers to NUMA0 primary CPUs first, then
  NUMA0 HT siblings after the first 16 workers.
- `OXB_PIN_MODE_NUMA1_ONLY`: pin workers to NUMA1 primary CPUs first, then
  NUMA1 HT siblings after the first 16 workers.

## Runtime users

- Secure Daemon IOD workers use the secure-daemon policy.
- Secure Daemon read workers use the secure-daemon policy.
- Secure Daemon SPDK EAL core mask is generated from the secure-daemon policy.
- DevFS SPDK IO/RPC workers use the DevFS policy.
- DevFS SPDK EAL core mask is generated from the DevFS policy.

## Notes

- `OXB_PIN_MODE_NUMA1_ONLY` assumes NUMA1 exists.
- In host-journaling mode, keep the Secure Daemon and DevFS policies disjoint
  unless an experiment intentionally wants CPU overlap.
- On machines without NUMA1, or with different CPU numbering, affinity setup can
  fail or workers can be pinned to unintended CPUs.
