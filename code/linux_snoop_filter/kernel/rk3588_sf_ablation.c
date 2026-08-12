// SPDX-License-Identifier: GPL-2.0
/*
 * Fixed-manifest address-set ablation for RK3588 real-time interference.
 *
 * This translation unit deliberately reuses the counter, worker handshake,
 * cache-maintenance, and page-allocation implementation of crossprobe.  It
 * never accepts or maps an arbitrary physical address: historical addresses
 * are represented only as ranks, and every line used below is resolved from
 * a page currently owned by sfcp_pages.
 */
#include <linux/sort.h>

#define SFCP_NAME                    "rk3588_sf_ablation"
#define SFCP_POOL_PAGES_DEFAULT      524288U
#define SFCP_ACCESS_ITEMS_DEFAULT    61U
#define SFCP_FILL_ROUNDS_DEFAULT     1000U
#define SFCP_MATCH_BITS_DEFAULT      25U
#define SFCP_STIMULUS_CPUS_DEFAULT   "6"
#define SFCP_TARGET_LOW_DEFAULT      0x624480ULL
#define SFCP_ADDRMASK_DEFAULT        0x1ffffffULL
#define SFCP_RESULT_PATH_DEFAULT     "/tmp/rk3588_sf_ablation.csv"
#define SFCP_MODULE_DESCRIPTION      \
	"RK3588 owned-page fixed-manifest address-set ablation"
#define SFCP_ABLATION_BUILD
#define SFCP_EXPERIMENT_ENTRY        sfab_run_experiment

static int sfab_run_experiment(void);
#include "rk3588_sf_crossprobe.c"

#ifndef SFCP_CUSTOM_ENTRY_SUPPORTED
#error "rk3588_sf_ablation requires the matching crossprobe custom-entry support"
#endif
MODULE_INFO(sfab_abi, "fixed-rank-ablation-v7");

#define SFAB_MANIFESTS               20U
#define SFAB_GROUP_LINES             62U
#define SFAB_STIMULUS_LINES          61U
#define SFAB_POOL_CAPACITY           256U
#define SFAB_NO_SLOT                 UINT_MAX
#define SFAB_MAX_FILL_SWEEP          16U
#define SFAB_MAX_RETAINED_PAGES      (SFAB_MANIFESTS * SFAB_GROUP_LINES)

static unsigned int trial_repetitions = 10U;
module_param(trial_repetitions, uint, 0444);
MODULE_PARM_DESC(trial_repetitions,
		 "Repetitions used to rank each tentative one-address removal");

static unsigned int validation_repetitions = 100U;
module_param(validation_repetitions, uint, 0444);
MODULE_PARM_DESC(validation_repetitions,
		 "Repetitions used to validate the full and each chosen reduced set");

static unsigned int min_success_percent = 95U;
module_param(min_success_percent, uint, 0444);
MODULE_PARM_DESC(min_success_percent,
		 "Minimum percentage of reloads above trigger_threshold");

static unsigned int trigger_threshold = 150U;
module_param(trigger_threshold, uint, 0444);
MODULE_PARM_DESC(trigger_threshold,
		 "Probe reload threshold in the selected counter's ticks");

static unsigned int min_candidate_items = 1U;
module_param(min_candidate_items, uint, 0444);
MODULE_PARM_DESC(min_candidate_items,
		 "Stop after reaching this many non-replaced candidate addresses");

static bool use_replacements;
module_param(use_replacements, bool, 0444);
MODULE_PARM_DESC(use_replacements,
		 "Keep 61 loads with adjacent controls; false drops removed addresses");

static unsigned int manifest_mask = (1U << SFAB_MANIFESTS) - 1U;
module_param(manifest_mask, uint, 0444);
MODULE_PARM_DESC(manifest_mask,
		 "Bit mask selecting manifests 0..19 (default 0xfffff runs all)");

static unsigned long long initial_active_mask = U64_MAX;
module_param(initial_active_mask, ullong, 0444);
MODULE_PARM_DESC(initial_active_mask,
		 "Optional 61-bit slot mask used to resume an exact reduced set");

static char *fill_rounds_sweep = "";
module_param(fill_rounds_sweep, charp, 0444);
MODULE_PARM_DESC(fill_rounds_sweep,
		 "Comma-separated fill_rounds values run with one owned-page allocation");

struct sfab_manifest {
	u8 source_version;
	u16 source_candidate;
	u8 required_pool;
	u64 target_masked;
	u8 rank[SFAB_GROUP_LINES]; /* rank[61] is the probe */
};

struct sfab_metric {
	u32 successes;
	u32 repetitions;
	u64 mean;
	u64 median;
	u64 p90;
	u64 minimum;
	u64 maximum;
	u64 probe_l1d_refill_total;
	u64 probe_l2d_refill_total;
	u64 probe_bus_access_total;
	u32 probe_l2d_refill_samples;
};

