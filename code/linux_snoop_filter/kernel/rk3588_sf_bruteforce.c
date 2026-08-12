// SPDX-License-Identifier: GPL-2.0
/*
 * Deterministic RK3588 snoop-filter conflict experiment.
 *
 * Stage 1 (single core):
 * 1) Try to allocate 512 MiB private page pool.
 * 2) Keep only pages with PA[23:6] fixed to 1.
 * 3) Enumerate C(64,5) combinations over PA[29:24] buckets.
 * 4) Build one 9-line group from 4 fixed anchor lines + 5 combination lines.
 * 5) Warm all 9 lines once, then repeatedly access first 8 lines for 20 rounds,
 *    then measure line 9 latency as t2. If t2 rises significantly, keep as
 *    stage-1 candidate.
 *
 * Stage 2 (cross-core):
 * - probe_cpu measures t1/t2 on line 9.
 * - stimulus_cpu performs the same repeated first-8-line disturbance.
 * - Significant and repeatable t2 increase confirms cross-core SF effect.
 */
#include <linux/atomic.h>
#include <linux/bitops.h>
#include <linux/completion.h>
#include <linux/cpu.h>
#include <linux/kthread.h>
#include <linux/math64.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/sched.h>
#include <linux/slab.h>
#include <linux/sort.h>
#include <linux/string.h>
#include <linux/vmalloc.h>

#include <asm/barrier.h>
#include <asm/sysreg.h>

#define SFB_NAME                      "rk3588_sf_bruteforce"
#define SFB_LINE_SIZE                 64U
#define SFB_GROUP_LINES               9U
#define SFB_EVICTOR_LINES             8U
#define SFB_ANCHOR_LINES              4U
#define SFB_COMBINATION_PICK          5U
#define SFB_BUCKET_BITS               6U
#define SFB_BUCKET_COUNT              (1U << SFB_BUCKET_BITS)
#define SFB_BUCKET_SHIFT              12U
#define SFB_TOTAL_COMBINATIONS        7624512ULL /* C(64,5) */
#define SFB_FIXED_PFN_LOW_MASK        0xfffU
#define SFB_FIXED_LINE_OFFSET         0xfc0U
#define SFB_DEFAULT_POOL_PAGES        131072U /* 512 MiB */
#define SFB_MAX_POOL_PAGES            262144U
#define SFB_MAX_REPETITIONS           10000U
#define SFB_MAX_CANDIDATES            1024U

static unsigned int pool_pages = SFB_DEFAULT_POOL_PAGES;
module_param(pool_pages, uint, 0444);
MODULE_PARM_DESC(pool_pages, "4 KiB pages in private pool (131072 = 512 MiB)");

static int probe_cpu = 6;
module_param(probe_cpu, int, 0444);
MODULE_PARM_DESC(probe_cpu, "CPU for stage-1 search and stage-2 t1/t2 probe");

static int stimulus_cpu = 7;
module_param(stimulus_cpu, int, 0444);
MODULE_PARM_DESC(stimulus_cpu, "CPU that accesses lines 1..8 in stage 2");

static unsigned int search_repetitions = 20;
module_param(search_repetitions, uint, 0444);
MODULE_PARM_DESC(search_repetitions, "Per-group stage-1 trials");

static unsigned int fill_rounds = 20;
module_param(fill_rounds, uint, 0444);
MODULE_PARM_DESC(fill_rounds, "Repeated passes over first eight lines");

static unsigned int stage1_min_delta;
module_param(stage1_min_delta, uint, 0444);
MODULE_PARM_DESC(stage1_min_delta, "Minimum stage-1 median (t2-t1) cycles; 0 derives from calibration");

static unsigned long long combination_start;
module_param(combination_start, ullong, 0444);
MODULE_PARM_DESC(combination_start, "First lexicographic C(64,5) rank to enumerate");

static unsigned long long combination_count;
module_param(combination_count, ullong, 0444);
MODULE_PARM_DESC(combination_count, "Ranks to enumerate; 0 means all remaining");

static unsigned long long progress_interval = 100000ULL;
module_param(progress_interval, ullong, 0444);
MODULE_PARM_DESC(progress_interval, "Stage-1 progress log interval in ranks");

static bool stop_on_candidate = true;
module_param(stop_on_candidate, bool, 0444);
MODULE_PARM_DESC(stop_on_candidate, "Stop stage 1 when first candidate is found");

static unsigned int validation_repetitions = 200;
module_param(validation_repetitions, uint, 0444);
MODULE_PARM_DESC(validation_repetitions, "Paired t1/t2 trials per candidate");

