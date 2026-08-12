// SPDX-License-Identifier: GPL-2.0
/*
 * RK3588 cross-core configurable-address probe experiment.
 *
 * CPU probe_cpu primes and reloads line[access_items].  Every CPU selected
 * by stimulus_cpus accesses line[0]..line[access_items - 1] for fill_rounds
 * ordered passes between those two probe accesses.  Each candidate/baseline
 * pair reuses the same target-matching probe address.  Candidate stimulus
 * addresses have the same (physical_address & addrmask) value as the probe,
 * while baseline stimulus addresses do not.  The address pool is enumerated
 * in 64-byte cache-line units.
 */
#include <linux/bitops.h>
#include <linux/completion.h>
#include <linux/cpu.h>
#include <linux/fs.h>
#include <linux/kernel.h>
#include <linux/kthread.h>
#include <linux/math64.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/random.h>
#include <linux/sched.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/vmalloc.h>
#include <linux/wait.h>

#include <asm/barrier.h>
#include <asm/sysreg.h>
#include <asm/unaligned.h>

#ifndef SFCP_NAME
#define SFCP_NAME                    "rk3588_sf_crossprobe"
#endif
#ifndef SFCP_POOL_PAGES_DEFAULT
#define SFCP_POOL_PAGES_DEFAULT      393216U /* 1.5 GiB */
#endif
#define SFCP_POOL_PAGES_MAX          1048576U /* 4 GiB */
#ifndef SFCP_ACCESS_ITEMS_DEFAULT
#define SFCP_ACCESS_ITEMS_DEFAULT    8U
#endif
#define SFCP_ACCESS_ITEMS_MAX        4095U
#ifndef SFCP_FILL_ROUNDS_DEFAULT
#define SFCP_FILL_ROUNDS_DEFAULT     10U
#endif
#define SFCP_FILL_ROUNDS_MAX         1000U
#define SFCP_DEFAULT_BASELINE_SETS   5000U
#define SFCP_DEFAULT_CANDIDATE_SETS  5000U
#define SFCP_MAX_SAMPLES             200000U
#define SFCP_CROSS_RETEST_THRESHOLD_DEFAULT 150U
#define SFCP_CROSS_RETEST_REPETITIONS_DEFAULT 100U
#define SFCP_CROSS_RETEST_REPETITIONS_MAX 1000U
#define SFCP_CROSS_RETEST_MAX_GROUPS_DEFAULT 1000U
#define SFCP_CROSS_RETEST_MAX_GROUPS 1000U
#ifndef SFCP_MATCH_BITS_DEFAULT
#define SFCP_MATCH_BITS_DEFAULT      23U
#endif
#define SFCP_MATCH_BITS_MIN          PAGE_SHIFT
#define SFCP_MATCH_BITS_MAX          32U
#define SFCP_DEFAULT_OFFSET          0xfc0U
#define SFCP_CACHE_LINE_SIZE         64U
#define SFCP_LINES_PER_PAGE          (PAGE_SIZE / SFCP_CACHE_LINE_SIZE)
#define SFCP_TARGET_RANDOM           U64_MAX
#define SFCP_PREFILL_WAIT_US_MAX     10000U
#define SFCP_PMU_EVENT_L1D_REFILL    0x03U
#define SFCP_PMU_EVENT_L1D_TLB_REFILL 0x05U
#define SFCP_PMU_EVENT_L2D_REFILL    0x17U
#define SFCP_PMU_EVENT_BUS_ACCESS    0x19U
#define SFCP_PMU_EVENT_LL_CACHE_RD   0x36U
#define SFCP_PMU_EVENT_LL_CACHE_MISS_RD 0x37U
#define SFCP_PMU_BASE_EVENT_MASK     (BIT(1) | BIT(2) | BIT(3))
#define SFCP_PMU_LL_EVENT_MASK       (BIT(4) | BIT(5))
#define SFCP_PMU_TLB_EVENT_MASK      BIT(0)

static unsigned int pool_pages = SFCP_POOL_PAGES_DEFAULT;
module_param(pool_pages, uint, 0444);
MODULE_PARM_DESC(pool_pages,
		 "4 KiB pages in private pool (393216 = 1.5 GiB, maximum 1048576 = 4 GiB)");

static int probe_cpu = 7;
module_param(probe_cpu, int, 0444);
MODULE_PARM_DESC(probe_cpu,
		 "Linux CPU that primes and reloads the final group address");

static int stimulus_cpu = -1;
module_param(stimulus_cpu, int, 0444);
MODULE_PARM_DESC(stimulus_cpu,
		 "Deprecated and ignored; use stimulus_cpus instead");

#ifndef SFCP_STIMULUS_CPUS_DEFAULT
#define SFCP_STIMULUS_CPUS_DEFAULT "0-6"
#endif
static char *stimulus_cpus = SFCP_STIMULUS_CPUS_DEFAULT;
module_param(stimulus_cpus, charp, 0444);
MODULE_PARM_DESC(stimulus_cpus,
		 "CPU list that concurrently accesses the stimulus lines (for example 0-6 or 0,2,4-6)");

static unsigned int line_offset = SFCP_DEFAULT_OFFSET;
module_param(line_offset, uint, 0444);
MODULE_PARM_DESC(line_offset,
		 "Deprecated 64-byte-aligned offset used only for counter calibration");

#ifndef SFCP_TARGET_LOW_DEFAULT
#define SFCP_TARGET_LOW_DEFAULT SFCP_TARGET_RANDOM
#endif
static unsigned long long target_low = SFCP_TARGET_LOW_DEFAULT;
module_param(target_low, ullong, 0444);
MODULE_PARM_DESC(target_low,
		 "Required value of (physical_address & addrmask); omitted means random");

static bool sfcp_target_random;

#ifndef SFCP_ADDRMASK_DEFAULT
#define SFCP_ADDRMASK_DEFAULT 0ULL
#endif
static unsigned long long addrmask = SFCP_ADDRMASK_DEFAULT;
module_param(addrmask, ullong, 0444);
MODULE_PARM_DESC(addrmask,
		 "Physical-address grouping mask; candidates have identical (address & addrmask); 0 uses legacy match_bits");

static unsigned int match_bits = SFCP_MATCH_BITS_DEFAULT;
module_param(match_bits, uint, 0444);
MODULE_PARM_DESC(match_bits,
		 "Legacy low-bit count used only when addrmask is omitted");

static unsigned int fill_rounds = SFCP_FILL_ROUNDS_DEFAULT;
module_param(fill_rounds, uint, 0444);
MODULE_PARM_DESC(fill_rounds,
		 "Passes each selected stimulus CPU makes over the access_items lines");

static unsigned int prefill_wait_us;
module_param(prefill_wait_us, uint, 0444);
MODULE_PARM_DESC(prefill_wait_us,
		 "Busy-wait microseconds between prefill-hot and wait-prefill-hot probe measurements (maximum 10000)");

static unsigned int access_items = SFCP_ACCESS_ITEMS_DEFAULT;
module_param(access_items, uint, 0444);
MODULE_PARM_DESC(access_items,
		 "Addresses touched by every stimulus CPU per pass; group has one additional probe address");

static unsigned int baseline_sets = SFCP_DEFAULT_BASELINE_SETS;
module_param(baseline_sets, uint, 0444);
MODULE_PARM_DESC(baseline_sets, "Number of unrelated baseline groups");

static unsigned int candidate_sets = SFCP_DEFAULT_CANDIDATE_SETS;
module_param(candidate_sets, uint, 0444);
MODULE_PARM_DESC(candidate_sets, "Number of same-low-bits candidate groups");

static unsigned int cross_retest_threshold =
	SFCP_CROSS_RETEST_THRESHOLD_DEFAULT;
module_param(cross_retest_threshold, uint, 0444);
MODULE_PARM_DESC(cross_retest_threshold,
		 "Retest a candidate group when its initial cross reload exceeds this many counter ticks");

static unsigned int cross_retest_repetitions =
	SFCP_CROSS_RETEST_REPETITIONS_DEFAULT;
module_param(cross_retest_repetitions, uint, 0444);
MODULE_PARM_DESC(cross_retest_repetitions,
		 "Cross-only repetitions for each candidate group above the threshold; 0 disables retesting");

static unsigned int cross_retest_max_groups =
	SFCP_CROSS_RETEST_MAX_GROUPS_DEFAULT;
module_param(cross_retest_max_groups, uint, 0444);
MODULE_PARM_DESC(cross_retest_max_groups,
		 "Maximum number of above-threshold candidate groups to retest");

#ifndef SFCP_RESULT_PATH_DEFAULT
#define SFCP_RESULT_PATH_DEFAULT "/tmp/rk3588_sf_crossprobe.csv"
#endif
static char *result_path = SFCP_RESULT_PATH_DEFAULT;
module_param(result_path, charp, 0444);
MODULE_PARM_DESC(result_path, "Absolute CSV path; parent directory must exist");

enum sfcp_counter_kind {
	SFCP_COUNTER_PMCCNTR,
	SFCP_COUNTER_PMEVT0,
	SFCP_COUNTER_CNTVCT,
};

struct sfcp_line {
	struct page *page;
	u32 offset;
};

struct sfcp_pmu_delta {
	u32 l1d_refill;
	u32 l1d_tlb_refill;
	u32 l2d_refill;
	u32 bus_access;
	u32 ll_cache_rd;
	u32 ll_cache_miss_rd;
};

struct sfcp_sample {
	u64 same_first_ticks;
	u64 same_prefill_ticks;
	u64 same_wait_prefill_ticks;
	u64 same_cpu_ticks;
	u64 cross_first_ticks;
	u64 cross_prefill_ticks;
	u64 cross_wait_prefill_ticks;
	u64 cross_idle_ticks;
	u64 cross_cpu_ticks;
	struct sfcp_pmu_delta same_pmu;
	struct sfcp_pmu_delta cross_idle_pmu;
	struct sfcp_pmu_delta cross_pmu;
	phys_addr_t pa[];
};

struct sfcp_cross_retest {
	u32 source_candidate_index;
	u32 repetition;
	u64 first_ticks;
	u64 prefill_ticks;
	u64 wait_prefill_ticks;
	u64 probe_ticks;
	struct sfcp_pmu_delta pmu;
};

struct sfcp_cross_retest_state {
	struct sfcp_cross_retest *sample;
	unsigned int capacity;
	unsigned int sample_count;
	unsigned int group_count;
};