static int sfab_parse_fill_rounds(unsigned int *values, unsigned int *count)
{
	char *cursor;
	char *copy;
	char *token;
	int ret = 0;

	if (!fill_rounds_sweep || !fill_rounds_sweep[0]) {
		values[0] = fill_rounds;
		*count = 1U;
		return 0;
	}
	copy = kstrdup(fill_rounds_sweep, GFP_KERNEL);
	if (!copy)
		return -ENOMEM;
	cursor = copy;
	*count = 0U;
	while ((token = strsep(&cursor, ",")) != NULL) {
		unsigned int value;

		if (!token[0] || *count >= SFAB_MAX_FILL_SWEEP ||
		    kstrtouint(token, 0, &value) || !value ||
		    value > SFCP_FILL_ROUNDS_MAX) {
			ret = -EINVAL;
			break;
		}
		values[(*count)++] = value;
	}
	if (!*count)
		ret = -EINVAL;
	kfree(copy);
	return ret;
}

/* One probe thread runs the experiment, so this scratch space is not shared. */
static struct sfab_metric sfab_trial_metrics[SFAB_STIMULUS_LINES];

/*
 * Selected from the two supplied CSVs by repeat success, median reload, then
 * lower MAD.  Each row is a permutation of the sorted, mask-matching pool.
 */
