# Ring Buffer Communication Channel (illufs ↔ secure_daemon)

## 1. Overview

### Problem
The original kernel–daemon communication uses **epoll/read/ioctl** per request,
incurring 3 syscalls per page on the read_folio path:

1. `wake_up_poll()` → daemon `epoll_wait()` returns
2. daemon `read(inode_fd)` → dequeues the request
3. daemon `ioctl(READ_END)` → signals completion

Each syscall costs ~1–3 μs (KPTI overhead), plus scheduling/context-switch
costs (~2–5 μs per transition). For 4 KiB page reads this dominates latency.

### Solution
A **shared-memory ring buffer** (inspired by io_uring / NVMe) replaces all
three syscalls with lock-free MPMC enqueue/dequeue on shared memory:

- **Submission Queue (SQ)**: kernel → daemon (read_folio, readahead, notifyopen)
- **Completion Queue (CQ)**: daemon → kernel (read_end, ra_end)

Fast-path operations require **zero syscalls**. Syscalls only occur during
sleep/wake transitions (eventfd write for SQ wake, ioctl for CQ wake).

### Performance Expectation
| Metric             | Legacy (epoll/ioctl) | Ring Buffer |
|--------------------|----------------------|-------------|
| Syscalls / page    | 3                    | 0 (fast path) |
| Per-syscall cost   | ~1–3 μs (KPTI)      | N/A         |
| Context switches   | 2 per request        | 0 (fast path) |
| Scheduling cost    | ~2–5 μs per switch   | N/A         |

## 2. Architecture

### Ring Buffer Layout (shared mmap region)

```
┌─────────────────────────────────────────┐
│  Ring Header (page-aligned)             │
│  ├─ magic, version, capacities, offsets │
│  ├─ sq_head (daemon), sq_tail (kernel)  │  ← each on own cacheline
│  ├─ cq_head (kernel), cq_tail (daemon) │
│  └─ daemon_sleeping, kthread_sleeping   │  ← doorbell flags
├─────────────────────────────────────────┤
│  SQ Array [4096 entries]                │  kernel produces, daemon consumes
├─────────────────────────────────────────┤
│  CQ Array [4096 entries]                │  daemon produces, kernel consumes
└─────────────────────────────────────────┘
```

Total mmap size: ~331,776 bytes (header page + SQ + CQ, all page-aligned).

### Lock-Free Algorithm
Uses **Vyukov's MPMC bounded queue** with per-slot sequence numbers:
- Enqueue: CAS on tail, store data, release sequence
- Dequeue: CAS on head, load data, release sequence
- No locks, no ABA problem, cache-friendly

### Current Per-Inode `msg_ring` ABI (`OXBOW_IPC_MSG_RING`)
The currently integrated fast path uses **one shared page per inode** for
kernel -> daemon messages. The page stores global positions (`head`/`tail`)
and fixed-size slots with sequence counters:

```c
#define OXBOW_MSG_RING_CAPACITY 64

struct oxbow_inode_msg_slot {
    u32 seq;
    u32 pad;
    struct oxbow_msg msg;
};

struct oxbow_inode_msg_ring {
    u32 head;
    u32 tail;
    u32 capacity; /* power-of-two */
    u32 mask;     /* capacity - 1 */
    struct oxbow_inode_msg_slot slots[OXBOW_MSG_RING_CAPACITY];
};
```

Implementation notes:
- Queue operations use lock-free MPMC CAS loops in both kernel and daemon.
- `seq` signed-distance checks are used to stay correct across `u32` wrap-around.
- If daemon cannot map/validate this ring (e.g., mode mismatch), daemon falls
  back to the legacy `read(inode_fd)` path.

### 3-Stage Hybrid Polling (both SQ and CQ consumers)
To minimize CPU usage while maintaining low latency:

```
Stage 1: Busy-spin (128 iterations)         → ~0.1 μs latency
Stage 2: Short sleep (500 × 1 μs nanosleep) → ~1 μs latency
Stage 3: Block on eventfd/waitqueue          → ~5–10 μs latency
```

After any work is done, the consumer resets to Stage 1.

## 3. SQ Events (kernel → daemon)

