// SPDX-License-Identifier: GPL-2.0
/*
 * RK3588 nine-address timing sampler.
 *
 * The module allocates independent 4 KiB pages instead of requesting one
 * physically contiguous 512 MiB allocation.  It then compares:
 *
 *   baseline:  nine addresses whose low 23 physical-address bits differ;
 *   candidate: nine addresses whose low 23 physical-address bits are equal.
 *
 * Each set is flushed once, accessed in order for ten passes, and timed as a
 * whole.  The reported value is the counter delta divided by ten.  The test
 * runs in a CPU-bound kthread, with preemption and local interrupts disabled
 * only around each short timed region.
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

#define SFS_NAME                    "rk3588_sf_sampler"
#define SFS_POOL_PAGES_DEFAULT      131072U /* 512 MiB */
#define SFS_POOL_PAGES_MAX          262144U
#define SFS_GROUP_LINES             9U
#define SFS_ACCESS_ROUNDS_DEFAULT   10U
#define SFS_ACCESS_ROUNDS_MAX       1000U
#define SFS_DEFAULT_BASELINE_SETS   1000U
#define SFS_DEFAULT_CANDIDATE_SETS  5000U
#define SFS_MAX_SAMPLES             200000U
#define SFS_LOW23_MASK              ((1U << 23) - 1U)
#define SFS_DEFAULT_OFFSET          0xfc0U
#define SFS_CSV_LINE_SIZE           320U

static unsigned int pool_pages = SFS_POOL_PAGES_DEFAULT;
module_param(pool_pages, uint, 0444);
MODULE_PARM_DESC(pool_pages, "4 KiB pages in private pool (131072 = 512 MiB)");

static int measure_cpu = 6;
module_param(measure_cpu, int, 0444);
MODULE_PARM_DESC(measure_cpu, "CPU on which all timed accesses run");

static unsigned int line_offset = SFS_DEFAULT_OFFSET;
module_param(line_offset, uint, 0444);
MODULE_PARM_DESC(line_offset, "64-byte-aligned address offset inside every page");

static unsigned int target_low23 = 0x7ffc0U;
module_param(target_low23, uint, 0444);
MODULE_PARM_DESC(target_low23, "Low 23 physical-address bits for candidate pages");

static unsigned int access_rounds = SFS_ACCESS_ROUNDS_DEFAULT;
module_param(access_rounds, uint, 0444);
MODULE_PARM_DESC(access_rounds, "Ordered passes over each nine-address set");

static unsigned int baseline_sets = SFS_DEFAULT_BASELINE_SETS;
module_param(baseline_sets, uint, 0444);
MODULE_PARM_DESC(baseline_sets, "Number of random unrelated nine-address sets");

static unsigned int candidate_sets = SFS_DEFAULT_CANDIDATE_SETS;
module_param(candidate_sets, uint, 0444);
MODULE_PARM_DESC(candidate_sets, "Number of random same-low23 nine-address sets");

static char *result_path = "/tmp/rk3588_sf_sampler.csv";
module_param(result_path, charp, 0444);
MODULE_PARM_DESC(result_path, "Absolute CSV output path; parent directory must exist");

enum sfs_counter_kind {
	SFS_COUNTER_PMCCNTR,
	SFS_COUNTER_CNTVCT,
};

struct sfs_line {
	struct page *page;
	u32 offset;
};

struct sfs_sample {
	u64 average_ticks;
	phys_addr_t pa[SFS_GROUP_LINES];
};

static struct page **sfs_pages;
static struct sfs_line *sfs_candidate_pool;
static struct sfs_line *sfs_unrelated_pool;
static unsigned int sfs_allocated_pages;
static unsigned int sfs_candidate_count;
static unsigned int sfs_unrelated_count;

static struct task_struct *sfs_worker;
static DECLARE_COMPLETION(sfs_worker_done);
static int sfs_worker_result;