static unsigned int calibration_repetitions = 300;
module_param(calibration_repetitions, uint, 0444);
MODULE_PARM_DESC(calibration_repetitions, "Cached and dc-civac calibration trials");

static unsigned int control_percent = 20;
module_param(control_percent, uint, 0444);
MODULE_PARM_DESC(control_percent, "Maximum slow t1 percentage in stage 2");

static unsigned int validation_percent = 40;
module_param(validation_percent, uint, 0444);
MODULE_PARM_DESC(validation_percent, "Minimum slow t2 percentage for SF confirmation");

static unsigned int validation_ratio_percent = 150;
module_param(validation_ratio_percent, uint, 0444);
MODULE_PARM_DESC(validation_ratio_percent, "Minimum median t2/t1 ratio in percent");

static unsigned int validation_min_delta;
module_param(validation_min_delta, uint, 0444);
MODULE_PARM_DESC(validation_min_delta, "Minimum median t2-t1 cycles; 0 derives from calibration");

static unsigned int max_candidates = 64;
module_param(max_candidates, uint, 0444);
MODULE_PARM_DESC(max_candidates, "Maximum stage-1 candidates retained");

static unsigned int max_results = 8;
module_param(max_results, uint, 0444);
MODULE_PARM_DESC(max_results, "Stop after this many stage-2 confirmations");

static unsigned int post_access_relax;
module_param(post_access_relax, uint, 0444);
MODULE_PARM_DESC(post_access_relax, "cpu_relax iterations after CPU7 finishes");

enum sfb_counter_kind {
	SFB_COUNTER_PMCCNTR,
	SFB_COUNTER_CNTVCT,
};

struct sfb_line {
	struct page *page;
	u32 offset;
};

struct sfb_group {
	struct sfb_line line[SFB_GROUP_LINES];
	u8 combo[SFB_COMBINATION_PICK];
	u64 combination_rank;
	u64 stage1_t1;
	u64 stage1_t2;
	u64 stage1_cycles;
};

static struct page **sfb_pages;
static struct page **sfb_bucket_pages;
static u32 *sfb_bucket_counts;
static u32 *sfb_bucket_starts;
static struct sfb_group *sfb_candidates;

static struct sfb_line sfb_anchor_lines[SFB_ANCHOR_LINES];
static u8 sfb_anchor_buckets[SFB_ANCHOR_LINES];

static unsigned int sfb_allocated_pages;
static unsigned int sfb_qualified_pages;
static unsigned int sfb_candidate_count;

static struct task_struct *sfb_probe_task;
static struct task_struct *sfb_stimulus_task;

static DECLARE_COMPLETION(sfb_prepare_request);
static DECLARE_COMPLETION(sfb_prepare_done);
static DECLARE_COMPLETION(sfb_access_request);
static DECLARE_COMPLETION(sfb_access_done);
static struct sfb_line sfb_validation_evictors[SFB_EVICTOR_LINES];
static atomic_t sfb_shutdown = ATOMIC_INIT(0);

/* Only probe kthread updates PMU bookkeeping. */
static enum sfb_counter_kind sfb_counter_kind = SFB_COUNTER_CNTVCT;
static u64 sfb_saved_pmcr;
static bool sfb_saved_cycle_enabled;
static bool sfb_pmu_changed;

static __always_inline void *sfb_address(const struct sfb_line *line)
{
	return (char *)page_address(line->page) + line->offset;
}

static __always_inline void sfb_touch(const void *address)
{
	(void)READ_ONCE(*(const u64 *)address);
}

static __always_inline void sfb_civac(const void *address)
{
	asm volatile("dc civac, %0" : : "r" (address) : "memory");
}

static __always_inline u64 sfb_read_counter(void)
{
	u64 value;

	isb();
	if (likely(sfb_counter_kind == SFB_COUNTER_PMCCNTR))
		value = read_sysreg(pmccntr_el0);
	else
		value = read_sysreg(cntvct_el0);
	isb();
	return value;
}

static __always_inline u64 sfb_counter_delta(u64 before, u64 after)
{
	if (sfb_counter_kind == SFB_COUNTER_PMCCNTR &&
	    !(read_sysreg(pmcr_el0) & BIT(6)))
		return (u32)after - (u32)before;
	return after - before;
}

static __always_inline u64 sfb_reload(const void *address)
{
	u64 before;
	u64 after;

	before = sfb_read_counter();
	sfb_touch(address);
	dsb(ish);
	after = sfb_read_counter();
	return sfb_counter_delta(before, after);
}

