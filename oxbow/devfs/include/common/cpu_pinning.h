#ifndef _OXB_CPU_PINNING_H_
#define _OXB_CPU_PINNING_H_

/*
 * Single source of truth for IO worker CPU pinning policy.
 *
 * Secure Daemon and DevFS may run on the same host in host-journaling mode,
 * so they have separate pinning policies. Keep their CPU ranges disjoint
 * unless an experiment intentionally wants overlap.
 *
 * HARDCODED testbed CPU layout (Libra06-class dual-socket):
 *   NUMA0 base = 0, NUMA1 base = 16.
 *   NUMA0 HT   = 32, NUMA1 HT   = 48.
 *
 * Hyper-threading on/off does NOT change these bases on this machine
 * because the kernel enumerates primary HT siblings first (0..31),
 * then mirror siblings (32..63). Verify with `lscpu -e`.
 */

#define OXB_NUMA0_BASE 0
#define OXB_NUMA1_BASE 16
#define OXB_NUMA0_HT_BASE 32
#define OXB_NUMA1_HT_BASE 48

/*
 * Pinning mode. Choose ONE:
 *
 *   OXB_PIN_MODE_SPLIT_NUMA
 *     Split IO workers across NUMA0 and NUMA1. First half of tids go to
 *     NUMA0 (NUMA0 takes the extra one when total is odd); the rest go
 *     to NUMA1. DPDK lcores and hugepages land on both sockets.
 *
 *   OXB_PIN_MODE_NUMA0_ONLY
 *     Pin every IO worker to NUMA0 primary CPUs first (CPU 0..15),
 *     then to NUMA0 hyper-thread siblings (CPU 32..47) when total
 *     worker/qpair count exceeds 16.
 *
 *   OXB_PIN_MODE_NUMA1_ONLY
 *     Pin every IO worker to NUMA1 primary CPUs first (CPU 16..31),
 *     then to NUMA1 hyper-thread siblings (CPU 48..63) when total
 *     worker/qpair count exceeds 16. This keeps 3-VF / 24-qpair
 *     experiments on the storage-local NUMA node.
 */
#define OXB_PIN_MODE_SPLIT_NUMA 0
#define OXB_PIN_MODE_NUMA0_ONLY 1
#define OXB_PIN_MODE_NUMA1_ONLY 2

enum oxb_pin_domain {
	OXB_PIN_DOMAIN_SECURE_DAEMON = 0,
	OXB_PIN_DOMAIN_DEVFS = 1,
};

#ifndef OXB_SD_PIN_MODE
#ifdef OXB_PIN_MODE
#define OXB_SD_PIN_MODE OXB_PIN_MODE
#else
#define OXB_SD_PIN_MODE OXB_PIN_MODE_NUMA1_ONLY
#endif
#endif

#ifndef OXB_DEVFS_PIN_MODE
#define OXB_DEVFS_PIN_MODE OXB_PIN_MODE_NUMA0_ONLY
#endif

/*
 * Return the CPU id that IO worker `tid` should be pinned to when the
 * pool has `total` IO worker threads.
 *
 * Examples (OXB_PIN_MODE_SPLIT_NUMA, default):
 *   total=8  -> tid 0..3 -> CPU 0..3,  tid 4..7  -> CPU 16..19
 *   total=16 -> tid 0..7 -> CPU 0..7,  tid 8..15 -> CPU 16..23
 *   total=9  -> tid 0..4 -> CPU 0..4,  tid 5..8  -> CPU 16..19
 *
 * Examples (OXB_PIN_MODE_NUMA0_ONLY):
 *   total=8  -> tid 0..7 -> CPU 0..7
 *   total=16 -> tid 0..15 -> CPU 0..15
 *   total=24 -> tid 0..15 -> CPU 0..15, tid 16..23 -> CPU 32..39
 * Examples (OXB_PIN_MODE_NUMA1_ONLY):
 *   total=8  -> tid 0..7 -> CPU 16..23
 *   total=16 -> tid 0..15 -> CPU 16..31
 *   total=24 -> tid 0..15 -> CPU 16..31, tid 16..23 -> CPU 48..55
 */
static inline int oxb_pin_cpu_for_mode(int mode, int tid, int total)
{
	if (mode == OXB_PIN_MODE_NUMA0_ONLY) {
		if (tid < 16)
			return OXB_NUMA0_BASE + tid;
		return OXB_NUMA0_HT_BASE + (tid - 16);
	}

	if (mode == OXB_PIN_MODE_NUMA1_ONLY) {
		if (tid < 16)
			return OXB_NUMA1_BASE + tid;
		return OXB_NUMA1_HT_BASE + (tid - 16);
	}

	int half = (total + 1) / 2;
	if (tid < half)
		return OXB_NUMA0_BASE + tid;
	return OXB_NUMA1_BASE + (tid - half);
}

/*
 * Return the NUMA node index (0 or 1) that tid maps to under the policy
 * above. Useful for logging so callers do not have to duplicate the
 * mode-specific split.
 */
static inline int oxb_pin_numa_for_mode(int mode, int tid, int total)
{
	if (mode == OXB_PIN_MODE_NUMA0_ONLY)
		return 0;

	if (mode == OXB_PIN_MODE_NUMA1_ONLY)
		return 1;

	int half = (total + 1) / 2;
	return (tid < half) ? 0 : 1;
}

static inline int oxb_sd_pin_cpu_for_tid(int tid, int total)
{
	return oxb_pin_cpu_for_mode(OXB_SD_PIN_MODE, tid, total);
}

static inline int oxb_sd_pin_numa_for_tid(int tid, int total)
{
	return oxb_pin_numa_for_mode(OXB_SD_PIN_MODE, tid, total);
}

static inline int oxb_devfs_pin_cpu_for_tid(int tid, int total)
{
	return oxb_pin_cpu_for_mode(OXB_DEVFS_PIN_MODE, tid, total);
}

static inline int oxb_devfs_pin_numa_for_tid(int tid, int total)
{
	return oxb_pin_numa_for_mode(OXB_DEVFS_PIN_MODE, tid, total);
}

static inline int oxb_pin_cpu_for_domain(enum oxb_pin_domain domain, int tid,
					 int total)
{
	if (domain == OXB_PIN_DOMAIN_DEVFS)
		return oxb_devfs_pin_cpu_for_tid(tid, total);
	return oxb_sd_pin_cpu_for_tid(tid, total);
}

static inline int oxb_pin_numa_for_domain(enum oxb_pin_domain domain, int tid,
					  int total)
{
	if (domain == OXB_PIN_DOMAIN_DEVFS)
		return oxb_devfs_pin_numa_for_tid(tid, total);
	return oxb_sd_pin_numa_for_tid(tid, total);
}

static inline const char *oxb_pin_domain_name(enum oxb_pin_domain domain)
{
	if (domain == OXB_PIN_DOMAIN_DEVFS)
		return "devfs";
	return "secure_daemon";
}

static inline int oxb_pin_cpu_for_tid(int tid, int total)
{
	return oxb_sd_pin_cpu_for_tid(tid, total);
}

static inline int oxb_pin_numa_for_tid(int tid, int total)
{
	return oxb_sd_pin_numa_for_tid(tid, total);
}

#endif /* _OXB_CPU_PINNING_H_ */
