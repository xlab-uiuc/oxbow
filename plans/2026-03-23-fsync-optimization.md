# 2026-03-23 fsync Optimization Plan

## Objective

- Reduce `fsync` latency (p50/p95/p99) in `secure_daemon`, and improve throughput for large `fsync` workloads.
- Reduce blocking sections in the NVMe write path while preserving crash consistency guarantees.
- Prioritize changes by **impact > risk > implementation complexity**.

## Scope

- In scope: `oxbow/secure_daemon/src/fs/sync.c`, `oxbow/secure_daemon/src/io/nvme.c`, `oxbow/secure_daemon/src/io_dispatcher.c`
- Out of scope: user API semantic changes (weakened `fsync` completion semantics) and filesystem format changes

## Current Bottleneck Summary

- In the staging path, when the sequence buffer is full, the following path waits synchronously:
  - `sync.c:resv_blk()` -> `nvme_direct_write(...)` -> `nvme_direct_write_async_wait_complete(...)`
- As a result, large `fsync` workloads cannot fully overlap copy/submit/poll, and repeated stalls occur in the middle.
- Descriptor/commit blocks are already submitted asynchronously and waited at the end, but intermediate reclaim writes are still effectively synchronous.

---

## Prioritized Improvement Plan

## Current Status (2026-03-23)

- **Done**: P0 async reclaim conversion with in-order slot safety and request accounting.
- **Deferred**: P1(1) split write-dedicated submit/poll pipeline due to architecture mismatch with the current direct fsync I/O handling model (RPC handler thread directly performs I/O).
- **Skipped for now**: P1(2) metadata batch flush, because its direct impact is mostly on the no-journal path, while current focus is journal-on `stage_file()`.
- **Skipped for now**: P2/P3 items, to avoid scope creep and close fsync optimization work at this point.
- **Next step**: Revisit deferred items later when engineering bandwidth is available.

---

## 1) P0 - Safe async conversion of stage reclaim writes (highest priority)

Detailed implementation plan: [2026-03-23-fsync-p0-implementation.md](./2026-03-23-fsync-p0-implementation.md)

### Background

- Today, reclaim writes in `resv_blk()` wait for synchronous completion right before sequence buffer reuse.
- This section repeats on large `fsync` operations and drives up end-to-end latency.

### Goal

- Convert reclaim writes to asynchronous submission while preserving safe buffer reuse.
- Guarantee completion of all in-flight writes before the final `fsync` response.

### Design Direction

1. **Issue intermediate reclaim writes asynchronously**
  - Use `nvme_direct_write_async(...)` instead of `nvme_direct_write(...)`
2. **Fix slot policy to in-order**
  - Slot allocation uses in-order ring advance from `head`
  - Completion may arrive out-of-order due to NVMe behavior
  - Slot reuse is allowed only by in-order reclaim from `tail` (contiguous completed region only)
3. **Protect buffer reuse**
  - Track busy/free state for each sequence slot
  - Never reuse a slot before completion
  - If free slots are insufficient, do partial drain with `stage_io_drain_until_free(...)`
4. **Unify `nr_total_reqs` and reclaim request accounting**
  - Manage reclaim issue, final data issue, and desc/commit issue in one cumulative counter model
  - Wait for only exact outstanding requests at the final barrier
5. **Strengthen error handling**
  - Record completion errors in `stage_tx`
  - Propagate errors at the final barrier and fail fast

### In-order Slot Policy Details

- In this P0 scope, slot reuse safety has the highest priority, and free-list based out-of-order reuse is not introduced.
- State transition:
  - `head` allocates submit slots in order
  - Each slot completion may arrive non-sequentially
  - `tail` advances only over contiguous completed slots
- Key invariants:
  - If any slot before `tail` is incomplete, no slot after it can be reused.
  - A `stage_tx` is not complete until `busy_slots == 0`.

### `nr_total_reqs` and reclaim request accounting details

- Previously, the local variable `nr_total_reqs` represented only requests submitted in a narrow section, which was not robust enough to include intermediate reclaim async operations.
- In P0, maintain the following per-`stage_tx` counters:
  - `req_issued_total`: total requests issued by this `stage_tx`
  - `req_completed_total`: total requests completed for this `stage_tx`
  - `req_issued_reclaim`: requests issued by reclaim path in `resv_blk()`
  - `req_issued_tail_data`: requests for final pending data issue
  - `req_issued_desc`: requests for desc block issue
  - `req_issued_commit`: requests for commit block issue
- Counting rules:
  - On reclaim issue: `n = nvme_direct_write_async(...)`
    - `req_issued_reclaim += n`
    - `req_issued_total += n`
  - Final data/desc/commit issues follow the same pattern: add to each category and to `req_issued_total`
  - During partial drain, add poll return `done` to `req_completed_total`
  - Before final barrier, calculate outstanding:
    - `outstanding = req_issued_total - req_completed_total`
  - After final wait:
    - `req_completed_total += outstanding`
    - assert: `req_completed_total == req_issued_total`
- Debug validation items:
  - `req_issued_total == req_issued_reclaim + req_issued_tail_data + req_issued_desc + req_issued_commit`
  - outstanding must never be negative
  - right before `fsync` return: `busy_slots == 0`

### Expected Impact

- Fewer mid-path stalls in large `fsync` operations
- Better overlap of copy/submit/poll
- Strong p95/p99 improvements expected, especially when file size repeatedly exceeds the sequence window