static const struct sfab_manifest sfab_manifest[SFAB_MANIFESTS] = {
	{ 1, 696, 62, 0x624480ULL, { 4,59,3,34,19,1,57,54,39,0,58,17,44,45,42,24,22,28,31,7,53,37,23,60,13,61,12,40,27,2,43,21,47,51,33,49,5,38,9,20,14,25,50,55,35,11,32,16,15,48,46,26,8,36,41,29,6,18,52,10,56,30 } },
	{ 1, 845, 62, 0x624480ULL, { 6,38,13,30,18,26,27,1,5,14,46,28,7,47,52,44,3,40,16,60,57,21,15,53,61,41,58,31,34,36,0,20,48,43,2,42,4,56,35,23,25,9,50,45,55,22,29,11,33,19,59,12,8,37,32,49,10,51,17,54,39,24 } },
	{ 1, 216, 62, 0x624480ULL, { 5,26,50,61,13,37,29,40,7,34,8,19,17,32,20,35,43,12,44,49,47,6,36,22,21,45,24,38,54,53,9,51,58,3,42,56,11,60,18,2,30,0,15,1,25,28,10,16,41,52,31,57,4,59,48,55,39,14,27,46,23,33 } },
	{ 1, 942, 62, 0x624480ULL, { 29,42,4,45,5,36,56,48,60,50,43,41,26,15,0,30,17,58,23,27,39,59,13,6,61,12,52,11,32,55,47,25,44,3,20,8,19,14,57,31,54,1,7,35,33,9,16,10,28,37,18,24,40,22,53,34,38,51,46,21,49,2 } },
	{ 1, 54,  62, 0x624480ULL, { 32,8,55,2,48,50,35,29,40,20,6,36,12,0,10,45,30,39,25,41,14,5,24,51,28,53,54,21,44,1,42,46,27,58,56,60,26,43,16,15,23,3,17,18,7,31,22,37,4,13,19,9,52,59,34,57,33,47,49,38,11,61 } },
	{ 2, 144, 66, 0x138e900ULL, { 48,11,60,5,41,0,27,21,16,8,18,59,31,13,25,46,36,47,23,15,45,52,53,54,55,7,4,1,33,26,43,56,9,61,39,10,42,35,19,20,29,37,64,62,63,3,57,65,17,49,28,6,44,32,2,12,22,30,51,38,50,58 } },
	{ 2, 221, 66, 0x138e900ULL, { 33,41,26,14,32,42,49,6,23,21,65,34,11,56,19,60,46,52,51,54,29,38,5,63,44,12,40,24,0,27,64,30,61,7,16,22,50,47,45,36,9,15,10,39,55,4,57,17,3,59,35,20,18,31,25,28,2,8,48,1,58,62 } },
	{ 2, 236, 66, 0x138e900ULL, { 5,33,35,17,49,9,40,42,46,53,38,36,15,4,10,21,18,19,0,59,20,14,58,26,64,54,28,22,63,25,62,60,39,52,37,27,45,3,8,56,6,29,23,12,44,32,2,31,16,48,24,34,30,51,61,55,11,57,13,47,43,1 } },
	{ 2, 400, 66, 0x138e900ULL, { 21,51,16,61,52,48,3,11,40,49,65,59,44,28,43,13,2,4,1,10,18,22,64,56,20,62,34,37,41,6,54,19,14,17,0,45,58,38,25,26,24,8,32,46,7,63,42,60,31,50,53,39,23,9,55,35,27,33,30,29,47,36 } },
	{ 2, 402, 66, 0x138e900ULL, { 45,6,8,50,9,39,64,47,59,55,62,51,26,17,11,48,20,5,13,56,53,23,16,24,49,41,0,14,31,32,4,61,7,38,19,46,2,25,1,29,57,44,43,60,22,63,52,35,27,54,12,10,21,58,15,37,3,18,28,65,30,33 } },
	{ 1, 401, 62, 0x624480ULL, { 40,42,29,28,36,61,16,43,11,38,23,58,45,53,37,9,50,8,30,14,32,35,18,41,27,56,60,44,5,7,2,10,49,13,39,22,21,24,34,31,46,26,59,19,51,12,15,48,1,54,17,6,55,47,25,3,57,33,20,52,4,0 } },
	{ 1, 653, 62, 0x624480ULL, { 38,35,20,26,55,6,23,48,4,2,7,25,50,18,30,44,33,28,19,11,40,54,1,41,46,49,31,58,24,56,8,5,9,61,39,42,45,34,43,12,16,47,57,10,21,17,60,13,53,3,52,27,15,14,36,32,51,29,37,22,59,0 } },
	{ 1, 646, 62, 0x624480ULL, { 18,15,8,60,55,5,37,9,26,36,45,34,42,14,31,30,47,33,32,61,56,23,22,29,49,39,16,27,28,48,41,20,3,35,21,40,52,13,11,44,12,24,53,38,58,46,2,25,43,7,57,6,4,1,0,59,10,19,17,51,50,54 } },
	{ 1, 74, 62, 0x624480ULL, { 11,40,41,43,49,42,0,36,27,13,53,38,8,4,61,55,15,57,2,31,6,47,60,34,22,9,54,7,37,16,20,21,5,35,17,12,52,32,1,51,19,24,18,30,33,44,46,23,3,58,39,28,26,14,50,25,45,56,10,48,29,59 } },
	{ 1, 105, 62, 0x624480ULL, { 55,35,47,26,49,51,54,24,41,58,43,17,3,11,12,34,5,10,4,16,46,37,1,38,57,15,30,13,48,59,22,32,6,39,18,61,53,14,27,44,45,31,36,33,21,25,52,7,8,60,20,28,29,42,9,2,40,56,19,50,23,0 } },
	{ 2, 491, 66, 0x138e900ULL, { 59,46,10,20,7,14,15,33,4,62,12,38,24,55,25,61,47,6,51,42,48,45,0,32,29,34,52,3,35,27,18,56,57,11,64,65,58,2,23,54,49,36,13,8,16,28,53,5,50,37,26,17,44,1,9,60,21,41,43,19,31,63 } },
	{ 2, 492, 66, 0x138e900ULL, { 58,48,7,11,2,63,56,51,57,18,21,40,61,38,17,65,47,15,60,42,54,55,10,25,8,26,23,9,20,52,19,45,0,6,62,30,27,13,14,1,34,46,43,29,16,4,24,36,32,33,12,22,49,64,53,31,41,37,3,35,28,59 } },
	{ 2, 504, 66, 0x138e900ULL, { 23,34,24,51,36,60,46,48,55,2,19,49,4,57,20,59,14,38,18,31,45,61,47,7,53,63,11,29,54,16,9,10,58,26,28,30,56,5,43,40,1,41,0,35,65,15,37,27,32,8,62,17,39,64,13,25,12,22,50,44,6,3 } },
	{ 2, 534, 66, 0x138e900ULL, { 59,15,22,55,62,21,27,41,46,0,18,28,9,49,57,14,37,2,31,11,52,56,44,42,48,23,7,10,47,33,60,13,36,19,38,51,65,54,45,5,24,12,53,16,50,29,39,25,63,8,34,6,32,26,58,43,3,35,1,40,30,4 } },
	{ 2, 565, 66, 0x138e900ULL, { 51,38,48,6,42,11,40,62,5,33,25,32,46,52,4,65,39,14,23,27,29,9,30,41,36,8,63,54,47,19,10,59,18,64,43,56,28,15,61,45,0,16,31,12,26,50,22,3,49,24,17,20,57,2,34,7,60,35,37,53,55,1 } },
};

static int sfab_line_compare(const void *left, const void *right)
{
	phys_addr_t a = sfcp_physical(left);
	phys_addr_t b = sfcp_physical(right);

	return a < b ? -1 : a > b;
}

static int sfab_u64_compare(const void *left, const void *right)
{
	u64 a = *(const u64 *)left;
	u64 b = *(const u64 *)right;

	return a < b ? -1 : a > b;
}