static int sfb_u64_compare(const void *left, const void *right)
{
	const u64 a = *(const u64 *)left;
	const u64 b = *(const u64 *)right;

	return (a > b) - (a < b);
}

static u64 sfb_median(u64 *samples, unsigned int count)
{
	sort(samples, count, sizeof(*samples), sfb_u64_compare, NULL);
	return samples[count / 2];
}

static int sfb_page_phys_compare(const void *left, const void *right)
{
	struct page * const *a = left;
	struct page * const *b = right;
	phys_addr_t pa = page_to_phys(*a);
	phys_addr_t pb = page_to_phys(*b);

	return (pa > pb) - (pa < pb);
}

static void sfb_enable_counter(void)
{
	u64 current_pmcr;
	u64 target_pmcr;
	u64 enabled;
	u64 before;
	u64 after;
	unsigned int i;

	sfb_saved_pmcr = read_sysreg(pmcr_el0);
	enabled = read_sysreg(pmcntenset_el0);
	sfb_saved_cycle_enabled = !!(enabled & BIT(31));

	target_pmcr = (sfb_saved_pmcr | BIT(0)) & ~BIT(3);
	current_pmcr = read_sysreg(pmcr_el0);
	if (current_pmcr != target_pmcr) {
		write_sysreg(target_pmcr, pmcr_el0);
		sfb_pmu_changed = true;
	}
	if (!(enabled & BIT(31))) {
		write_sysreg(BIT(31), pmcntenset_el0);
		sfb_pmu_changed = true;
	}
	isb();

	before = read_sysreg(pmccntr_el0);
	for (i = 0; i < 256; ++i)
		cpu_relax();
	after = read_sysreg(pmccntr_el0);
	current_pmcr = read_sysreg(pmcr_el0);
	enabled = read_sysreg(pmcntenset_el0);
	if ((current_pmcr & BIT(0)) && (enabled & BIT(31)) && after != before)
		sfb_counter_kind = SFB_COUNTER_PMCCNTR;
	else
		sfb_counter_kind = SFB_COUNTER_CNTVCT;
}

static void sfb_restore_counter(void)
{
	if (!sfb_pmu_changed)
		return;
	if (!sfb_saved_cycle_enabled)
		write_sysreg(BIT(31), pmcntenclr_el0);
	write_sysreg(sfb_saved_pmcr, pmcr_el0);
	isb();
	sfb_pmu_changed = false;
}

static int sfb_calibrate(const struct sfb_line *line, u64 *hit_median,
			 u64 *miss_median, u64 *threshold)
{
	void *address = sfb_address(line);
	u64 *hit;
	u64 *miss;
	unsigned int i;
	int ret = 0;

	hit = kvmalloc_array(calibration_repetitions, sizeof(*hit), GFP_KERNEL);
	miss = kvmalloc_array(calibration_repetitions, sizeof(*miss), GFP_KERNEL);
	if (!hit || !miss) {
		ret = -ENOMEM;
		goto out;
	}

	for (i = 0; i < calibration_repetitions; ++i) {
		sfb_touch(address);
		dsb(ish);
		hit[i] = sfb_reload(address);

		sfb_civac(address);
		dsb(ish);
		isb();
		miss[i] = sfb_reload(address);
	}
	*hit_median = sfb_median(hit, calibration_repetitions);
	*miss_median = sfb_median(miss, calibration_repetitions);
	if (*miss_median <= *hit_median) {
		ret = -ERANGE;
		goto out;
	}
	*threshold = *hit_median + (*miss_median - *hit_median) / 2;
out:
	kvfree(miss);
	kvfree(hit);
	return ret;
}

static u64 sfb_choose(unsigned int n, unsigned int k)
{
	unsigned int i;
	u64 result = 1;

	if (k > n)
		return 0;
	if (k > n - k)
		k = n - k;
	for (i = 1; i <= k; ++i)
		result = div_u64(result * (n - k + i), i);
	return result;
}

static int sfb_unrank_combination(u64 rank, u8 *combination)
{
	unsigned int position;
	unsigned int next = 0;

	if (rank >= SFB_TOTAL_COMBINATIONS)
		return -ERANGE;

	for (position = 0; position < SFB_COMBINATION_PICK; ++position) {
		unsigned int value;
		unsigned int remaining = SFB_COMBINATION_PICK - position - 1U;

		for (value = next;
		     value <= SFB_BUCKET_COUNT - remaining - 1U; ++value) {
			u64 block = sfb_choose(SFB_BUCKET_COUNT - value - 1U,
					       remaining);

			if (rank < block) {
				combination[position] = value;
				next = value + 1U;
				break;
			}
			rank -= block;
		}
	}
	return 0;
}