struct sfcp_handshake {
	unsigned int prepare_request;
	unsigned int access_go;
	unsigned int active_items;
} __aligned(PAGE_SIZE);

static struct page **sfcp_pages;
static struct sfcp_line *sfcp_candidate_pool;
static struct sfcp_line *sfcp_unrelated_pool;
static unsigned int sfcp_allocated_pages;
static unsigned int sfcp_candidate_count;
static unsigned int sfcp_unrelated_count;
static unsigned int sfcp_candidate_total;
static unsigned int sfcp_unrelated_total;
static u64 sfcp_active_addrmask;

static struct task_struct *sfcp_probe_task;
static struct task_struct **sfcp_stimulus_tasks;
static unsigned int *sfcp_prepare_done;
static unsigned int *sfcp_access_done;
static cpumask_t sfcp_stimulus_mask;
static unsigned int sfcp_stimulus_count;
static DECLARE_COMPLETION(sfcp_probe_done);
static DECLARE_WAIT_QUEUE_HEAD(sfcp_prepare_waitq);
static struct sfcp_line *sfcp_shared_evictors;
static size_t sfcp_sample_size;
static struct sfcp_handshake sfcp_sync;
static bool sfcp_shutdown;
static int sfcp_probe_result;

/* Only the probe_cpu thread touches the PMU bookkeeping below. */
static enum sfcp_counter_kind sfcp_counter_kind = SFCP_COUNTER_CNTVCT;
static u64 sfcp_saved_pmcr;
static u64 sfcp_saved_pmccfiltr;
static u64 sfcp_saved_pmevtyper0;
static u64 sfcp_saved_pmevcntr0;
static u64 sfcp_saved_pmevtyper1;
static u64 sfcp_saved_pmevtyper2;
static u64 sfcp_saved_pmevtyper3;
static u64 sfcp_saved_pmevtyper4;
static u64 sfcp_saved_pmevtyper5;
static u64 sfcp_saved_pmevcntr1;
static u64 sfcp_saved_pmevcntr2;
static u64 sfcp_saved_pmevcntr3;
static u64 sfcp_saved_pmevcntr4;
static u64 sfcp_saved_pmevcntr5;
static bool sfcp_saved_cycle_enabled;
static bool sfcp_saved_event0_enabled;
static u64 sfcp_saved_probe_events_enabled;
static bool sfcp_event0_changed;
static bool sfcp_probe_events_available;
static bool sfcp_ll_events_available;
static bool sfcp_l1d_tlb_event_available;
static bool sfcp_probe_events_changed;
static u64 sfcp_probe_event_mask;
static bool sfcp_pmu_changed;
static u64 sfcp_counter_calibration_delta;

static __always_inline unsigned int sfcp_group_lines(void)
{
	return access_items + 1U;
}

static __always_inline u64 sfcp_legacy_match_mask(void)
{
	if (match_bits == 32U)
		return U32_MAX;
	return (1ULL << match_bits) - 1ULL;
}

static __always_inline struct sfcp_sample *sfcp_sample_at(void *samples,
							  unsigned int index)
{
	return (struct sfcp_sample *)((char *)samples +
				     (size_t)index * sfcp_sample_size);
}

static __always_inline void *sfcp_address(const struct sfcp_line *line)
{
	return (char *)page_address(line->page) + line->offset;
}

static __always_inline phys_addr_t sfcp_physical(const struct sfcp_line *line)
{
	return page_to_phys(line->page) + line->offset;
}

static __always_inline void sfcp_touch(const void *address)
{
	u64 value;

	/* Controlled bit scans may select any byte within a cache line. */
	value = get_unaligned((const u64 *)address);
	asm volatile("" : : "r" (value) : "memory");
}

static __always_inline void sfcp_civac(const void *address)
{
	asm volatile("dc civac, %0" : : "r" (address) : "memory");
}

static __always_inline u64 sfcp_read_counter(void)
{
	u64 value;

	isb();
	if (likely(sfcp_counter_kind == SFCP_COUNTER_PMCCNTR))
		value = read_sysreg(pmccntr_el0);
	else if (sfcp_counter_kind == SFCP_COUNTER_PMEVT0)
		value = read_sysreg(pmevcntr0_el0);
	else
		value = read_sysreg(cntvct_el0);
	isb();
	return value;
}

static __always_inline u64 sfcp_counter_delta(u64 before, u64 after)
{
	if ((sfcp_counter_kind == SFCP_COUNTER_PMCCNTR &&
	     !(read_sysreg(pmcr_el0) & BIT(6))) ||
	    sfcp_counter_kind == SFCP_COUNTER_PMEVT0)
		return (u32)after - (u32)before;
	return after - before;
}

static const char *sfcp_counter_name(void)
{
	switch (sfcp_counter_kind) {
	case SFCP_COUNTER_PMCCNTR:
		return "pmccntr_cycles";
	case SFCP_COUNTER_PMEVT0:
		return "pmevcntr0_cpu_cycles";
	default:
		return "cntvct_ticks";
	}
}

static __always_inline u32 sfcp_random_below(u32 upper)
{
	if (upper <= 1)
		return 0;
	return reciprocal_scale(get_random_u32(), upper);
}

/*
 * Keep a uniform sample of at most pool_pages lines from each class.  A full
 * 1.5 GiB pool contains about 25 million cache lines, so storing every
 * non-matching line would otherwise consume hundreds of additional MiB.
 */
static void sfcp_reservoir_add(struct sfcp_line *pool,
			       unsigned int *stored,
			       unsigned int *total,
			       const struct sfcp_line *line)
{
	u32 slot;

	++*total;
	if (*stored < pool_pages) {
		pool[(*stored)++] = *line;
		return;
	}

	slot = sfcp_random_below(*total);
	if (slot < pool_pages)
		pool[slot] = *line;
}

static u64 sfcp_calibrate_counter(void)
{
	void *address = (char *)page_address(sfcp_pages[0]) + line_offset;
	u64 before;
	u64 after;

	sfcp_civac(address);
	dsb(ish);
	isb();
	before = sfcp_read_counter();
	sfcp_touch(address);
	dsb(ish);
	after = sfcp_read_counter();
	return sfcp_counter_delta(before, after);
}

static void sfcp_enable_probe_events(u64 enabled)
{
	unsigned int event_counters =
		(unsigned int)((sfcp_saved_pmcr >> 11) & 0x1fU);
	u64 implemented_events0 = read_sysreg(pmceid0_el0);
	u64 implemented_events1 = read_sysreg(pmceid1_el0);

	/* Counters 1, 2, and 3 are required in addition to event counter 0. */
	if (event_counters < 4U)
		return;
	if (!(implemented_events0 & BIT(SFCP_PMU_EVENT_L1D_REFILL)) ||
	    !(implemented_events0 & BIT(SFCP_PMU_EVENT_L2D_REFILL)) ||
	    !(implemented_events0 & BIT(SFCP_PMU_EVENT_BUS_ACCESS)))
		return;
	sfcp_probe_event_mask = SFCP_PMU_BASE_EVENT_MASK;
	if (implemented_events0 & BIT(SFCP_PMU_EVENT_L1D_TLB_REFILL)) {
		/*
		 * PMCCNTR normally supplies cycles, leaving programmable counter 0
		 * available for the sixth probe event.  Save it here; if PMCCNTR is
		 * unusable, sfcp_enable_counter() reuses counter 0 for cycles and
		 * disables this optional TLB event for the current module load.
		 */
		sfcp_saved_pmevtyper0 = read_sysreg(pmevtyper0_el0);
		sfcp_saved_pmevcntr0 = read_sysreg(pmevcntr0_el0);
		sfcp_saved_event0_enabled = !!(enabled & BIT(0));
		sfcp_event0_changed = true;
		sfcp_l1d_tlb_event_available = true;
		sfcp_probe_event_mask |= SFCP_PMU_TLB_EVENT_MASK;
	}
	if (event_counters >= 6U &&
	    (implemented_events1 & BIT(SFCP_PMU_EVENT_LL_CACHE_RD - 32U)) &&
	    (implemented_events1 &
	     BIT(SFCP_PMU_EVENT_LL_CACHE_MISS_RD - 32U))) {
		sfcp_ll_events_available = true;
		sfcp_probe_event_mask |= SFCP_PMU_LL_EVENT_MASK;
	}

	sfcp_saved_pmevtyper1 = read_sysreg(pmevtyper1_el0);
	sfcp_saved_pmevtyper2 = read_sysreg(pmevtyper2_el0);
	sfcp_saved_pmevtyper3 = read_sysreg(pmevtyper3_el0);
	sfcp_saved_pmevcntr1 = read_sysreg(pmevcntr1_el0);
	sfcp_saved_pmevcntr2 = read_sysreg(pmevcntr2_el0);
	sfcp_saved_pmevcntr3 = read_sysreg(pmevcntr3_el0);
	if (sfcp_ll_events_available) {
		sfcp_saved_pmevtyper4 = read_sysreg(pmevtyper4_el0);
		sfcp_saved_pmevtyper5 = read_sysreg(pmevtyper5_el0);
		sfcp_saved_pmevcntr4 = read_sysreg(pmevcntr4_el0);
		sfcp_saved_pmevcntr5 = read_sysreg(pmevcntr5_el0);
	}
	sfcp_saved_probe_events_enabled =
		enabled & sfcp_probe_event_mask;

	write_sysreg(sfcp_probe_event_mask, pmcntenclr_el0);
	if (sfcp_l1d_tlb_event_available) {
		write_sysreg(SFCP_PMU_EVENT_L1D_TLB_REFILL,
			     pmevtyper0_el0);
		write_sysreg(0, pmevcntr0_el0);
	}
	write_sysreg(SFCP_PMU_EVENT_L1D_REFILL, pmevtyper1_el0);
	write_sysreg(SFCP_PMU_EVENT_L2D_REFILL, pmevtyper2_el0);
	write_sysreg(SFCP_PMU_EVENT_BUS_ACCESS, pmevtyper3_el0);
	write_sysreg(0, pmevcntr1_el0);
	write_sysreg(0, pmevcntr2_el0);
	write_sysreg(0, pmevcntr3_el0);
	if (sfcp_ll_events_available) {
		write_sysreg(SFCP_PMU_EVENT_LL_CACHE_RD, pmevtyper4_el0);
		write_sysreg(SFCP_PMU_EVENT_LL_CACHE_MISS_RD, pmevtyper5_el0);
		write_sysreg(0, pmevcntr4_el0);
		write_sysreg(0, pmevcntr5_el0);
	}
	write_sysreg(sfcp_probe_event_mask, pmcntenset_el0);
	isb();

	sfcp_probe_events_available = true;
	sfcp_probe_events_changed = true;
	sfcp_pmu_changed = true;
}