static int sfab_page_compare(const void *left, const void *right)
{
	unsigned long a = (unsigned long)*(struct page * const *)left;
	unsigned long b = (unsigned long)*(struct page * const *)right;

	return a < b ? -1 : a > b;
}

static bool sfab_page_is_retained(struct page *page, struct page **retained,
				  unsigned int retained_count)
{
	unsigned long target = (unsigned long)page;
	unsigned int low = 0;
	unsigned int high = retained_count;

	while (low < high) {
		unsigned int middle = low + (high - low) / 2U;
		unsigned long candidate = (unsigned long)retained[middle];

		if (candidate < target)
			low = middle + 1U;
		else
			high = middle;
	}
	return low < retained_count && retained[low] == page;
}

static int sfab_release_unused_pages(const struct sfcp_line *pool_v1,
				     const struct sfcp_line *pool_v2)
{
	struct page **retained;
	u64 active_mask = initial_active_mask == U64_MAX ?
		GENMASK_ULL(60, 0) : initial_active_mask;
	unsigned int retained_count = 0;
	unsigned int old_count = sfcp_allocated_pages;
	unsigned int write_index = 0;
	unsigned int manifest_index;
	unsigned int i;

	retained = kvmalloc_array(SFAB_MAX_RETAINED_PAGES,
				  sizeof(*retained), GFP_KERNEL);
	if (!retained)
		return -ENOMEM;
	for (manifest_index = 0; manifest_index < SFAB_MANIFESTS;
	     ++manifest_index) {
		const struct sfab_manifest *manifest =
			&sfab_manifest[manifest_index];
		const struct sfcp_line *pool = manifest->source_version == 1 ?
			pool_v1 : pool_v2;

		if (!(manifest_mask & BIT(manifest_index)))
			continue;
		for (i = 0; i < SFAB_STIMULUS_LINES; ++i) {
			if (!use_replacements && !(active_mask & BIT_ULL(i)))
				continue;
			retained[retained_count++] =
				pool[manifest->rank[i]].page;
		}
		retained[retained_count++] =
			pool[manifest->rank[SFAB_STIMULUS_LINES]].page;
	}
	sort(retained, retained_count, sizeof(*retained),
	     sfab_page_compare, NULL);
	for (i = 0; i < old_count; ++i) {
		struct page *page = sfcp_pages[i];

		if (sfab_page_is_retained(page, retained, retained_count))
			sfcp_pages[write_index++] = page;
		else
			__free_page(page);
	}
	sfcp_allocated_pages = write_index;
	pr_info(SFCP_NAME ": released %u unused pool pages; retaining %u owned pages for selected manifests\n",
		old_count - write_index, write_index);
	kvfree(retained);
	return 0;
}

static int sfab_collect_pool(u64 target, struct sfcp_line *pool,
			     unsigned int *count)
{
	unsigned int offset = target & (PAGE_SIZE - 1U);
	unsigned int i;

	*count = 0;
	for (i = 0; i < sfcp_allocated_pages; ++i) {
		phys_addr_t pa = page_to_phys(sfcp_pages[i]) + offset;

		if ((pa & sfcp_active_addrmask) != target)
			continue;
		if (*count == SFAB_POOL_CAPACITY)
			return -E2BIG;
		pool[*count].page = sfcp_pages[i];
		pool[*count].offset = offset;
		++*count;
	}
	sort(pool, *count, sizeof(*pool), sfab_line_compare, NULL);
	return 0;
}

static struct sfcp_line sfab_replacement(const struct sfcp_line *candidate)
{
	struct sfcp_line replacement = *candidate;

	/* Adjacent, non-matching line in the same currently-owned page. */
	replacement.offset ^= SFCP_CACHE_LINE_SIZE;
	return replacement;
}

static unsigned int sfab_build_group(const struct sfab_manifest *manifest,
				     const struct sfcp_line *pool,
				     const bool *active,
				     struct sfcp_line *group)
{
	unsigned int packed = 0;
	unsigned int i;

	for (i = 0; i < SFAB_STIMULUS_LINES; ++i) {
		const struct sfcp_line *candidate = &pool[manifest->rank[i]];

		if (use_replacements)
			group[i] = active[i] ? *candidate :
				sfab_replacement(candidate);
		else if (active[i])
			group[packed++] = *candidate;
	}
	group[SFAB_STIMULUS_LINES] = pool[manifest->rank[SFAB_STIMULUS_LINES]];
	return use_replacements ? SFAB_STIMULUS_LINES : packed;
}

static bool sfab_is_stable(const struct sfab_metric *metric)
{
	return (u64)metric->successes * 100ULL >=
	       (u64)metric->repetitions * min_success_percent;
}