/* Accessed only by the CPU-bound worker. */
static enum sfs_counter_kind sfs_counter_kind = SFS_COUNTER_CNTVCT;
static u64 sfs_saved_pmcr;
static bool sfs_saved_cycle_enabled;
static bool sfs_pmu_changed;

static __always_inline void *sfs_address(const struct sfs_line *line)
{
	return (char *)page_address(line->page) + line->offset;
}

static __always_inline phys_addr_t sfs_physical(const struct sfs_line *line)
{
	return page_to_phys(line->page) + line->offset;
}

static __always_inline void sfs_touch(const void *address)
{
	(void)READ_ONCE(*(const u64 *)address);
}

static __always_inline void sfs_civac(const void *address)
{
	asm volatile("dc civac, %0" : : "r" (address) : "memory");
}

static __always_inline u64 sfs_read_counter(void)
{
	u64 value;

	isb();
	if (likely(sfs_counter_kind == SFS_COUNTER_PMCCNTR))
		value = read_sysreg(pmccntr_el0);
	else
		value = read_sysreg(cntvct_el0);
	isb();
	return value;
}

static __always_inline u64 sfs_counter_delta(u64 before, u64 after)
{
	if (sfs_counter_kind == SFS_COUNTER_PMCCNTR &&
	    !(read_sysreg(pmcr_el0) & BIT(6)))
		return (u32)after - (u32)before;
	return after - before;
}

static __always_inline u32 sfs_random_below(u32 upper)
{
	if (upper <= 1)
		return 0;
	return reciprocal_scale(get_random_u32(), upper);
}

static void sfs_enable_counter(void)
{
	u64 current_pmcr;
	u64 target_pmcr;
	u64 enabled;
	u64 before;
	u64 after;
	unsigned int i;

	sfs_saved_pmcr = read_sysreg(pmcr_el0);
	enabled = read_sysreg(pmcntenset_el0);
	sfs_saved_cycle_enabled = !!(enabled & BIT(31));

	/* Enable an undivided PMU cycle counter when EL1 permits it. */
	target_pmcr = (sfs_saved_pmcr | BIT(0)) & ~BIT(3);
	current_pmcr = read_sysreg(pmcr_el0);
	if (current_pmcr != target_pmcr) {
		write_sysreg(target_pmcr, pmcr_el0);
		sfs_pmu_changed = true;
	}
	if (!(enabled & BIT(31))) {
		write_sysreg(BIT(31), pmcntenset_el0);
		sfs_pmu_changed = true;
	}
	isb();

	before = read_sysreg(pmccntr_el0);
	for (i = 0; i < 256; ++i)
		cpu_relax();
	after = read_sysreg(pmccntr_el0);
	current_pmcr = read_sysreg(pmcr_el0);
	enabled = read_sysreg(pmcntenset_el0);
	if ((current_pmcr & BIT(0)) && (enabled & BIT(31)) && after != before)
		sfs_counter_kind = SFS_COUNTER_PMCCNTR;
	else
		sfs_counter_kind = SFS_COUNTER_CNTVCT;
}

static void sfs_restore_counter(void)
{
	if (!sfs_pmu_changed)
		return;
	if (!sfs_saved_cycle_enabled)
		write_sysreg(BIT(31), pmcntenclr_el0);
	write_sysreg(sfs_saved_pmcr, pmcr_el0);
	isb();
	sfs_pmu_changed = false;
}

