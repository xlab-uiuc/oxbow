# Kernel-Daemon IPC Optimization: Design History and Final Architecture

> Status snapshot (2026-03-23): A+B request path is in use (`epoll + per-inode msg_ring`), and the per-inode ring has been upgraded to a lock-free MPMC slot-sequence queue. Daemon now has a legacy fallback path when msg_ring mmap/header validation fails. SHM GUP cache is implemented with compile-time toggle, and READ_END waiter lookup has been upgraded to O(1)-average lookup in kernel.

## 1. Problem Statement

The original kernel (illufs) to secure_daemon communication uses **epoll/read/ioctl**
per request, incurring up to **4 syscalls per page** on the read_folio path:

1. `epoll_wait()` — daemon waits for events
2. `read(inode_fd)` — daemon reads the request message (`copy_to_user`)
3. `ioctl(READ_END)` — daemon signals completion
4. `epoll_ctl(EPOLL_CTL_MOD)` — EPOLLONESHOT rearm

Each syscall costs ~1-3 us (KPTI overhead) plus scheduling costs.
For 4 KB page reads this IPC overhead dominates latency.

## 2. Approach 1: Full Ring Buffer (SQ + CQ)

### Design

Replace all syscalls with a shared-memory ring buffer (Vyukov MPMC):

- **Submission Queue (SQ)**: kernel -> daemon (read_folio, readahead, notifyopen)
- **Completion Queue (CQ)**: daemon -> kernel (read_end, ra_end)
- 3-stage hybrid polling (busy-spin -> nanosleep -> eventfd/waitqueue block)
- Fast path: **0 syscalls**

### Implementation

- Kernel: `ring.c` with SQ enqueue, CQ dequeue, CQ poller kthread
- Daemon: `ring.c` with SQ dequeue, CQ enqueue
- Gated by `CONFIG_OXBOW_RING_BUFFER` (kernel) / `-DOXBOW_RING_BUFFER` (daemon)
- Runtime switching via `ring_eventfd` sentinel in DAEMON_INIT ioctl

### Code Review Findings


| ID  | Severity | Issue                                                              |
| --- | -------- | ------------------------------------------------------------------ |
| C1  | Critical | SQ full -> infinite sleep (return value of submit_read ignored)    |
| C2  | Critical | CQ full + kthread sleeping -> deadlock (spin without wake)         |
| C3  | Critical | NOTIFYOPEN dropped in Stage 3 race dequeue                         |
| M1  | Medium   | kthread_sleeping false sharing (same cacheline as daemon_sleeping) |
| M2  | Medium   | PERF_OXBOW_KERNEL RA latency measurement missing in ring path      |
| M3  | Medium   | x86-only `_mm_pause()`                                             |
| M4  | Medium   | Debug build left enabled in meson.build                            |
| L1  | Low      | eventfd leak on ioctl failure                                      |
| L2  | Low      | No graceful shutdown for ring poll threads                         |
| L3  | Low      | `ilookup()` per CQ completion (hash lookup + refcount)             |


All issues were fixed. C1-C3 were critical correctness bugs.
L3 was addressed by adding `ilino_ptr` to SQ/CQ entries (kernel pointer echo-back).

### Benchmark Result

**Throughput was WORSE than baseline.**


|               | Base | Ring Buffer |
| ------------- | ---- | ----------- |
| SR Throughput | 800  | 700-750     |
| RR Throughput | 700  | 600-650     |


### Root Cause Analysis

The full ring buffer approach introduced new bottlenecks that outweighed
the syscall savings:

1. **CQ poller kthread is single-threaded bottleneck**: All completions
  from N daemon workers serialized through 1 kthread. In the legacy path,
   each NVMe worker calls `ioctl(READ_END)` independently (N-way parallel).
2. **SQ CAS contention**: 4 daemon poll workers CAS on the same `sq_head`
  cacheline. Legacy epoll uses EPOLLONESHOT for contention-free per-inode
   dispatch.
3. **Busy-polling CPU waste**: 4 SQ workers + 1 CQ kthread = 5 cores
  consumed by busy-polling. Legacy epoll workers sleep when idle.
4. **Extra completion hop**: Legacy: daemon -> ioctl -> direct wake (1 hop).
  Ring: daemon -> CQ enqueue -> kthread -> wake (2 hops + scheduling).