static int sfab_measure(const struct sfcp_line *group,
			unsigned int stimulus_items, unsigned int repetitions,
			struct sfab_metric *metric, u64 *ticks)
{
	u64 sum = 0;
	unsigned int i;

	memset(metric, 0, sizeof(*metric));
	metric->repetitions = repetitions;
	metric->minimum = U64_MAX;
	for (i = 0; i < repetitions; ++i) {
		u64 first, prefill, wait_prefill;
		struct sfcp_pmu_delta pmu;

		if (unlikely(kthread_should_stop()))
			return -EINTR;
		ticks[i] = sfcp_measure_cross_probe(group, stimulus_items, &first,
						    &prefill, &wait_prefill,
						    &pmu);
		sum += ticks[i];
		metric->probe_l1d_refill_total += pmu.l1d_refill;
		metric->probe_l2d_refill_total += pmu.l2d_refill;
		metric->probe_bus_access_total += pmu.bus_access;
		if (pmu.l2d_refill)
			++metric->probe_l2d_refill_samples;
		metric->minimum = min(metric->minimum, ticks[i]);
		metric->maximum = max(metric->maximum, ticks[i]);
		if (ticks[i] > trigger_threshold)
			++metric->successes;
		if (!(i & 0xfU))
			cond_resched();
	}
	metric->mean = div_u64(sum, repetitions);
	sort(ticks, repetitions, sizeof(*ticks), sfab_u64_compare, NULL);
	metric->median = ticks[(repetitions - 1U) / 2U];
	metric->p90 = ticks[DIV_ROUND_UP(repetitions * 9U, 10U) - 1U];
	return 0;
}

static int sfab_write_header(struct file *file, loff_t *position,
			     unsigned int pool_v1, unsigned int pool_v2)
{
	char buffer[1536];
	size_t used;

	used = scnprintf(buffer, sizeof(buffer),
		"# module,%s\n"
		"# experiment,fixed_rank_%s_ablation\n"
		"# owned_pages_only,true\n"
		"# arbitrary_physical_address_input,false\n"
		"# counter,%s\n"
		"# counter_calibration_delta,%llu\n"
		"# probe_cpu,%d\n"
		"# stimulus_cpus,%s\n"
		"# pool_pages,%u\n"
		"# addrmask,0x%llx\n"
		"# v1_target_masked,0x624480\n"
		"# v2_target_masked,0x138e900\n"
		"# v1_owned_candidate_pool,%u\n"
		"# v2_owned_candidate_pool,%u\n"
		"# fixed_manifests,%u\n"
		"# stimulus_slots,%u\n"
		"# fill_rounds,%u\n"
		"# fill_rounds_sweep,%s\n"
		"# trigger_threshold,%u\n"
		"# min_success_percent,%u\n"
		"# trial_repetitions,%u\n"
		"# validation_repetitions,%u\n"
		"# use_replacements,%s\n"
		"# manifest_mask,0x%x\n"
		"# initial_active_mask,0x%llx\n"
		"record,manifest,source_version,source_candidate,step,fill_rounds,active_candidates,test_removed_slot,chosen,stable,successes,repetitions,success_percent,mean_ticks,median_ticks,p90_ticks,min_ticks,max_ticks,probe_l1d_refill_total,probe_l2d_refill_total,probe_bus_access_total,probe_l2d_refill_samples,probe_pa,removed_pa,replacement_pa\n",
		SFCP_NAME, use_replacements ? "replacement" : "drop",
		sfcp_counter_name(),
		(unsigned long long)sfcp_counter_calibration_delta,
		probe_cpu, stimulus_cpus, pool_pages,
		(unsigned long long)sfcp_active_addrmask, pool_v1, pool_v2,
		SFAB_MANIFESTS, SFAB_STIMULUS_LINES, fill_rounds,
		fill_rounds_sweep && fill_rounds_sweep[0] ?
			fill_rounds_sweep : "disabled",
		trigger_threshold, min_success_percent, trial_repetitions,
		validation_repetitions,
		use_replacements ? "true" : "false", manifest_mask,
		(unsigned long long)initial_active_mask);
	if (used >= sizeof(buffer) - 1U)
		return -EOVERFLOW;
	return sfcp_write_all(file, buffer, used, position);
}

