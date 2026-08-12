// SPDX-License-Identifier: GPL-2.0
/*
 * RK3588 same-core nine-address probe experiment.
 *
 * For each group:
 *   1) invalidate all nine cache lines;
 *   2) access line[8] once to make it the cached probe;
 *   3) access line[0]..line[7] in order for fill_rounds passes;
 *   4) precisely time one more access to line[8].
 *
 * Baseline groups have pairwise-distinct PA[22:0].  Candidate groups have
 * identical PA[22:0].  Both paths use exactly the same access sequence.
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

#include <asm/barrier.h>
#include <asm/sysreg.h>

#define SFP_NAME                    "rk3588_sf_probe"
#define SFP_POOL_PAGES_DEFAULT      131072U /* 512 MiB */
#define SFP_POOL_PAGES_MAX          262144U
#define SFP_GROUP_LINES             9U
#define SFP_EVICTOR_LINES           8U
#define SFP_FILL_ROUNDS_DEFAULT     10U
#define SFP_FILL_ROUNDS_MAX         1000U
#define SFP_DEFAULT_BASELINE_SETS   5000U
#define SFP_DEFAULT_CANDIDATE_SETS  5000U
#define SFP_MAX_SAMPLES             200000U
#define SFP_LOW23_MASK              ((1U << 23) - 1U)
#define SFP_DEFAULT_OFFSET          0xfc0U
#define SFP_CSV_LINE_SIZE           320U

static unsigned int pool_pages = SFP_POOL_PAGES_DEFAULT;
module_param(pool_pages, uint, 0444);
MODULE_PARM_DESC(pool_pages, "4 KiB pages in private pool (131072 = 512 MiB)");

static int measure_cpu = 7;
module_param(measure_cpu, int, 0444);
MODULE_PARM_DESC(measure_cpu, "Linux CPU number for the entire experiment");

static unsigned int line_offset = SFP_DEFAULT_OFFSET;
module_param(line_offset, uint, 0444);
MODULE_PARM_DESC(line_offset, "64-byte-aligned address offset inside every page");

static unsigned int target_low23 = 0x7ffc0U;
module_param(target_low23, uint, 0444);
MODULE_PARM_DESC(target_low23, "Low 23 physical-address bits for candidate pages");

static unsigned int fill_rounds = SFP_FILL_ROUNDS_DEFAULT;
module_param(fill_rounds, uint, 0444);
MODULE_PARM_DESC(fill_rounds, "Ordered passes over line[0] through line[7]");

static unsigned int baseline_sets = SFP_DEFAULT_BASELINE_SETS;
module_param(baseline_sets, uint, 0444);
MODULE_PARM_DESC(baseline_sets, "Number of unrelated baseline groups");

static unsigned int candidate_sets = SFP_DEFAULT_CANDIDATE_SETS;
module_param(candidate_sets, uint, 0444);
MODULE_PARM_DESC(candidate_sets, "Number of same-low23 candidate groups");

static char *result_path = "/tmp/rk3588_sf_probe.csv";
module_param(result_path, charp, 0444);
MODULE_PARM_DESC(result_path, "Absolute CSV path; parent directory must exist");

enum sfp_counter_kind {
	SFP_COUNTER_PMCCNTR,
	SFP_COUNTER_CNTVCT,
};

struct sfp_line {
	struct page *page;
	u32 offset;
};

struct sfp_sample {
	u64 probe_ticks;
	phys_addr_t pa[SFP_GROUP_LINES];
};

static struct page **sfp_pages;
static struct sfp_line *sfp_candidate_pool;
static struct sfp_line *sfp_unrelated_pool;
static unsigned int sfp_allocated_pages;
static unsigned int sfp_candidate_count;
static unsigned int sfp_unrelated_count;

static struct task_struct *sfp_worker;
static DECLARE_COMPLETION(sfp_worker_done);
static int sfp_worker_result;

/* The following PMU state is touched only by the CPU-bound worker. */
static enum sfp_counter_kind sfp_counter_kind = SFP_COUNTER_CNTVCT;
static u64 sfp_saved_pmcr;
static bool sfp_saved_cycle_enabled;
static bool sfp_pmu_changed;

static __always_inline void *sfp_address(const struct sfp_line *line)
{
	return (char *)page_address(line->page) + line->offset;
}

static __always_inline phys_addr_t sfp_physical(const struct sfp_line *line)
{
	return page_to_phys(line->page) + line->offset;
}