static void sfcp_enable_counter(void)
{
	u64 current_pmcr;
	u64 target_pmcr;
	u64 target_filter;
	u64 enabled;
	u64 before;
	u64 after;
	unsigned int i;

	sfcp_saved_pmcr = read_sysreg(pmcr_el0);
	sfcp_saved_pmccfiltr = read_sysreg(pmccfiltr_el0);
	enabled = read_sysreg(pmcntenset_el0);
	sfcp_saved_cycle_enabled = !!(enabled & BIT(31));
	sfcp_enable_probe_events(enabled);

	target_pmcr = (sfcp_saved_pmcr | BIT(0)) & ~BIT(3);
	current_pmcr = read_sysreg(pmcr_el0);
	if (current_pmcr != target_pmcr) {
		write_sysreg(target_pmcr, pmcr_el0);
		sfcp_pmu_changed = true;
	}
	if (!(enabled & BIT(31))) {
		write_sysreg(BIT(31), pmcntenset_el0);
		sfcp_pmu_changed = true;
	}
	target_filter = sfcp_saved_pmccfiltr & ~BIT(31);
	if (target_filter != sfcp_saved_pmccfiltr) {
		write_sysreg(target_filter, pmccfiltr_el0);
		sfcp_pmu_changed = true;
	}
	isb();

	before = read_sysreg(pmccntr_el0);
	for (i = 0; i < 256; ++i)
		cpu_relax();
	after = read_sysreg(pmccntr_el0);
	current_pmcr = read_sysreg(pmcr_el0);
	enabled = read_sysreg(pmcntenset_el0);
	if ((current_pmcr & BIT(0)) && !(current_pmcr & BIT(3)) &&
	    (enabled & BIT(31)) && after != before) {
		sfcp_counter_kind = SFCP_COUNTER_PMCCNTR;
		sfcp_counter_calibration_delta = sfcp_calibrate_counter();
		if (sfcp_counter_calibration_delta)
			return;
	}

	/*
	 * Some firmware leaves PMCCNTR divided or filtered despite PMCR/PMCCFILTR
	 * writes.  Programmable event 0 with architected event 0x11 provides an
	 * undivided CPU-cycle counter for the same EL1 measurement.
	 */
	if (!sfcp_event0_changed) {
		sfcp_saved_pmevtyper0 = read_sysreg(pmevtyper0_el0);
		sfcp_saved_pmevcntr0 = read_sysreg(pmevcntr0_el0);
		sfcp_saved_event0_enabled = !!(enabled & BIT(0));
		sfcp_event0_changed = true;
	}
	sfcp_l1d_tlb_event_available = false;
	sfcp_probe_event_mask &= ~SFCP_PMU_TLB_EVENT_MASK;
	write_sysreg(BIT(0), pmcntenclr_el0);
	write_sysreg(0x11, pmevtyper0_el0);
	write_sysreg(0, pmevcntr0_el0);
	write_sysreg(BIT(0), pmcntenset_el0);
	isb();
	sfcp_pmu_changed = true;
	sfcp_counter_kind = SFCP_COUNTER_PMEVT0;
	sfcp_counter_calibration_delta = sfcp_calibrate_counter();
	if (sfcp_counter_calibration_delta)
		return;

	sfcp_counter_kind = SFCP_COUNTER_CNTVCT;
	sfcp_counter_calibration_delta = sfcp_calibrate_counter();
}

static void sfcp_restore_counter(void)
{
	if (!sfcp_pmu_changed)
		return;
	if (sfcp_probe_events_changed) {
		write_sysreg(sfcp_probe_event_mask, pmcntenclr_el0);
		write_sysreg(sfcp_saved_pmevtyper1, pmevtyper1_el0);
		write_sysreg(sfcp_saved_pmevtyper2, pmevtyper2_el0);
		write_sysreg(sfcp_saved_pmevtyper3, pmevtyper3_el0);
		write_sysreg(sfcp_saved_pmevcntr1, pmevcntr1_el0);
		write_sysreg(sfcp_saved_pmevcntr2, pmevcntr2_el0);
		write_sysreg(sfcp_saved_pmevcntr3, pmevcntr3_el0);
		if (sfcp_ll_events_available) {
			write_sysreg(sfcp_saved_pmevtyper4, pmevtyper4_el0);
			write_sysreg(sfcp_saved_pmevtyper5, pmevtyper5_el0);
			write_sysreg(sfcp_saved_pmevcntr4, pmevcntr4_el0);
			write_sysreg(sfcp_saved_pmevcntr5, pmevcntr5_el0);
		}
		if (sfcp_saved_probe_events_enabled)
			write_sysreg(sfcp_saved_probe_events_enabled,
				     pmcntenset_el0);
		sfcp_probe_events_changed = false;
		sfcp_probe_events_available = false;
		sfcp_ll_events_available = false;
		sfcp_l1d_tlb_event_available = false;
		sfcp_probe_event_mask = 0;
	}
	if (sfcp_event0_changed) {
		write_sysreg(BIT(0), pmcntenclr_el0);
		write_sysreg(sfcp_saved_pmevtyper0, pmevtyper0_el0);
		write_sysreg(sfcp_saved_pmevcntr0, pmevcntr0_el0);
		if (sfcp_saved_event0_enabled)
			write_sysreg(BIT(0), pmcntenset_el0);
		sfcp_event0_changed = false;
	}
	if (!sfcp_saved_cycle_enabled)
		write_sysreg(BIT(31), pmcntenclr_el0);
	write_sysreg(sfcp_saved_pmccfiltr, pmccfiltr_el0);
	write_sysreg(sfcp_saved_pmcr, pmcr_el0);
	isb();
	sfcp_pmu_changed = false;
}

static __always_inline void sfcp_rearm_counter(void)
{
	u64 pmcr;

	if (sfcp_counter_kind == SFCP_COUNTER_CNTVCT)
		return;

	pmcr = (read_sysreg(pmcr_el0) | BIT(0)) & ~BIT(3);
	write_sysreg(pmcr, pmcr_el0);
	if (sfcp_counter_kind == SFCP_COUNTER_PMCCNTR) {
		write_sysreg(read_sysreg(pmccfiltr_el0) & ~BIT(31),
			     pmccfiltr_el0);
		write_sysreg(BIT(31), pmcntenset_el0);
	} else {
		write_sysreg(0x11, pmevtyper0_el0);
		write_sysreg(BIT(0), pmcntenset_el0);
	}
	isb();
}

static __always_inline void sfcp_reset_probe_events(void)
{
	if (!sfcp_probe_events_available)
		return;

	write_sysreg(sfcp_probe_event_mask, pmcntenclr_el0);
	if (sfcp_l1d_tlb_event_available) {
		write_sysreg(SFCP_PMU_EVENT_L1D_TLB_REFILL,
			     pmevtyper0_el0);
		write_sysreg(0, pmevcntr0_el0);
	}
	write_sysreg(SFCP_PMU_EVENT_L1D_REFILL, pmevtyper1_el0);
	write_sysreg(SFCP_PMU_EVENT_L2D_REFILL, pmevtyper2_el0);
	write_sysreg(SFCP_PMU_EVENT_BUS_ACCESS, pmevtyper3_el0);
	write_sysreg(0, pmevcntr1_el0);
	write_sysreg(0, pmevcntr2_el0);
	write_sysreg(0, pmevcntr3_el0);
	if (sfcp_ll_events_available) {
		write_sysreg(SFCP_PMU_EVENT_LL_CACHE_RD, pmevtyper4_el0);
		write_sysreg(SFCP_PMU_EVENT_LL_CACHE_MISS_RD, pmevtyper5_el0);
		write_sysreg(0, pmevcntr4_el0);
		write_sysreg(0, pmevcntr5_el0);
	}
	write_sysreg(sfcp_probe_event_mask, pmcntenset_el0);
	isb();
}

static __always_inline void
sfcp_read_probe_events(struct sfcp_pmu_delta *delta)
{
	if (!sfcp_probe_events_available) {
		delta->l1d_refill = 0;
		delta->l1d_tlb_refill = 0;
		delta->l2d_refill = 0;
		delta->bus_access = 0;
		delta->ll_cache_rd = 0;
		delta->ll_cache_miss_rd = 0;
		return;
	}

	isb();
	delta->l1d_refill = (u32)read_sysreg(pmevcntr1_el0);
	delta->l1d_tlb_refill = sfcp_l1d_tlb_event_available ?
		(u32)read_sysreg(pmevcntr0_el0) : 0;
	delta->l2d_refill = (u32)read_sysreg(pmevcntr2_el0);
	delta->bus_access = (u32)read_sysreg(pmevcntr3_el0);
	if (sfcp_ll_events_available) {
		delta->ll_cache_rd = (u32)read_sysreg(pmevcntr4_el0);
		delta->ll_cache_miss_rd = (u32)read_sysreg(pmevcntr5_el0);
	} else {
		delta->ll_cache_rd = 0;
		delta->ll_cache_miss_rd = 0;
	}
	isb();
}

static __always_inline void sfcp_wait_after_prefill(void)
{
	u64 frequency;
	u64 wait_ticks;
	u64 start;

	if (!prefill_wait_us)
		return;

	frequency = read_sysreg(cntfrq_el0);
	wait_ticks = div_u64((u64)prefill_wait_us * frequency + 999999ULL,
			     1000000ULL);
	isb();
	start = read_sysreg(cntvct_el0);
	while (read_sysreg(cntvct_el0) - start < wait_ticks)
		cpu_relax();
	isb();
}

static bool sfcp_pick_stimulus(const struct sfcp_line *pool,
			       unsigned int pool_size,
			       const struct sfcp_line *probe, u32 *idx)
{
	unsigned int i;

	if (pool_size < access_items)
		return false;

	for (i = 0; i < access_items; ++i) {
		unsigned int retry;

		for (retry = 0; retry < 4096; ++retry) {
			u32 candidate = sfcp_random_below(pool_size);
			unsigned int j;
			bool reject = false;

			if (pool[candidate].page == probe->page &&
			    pool[candidate].offset == probe->offset)
				continue;
			for (j = 0; j < i; ++j) {
				if (idx[j] == candidate) {
					reject = true;
					break;
				}
			}
			if (!reject) {
				idx[i] = candidate;
				break;
			}
		}
		if (retry == 4096)
			return false;
	}
	return true;
}