static bool sfb_is_anchor_bucket(u8 bucket)
{
	unsigned int i;

	for (i = 0; i < SFB_ANCHOR_LINES; ++i)
		if (sfb_anchor_buckets[i] == bucket)
			return true;
	return false;
}

static bool sfb_combo_valid(const u8 *combination)
{
	unsigned int i;

	for (i = 0; i < SFB_COMBINATION_PICK; ++i) {
		u8 bucket = combination[i];

		if (!sfb_bucket_counts[bucket])
			return false;
		if (sfb_is_anchor_bucket(bucket))
			return false;
	}
	return true;
}

static void sfb_make_combination_group(struct sfb_group *group,
				       const u8 *combination, u64 rank)
{
	unsigned int i;

	group->combination_rank = rank;
	for (i = 0; i < SFB_ANCHOR_LINES; ++i)
		group->line[i] = sfb_anchor_lines[i];

	for (i = 0; i < SFB_COMBINATION_PICK; ++i) {
		u8 bucket = combination[i];
		u32 start = sfb_bucket_starts[bucket];

		group->combo[i] = bucket;
		group->line[SFB_ANCHOR_LINES + i].page = sfb_bucket_pages[start];
		group->line[SFB_ANCHOR_LINES + i].offset = SFB_FIXED_LINE_OFFSET;
	}
}

static u64 sfb_stage1_group_latency(struct sfb_group *group,
				    u64 *t1_samples, u64 *t2_samples)
{
	unsigned int rep;
	unsigned int i;
	unsigned int round;
	void *probe = sfb_address(&group->line[SFB_GROUP_LINES - 1U]);

	for (rep = 0; rep < search_repetitions; ++rep) {
		for (i = 0; i < SFB_GROUP_LINES; ++i)
			sfb_civac(sfb_address(&group->line[i]));
		dsb(ish);
		isb();

		for (i = 0; i < SFB_GROUP_LINES; ++i)
			sfb_touch(sfb_address(&group->line[i]));
		dsb(ish);

		t1_samples[rep] = sfb_reload(probe);
		for (round = 0; round < fill_rounds; ++round) {
			for (i = 0; i < SFB_EVICTOR_LINES; ++i)
				sfb_touch(sfb_address(&group->line[i]));
			dsb(ish);
		}
		t2_samples[rep] = sfb_reload(probe);
	}
	group->stage1_t1 = sfb_median(t1_samples, search_repetitions);
	group->stage1_t2 = sfb_median(t2_samples, search_repetitions);
	if (group->stage1_t2 <= group->stage1_t1)
		return 0;
	return group->stage1_t2 - group->stage1_t1;
}

static void sfb_publish_validation_group(const struct sfb_group *group)
{
	unsigned int i;

	for (i = 0; i < SFB_EVICTOR_LINES; ++i) {
		WRITE_ONCE(sfb_validation_evictors[i].page, group->line[i].page);
		WRITE_ONCE(sfb_validation_evictors[i].offset, group->line[i].offset);
	}
	/* completion provides release ordering for the worker. */
}

static int sfb_stimulus_thread(void *unused)
{
	unsigned int i;
	unsigned int round;

	(void)unused;
	while (!kthread_should_stop() && !atomic_read(&sfb_shutdown)) {
		struct sfb_line line;

		wait_for_completion(&sfb_prepare_request);
		if (kthread_should_stop() || atomic_read(&sfb_shutdown))
			break;

		for (i = 0; i < SFB_EVICTOR_LINES; ++i) {
			line.page = READ_ONCE(sfb_validation_evictors[i].page);
			line.offset = READ_ONCE(sfb_validation_evictors[i].offset);
			sfb_civac(sfb_address(&line));
		}
		dsb(ish);
		isb();
		complete(&sfb_prepare_done);

		wait_for_completion(&sfb_access_request);
		if (kthread_should_stop() || atomic_read(&sfb_shutdown))
			break;

		for (round = 0; round < fill_rounds; ++round) {
			for (i = 0; i < SFB_EVICTOR_LINES; ++i) {
				line.page = READ_ONCE(sfb_validation_evictors[i].page);
				line.offset = READ_ONCE(sfb_validation_evictors[i].offset);
				sfb_touch(sfb_address(&line));
			}
			dsb(ish);
		}
		complete(&sfb_access_done);
	}
	return 0;
}