| Event                        | Code   | Description                          | CQ Response |
|------------------------------|--------|--------------------------------------|-------------|
| `ILLUFS_FILEWORKER_READ`     | 0x13   | Single page read (read_folio)        | READ_END    |
| `ILLUFS_FILEWORKER_RA`       | 0x12   | Readahead (multi-page)               | RA_END      |
| `ILLUFS_FILEWORKER_NOTIFYOPEN` | 0x17 | File open notification (fire-and-forget) | None     |

### SQ Entry Structure
```c
struct oxbow_sq_entry {
    u32 seq;            /* Vyukov MPMC sequence tag */
    u8  event;
    u8  pad[3];
    u64 ino;            /* inode number */
    union {
        struct { u64 folio; u64 idx; } read;
        struct { u64 ractl; u64 idx; u32 nr_pages; u32 pad; } readahead;
        struct { s32 fd; s32 pid; } notify_open;
    } arg;
};
```

## 4. CQ Events (daemon → kernel)

| Event              | Code | Description                         |
|--------------------|------|-------------------------------------|
| `OXBOW_CQ_READ_END` | 0x01 | Single page read complete          |
| `OXBOW_CQ_RA_END`   | 0x02 | Readahead batch complete           |

### CQ Entry Structure
```c
struct oxbow_cq_entry {
    u32 seq;
    u8  event;
    u8  pad[3];
    u64 ino;
    union {
        struct { u64 folio; } read_end;
        struct { u64 ractl; u64 index; u32 nr_pages; u32 success; } ra_end;
    } arg;
};
```

## 5. Sleep/Wake Mechanism

### SQ Wake (kernel → daemon)
When kernel enqueues to SQ and finds `daemon_sleeping == 1`:
1. Kernel calls `eventfd_signal(daemon_eventfd_ctx, 1)`
2. Daemon wakes from `read(eventfd)` in Stage 3
3. Daemon sets `daemon_sleeping = 0` and resumes polling

### CQ Wake (daemon → kernel)
When daemon enqueues to CQ and finds `kthread_sleeping == 1`:
1. Daemon calls `ioctl(manager_fd, OXBOW_IOCTL_CQ_WAKE)`
2. Kernel's CQ poller kthread wakes from `wait_event_interruptible()`
3. Kthread sets `kthread_sleeping = 0` and resumes polling

**Design note**: CQ wake uses ioctl (not direct eventfd) because
`struct eventfd_ctx` internals are not exposed to kernel modules.
This is acceptable because CQ wake only happens in the slow path
(kthread was already sleeping = low-frequency scenario).

## 6. Initialization Sequence

### Kernel Side (fs.c → ring.c)
```
daemon_init() receives ring_eventfd from daemon
  → oxbow_ring_alloc()        : vmalloc_user() shared memory
  → eventfd_ctx_fdget()       : get daemon's SQ eventfd context
  → oxbow_ring_set_daemon_eventfd()
  → copy_to_user(ring_mmap_size)  : tell daemon the mmap size
  → oxbow_ring_start_poller() : start CQ poller kthread
```

### Daemon Side (dir_ops.c → ring.c → file_ops.c)
```
do_daemon_init()
  → eventfd(0, EFD_CLOEXEC)              : create SQ eventfd
  → ioctl(DAEMON_INIT, {ring_eventfd=N})  : pass fd to kernel
  → kernel returns ring_mmap_size
  → oxbow_daemon_ring_init(manager_fd, mmap_size)
      → mmap(MAP_SHARED, manager_fd, OXBOW_RING_MMAP_PGOFF)
      → verify magic, parse header
start_file_dispatcher()  (called AFTER ring init)
  → oxbow_daemon_ring_is_ready() == true
  → launch ring poll workers (not epoll workers)
```

**Critical**: `start_file_dispatcher()` must be called AFTER
`oxbow_daemon_ring_init()`. The original code called `file_dispatcher_init()`
inside `init_file_workers()` which ran BEFORE ring init, causing the daemon
to fall back to epoll mode.

## 7. Compile-Time & Runtime Configuration

### Kernel
- Kconfig option: `CONFIG_OXBOW_RING_BUFFER` (bool, depends on `OXBOW_ILLUFS`)
- Enable: `make menuconfig` → File systems → Oxbow Ring Buffer
- `ring.o` is conditionally compiled via `illufs-$(CONFIG_OXBOW_RING_BUFFER) += ring.o`