static void sfcp_publish_evictors(const struct sfcp_line *group)
{
	unsigned int i;

	for (i = 0; i < access_items; ++i) {
		WRITE_ONCE(sfcp_shared_evictors[i].page, group[i].page);
		WRITE_ONCE(sfcp_shared_evictors[i].offset, group[i].offset);
	}
	/* The release store to prepare_request publishes these descriptors. */
}

static int sfcp_stimulus_thread(void *unused)
{
	unsigned int cpu = (unsigned long)unused;
	unsigned int i;
	unsigned int round;
	unsigned int active_items;
	unsigned int sequence = 0;

	(void)unused;
	for (;;) {
		wait_event_interruptible(sfcp_prepare_waitq,
			smp_load_acquire(&sfcp_sync.prepare_request) != sequence ||
			READ_ONCE(sfcp_shutdown) || kthread_should_stop());
		if (READ_ONCE(sfcp_shutdown) || kthread_should_stop())
			break;
		sequence = READ_ONCE(sfcp_sync.prepare_request);

		/*
		 * Acknowledge that this already-running thread has observed the
		 * newly published descriptors.  Cache maintenance is performed
		 * only by probe_cpu before it primes the probe.
		 */
		smp_store_release(&sfcp_prepare_done[cpu], sequence);

		/*
		 * Every stimulus thread is already runnable before probe_cpu
		 * primes.  Spin on a cache-line flag so probe_cpu does not execute
		 * scheduler wake-up paths after priming.
		 */
		while (smp_load_acquire(&sfcp_sync.access_go) != sequence &&
		       !READ_ONCE(sfcp_shutdown))
			cpu_relax();
		if (READ_ONCE(sfcp_shutdown) || kthread_should_stop())
			break;

		active_items = READ_ONCE(sfcp_sync.active_items);
		for (round = 0; active_items && round < fill_rounds; ++round) {
			for (i = 0; i < active_items; ++i) {
				struct sfcp_line line;

				line.page =
					READ_ONCE(sfcp_shared_evictors[i].page);
				line.offset =
					READ_ONCE(sfcp_shared_evictors[i].offset);
				sfcp_touch(sfcp_address(&line));
			}
			dsb(ish);
		}
		smp_store_release(&sfcp_access_done[cpu], sequence);
	}
	return 0;
}

static __always_inline u64 sfcp_timed_touch(const void *address)
{
	u64 before;
	u64 after;

	before = sfcp_read_counter();
	sfcp_touch(address);
	dsb(ish);
	after = sfcp_read_counter();
	return sfcp_counter_delta(before, after);
}

static __always_inline u64
sfcp_timed_touch_with_pmu(const void *address, struct sfcp_pmu_delta *delta)
{
	u64 before;
	u64 after;

	sfcp_reset_probe_events();
	before = sfcp_read_counter();
	sfcp_touch(address);
	dsb(ish);
	after = sfcp_read_counter();
	sfcp_read_probe_events(delta);
	return sfcp_counter_delta(before, after);
}

static u64 sfcp_measure_cross_probe(const struct sfcp_line *group,
				    unsigned int stimulus_items,
				    u64 *first_ticks, u64 *prefill_ticks,
				    u64 *wait_prefill_ticks,
				    struct sfcp_pmu_delta *reload_pmu)
{
	void *probe = sfcp_address(&group[access_items]);
	unsigned long irq_flags;
	u64 reload_ticks;
	unsigned int sequence;
	unsigned int cpu;
	unsigned int i;

	sfcp_publish_evictors(group);
	WRITE_ONCE(sfcp_sync.active_items, stimulus_items);
	sequence = READ_ONCE(sfcp_sync.prepare_request) + 1U;
	smp_store_release(&sfcp_sync.prepare_request, sequence);
	wake_up_all(&sfcp_prepare_waitq);
	for_each_cpu(cpu, &sfcp_stimulus_mask) {
		while (smp_load_acquire(&sfcp_prepare_done[cpu]) != sequence &&
		       !READ_ONCE(sfcp_shutdown))
			cpu_relax();
	}
	if (unlikely(READ_ONCE(sfcp_shutdown)))
		return 0;

	sfcp_rearm_counter();

	/*
	 * probe_cpu performs all cache maintenance, primes the probe, releases
	 * the stimulus CPUs to perform loads only, and then reloads the probe.
	 * Keep the complete clean/prime/fill/reload sequence unscheduled.
	 */
	preempt_disable();
	local_irq_save(irq_flags);
#ifdef SFCP_ABLATION_BUILD
	/* Drop-mode ablation must not touch descriptors removed from the set. */
	for (i = 0; i < stimulus_items; ++i)
		sfcp_civac(sfcp_address(&group[i]));
	sfcp_civac(probe);
#else
	for (i = 0; i < sfcp_group_lines(); ++i)
		sfcp_civac(sfcp_address(&group[i]));
#endif
	dsb(ish);
	isb();

	*first_ticks = sfcp_timed_touch(probe);
	*prefill_ticks = sfcp_timed_touch(probe);
	sfcp_wait_after_prefill();
	*wait_prefill_ticks = sfcp_timed_touch(probe);

	smp_store_release(&sfcp_sync.access_go, sequence);
	for_each_cpu(cpu, &sfcp_stimulus_mask) {
		while (smp_load_acquire(&sfcp_access_done[cpu]) != sequence)
			cpu_relax();
	}

	/*
	 * The access_done loads observe lines just written by the stimulus
	 * CPUs. Drain those coherence transactions outside the timed interval.
	 */
	dsb(ish);
	isb();
	reload_ticks = sfcp_timed_touch_with_pmu(probe, reload_pmu);
	local_irq_restore(irq_flags);
	preempt_enable();
	return reload_ticks;
}

static u64 sfcp_measure_same_probe_n(const struct sfcp_line *group,
				     unsigned int stimulus_items,
				     u64 *first_ticks, u64 *prefill_ticks,
				     u64 *wait_prefill_ticks,
				     struct sfcp_pmu_delta *reload_pmu)
{
	void *probe = sfcp_address(&group[access_items]);
	unsigned long irq_flags;
	unsigned int round;
	unsigned int i;
	u64 reload_ticks;

	sfcp_rearm_counter();

	preempt_disable();
	local_irq_save(irq_flags);
	for (i = 0; i < stimulus_items; ++i)
		sfcp_civac(sfcp_address(&group[i]));
	sfcp_civac(probe);
	dsb(ish);
	isb();

	*first_ticks = sfcp_timed_touch(probe);
	*prefill_ticks = sfcp_timed_touch(probe);
	sfcp_wait_after_prefill();
	*wait_prefill_ticks = sfcp_timed_touch(probe);

	for (round = 0; round < fill_rounds; ++round) {
		for (i = 0; i < stimulus_items; ++i)
			sfcp_touch(sfcp_address(&group[i]));
		dsb(ish);
	}

	reload_ticks = sfcp_timed_touch_with_pmu(probe, reload_pmu);
	local_irq_restore(irq_flags);
	preempt_enable();
	return reload_ticks;
}

static u64 sfcp_measure_same_probe(const struct sfcp_line *group,
				   u64 *first_ticks, u64 *prefill_ticks,
				   u64 *wait_prefill_ticks,
				   struct sfcp_pmu_delta *reload_pmu)
{
	return sfcp_measure_same_probe_n(group, access_items, first_ticks,
					 prefill_ticks, wait_prefill_ticks,
					 reload_pmu);
}

static int sfcp_take_sample(const struct sfcp_line *pool,
			    unsigned int pool_size,
			    const struct sfcp_line *probe,
			    struct sfcp_sample *sample, u32 *idx,
			    struct sfcp_line *group, bool cross_first)
{
	unsigned int group_lines = sfcp_group_lines();
	unsigned int i;
	u64 idle_first_ticks;
	u64 idle_prefill_ticks;
	u64 idle_wait_prefill_ticks;

	if (!sfcp_pick_stimulus(pool, pool_size, probe, idx))
		return -ERANGE;
	for (i = 0; i < access_items; ++i)
		group[i] = pool[idx[i]];
	group[access_items] = *probe;
	for (i = 0; i < group_lines; ++i)
		sample->pa[i] = sfcp_physical(&group[i]);
	if (cross_first) {
		sample->cross_cpu_ticks = sfcp_measure_cross_probe(
			group, access_items, &sample->cross_first_ticks,
			&sample->cross_prefill_ticks,
			&sample->cross_wait_prefill_ticks,
			&sample->cross_pmu);
		sample->cross_idle_ticks = sfcp_measure_cross_probe(
			group, 0, &idle_first_ticks, &idle_prefill_ticks,
			&idle_wait_prefill_ticks,
			&sample->cross_idle_pmu);
		sample->same_cpu_ticks = sfcp_measure_same_probe(
			group, &sample->same_first_ticks,
			&sample->same_prefill_ticks,
			&sample->same_wait_prefill_ticks,
			&sample->same_pmu);
	} else {
		sample->same_cpu_ticks = sfcp_measure_same_probe(
			group, &sample->same_first_ticks,
			&sample->same_prefill_ticks,
			&sample->same_wait_prefill_ticks,
			&sample->same_pmu);
		sample->cross_idle_ticks = sfcp_measure_cross_probe(
			group, 0, &idle_first_ticks, &idle_prefill_ticks,
			&idle_wait_prefill_ticks,
			&sample->cross_idle_pmu);
		sample->cross_cpu_ticks = sfcp_measure_cross_probe(
			group, access_items, &sample->cross_first_ticks,
			&sample->cross_prefill_ticks,
			&sample->cross_wait_prefill_ticks,
			&sample->cross_pmu);
	}
	return 0;
}

static int
sfcp_retest_candidate_group(unsigned int source_index,
			    const struct sfcp_sample *initial,
			    const struct sfcp_line *group,
			    struct sfcp_cross_retest_state *state)
{
	u64 sum = 0;
	u64 minimum = U64_MAX;
	u64 maximum = 0;
	unsigned int successes = 0;
	unsigned int repetition;

	if (!cross_retest_repetitions ||
	    initial->cross_cpu_ticks <= cross_retest_threshold ||
	    state->group_count >= cross_retest_max_groups)
		return 0;
	if (state->sample_count + cross_retest_repetitions > state->capacity)
		return -ENOSPC;