static int sfb_validate_group(const struct sfb_group *group, u64 threshold,
			      u64 min_delta, u64 *median_t1,
			      u64 *median_t2, unsigned int *slow_t1_percent,
			      unsigned int *slow_t2_percent)
{
	void *probe = sfb_address(&group->line[SFB_GROUP_LINES - 1U]);
	u64 *t1;
	u64 *t2;
	unsigned int slow_t1 = 0;
	unsigned int slow_t2 = 0;
	unsigned int rep;
	unsigned int wait;
	bool ratio_ok;
	bool delta_ok;
	bool percent_ok;
	int ret = 0;

	t1 = kvmalloc_array(validation_repetitions, sizeof(*t1), GFP_KERNEL);
	t2 = kvmalloc_array(validation_repetitions, sizeof(*t2), GFP_KERNEL);
	if (!t1 || !t2) {
		ret = -ENOMEM;
		goto out;
	}

	sfb_publish_validation_group(group);
	for (rep = 0; rep < validation_repetitions; ++rep) {
		if (unlikely(kthread_should_stop())) {
			ret = -EINTR;
			goto out;
		}

		complete(&sfb_prepare_request);
		wait_for_completion(&sfb_prepare_done);

		sfb_civac(probe);
		dsb(ish);
		isb();
		sfb_touch(probe);
		dsb(ish);
		t1[rep] = sfb_reload(probe);
		if (t1[rep] >= threshold)
			slow_t1++;

		complete(&sfb_access_request);
		wait_for_completion(&sfb_access_done);
		for (wait = 0; wait < post_access_relax; ++wait)
			cpu_relax();
		t2[rep] = sfb_reload(probe);
		if (t2[rep] >= threshold)
			slow_t2++;
	}

	*slow_t1_percent = DIV_ROUND_CLOSEST(slow_t1 * 100U,
					     validation_repetitions);
	*slow_t2_percent = DIV_ROUND_CLOSEST(slow_t2 * 100U,
					     validation_repetitions);
	*median_t1 = sfb_median(t1, validation_repetitions);
	*median_t2 = sfb_median(t2, validation_repetitions);

	ratio_ok = *median_t2 * 100ULL >=
		   *median_t1 * (u64)validation_ratio_percent;
	delta_ok = *median_t2 >= *median_t1 + min_delta;
	percent_ok = *slow_t1_percent <= control_percent &&
		     *slow_t2_percent >= validation_percent;
	ret = ratio_ok && delta_ok && percent_ok ? 1 : 0;
out:
	kvfree(t2);
	kvfree(t1);
	return ret;
}

static void sfb_print_group(const char *tag, unsigned int number,
			    const struct sfb_group *group)
{
	unsigned int i;

	pr_info(SFB_NAME ": %s[%u] rank=%llu combo=[%u,%u,%u,%u,%u] stage1_t1=%llu stage1_t2=%llu stage1_delta=%llu\n",
		tag, number,
		(unsigned long long)group->combination_rank,
		group->combo[0], group->combo[1], group->combo[2],
		group->combo[3], group->combo[4],
		(unsigned long long)group->stage1_t1,
		(unsigned long long)group->stage1_t2,
		(unsigned long long)group->stage1_cycles);

	for (i = 0; i < SFB_GROUP_LINES; ++i) {
		phys_addr_t pa = page_to_phys(group->line[i].page) +
				 group->line[i].offset;

		pr_info(SFB_NAME ": %s[%u] line[%u]_pa=%pa page_offset=0x%x\n",
			tag, number, i, &pa, group->line[i].offset);
	}
}