### Daemon
- Meson build flag: `'-DOXBOW_RING_BUFFER'` in `c_args` of `meson.build`
- To disable: remove the flag from `c_args` array and rebuild

### Runtime Switching
No kernel rebuild needed to switch modes. The daemon controls the mode:
- **Ring buffer enabled**: daemon sends `ring_eventfd >= 0` in DAEMON_INIT
- **Ring buffer disabled**: daemon sends `ring_eventfd = -1`
- Kernel checks `if (daemon_init.ring_eventfd >= 0)` to decide

The `dofdio_daemon_init` struct always includes `ring_eventfd` and
`ring_mmap_size` fields (unconditional) to avoid ioctl size mismatches.

## 8. Modified Files

### Kernel (`oxbow/linux-kernel/`)
| File | Change |
|------|--------|
| `include/uapi/linux/oxbow/oxbow_kernel.h` | SQ/CQ entry structs, ring header, constants |
| `include/uapi/linux/illufs_diropsfd.h` | `dofdio_daemon_init` + ring fields, `OXBOW_IOCTL_CQ_WAKE` |
| `fs/Kconfig` | `CONFIG_OXBOW_RING_BUFFER` option |
| `fs/illufs/Makefile` | Conditional `ring.o` compilation |
| `fs/illufs/ring.c` | **NEW** — MPMC ring ops, CQ poller kthread, SQ submit helpers |
| `fs/illufs/illufs.h` | Ring API declarations |
| `fs/illufs/fs.c` | `daemon_init()` ring setup, `OXBOW_IOCTL_CQ_WAKE` handler |
| `fs/illufs/file.c` | `read_folio`/`readahead`/`__file_notify_open` → SQ path, `illufs_manager_mmap` |
| `fs/illufs/wake.c` | `illufs_waker_file_notify_open` → SQ path |
| `fs/illufs/diropsfd.c` | Manager fd opened with `O_RDWR` for mmap |

### Daemon (`oxbow/secure_daemon/`)
| File | Change |
|------|--------|
| `include/kernfs_ring.h` | **NEW** — Ring API declarations |
| `src/kernfs/ring.c` | **NEW** — MPMC ring ops (userspace C11 atomics), mmap, CQ enqueue |
| `src/kernfs/file_ops.c` | `file_ring_poll_loop`, register/deregister skip epoll |
| `src/kernfs/dir_ops.c` | `do_daemon_init()` eventfd + ring init |
| `src/fs/mpage.c` | `end_io_no_bio`, `mpage_end_io_noread`, `mpage_end_io_failed` → CQ path |
| `src/secure_daemon.c` | Init order: `start_file_dispatcher()` after `init_fs_manager()` |
| `include/kernfs.h` | `start_file_dispatcher()` declaration |
| `include/fs/fs.h` | `end_io_no_bio()` signature update (added ino param) |
| `meson.build` | `ring.c` source, `-DOXBOW_RING_BUFFER` flag |
| `test/ring_buffer_test.c` | **NEW** — Standalone MPMC algorithm unit test |

## 9. Message Flow (read_folio example)

```
Application: read(file, buf, 4096)
    │
    ▼
Kernel VFS → illufs_read_folio()
    │  ┌─ add waiter to complet_wqh
    │  ├─ oxbow_ring_submit_read()  ← SQ enqueue (lock-free)
    │  │    └─ if daemon_sleeping: eventfd_signal()
    │  └─ schedule()                ← sleep until CQ completes
    │
    ▼
Daemon: file_ring_poll_loop()
    │  ├─ oxbow_daemon_sq_try_dequeue() ← SQ dequeue (lock-free)
    │  ├─ do_readpage()
    │  │    └─ NVMe read via SPDK
    │  └─ end_io_no_bio()
    │       └─ oxbow_daemon_cq_read_end() ← CQ enqueue (lock-free)
    │            └─ if kthread_sleeping: ioctl(CQ_WAKE)
    │
    ▼
Kernel: oxbow_cq_poller kthread
    │  ├─ oxbow_cq_try_dequeue()    ← CQ dequeue (lock-free)
    │  └─ process_read_end()
    │       ├─ folio_mark_uptodate()
    │       ├─ folio_unlock()
    │       └─ wake waiter          ← application resumes
    │
    ▼
Application: read() returns with data
```