	++state->group_count;
	for (repetition = 0; repetition < cross_retest_repetitions;
	     ++repetition) {
		struct sfcp_cross_retest *result;

		if (unlikely(kthread_should_stop()))
			return -EINTR;
		result = &state->sample[state->sample_count++];
		result->source_candidate_index = source_index;
		result->repetition = repetition;
		result->probe_ticks = sfcp_measure_cross_probe(
			group, access_items, &result->first_ticks,
			&result->prefill_ticks, &result->wait_prefill_ticks,
			&result->pmu);
		sum += result->probe_ticks;
		minimum = min(minimum, result->probe_ticks);
		maximum = max(maximum, result->probe_ticks);
		if (result->probe_ticks > cross_retest_threshold)
			++successes;
	}

	pr_info(SFCP_NAME ": cross retest candidate=%u initial=%llu threshold=%u repeat_success=%u/%u mean=%llu min=%llu max=%llu\n",
		source_index, (unsigned long long)initial->cross_cpu_ticks,
		cross_retest_threshold, successes, cross_retest_repetitions,
		(unsigned long long)div_u64(sum, cross_retest_repetitions),
		(unsigned long long)minimum, (unsigned long long)maximum);
	return 0;
}

static int sfcp_collect_interleaved(void *baseline, void *candidate,
				    u32 *idx, struct sfcp_line *group,
				    struct sfcp_cross_retest_state *retest)
{
	unsigned int count = max(baseline_sets, candidate_sets);
	unsigned int sequence = 0;
	unsigned int i;
	struct sfcp_sample *sample;
	int ret;

	for (i = 0; i < count; ++i) {
		const struct sfcp_line *probe;

		if (unlikely(kthread_should_stop()))
			return -EINTR;

		probe = &sfcp_candidate_pool[
			sfcp_random_below(sfcp_candidate_count)];

		/* Alternate immediate ordering to balance slow system drift. */
		if (i & 1U) {
			if (i < candidate_sets) {
				sample = sfcp_sample_at(candidate, i);
				ret = sfcp_take_sample(sfcp_candidate_pool,
					sfcp_candidate_count, probe,
					sample, idx, group,
					!!(sequence++ & 1U));
				if (ret)
					return ret;
				ret = sfcp_retest_candidate_group(i, sample, group,
							  retest);
				if (ret)
					return ret;
			}
			if (i < baseline_sets) {
				ret = sfcp_take_sample(sfcp_unrelated_pool,
					sfcp_unrelated_count, probe,
					sfcp_sample_at(baseline, i), idx, group,
					!!(sequence++ & 1U));
				if (ret)
					return ret;
			}
		} else {
			if (i < baseline_sets) {
				ret = sfcp_take_sample(sfcp_unrelated_pool,
					sfcp_unrelated_count, probe,
					sfcp_sample_at(baseline, i), idx, group,
					!!(sequence++ & 1U));
				if (ret)
					return ret;
			}
			if (i < candidate_sets) {
				sample = sfcp_sample_at(candidate, i);
				ret = sfcp_take_sample(sfcp_candidate_pool,
					sfcp_candidate_count, probe,
					sample, idx, group,
					!!(sequence++ & 1U));
				if (ret)
					return ret;
				ret = sfcp_retest_candidate_group(i, sample, group,
							  retest);
				if (ret)
					return ret;
			}
		}

		if (!(i & 0x3ffU))
			cond_resched();
	}
	return 0;
}

static int sfcp_write_all(struct file *file, const char *buffer, size_t count,
			  loff_t *position)
{
	while (count) {
		ssize_t written = kernel_write(file, buffer, count, position);

		if (written < 0)
			return (int)written;
		if (!written)
			return -EIO;
		buffer += written;
		count -= written;
	}
	return 0;
}

static int sfcp_write_sample(struct file *file, const char *kind,
			     unsigned int index,
			     const struct sfcp_sample *sample,
			     u64 probe_ticks,
			     loff_t *position, char *row, size_t row_size)
{
	static const struct sfcp_pmu_delta no_events;
	const struct sfcp_pmu_delta *events = &no_events;
	size_t used;
	unsigned int i;

	if (!strcmp(kind, "same_baseline") ||
	    !strcmp(kind, "same_candidate"))
		events = &sample->same_pmu;
	else if (!strcmp(kind, "cross_idle_baseline") ||
		 !strcmp(kind, "cross_idle_candidate"))
		events = &sample->cross_idle_pmu;
	else if (!strcmp(kind, "cross_baseline") ||
		 !strcmp(kind, "cross_candidate"))
		events = &sample->cross_pmu;

	used = scnprintf(row, row_size,
			 "%s,%u,%llu,%u,%u,%u,%u,%u,%u",
			 kind, index, (unsigned long long)probe_ticks,
			 events->l1d_refill, events->l1d_tlb_refill,
			 events->l2d_refill,
			 events->bus_access, events->ll_cache_rd,
			 events->ll_cache_miss_rd);
	for (i = 0; i < sfcp_group_lines(); ++i)
		used += scnprintf(row + used, row_size - used, ",0x%llx",
				 (unsigned long long)sample->pa[i]);
	used += scnprintf(row + used, row_size - used, ",-1,-1\n");
	if (used >= row_size - 1U)
		return -EOVERFLOW;
	return sfcp_write_all(file, row, used, position);
}

static int sfcp_write_retest(struct file *file, unsigned int index,
			     const struct sfcp_cross_retest *retest,
			     const struct sfcp_sample *source,
			     loff_t *position, char *row, size_t row_size)
{
	size_t used;
	unsigned int i;

	used = scnprintf(row, row_size,
			 "cross_candidate_retest,%u,%llu,%u,%u,%u,%u,%u,%u",
			 index, (unsigned long long)retest->probe_ticks,
			 retest->pmu.l1d_refill, retest->pmu.l1d_tlb_refill,
			 retest->pmu.l2d_refill,
			 retest->pmu.bus_access, retest->pmu.ll_cache_rd,
			 retest->pmu.ll_cache_miss_rd);
	for (i = 0; i < sfcp_group_lines(); ++i)
		used += scnprintf(row + used, row_size - used, ",0x%llx",
				 (unsigned long long)source->pa[i]);
	used += scnprintf(row + used, row_size - used, ",%u,%u\n",
			  retest->source_candidate_index, retest->repetition);
	if (used >= row_size - 1U)
		return -EOVERFLOW;
	return sfcp_write_all(file, row, used, position);
}