static int sfb_probe_thread(void *unused)
{
	u64 hit_median;
	u64 miss_median;
	u64 threshold;
	u64 stage1_delta;
	u64 validation_delta;
	u64 rank_begin;
	u64 rank_end;
	u64 rank;
	u64 scanned = 0;
	u64 valid_groups = 0;
	u64 *t1_samples = NULL;
	u64 *t2_samples = NULL;
	u8 combination[SFB_COMBINATION_PICK];
	unsigned int candidate;
	unsigned int confirmed = 0;
	struct sfb_line calibration_line;
	int ret;

	(void)unused;
	sfb_enable_counter();
	pr_info(SFB_NAME ": counter=%s%s\n",
		sfb_counter_kind == SFB_COUNTER_PMCCNTR ?
		"pmccntr" : "cntvct",
		sfb_counter_kind == SFB_COUNTER_CNTVCT ?
		" (coarse fallback; do not trust small deltas)" : "");

	calibration_line = sfb_anchor_lines[0];
	ret = sfb_calibrate(&calibration_line, &hit_median, &miss_median,
			    &threshold);
	if (ret) {
		pr_err(SFB_NAME ": calibration failed: %d\n", ret);
		goto out_restore;
	}

	stage1_delta = stage1_min_delta;
	if (!stage1_delta)
		stage1_delta = max_t(u64, 1, (miss_median - hit_median) / 2);
	validation_delta = validation_min_delta;
	if (!validation_delta)
		validation_delta = max_t(u64, 1, (miss_median - hit_median) / 4);

	pr_info(SFB_NAME ": calibration hit=%llu miss=%llu threshold=%llu stage1_delta=%llu validation_delta=%llu\n",
		(unsigned long long)hit_median,
		(unsigned long long)miss_median,
		(unsigned long long)threshold,
		(unsigned long long)stage1_delta,
		(unsigned long long)validation_delta);

	t1_samples = kvmalloc_array(search_repetitions, sizeof(*t1_samples),
				    GFP_KERNEL);
	t2_samples = kvmalloc_array(search_repetitions, sizeof(*t2_samples),
				    GFP_KERNEL);
	if (!t1_samples || !t2_samples) {
		ret = -ENOMEM;
		goto out_stage1;
	}

	rank_begin = combination_start;
	if (rank_begin >= SFB_TOTAL_COMBINATIONS) {
		ret = -ERANGE;
		pr_err(SFB_NAME ": combination_start=%llu out of range [0,%llu)\n",
			(unsigned long long)rank_begin,
			(unsigned long long)SFB_TOTAL_COMBINATIONS);
		goto out_stage1;
	}
	rank_end = SFB_TOTAL_COMBINATIONS;
	if (combination_count && combination_count < rank_end - rank_begin)
		rank_end = rank_begin + combination_count;

	pr_info(SFB_NAME ": stage1 begin: fixed PA[23:6]=1, C(64,5) rank [%llu,%llu), fill_rounds=%u\n",
		(unsigned long long)rank_begin,
		(unsigned long long)rank_end,
		fill_rounds);

	for (rank = rank_begin; rank < rank_end; ++rank) {
		struct sfb_group group;

		if (unlikely(kthread_should_stop())) {
			ret = -EINTR;
			goto out_stage1;
		}

		scanned++;
		if (progress_interval && !(scanned % progress_interval))
			pr_info(SFB_NAME ": stage1 progress scanned=%llu valid=%llu candidates=%u current_rank=%llu\n",
				(unsigned long long)scanned,
				(unsigned long long)valid_groups,
				sfb_candidate_count,
				(unsigned long long)rank);

		ret = sfb_unrank_combination(rank, combination);
		if (ret)
			goto out_stage1;
		if (!sfb_combo_valid(combination))
			continue;

		memset(&group, 0, sizeof(group));
		sfb_make_combination_group(&group, combination, rank);
		group.stage1_cycles =
			sfb_stage1_group_latency(&group, t1_samples, t2_samples);
		valid_groups++;

		if (group.stage1_cycles >= stage1_delta) {
			sfb_candidates[sfb_candidate_count] = group;
			sfb_print_group("CANDIDATE", sfb_candidate_count, &group);
			sfb_candidate_count++;
			if (stop_on_candidate || sfb_candidate_count == max_candidates)
				break;
		}
		cond_resched();
	}

	pr_info(SFB_NAME ": stage1 complete: scanned=%llu valid=%llu candidates=%u\n",
		(unsigned long long)scanned,
		(unsigned long long)valid_groups,
		sfb_candidate_count);

	pr_info(SFB_NAME ": stage2 begin: probe_cpu=%d stimulus_cpu=%d repetitions=%u\n",
		probe_cpu, stimulus_cpu, validation_repetitions);
	for (candidate = 0; candidate < sfb_candidate_count; ++candidate) {
		u64 median_t1 = 0;
		u64 median_t2 = 0;
		unsigned int slow_t1 = 0;
		unsigned int slow_t2 = 0;

		ret = sfb_validate_group(&sfb_candidates[candidate], threshold,
					 validation_delta,
					 &median_t1, &median_t2,
					 &slow_t1, &slow_t2);
		if (ret < 0)
			goto out_stage1;

		pr_info(SFB_NAME ": stage2 candidate=%u t1_median=%llu t2_median=%llu slow_t1=%u%% slow_t2=%u%% result=%s\n",
			candidate,
			(unsigned long long)median_t1,
			(unsigned long long)median_t2,
			slow_t1, slow_t2,
			ret ? "SF_CONFIRMED" : "REJECTED");
		if (ret) {
			sfb_print_group("SF_CONFIRMED", confirmed,
					&sfb_candidates[candidate]);
			confirmed++;
			if (confirmed == max_results)
				break;
		}
	}
	pr_info(SFB_NAME ": finished: candidates=%u sf_confirmed=%u\n",
		sfb_candidate_count, confirmed);
	ret = 0;

out_stage1:
	kvfree(t2_samples);
	kvfree(t1_samples);
out_restore:
	if (ret && ret != -EINTR)
		pr_err(SFB_NAME ": experiment stopped: %d\n", ret);
	sfb_restore_counter();
	return ret;
}

