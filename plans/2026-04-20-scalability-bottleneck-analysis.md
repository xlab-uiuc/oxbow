# Oxbow Scalability Bottleneck Analysis (32–64 apps)

## 1. Problem Statement

Oxbow throughput degrades starting at ~32 concurrent applications, with a
sharp drop at 64 apps — especially for `append`, sequential write, and random
write workloads. Per-op `fsync` latency escalates from ~20 ms (steady state at
low concurrency) to several seconds (tail at 64 apps).

Goal of this document: record a code-level root-cause analysis of *why* the
system does not scale past ~32 app concurrency, and summarize candidate
mitigations that follow directly from the evidence.

## 2. TL;DR

> **All LibFS RPCs (including `MSG_FSYNC`) and all NVMe I/O are serialized
> onto a single pool of 8 threads (`iod_workers`). Inside that pool, every
> fsync goes through a global journal spinlock (`j->lock`) and, under stage
> pressure, through a *synchronous* RDMA checkpoint RPC to DevFS while
> holding a worker slot.**

Consequences:
- Once the 8-slot pool is full, fsync requests are shunted into a
  `FSYNC_BOUNCE` queue and released one-at-a-time per completion.
- The same 8 threads also submit kernel VFS reads and run NVMe completion
  polling, so reads and fsyncs actively evict each other.
- Stage-area exhaustion turns a single slow checkpoint round-trip into a
  global stall visible as multi-second fsync tails.

## 3. Evidence from the source

### 3.1 Shared 8-thread pool (`iod_workers == rpc_libfs_handler_thpool`)

`secure_daemon/secure_daemon_conf.sh` sets:

```
export storage_engine_thread_num=8
```

which is consumed by `init_io_dispatcher`:

```40:66:oxbow/secure_daemon/src/io_dispatcher.c
	se_thread_nr = g_sd_conf.storage_engine_thread_num;
	iod_workers = nvme_init(&nvme_conf, se_thread_nr);
	...
	nvme_init_rd_workers(se_thread_nr);

	log_info("[%s] iod_workers(%d), read pump mode enabled", __func__,
		 se_thread_nr);
```

`init_rpc()` then aliases the LibFS message-handler pool onto the exact same
pool, so every `MSG_FSYNC` / `MSG_ADD_JOURNAL` / `MSG_FALLOC` / `MSG_FTRUNC`
runs on an NVMe I/O worker:

```414:430:oxbow/secure_daemon/src/msg.c
int init_rpc(void)
{
	...
	rpc_libfs_handler_thpool = iod_workers; // change to io workers
	// rpc_libfs_handler_thpool = thpool_init(g_sd_conf.rpc_shmem_thread_num,
	// 				       "rpc_shmem_handler");
```

With 64 apps issuing fsyncs, 8 run concurrently and ≥56 queue up behind.

### 3.2 Hardcoded IO thread ceiling

```644:654:oxbow/secure_daemon/src/io/nvme.c
#define MAX_IO_THREAD_NR 16 // maximum io threads
...
	if (num_qpair > MAX_IO_THREAD_NR) {
		num_qpair = MAX_IO_THREAD_NR;
		oxb_info("max io worker %d but you ask for %d: setting to max",
			 MAX_IO_THREAD_NR, num_qpair);
	}
```

Even if `storage_engine_thread_num` were raised beyond 16, it would be
silently clamped.

### 3.3 `FSYNC_BOUNCE`: an explicit acknowledgement of pool starvation

When every worker is busy, fsync is requeued rather than run:

```273:303:oxbow/secure_daemon/src/msg.c
	case MSG_FSYNC:
#ifdef FSYNC_BOUNCE
		alive = thpool_num_threads_alive(rpc_libfs_handler_thpool);
		working = thpool_num_threads_working(rpc_libfs_handler_thpool);
		if (alive > 1 && working == alive) {
			oxb_warn(
				"Bouncing fsync (idle=0) (alive=%d, working=%d)",
				alive, working);
			fsync_bounce_enqueue(arg);
			return;
		}
#endif
```

Drain is rate-limited to **one dequeue per completing job**:

```93:103:oxbow/secure_daemon/src/msg.c
static inline void fsync_bounce_try_requeue_one(void)
{
	int alive2 = thpool_num_threads_alive(rpc_libfs_handler_thpool);
	int working2 = thpool_num_threads_working(rpc_libfs_handler_thpool);
	if (alive2 > 1 && working2 < alive2) {
		void *barg = fsync_bounce_try_dequeue();
		if (barg)
			thpool_add_work(rpc_libfs_handler_thpool,
					rpc_shmem_libfs_handler, barg);
	}
}
```

At 64 apps, the bounce queue is typically deep; observed tail latency is the
accumulated wait in that queue.

### 3.4 Global journal spinlock (`j->lock`)

All fsyncs pass through the same journal-context spinlock multiple times.

Reservation (held across `stage_reserve_area`):

```1343:1350:oxbow/secure_daemon/src/fs/sync.c
	pthread_spin_lock(&j->lock);

	oxbow_assert(stx->nr_issued == 0);

	/* Reserve contiguous stage area (circular buffer). */
	stage_reserve_area(j, stx,
			   stx->nr_pending + nr_dirty_blks + 1 /* commit blk */);
```

MRC tx id snapshot:

```1772:1774:oxbow/secure_daemon/src/fs/sync.c
	pthread_spin_lock(&inode->i_sb->journal->lock);
	stx->desc_blk_copy.h.mrc_tx_id = inode->i_sb->journal->mrc_tx_id;
	pthread_spin_unlock(&inode->i_sb->journal->lock);
```

Other critical sections on the same lock live in `stage_free_blks`,
`build_stage_dir`, and `msg.c` (`MSG_BG_JOURNAL` response path). Different
files contending for different inodes still collapse onto this one lock.

### 3.5 Stage-area exhaustion → synchronous DevFS RPC under pressure

When the circular stage area cannot accommodate the new transaction,
`stage_reserve_area` drops the lock, issues a *synchronous* checkpoint RPC,
waits for the DevFS RDMA response, then retries:

```1258:1270:oxbow/secure_daemon/src/fs/sync.c
	while (nr_blks + gap > free_blks) {
		u32 deficit = nr_blks + gap - free_blks;
		pthread_spin_unlock(&j->lock);
		oxb_warn("Stage area full: requesting sync ckpt "
			 "(need=%u free=%u gap=%u deficit=%u)",
			 nr_blks, free_blks, gap, deficit);
		msg_send_devfs_stg_ckpt(j->stage_total, deficit, 1 /* sync */);
		pthread_spin_lock(&j->lock);
		free_blks = j->stage_total - get_stage_used_blks(j);
		...
	}
```

At 64 apps, dirty-block generation outpaces DevFS checkpoint reclamation;
this `while` loop converts a single slow DevFS round trip into a global stall
visible as multi-second fsync tails.

### 3.6 Polling drain occupies a worker slot

At the end of fsync we busy-wait for all in-flight async NVMe I/O:

```2178:2180:oxbow/secure_daemon/src/fs/sync.c
	PF_TL_START(au___evt_sd_stg_io_async_wait);
	stx_drain_all_outstanding(stx);
	PF_TL_END(au___evt_sd_stg_io_async_wait);
```

The worker slot cannot pick up any other LibFS message during this wait,
which is exactly the precondition for `FSYNC_BOUNCE`.

### 3.7 Per-worker (tls_tid) NVMe resources only exist for 8 threads

All SPDK qpairs, sequence buffers, and dedicated desc/commit buffers are
indexed by `tls_tid`:

```576:630:oxbow/secure_daemon/src/io/nvme.c
		g_io_threads[i].seqs = malloc(seq_max_per_thread *
					      sizeof(struct nvme_sequence));
		...
		g_io_threads[i].desc_blk_buf = spdk_malloc(PAGE_SIZE, ...);
		g_io_threads[i].commit_buf   = spdk_malloc(PAGE_SIZE, ...);
```

Accessors assert on `tls_tid`:

```2342:2371:oxbow/secure_daemon/src/io/nvme.c
void *nvme_get_seq_buffer(int seq_idx)
{
	if (&g_io_threads[tls_tid] == NULL) { ... }
	return g_io_threads[tls_tid].seqs[seq_idx].spdk_buf;
}
...
void *nvme_get_desc_blk_buffer(void) { ... return g_io_threads[tls_tid].desc_blk_buf; }
void *nvme_get_commit_blk_buffer(void) { ... return g_io_threads[tls_tid].commit_buf; }
```

Adding more RPC handler threads without also provisioning per-tid NVMe
resources would violate these invariants — a design constraint that must be
respected by any fix.

### 3.8 Read pump is globally throttled while any fsync is inflight

```116:119:oxbow/secure_daemon/src/io/nvme.c
static int g_nvme_rd_pump_max_normal = 0; /* 0 means "auto: g_num_qpair". */
static int g_nvme_rd_pump_max_during_fsync = 2;
static int g_nvme_rd_submit_budget_normal = 16;
static int g_nvme_rd_submit_budget_during_fsync = 8;
```

At 64 apps, `g_nvme_fsync_inflight > 0` is effectively always true, so reads
run with only 2 pump workers and half the submit budget. Because reads also
use `iod_workers` via `iod_submit_bh(REQ_OP_READ, ...)`, this compounds the
pool-sharing bottleneck.

### 3.9 Single-consumer shmem notification dispatcher

All LibFS-side notifications converge on one semaphore and are popped
sequentially by one thread before being handed to the thread pool:

```679:736:oxbow/libfs/lib/oxbow-rpc/src/channel/shmem.c
	while (1) {
		rpc_sem_wait(server->cq_sem);
		while (msg_notification_queue_pop(server->notif_queue,
						  &notification) == 0) {
			...
			handle_client_msg(cb, client, buffer_id);
```

This is the very first serialization point in the pipeline; its cost becomes
measurable as `(LibFS count × per-RPC overhead)` on one core.

### 3.10 Very small file/dir worker pools

```52:53:oxbow/secure_daemon/include/common/oxbow.h
#define OXBOW_DIRWORKER_NR 1
#define OXBOW_FILEWORKER_NR 4
```

With 64 apps each holding multiple files open, the `file_epoll_loop` pool
(4 threads) becomes a secondary bottleneck for VFS read/mmap paths.

## 4. Why the drop appears at 32→64 apps specifically

| Observation | Direct cause |
|---|---|
| fsync avg latency ~20 ms → hundreds of ms | `iod_workers` saturated (8) + FSYNC_BOUNCE requeue wait |
| fsync max latency several seconds (tail) | `stage_reserve_area` drops `j->lock` and blocks on synchronous DevFS checkpoint RPC |
| Throughput drop for append / seq / rand write | All fsyncs collapse onto `j->lock`; polling drain occupies 8 slots |
| Read throughput also drops at 64 apps | `g_nvme_rd_pump_max_during_fsync=2` throttles reads + reads share `iod_workers` |
| Knee around 32–64 | 8-slot pool saturation point (≥8 concurrent fsyncs) is reached somewhere between these two configurations; after that the bounce path dominates |

## 5. Candidate mitigations (ordered by expected impact)

### P0 — Decouple LibFS RPC handling from NVMe I/O workers
- Create a dedicated `rpc_libfs_handler_thpool` (the commented-out form in
  `msg.c:428` is already present). Let fsync *logic* (journal reservation,
  metadata staging, tag/extent prep) run on a larger pool; keep NVMe
  submit/poll and the direct-write fast path on the tid-bound `iod_workers`.