## 10. Debugging

### Log Tags (init path)
Kernel (`dmesg`):
- `[RING_INIT 1/5]` ~ `[RING_INIT 5/5]` — ring alloc, eventfd, mmap, poller

Daemon (daemon log):
- `[RING_INIT 1/4]` ~ `[RING_INIT 4/4]` — eventfd, ioctl, mmap, ready check
- `[RING_MMAP 1/3]` ~ `[RING_MMAP 3/3]` — mmap details
- `[RING_DISPATCH]` — ring vs epoll mode decision

### Log Tags (message path, first occurrence only)
Kernel:
- `[MSG] first SQ READ ino=...` — first SQ read enqueue
- `[MSG] first SQ RA ino=...` — first SQ readahead enqueue
- `[MSG] first SQ NOTIFYOPEN ino=...` — first SQ notifyopen enqueue
- `first CQ dequeue event=...` — CQ poller first dequeue
- `[MSG] first READ_END wake OK` — first successful read completion
- `[MSG] first RA_END ino=...` — first RA completion

Daemon:
- `ring: first SQ dequeue event=...` — first SQ dequeue
- `ring: first CQ READ_END ino=...` — first CQ read enqueue
- `ring: first CQ RA_END ino=...` — first CQ RA enqueue

### Debugging Stuck Scenarios
If benchmark hangs, check logs in order:
1. Init logs present? → Ring initialized OK
2. `first SQ READ` in dmesg? → Kernel is submitting
3. `first SQ dequeue` in daemon log? → Daemon is receiving
4. `first CQ READ_END` in daemon log? → Daemon completed I/O
5. `first CQ dequeue` in dmesg? → Kernel received completion
6. `first READ_END wake OK` in dmesg? → Waiter woken up

## 11. Known Issues & Fixes Applied

| Issue | Root Cause | Fix |
|-------|-----------|-----|
| mmap failed (EACCES) | Manager fd opened O_RDONLY | Changed to O_RDWR in diropsfd.c |
| ioctl DAEMON_INIT failed (size mismatch) | Conditional ring fields in struct | Made fields unconditional, use -1 sentinel |
| Benchmark stuck (eventfd mismatch) | Daemon created new eventfd instead of using registered one | Added `oxbow_daemon_ring_set_sq_eventfd()` |
| Daemon crash in file_dispatcher_register | epoll_ctl on fd=-1 in ring mode | Skip epoll_ctl when ring ready |
| Daemon used epoll despite ring enabled | file_dispatcher_init called before ring_init | Reordered: start_file_dispatcher() after init_fs_manager() |
| mpage_end_io_noread used ioctl | Missing ring path for RA shortcuts | Added CQ path to mpage_end_io_noread/failed |
| NOTIFYOPEN events lost | epoll disabled but NOTIFYOPEN still used wake_up_poll | Added NOTIFYOPEN to SQ events |

## 12. Patches

- `kernel_ring_buffer_full.patch` — All kernel changes (modified + new files)
- `daemon_ring_buffer_full.patch` — All daemon changes (modified + new files)

Apply from repo root:
```bash
# Kernel (apply in linux-kernel submodule)
cd oxbow/linux-kernel
git apply ../../kernel_ring_buffer_full.patch

# Daemon (apply in repo root)
cd /path/to/oxbow
git apply daemon_ring_buffer_full.patch
```

## 13. Build & Configuration

```bash
# Kernel: enable CONFIG_OXBOW_RING_BUFFER via menuconfig, then build
cd oxbow/linux-kernel
make menuconfig   # File systems → Oxbow Ring Buffer = Y
make -j$(nproc)

# Daemon: OXBOW_RING_BUFFER is enabled by default in meson.build c_args
cd oxbow/secure_daemon
./build.sh re

# To disable ring buffer (daemon only, no kernel rebuild):
# Remove '-DOXBOW_RING_BUFFER' from c_args in meson.build, rebuild
```
