# 2026-03-23 P0 Detailed Implementation Plan

## Document Objective

- Break down the highest-priority `fsync` task (P0), **async conversion of stage reclaim writes**, into actionable implementation units.
- Requirements:
  - Slot reuse must strictly follow an **in-order policy**.
  - Preserve durability semantics at `fsync` return.
  - Guarantee request accounting consistency (`issued/completed`).

## Target Code and Change Boundary

- Core files
  - `oxbow/secure_daemon/src/fs/sync.c`
  - `oxbow/secure_daemon/src/io/nvme.c`
  - `oxbow/secure_daemon/include/fs/fs.h` (extend `stage_tx` structure if needed)
- Out of scope
  - user-facing API/ABI changes
  - journal format or recovery format changes
  - split write-dedicated ring/poller (P1 scope)

## Implementation Principles

- **Semantics first**: return `fsync` completion only after all required write completions
- **In-order reuse**: use only `head` for allocation and `tail` for reuse
- **Bounded complexity**: do not introduce free-list based out-of-order reuse in P0
- **Observability**: add counters/assertions/profile events first, then switch behavior

## Design Details

## 1) In-order Slot Management Model

- State variables (transaction-local)
  - `slot_head`: next slot index to use for issue
  - `slot_tail`: oldest reusable slot index
  - `slot_capacity`: total number of available slots
  - `slot_busy[]`: in-flight state per slot
  - `slot_done[]`: completion state per slot
  - `busy_slots`: number of current in-flight slots

- Behavioral rules
  - allocate: assign free slots in order from `slot_head`
  - completion: set `slot_done[idx] = true` for the completed slot
  - reclaim: release only contiguous `slot_done == true` region from `slot_tail`
  - no-reuse condition: if any slot before `slot_tail` is incomplete, do not reuse any slot after it

- Key invariants
  - `0 <= busy_slots <= slot_capacity`
  - reset `slot_done[released_idx]` after reclaim
  - when `busy_slots == 0`, `issued == completed` must hold

## 2) Request Accounting Model

- Transaction counters
  - `req_issued_total`
  - `req_completed_total`
  - `req_issued_reclaim`
  - `req_issued_tail_data`
  - `req_issued_desc`
  - `req_issued_commit`

- Update rules
  - reclaim issue: `n = nvme_direct_write_async(...)`
    - `req_issued_reclaim += n`
    - `req_issued_total += n`
  - tail data issue:
    - `req_issued_tail_data += n`
    - `req_issued_total += n`
  - desc/commit issue:
    - increment each category and `req_issued_total`
  - poll/drain:
    - `req_completed_total += done`
  - final barrier:
    - `outstanding = req_issued_total - req_completed_total`
    - `wait(outstanding)`
    - assert `req_completed_total == req_issued_total` after wait

## 3) Drain Policy

- `stage_io_drain_until_free(min_free_slots)`
  - do partial drain only when free slots are insufficient
  - process only a minimal amount to avoid losing async benefit due to over-drain
- `stage_io_drain_all()`
  - drain all outstanding requests right after commit issue or right before `fsync` return

## Function-level Change Plan

## A. `sync.c`

### A-1) Extend `stage_tx`

- Add fields
  - slot state fields (`slot_head`, `slot_tail`, `busy_slots`, bitmap/array)
  - request counters (`req_issued_total`, `req_completed_total`, and per-type issued counters)
  - error state (`io_error`)

### A-2) `new_stage_tx()`

- Initialization items
  - initialize slot ring
  - initialize request counters
  - initialize error state

### A-3) `resv_blk()`

- Convert existing synchronous reclaim write call into async issue
- Sequence when slots are insufficient:
  1. `stage_io_drain_until_free(...)`
  2. If still insufficient, assert + fail fast
- Accumulate counters on successful reclaim issue

### A-4) `stage_file()`

- Reflect counters for final pending data issue
- Reflect counters for desc/commit issue
- Execute final barrier based on `outstanding`
- Consistency assert right before return:
  - `req_completed_total == req_issued_total`
  - `busy_slots == 0`

## B. `nvme.c`

### B-1) Strengthen completion callback path

- Provide a hook to update slot done/busy state per completion
- If needed, extend callback context to `stage_tx + slot_idx`

### B-2) Add/organize helper APIs

- Example APIs
  - `nvme_stage_issue_async(...)`
  - `nvme_stage_poll_once(...)`
  - `nvme_stage_wait_n(...)`
- Goal: provide a thin API layer so that `sync.c` can handle slots/counters more easily

## C. Instrumentation

- Candidate `PF_TL_EVT` additions
  - reclaim async issue latency
  - drain wait latency
  - outstanding depth histogram
- Debug logs
  - immediate dump on issue/completion mismatch
  - print slot head/tail snapshots

## Recommended Step-by-step Order

1. **Step 0 - Add observability**
   - Add counters/assertions/logs first (no behavior change)
2. **Step 1 - Introduce slot state tracking**
   - Apply in-order head/tail + busy/done state updates
3. **Step 2 - Convert reclaim to async issue**
   - Switch `resv_blk()` path + introduce partial drain
4. **Step 3 - Switch to integrated barrier**
   - Reduce dependence on local `nr_total_reqs`; close transaction with `stage_tx` counters
5. **Step 4 - Stabilization**
   - strengthen asserts, propagate errors, run regression tests

## Test Plan

## Functional/Consistency Tests

- single-thread small/large `fsync`
- multi-thread `fsync` storm (same inode / different inode)
- crash injection (forced termination after issue) + recovery validation

## Performance Tests

- compare to baseline
  - `fsync` p50/p95/p99
  - throughput (MiB/s)
  - number of NVMe commands per `fsync`
  - drain count and average outstanding

## Regression Prevention Checks

- zero data mismatch
- zero completion-mismatch assertions
- zero deadlock/livelock

## Rollback Plan

- Keep immediate fallback to synchronous reclaim path with a compile-time flag
  - e.g., `OXBOW_FSYNC_RECLAIM_ASYNC`
- If issues occur, turn the flag off to restore the old path, then perform root-cause analysis

## Definition of Done

- Functionality:
  - confirm compliance with in-order slot policy
  - guarantee `issued == completed` and `busy_slots == 0`
- Performance:
  - meaningful improvement in large-`fsync` p95 or throughput (target >= 15~20%)
- Stability:
  - pass regression and recovery tests