static int sfab_write_metric(struct file *file, loff_t *position,
			     const char *record, unsigned int manifest_index,
			     const struct sfab_manifest *manifest,
			     unsigned int step, unsigned int active_count,
			     unsigned int removed_slot, bool chosen,
			     const struct sfab_metric *metric,
			     const struct sfcp_line *pool)
{
	char row[512];
	phys_addr_t probe_pa = sfcp_physical(
		&pool[manifest->rank[SFAB_STIMULUS_LINES]]);
	phys_addr_t removed_pa = 0;
	phys_addr_t replacement_pa = 0;
	size_t used;

	if (removed_slot != SFAB_NO_SLOT) {
		const struct sfcp_line *line = &pool[manifest->rank[removed_slot]];
		struct sfcp_line replacement = sfab_replacement(line);

		removed_pa = sfcp_physical(line);
		if (use_replacements)
			replacement_pa = sfcp_physical(&replacement);
	}
	used = scnprintf(row, sizeof(row),
		"%s,%u,%u,%u,%u,%u,%u,%d,%u,%u,%u,%u,%u,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%u,0x%llx,0x%llx,0x%llx\n",
		record, manifest_index, manifest->source_version,
		manifest->source_candidate, step, fill_rounds, active_count,
		removed_slot == SFAB_NO_SLOT ? -1 : (int)removed_slot,
		chosen, sfab_is_stable(metric), metric->successes,
		metric->repetitions,
		metric->repetitions ?
			(metric->successes * 100U) / metric->repetitions : 0,
		(unsigned long long)metric->mean,
		(unsigned long long)metric->median,
		(unsigned long long)metric->p90,
		(unsigned long long)metric->minimum,
		(unsigned long long)metric->maximum,
		(unsigned long long)metric->probe_l1d_refill_total,
		(unsigned long long)metric->probe_l2d_refill_total,
		(unsigned long long)metric->probe_bus_access_total,
		metric->probe_l2d_refill_samples,
		(unsigned long long)probe_pa,
		(unsigned long long)removed_pa,
		(unsigned long long)replacement_pa);
	if (used >= sizeof(row) - 1U)
		return -EOVERFLOW;
	return sfcp_write_all(file, row, used, position);
}

static int sfab_write_final_members(struct file *file, loff_t *position,
				    unsigned int manifest_index,
				    const struct sfab_manifest *manifest,
				    const bool *active,
				    const struct sfcp_line *pool)
{
	char row[256];
	unsigned int i;
	int ret = 0;

	for (i = 0; !ret && i < SFAB_STIMULUS_LINES; ++i) {
		size_t used;

		if (!active[i])
			continue;
		used = scnprintf(row, sizeof(row),
			"final_member,%u,%u,%u,0,%u,0,%u,1,1,0,0,0,0,0,0,0,0,0,0,0,0,0x%llx,0x%llx,0x0\n",
			manifest_index, manifest->source_version,
			manifest->source_candidate, fill_rounds, i,
			(unsigned long long)sfcp_physical(
				&pool[manifest->rank[SFAB_STIMULUS_LINES]]),
			(unsigned long long)sfcp_physical(
				&pool[manifest->rank[i]]));
		ret = sfcp_write_all(file, row, used, position);
	}
	return ret;
}

static bool sfab_better_trial(const struct sfab_metric *candidate,
			      const struct sfab_metric *best,
			      unsigned int candidate_slot,
			      unsigned int best_slot)
{
	if (!sfab_is_stable(candidate))
		return false;
	if (best_slot == SFAB_NO_SLOT)
		return true;
	if (candidate->successes != best->successes)
		return candidate->successes > best->successes;
	if (candidate->median != best->median)
		return candidate->median > best->median;
	if (candidate->p90 != best->p90)
		return candidate->p90 > best->p90;
	return candidate_slot < best_slot;
}

static int sfab_run_manifest(struct file *file, loff_t *position,
			     unsigned int manifest_index,
			     const struct sfab_manifest *manifest,
			     const struct sfcp_line *pool,
			     struct sfcp_line *group, u64 *ticks)
{
	struct sfab_metric *trial = sfab_trial_metrics;
	struct sfab_metric current_metric;
	struct sfab_metric validation_metric;
	bool tested[SFAB_STIMULUS_LINES];
	bool active[SFAB_STIMULUS_LINES];
	unsigned int active_count = SFAB_STIMULUS_LINES;
	unsigned int stimulus_items;
	unsigned int step = 0;
	unsigned int i;
	int ret;

	if (initial_active_mask == U64_MAX) {
		memset(active, 1, sizeof(active));
	} else {
		active_count = 0;
		for (i = 0; i < SFAB_STIMULUS_LINES; ++i) {
			active[i] = !!(initial_active_mask & BIT_ULL(i));
			if (active[i])
				++active_count;
		}
	}
	stimulus_items = sfab_build_group(manifest, pool, active, group);
	ret = sfab_measure(group, stimulus_items, validation_repetitions,
			   &current_metric, ticks);
	if (ret)
		return ret;
	ret = sfab_write_metric(file, position, "initial_validation",
				manifest_index, manifest, step, active_count,
				SFAB_NO_SLOT, true, &current_metric, pool);
	if (ret)
		return ret;
	pr_info(SFCP_NAME ": manifest=%u source=v%u/%u initial active=%u stable=%u success=%u/%u mean=%llu median=%llu p90=%llu\n",
		manifest_index, manifest->source_version,
		manifest->source_candidate, active_count,
		sfab_is_stable(&current_metric), current_metric.successes,
		current_metric.repetitions,
		(unsigned long long)current_metric.mean,
		(unsigned long long)current_metric.median,
		(unsigned long long)current_metric.p90);
	if (!sfab_is_stable(&current_metric))
		goto final;