static int sfb_pick_anchor_lines(void)
{
	unsigned int i;
	unsigned int picked = 0;

	for (i = 0; i < SFB_BUCKET_COUNT && picked < SFB_ANCHOR_LINES; ++i) {
		u32 start;

		if (!sfb_bucket_counts[i])
			continue;
		start = sfb_bucket_starts[i];
		sfb_anchor_buckets[picked] = i;
		sfb_anchor_lines[picked].page = sfb_bucket_pages[start];
		sfb_anchor_lines[picked].offset = SFB_FIXED_LINE_OFFSET;
		picked++;
	}
	if (picked != SFB_ANCHOR_LINES)
		return -ENOSPC;
	return 0;
}

static void sfb_free_pool(void)
{
	unsigned int i;

	for (i = 0; i < sfb_allocated_pages; ++i)
		__free_page(sfb_pages[i]);
	kvfree(sfb_candidates);
	kvfree(sfb_bucket_starts);
	kvfree(sfb_bucket_counts);
	kvfree(sfb_bucket_pages);
	kvfree(sfb_pages);
	sfb_candidates = NULL;
	sfb_bucket_starts = NULL;
	sfb_bucket_counts = NULL;
	sfb_bucket_pages = NULL;
	sfb_pages = NULL;
	sfb_allocated_pages = 0;
	sfb_qualified_pages = 0;
	sfb_candidate_count = 0;
}

static int sfb_allocate_pool(void)
{
	u32 cursor[SFB_BUCKET_COUNT];
	unsigned int i;

	sfb_pages = kvmalloc_array(pool_pages, sizeof(*sfb_pages), GFP_KERNEL);
	sfb_bucket_counts = kvcalloc(SFB_BUCKET_COUNT,
				     sizeof(*sfb_bucket_counts), GFP_KERNEL);
	sfb_bucket_starts = kvmalloc_array(SFB_BUCKET_COUNT + 1,
					   sizeof(*sfb_bucket_starts), GFP_KERNEL);
	sfb_candidates = kvmalloc_array(max_candidates,
				 sizeof(*sfb_candidates), GFP_KERNEL);
	if (!sfb_pages || !sfb_bucket_counts || !sfb_bucket_starts ||
	    !sfb_candidates)
		return -ENOMEM;

	for (i = 0; i < pool_pages; ++i) {
		struct page *page = alloc_page(GFP_KERNEL | __GFP_ZERO);
		unsigned long pfn;
		u32 bucket;

		if (!page)
			return -ENOMEM;
		sfb_pages[sfb_allocated_pages++] = page;

		pfn = page_to_pfn(page);
		if ((pfn & SFB_FIXED_PFN_LOW_MASK) != SFB_FIXED_PFN_LOW_MASK)
			continue;

		bucket = (pfn >> SFB_BUCKET_SHIFT) & (SFB_BUCKET_COUNT - 1U);
		sfb_bucket_counts[bucket]++;
		sfb_qualified_pages++;
	}

	if (sfb_qualified_pages < SFB_GROUP_LINES)
		return -ENOSPC;

	sfb_bucket_starts[0] = 0;
	for (i = 0; i < SFB_BUCKET_COUNT; ++i) {
		sfb_bucket_starts[i + 1] =
			sfb_bucket_starts[i] + sfb_bucket_counts[i];
		cursor[i] = sfb_bucket_starts[i];
	}

	sfb_bucket_pages = kvmalloc_array(sfb_qualified_pages,
					 sizeof(*sfb_bucket_pages), GFP_KERNEL);
	if (!sfb_bucket_pages)
		return -ENOMEM;

	for (i = 0; i < sfb_allocated_pages; ++i) {
		unsigned long pfn = page_to_pfn(sfb_pages[i]);
		u32 bucket;

		if ((pfn & SFB_FIXED_PFN_LOW_MASK) != SFB_FIXED_PFN_LOW_MASK)
			continue;
		bucket = (pfn >> SFB_BUCKET_SHIFT) & (SFB_BUCKET_COUNT - 1U);
		sfb_bucket_pages[cursor[bucket]++] = sfb_pages[i];
	}

	for (i = 0; i < SFB_BUCKET_COUNT; ++i)
		if (sfb_bucket_counts[i] > 1)
			sort(&sfb_bucket_pages[sfb_bucket_starts[i]],
			     sfb_bucket_counts[i],
			     sizeof(*sfb_bucket_pages),
			     sfb_page_phys_compare, NULL);

	return sfb_pick_anchor_lines();
}