static bool sfs_pick_group(const struct sfs_line *pool, unsigned int pool_size,
			   bool require_distinct_low23, u32 *idx)
{
	unsigned int i;

	if (pool_size < SFS_GROUP_LINES)
		return false;

	for (i = 0; i < SFS_GROUP_LINES; ++i) {
		unsigned int retry;

		for (retry = 0; retry < 4096; ++retry) {
			u32 candidate = sfs_random_below(pool_size);
			u32 candidate_low23 =
				(u32)(sfs_physical(&pool[candidate]) &
				      SFS_LOW23_MASK);
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
					(u32)(sfs_physical(&pool[idx[j]]) &
					      SFS_LOW23_MASK);
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

static u64 sfs_measure_group(const struct sfs_line *group)
{
	unsigned long irq_flags;
	u64 before;
	u64 after;
	unsigned int round;
	unsigned int i;

	/*
	 * Flush only once.  Round 0 is the common cold start; rounds 1..N-1
	 * expose whether repeated accesses to the set remain cached.
	 */
	for (i = 0; i < SFS_GROUP_LINES; ++i)
		sfs_civac(sfs_address(&group[i]));
	dsb(ish);
	isb();

	preempt_disable();
	local_irq_save(irq_flags);
	before = sfs_read_counter();
	for (round = 0; round < access_rounds; ++round) {
		for (i = 0; i < SFS_GROUP_LINES; ++i)
			sfs_touch(sfs_address(&group[i]));
		dsb(ish);
	}
	after = sfs_read_counter();
	local_irq_restore(irq_flags);
	preempt_enable();

	return div_u64(sfs_counter_delta(before, after), access_rounds);
}

static int sfs_collect_samples(const struct sfs_line *pool,
			       unsigned int pool_size,
			       bool require_distinct_low23,
			       struct sfs_sample *samples,
			       unsigned int sample_count)
{
	u32 idx[SFS_GROUP_LINES];
	struct sfs_line group[SFS_GROUP_LINES];
	unsigned int i;

	for (i = 0; i < sample_count; ++i) {
		unsigned int j;

		if (unlikely(kthread_should_stop()))
			return -EINTR;
		if (!sfs_pick_group(pool, pool_size, require_distinct_low23,
				    idx))
			return -ERANGE;
		for (j = 0; j < SFS_GROUP_LINES; ++j) {
			group[j] = pool[idx[j]];
			samples[i].pa[j] = sfs_physical(&group[j]);
		}
		samples[i].average_ticks = sfs_measure_group(group);

		if (!(i & 0x3ffU))
			cond_resched();
	}
	return 0;
}

static int sfs_write_all(struct file *file, const char *buffer, size_t count,
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

static int sfs_write_sample(struct file *file, const char *kind,
			    unsigned int index,
			    const struct sfs_sample *sample, loff_t *position)
{
	char row[SFS_CSV_LINE_SIZE];
	size_t used;
	unsigned int i;

	used = scnprintf(row, sizeof(row), "%s,%u,%llu", kind, index,
			 (unsigned long long)sample->average_ticks);
	for (i = 0; i < SFS_GROUP_LINES; ++i)
		used += scnprintf(row + used, sizeof(row) - used, ",0x%llx",
				 (unsigned long long)sample->pa[i]);
	used += scnprintf(row + used, sizeof(row) - used, "\n");
	if (used >= sizeof(row))
		return -EOVERFLOW;
	return sfs_write_all(file, row, used, position);
}

static int sfs_write_csv(const struct sfs_sample *baseline,
			 const struct sfs_sample *candidate,
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
		pr_err(SFS_NAME ": filp_open(%s) failed: %d\n",
		       result_path, ret);
		return ret;
	}

	used = scnprintf(header, sizeof(header),
		"# module,%s\n"
		"# counter,%s\n"
		"# counter_frequency_hz,%llu\n"
		"# measure_cpu,%d\n"
		"# pool_pages,%u\n"
		"# allocated_bytes,%llu\n"
		"# line_offset,0x%x\n"
		"# target_low23,0x%x\n"
		"# access_rounds,%u\n"
		"# baseline_count,%u\n"
		"# candidate_count,%u\n"
		"# candidate_pool_size,%u\n"
		"# baseline_mean,%llu\n"
		"# candidate_mean,%llu\n"
		"type,index,avg_ticks,pa0,pa1,pa2,pa3,pa4,pa5,pa6,pa7,pa8\n",
		SFS_NAME,
		sfs_counter_kind == SFS_COUNTER_PMCCNTR ?
			"pmccntr_cycles" : "cntvct_ticks",
		(unsigned long long)(sfs_counter_kind == SFS_COUNTER_CNTVCT ?
			read_sysreg(cntfrq_el0) : 0),
		measure_cpu, pool_pages,
		(unsigned long long)pool_pages << PAGE_SHIFT,
		line_offset, target_low23, access_rounds,
		baseline_sets, candidate_sets, sfs_candidate_count,
		(unsigned long long)baseline_mean,
		(unsigned long long)candidate_mean);

	ret = sfs_write_all(file, header, used, &position);
	for (i = 0; !ret && i < baseline_sets; ++i)
		ret = sfs_write_sample(file, "baseline", i, &baseline[i],
				       &position);
	for (i = 0; !ret && i < candidate_sets; ++i)
		ret = sfs_write_sample(file, "candidate", i, &candidate[i],
				       &position);

	filp_close(file, NULL);
	if (ret)
		pr_err(SFS_NAME ": writing %s failed: %d\n", result_path, ret);
	else
		pr_info(SFS_NAME ": wrote %u baseline and %u candidate sets to %s\n",
			baseline_sets, candidate_sets, result_path);
	return ret;
}

static int sfs_run_experiment(void)
{
	struct sfs_sample *baseline = NULL;
	struct sfs_sample *candidate = NULL;
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

	sfs_enable_counter();
	pr_info(SFS_NAME ": counter=%s cpu=%d allocated=%u pages candidate_pool=%u unrelated_pool=%u\n",
		sfs_counter_kind == SFS_COUNTER_PMCCNTR ?
			"pmccntr_cycles" : "cntvct_ticks",
		measure_cpu, sfs_allocated_pages, sfs_candidate_count,
		sfs_unrelated_count);

	ret = sfs_collect_samples(sfs_unrelated_pool, sfs_unrelated_count,
				  true, baseline, baseline_sets);
	if (ret)
		goto out_counter;
	ret = sfs_collect_samples(sfs_candidate_pool, sfs_candidate_count,
				  false, candidate, candidate_sets);
	if (ret)
		goto out_counter;

	for (i = 0; i < baseline_sets; ++i)
		baseline_sum += baseline[i].average_ticks;
	for (i = 0; i < candidate_sets; ++i)
		candidate_sum += candidate[i].average_ticks;
	baseline_mean = div_u64(baseline_sum, baseline_sets);
	candidate_mean = div_u64(candidate_sum, candidate_sets);

	pr_info(SFS_NAME ": baseline_mean=%llu candidate_mean=%llu delta=%lld\n",
		(unsigned long long)baseline_mean,
		(unsigned long long)candidate_mean,
		(long long)candidate_mean - (long long)baseline_mean);
	ret = sfs_write_csv(baseline, candidate, baseline_mean,
			    candidate_mean);

out_counter:
	sfs_restore_counter();
out:
	kvfree(candidate);
	kvfree(baseline);
	return ret;
}

static int sfs_worker_thread(void *unused)
{
	(void)unused;
	sfs_worker_result = sfs_run_experiment();
	complete(&sfs_worker_done);
	return 0;
}

static void sfs_free_pool(void)
{
	unsigned int i;

	for (i = 0; i < sfs_allocated_pages; ++i)
		__free_page(sfs_pages[i]);
	kvfree(sfs_candidate_pool);
	kvfree(sfs_unrelated_pool);
	kvfree(sfs_pages);
	sfs_candidate_pool = NULL;
	sfs_unrelated_pool = NULL;
	sfs_pages = NULL;
	sfs_allocated_pages = 0;
	sfs_candidate_count = 0;
	sfs_unrelated_count = 0;
}

static int sfs_allocate_pool(void)
{
	unsigned int i;

	sfs_pages = kvmalloc_array(pool_pages, sizeof(*sfs_pages), GFP_KERNEL);
	sfs_candidate_pool =
		kvmalloc_array(pool_pages, sizeof(*sfs_candidate_pool),
			       GFP_KERNEL);
	sfs_unrelated_pool =
		kvmalloc_array(pool_pages, sizeof(*sfs_unrelated_pool),
			       GFP_KERNEL);
	if (!sfs_pages || !sfs_candidate_pool || !sfs_unrelated_pool)
		return -ENOMEM;

	for (i = 0; i < pool_pages; ++i) {
		struct page *page = alloc_page(GFP_KERNEL | __GFP_ZERO);
		phys_addr_t pa;
		struct sfs_line *line;

		if (!page)
			return -ENOMEM;
		sfs_pages[sfs_allocated_pages++] = page;
		pa = page_to_phys(page) + line_offset;

		if ((pa & SFS_LOW23_MASK) == target_low23)
			line = &sfs_candidate_pool[sfs_candidate_count++];
		else
			line = &sfs_unrelated_pool[sfs_unrelated_count++];
		line->page = page;
		line->offset = line_offset;
	}
	return 0;
}

static int __init sfs_init(void)
{
	int ret;

	if (PAGE_SIZE != 4096 || !pool_pages ||
	    pool_pages > SFS_POOL_PAGES_MAX ||
	    measure_cpu < 0 || measure_cpu >= nr_cpu_ids ||
	    (line_offset & 0x3fU) || line_offset >= PAGE_SIZE ||
	    target_low23 > SFS_LOW23_MASK ||
	    (target_low23 & (PAGE_SIZE - 1U)) != line_offset ||
	    !access_rounds || access_rounds > SFS_ACCESS_ROUNDS_MAX ||
	    !baseline_sets || baseline_sets > SFS_MAX_SAMPLES ||
	    !candidate_sets || candidate_sets > SFS_MAX_SAMPLES ||
	    !result_path || result_path[0] != '/')
		return -EINVAL;

	cpus_read_lock();
	ret = cpu_online(measure_cpu) ? 0 : -ENODEV;
	cpus_read_unlock();
	if (ret)
		return ret;

	ret = sfs_allocate_pool();
	if (ret)
		goto out;
	if (sfs_candidate_count < SFS_GROUP_LINES ||
	    sfs_unrelated_count < SFS_GROUP_LINES) {
		pr_err(SFS_NAME ": insufficient addresses: candidate=%u unrelated=%u; try another target_low23 or a larger pool\n",
		       sfs_candidate_count, sfs_unrelated_count);
		ret = -ENOSPC;
		goto out;
	}

	reinit_completion(&sfs_worker_done);
	sfs_worker_result = -EINPROGRESS;
	sfs_worker = kthread_create(sfs_worker_thread, NULL, "sfs_measure/%d",
				    measure_cpu);
	if (IS_ERR(sfs_worker)) {
		ret = PTR_ERR(sfs_worker);
		sfs_worker = NULL;
		goto out;
	}
	kthread_bind(sfs_worker, measure_cpu);
	wake_up_process(sfs_worker);
	wait_for_completion(&sfs_worker_done);
	ret = sfs_worker_result;
	/*
	 * The worker has exited after completion.  No timing data remains in
	 * kernel memory; the CSV is the experiment artifact.
	 */
	sfs_worker = NULL;

out:
	if (ret)
		pr_err(SFS_NAME ": experiment failed: %d\n", ret);
	sfs_free_pool();
	return ret;
}

static void __exit sfs_exit(void)
{
	if (sfs_worker)
		kthread_stop(sfs_worker);
	sfs_free_pool();
	pr_info(SFS_NAME ": unloaded\n");
}

module_init(sfs_init);
module_exit(sfs_exit);

MODULE_DESCRIPTION("RK3588 same-low23 nine-address timing distribution sampler");
MODULE_AUTHOR("HuaijiGao");
MODULE_LICENSE("GPL");