static int sfcp_write_csv(void *baseline, void *candidate,
			  const struct sfcp_cross_retest_state *retest,
			  u64 same_first_baseline_mean,
			  u64 same_first_candidate_mean,
			  u64 cross_first_baseline_mean,
			  u64 cross_first_candidate_mean,
			  u64 same_prefill_baseline_mean,
			  u64 same_prefill_candidate_mean,
			  u64 cross_prefill_baseline_mean,
			  u64 cross_prefill_candidate_mean,
			  u64 same_wait_prefill_baseline_mean,
			  u64 same_wait_prefill_candidate_mean,
			  u64 cross_wait_prefill_baseline_mean,
			  u64 cross_wait_prefill_candidate_mean,
			  u64 cross_idle_baseline_mean,
			  u64 cross_idle_candidate_mean,
			  u64 same_baseline_mean, u64 same_candidate_mean,
			  u64 cross_baseline_mean, u64 cross_candidate_mean)
{
	struct file *file;
	loff_t position = 0;
	size_t header_size = 4096U + (size_t)access_items * 16U;
	size_t row_size = 96U + (size_t)sfcp_group_lines() * 24U;
	char *header;
	char *row;
	size_t used;
	unsigned int i;
	int ret;

	header = kvmalloc(header_size, GFP_KERNEL);
	row = kvmalloc(row_size, GFP_KERNEL);
	if (!header || !row) {
		ret = -ENOMEM;
		goto out_buffers;
	}

	file = filp_open(result_path, O_CREAT | O_TRUNC | O_WRONLY, 0644);
	if (IS_ERR(file)) {
		ret = PTR_ERR(file);
		pr_err(SFCP_NAME ": filp_open(%s) failed: %d\n",
		       result_path, ret);
		goto out_buffers;
	}

	used = scnprintf(header, header_size,
		"# module,%s\n"
		"# experiment,paired_same_cpu_and_cross_cpu_prime_fill_probe\n"
		"# counter,%s\n"
		"# counter_frequency_hz,%llu\n"
		"# counter_calibration_delta,%llu\n"
		"# probe_pmu_events_available,%s\n"
		"# ll_cache_events_available,%s\n"
		"# l1d_tlb_refill_event_available,%s\n"
		"# probe_pmu_event_l1d_refill,0x03\n"
		"# probe_pmu_event_l1d_tlb_refill,0x05\n"
		"# probe_pmu_event_l2d_refill,0x17\n"
		"# probe_pmu_event_bus_access,0x19\n"
		"# probe_pmu_event_ll_cache_rd,0x36\n"
		"# probe_pmu_event_ll_cache_miss_rd,0x37\n"
		"# probe_cpu,%d\n"
		"# stimulus_cpus,%s\n"
		"# stimulus_cpu_count,%u\n"
		"# pool_pages,%u\n"
		"# allocated_bytes,%llu\n"
		"# address_unit_bytes,%u\n"
		"# addrmask,0x%llx\n"
		"# calibration_line_offset,0x%x\n"
		"# match_bits,%u\n"
		"# target_masked,0x%llx\n"
		"# target_mode,%s\n"
		"# paired_probe,true\n"
		"# cross_cache_maintenance_cpu,probe_cpu\n"
		"# access_items,%u\n"
		"# group_lines,%u\n"
		"# fill_rounds,%u\n"
		"# prefill_wait_us,%u\n"
		"# baseline_count,%u\n"
		"# candidate_count,%u\n"
		"# candidate_pool_size,%u\n"
		"# cross_retest_threshold,%u\n"
		"# cross_retest_repetitions,%u\n"
		"# cross_retest_max_groups,%u\n"
		"# cross_retest_triggered_groups,%u\n"
		"# cross_retest_sample_count,%u\n"
		"# same_first_baseline_mean,%llu\n"
		"# same_first_candidate_mean,%llu\n"
		"# cross_first_baseline_mean,%llu\n"
		"# cross_first_candidate_mean,%llu\n"
		"# same_prefill_baseline_mean,%llu\n"
		"# same_prefill_candidate_mean,%llu\n"
		"# cross_prefill_baseline_mean,%llu\n"
		"# cross_prefill_candidate_mean,%llu\n"
		"# same_wait_prefill_baseline_mean,%llu\n"
		"# same_wait_prefill_candidate_mean,%llu\n"
		"# cross_wait_prefill_baseline_mean,%llu\n"
		"# cross_wait_prefill_candidate_mean,%llu\n"
		"# cross_idle_baseline_mean,%llu\n"
		"# cross_idle_candidate_mean,%llu\n"
		"# same_baseline_mean,%llu\n"
		"# same_candidate_mean,%llu\n"
		"# same_candidate_minus_baseline,%lld\n"
		"# cross_baseline_mean,%llu\n"
		"# cross_candidate_mean,%llu\n"
		"# cross_candidate_minus_baseline,%lld\n"
		"# cross_minus_same_baseline,%lld\n"
		"# cross_minus_same_candidate,%lld\n",
		SFCP_NAME,
		sfcp_counter_name(),
		(unsigned long long)(sfcp_counter_kind == SFCP_COUNTER_CNTVCT ?
			read_sysreg(cntfrq_el0) : 0),
		(unsigned long long)sfcp_counter_calibration_delta,
		sfcp_probe_events_available ? "true" : "false",
		sfcp_ll_events_available ? "true" : "false",
		sfcp_l1d_tlb_event_available ? "true" : "false",
		probe_cpu, stimulus_cpus, sfcp_stimulus_count, pool_pages,
		(unsigned long long)pool_pages << PAGE_SHIFT,
		SFCP_CACHE_LINE_SIZE,
		(unsigned long long)sfcp_active_addrmask,
		line_offset, match_bits, target_low,
		sfcp_target_random ? "random" : "explicit", access_items,
		sfcp_group_lines(),
		fill_rounds, prefill_wait_us,
		baseline_sets, candidate_sets, sfcp_candidate_count,
		cross_retest_threshold, cross_retest_repetitions,
		cross_retest_max_groups, retest->group_count,
		retest->sample_count,
		(unsigned long long)same_first_baseline_mean,
		(unsigned long long)same_first_candidate_mean,
		(unsigned long long)cross_first_baseline_mean,
		(unsigned long long)cross_first_candidate_mean,
		(unsigned long long)same_prefill_baseline_mean,
		(unsigned long long)same_prefill_candidate_mean,
		(unsigned long long)cross_prefill_baseline_mean,
		(unsigned long long)cross_prefill_candidate_mean,
		(unsigned long long)same_wait_prefill_baseline_mean,
		(unsigned long long)same_wait_prefill_candidate_mean,
		(unsigned long long)cross_wait_prefill_baseline_mean,
		(unsigned long long)cross_wait_prefill_candidate_mean,
		(unsigned long long)cross_idle_baseline_mean,
		(unsigned long long)cross_idle_candidate_mean,
		(unsigned long long)same_baseline_mean,
		(unsigned long long)same_candidate_mean,
		(long long)same_candidate_mean - (long long)same_baseline_mean,
		(unsigned long long)cross_baseline_mean,
		(unsigned long long)cross_candidate_mean,
		(long long)cross_candidate_mean -
			(long long)cross_baseline_mean,
		(long long)cross_baseline_mean - (long long)same_baseline_mean,
		(long long)cross_candidate_mean -
			(long long)same_candidate_mean);
	used += scnprintf(header + used, header_size - used,
			  "type,index,probe_ticks,l1d_refill,l1d_tlb_refill,l2d_refill,bus_access,ll_cache_rd,ll_cache_miss_rd");
	for (i = 0; i < access_items; ++i)
		used += scnprintf(header + used, header_size - used,
				  ",pa%u", i);
	used += scnprintf(header + used, header_size - used,
			  ",probe_pa,source_candidate_index,repetition\n");
	if (used >= header_size - 1U) {
		ret = -EOVERFLOW;
		goto out_file;
	}

	ret = sfcp_write_all(file, header, used, &position);
	for (i = 0; !ret && i < baseline_sets; ++i) {
		struct sfcp_sample *sample = sfcp_sample_at(baseline, i);

		ret = sfcp_write_sample(file, "same_first_baseline", i,
					sample, sample->same_first_ticks,
					&position, row, row_size);
		if (!ret)
			ret = sfcp_write_sample(file, "cross_first_baseline",
						i, sample,
						sample->cross_first_ticks,
						&position, row, row_size);
		if (!ret)
			ret = sfcp_write_sample(file, "same_prefill_baseline",
						i, sample,
						sample->same_prefill_ticks,
						&position, row, row_size);
		if (!ret)
			ret = sfcp_write_sample(file, "cross_prefill_baseline",
						i, sample,
						sample->cross_prefill_ticks,
						&position, row, row_size);
		if (!ret)
			ret = sfcp_write_sample(file,
						"same_wait_prefill_baseline",
						i, sample,
						sample->same_wait_prefill_ticks,
						&position, row, row_size);
		if (!ret)
			ret = sfcp_write_sample(file,
						"cross_wait_prefill_baseline",
						i, sample,
						sample->cross_wait_prefill_ticks,
						&position, row, row_size);
		if (!ret)
			ret = sfcp_write_sample(file, "cross_idle_baseline",
						i, sample,
						sample->cross_idle_ticks,
						&position, row, row_size);
		if (!ret)
			ret = sfcp_write_sample(file, "same_baseline", i, sample,
					sample->same_cpu_ticks, &position,
					row, row_size);
		if (!ret)
			ret = sfcp_write_sample(file, "cross_baseline", i,
						sample, sample->cross_cpu_ticks,
						&position, row, row_size);
	}
	for (i = 0; !ret && i < candidate_sets; ++i) {
		struct sfcp_sample *sample = sfcp_sample_at(candidate, i);

		ret = sfcp_write_sample(file, "same_first_candidate", i,
					sample, sample->same_first_ticks,
					&position, row, row_size);
		if (!ret)
			ret = sfcp_write_sample(file, "cross_first_candidate",
						i, sample,
						sample->cross_first_ticks,
						&position, row, row_size);
		if (!ret)
			ret = sfcp_write_sample(file, "same_prefill_candidate",
						i, sample,
						sample->same_prefill_ticks,
						&position, row, row_size);
		if (!ret)
			ret = sfcp_write_sample(file, "cross_prefill_candidate",
						i, sample,
						sample->cross_prefill_ticks,
						&position, row, row_size);
		if (!ret)
			ret = sfcp_write_sample(file,
						"same_wait_prefill_candidate",
						i, sample,
						sample->same_wait_prefill_ticks,
						&position, row, row_size);
		if (!ret)
			ret = sfcp_write_sample(file,
						"cross_wait_prefill_candidate",
						i, sample,
						sample->cross_wait_prefill_ticks,
						&position, row, row_size);
		if (!ret)
			ret = sfcp_write_sample(file, "cross_idle_candidate",
						i, sample,
						sample->cross_idle_ticks,
						&position, row, row_size);
		if (!ret)
			ret = sfcp_write_sample(file, "same_candidate", i, sample,
					sample->same_cpu_ticks, &position,
					row, row_size);
		if (!ret)
			ret = sfcp_write_sample(file, "cross_candidate", i,
						sample, sample->cross_cpu_ticks,
						&position, row, row_size);
	}
	for (i = 0; !ret && i < retest->sample_count; ++i) {
		const struct sfcp_cross_retest *result = &retest->sample[i];
		struct sfcp_sample *source = sfcp_sample_at(
			candidate, result->source_candidate_index);

		ret = sfcp_write_retest(file, i, result, source, &position,
					row, row_size);
	}

out_file:
	filp_close(file, NULL);
	if (ret)
		pr_err(SFCP_NAME ": writing %s failed: %d\n",
		       result_path, ret);
	else
		pr_info(SFCP_NAME ": wrote paired same/cross measurements for %u baseline, %u candidate, and %u cross-retest samples from %u triggered groups to %s\n",
			baseline_sets, candidate_sets, retest->sample_count,
			retest->group_count, result_path);
out_buffers:
	kvfree(row);
	kvfree(header);
	return ret;
}