## 3. Approach 2: Hybrid (SQ Ring + ioctl Completion)

### Design

Keep SQ ring buffer for request path, remove CQ and restore ioctl for
completion path:

```
Request:    Kernel -> SQ ring -> Daemon    (0 syscall)
Completion: Daemon -> ioctl(READ_END)      (1 syscall, N-way parallel)
```

### Changes from Full Ring Buffer

- Removed: CQ poller kthread, CQ dequeue, process_read_end, process_ra_end,
OXBOW_IOCTL_CQ_WAKE, all CQ enqueue paths in daemon mpage.c
- Kept: SQ ring, SQ poll workers, eventfd wakeup
- Added: ILLUFS_IOCTL_SET_DAEMON_PTR for O(1) inode dispatch (eliminates
`ihold()` hash lookup + spinlock per SQ event)

### Benchmark Result

**Still worse than baseline** (CQ bottleneck resolved, but SQ issues remain).


|               | Base | Hybrid  |
| ------------- | ---- | ------- |
| SR Throughput | 800  | 630-714 |
| RR Throughput | 700  | 720-735 |


RR improved slightly (CQ removal helped), but SR degraded further.
SQ CAS contention and busy-polling CPU waste were the remaining problems.

## 4. Approach 3: A+B (Per-Inode Shared Memory + Optimized Epoll)

### Design Insight

The legacy epoll path has two valuable properties the ring buffer lost:

- **Per-inode dispatch**: EPOLLONESHOT assigns each inode to exactly one
worker — zero contention
- **CPU efficiency**: workers sleep in `epoll_wait` when idle — zero
busy-polling waste

The A+B approach keeps these properties while eliminating unnecessary syscalls:

**A. Remove EPOLLONESHOT** — eliminates `epoll_ctl(EPOLL_CTL_MOD)` rearm
syscall per batch. Uses EPOLLET-only with test-and-clear I_CLOSING for
thread-safe close handling.

**B. Per-inode shared memory message ring** — eliminates `read()` syscall
entirely on the fast path. Kernel allocates a page per inode, daemon mmaps it.
Kernel writes `oxbow_msg` to a shared lock-free MPMC ring (slot sequence + CAS),
daemon dequeues from it after epoll notification.

### Architecture

```
Kernel: illufs_read_folio()
  |-- add waiter to complet_wqh
  |-- write oxbow_msg to ilino->msg_ring  (shared page, 0 syscall)
  |-- wake_up_poll(fd_wqh)                (triggers epoll)
  |-- schedule()                          (sleep until completion)

Daemon: epoll_wait() returns              (1 syscall per batch)
  |-- read from inode->msg_ring           (shared memory, 0 syscall)
  |-- dispatch: do_readpage / do_readahead
  |-- NVMe I/O
  |-- ioctl(READ_END)                     (1 syscall, N-way parallel)
  |-- kernel wakes sleeping thread
```

### Syscall Comparison

```
                    epoll_wait  read()   ioctl    epoll_ctl  Total
Legacy (per page):   amortized    1       1         1        ~3-4
A+B (per page):      amortized    0       1         0        ~1-2
Hybrid ring:            0         0       1         0          1
```

A+B achieves ~2 syscalls/event (amortized) with:

- CAS contention localized per inode (no global shared request queue)
- Zero busy-poll CPU waste (epoll_wait blocks when idle)
- Natural load balancing (epoll distributes events to idle workers)

### Implementation Details

#### Kernel (`CONFIG_OXBOW_IPC_MSG_RING`)

- `struct oxbow_inode_msg_ring` (per-inode MPMC ring in shared page):
  - `head`, `tail`, `capacity`, `mask`
  - fixed `slots[OXBOW_MSG_RING_CAPACITY]`, each slot has `seq + msg`
- `oxbow_msg_ring_alloc()`: allocates page in `illufs_ioctl_newfile()`
- `oxbow_msg_ring_enqueue()`: CAS tail-claim + slot write + release on slot seq
- `illufs_file_mmap()`: handles OXBOW_MSG_RING_MMAP_PGOFF for daemon mmap
- `illufs_daemon_poll()`: checks msg ring tail != head for EPOLLIN
- `illufs_read_folio()`: waiter on complet_wqh + msg_ring_enqueue + wake_up_poll
- `illufs_readahead()`: msg_ring_enqueue + wake_up_poll (no waiter)