static __always_inline void sfp_touch(const void *address)
{
	(void)READ_ONCE(*(const u64 *)address);
}

static __always_inline void sfp_civac(const void *address)
{
	asm volatile("dc civac, %0" : : "r" (address) : "memory");
}

static __always_inline u64 sfp_read_counter(void)
{
	u64 value;

	isb();
	if (likely(sfp_counter_kind == SFP_COUNTER_PMCCNTR))
		value = read_sysreg(pmccntr_el0);
	else
		value = read_sysreg(cntvct_el0);
	isb();
	return value;
}

static __always_inline u64 sfp_counter_delta(u64 before, u64 after)
{
	if (sfp_counter_kind == SFP_COUNTER_PMCCNTR &&
	    !(read_sysreg(pmcr_el0) & BIT(6)))
		return (u32)after - (u32)before;
	return after - before;
}

static __always_inline u32 sfp_random_below(u32 upper)
{
	if (upper <= 1)
		return 0;
	return reciprocal_scale(get_random_u32(), upper);
}

static void sfp_enable_counter(void)
{
	u64 current_pmcr;
	u64 target_pmcr;
	u64 enabled;
	u64 before;
	u64 after;
	unsigned int i;

	sfp_saved_pmcr = read_sysreg(pmcr_el0);
	enabled = read_sysreg(pmcntenset_el0);
	sfp_saved_cycle_enabled = !!(enabled & BIT(31));

	target_pmcr = (sfp_saved_pmcr | BIT(0)) & ~BIT(3);
	current_pmcr = read_sysreg(pmcr_el0);
	if (current_pmcr != target_pmcr) {
		write_sysreg(target_pmcr, pmcr_el0);
		sfp_pmu_changed = true;
	}
	if (!(enabled & BIT(31))) {
		write_sysreg(BIT(31), pmcntenset_el0);
		sfp_pmu_changed = true;
	}
	isb();

	before = read_sysreg(pmccntr_el0);
	for (i = 0; i < 256; ++i)
		cpu_relax();
	after = read_sysreg(pmccntr_el0);
	current_pmcr = read_sysreg(pmcr_el0);
	enabled = read_sysreg(pmcntenset_el0);
	if ((current_pmcr & BIT(0)) && (enabled & BIT(31)) && after != before)
		sfp_counter_kind = SFP_COUNTER_PMCCNTR;
	else
		sfp_counter_kind = SFP_COUNTER_CNTVCT;
}

static void sfp_restore_counter(void)
{
	if (!sfp_pmu_changed)
		return;
	if (!sfp_saved_cycle_enabled)
		write_sysreg(BIT(31), pmcntenclr_el0);
	write_sysreg(sfp_saved_pmcr, pmcr_el0);
	isb();
	sfp_pmu_changed = false;
}