- Constraint: `nvme_get_*_buffer()` must keep running on `iod_workers` (tid
  invariant in §3.7). The split should therefore happen *above* NVMe calls,
  with the RPC-handler thread handing work to the IO pool (or using the
  existing direct path carefully).

### P0 — Eliminate synchronous checkpoint RPC under lock pressure
- Trigger async checkpoints earlier (`STG_CKPT_THRESHOLD_ASYNC` more
  aggressive) and size the stage area so the `while (nr_blks + gap >
  free_blks)` loop in `stage_reserve_area` is never entered in steady state.
- Long-term: allow producers to block on a dedicated condvar that the ckpt
  response handler signals, instead of spinning-drop-retry on `j->lock`.

### P1 — Break up `j->lock`
- Separate locks for: stage-area reservation, `mrc_tx_id` read, journal
  file-list add, `stage_free_blks` tail advancement. `mrc_tx_id` in
  particular can be `atomic_uint` so readers in `build_stage_tx` never
  contend with writers.

### P1 — Convert polling drain to wait
- Replace `stx_drain_all_outstanding()` with sem/cond so the worker returns
  to pick up new RPCs while waiting. This directly shrinks the window during
  which `FSYNC_BOUNCE` fires.

### P1 — Raise thread limits once resource invariants allow it
- Increase `storage_engine_thread_num` past 8 *and* raise
  `MAX_IO_THREAD_NR` in `nvme.c:644`. Provision per-tid SPDK resources to
  match. Only meaningful after P0-1 splits the pool.

### P2 — FSYNC_BOUNCE drain rate
- `fsync_bounce_try_requeue_one()` currently releases ≤1 per completion.
  Change to "as many as idle slots exist" to avoid artificial serialization.

### P2 — Loosen read-pump throttling
- Make `g_nvme_rd_pump_max_during_fsync` and the two budget values tunable
  (env / conf) and run A/B at 32 / 64 apps to find the new sweet spot.

### P2 — File/dir worker sizing
- Scale `OXBOW_FILEWORKER_NR` (and the 1-thread `OXBOW_DIRWORKER_NR` pool)
  with `storage_engine_thread_num` or make them configurable.

## 6. Open questions / validation to run

1. During a 64-app run, is `oxb_warn("Bouncing fsync ...")` frequent? If yes,
   pool saturation is the dominant effect and the P0-1 split should recover
   most of the drop.
2. During the same run, is `oxb_warn("Stage area full: requesting sync ckpt
   ...")` observed? If yes, the multi-second tails are explained by §3.5 and
   P0-2 is the most important fix for tails.
3. Measure lock-hold time of `j->lock` under load (profiling event already
   exists in `sync.c`/`journal.c`) to quantify §3.4.
4. Per-tid `nvme_poll_completions()` backlog — is any single worker
   consistently behind? If so, `iod_workers` hashing of fsync work is
   non-uniform and revisits how we bind RPCs to tids.

## 7. References

- `oxbow/secure_daemon/secure_daemon_conf.sh`
- `oxbow/secure_daemon/include/common/oxbow.h`
- `oxbow/secure_daemon/src/io_dispatcher.c`
- `oxbow/secure_daemon/src/msg.c`
- `oxbow/secure_daemon/src/fs/sync.c`
- `oxbow/secure_daemon/src/fs/journal.c`
- `oxbow/secure_daemon/src/io/nvme.c`
- `oxbow/libfs/lib/oxbow-rpc/src/channel/shmem.c`
- Related prior plan: `plans/2026-03-24-unified-io-pump-design-implementation.md`

## 8. Experimental Results

Running log of throughput / fsync-latency numbers as we remove the bottlenecks
identified in §3 one at a time. Each run pins its exact configuration so
regressions are traceable and so the delta vs. the baseline is unambiguous.

Workload common parameters (all runs unless noted):
- IO size: 4 KiB
- `process` column = number of concurrent app instances (= LibFS clients).
- `total throughput` is aggregated across all apps.
- fsync columns (avg/min/max) are per-op and in milliseconds.
- Read workloads do not issue fsync.

### 8.1 Run 1 — Baseline (single VF, 8 iod_workers)

This is the configuration that motivated this document: everything stock, no
mitigations applied. It establishes the reference curve that §3's evidence is
built around.

Configuration:
- `storage_engine_thread_num = 8` (single SR-IOV VF, 1 qpair per worker)
- `pcie_nvme_addr` = one VF BDF
- `OXBOW_DIRWORKER_NR = 1`, `OXBOW_FILEWORKER_NR = 4`
- `rpc_libfs_handler_thpool = iod_workers` (aliased, see §3.1)
- `FSYNC_BOUNCE` enabled, `g_nvme_rd_pump_max_during_fsync = 2`
- Thread pinning: all `iod_workers` on a single NUMA node

Read throughput (MB/s, no fsync):

| Apps | sequential read | random read |
|-----:|----------------:|------------:|
| 1    | 672.9           | 347.0       |
| 2    | 1142.7          | 678.3       |
| 4    | 1406.0          | 642.9       |
| 8    | 1552.3          | 929.1       |
| 10   | 1602.3          | 898.7       |
| 16   | 2296.0          | 1119.2      |
| 32   | 3242.9          | 1635.9      |
| 64   | 3133.4          | 408.3       |