#### Daemon (`-DOXBOW_IPC_MSG_RING`)

- `request_file_init()`: mmap msg ring page from inode fd and validate header
- `file_epoll_loop()`: CAS-based dequeue from `inode->msg_ring` on fast path
- `file_dispatcher_register()`: EPOLLIN | EPOLLET (no EPOLLONESHOT)
- Close path: test-and-clear I_CLOSING to prevent double cleanup
- Fallback:
  - if msg ring mmap is unsupported (EINVAL/ENODEV/EPERM), or ring header is invalid,
    daemon falls back to legacy `read(inode_fd)` draining path

### Benchmark Result (historical, pre-MPMC conversion)

**Best performance across all modes.**


|               | Base   | A+B (Base+) | Improvement |
| ------------- | ------ | ----------- | ----------- |
| SR Throughput | 800    | 830         | +3.7%       |
| RR Throughput | 700    | 858         | +22.6%      |
| SR Latency    | 2.3 us | 2.17 us     | -5.6%       |
| RR Latency    | 4.7 us | 4.5 us      | -4.3%       |


Random read throughput improved by 22.6% due to per-page syscall reduction.
Sequential read improved modestly because readahead amortizes IPC costs.

Note:
- The numbers above were measured before the lock-free MPMC queue conversion.
- After MPMC changes, the system has been build-validated, but runtime
  performance re-measurement is still pending.

## 5. Implementation Status Update (2026-03-22 ~ 2026-03-23)

### 5.1 Implemented: SHM GUP Cache (per-inode PEB cache)

- `illufs_shm_get_page_state()` now supports a compile-time gated cache path:
  - `CONFIG_OXBOW_SHM_GUP_CACHE=y`: cache enabled
  - `CONFIG_OXBOW_SHM_GUP_CACHE=n`: fallback path (per-lookup GUP)
- Cache lifecycle is wired to:
  - inode init/destroy
  - `ILLUFS_IOCTL_NEWFILE` remap boundary (immediate invalidate)
- Locking evolved from mutex to spinlock-based metadata protection with:
  - lockless snapshot + revalidate for fast-hit
  - GUP always outside lock

### 5.2 Implemented: GUP Counter and Dump (debug/profiling)

- `CONFIG_OXBOW_SHM_GUP_COUNTER` controls counter/log code generation:
  - default `n` (disabled)
  - when enabled: `lookup`, `gup`, `gup_fail`, and `hit/miss` (cache-on) are tracked
- Periodic print is rate-limited; final summary is printed on daemon uninit:
  - `illufs_shm_counter_dump("daemon_uninit")`
- Safety/UX:
  - Kconfig explicitly says to disable this option for throughput/latency measurements
  - kernel prints `pr_warn_once(...)` when this option is enabled

### 5.3 Implemented: READ_END O(1)-Average Waiter Lookup

- Previous path in `illufs_ioctl_read_end()` performed O(n) list walk over `complet_wqh`.
- Current path introduces per-inode hash map (`folio -> waiter`) in kernel:
  - waiter is inserted at submit time in `illufs_read_folio()`
  - completion path does direct lookup and wakes matching waiter
  - no userspace protocol change required (still keyed by folio token)
- Scope of this change:
  - improves READ_END lookup cost (mainly read_folio path)
  - does not change RA_END grouping/completion semantics

### 5.4 Validation Snapshot from Current Runs

For the same sequential-read workload (`lookup=262176`):

| Configuration | lookup | gup | hit | miss | Notes |
|---|---:|---:|---:|---:|---|
| GUP cache ON (legacy) | 262176 | 33 | 262143 | 33 | Expected (one miss per touched PEB) |
| GUP cache OFF (legacy) | 262176 | 262176 | - | - | Expected fallback behavior |
| GUP cache ON (legacy, spinlock cache metadata) | 262176 | 33 | 262143 | 33 | Same cache policy result |
| GUP cache ON (msg_ring, spinlock cache metadata) | 262176 | 33 | 262143 | 33 | Same cache policy result |

Interpretation:
- GUP cache behavior is correct and mode-independent.
- Counter values are policy-level indicators; they are not direct throughput numbers.

### 5.5 Implemented: Per-Inode `msg_ring` MPMC Conversion

- Queue data structure upgraded from SPSC-style cursor update to slot-sequence MPMC:
  - slot layout: `seq + msg`
  - ring header: `head`, `tail`, `capacity`, `mask`