static int __maybe_unused sfcp_run_experiment(void)
{
	void *baseline = NULL;
	void *candidate = NULL;
	u32 *idx = NULL;
	struct sfcp_line *group = NULL;
	struct sfcp_cross_retest_state retest = { };
	u64 same_first_baseline_sum = 0;
	u64 same_first_candidate_sum = 0;
	u64 cross_first_baseline_sum = 0;
	u64 cross_first_candidate_sum = 0;
	u64 same_prefill_baseline_sum = 0;
	u64 same_prefill_candidate_sum = 0;
	u64 cross_prefill_baseline_sum = 0;
	u64 cross_prefill_candidate_sum = 0;
	u64 same_wait_prefill_baseline_sum = 0;
	u64 same_wait_prefill_candidate_sum = 0;
	u64 cross_wait_prefill_baseline_sum = 0;
	u64 cross_wait_prefill_candidate_sum = 0;
	u64 cross_idle_baseline_sum = 0;
	u64 cross_idle_candidate_sum = 0;
	u64 same_baseline_sum = 0;
	u64 same_candidate_sum = 0;
	u64 cross_baseline_sum = 0;
	u64 cross_candidate_sum = 0;
	u64 same_first_baseline_mean;
	u64 same_first_candidate_mean;
	u64 cross_first_baseline_mean;
	u64 cross_first_candidate_mean;
	u64 same_prefill_baseline_mean;
	u64 same_prefill_candidate_mean;
	u64 cross_prefill_baseline_mean;
	u64 cross_prefill_candidate_mean;
	u64 same_wait_prefill_baseline_mean;
	u64 same_wait_prefill_candidate_mean;
	u64 cross_wait_prefill_baseline_mean;
	u64 cross_wait_prefill_candidate_mean;
	u64 cross_idle_baseline_mean;
	u64 cross_idle_candidate_mean;
	u64 same_baseline_mean;
	u64 same_candidate_mean;
	u64 cross_baseline_mean;
	u64 cross_candidate_mean;
	unsigned int i;
	int ret;

	if (cross_retest_repetitions && cross_retest_max_groups) {
		unsigned int groups = min(candidate_sets,
					  cross_retest_max_groups);

		retest.capacity = groups * cross_retest_repetitions;
		retest.sample = kvmalloc_array(retest.capacity,
					       sizeof(*retest.sample), GFP_KERNEL);
		if (!retest.sample) {
			ret = -ENOMEM;
			goto out;
		}
	}
	baseline = kvmalloc_array(baseline_sets, sfcp_sample_size, GFP_KERNEL);
	candidate = kvmalloc_array(candidate_sets, sfcp_sample_size,
				  GFP_KERNEL);
	idx = kvmalloc_array(sfcp_group_lines(), sizeof(*idx), GFP_KERNEL);
	group = kvmalloc_array(sfcp_group_lines(), sizeof(*group), GFP_KERNEL);
	if (!baseline || !candidate || !idx || !group) {
		ret = -ENOMEM;
		goto out;
	}

	sfcp_enable_counter();
	pr_info(SFCP_NAME ": counter=%s calibration_delta=%llu probe_pmu_events=%s ll_cache_events=%s l1d_tlb_refill_event=%s pmcr=0x%llx pmccfiltr=0x%llx probe_cpu=%d stimulus_cpus=%s stimulus_cpu_count=%u addrmask=0x%llx target_masked=0x%llx address_unit=%u access_items=%u group_lines=%u prefill_wait_us=%u allocated_pages=%u candidate_pool=%u/%u unrelated_pool=%u/%u cross_retest_threshold=%u repetitions=%u max_groups=%u\n",
		sfcp_counter_name(),
		(unsigned long long)sfcp_counter_calibration_delta,
		sfcp_probe_events_available ? "enabled" : "unavailable",
		sfcp_ll_events_available ? "enabled" : "unavailable",
		sfcp_l1d_tlb_event_available ? "enabled" : "unavailable",
		(unsigned long long)read_sysreg(pmcr_el0),
		(unsigned long long)read_sysreg(pmccfiltr_el0),
		probe_cpu, stimulus_cpus, sfcp_stimulus_count,
		(unsigned long long)sfcp_active_addrmask, target_low,
		SFCP_CACHE_LINE_SIZE, access_items,
		sfcp_group_lines(), prefill_wait_us,
		sfcp_allocated_pages,
		sfcp_candidate_count, sfcp_candidate_total,
		sfcp_unrelated_count, sfcp_unrelated_total,
		cross_retest_threshold, cross_retest_repetitions,
		cross_retest_max_groups);
	if (!sfcp_counter_calibration_delta) {
		pr_err(SFCP_NAME ": no counter progress across a cold serialized load; refusing to write an all-zero experiment\n");
		ret = -EOPNOTSUPP;
		goto out_counter;
	}

	ret = sfcp_collect_interleaved(baseline, candidate, idx, group,
				       &retest);
	if (ret)
		goto out_counter;

	for (i = 0; i < baseline_sets; ++i) {
		struct sfcp_sample *sample = sfcp_sample_at(baseline, i);

		same_first_baseline_sum += sample->same_first_ticks;
		cross_first_baseline_sum += sample->cross_first_ticks;
		same_prefill_baseline_sum += sample->same_prefill_ticks;
		cross_prefill_baseline_sum += sample->cross_prefill_ticks;
		same_wait_prefill_baseline_sum +=
			sample->same_wait_prefill_ticks;
		cross_wait_prefill_baseline_sum +=
			sample->cross_wait_prefill_ticks;
		cross_idle_baseline_sum += sample->cross_idle_ticks;
		same_baseline_sum += sample->same_cpu_ticks;
		cross_baseline_sum += sample->cross_cpu_ticks;
	}
	for (i = 0; i < candidate_sets; ++i) {
		struct sfcp_sample *sample = sfcp_sample_at(candidate, i);

		same_first_candidate_sum += sample->same_first_ticks;
		cross_first_candidate_sum += sample->cross_first_ticks;
		same_prefill_candidate_sum += sample->same_prefill_ticks;
		cross_prefill_candidate_sum += sample->cross_prefill_ticks;
		same_wait_prefill_candidate_sum +=
			sample->same_wait_prefill_ticks;
		cross_wait_prefill_candidate_sum +=
			sample->cross_wait_prefill_ticks;
		cross_idle_candidate_sum += sample->cross_idle_ticks;
		same_candidate_sum += sample->same_cpu_ticks;
		cross_candidate_sum += sample->cross_cpu_ticks;
	}
	if (!same_baseline_sum && !same_candidate_sum &&
	    !cross_baseline_sum && !cross_candidate_sum) {
		pr_err(SFCP_NAME ": every measured reload was zero with counter=%s; refusing invalid results\n",
		       sfcp_counter_name());
		ret = -ERANGE;
		goto out_counter;
	}
	same_first_baseline_mean =
		div_u64(same_first_baseline_sum, baseline_sets);
	same_first_candidate_mean =
		div_u64(same_first_candidate_sum, candidate_sets);
	cross_first_baseline_mean =
		div_u64(cross_first_baseline_sum, baseline_sets);
	cross_first_candidate_mean =
		div_u64(cross_first_candidate_sum, candidate_sets);
	same_prefill_baseline_mean =
		div_u64(same_prefill_baseline_sum, baseline_sets);
	same_prefill_candidate_mean =
		div_u64(same_prefill_candidate_sum, candidate_sets);
	cross_prefill_baseline_mean =
		div_u64(cross_prefill_baseline_sum, baseline_sets);
	cross_prefill_candidate_mean =
		div_u64(cross_prefill_candidate_sum, candidate_sets);
	same_wait_prefill_baseline_mean =
		div_u64(same_wait_prefill_baseline_sum, baseline_sets);
	same_wait_prefill_candidate_mean =
		div_u64(same_wait_prefill_candidate_sum, candidate_sets);
	cross_wait_prefill_baseline_mean =
		div_u64(cross_wait_prefill_baseline_sum, baseline_sets);
	cross_wait_prefill_candidate_mean =
		div_u64(cross_wait_prefill_candidate_sum, candidate_sets);
	cross_idle_baseline_mean =
		div_u64(cross_idle_baseline_sum, baseline_sets);
	cross_idle_candidate_mean =
		div_u64(cross_idle_candidate_sum, candidate_sets);
	same_baseline_mean = div_u64(same_baseline_sum, baseline_sets);
	same_candidate_mean = div_u64(same_candidate_sum, candidate_sets);
	cross_baseline_mean = div_u64(cross_baseline_sum, baseline_sets);
	cross_candidate_mean = div_u64(cross_candidate_sum, candidate_sets);

	pr_info(SFCP_NAME ": first-access same baseline=%llu candidate=%llu; cross baseline=%llu candidate=%llu\n",
		(unsigned long long)same_first_baseline_mean,
		(unsigned long long)same_first_candidate_mean,
		(unsigned long long)cross_first_baseline_mean,
		(unsigned long long)cross_first_candidate_mean);
	pr_info(SFCP_NAME ": pre-fill-hot same baseline=%llu candidate=%llu; cross baseline=%llu candidate=%llu\n",
		(unsigned long long)same_prefill_baseline_mean,
		(unsigned long long)same_prefill_candidate_mean,
		(unsigned long long)cross_prefill_baseline_mean,
		(unsigned long long)cross_prefill_candidate_mean);
	pr_info(SFCP_NAME ": wait-prefill-hot wait_us=%u same baseline=%llu candidate=%llu; cross baseline=%llu candidate=%llu\n",
		prefill_wait_us,
		(unsigned long long)same_wait_prefill_baseline_mean,
		(unsigned long long)same_wait_prefill_candidate_mean,
		(unsigned long long)cross_wait_prefill_baseline_mean,
		(unsigned long long)cross_wait_prefill_candidate_mean);
	pr_info(SFCP_NAME ": cross-idle baseline=%llu candidate=%llu (%u stimulus CPU handshakes, zero item accesses)\n",
		(unsigned long long)cross_idle_baseline_mean,
		(unsigned long long)cross_idle_candidate_mean,
		sfcp_stimulus_count);
	pr_info(SFCP_NAME ": same baseline=%llu candidate=%llu candidate-baseline=%lld\n",
		(unsigned long long)same_baseline_mean,
		(unsigned long long)same_candidate_mean,
		(long long)same_candidate_mean - (long long)same_baseline_mean);
	pr_info(SFCP_NAME ": cross baseline=%llu candidate=%llu candidate-baseline=%lld\n",
		(unsigned long long)cross_baseline_mean,
		(unsigned long long)cross_candidate_mean,
		(long long)cross_candidate_mean -
			(long long)cross_baseline_mean);
	pr_info(SFCP_NAME ": cross-same baseline=%lld candidate=%lld\n",
		(long long)cross_baseline_mean - (long long)same_baseline_mean,
		(long long)cross_candidate_mean -
			(long long)same_candidate_mean);
	ret = sfcp_write_csv(baseline, candidate, &retest,
			     same_first_baseline_mean,
			     same_first_candidate_mean,
			     cross_first_baseline_mean,
			     cross_first_candidate_mean,
			     same_prefill_baseline_mean,
			     same_prefill_candidate_mean,
			     cross_prefill_baseline_mean,
			     cross_prefill_candidate_mean,
			     same_wait_prefill_baseline_mean,
			     same_wait_prefill_candidate_mean,
			     cross_wait_prefill_baseline_mean,
			     cross_wait_prefill_candidate_mean,
			     cross_idle_baseline_mean,
			     cross_idle_candidate_mean,
			     same_baseline_mean, same_candidate_mean,
			     cross_baseline_mean, cross_candidate_mean);

out_counter:
	sfcp_restore_counter();
out:
	kvfree(retest.sample);
	kvfree(group);
	kvfree(idx);
	kvfree(candidate);
	kvfree(baseline);
	return ret;
}

static int sfcp_probe_thread(void *unused)
{
	(void)unused;
#ifdef SFCP_EXPERIMENT_ENTRY
#define SFCP_CUSTOM_ENTRY_SUPPORTED 1
	sfcp_probe_result = SFCP_EXPERIMENT_ENTRY();
#else
	sfcp_probe_result = sfcp_run_experiment();
#endif
	complete(&sfcp_probe_done);
	return 0;
}

static void sfcp_free_pool(void)
{
	unsigned int i;

	for (i = 0; i < sfcp_allocated_pages; ++i)
		__free_page(sfcp_pages[i]);
	kvfree(sfcp_candidate_pool);
	kvfree(sfcp_unrelated_pool);
	kvfree(sfcp_shared_evictors);
	kvfree(sfcp_pages);
	sfcp_candidate_pool = NULL;
	sfcp_unrelated_pool = NULL;
	sfcp_shared_evictors = NULL;
	sfcp_pages = NULL;
	sfcp_allocated_pages = 0;
	sfcp_candidate_count = 0;
	sfcp_unrelated_count = 0;
	sfcp_candidate_total = 0;
	sfcp_unrelated_total = 0;
}