	while (active_count > min_candidate_items) {
		struct sfab_metric best = { };
		unsigned int best_slot = SFAB_NO_SLOT;
		bool accepted = false;

		memset(tested, 0, sizeof(tested));
		for (i = 0; i < SFAB_STIMULUS_LINES; ++i) {
			if (!active[i])
				continue;
			active[i] = false;
			stimulus_items = sfab_build_group(manifest, pool, active,
						  group);
			ret = sfab_measure(group, stimulus_items, trial_repetitions,
					   &trial[i], ticks);
			active[i] = true;
			if (ret)
				return ret;
			tested[i] = true;
			if (sfab_better_trial(&trial[i], &best, i, best_slot)) {
				best = trial[i];
				best_slot = i;
			}
		}
		for (i = 0; i < SFAB_STIMULUS_LINES; ++i) {
			if (!tested[i])
				continue;
			ret = sfab_write_metric(file, position, "candidate_trial",
					manifest_index, manifest, step + 1U,
					active_count - 1U, i, i == best_slot,
					&trial[i], pool);
			if (ret)
				return ret;
		}
		while (best_slot != SFAB_NO_SLOT) {
			active[best_slot] = false;
			stimulus_items = sfab_build_group(manifest, pool, active,
						  group);
			ret = sfab_measure(group, stimulus_items,
					   validation_repetitions,
					   &validation_metric, ticks);
			if (ret)
				return ret;
			ret = sfab_write_metric(file, position,
					"chosen_validation", manifest_index,
					manifest, step + 1U,
					active_count - 1U, best_slot, true,
					&validation_metric, pool);
			if (ret)
				return ret;
			if (sfab_is_stable(&validation_metric)) {
				current_metric = validation_metric;
				accepted = true;
				break;
			}

			/* Try the next stable short-test result at this step. */
			active[best_slot] = true;
			tested[best_slot] = false;
			best_slot = SFAB_NO_SLOT;
			memset(&best, 0, sizeof(best));
			for (i = 0; i < SFAB_STIMULUS_LINES; ++i) {
				if (tested[i] && sfab_better_trial(&trial[i],
						&best, i, best_slot)) {
					best = trial[i];
					best_slot = i;
				}
			}
		}
		if (!accepted)
			break;
		--active_count;
		++step;
		pr_info(SFCP_NAME ": manifest=%u source=v%u/%u mode=%s stable with %u active candidate lines median=%llu success=%u/%u\n",
			manifest_index, manifest->source_version,
			manifest->source_candidate,
			use_replacements ? "replacement" : "drop", active_count,
			(unsigned long long)current_metric.median,
			current_metric.successes, current_metric.repetitions);
	}

final:
	ret = sfab_write_metric(file, position, "final_summary",
				manifest_index, manifest, step, active_count,
				SFAB_NO_SLOT, true, &current_metric, pool);
	if (!ret)
		ret = sfab_write_final_members(file, position, manifest_index,
					       manifest, active, pool);
	return ret;
}