static bool sfp_pick_group(const struct sfp_line *pool, unsigned int pool_size,
			   bool require_distinct_low23, u32 *idx)
{
	unsigned int i;

	if (pool_size < SFP_GROUP_LINES)
		return false;

	for (i = 0; i < SFP_GROUP_LINES; ++i) {
		unsigned int retry;

		for (retry = 0; retry < 4096; ++retry) {
			u32 candidate = sfp_random_below(pool_size);
			u32 candidate_low23 =
				(u32)(sfp_physical(&pool[candidate]) &
				      SFP_LOW23_MASK);
			unsigned int j;
			bool reject = false;

			for (j = 0; j < i; ++j) {
				u32 selected_low23;

				if (idx[j] == candidate) {
					reject = true;
					break;
				}
				if (!require_distinct_low23)
					continue;
				selected_low23 =
					(u32)(sfp_physical(&pool[idx[j]]) &
					      SFP_LOW23_MASK);
				if (selected_low23 == candidate_low23) {
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

static u64 sfp_measure_probe(const struct sfp_line *group)
{
	void *probe = sfp_address(&group[SFP_GROUP_LINES - 1U]);
	unsigned long irq_flags;
	u64 before;
	u64 after;
	unsigned int round;
	unsigned int i;

	/* Give every group the same empty starting state. */
	for (i = 0; i < SFP_GROUP_LINES; ++i)
		sfp_civac(sfp_address(&group[i]));
	dsb(ish);
	isb();

	/*
	 * Keep the complete prime/fill/probe sequence on this CPU without
	 * unrelated scheduling or local interrupt cache activity.
	 */
	preempt_disable();
	local_irq_save(irq_flags);

	/* Prime line[8]. */
	sfp_touch(probe);
	dsb(ish);

	/* Fill with line[0]..line[7], in the requested fixed order. */
	for (round = 0; round < fill_rounds; ++round) {
		for (i = 0; i < SFP_EVICTOR_LINES; ++i)
			sfp_touch(sfp_address(&group[i]));
		dsb(ish);
	}

	/* Probe only line[8]; the counter surrounds this single load. */
	before = sfp_read_counter();
	sfp_touch(probe);
	dsb(ish);
	after = sfp_read_counter();

	local_irq_restore(irq_flags);
	preempt_enable();
	return sfp_counter_delta(before, after);
}

static int sfp_take_sample(const struct sfp_line *pool,
			   unsigned int pool_size,
			   bool require_distinct_low23,
			   struct sfp_sample *sample)
{
	u32 idx[SFP_GROUP_LINES];
	struct sfp_line group[SFP_GROUP_LINES];
	unsigned int i;

	if (!sfp_pick_group(pool, pool_size, require_distinct_low23, idx))
		return -ERANGE;
	for (i = 0; i < SFP_GROUP_LINES; ++i) {
		group[i] = pool[idx[i]];
		sample->pa[i] = sfp_physical(&group[i]);
	}
	sample->probe_ticks = sfp_measure_probe(group);
	return 0;
}

static int sfp_collect_interleaved(struct sfp_sample *baseline,
				   struct sfp_sample *candidate)
{
	unsigned int count = max(baseline_sets, candidate_sets);
	unsigned int i;
	int ret;

	/*
	 * Interleave the two populations and reverse their immediate order on
	 * alternating iterations.  This limits frequency/temperature drift
	 * from being mistaken for a population difference.
	 */
	for (i = 0; i < count; ++i) {
		if (unlikely(kthread_should_stop()))
			return -EINTR;

		if (i & 1U) {
			if (i < candidate_sets) {
				ret = sfp_take_sample(sfp_candidate_pool,
					sfp_candidate_count, false, &candidate[i]);
				if (ret)
					return ret;
			}
			if (i < baseline_sets) {
				ret = sfp_take_sample(sfp_unrelated_pool,
					sfp_unrelated_count, true, &baseline[i]);
				if (ret)
					return ret;
			}
		} else {
			if (i < baseline_sets) {
				ret = sfp_take_sample(sfp_unrelated_pool,
					sfp_unrelated_count, true, &baseline[i]);
				if (ret)
					return ret;
			}
			if (i < candidate_sets) {
				ret = sfp_take_sample(sfp_candidate_pool,
					sfp_candidate_count, false, &candidate[i]);
				if (ret)
					return ret;
			}
		}

		if (!(i & 0x3ffU))
			cond_resched();
	}
	return 0;
}

static int sfp_write_all(struct file *file, const char *buffer, size_t count,
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

static int sfp_write_sample(struct file *file, const char *kind,
			    unsigned int index,
			    const struct sfp_sample *sample, loff_t *position)
{
	char row[SFP_CSV_LINE_SIZE];
	size_t used;
	unsigned int i;

	used = scnprintf(row, sizeof(row), "%s,%u,%llu", kind, index,
			 (unsigned long long)sample->probe_ticks);
	for (i = 0; i < SFP_GROUP_LINES; ++i)
		used += scnprintf(row + used, sizeof(row) - used, ",0x%llx",
				 (unsigned long long)sample->pa[i]);
	used += scnprintf(row + used, sizeof(row) - used, "\n");
	if (used >= sizeof(row))
		return -EOVERFLOW;
	return sfp_write_all(file, row, used, position);
}

static int sfp_write_csv(const struct sfp_sample *baseline,
			 const struct sfp_sample *candidate,
			 u64 baseline_mean, u64 candidate_mean)
{
	struct file *file;
	loff_t position = 0;
	char header[1024];
	size_t used;
	unsigned int i;
	int ret;

	file = filp_open(result_path, O_CREAT | O_TRUNC | O_WRONLY, 0644);
	if (IS_ERR(file)) {
		ret = PTR_ERR(file);
		pr_err(SFP_NAME ": filp_open(%s) failed: %d\n",
		       result_path, ret);
		return ret;
	}

	used = scnprintf(header, sizeof(header),
		"# module,%s\n"
		"# experiment,prime_line8_fill_lines0_7_probe_line8\n"
		"# counter,%s\n"
		"# counter_frequency_hz,%llu\n"
		"# measure_cpu,%d\n"
		"# pool_pages,%u\n"
		"# allocated_bytes,%llu\n"
		"# line_offset,0x%x\n"
		"# target_low23,0x%x\n"
		"# fill_rounds,%u\n"
		"# baseline_count,%u\n"
		"# candidate_count,%u\n"
		"# candidate_pool_size,%u\n"
		"# baseline_mean,%llu\n"
		"# candidate_mean,%llu\n"
		"# mean_delta,%lld\n"
		"type,index,probe_ticks,pa0,pa1,pa2,pa3,pa4,pa5,pa6,pa7,probe_pa\n",
		SFP_NAME,
		sfp_counter_kind == SFP_COUNTER_PMCCNTR ?
			"pmccntr_cycles" : "cntvct_ticks",
		(unsigned long long)(sfp_counter_kind == SFP_COUNTER_CNTVCT ?
			read_sysreg(cntfrq_el0) : 0),
		measure_cpu, pool_pages,
		(unsigned long long)pool_pages << PAGE_SHIFT,
		line_offset, target_low23, fill_rounds,
		baseline_sets, candidate_sets, sfp_candidate_count,
		(unsigned long long)baseline_mean,
		(unsigned long long)candidate_mean,
		(long long)candidate_mean - (long long)baseline_mean);

	ret = sfp_write_all(file, header, used, &position);
	for (i = 0; !ret && i < baseline_sets; ++i)
		ret = sfp_write_sample(file, "baseline", i, &baseline[i],
				       &position);
	for (i = 0; !ret && i < candidate_sets; ++i)
		ret = sfp_write_sample(file, "candidate", i, &candidate[i],
				       &position);

	filp_close(file, NULL);
	if (ret)
		pr_err(SFP_NAME ": writing %s failed: %d\n", result_path, ret);
	else
		pr_info(SFP_NAME ": wrote %u baseline and %u candidate samples to %s\n",
			baseline_sets, candidate_sets, result_path);
	return ret;
}

static int sfp_run_experiment(void)
{
	struct sfp_sample *baseline = NULL;
	struct sfp_sample *candidate = NULL;
	u64 baseline_sum = 0;
	u64 candidate_sum = 0;
	u64 baseline_mean;
	u64 candidate_mean;
	unsigned int i;
	int ret;

	baseline = kvmalloc_array(baseline_sets, sizeof(*baseline), GFP_KERNEL);
	candidate = kvmalloc_array(candidate_sets, sizeof(*candidate), GFP_KERNEL);
	if (!baseline || !candidate) {
		ret = -ENOMEM;
		goto out;
	}

	sfp_enable_counter();
	pr_info(SFP_NAME ": counter=%s cpu=%d allocated=%u candidate_pool=%u unrelated_pool=%u\n",
		sfp_counter_kind == SFP_COUNTER_PMCCNTR ?
			"pmccntr_cycles" : "cntvct_ticks",
		measure_cpu, sfp_allocated_pages, sfp_candidate_count,
		sfp_unrelated_count);

	ret = sfp_collect_interleaved(baseline, candidate);
	if (ret)
		goto out_counter;

	for (i = 0; i < baseline_sets; ++i)
		baseline_sum += baseline[i].probe_ticks;
	for (i = 0; i < candidate_sets; ++i)
		candidate_sum += candidate[i].probe_ticks;
	baseline_mean = div_u64(baseline_sum, baseline_sets);
	candidate_mean = div_u64(candidate_sum, candidate_sets);

	pr_info(SFP_NAME ": baseline_mean=%llu candidate_mean=%llu delta=%lld\n",
		(unsigned long long)baseline_mean,
		(unsigned long long)candidate_mean,
		(long long)candidate_mean - (long long)baseline_mean);
	ret = sfp_write_csv(baseline, candidate, baseline_mean,
			    candidate_mean);

out_counter:
	sfp_restore_counter();
out:
	kvfree(candidate);
	kvfree(baseline);
	return ret;
}

static int sfp_worker_thread(void *unused)
{
	(void)unused;
	sfp_worker_result = sfp_run_experiment();
	complete(&sfp_worker_done);
	return 0;
}

static void sfp_free_pool(void)
{
	unsigned int i;

	for (i = 0; i < sfp_allocated_pages; ++i)
		__free_page(sfp_pages[i]);
	kvfree(sfp_candidate_pool);
	kvfree(sfp_unrelated_pool);
	kvfree(sfp_pages);
	sfp_candidate_pool = NULL;
	sfp_unrelated_pool = NULL;
	sfp_pages = NULL;
	sfp_allocated_pages = 0;
	sfp_candidate_count = 0;
	sfp_unrelated_count = 0;
}

static int sfp_allocate_pool(void)
{
	unsigned int i;

	sfp_pages = kvmalloc_array(pool_pages, sizeof(*sfp_pages), GFP_KERNEL);
	sfp_candidate_pool =
		kvmalloc_array(pool_pages, sizeof(*sfp_candidate_pool),
			       GFP_KERNEL);
	sfp_unrelated_pool =
		kvmalloc_array(pool_pages, sizeof(*sfp_unrelated_pool),
			       GFP_KERNEL);
	if (!sfp_pages || !sfp_candidate_pool || !sfp_unrelated_pool)
		return -ENOMEM;

	for (i = 0; i < pool_pages; ++i) {
		struct page *page = alloc_page(GFP_KERNEL | __GFP_ZERO);
		phys_addr_t pa;
		struct sfp_line *line;

		if (!page)
			return -ENOMEM;
		sfp_pages[sfp_allocated_pages++] = page;
		pa = page_to_phys(page) + line_offset;

		if ((pa & SFP_LOW23_MASK) == target_low23)
			line = &sfp_candidate_pool[sfp_candidate_count++];
		else
			line = &sfp_unrelated_pool[sfp_unrelated_count++];
		line->page = page;
		line->offset = line_offset;
	}
	return 0;
}

static int __init sfp_init(void)
{
	int ret;

	if (PAGE_SIZE != 4096 || !pool_pages ||
	    pool_pages > SFP_POOL_PAGES_MAX ||
	    measure_cpu < 0 || measure_cpu >= nr_cpu_ids ||
	    (line_offset & 0x3fU) || line_offset >= PAGE_SIZE ||
	    target_low23 > SFP_LOW23_MASK ||
	    (target_low23 & (PAGE_SIZE - 1U)) != line_offset ||
	    !fill_rounds || fill_rounds > SFP_FILL_ROUNDS_MAX ||
	    !baseline_sets || baseline_sets > SFP_MAX_SAMPLES ||
	    !candidate_sets || candidate_sets > SFP_MAX_SAMPLES ||
	    !result_path || result_path[0] != '/')
		return -EINVAL;

	cpus_read_lock();
	ret = cpu_online(measure_cpu) ? 0 : -ENODEV;
	cpus_read_unlock();
	if (ret)
		return ret;

	ret = sfp_allocate_pool();
	if (ret)
		goto out;
	if (sfp_candidate_count < SFP_GROUP_LINES ||
	    sfp_unrelated_count < SFP_GROUP_LINES) {
		pr_err(SFP_NAME ": insufficient addresses: candidate=%u unrelated=%u; try another target_low23 or a larger pool\n",
		       sfp_candidate_count, sfp_unrelated_count);
		ret = -ENOSPC;
		goto out;
	}

	reinit_completion(&sfp_worker_done);
	sfp_worker_result = -EINPROGRESS;
	sfp_worker = kthread_create(sfp_worker_thread, NULL, "sfp_measure/%d",
				    measure_cpu);
	if (IS_ERR(sfp_worker)) {
		ret = PTR_ERR(sfp_worker);
		sfp_worker = NULL;
		goto out;
	}
	kthread_bind(sfp_worker, measure_cpu);
	wake_up_process(sfp_worker);
	wait_for_completion(&sfp_worker_done);
	ret = sfp_worker_result;
	sfp_worker = NULL;

out:
	if (ret)
		pr_err(SFP_NAME ": experiment failed: %d\n", ret);
	sfp_free_pool();
	return ret;
}

static void __exit sfp_exit(void)
{
	if (sfp_worker)
		kthread_stop(sfp_worker);
	sfp_free_pool();
	pr_info(SFP_NAME ": unloaded\n");
}

module_init(sfp_init);
module_exit(sfp_exit);

MODULE_DESCRIPTION("RK3588 same-core prime/fill/probe low23 comparison");
MODULE_AUTHOR("HuaijiGao");
MODULE_LICENSE("GPL");