static int sfcp_allocate_pool(void)
{
	unsigned int i;

	sfcp_pages = kvmalloc_array(pool_pages, sizeof(*sfcp_pages), GFP_KERNEL);
	sfcp_candidate_pool =
		kvmalloc_array(pool_pages, sizeof(*sfcp_candidate_pool),
			       GFP_KERNEL);
	sfcp_unrelated_pool =
		kvmalloc_array(pool_pages, sizeof(*sfcp_unrelated_pool),
			       GFP_KERNEL);
	sfcp_shared_evictors =
		kvmalloc_array(access_items, sizeof(*sfcp_shared_evictors),
			       GFP_KERNEL);
	if (!sfcp_pages || !sfcp_candidate_pool || !sfcp_unrelated_pool ||
	    !sfcp_shared_evictors)
		return -ENOMEM;

	for (i = 0; i < pool_pages; ++i) {
		struct page *page = alloc_page(GFP_KERNEL | __GFP_ZERO);

		if (!page)
			return -ENOMEM;
		sfcp_pages[sfcp_allocated_pages++] = page;
	}

	if (sfcp_target_random) {
		unsigned int selected =
			sfcp_random_below(sfcp_allocated_pages);
		unsigned int selected_offset =
			sfcp_random_below(SFCP_LINES_PER_PAGE) *
			SFCP_CACHE_LINE_SIZE;

		target_low = (page_to_phys(sfcp_pages[selected]) +
			      selected_offset) & sfcp_active_addrmask;
		pr_info(SFCP_NAME ": randomly selected target_masked=0x%llx with addrmask=0x%llx\n",
			target_low,
			(unsigned long long)sfcp_active_addrmask);
	}

	for (i = 0; i < sfcp_allocated_pages; ++i) {
#ifdef SFCP_ABLATION_BUILD
		/*
		 * The ablation build reconstructs both historical target pools from
		 * sfcp_pages in its experiment entry point.  Init only needs enough
		 * lines for the common validation and worker setup, so avoid walking
		 * every cache line and maintaining a huge unrelated reservoir.
		 */
		unsigned int offset = target_low & (PAGE_SIZE - 1U);
		phys_addr_t pa = page_to_phys(sfcp_pages[i]) + offset;
		struct sfcp_line line = {
			.page = sfcp_pages[i],
			.offset = offset,
		};

		if ((pa & sfcp_active_addrmask) == target_low)
			sfcp_reservoir_add(sfcp_candidate_pool,
					   &sfcp_candidate_count,
					   &sfcp_candidate_total, &line);
		line.offset ^= SFCP_CACHE_LINE_SIZE;
		sfcp_reservoir_add(sfcp_unrelated_pool, &sfcp_unrelated_count,
				   &sfcp_unrelated_total, &line);
#else
		unsigned int offset;

		for (offset = 0; offset < PAGE_SIZE;
		     offset += SFCP_CACHE_LINE_SIZE) {
			phys_addr_t pa = page_to_phys(sfcp_pages[i]) + offset;
			struct sfcp_line line = {
				.page = sfcp_pages[i],
				.offset = offset,
			};

			if ((pa & sfcp_active_addrmask) == target_low)
				sfcp_reservoir_add(sfcp_candidate_pool,
						   &sfcp_candidate_count,
						   &sfcp_candidate_total,
						   &line);
			else
				sfcp_reservoir_add(sfcp_unrelated_pool,
						   &sfcp_unrelated_count,
						   &sfcp_unrelated_total,
						   &line);
		}
#endif
	}
	return 0;
}

static void sfcp_stop_stimulus(void)
{
	unsigned int cpu;

	WRITE_ONCE(sfcp_shutdown, true);
	wake_up_all(&sfcp_prepare_waitq);
	for_each_cpu(cpu, &sfcp_stimulus_mask) {
		if (!sfcp_stimulus_tasks || !sfcp_stimulus_tasks[cpu])
			continue;
		kthread_stop(sfcp_stimulus_tasks[cpu]);
		sfcp_stimulus_tasks[cpu] = NULL;
	}
}

static int __init sfcp_init(void)
{
	unsigned int cpu;
	int ret;

	if (PAGE_SIZE != 4096 || !pool_pages ||
	    pool_pages > SFCP_POOL_PAGES_MAX ||
	    !access_items || access_items > SFCP_ACCESS_ITEMS_MAX ||
	    probe_cpu < 0 || probe_cpu >= nr_cpu_ids ||
	    (line_offset & (SFCP_CACHE_LINE_SIZE - 1U)) ||
	    line_offset >= PAGE_SIZE ||
	    match_bits < SFCP_MATCH_BITS_MIN ||
	    match_bits > SFCP_MATCH_BITS_MAX ||
	    !fill_rounds || fill_rounds > SFCP_FILL_ROUNDS_MAX ||
	    prefill_wait_us > SFCP_PREFILL_WAIT_US_MAX ||
	    cross_retest_repetitions > SFCP_CROSS_RETEST_REPETITIONS_MAX ||
	    cross_retest_max_groups > SFCP_CROSS_RETEST_MAX_GROUPS ||
	    !baseline_sets || baseline_sets > SFCP_MAX_SAMPLES ||
	    !candidate_sets || candidate_sets > SFCP_MAX_SAMPLES ||
	    !result_path || result_path[0] != '/' ||
	    !stimulus_cpus || !stimulus_cpus[0])
		return -EINVAL;

	sfcp_active_addrmask = addrmask ? addrmask : sfcp_legacy_match_mask();
	sfcp_target_random = target_low == SFCP_TARGET_RANDOM;
	if (!sfcp_target_random &&
	    ((target_low & ~sfcp_active_addrmask) ||
	     (target_low & (SFCP_CACHE_LINE_SIZE - 1U))))
		return -EINVAL;

	sfcp_sample_size = sizeof(struct sfcp_sample) +
		(size_t)sfcp_group_lines() * sizeof(phys_addr_t);

	ret = cpulist_parse(stimulus_cpus, &sfcp_stimulus_mask);
	if (ret || cpumask_empty(&sfcp_stimulus_mask) ||
	    cpumask_test_cpu(probe_cpu, &sfcp_stimulus_mask))
		return -EINVAL;

	cpus_read_lock();
	ret = cpu_online(probe_cpu) &&
	      cpumask_subset(&sfcp_stimulus_mask, cpu_online_mask) ?
		0 : -ENODEV;
	if (!ret)
		sfcp_stimulus_count = cpumask_weight(&sfcp_stimulus_mask);
	cpus_read_unlock();
	if (ret)
		return ret;

	sfcp_stimulus_tasks =
		kcalloc(nr_cpu_ids, sizeof(*sfcp_stimulus_tasks), GFP_KERNEL);
	sfcp_prepare_done =
		kcalloc(nr_cpu_ids, sizeof(*sfcp_prepare_done), GFP_KERNEL);
	sfcp_access_done =
		kcalloc(nr_cpu_ids, sizeof(*sfcp_access_done), GFP_KERNEL);
	if (!sfcp_stimulus_tasks || !sfcp_prepare_done || !sfcp_access_done) {
		ret = -ENOMEM;
		goto out;
	}

	ret = sfcp_allocate_pool();
	if (ret)
		goto out;
	if (sfcp_candidate_count < sfcp_group_lines() ||
	    sfcp_unrelated_count < access_items) {
		pr_err(SFCP_NAME ": paired experiment needs %u mask-matching cache lines (one shared probe plus %u stimulus) and %u non-matching baseline lines, but candidate=%u unrelated=%u; reduce access_items, relax addrmask, try another target_low, or use a larger pool\n",
		       sfcp_group_lines(), access_items, access_items,
		       sfcp_candidate_count, sfcp_unrelated_count);
		ret = -ENOSPC;
		goto out;
	}

	WRITE_ONCE(sfcp_shutdown, false);
	WRITE_ONCE(sfcp_sync.prepare_request, 0);
	WRITE_ONCE(sfcp_sync.access_go, 0);
	WRITE_ONCE(sfcp_sync.active_items, 0);
	reinit_completion(&sfcp_probe_done);

	for_each_cpu(cpu, &sfcp_stimulus_mask) {
		sfcp_stimulus_tasks[cpu] =
			kthread_create(sfcp_stimulus_thread,
				       (void *)(unsigned long)cpu,
				       "sfcp_stim/%u", cpu);
		if (IS_ERR(sfcp_stimulus_tasks[cpu])) {
			ret = PTR_ERR(sfcp_stimulus_tasks[cpu]);
			sfcp_stimulus_tasks[cpu] = NULL;
			sfcp_stop_stimulus();
			goto out;
		}
		kthread_bind(sfcp_stimulus_tasks[cpu], cpu);
		wake_up_process(sfcp_stimulus_tasks[cpu]);
	}

	sfcp_probe_result = -EINPROGRESS;
	sfcp_probe_task = kthread_create(sfcp_probe_thread, NULL,
					 "sfcp_probe/%d", probe_cpu);
	if (IS_ERR(sfcp_probe_task)) {
		ret = PTR_ERR(sfcp_probe_task);
		sfcp_probe_task = NULL;
		sfcp_stop_stimulus();
		goto out;
	}
	kthread_bind(sfcp_probe_task, probe_cpu);
	wake_up_process(sfcp_probe_task);
	wait_for_completion(&sfcp_probe_done);
	ret = sfcp_probe_result;
	sfcp_probe_task = NULL;
	sfcp_stop_stimulus();

out:
	if (ret)
		pr_err(SFCP_NAME ": experiment failed: %d\n", ret);
	sfcp_free_pool();
	kfree(sfcp_access_done);
	sfcp_access_done = NULL;
	kfree(sfcp_prepare_done);
	sfcp_prepare_done = NULL;
	kfree(sfcp_stimulus_tasks);
	sfcp_stimulus_tasks = NULL;
	return ret;
}

static void __exit sfcp_exit(void)
{
	if (sfcp_probe_task) {
		kthread_stop(sfcp_probe_task);
		sfcp_probe_task = NULL;
	}
	sfcp_stop_stimulus();
	sfcp_free_pool();
	pr_info(SFCP_NAME ": unloaded\n");
}

module_init(sfcp_init);
module_exit(sfcp_exit);

#ifndef SFCP_MODULE_DESCRIPTION
#define SFCP_MODULE_DESCRIPTION \
	"RK3588 cross-core prime/fill/probe configurable-low-bits comparison"
#endif
MODULE_DESCRIPTION(SFCP_MODULE_DESCRIPTION);
MODULE_AUTHOR("HuaijiGao");
MODULE_LICENSE("GPL");