static int sfab_run_experiment(void)
{
	struct sfcp_line *pool_v1 = NULL;
	struct sfcp_line *pool_v2 = NULL;
	struct sfcp_line *group = NULL;
	u64 *ticks = NULL;
	struct file *file = NULL;
	loff_t position = 0;
	unsigned int count_v1;
	unsigned int count_v2;
	unsigned int max_repetitions;
	unsigned int sweep_values[SFAB_MAX_FILL_SWEEP];
	unsigned int sweep_count;
	unsigned int sweep_index;
	unsigned int original_fill_rounds = fill_rounds;
	unsigned int i;
	int ret;

	ret = sfab_parse_fill_rounds(sweep_values, &sweep_count);
	if (ret) {
		pr_err(SFCP_NAME ": invalid fill_rounds_sweep='%s'; use 1..%u comma-separated values (maximum %u)\n",
		       fill_rounds_sweep ? fill_rounds_sweep : "",
		       SFCP_FILL_ROUNDS_MAX, SFAB_MAX_FILL_SWEEP);
		return ret;
	}

	if (access_items != SFAB_STIMULUS_LINES ||
	    sfcp_active_addrmask != 0x1ffffffULL ||
	    !trial_repetitions || trial_repetitions > 1000U ||
	    !validation_repetitions || validation_repetitions > 1000U ||
	    !min_success_percent || min_success_percent > 100U ||
	    !trigger_threshold || !min_candidate_items || !manifest_mask ||
	    (manifest_mask & ~((1U << SFAB_MANIFESTS) - 1U)) ||
	    (initial_active_mask != U64_MAX &&
	     (!initial_active_mask ||
	      (initial_active_mask & ~GENMASK_ULL(60, 0)) ||
	      hweight64(initial_active_mask) < min_candidate_items)) ||
	    min_candidate_items > SFAB_STIMULUS_LINES) {
		pr_err(SFCP_NAME ": requires access_items=61 addrmask=0x1ffffff and valid ablation repetition/stability parameters\n");
		return -EINVAL;
	}

	pool_v1 = kvmalloc_array(SFAB_POOL_CAPACITY, sizeof(*pool_v1),
				 GFP_KERNEL);
	pool_v2 = kvmalloc_array(SFAB_POOL_CAPACITY, sizeof(*pool_v2),
				 GFP_KERNEL);
	group = kvmalloc_array(SFAB_GROUP_LINES, sizeof(*group), GFP_KERNEL);
	max_repetitions = max(trial_repetitions, validation_repetitions);
	ticks = kvmalloc_array(max_repetitions, sizeof(*ticks), GFP_KERNEL);
	if (!pool_v1 || !pool_v2 || !group || !ticks) {
		ret = -ENOMEM;
		goto out;
	}

	ret = sfab_collect_pool(0x624480ULL, pool_v1, &count_v1);
	if (ret)
		goto out;
	ret = sfab_collect_pool(0x138e900ULL, pool_v2, &count_v2);
	if (ret)
		goto out;
	if (count_v1 < 62U || count_v2 < 66U) {
		pr_err(SFCP_NAME ": fixed manifests require owned candidate pools v1>=62 and v2>=66, got v1=%u v2=%u; increase pool_pages or retry after reboot\n",
		       count_v1, count_v2);
		ret = -ENOSPC;
		goto out;
	}
	ret = sfab_release_unused_pages(pool_v1, pool_v2);
	if (ret)
		goto out;

	sfcp_enable_counter();
	if (!sfcp_counter_calibration_delta) {
		ret = -EOPNOTSUPP;
		goto out_counter;
	}
	file = filp_open(result_path, O_CREAT | O_TRUNC | O_WRONLY, 0644);
	if (IS_ERR(file)) {
		ret = PTR_ERR(file);
		file = NULL;
		goto out_counter;
	}
	ret = sfab_write_header(file, &position, count_v1, count_v2);
	if (!ret)
		pr_info(SFCP_NAME ": start mode=%s manifest_mask=0x%x initial_active_mask=0x%llx fill_rounds=%u sweep=%s trial_repetitions=%u validation_repetitions=%u threshold=%u min_success=%u%% min_candidate_items=%u pools=v1:%u,v2:%u\n",
			use_replacements ? "replacement" : "drop", manifest_mask,
			(unsigned long long)initial_active_mask, fill_rounds,
			fill_rounds_sweep && fill_rounds_sweep[0] ?
				fill_rounds_sweep : "disabled",
			trial_repetitions, validation_repetitions,
			trigger_threshold, min_success_percent,
			min_candidate_items, count_v1, count_v2);
	for (sweep_index = 0; !ret && sweep_index < sweep_count;
	     ++sweep_index) {
		fill_rounds = sweep_values[sweep_index];
		pr_info(SFCP_NAME ": fill sweep %u/%u fill_rounds=%u\n",
			sweep_index + 1U, sweep_count, fill_rounds);
		for (i = 0; !ret && i < SFAB_MANIFESTS; ++i) {
			const struct sfab_manifest *manifest = &sfab_manifest[i];
			const struct sfcp_line *pool =
				manifest->source_version == 1 ? pool_v1 : pool_v2;

			if (!(manifest_mask & BIT(i)))
				continue;
			ret = sfab_run_manifest(file, &position, i, manifest, pool,
						group, ticks);
		}
	}
	fill_rounds = original_fill_rounds;
	filp_close(file, NULL);
	file = NULL;
	if (!ret)
		pr_info(SFCP_NAME ": wrote fixed-manifest %s ablation to %s\n",
			use_replacements ? "replacement" : "drop", result_path);

out_counter:
	sfcp_restore_counter();
out:
	fill_rounds = original_fill_rounds;
	if (file)
		filp_close(file, NULL);
	kvfree(ticks);
	kvfree(group);
	kvfree(pool_v2);
	kvfree(pool_v1);
	return ret;
}