### Risk

- Data corruption risk if slots are reused incorrectly
- Premature completion risk if request accounting misses updates
- Potential bug risk if completion path and `tls_tid`/qpair affinity diverge

### Step-by-step Implementation Checklist

- Add in-flight request and slot tracking fields to `stage_tx`
- Fix slot allocate/reuse to in-order policy (`head` allocate, `tail` reclaim)
- Convert reclaim path in `resv_blk()` to async issue
- Add slot reuse guard logic
- Apply `req_issued_total/req_completed_total` and reclaim request counters
- Validate full request consistency at final drain point
- Add debug asserts/counters (`issued == completed`)

---

## 2) P1 - Split write-dedicated submit/poll pipeline [DEFERRED for current architecture]

### Defer Decision (2026-03-23)

- The current architecture intentionally uses I/O worker threads as RPC handler threads for fsync, so the request-handling thread directly performs I/O to reduce scheduling overhead and latency.
- Splitting write submit/poll into another dedicated pipeline would introduce extra handoff/scheduling points and can conflict with this low-latency design assumption.
- Given the current target (minimal latency sacrifice with high multi-thread throughput), this item is deferred until there is time for a broader architecture-level revisit.

### Background

- The current structure is optimized mainly for reads, and writes still spend a high portion of time in per-call synchronous polling.
- Existing code comments also note that proper write pipelining requires separate read/write rings.

### Design Direction

- Introduce a write-dedicated ring buffer (`g_wr_bio_ring_buf`)
- Add a write-dedicated poll worker to strengthen submit/poll overlap
- In the `fsync` path, issue requests and let the write poller reap completions

### Expected Impact

- Better queue-depth utilization in `sync_file_data` and metadata flush paths
- Throughput improvements under write-heavy workloads

### Risk

- Increased ownership/lock-ordering complexity across threads
- Possible contention with the existing shared resources used by read paths

---

## 3) P1 - Batch metadata flush (`sync_dirty_buffer`) [SKIPPED for current scope]

### Skip Decision (2026-03-23)

- This optimization primarily affects the no-journal path (`sync_file()` / `sync_inode_metadata()` / `sync_all_fs_metadata()`), where metadata blocks are flushed via `sync_dirty_buffer()`.
- The current performance scope is journal-on `fsync` centered on `stage_file()`.
- In journal-on mode, `do_fsync()` routes to `stage_file()`, so metadata batch flush has little to no direct impact on the current target path.
- Therefore, this item is deferred and excluded from the current execution plan.

---

## 4) P2 - Adaptive `STG_WAIT_MODE` policy [SKIPPED for now]

### Background

- The current default is `ALWAYS_PERSIST`, where `fsync` may additionally write background journaling waiting-dirty data.
- Depending on workload, this can increase write amplification.

### Design Direction

- Use a hybrid policy of bounded wait + persist
- Decide dynamically based on waiting-dirty size, recent commit delay, and stage free-space ratio

### Expected Impact

- Fewer unnecessary extra writes
- Reduced interference between `fsync` and background journaling

---

## 5) P2 - fsync request coalescing (inode-level) [SKIPPED for now]

### Background

- When concurrent `fsync` requests target the same inode, redundant work can occur.

### Design Direction

- Coalesce in-flight `fsync` requests per inode into a single job with waiter fan-out
- Attach subsequent requests for the same inode to the existing in-flight completion

### Expected Impact

- Remove redundant stage work
- Reduce high-concurrency tail latency

---

## 6) P3 - memcpy path optimization [SKIPPED for now]

### Background

- `memcpy` cost before write submit can become a CPU bottleneck.

### Design Direction

- Improve copy batching and test prefetch/non-temporal copy
- Long-term: evaluate zero-copy feasibility based on SPDK SGL

### Expected Impact

- Reduced CPU usage and higher submit rate

---

## Validation Plan

## Common Metrics

- `fsync` latency: p50, p95, p99
- Number of NVMe write commands per `fsync`
- Written bytes / metadata bytes per `fsync`
- In-flight depth distribution
- Completion errors and retry count

## Experiment Scenarios

1. Small sync write (4KB~64KB)
2. Medium/large sync write (1MB~1GB)
3. Multi-thread same-inode `fsync` storm
4. Mixed read/write + periodic `fsync`

## Pass Criteria (draft)

- At least 20% improvement in large-`fsync` p95, or at least 20% throughput gain at equal latency
- No data integrity regression (including crash/recovery tests)
- Zero completion-accounting mismatch

---

## Recommended Execution Order

1. **Done**: implement/validate P0 (async reclaim + safe slot reuse guard)
2. **Deferred**: P1(1) split write-dedicated submit/poll pipeline
3. **Skipped for now**: P1(2) metadata batch flush (`sync_file` no-journal path only)
4. **Skipped for now**: P2 policy tuning (`STG_WAIT_MODE`, `fsync` coalescing)
5. **Skipped for now**: P3 memcpy/zero-copy experiments
6. **Close now**: end fsync optimization work here and revisit later when bandwidth is available

## Notes

- Keep `fsync` semantics.
In other words, by syscall return, all durability writes required by that `fsync` must be completed.
- The purpose of async conversion is not early response; it is to reduce internal pipeline stalls and shorten completion time.