static int __init sfb_init(void)
{
	int ret;

	if (PAGE_SIZE != 4096 || !pool_pages || pool_pages > SFB_MAX_POOL_PAGES ||
	    !search_repetitions || search_repetitions > SFB_MAX_REPETITIONS ||
	    !fill_rounds || fill_rounds > SFB_MAX_REPETITIONS ||
	    !validation_repetitions || validation_repetitions > SFB_MAX_REPETITIONS ||
	    !calibration_repetitions || calibration_repetitions > SFB_MAX_REPETITIONS ||
	    control_percent > 100 || validation_percent > 100 ||
	    !validation_ratio_percent || !max_candidates ||
	    max_candidates > SFB_MAX_CANDIDATES || !max_results ||
	    max_results > max_candidates || probe_cpu < 0 || probe_cpu >= nr_cpu_ids ||
	    stimulus_cpu < 0 || stimulus_cpu >= nr_cpu_ids ||
	    probe_cpu == stimulus_cpu)
		return -EINVAL;

	cpus_read_lock();
	if (!cpu_online(probe_cpu) || !cpu_online(stimulus_cpu)) {
		cpus_read_unlock();
		return -ENODEV;
	}
	cpus_read_unlock();

	ret = sfb_allocate_pool();
	if (ret)
		goto fail_pool;

	pr_info(SFB_NAME ": allocated=%u pages (%llu MiB), qualified=%u (PA[23:6]=1), anchors=[%u,%u,%u,%u]\n",
		sfb_allocated_pages,
		(unsigned long long)sfb_allocated_pages << PAGE_SHIFT >> 20,
		sfb_qualified_pages,
		sfb_anchor_buckets[0], sfb_anchor_buckets[1],
		sfb_anchor_buckets[2], sfb_anchor_buckets[3]);

	sfb_stimulus_task = kthread_create(sfb_stimulus_thread, NULL,
					   "sfb_stim/%d", stimulus_cpu);
	if (IS_ERR(sfb_stimulus_task)) {
		ret = PTR_ERR(sfb_stimulus_task);
		sfb_stimulus_task = NULL;
		goto fail_pool;
	}
	kthread_bind(sfb_stimulus_task, stimulus_cpu);
	wake_up_process(sfb_stimulus_task);

	sfb_probe_task = kthread_create(sfb_probe_thread, NULL,
				"sfb_probe/%d", probe_cpu);
	if (IS_ERR(sfb_probe_task)) {
		ret = PTR_ERR(sfb_probe_task);
		sfb_probe_task = NULL;
		goto fail_stimulus;
	}
	kthread_bind(sfb_probe_task, probe_cpu);
	wake_up_process(sfb_probe_task);
	return 0;

fail_stimulus:
	atomic_set(&sfb_shutdown, 1);
	complete_all(&sfb_prepare_request);
	complete_all(&sfb_access_request);
	kthread_stop(sfb_stimulus_task);
	sfb_stimulus_task = NULL;
fail_pool:
	sfb_free_pool();
	return ret;
}

static void __exit sfb_exit(void)
{
	if (sfb_probe_task) {
		kthread_stop(sfb_probe_task);
		sfb_probe_task = NULL;
	}
	if (sfb_stimulus_task) {
		atomic_set(&sfb_shutdown, 1);
		complete_all(&sfb_prepare_request);
		complete_all(&sfb_access_request);
		kthread_stop(sfb_stimulus_task);
		sfb_stimulus_task = NULL;
	}
	sfb_free_pool();
	pr_info(SFB_NAME ": unloaded\n");
}

module_init(sfb_init);
module_exit(sfb_exit);

MODULE_DESCRIPTION("Deterministic RK3588 SF cross-core validator with C(64,5) traversal");
MODULE_AUTHOR("HuaijiGao");
MODULE_LICENSE("GPL");