- Kernel enqueue path:
  - CAS tail-claim, write message to claimed slot, release-publish by slot `seq`
  - full condition semantics are unchanged (`EAGAIN`, fail-fast path kept)
- Daemon dequeue path:
  - CAS head-claim, read message from claimed slot, release slot back by sequence bump
- Wrap-around safety:
  - signed distance checks (`seq - pos`) are used on both enqueue/dequeue paths

### 5.6 Implemented: Mode-Mismatch Fallback and Error-Path Leak Fix

- Daemon now falls back to legacy `read(inode_fd)` path when:
  - msg_ring mmap is unsupported (`EINVAL`/`ENODEV`/`EPERM`)
  - msg_ring header validation fails (`capacity` power-of-two, `mask == capacity - 1`)
- Kernel leak fix:
  - `oxbow_file_fd()` error path now `fput(file)` before dropping inode/fd, fixing
    the `alloc_file_pseudo()` failure-path reference leak.

## 6. Remaining Optimization Opportunities (Updated Priority)

### High Impact

1. **READ_END batching (selective/adaptive)**: still open.
   - Important caveat: aggressive batching can hurt tail latency.
   - Recommended strategy is adaptive batching (favor RR/high-QD, keep SR latency-safe behavior).
2. **Message-ring wakeup coalescing**: avoid redundant `wake_up_poll()` when ring transitions are already visible.
3. **RA SHM state lookup batching**: reduce per-page function/lock/bit-check overhead in readahead windows.

### Medium Impact

1. **Daemon epoll path lock reduction** (`inode_lock` around event handling) via atomic state operations.
2. **`ra_cnt` accounting simplification** (`spin_lock` -> atomic path).

### Deferred / Not Feasible in Current Design

1. **SPDK DMA -> SHM zero-copy** is currently considered not feasible with the current architecture constraints.

## 7. Current Compile-Time Mode Selection (Updated)

### IPC mode (kernel <-> daemon request path)

| Setting | Request Path | Completion Path | Notes |
|---|---|---|---|
| `CONFIG_OXBOW_IPC_MSG_RING=y` + daemon `-DOXBOW_IPC_MSG_RING` | per-inode msg_ring + EPOLLET | ioctl (`READ_END`/`RA_END`) | Fast path |
| `CONFIG_OXBOW_IPC_MSG_RING=n` + daemon without `-DOXBOW_IPC_MSG_RING` | legacy epoll/read path | ioctl (`READ_END`/`RA_END`) | Baseline-compatible path |
| Mismatch (`daemon -DOXBOW_IPC_MSG_RING`, kernel no ring support) | daemon runtime fallback to legacy read path | ioctl (`READ_END`/`RA_END`) | Warning is logged; service keeps running |

### SHM page-state optimization flags

| Setting | Default | Meaning |
|---|---|---|
| `CONFIG_OXBOW_SHM_GUP_CACHE` | `y` | Enables per-inode SHM PEB cache for `illufs_shm_get_page_state()` |
| `CONFIG_OXBOW_SHM_GUP_COUNTER` | `n` | Enables debug counters/logs/dump for GUP/cache behavior |

Recommended for performance runs:
- `CONFIG_OXBOW_SHM_GUP_COUNTER=n`

## 8. Key Lessons

1. **Eliminating syscalls is not always a net win.** The ring buffer removed
  3 syscalls per page but introduced CAS contention, busy-polling CPU waste,
   and completion serialization that cost more than the syscalls saved.
2. **Per-resource dispatch beats global queues.** Epoll's EPOLLONESHOT
  provides natural per-inode partitioning with zero contention. A global
   MPMC queue forces all workers to compete on shared state.
3. **Completion parallelism matters more than request parallelism.** The CQ
  kthread bottleneck was the single biggest performance issue. N daemon
   workers calling ioctl independently outperforms any single-consumer
   completion queue.
4. **Shared memory for data, syscalls for notification.** The optimal split
  is: transfer request/completion DATA via shared memory (zero-copy), but
   use lightweight syscalls (epoll, ioctl) for NOTIFICATION of state changes.
   Trying to eliminate notification syscalls via busy-polling costs more CPU
   than it saves.
5. **Measure before optimizing.** The initial estimation predicted ring
  buffers would be faster. Only microbenchmarks revealed the hidden costs.