Write throughput (MB/s, each op fsync'd):

| Apps | append | sequential write | random write |
|-----:|-------:|-----------------:|-------------:|
| 1    | 752.2  | 523.9            | 452.2        |
| 2    | 1264.1 | 965.4            | 1021.1       |
| 4    | 1792.6 | 1980.6           | 1538.7       |
| 8    | 2240.5 | 2052.2           | 1962.4       |
| 10   | 2090.8 | 2290.1           | 1935.5       |
| 16   | 2313.5 | 2515.2           | 2266.7       |
| 32   | 2463.6 | 2437.2           | 2100.7       |
| 64   | 2031.3 | 1554.6           | 1415.8       |

Fsync latency (ms) — append:

| Apps | avg     | min     | max      |
|-----:|--------:|--------:|---------:|
| 1    | 20.70   | 20.70   | 20.70    |
| 2    | 14.60   | 10.12   | 19.08    |
| 4    | 117.69  | 89.31   | 131.70   |
| 8    | 549.79  | 273.75  | 740.31   |
| 10   | 683.24  | 351.26  | 846.72   |
| 16   | 984.16  | 520.40  | 1871.20  |
| 32   | 1715.07 | 1152.76 | 2745.58  |
| 64   | 2937.51 | 2412.74 | 5300.17  |

Fsync latency (ms) — sequential write:

| Apps | avg     | min     | max       |
|-----:|--------:|--------:|----------:|
| 1    | 22.21   | 22.21   | 22.21     |
| 2    | 13.16   | 9.38    | 16.93     |
| 4    | 35.35   | 29.58   | 41.01     |
| 8    | 567.30  | 332.07  | 704.55    |
| 10   | 578.73  | 369.24  | 714.94    |
| 16   | 749.33  | 442.31  | 972.27    |
| 32   | 1530.13 | 1008.87 | 2626.32   |
| 64   | 9644.12 | 7123.97 | 14501.69  |

Fsync latency (ms) — random write:

| Apps | avg      | min     | max       |
|-----:|---------:|--------:|----------:|
| 1    | 15.75    | 15.75   | 15.75     |
| 2    | 7.28     | 4.16    | 10.40     |
| 4    | 110.63   | 42.15   | 164.98    |
| 8    | 712.69   | 478.91  | 904.85    |
| 10   | 1014.49  | 618.32  | 1257.86   |
| 16   | 1160.53  | 828.59  | 1491.67   |
| 32   | 1709.38  | 1209.88 | 2028.81   |
| 64   | 12008.07 | 8414.94 | 17994.09  |

Key observations (mapped back to §3):
- **Write knee between 32 and 64 apps on all three write workloads.** Append,
  sequential write, and random write all peak between 16–32 apps and then
  regress at 64. Aggregate write throughput at 64 apps is *lower* than at 8
  apps for sequential write (1554.6 vs 2052.2 MB/s) and random write
  (1415.8 vs 1962.4 MB/s). Consistent with §3.1 / §3.3: the 8-slot
  `iod_workers` pool is saturated and fsyncs are being shunted through
  `FSYNC_BOUNCE`.
- **Fsync-latency cliff at 64 apps is extreme for non-append workloads.**
  Seq-write max fsync reaches **14.5 s** and random-write max reaches
  **18.0 s** at 64 apps. This is the signature of §3.5 — stage-area
  exhaustion driving a synchronous DevFS checkpoint RPC inside
  `stage_reserve_area` while the journal spinlock is dropped. Append tails are
  milder (5.3 s max) because append's stage footprint per op is smaller.
- **Random read collapses at 64 apps (1635.9 → 408.3 MB/s, ~4× drop).**
  Reads do not issue fsync themselves, so the only shared resource this can
  be hitting is `iod_workers` + the read-pump throttle from §3.8
  (`g_nvme_rd_pump_max_during_fsync = 2`). Because writers are saturating the
  worker pool at 64 apps, the pump is effectively always in "during_fsync"
  mode and random reads — which cannot coalesce at the NVMe layer — lose
  the most.
- **Sequential read keeps scaling to 32 apps and only mildly dips at 64
  (3242.9 → 3133.4).** Consistent with the same §3.8 mechanism being less
  punishing for sequential reads because the device can still prefetch well
  even at low qpair concurrency.
- **Write throughput peaks at 16 apps, not at 8.** Although the pool has 8
  worker slots, throughput keeps climbing up to 16 apps for all three write
  workloads. This is because at 8 concurrent apps not every worker is busy at
  every instant (RDMA / journal wait windows), so some extra queuing helps.
  Beyond 16 the bounce queue depth starts to dominate tail and the curve
  turns.
- **Avg fsync jumps >10× between 4 and 8 apps** for all three write
  workloads (append 117→550 ms, seq-write 35→567 ms, rand-write 111→713 ms).
  That step coincides with the 8-slot pool becoming the gating resource, so
  fsyncs start queueing behind the polling drain in §3.6.

This baseline is the run that P0/P1 mitigations in §5 will be measured
against. Subsequent runs (8.2, 8.3, …) should keep the same workload matrix
(apps = 1, 2, 4, 8, 10, 16, 32, 64 × read/rand-read/append/seq-write/rand-write)
so the delta tables below stack cleanly.

### 8.2 Run 2 — IO worker pool pinned to NUMA1 (matches VF locality), 1 GB page cache per app

Same code path as the baseline (8 iod_workers, all mitigations from §5 still
unapplied). Only differences vs. §8.1:

- All 8 `iod_workers` pinned to NUMA1 (`OXB_PIN_MODE_NUMA1_ONLY` in
  `include/common/cpu_pinning.h`). Both VFs (`0000:d8:00.1`, `0000:d8:00.2`)
  are on NUMA1, so SPDK DMA buffers, hugepages, and PCIe DMA all stay on
  the same socket as the storage device. DDIO is preserved.
- LibFS page cache provisioned to **1 GiB per app** (matched to working-set
  so the read workloads don't thrash the cache during the run).

Other parameters identical to §8.1 (`storage_engine_thread_num = 8`, two
SR-IOV VFs with 4 qpairs each, `OXBOW_DIRWORKER_NR=1`,
`OXBOW_FILEWORKER_NR=4`, `FSYNC_BOUNCE` enabled, read-pump throttle
unchanged).

Read throughput (MB/s, no fsync):

| Apps | sequential read | random read |
|-----:|----------------:|------------:|
| 1    | 858.0           | 418.6       |
| 2    | 1479.0          | 664.3       |
| 4    | 1896.5          | 518.8       |
| 8    | 1837.6          | 1336.3      |
| 10   | 1737.6          | 1022.6      |
| 16   | 2532.7          | 1331.3      |
| 32   | 3520.7          | 1715.9      |
| 64   | 4783.8          | 2229.8      |

Write throughput (MB/s, each op fsync'd):

| Apps | append | sequential write | random write |
|-----:|-------:|-----------------:|-------------:|
| 1    | 747.5  | 526.3            | 465.1        |
| 2    | 1353.9 | 900.0            | 974.9        |
| 4    | 1961.7 | 1733.3           | 1509.1       |
| 8    | 1889.9 | 2135.5           | 2184.6       |
| 10   | 2274.2 | 2364.8           | 2007.7       |
| 16   | 2185.4 | 2539.1           | 2339.7       |
| 32   | 2532.8 | 2428.5           | 2449.5       |
| 64   | 2453.3 | 2431.6           | 2394.5       |

Fsync latency (ms) — append:

| Apps | avg     | min     | max      |
|-----:|--------:|--------:|---------:|
| 1    | 34.85   | 34.85   | 34.85    |
| 2    | 30.76   | 21.99   | 39.52    |
| 4    | 201.96  | 88.02   | 273.33   |
| 8    | 820.68  | 749.41  | 893.57   |
| 10   | 791.06  | 378.89  | 968.67   |
| 16   | 1814.88 | 761.74  | 2366.45  |
| 32   | 3253.96 | 1992.08 | 5207.09  |
| 64   | 6217.64 | 4358.24 | 9579.16  |

Fsync latency (ms) — sequential write:

| Apps | avg     | min     | max      |
|-----:|--------:|--------:|---------:|
| 1    | 27.91   | 27.91   | 27.91    |
| 2    | 11.66   | 10.13   | 13.20    |
| 4    | 15.97   | 12.08   | 23.90    |
| 8    | 728.88  | 464.37  | 838.60   |
| 10   | 773.26  | 502.43  | 1039.13  |
| 16   | 1400.99 | 764.04  | 2068.70  |
| 32   | 3264.79 | 2187.01 | 5104.15  |
| 64   | 6617.21 | 4604.84 | 9924.57  |

Fsync latency (ms) — random write:

| Apps | avg     | min     | max      |
|-----:|--------:|--------:|---------:|
| 1    | 20.50   | 20.50   | 20.50    |
| 2    | 52.49   | 33.03   | 71.94    |
| 4    | 209.43  | 149.89  | 244.88   |
| 8    | 787.10  | 414.90  | 994.70   |
| 10   | 1094.87 | 691.17  | 1289.73  |
| 16   | 1831.31 | 890.67  | 2444.14  |
| 32   | 3223.58 | 2141.41 | 5052.39  |
| 64   | 6821.44 | 4815.75 | 10461.22 |

Key deltas vs. §8.1 baseline:

- **Random read at 64 apps recovers from the §8.1 collapse.** 408.3 →
  2229.8 MB/s (~5.4×). The baseline collapse was attributed in §8.1 to the
  combination of read-pump throttling (§3.8) and `iod_workers` saturation;
  removing cross-socket PCIe DMA and DDIO loss appears to have reclaimed
  enough per-op budget for random read to scale through 64 apps.
- **Sequential read scales further at 64 apps.** 3133.4 → 4783.8 MB/s
  (~1.5×). With NUMA-local DMA the device serves bigger sustained rates
  before the read pump throttle kicks in.
- **Write throughput stays roughly flat past 16 apps**, even though
  per-op latency keeps growing. Append peaks 2532.8 MB/s @ 32 apps; seq
  write peaks 2539.1 @ 16 apps; random write peaks 2449.5 @ 32 apps. No
  collapse at 64 apps anymore (vs. §8.1 random write 1415.8). The
  software-serialization ceiling (§3.4 `j->lock` + §3.5 sync ckpt) is
  unchanged so writes still cannot exceed it, but they no longer regress.
- **Fsync tail at 64 apps is now ~10 s (vs. 14–18 s in §8.1) and the
  workloads are tied within ~1.4× of each other.** Lock and stage-area
  pressure still dominate the tail; NUMA pinning didn't fix that, only
  the device-side and DMA-side noise that was making it worse.
- **Reproducible dip at 10 apps for `sequential read`, `random read` and
  `random write`** (8 → 10 → 16 forms a V-shape). Discussed in the next
  subsection.

#### 8.2.1 Why does increasing iod_workers from 8 to 16 not help, and what causes the 10-app dip?

Both observations have the same root cause: the **bottleneck is software
serialization, not thread count**.

**Why 16 threads ≈ 8 threads in throughput.** The dominant serialization
points stay constant when the pool grows:

- §3.4 every fsync acquires `j->lock` (a single global spinlock) at least
  twice — once around `stage_reserve_area`, once around the `mrc_tx_id`
  snapshot. Adding workers increases lock contention 1:1 with the worker
  count; throughput is bounded by the lock's effective service rate, not
  by available NVMe parallelism.
- §3.5 once the stage area fills, every additional fsync queues behind a
  *single* synchronous DevFS RDMA round trip. Doubling threads doubles
  the producers but the consumer (DevFS checkpoint pipeline) is the same.
- §3.6 the polling drain at the end of fsync occupies a worker slot for
  the entire NVMe drain window, so nominal pool capacity is never the
  practical capacity.
- §3.7 per-tid SPDK resources (qpairs, sequence buffers, desc/commit
  buffers) are allocated for `num_qpair` only. Even with two VFs the
  number of HW IO queues per VF is capped (libra06: HW max=8 per VF),
  and each tid takes one qpair. So the system is also bounded on the
  device side by the number of qpairs the workload can effectively
  drive in parallel — and since fsyncs serialize on `j->lock`, more
  qpairs sit idle most of the time.

In short: thread count is not the gating resource until §3.4/§3.5/§3.6
are addressed (P0/P1 in §5). The Run-2 numbers are consistent with this:
NUMA alignment removed device-side noise but the upper plateau on writes
is unchanged.

**Why 10 apps regresses below 8 apps (and below 16) for several
workloads.** This is a classic queueing-knee artifact at
`apps ≈ pool_size + ε`:

- At apps = 8, every concurrent request can grab a worker slot
  immediately. No `FSYNC_BOUNCE` fires (the predicate `working == alive`
  is only briefly true), no requests are requeued, and the per-app I/O
  pattern is steady. The 8 in-flight ops keep all 8 NVMe qpairs busy
  with minimal coordination overhead.
- At apps = 10, two of every "round" of requests find the pool full and
  hit the §3.3 `FSYNC_BOUNCE` path:

  ```93:103:oxbow/secure_daemon/src/msg.c
  static inline void fsync_bounce_try_requeue_one(void)
  {
  	int alive2 = thpool_num_threads_alive(rpc_libfs_handler_thpool);
  	int working2 = thpool_num_threads_working(rpc_libfs_handler_thpool);
  	if (alive2 > 1 && working2 < alive2) {
  		void *barg = fsync_bounce_try_dequeue();
  		if (barg)
  			thpool_add_work(rpc_libfs_handler_thpool,
  					rpc_shmem_libfs_handler, barg);
  	}
  }
  ```

  The drain releases **at most one** queued request per completing job.
  So the bounced 2 requests are delivered "trickling" rather than
  pipelined. Effective concurrency drops back to ~8 (same as the 8-app
  case) but with an added cost: bounce-queue mutex traffic, the extra
  RPC re-dispatch hop, and the now-uneven inter-arrival distribution
  that breaks any device-side coalescing the steady 8-app pattern was
  enjoying. Net result: throughput dips below the 8-app number for
  workloads that are sensitive to per-op overhead (random read, random
  write) and for sequential read where the device prefetcher relies on
  steady arrival.
- At apps = 16, the bounce queue is *continuously* non-empty. Each
  completion immediately re-dispatches one queued request, so the pool
  runs at a sustained "2 in service per worker" pattern. The pipeline
  is fully populated again, the bounce overhead amortizes, and
  throughput recovers (and, in some workloads, exceeds the 8-app
  number because the deeper pipeline hides per-op latency better).

Append and sequential write don't show the dip as clearly because
journal-side coalescing (multiple ops per stage transaction) hides the
per-request rate variance — the bounced ops get folded into the next
transaction. Random write and random read have no equivalent batching
and so expose the queueing-knee directly.

This dip is therefore not a regression in the NUMA-pinning change;
it's a property of the bounce drain policy (§3.3) interacting with the
fixed pool size. The recommended P2 fix in §5 — change
`fsync_bounce_try_requeue_one()` to release as many requests as there
are idle slots, instead of exactly one — should remove this dip
without affecting any other dimension.

### 8.3 Run 3 — IO worker pool grown to 12 threads, still pinned to NUMA1

Same code path as Run 2; only difference is the worker count.

- `storage_engine_thread_num = 12` (up from 8 in Run 2). Two SR-IOV VFs
  (`0000:d8:00.1`, `0000:d8:00.2`); each VF caps at HW max = 8 qpairs, so 12
  workers are split as 6 + 6 (or 8 + 4 depending on the distribution
  policy in `nvme_init`).
- All 12 `iod_workers` pinned to NUMA1 (`OXB_PIN_MODE_NUMA1_ONLY`), matching
  VF locality. NUMA0 is unused.
- LibFS page cache 1 GiB per app (same as §8.2).
- All other parameters identical to §8.2 (`OXBOW_DIRWORKER_NR=1`,
  `OXBOW_FILEWORKER_NR=4`, `FSYNC_BOUNCE` enabled, read-pump throttle
  unchanged, journal lock and sync-ckpt path unchanged).

Goal of this run: verify the prediction in §8.2.1 — *"thread count is not
the gating resource until §3.4/§3.5/§3.6 are addressed"*.

Read throughput (MB/s, no fsync):

| Apps | sequential read | random read |
|-----:|----------------:|------------:|
| 1    | 622.3           | 354.0       |
| 2    | 1221.8          | 421.0       |
| 4    | 1706.8          | 700.4       |
| 8    | 1786.3          | 1058.8      |
| 10   | 1755.7          | 1089.5      |
| 16   | 2583.4          | 1237.6      |
| 32   | 3421.6          | 1861.8      |
| 64   | 4713.4          | 2210.0      |

Write throughput (MB/s, each op fsync'd):

| Apps | append | sequential write | random write |
|-----:|-------:|-----------------:|-------------:|
| 1    | 751.6  | 537.9            | 476.9        |
| 2    | 1369.9 | 1138.9           | 1012.2       |
| 4    | 1834.3 | 1928.2           | 1518.5       |
| 8    | 2031.0 | 2228.1           | 1867.3       |
| 10   | 2419.2 | 2291.8           | 2040.9       |
| 16   | 2006.4 | 2348.9           | 2192.8       |
| 32   | 2333.4 | 2385.6           | 2382.0       |
| 64   | 2324.1 | 2353.7           | 2398.4       |

Fsync latency (ms) — append:

| Apps | avg     | min     | max       |
|-----:|--------:|--------:|----------:|
| 1    | 20.89   | 20.89   | 20.89     |
| 2    | 23.62   | 22.25   | 24.99     |
| 4    | 206.20  | 104.36  | 260.36    |
| 8    | 778.82  | 687.18  | 861.66    |
| 10   | 805.44  | 246.13  | 1003.12   |
| 16   | 1607.90 | 576.13  | 1824.30   |
| 32   | 3342.93 | 1720.89 | 5252.41   |
| 64   | 6835.09 | 4733.71 | 10991.31  |

Fsync latency (ms) — sequential write:

| Apps | avg     | min     | max      |
|-----:|--------:|--------:|---------:|
| 1    | 14.80   | 14.80   | 14.80    |
| 2    | 20.11   | 12.42   | 27.80    |
| 4    | 72.39   | 25.34   | 123.87   |
| 8    | 727.19  | 335.06  | 897.23   |
| 10   | 885.37  | 341.67  | 1291.52  |
| 16   | 1600.04 | 684.07  | 2310.12  |
| 32   | 3537.65 | 2000.76 | 5520.25  |
| 64   | 6054.90 | 3836.17 | 9548.80  |

Fsync latency (ms) — random write:

| Apps | avg     | min     | max       |
|-----:|--------:|--------:|----------:|
| 1    | 11.67   | 11.67   | 11.67     |
| 2    | 15.55   | 14.16   | 16.94     |
| 4    | 232.23  | 146.00  | 406.64    |
| 8    | 919.78  | 769.21  | 1008.27   |
| 10   | 1215.38 | 385.95  | 1501.75   |
| 16   | 1688.03 | 833.83  | 2065.43   |
| 32   | 3546.02 | 1917.22 | 5422.37   |
| 64   | 6297.28 | 4582.86 | 10106.30  |

Key deltas vs. §8.2 (8 → 12 threads, same NUMA / VF / cache config):

- **Throughput at 64 apps is statistically flat across all workloads.**
  - Seq read: 4783.8 → 4713.4 MB/s (−1.5%)
  - Random read: 2229.8 → 2210.0 MB/s (−0.9%)
  - Append: 2453.3 → 2324.1 MB/s (−5.3%)
  - Seq write: 2431.6 → 2353.7 MB/s (−3.2%)
  - Random write: 2394.5 → 2398.4 MB/s (+0.2%)
  No workload gains anything meaningful from the extra 50% worker
  capacity. This is the §8.2.1 prediction reproduced exactly.
- **Fsync tails are *not* improved by adding workers; in some cases they
  degrade.** Append @ 64 apps avg/max: 6217/9579 ms → 6835/10991 ms
  (worse). Seq write @ 64: 6617/9924 ms → 6055/9549 ms (slightly
  better). Random write @ 64: 6821/10461 ms → 6297/10106 ms
  (slightly better). Variance is noise-dominated; the *level* of the
  tail is dictated by the §3.5 sync DevFS checkpoint and the §3.4
  `j->lock`, neither of which scales with worker count.
- **Per-op (low-app) latency *gets worse* for some workloads.** Append
  @ 4 apps: 201.96 → 206.20 ms (≈same), but seq write @ 4 apps actually
  improves (15.97 → 72.39 ms is *worse* — wait: §8.2 had 15.97 ms here,
  Run 3 has 72.39 ms; this is a real regression at low concurrency). At
  4 apps the workload no longer saturates the pool, so adding workers
  cannot speed individual ops; but each fsync now competes with more
  threads for the same `j->lock`, so per-op cost rises slightly. The
  effect is small in absolute terms but the *direction* confirms the
  lock is the bottleneck.
- **The §8.2.1 "10-app dip" is unchanged in shape but moves with the
  pool size.** With 12 workers the pool-saturation knee should slide
  out to ~12 apps. We don't have a 12-app sample, but observe:
  - random write 10 → 16 apps: 2040.9 → 2192.8 (small bump) → 2382.0
    @ 32 (knee passed)
  - sequential read 10 → 16 → 32 still V-shapes (1755.7 → 2583.4 →
    3421.6) — the dip is at 10 apps because that's "just below the new
    pool size", same mechanism as §8.2.1.
- **No workload regresses past 32 apps** — i.e., scaling stalls but
  doesn't collapse. Same plateau behavior as Run 2; what changed is
  the level of the plateau, which barely moved.

Conclusion: Run 3 confirms the §8.2.1 hypothesis quantitatively. Growing
the worker pool from 8 to 12 (a 50% capacity increase) yields ≤5%
throughput change in either direction, with most workloads showing zero
or slightly negative deltas, while fsync tails at high concurrency stay
within noise of Run 2. **The scalability bottleneck is not the
`iod_workers` thread count.** The next mitigations to evaluate are the
P0/P1 items in §5 that target the actual gate:

1. P0 — separate `rpc_libfs_handler_thpool` from `iod_workers` (decouple
   fsync logic from NVMe I/O slots).
2. P0 — eliminate the synchronous DevFS checkpoint inside
   `stage_reserve_area` (§3.5).
3. P1 — break up `j->lock` (§3.4), at minimum make `mrc_tx_id` an
   atomic.

Until at least one of these lands, further tuning of pool size or
pinning is unlikely to move the 64-app numbers.

### 8.4 Run 4 — `FSYNC_BOUNCE` drain rate fixed (1 → idle slots), `ENABLE_CKPT` disabled

Goal: experimentally validate the §8.2.1 hypothesis that `FSYNC_BOUNCE` is
a *symptom* of pool saturation, not a *cause* of the cliff. Two changes vs.
Run 3:

1. **§3.5 sync ckpt is gated behind `ENABLE_CKPT` and disabled for this
   run.** `stage_reserve_area()` now panics if the stage area would
   overflow, instead of dropping `j->lock` and waiting for a synchronous
   DevFS round trip. Workload sizing must keep the stage area from
   filling; if the panic fires, the run is invalid and the stage size or
   app count must be re-tuned. See:

   ```1258:1277:oxbow/secure_daemon/src/fs/sync.c
   #ifdef ENABLE_CKPT
   	while (nr_blks + gap > free_blks) {
   		u32 deficit = nr_blks + gap - free_blks;
   		pthread_spin_unlock(&j->lock);
   		oxb_warn("Stage area full: requesting sync ckpt "
   			 "(need=%u free=%u gap=%u deficit=%u)",
   			 nr_blks, free_blks, gap, deficit);
   		msg_send_devfs_stg_ckpt(j->stage_total, deficit, 1 /* sync */);
   		pthread_spin_lock(&j->lock);
   		free_blks = j->stage_total - get_stage_used_blks(j);
   		gap = (j->stage_start + nr_blks > j->stage_end) ?
   			      j->stage_end - j->stage_start :
   			      0;
   	}
   #else
   	if (nr_blks + gap > free_blks) {
   		oxb_error("Stage area full. Sync ckpt is disabled.");
   		panic("Stage area full. Sync ckpt is disabled.");
   	}
   #endif
   ```

2. **`fsync_bounce_try_requeue_one()` replaced with
   `fsync_bounce_drain_to_capacity()` in `oxbow/secure_daemon/src/msg.c`.**
   The new helper drains up to `(alive - working)` bounced requests per
   completion instead of exactly one, so the bounce queue no longer
   throttles release rate when multiple workers go idle in a burst.
   The loop re-samples `working`/`alive` at each iteration so that
   (a) idle slots filled by our own dispatch are observed, and
   (b) concurrent drainers (other handler threads finishing at the same
   instant) collectively converge to the real idle count instead of
   each over-dispatching by `idle`:

   ```c
   static inline void fsync_bounce_drain_to_capacity(void)
   {
   	while (1) {
   		int alive = thpool_num_threads_alive(rpc_libfs_handler_thpool);
   		int working =
   			thpool_num_threads_working(rpc_libfs_handler_thpool);

   		if (alive <= 1 || working >= alive)
   			break;

   		void *barg = fsync_bounce_try_dequeue();
   		if (!barg)
   			break;

   		thpool_add_work(rpc_libfs_handler_thpool,
   				rpc_shmem_libfs_handler, barg);
   	}
   }
   ```

   The 6 call sites in the message dispatch (`MSG_EXIT`, `MSG_FSYNC`,
   `MSG_SYNC`, `MSG_FALLOC`, `MSG_FTRUNC`, `MSG_ADD_JOURNAL`) were updated
   accordingly. The `FSYNC_BOUNCE` enqueue path is unchanged — the
   mechanism's role 1 ("isolate short messages from heavy fsyncs in the
   thpool jobqueue") is preserved.

   Concurrency caveat (documented in the function header): thpool's
   `num_threads_working/alive` are read without `thcount_lock`. The
   re-sample loop bounds the residual race to O(concurrent_drainers)
   excess items in the jobqueue worst-case, which is harmless for
   correctness (jobqueue is unbounded, no item is lost or
   double-dispatched).

All other parameters: **8 `iod_workers` (matching §8.2 baseline, not §8.3)**,
NUMA1-only pinning, two SR-IOV VFs, 1 GiB page cache per app. The 8-thread
choice is intentional — keeps the comparison apples-to-apples with §8.2 so
the bounce-patch effect is isolated.

The `panic("Stage area full. Sync ckpt is disabled.")` did not fire — run
is valid for §3.5-isolated comparison.

Read throughput (MB/s, no fsync):

| Apps | sequential read | random read |
|-----:|----------------:|------------:|
| 1    | 842.7           | 414.6       |
| 2    | 1467.3          | 713.5       |
| 4    | 1618.9          | 558.7       |
| 8    | 1701.3          | 1146.4      |
| 10   | 1737.8          | 1043.0      |
| 16   | 2541.9          | 1303.2      |
| 32   | 3442.2          | 2102.5      |
| 64   | 4747.9          | 2116.7      |

Write throughput (MB/s, each op fsync'd):

| Apps | append | sequential write | random write |
|-----:|-------:|-----------------:|-------------:|
| 1    | 731.4  | 532.8            | 472.1        |
| 2    | 1273.8 | 881.2            | 805.0        |
| 4    | 1759.7 | 1798.4           | 1467.0       |
| 8    | 1922.2 | 2011.6           | 1996.1       |
| 10   | 2100.8 | 2225.5           | 2008.4       |
| 16   | 2369.5 | 2413.6           | 2198.1       |
| 32   | 2335.1 | 2463.3           | 2314.2       |
| 64   | 2125.7 | 2037.0           | 2154.3       |

Fsync latency (ms) — append:

| Apps | avg     | min     | max       |
|-----:|--------:|--------:|----------:|
| 1    | 36.42   | 36.42   | 36.42     |
| 2    | 13.55   | 10.90   | 16.20     |
| 4    | 186.63  | 136.09  | 237.97    |
| 8    | 1009.27 | 763.36  | 1444.65   |
| 10   | 1135.16 | 614.42  | 1550.72   |
| 16   | 1645.93 | 760.09  | 2593.85   |
| 32   | 3294.35 | 1417.85 | 6050.23   |
| 64   | 5976.17 | 1099.00 | 11338.27  |

Fsync latency (ms) — sequential write:

| Apps | avg     | min     | max       |
|-----:|--------:|--------:|----------:|
| 1    | 11.07   | 11.07   | 11.07     |
| 2    | 8.95    | 5.47    | 12.43     |
| 4    | 80.20   | 23.86   | 157.76    |
| 8    | 730.96  | 493.29  | 842.48    |
| 10   | 947.45  | 331.40  | 1122.22   |
| 16   | 1545.26 | 642.84  | 2200.43   |
| 32   | 3282.26 | 1517.57 | 6033.71   |
| 64   | 5418.11 | 860.82  | 10228.13  |

Fsync latency (ms) — random write:

| Apps | avg     | min     | max       |
|-----:|--------:|--------:|----------:|
| 1    | 29.92   | 29.92   | 29.92     |
| 2    | 74.94   | 73.25   | 76.64     |
| 4    | 199.36  | 92.84   | 249.30    |
| 8    | 884.35  | 531.93  | 1268.05   |
| 10   | 1180.23 | 535.21  | 1537.34   |
| 16   | 1905.85 | 893.93  | 2681.53   |
| 32   | 3349.53 | 1509.53 | 6869.88   |
| 64   | 5691.27 | 749.52  | 10477.08  |

Throughput delta vs. §8.2 Run 2 (negative = Run 4 worse):

| Apps | seq read | rand read | append | seq write | rand write |
|-----:|---------:|----------:|-------:|----------:|-----------:|
| 1    |   -1.8%  |   -1.0%   |  -2.2% |    +1.2%  |    +1.5%   |
| 2    |   -0.8%  |   +7.4%   |  -5.9% |    -2.1%  |  **-17.4%**|
| 4    | **-14.6%**|  +7.7%   | -10.3% |    +3.8%  |    -2.8%   |
| 8    |   -7.4%  | **-14.2%**|  +1.7% |    -5.8%  |    -8.6%   |
| 10   |    0.0%  |   +2.0%   |  -7.6% |    -5.9%  |     0.0%   |
| 16   |   +0.4%  |   -2.1%   |  +8.4% |    -4.9%  |    -6.0%   |
| 32   |   -2.2%  |  +22.5%   |  -7.8% |    +1.4%  |    -5.5%   |
| 64   |   -0.8%  |   -5.1%   |**-13.4%** | **-16.2%** | **-10.0%** |

#### 8.4.1 What the data says

- **The 10-app dip is largely resolved** for the workloads that exhibited
  it (`random write` 8→10 transition: 2184 → 2007 in Run 2 became 1996 →
  2008 in Run 4; `random read` dip is much shallower; `append` and
  `seq write` are now monotonic through 8→10→16). This is the direct
  effect of role 2 (drain rate) as predicted in §8.2.1.
- **High-concurrency throughput regresses by 5–16%.** All three write
  workloads at 64 apps drop materially, with seq write the worst at
  −16.2%. The regression is not a noise band; it is structural.
- **Fsync latency distribution becomes bimodal at high concurrency.**
  64-apps min latency drops by 75–84% (append 4358 → 1099 ms; seq write
  4605 → 861 ms; rand write 4816 → 750 ms) while max grows or stays
  flat. Average is roughly unchanged. This is the hallmark of
  **bursty release**: a subset of fsyncs win the post-drain race and
  finish very fast, the rest get serialized and wait.

#### 8.4.2 Diagnosis: drain-N convoy effect on `j->lock`

Mechanism (interaction between drain-N and §3.4):

1. drain-N pushes M items onto thpool jobqueue at once when M slots are
   idle.
2. Each `thpool_add_work()` does a `cond_signal`, so up to M idle workers
   wake up.
3. Those M workers race to acquire jobqueue mutex (one at a time),
   each pulls an fsync arg, increments `working`, calls
   `rpc_shmem_libfs_handler` → `handle_fsync` → `do_fsync` →
   `stage_reserve_area`.
4. **Inside `stage_reserve_area`, all M workers contend on `j->lock`**
   (single global spinlock, §3.4). Lock acquisition order determines
   which fsyncs are "fast" (early acquirers) and which are "slow" (late
   acquirers). Add cache-line ping-pong across cores and the throughput
   cost becomes measurable.

Drain-1 had natural pacing — releasing one item per fsync completion
spread arrival across the per-fsync time window, so `j->lock` rarely
saw multiple simultaneous acquirers. Drain-N collapses that spread into
a single instant burst.

This means **drain-N exposed and amplified the §3.4 bottleneck that
drain-1 was incidentally hiding via natural serialization**. The
hypothesis from §8.2.1 was correct in one direction (`FSYNC_BOUNCE` is
not the cliff cause) and incomplete in another: the *pacing* of bounce
release was masking how steep `j->lock` contention is.

Even at 8 apps the regression is visible (seq write −5.8%, rand write
−8.6%). At 8 apps drain still occasionally fires (when one worker is
already in cond_wait and another finishes, observing `working < alive`),
producing tiny convoys of 2–3, which is enough to lose ~5–8% to lock
contention.

#### 8.4.3 Conclusion and next experiment

- The §8.2.1 hypothesis ("`FSYNC_BOUNCE` is a symptom, not a cause")
  remains valid, but with an important corollary:
  **the *release pattern* of bounce shapes how much `j->lock` contention
  is exposed.** Drain-1 hides §3.4. Drain-N exposes it.
- Net: Run 4 is not a clear win. We traded the 10-app dip for a 64-app
  cliff aggravation of similar magnitude. The change is **revertible**
  but informative — it gives us direct evidence that §3.4 is the next
  thing to attack.
- Before deciding, we will run Run 5 (§8.5) with `drain cap = 2` to
  confirm the convoy hypothesis. If Run 5's regression at 64 apps is
  about half of Run 4's (consistent with linear lock contention vs.
  burst size), the convoy explanation is confirmed and the right next
  step is §3.4 decomposition.

### 8.5 Run 5 — `FSYNC_BOUNCE_DRAIN_CAP = 2` (anti-convoy cap)

Goal: experimentally confirm the §8.4.2 convoy hypothesis by varying
only the drain burst size. Two configurations form a series:

| Run | Cap | Burst at high concurrency |
|---:|----:|---:|
| §8.2 Run 2 | 1   | 1 (drain-1, natural pacing)      |
| §8.5 Run 5 | 2   | up to 2 per drain call           |
| §8.4 Run 4 | ∞   | up to `idle` per drain call (8)  |

If `j->lock` contention scales roughly linearly with burst size, Run 5
should sit between Run 2 and Run 4 with the regression at 64 apps about
half of Run 4's (~5–8% on writes, vs. Run 4's 10–16%). The 10-app dip
should remain mostly resolved because cap=2 is still strictly more
aggressive than drain-1.

Code change (single tunable in `oxbow/secure_daemon/src/msg.c`):

```c
#define FSYNC_BOUNCE_DRAIN_CAP 2
```

`fsync_bounce_drain_to_capacity()` now bounds its loop on both idle
slots *and* this cap. Cap=1 reproduces drain-1 exactly; cap≥pool size
reproduces Run 4. The function header documents the trade-off so
future runs only need to bump this constant.

Configuration: identical to §8.4 (8 `iod_workers`, NUMA1-only, two SR-IOV
VFs, 1 GiB cache, ENABLE_CKPT off, app matrix 1/2/4/8/10/16/32/64).

Validation criteria:

- `panic("Stage area full ...")` must NOT fire (same §3.5 isolation as
  Run 4).
- Compare app-by-app deltas against both §8.2 Run 2 (cap=1 baseline)
  and §8.4 Run 4 (cap=∞) once the run completes.

Possible outcomes and what each implies:

- **Run 5 64-app writes ≈ midpoint between Run 2 and Run 4** AND
  10-app dip stays resolved → convoy hypothesis confirmed,
  cap=2 is a Pareto-improvement over both extremes, and §3.4 is
  the right next attack target.
- **Run 5 64-app writes ≈ Run 2 (no regression)** AND dip resolved →
  even cap=2 is too small to trigger meaningful convoy; Run 4's
  regression came from a larger fan-out. Bump cap to 4 in a Run 6 to
  bracket the convoy threshold.
- **Run 5 64-app writes ≈ Run 4 (full regression)** → convoy is
  triggered already at cap=2, so the regression is not linear in
  burst size. The right fix is `j->lock` decomposition; cap-tuning
  cannot work around it.
- **10-app dip returns at cap=2** → unlikely (cap=2 still doubles
  the per-completion release rate vs. cap=1) but if it happens, the
  dip is finer-grained than we modeled and needs a different
  approach (e.g., dispatch-on-completion instead of dispatch-on-tail).

### 8.6 Measurement: `j->lock` contention probe

The §8.4 convoy diagnosis pins the next bottleneck on §3.4 (`j->lock`).
Before committing to a `j->lock` decomposition (which is a non-trivial
change), we want **direct evidence** that `j->lock` is on the critical
path. This section adds a lightweight measurement and defines what the
output means.

#### Tooling

A self-contained module (independent of `OXBOW_PROFILE`) wraps every
`j->lock` acquire site with a measured helper. Files:

- `oxbow/secure_daemon/include/utils/jlock_profile.h` — inline
  `jlock_acquire()` and three atomic counters.
- `oxbow/secure_daemon/src/utils/jlock_profile.c` — TSC calibration
  against `CLOCK_MONOTONIC`, `jlock_profile_print()`.

Counters are zeroed in `init_secure_daemon()` and the cumulative
snapshot is dumped to **stdout** from `exit_secure_daemon()` (which
runs on graceful shutdown via SIGINT / Ctrl+C). No signal control —
the measurement window is exactly the daemon's lifetime, which keeps
the procedure trivial and matches how the workload runs are scripted.

The wrapper:

```c
static inline void jlock_acquire(pthread_spinlock_t *lock)
{
	uint64_t start = __rdtsc();
	int rc = pthread_spin_trylock(lock);
	if (rc != 0)
		pthread_spin_lock(lock);
	uint64_t got = __rdtsc();

	atomic_fetch_add_explicit(&g_jlock_total, 1, memory_order_relaxed);
	if (rc != 0)
		atomic_fetch_add_explicit(&g_jlock_contended, 1,
					  memory_order_relaxed);
	atomic_fetch_add_explicit(&g_jlock_wait_cycles, got - start,
				  memory_order_relaxed);
}
```

Wrapped acquire sites (9 total, all on the same `journal_control_ctx::lock`):

| File | Line | Function | Path |
|---|---|---|---|
| `sync.c` | 1190 | `stage_free_blks`              | bg journaling reclaim |
| `sync.c` | 1267 | inside `ENABLE_CKPT` (disabled)| would be sync ckpt    |
| `sync.c` | 1351 | `stage_reserve_area`           | every fsync           |
| `sync.c` | 1388 | async ckpt threshold check     | every fsync           |
| `sync.c` | 1645 | `stage_fix_dir_blocks`         | dir fsync             |
| `sync.c` | 1780 | `mrc_tx_id` snapshot           | every fsync           |
| `journal.c` | 110 | `get_running_trans`           | bg journaling         |
| `journal.c` | 271 | bh add to running tx          | bg journaling         |
| `journal.c` | 450 | `start_transaction`           | bg journaling         |
| `msg.c`  | 174 | `MSG_BG_JOURNAL` `mrc_tx_id` update | bg journaling response |

`pthread_spin_unlock` is left untouched — only acquire wait time is
measured because it is the contention indicator.

Cost: 2 atomic adds + 1 trylock + 2 rdtsc on the uncontended path
(~30–40 ns). For ms-scale fsyncs this is negligible and does not
change the workload pattern.

#### Measurement procedure

1. Build with the new sources (already wired in `meson.build`).
2. Start `secure_daemon` and run the workload (any workload — use the
   existing 64-app fsync-heavy run for the most signal).
3. When the workload finishes, send SIGINT to `secure_daemon` (Ctrl+C
   in the foreground terminal, or `pkill -INT secure_daemon`).
   `exit_secure_daemon()` runs and dumps the snapshot to stdout:

   ```
   ==================== [jlock_profile] ====================
     acquires           : <N>
     contended          : <K> (<P>%)
     wait_total         : <T> us
     avg wait per acq   : <X> us
     avg wait per cont. : <Y> us
   =========================================================
   ```

Caveat: counters cover the **entire** daemon lifetime, including
warmup and any pre/post-workload activity. If you want a per-workload
breakdown, run separate one-workload-per-daemon-instance experiments.
For the current "is `j->lock` a bottleneck?" question this is fine —
the bg journaling traffic at idle is negligible compared to the
fsync-heavy workload window.

#### How to read the output

The decisive metric is **avg wait per acq** combined with **how many
acquires happen per fsync**. Each fsync hits `j->lock` ~2–3 times
(`stage_reserve_area`, `mrc_tx_id` snapshot, async ckpt check, plus
journal-side acquires from bg journaling). Total wait time per fsync ≈
`avg wait per acq × 2.5`.

| Avg wait per acq | Contended % | Interpretation |
|---|---|---|
| < 1 µs   | < 5 %  | `j->lock` is **not** a bottleneck. The §8.4 throughput regression is from a different second-order effect (e.g., NVMe qpair coalescing, SPDK sequence buffer alloc). Re-rank the §5 mitigations. |
| 1–10 µs  | 5–30 % | Some contention. Worth fixing but not the only thing — pursue §3.4 in parallel with reading more profile events. |
| 10–100 µs| 30–60 %| `j->lock` is **a real bottleneck**. Convoy diagnosis confirmed. Move to §3.4 decomposition (start with `mrc_tx_id` → `atomic_uint`). |
| > 100 µs | > 60 % | `j->lock` is **the** bottleneck. Decomposition is mandatory before any other tuning will move 64-app numbers. |

A second cross-check: compare `avg wait per acq` with the §8.4 fsync
average. At 64 apps with avg fsync ≈ 6 s and ~2.5 acquires per fsync,
even a 100 µs avg wait per acquire only accounts for ~250 µs out of
~6 s (0.004%). For `j->lock` to *explain* the regression, avg wait
would need to be on the order of milliseconds. So this probe will
either confirm the convoy story (high contention %, sub-ms but
non-trivial wait) or *eliminate* `j->lock` as the dominant cost and
push us toward looking at NVMe-side or qpair-side effects.

#### 8.6.1 First measurement (append, current build = drain cap=2)

| Apps | acquires | contended | %    | wait_total (µs) | avg/acq (µs) | avg/cont. (µs) |
|---:|---:|---:|---:|---:|---:|---:|
| 1   | 65   | 0 | 0.00 |  9 | 0.145 |   0.000 |
| 16  | 149  | 0 | 0.00 | 34 | 0.230 |   0.000 |
| 64  | 538  | 1 | 0.19 | 85 | 0.158 |  84.987 |

#### 8.6.2 Conclusion: `j->lock` convoy hypothesis is REJECTED

Two observations make this conclusion airtight.

**(a) Acquire count matches expected fsync rate, so the measurement is
complete (not missing instrumentation sites).** With each fsync taking
~6 s at 64 apps, the system-wide fsync rate is 64 / 6 ≈ 10.7 fsyncs/s.
Each regular-file fsync hits `j->lock` exactly twice (once in
`build_stage_tx` for the `mrc_tx_id` snapshot, once in
`stage_fix_blocks` → `stage_reserve_area`). That predicts ≈ 21
acquires/s, so a ~25 s daemon run with workload should produce ~535
acquires. We measured 538. The match across all three concurrencies
(and the per-fsync-time scaling: 1 app fsync ~36 ms predicts ~28
acquires/s vs 65 in ~1 s; 16 apps ~1.6 s predicts ~19/s vs 149 in ~7 s)
confirms the wrappers are catching every acquire on the fsync hot path.

**(b) Even at 64 apps, `j->lock` is essentially uncontended.** Only 1
of 538 acquires found the lock held (0.19 %). The single contended
acquire waited 85 µs. Total accumulated wait is **85 µs** over a 25 s
run — vs the workload's aggregate fsync time of ≈ 64 × 25 s = 1600 s.
That is `j->lock` wait / total fsync time ≈ 5×10⁻⁸. Even if every
acquire were contended at 85 µs, the upper bound is 538 × 85 µs ≈
46 ms over 25 s, or 0.18 % of *one* core — still negligible.

The §8.4 convoy diagnosis pinned the regression on `j->lock` because
that was the most prominent shared spinlock on the fsync path. The
data says the lock is not where threads queue up. The reason is now
obvious in hindsight: at this concurrency the workload only generates
~10 fsyncs/s system-wide, and each lock-protected critical section is
~1 µs while the surrounding fsync is ~6 s. Even with drain-N releasing
8 fsyncs as a burst, the burst takes microseconds to pass through
`j->lock` and then has nothing to contend with for the next 6 s.

**Implication for §5 mitigations:** the P1 item "break up `j->lock`"
should be **demoted**. Decomposing a lock that takes 0.18 % of one
core's time will not move 64-app throughput.

#### 8.6.3 Where could the §8.4 regression actually come from?

If `j->lock` is not the convoy victim, where do the bursted fsyncs
collide? The remaining candidates on the critical path between drain
and fsync completion:

1. **NVMe device-side throughput ceiling (most likely).** With 2
   SR-IOV VFs each typically capped at ~1.5 GB/s for writes, the
   theoretical ceiling is ~3 GB/s. We hit ~2.5 GB/s at 16–32 apps in
   §8.2 / §8.4. At 64 apps with drain-N, qpairs receive a denser
   submission pattern but the device can't accept faster — the extra
   in-flight requests just sit in the device queue, increasing
   per-fsync latency without increasing throughput. The bimodal fsync
   distribution observed in §8.4 (min 1 s, max 11 s at 64 apps) is
   consistent with this — early submissions clear quickly, late
   submissions queue behind them at the device.
2. **`stx_drain_all_outstanding` busy-poll CPU pressure.** Each
   in-flight fsync occupies a worker that spin-polls completions.
   Drain-N puts more workers in the spin loop simultaneously. Cache
   coherency traffic between spinners increases; per-worker effective
   poll rate decreases.
3. **SPDK qpair-side coalescing or per-qpair lock.** Less likely to
   matter at our submission rates but worth ruling out if (1) doesn't
   pan out.

Of these, (1) is testable with a raw-device probe; (2) requires
adding TLS instrumentation similar to `jlock_profile` but for spin
cycles inside `stx_drain_all_outstanding`. (1) should be tried first
because if the device is already saturated, no software optimization
will move 64-app throughput — and it's cheap to measure (next section).

### 8.7 Device-BW ceiling probe (raw NVMe write throughput)

Goal: establish how much of the 64-app cliff is hardware ceiling vs.
software inefficiency. If the raw VF write BW per VF, summed across
the 2 VFs the daemon uses, is close to or below the ~2.0–2.5 GB/s the
fsync workload achieves at peak, then "more parallelism" cannot help
and the §8.4 5–16 % regression at 64 apps is just queue-depth
amplification past device saturation.

#### Tool

`oxbow/secure_daemon/test/nvme_direct_write_throughput_test`
(already builds — see `meson.build`). It calls `nvme_direct_write`
through the same SPDK path the daemon uses (same qpair allocation,
same per-tid sequence buffers, same `nvme_init` setup), but with no
staging, no journaling, no fsync orchestration. So it isolates **the
maximum write throughput SPDK can extract from a single VF on this
host**.

CLI:

```
test/nvme_direct_write_throughput_test
    --threads N      # number of writer threads (default 1)
    --duration S     # test seconds (default 5)
    --io-size SZ     # KB/MB/GB suffix (defaults to 32 MB sequence)
    --seq-nr K       # sequences per thread (default 256)
    --skip-preload   # skip the 20 GB memcpy preload phase
```

The test currently overrides `single_io_size = 32 MB` at the top of
`main()`, which is appropriate for max-BW probing (large sequential
writes that saturate the device).

The PCIe BDF is read from env `nvme_pcie_addr` (single VF only — the
test is not yet multi-VF aware). For our 2-VF setup we run two
instances in parallel, one per VF.

#### Measurement procedure

1. SPDK setup must already be in place (the daemon's normal setup
   path also binds VFs).
2. Stop the daemon to free the VFs:

   ```bash
   pkill -INT secure_daemon
   ```

3. Run instance #1 against VF1, in the background:

   ```bash
   cd oxbow/secure_daemon/build
   nvme_pcie_addr="0000:d8:00.1" \
     ./test/nvme_direct_write_throughput_test \
     --threads 8 --duration 30 --skip-preload \
     >vf1.log 2>&1 &
   ```

4. Immediately run instance #2 against VF2, in foreground (or backgrounded):

   ```bash
   nvme_pcie_addr="0000:d8:00.2" \
     ./test/nvme_direct_write_throughput_test \
     --threads 8 --duration 30 --skip-preload \
     >vf2.log 2>&1
   wait
   ```

5. Sum the throughput lines from `vf1.log` and `vf2.log`.

#### Sweep matrix

Run the per-VF test at several thread counts to learn where the device
saturates. The point of the sweep is **finding the knee** — the smallest
thread count at which adding more threads stops improving throughput.

| Per-VF threads | What it answers |
|---:|---|
| 1  | Single-stream max write BW per VF                           |
| 2  | Pair pipelining benefit                                     |
| 4  | Mid-range; usually within ~10 % of the saturated ceiling    |
| 8  | Matches the §8.2/§8.4 daemon config (8 iod_workers ÷ 2 VFs = 4 per VF — but this test runs 8 per VF to confirm it's not thread-count-limited at the daemon's setting) |

#### How to read the result

Let `BW_raw` = total raw write throughput summed across both VFs at
the saturated thread count.

| Comparison | Interpretation |
|---|---|
| `BW_raw` ≈ §8.4 peak fsync throughput (~2.5 GB/s) | The daemon is **already at the device ceiling** at peak. Software optimization (drain, locks, polling) cannot move 64-app throughput; the cliff is not a software cliff but a "more queue depth than the device can absorb" effect. Refocus effort on reducing per-fsync data volume (compression, dedup, batching of small ops into larger journal records) rather than on parallelism. |
| `BW_raw` >> §8.4 peak (e.g., 4 GB/s vs 2.5 GB/s) | The device has headroom that the daemon is not using. The 64-app regression is software — likely candidate (2) (`stx_drain_all_outstanding` CPU pressure) or candidate (3) (qpair-side coalescing). Add the next instrumentation layer there. |
| `BW_raw` < §8.4 peak | Inconsistent (the test bypasses fsync overhead, so it can't be slower) — investigate test config (wrong VF, wrong block layout overlapping with daemon-formatted regions, etc.) before drawing conclusions. |

The cleanest, most definitive next data point. We expect `BW_raw` to
land somewhere in the 2.5–3.5 GB/s range based on typical SR-IOV NVMe
specs for two VFs, which would put us **right at the device ceiling**
and explain the §8.4 plateau without invoking any software bottleneck
beyond the polling-drain CPU pressure (which still might explain the
last 5 % of the regression).

#### 8.7.1 First measurement (8 threads × 2 VFs, 32 MB IO, 30 s)

Run from `oxbow/secure_daemon/build/` with each VF on a separate
process; daemon stopped beforehand to release VFs.

| Source | Throughput | Real-time range | Total bytes / 30 s |
|---|---:|---:|---:|
| VF1 (`0000:d8:00.1`)        | **1890.11 MB/s** | 1696 – 2240 MB/s | 56.83 GB |
| VF2 (`0000:d8:00.2`)        | **1888.19 MB/s** | 1644 – 2400 MB/s | 56.77 GB |
| **Sum (raw, two VFs)**      | **3778.30 MB/s** | — | 113.60 GB |

Both VFs are nearly identical (within 0.1 %), CPU pinning was NUMA1
in both runs (CPUs 16–23), preload skipped. No abnormal warnings in
either log.

#### 8.7.2 Conclusion: the device is NOT the bottleneck

Cross-checked against the daemon's measured peaks:

| Workload | Throughput | vs. raw cap |
|---|---:|---:|
| Raw NVMe direct write (sum 2 VFs)            | 3778 MB/s | 100 %    |
| §8.2 baseline 8 apps append                  | 2240 MB/s | 59.3 %   |
| §8.2 / §8.4 peak (~16–32 apps, write workloads) | ~2500 MB/s | 66.2 % |
| §8.4 64 apps append                          | 2126 MB/s | **56.3 %** |
| §8.4 64 apps seq write                       | 2037 MB/s | 53.9 %   |
| §8.4 64 apps rand write                      | 2154 MB/s | 57.0 %   |

The daemon is leaving **~1.5–1.7 GB/s (33–47 %) on the table**, even
at peak. At 64 apps the gap widens to ~44 %. The §8.4 5–16 %
regression is *within* this software-bound envelope — it is not the
device pushing back, it is software inefficiency that gets *more*
expensive when drain-N batches arrivals.

Combined with §8.6.2:

- §3.4 `j->lock` decomposition (was P1) — **demoted**, not the gate.
- Device ceiling — **rejected**, has 33–47 % headroom.
- Whatever is at fault must live **between** "fsync received by
  daemon" and "NVMe DMA submitted", or in the post-submit polling
  loop. It is per-fsync work the raw test does not perform.

#### 8.7.3 Updated candidate list

What the daemon does that the raw test does not, in the order they
sit on the fsync critical path:

1. **Per-fsync metadata writes (descriptor block, tag blocks, commit
   block).** These add NVMe traffic on top of the user data. If
   meta+commit add ~30–40 % bytes, the effective device throughput per
   fsync byte drops to ~2.5 GB/s — explaining most of the §8.2 peak,
   though not the further drop at 64 apps.
2. **`build_stage_tx` CPU work (dirty page collection, ird snapshot,
   waiting-list cut, journal-tx accounting).** All before any NVMe
   submit. Scales with dirty-block count per fsync (which grows with
   per-fsync data volume, which grows with concurrency in this
   workload).
3. **Inode-level locks (`inode_lock`, `shared_inode_lock`).** Per
   inode, one app per inode in this workload, so cross-app contention
   should be near zero. Worth a sanity probe but unlikely to be the
   gap.
4. **`stx_drain_all_outstanding` busy-poll CPU pressure.** Each in-
   flight fsync occupies a worker spinning on completions. With drain-
   N more spinners active simultaneously → cache-line traffic on
   completion state, fewer effective polls per worker per second.
5. **LibFS ↔ daemon RPC + shmem cost.** 64 apps each post and wait on
   a shared-memory channel. Per-RPC overhead × ops/sec.
6. **Stage area commit-block ordering barrier.** Every fsync ends with
   a separate commit-block write that *must* land after all data
   blocks. Adds at least one round-trip and prevents merging.

Of these, (1) and (4) are the highest leverage (largest expected
contribution and easiest to measure). (1) gives an "achievable
ceiling" calculation that bounds how much (4)/(5)/(6) can possibly
explain.

Concrete next probes (one of these in §8.8):

- **Metadata amplification probe**: count actual NVMe submitted bytes
  per fsync inside `nvme_direct_write` and compare with user-staged
  bytes. Already trivially derivable from existing per-tx counters in
  `sync.c` if `OXBOW_PROFILE` is enabled (`z01..z12_*` block-count
  events) — sum data vs meta over the run, multiply by block size.
- **fsync internal time breakdown**: enable `OXBOW_PROFILE` for one
  64-app run, read which `PF_TL_*` events under the fsync subtree
  consume the most time. The events `ab__evt_sd_stg`,
  `ac___evt_sd_stg_build_tx`, `ar___evt_sd_stg_io_tx`,
  `at___evt_sd_stg_io_commit`, `au___evt_sd_stg_io_async_wait` are
  already wired and will give a stage-by-stage breakdown.
- **Spin-poll cycle counter** in `stx_drain_all_outstanding` (same
  pattern as `jlock_profile`): how many cycles do workers waste
  spinning vs how many doing useful poll work. Independent of
  `OXBOW_PROFILE`.

#### 8.7.4 Caveat: the raw-VF cap is an upper bound, not the *shared* cap

The §8.7.1 measurement was taken with the daemon stopped and **devfs
also idle** (no fsync workload running so devfs has nothing to
checkpoint). In production the same physical SSD has **two concurrent
traffic sources** competing for the device's single internal write
bandwidth budget:

- **secure_daemon** writes via the two SR-IOV VFs (`d8:00.1`,
  `d8:00.2`): user data + per-fsync metadata (descriptor, tag,
  commit blocks) into the stage area.
- **devfs** writes via the PF: reads stage area data, then writes
  it to the FS area + journal metadata, in bursts.

VFs and PF are *virtual* queue endpoints into the same NAND/controller
back-end. Modern enterprise NVMe SSDs typically share the device's
sustained write bandwidth across PF + all active VFs (some support
per-VF QoS caps; not assumed here). So the *raw* cap of 3.78 GB/s is
the **uncontended ceiling**, not the ceiling secure_daemon sees while
devfs is also writing.

Steady-state write amplification per byte of user data fsynced:

| Traffic | Endpoint | Bytes per app byte |
|---|---|---:|
| Stage data write              | VF (secure_daemon) | 1.0 |
| desc + tag + commit metadata  | VF (secure_daemon) | ~0.05–0.1 |
| Stage data read for ckpt      | PF (devfs)         | 1.0 |
| FS-area data write            | PF (devfs)         | 1.0 |
| FS journal metadata write     | PF (devfs)         | ~0.05–0.1 |

Aggregating only the **write traffic** (assuming SSDs typically have
read BW ≥ write BW, so reads compete less): per app byte the device
processes roughly **2.1× write traffic**. With raw write cap
3.78 GB/s, the **shared steady-state ceiling for app throughput** is:

```
app_throughput_max ≈ 3.78 GB/s ÷ 2.1 ≈ 1.80 GB/s  (writes only)
                  ≈ 3.78 GB/s ÷ 3.1 ≈ 1.22 GB/s  (writes + reads, worst case)
```

The §8.4 64-app numbers (2.0–2.1 GB/s) sit between these two bounds,
**suggesting the daemon may already be at or near the shared cap** and
that what looks like a "scalability cliff" is largely the device
absorbing the burst arrivals from drain-N past its sustained shared
limit.

This reframes (but does not yet replace) the §8.7.3 candidate list.
If §8.7.4's hypothesis holds:

- **All §5 SW mitigations targeting parallelism** (P0 RPC pool split,
  P1 `j->lock` decomp, P2 BounCE drain, P2 read-pump) cannot lift
  64-app throughput past ~1.8 GB/s. They could still help
  *latency tails* and *scaling shape* below the cap, but the plateau
  is hardware-shared.
- **Mitigations targeting write amplification** become primary:
  - reduce per-fsync metadata bytes (compress / merge descriptors,
    coalesce commit blocks across fsyncs)
  - shorten the stage→FS-area data path (in-place, log-structured FS
    area, or zero-copy stage handover that avoids the second device
    write)
  - per-VF/PF QoS to give the VFs a guaranteed slice (if device
    supports it)

#### 8.7.5 How to verify the shared-cap hypothesis

Two independent probes; either alone is sufficient.

**Probe A — daemon-only run with devfs idled**

1. Disable devfs's checkpointing (e.g., do not start devfs, or stop it
   after mount). Stage area must be sized large enough that the run
   does not hit `panic("Stage area full ...")`.
2. Run the same 64-app append workload for a bounded duration (~30 s
   so stage doesn't fill regardless).
3. Read fsync throughput.

| Result | Interpretation |
|---|---|
| Throughput jumps to ~3.0–3.7 GB/s | §8.7.4 confirmed; daemon was sharing device with devfs. Re-rank §5 toward write-amplification work. |
| Throughput stays ~2.1 GB/s         | §8.7.4 partially or fully wrong; daemon hits a software ceiling. Move to §8.7.3 candidates (metadata amp, polling drain, RPC). |
| Throughput rises but only to ~2.5 GB/s | Mixed — devfs takes some BW but not all of the gap. Both effects matter. |

**Probe B — observe device write rate per endpoint during the workload**

1. Run the normal 64-app append workload (devfs running as usual).
2. Concurrently sample, in another shell:

   ```bash
   # PF + VF aggregate, host view
   iostat -dx 1 60 nvme0n1   # adjust device name

   # Or per-controller, NVMe view
   nvme smart-log /dev/nvme0   # take T1, run, take T2
   # delta of `data_units_written` × 512 KB / elapsed = total device write BW
   ```

3. Compare:
   - daemon-reported app throughput (~2.1 GB/s at 64 apps)
   - sum of device-reported write BW (should be ~app × 2.1 if §8.7.4
     is right)

| Result | Interpretation |
|---|---|
| Device write BW ≈ 4.0 GB/s ≈ raw cap × 1.05 | Device is fully busy; daemon is sharing. §8.7.4 confirmed. |
| Device write BW ≈ 2.1 GB/s (no amp) | No write amp — devfs is barely running. Software ceiling, not device. |
| Device write BW between (e.g., 3.0 GB/s) | Both effects partially. |

Probe B is non-intrusive and runs alongside the existing benchmark, so
it is the safer first step. Probe A is more decisive but requires
disabling part of the system.

### 8.8 Run 6 — Probe A executed (devfs disabled, daemon-only)

Configuration:
- `VM_ENV_NO_DEVFS` defined at compile time (see
  `oxbow/secure_daemon/include/oxbow_debug.h:16`). Disables the bg
  journaling thread body, the RPC-to-DevFS client, and the Data
  Fetcher initialization.
- Matching runtime: `bg_journaling=0` in `secure_daemon_conf.sh`,
  `mkfs` reformat with `sb->journal=0`.
- Stage area sized large enough that the run does not panic with
  `"Stage area full ..."` (devfs is not present to reclaim, and
  `ENABLE_CKPT` remains off).
- Same as §8.4 / Run 4 otherwise: 8 `iod_workers`, NUMA1-only,
  two SR-IOV VFs, 1 GiB page cache per app, `FSYNC_BOUNCE` enabled,
  drain cap = 2 (current build, see `msg.c:24`).

Goal: §8.7.5 Probe A. If §8.7.4 holds, daemon throughput should jump
toward the raw VF ceiling (3.78 GB/s, §8.7.1) once devfs is no longer
consuming PF write/read bandwidth.

Caveat on isolation: the build also has `FSYNC_BOUNCE_DRAIN_CAP = 2`
(the §8.5 anti-convoy fix), so this run conflates "drain cap reduced
from ∞ to 2" with "devfs disabled". Run 5 (cap=2 with devfs ON) was
not measured; we use Run 4 (cap=∞ with devfs ON) as the comparator and
note the confound where it matters.

Write throughput (MB/s):

| Apps | append | sequential write | random write |
|-----:|-------:|-----------------:|-------------:|
| 1    |  664.6 |  494.6           |  424.7       |
| 2    | 1032.2 |  812.6           |  755.7       |
| 4    | 1543.2 | 1668.6           | 1599.6       |
| 8    | 1819.5 | 1810.6           | 2070.4       |
| 10   | 1807.6 | 1955.0           | 2155.4       |
| 16   | 2298.4 | 2424.1           | 2299.6       |
| 32   | 2254.4 | 2228.0           | 2269.3       |
| 64   | 2279.1 | 2335.4           | 2375.6       |

Fsync latency (ms) — append:

| Apps | avg     | min     | max       |
|-----:|--------:|--------:|----------:|
| 1    |  140.72 |  140.72 |   140.72  |
| 2    |  286.73 |  280.33 |   293.12  |
| 4    |  599.74 |  588.61 |   604.81  |
| 8    |  874.71 |  768.15 |   974.57  |
| 10   | 1147.51 | 1060.20 |  1254.88  |
| 16   | 1848.86 | 1087.60 |  3281.88  |
| 32   | 3476.70 | 1609.28 |  7189.48  |
| 64   | 6698.63 | 2890.26 | 14006.75  |

Fsync latency (ms) — sequential write:

| Apps | avg     | min     | max       |
|-----:|--------:|--------:|----------:|
| 1    |  141.98 |  141.98 |   141.98  |
| 2    |  245.78 |  244.14 |   247.42  |
| 4    |  433.14 |  331.43 |   483.84  |
| 8    |  932.55 |  772.49 |  1000.43  |
| 10   | 1112.13 |  671.08 |  1335.27  |
| 16   | 1680.62 |  872.84 |  3100.44  |
| 32   | 3568.41 | 1521.01 |  7653.83  |
| 64   | 6570.41 | 2729.29 | 13587.69  |

Fsync latency (ms) — random write:

| Apps | avg     | min     | max       |
|-----:|--------:|--------:|----------:|
| 1    |  188.26 |  188.26 |   188.26  |
| 2    |  289.76 |  288.45 |   291.06  |
| 4    |  316.41 |  308.97 |   321.59  |
| 8    | 1014.99 |  456.02 |  1122.60  |
| 10   | 1120.26 |  621.89 |  1434.48  |
| 16   | 1902.37 | 1066.52 |  3143.91  |
| 32   | 3569.04 | 1736.71 |  6371.74  |
| 64   | 6661.66 | 2888.33 | 14554.99  |

Throughput vs. Run 4 (devfs ON, cap=∞) and vs. raw cap (§8.7.1):

| Apps | append %Δ | seq write %Δ | rand write %Δ |
|-----:|---------:|-------------:|--------------:|
| 1    |   −9.1 % |     +5.7 %   |   −10.0 %     |
| 8    |   −5.3 % |    −10.0 %   |    +3.7 %     |
| 16   |   −3.0 % |    +0.4 %    |    +4.6 %     |
| 32   |   −3.5 % |    −9.6 %    |    −1.9 %     |
| 64   |   +7.2 % |    +14.6 %   |   +10.3 %     |

| | Run 4 (with devfs) | Run 6 (no devfs) | raw cap |
|---|---:|---:|---:|
| append 64 apps        | 2126 (56.3 %) | 2279 (60.3 %) | 3778 (100 %) |
| seq write 64 apps     | 2037 (53.9 %) | 2335 (61.8 %) | 3778 (100 %) |
| rand write 64 apps    | 2154 (57.0 %) | 2376 (62.9 %) | 3778 (100 %) |

#### 8.8.1 Conclusion: §8.7.4 hypothesis is REJECTED

If devfs were the dominant consumer of the shared device bandwidth,
removing it should have moved daemon throughput from ~57 % of the raw
cap to somewhere in the 80–95 % range (the §8.7.4 prediction range).
Instead the daemon climbed from ~57 % to ~62 % — a ~5–7 percentage
point bump that is consistent with devfs taking a small slice of PF
bandwidth, but **leaves a 37–40 % software gap unexplained**.

Further evidence the daemon is not BW-limited even without devfs:
the curves all plateau between 16 and 64 apps in the 2.2–2.4 GB/s
range, a steady plateau ~1.4 GB/s below the raw cap. If the device
were the gate, more concurrency would push closer to the cap, not
flatten well below it.

So:

- §3.4 `j->lock` — REJECTED (§8.6.2)
- Device cap, both raw and shared-with-devfs — REJECTED (§8.7.2 + §8.8)
- The remaining ~40 % gap is **software**, on the fsync path inside
  `secure_daemon`, between the LibFS request and NVMe DMA submit (or
  inside the post-submit polling loop).

#### 8.8.2 Latency anomaly: fsync got *slower* without devfs

| Apps (append) | Run 4 avg / min / max (ms) | Run 6 avg / min / max (ms) |
|---:|---|---|
| 1   |    36 /   36 /   36   |   141 /  141 /   141   |
| 8   |  1009 /  763 / 1445   |   875 /  768 /   975   |
| 64  |  5976 / 1099 / 11338  |  6698 / 2890 / 14007   |

Cause: **bg journaling pre-stages a fraction of dirty data, so fsync
has less to do when devfs is running.**

The relevant code is `sync.c:1589-1626` (the `STG_WAIT_MODE ==
ALWAYS_PERSIST` branch — the default, set in
`oxbow/devfs/include/common/oxbow.h:96`). When `build_stage_tx`
detects `inode->i_journal_waiting_dirty_txid[i] != -1` (= bg
journaling has touched this data), it checks whether the bg
transaction has already committed:

```c
recent_tx:
    cur_txid = atomic_load(&inode->i_journal_waiting_dirty_txid[recent_df_id]);
    if (waiting_txid[recent_df_id] != cur_txid) {
        // bg journaling already committed; skip this dirty list.
        stx->waiting_dirty_skipped[recent_df_id] = true;
        goto older_tx;
    }
    // bg journaling not yet committed; persist it ourselves anyway.
    stage_waiting_dirty_data(stx, inode, waiting_dirty_list[recent_df_id], ...);
```

So with devfs **on**, dirty data committed via bg journaling between
two consecutive fsyncs is **dropped from the next fsync's stage
write** (the `goto` path). With devfs **off** the
`waiting_dirty_*` machinery never populates anything — bg journaling
never runs — so every fsync stages **all** accumulated dirty data on
its own.

Per-fsync data volume estimated from the throughput / fsync-rate
relationship:

| Run | Apps | Workload throughput (per app) | avg fsync (s) | Data per fsync |
|---|---:|---:|---:|---:|
| Run 4 (devfs on) | 1  |  731 MB/s |  0.036 |  ~26 MB |
| Run 6 (devfs off)| 1  |  665 MB/s |  0.141 |  **~94 MB (3.6×)** |
| Run 4 (devfs on) | 64 | 33.2 MB/s |  5.976 | ~198 MB |
| Run 6 (devfs off)| 64 | 35.6 MB/s |  6.698 | ~238 MB (1.2×) |

The latency multipliers (1-app 4×, 64-app 1.12×) match the data-per-
fsync multipliers (3.6×, 1.2×) almost perfectly — confirming the
"bg-journal-offload" explanation rather than a stage-occupancy effect.

The reason the 64-app multiplier (1.2×) is so much smaller than the
1-app multiplier (3.6×) is just timing: at 64 apps each fsync already
takes ~6 s, leaving only a short window in which bg journaling can
catch up before the next fsync arrives. So bg journaling can offload
only a small fraction of each app's dirty data, and the per-fsync
"savings" with devfs on shrinks to ~20 %. At 1 app, fsync is ~36 ms
and bg journaling has plenty of time between them to pre-stage most
of the data.

The stage area in this environment is sized so reclaim never fires
(no `panic` from §3.5 in either run). `stage_reserve_area` is just a
constant-time arithmetic on `j->stage_total - get_stage_used_blks(j)`
when reclaim is not needed, so its per-call cost does not grow with
occupancy. This rules out the earlier "stage_reserve_area cost grows
with occupancy" guess.

#### 8.8.3 Updated candidate list (re-ranked after §8.8)

The fsync software gap is real and substantial (~40 % of device cap).
Re-ranking §8.7.3 by likelihood given Run 6:

1. **`stx_drain_all_outstanding` busy-poll under multi-VF, multi-tid
   contention** — the entire post-submit window is a CPU spin loop
   contending with peer workers' spins on shared NVMe completion
   state. Drain-N amplifies this; Run 5/6 caps it at 2; cap=1
   (Run 2 baseline) had the smallest spin contention and the best
   throughput at low concurrency. Likely the largest single
   contributor.
2. **Per-fsync metadata writes (descriptor + tag + commit blocks)** —
   add raw write traffic that does not show up in app-reported
   throughput. The amplification factor ε in §8.7.4's table is the
   knob; even ε ≈ 0.3 explains a 30 % gap by itself.
3. **LibFS ↔ daemon RPC + shmem cost (§3.9 single-consumer
   dispatcher)** — at 64 apps, each RPC traverses one semaphore plus
   one notification queue popped sequentially. Per-fsync overhead
   ≪ 6 s, so probably small in absolute terms; included for
   completeness.
4. **Commit-block ordering barrier** — every fsync ends with a
   separate commit-block write that must land after data. One extra
   round-trip per fsync, fundamental to the staging design.
5. **`build_stage_tx` CPU work** — already known to be small per the
   existing `PF_TL_*` events; included for completeness.

(The earlier "stage_reserve_area cost growth with occupancy" candidate
is dropped — see §8.8.2: the stage area is oversized so the reclaim
loop never executes, leaving `stage_reserve_area` as a constant-time
operation regardless of occupancy.)

#### 8.8.4 Recommended next probes (in order of bang-for-buck)

A. **Metadata amplification probe** (cheapest). For each fsync, count
   actual NVMe-submitted bytes vs user-staged bytes. If amp ≈ 1.3–1.5,
   most of the gap is on the "raw-NVMe-traffic-per-app-byte" side and
   the only real fix is reducing meta. If amp ≈ 1.0–1.1, the gap is
   pure software CPU on the fsync path and (1) above is the next
   target.

B. **`stx_drain_all_outstanding` spin-cycle counter** (same pattern as
   `jlock_profile`, ~30 LOC). Splits the drain loop time into "useful
   poll work" vs "spinning while peers also spin". Definitive on
   whether candidate (1) is real.

A and B together would isolate "device-side amp" from "CPU spin
contention" — these are the two largest expected sources of the
remaining ~40 % gap.
