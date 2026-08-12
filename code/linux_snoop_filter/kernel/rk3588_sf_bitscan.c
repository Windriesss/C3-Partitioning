// SPDX-License-Identifier: GPL-2.0
/*
 * Owned-page probe address-bit sensitivity experiment for RK3588.
 *
 * A computed XOR physical address is never mapped or dereferenced.  It is
 * used only as a lookup key over pages currently allocated and held by this
 * module.  A bit is reported unavailable unless an exact owned page exists.
 */
#include <linux/sort.h>

#ifndef SFBS_STIMULUS_SLOTS
#define SFBS_STIMULUS_SLOTS          61U
#endif
#if SFBS_STIMULUS_SLOTS < 1 || SFBS_STIMULUS_SLOTS > 61
#error "SFBS_STIMULUS_SLOTS must be in 1..61"
#endif

#define SFCP_NAME                    "rk3588_sf_bitscan"
#define SFCP_POOL_PAGES_DEFAULT      655360U
#define SFCP_ACCESS_ITEMS_DEFAULT    SFBS_STIMULUS_SLOTS
#define SFCP_FILL_ROUNDS_DEFAULT     1U
#define SFCP_MATCH_BITS_DEFAULT      25U
#define SFCP_STIMULUS_CPUS_DEFAULT   "6"
#define SFCP_TARGET_LOW_DEFAULT      0x138e900ULL
#define SFCP_ADDRMASK_DEFAULT        0x1ffffffULL
#define SFCP_RESULT_PATH_DEFAULT     "/tmp/rk3588_sf_bitscan.csv"
#define SFCP_MODULE_DESCRIPTION      \
	"RK3588 owned-page paired probe physical-address bit sensitivity"
#define SFCP_ABLATION_BUILD
#define SFCP_EXPERIMENT_ENTRY        sfbs_run_experiment

static int sfbs_run_experiment(void);
static int sfbs_run_single_experiment(void);
static int sfbs_run_pair_experiment(void);
static int sfbs_run_color_matrix_experiment(void);
static int sfbs_run_joint_bit_experiment(void);
#include "rk3588_sf_crossprobe.c"

#ifndef SFCP_CUSTOM_ENTRY_SUPPORTED
#error "rk3588_sf_bitscan requires matching crossprobe custom-entry support"
#endif
MODULE_INFO(sfbs_abi, "owned-probe-bitscan-v20");
MODULE_INFO(sfbs_slots, __stringify(SFBS_STIMULUS_SLOTS));

#define SFBS_POOL_CAPACITY           256U
#define SFBS_GROUP_LINES             (SFBS_STIMULUS_SLOTS + 1U)
#define SFBS_MAX_REPETITIONS         10000U
#define SFBS_MAX_RETAINED_PAGES      96U
#define SFBS_MIN_TEST_BIT             0U
#define SFBS_MAX_TEST_BIT            47U
#define SFBS_NO_SECOND_BIT           64U
#define SFBS_PAIR_TABLE_SIZE         (64U * 64U)
#define SFBS_MAX_TRIPLE_TARGETS      11480U
#define SFBS_MAX_COMBO_RETAINED      (SFBS_GROUP_LINES + 64U + \
					      SFBS_PAIR_TABLE_SIZE + \
					      SFBS_MAX_TRIPLE_TARGETS)
#define SFBS_COLOR_COUNT              4U
#define SFBS_MAX_COLOR_RETAINED       (SFBS_COLOR_COUNT * SFBS_GROUP_LINES)
#define SFBS_MAX_JOINT_RETAINED       (64U * SFBS_GROUP_LINES + \
					      SFBS_GROUP_LINES)

static unsigned int scan_order = 1U;
module_param(scan_order, uint, 0444);
MODULE_PARM_DESC(scan_order,
		 "Mode: 1 single, 2 pair, 3 triple, 4 color matrix, 5 joint bit scan, 6 same/cross attribution, 7 per-bit same/cross item sweep, or 8 same/cross item sweep only");

static char *experiment_run_id = "run0";
module_param(experiment_run_id, charp, 0444);
MODULE_PARM_DESC(experiment_run_id,
		 "Artifact run identifier written into CSV metadata");

static unsigned int item_sweep_first;
module_param(item_sweep_first, uint, 0444);
MODULE_PARM_DESC(item_sweep_first,
		 "First stimulus count in same/cross attribution sweep");

#if SFBS_STIMULUS_SLOTS < 24
static unsigned int item_sweep_last = SFBS_STIMULUS_SLOTS;
#else
static unsigned int item_sweep_last = 24U;
#endif
module_param(item_sweep_last, uint, 0444);
MODULE_PARM_DESC(item_sweep_last,
		 "Last stimulus count in same/cross attribution sweep");

static unsigned int item_sweep_repetitions = 200U;
module_param(item_sweep_repetitions, uint, 0444);
MODULE_PARM_DESC(item_sweep_repetitions,
		 "Trials per same/cross baseline/candidate item-sweep cell");

#define SFBS_MAX_SWEEP_ROUNDS 8U
static unsigned int item_sweep_rounds[SFBS_MAX_SWEEP_ROUNDS] = {
	1U, 10U, 100U, 1000U
};
static unsigned int item_sweep_rounds_count = 4U;
module_param_array_named(item_sweep_rounds, item_sweep_rounds, uint,
			 &item_sweep_rounds_count, 0444);
MODULE_PARM_DESC(item_sweep_rounds,
		 "Comma-separated fill-round values measured on one held candidate set");

static unsigned int attribution_control_repetitions = 100U;
module_param(attribution_control_repetitions, uint, 0444);
MODULE_PARM_DESC(attribution_control_repetitions,
		 "Idle trials per locality and bit in attribution mode");

static bool attribution_cross_only_bits;
module_param(attribution_cross_only_bits, bool, 0444);
MODULE_PARM_DESC(attribution_cross_only_bits,
		 "Skip same-core per-bit tests in attribution mode; item sweep still measures same and cross");

static unsigned long long joint_rescue_mask = BIT_ULL(16) | BIT_ULL(17);
module_param(joint_rescue_mask, ullong, 0444);
MODULE_PARM_DESC(joint_rescue_mask,
		 "Physical-address bits that additionally require whole-group XOR joint rescue in attribution mode");

static unsigned int latency_bin1 = 90U;
module_param(latency_bin1, uint, 0444);
MODULE_PARM_DESC(latency_bin1, "Upper bound of latency histogram bin 0");

static unsigned int latency_bin2 = 130U;
module_param(latency_bin2, uint, 0444);
MODULE_PARM_DESC(latency_bin2, "Upper bound of latency histogram bin 1");

static unsigned int latency_bin3 = 220U;
module_param(latency_bin3, uint, 0444);
MODULE_PARM_DESC(latency_bin3, "Upper bound of latency histogram bin 2");

static unsigned int color_bit0 = 16U;
module_param(color_bit0, uint, 0444);
MODULE_PARM_DESC(color_bit0, "Low color-selector physical-address bit");

static unsigned int color_bit1 = 17U;
module_param(color_bit1, uint, 0444);
MODULE_PARM_DESC(color_bit1, "High color-selector physical-address bit");

static unsigned int color_repetitions = 1000U;
module_param(color_repetitions, uint, 0444);
MODULE_PARM_DESC(color_repetitions,
		 "Paired trials for every off-diagonal color-matrix cell");

static unsigned int bit_first = 12U;
module_param(bit_first, uint, 0444);
MODULE_PARM_DESC(bit_first, "First physical-address bit to test (minimum 0)");

static unsigned int bit_last = 30U;
module_param(bit_last, uint, 0444);
MODULE_PARM_DESC(bit_last, "Last physical-address bit to test (maximum 47)");

static unsigned int bit_repetitions = 1000U;
module_param(bit_repetitions, uint, 0444);
MODULE_PARM_DESC(bit_repetitions, "Paired original/flipped trials per bit");

static unsigned int pair_screen_repetitions = 100U;
module_param(pair_screen_repetitions, uint, 0444);
MODULE_PARM_DESC(pair_screen_repetitions,
		 "Short original/double-flip trials for every available bit pair");

static unsigned int pair_screen_percent = 80U;
module_param(pair_screen_percent, uint, 0444);
MODULE_PARM_DESC(pair_screen_percent,
		 "Minimum double-flip success percentage promoted to long validation");

static unsigned int single_max_percent = 20U;
module_param(single_max_percent, uint, 0444);
MODULE_PARM_DESC(single_max_percent,
		 "Maximum success percentage allowed for each constituent single flip");

static unsigned int triple_screen_repetitions = 100U;
module_param(triple_screen_repetitions, uint, 0444);
MODULE_PARM_DESC(triple_screen_repetitions,
		 "Short original/triple-flip trials for every available triple");

static unsigned int triple_screen_percent = 80U;
module_param(triple_screen_percent, uint, 0444);
MODULE_PARM_DESC(triple_screen_percent,
		 "Minimum triple-flip success percentage promoted to long validation");

static unsigned int pair_max_percent = 20U;
module_param(pair_max_percent, uint, 0444);
MODULE_PARM_DESC(pair_max_percent,
		 "Maximum screen percentage allowed for each constituent pair");

static unsigned int trigger_threshold = 150U;
module_param(trigger_threshold, uint, 0444);
MODULE_PARM_DESC(trigger_threshold, "Probe reload threshold in counter ticks");

static unsigned int stable_percent = 95U;
module_param(stable_percent, uint, 0444);
MODULE_PARM_DESC(stable_percent, "Success percentage used only for CSV stable flag");

static bool auto_find_stable = true;
module_param(auto_find_stable, bool, 0444);
MODULE_PARM_DESC(auto_find_stable,
		 "Find and validate a stable candidate group in the current allocation");

static unsigned int candidate_items = SFBS_STIMULUS_SLOTS;
module_param(candidate_items, uint, 0444);
MODULE_PARM_DESC(candidate_items, "Initial stimulus addresses in online stable-group search");

static unsigned int target_candidate_items = 15U;
module_param(target_candidate_items, uint, 0444);
MODULE_PARM_DESC(target_candidate_items,
		 "Required final candidate count after greedy deletion");

static unsigned int reduction_repetitions = 20U;
module_param(reduction_repetitions, uint, 0444);
MODULE_PARM_DESC(reduction_repetitions,
		 "Short trials for each proposed candidate deletion");

static unsigned int reduction_validation_repetitions = 200U;
module_param(reduction_validation_repetitions, uint, 0444);
MODULE_PARM_DESC(reduction_validation_repetitions,
		 "Long trials required before accepting a candidate deletion");

static unsigned int scan_passes = 3U;
module_param(scan_passes, uint, 0444);
MODULE_PARM_DESC(scan_passes,
		 "Complete scan passes on the same held pages and final candidate set");

static unsigned int search_groups = 2000U;
module_param(search_groups, uint, 0444);
MODULE_PARM_DESC(search_groups, "Maximum random candidate groups tested online");

static unsigned int search_repetitions = 20U;
module_param(search_repetitions, uint, 0444);
MODULE_PARM_DESC(search_repetitions, "Short repetitions per online candidate group");

static unsigned int baseline_repetitions = 1000U;
module_param(baseline_repetitions, uint, 0444);
MODULE_PARM_DESC(baseline_repetitions,
		 "Long repetitions required before any bit scan is allowed");

/* Stable RK3588 m5 set; reduced-capacity builds default to all valid slots. */
#if SFBS_STIMULUS_SLOTS == 61
static unsigned long long stimulus_mask = 0x60082613a900030ULL;
#else
static unsigned long long stimulus_mask =
	GENMASK_ULL(SFBS_STIMULUS_SLOTS - 1U, 0U);
#endif
module_param(stimulus_mask, ullong, 0444);
MODULE_PARM_DESC(stimulus_mask, "Fixed stimulus slot mask within the compiled capacity");

static const u8 sfbs_rank[] = {
	48,11,60,5,41,0,27,21,16,8,18,59,31,13,25,46,
	36,47,23,15,45,52,53,54,55,7,4,1,33,26,43,56,
	9,61,39,10,42,35,19,20,29,37,64,62,63,3,57,65,
	17,49,28,6,44,32,2,12,22,30,51,38,50,58
};

struct sfbs_metric {
	u32 successes;
	u32 repetitions;
	u64 sum;
	u64 mean;
	u64 median;
	u64 p90;
	u64 minimum;
	u64 maximum;
	u32 probe_l1d_refill_total;
	u32 probe_l1d_tlb_refill_total;
	u32 probe_l2d_refill_total;
	u32 probe_bus_access_total;
	u32 probe_ll_cache_rd_total;
	u32 probe_ll_cache_miss_total;
	u32 probe_l1d_refill_samples;
	u32 probe_l1d_tlb_refill_samples;
	u32 probe_l2d_refill_samples;
	u32 probe_ll_cache_rd_samples;
	u32 probe_ll_cache_miss_samples;
	u32 latency_bins[4];
};

struct sfbs_pair_target {
	struct sfcp_line line;
	bool available;
	bool candidate_alias;
	bool selected;
	struct sfbs_metric screen_original;
	struct sfbs_metric screen_double;
};

struct sfbs_triple_target {
	u8 bit_i;
	u8 bit_j;
	u8 bit_k;
	struct sfcp_line line;
	bool available;
	bool candidate_alias;
	bool selected;
	struct sfbs_metric screen_original;
	struct sfbs_metric screen_triple;
};

struct sfbs_reduction_step {
	phys_addr_t removed_pa;
	u32 before_items;
	u32 after_items;
	struct sfbs_metric short_metric;
	struct sfbs_metric validation_metric;
};

static unsigned int sfbs_current_pass;

static bool sfbs_sweep_rounds_are_valid(void)
{
	unsigned int index;

	if (!item_sweep_rounds_count ||
	    item_sweep_rounds_count > SFBS_MAX_SWEEP_ROUNDS)
		return false;
	for (index = 0; index < item_sweep_rounds_count; ++index) {
		if (!item_sweep_rounds[index] || item_sweep_rounds[index] > 10000U)
			return false;
	}
	return true;
}

static bool sfbs_probe_aliases_stimulus(const struct sfcp_line *probe,
					const struct sfcp_line *group,
					unsigned int active_items);

static int sfbs_line_compare(const void *left, const void *right)
{
	phys_addr_t a = sfcp_physical(left);
	phys_addr_t b = sfcp_physical(right);

	return a < b ? -1 : a > b;
}

static int sfbs_u64_compare(const void *left, const void *right)
{
	u64 a = *(const u64 *)left;
	u64 b = *(const u64 *)right;

	return a < b ? -1 : a > b;
}

static int sfbs_page_compare(const void *left, const void *right)
{
	unsigned long a = (unsigned long)*(struct page * const *)left;
	unsigned long b = (unsigned long)*(struct page * const *)right;

	return a < b ? -1 : a > b;
}

static int sfbs_page_phys_compare(const void *left, const void *right)
{
	phys_addr_t a = page_to_phys(*(struct page * const *)left);
	phys_addr_t b = page_to_phys(*(struct page * const *)right);

	return a < b ? -1 : a > b;
}

static bool sfbs_page_is_retained(struct page *page, struct page **retained,
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

static int sfbs_collect_pool(struct sfcp_line *pool, unsigned int *count)
{
	unsigned int offset = target_low & (PAGE_SIZE - 1U);
	unsigned int i;

	*count = 0;
	for (i = 0; i < sfcp_allocated_pages; ++i) {
		phys_addr_t pa = page_to_phys(sfcp_pages[i]) + offset;

		if ((pa & sfcp_active_addrmask) != target_low)
			continue;
		if (*count == SFBS_POOL_CAPACITY)
			return -E2BIG;
		pool[*count].page = sfcp_pages[i];
		pool[*count].offset = offset;
		++*count;
	}
	sort(pool, *count, sizeof(*pool), sfbs_line_compare, NULL);
	return 0;
}

static bool sfbs_find_owned_line(phys_addr_t target_pa,
				 struct sfcp_line *line)
{
	phys_addr_t page_pa = target_pa & PAGE_MASK;
	unsigned int offset = target_pa & (PAGE_SIZE - 1U);
	unsigned int low = 0;
	unsigned int high = sfcp_allocated_pages;

	while (low < high) {
		unsigned int middle = low + (high - low) / 2U;
		phys_addr_t candidate_pa = page_to_phys(sfcp_pages[middle]);

		if (candidate_pa < page_pa)
			low = middle + 1U;
		else
			high = middle;
	}
	if (low == sfcp_allocated_pages ||
	    page_to_phys(sfcp_pages[low]) != page_pa)
		return false;
	line->page = sfcp_pages[low];
	line->offset = offset;
	return true;
}

static phys_addr_t sfbs_color_pa(phys_addr_t pa, unsigned int color)
{
	u64 mask = BIT_ULL(color_bit0) | BIT_ULL(color_bit1);

	pa &= ~mask;
	if (color & 1U)
		pa |= BIT_ULL(color_bit0);
	if (color & 2U)
		pa |= BIT_ULL(color_bit1);
	return pa;
}

static int sfbs_collect_color_pool(struct sfcp_line *pool,
				   unsigned int *count)
{
	unsigned int input_count;
	unsigned int read_index;
	unsigned int write_index = 0;
	int ret;

	ret = sfbs_collect_pool(pool, &input_count);
	if (ret)
		return ret;
	for (read_index = 0; read_index < input_count; ++read_index) {
		phys_addr_t pa = sfcp_physical(&pool[read_index]);
		unsigned int color;
		bool complete = true;

		for (color = 0; color < SFBS_COLOR_COUNT; ++color) {
			struct sfcp_line line;

			if (!sfbs_find_owned_line(sfbs_color_pa(pa, color), &line)) {
				complete = false;
				break;
			}
		}
		if (complete)
			pool[write_index++] = pool[read_index];
	}
	*count = write_index;
	pr_info(SFCP_NAME ": color quartet pool=%u/%u for bits %u,%u\n",
		write_index, input_count, color_bit0, color_bit1);
	return 0;
}

static int sfbs_build_color_groups(const struct sfcp_line *base_group,
		unsigned int active_items, struct sfcp_line *color_groups)
{
	unsigned int color;
	unsigned int i;

	for (color = 0; color < SFBS_COLOR_COUNT; ++color) {
		struct sfcp_line *group =
			&color_groups[color * SFBS_GROUP_LINES];

		for (i = 0; i < active_items; ++i) {
			phys_addr_t target = sfbs_color_pa(
				sfcp_physical(&base_group[i]), color);

			if (!sfbs_find_owned_line(target, &group[i]))
				return -ENOSPC;
		}
		if (!sfbs_find_owned_line(sfbs_color_pa(sfcp_physical(
				&base_group[SFBS_STIMULUS_SLOTS]), color),
				&group[SFBS_STIMULUS_SLOTS]))
			return -ENOSPC;
	}
	return 0;
}

static int sfbs_release_unused_color_pages(const struct sfcp_line *groups,
					   unsigned int active_items)
{
	struct page **retained;
	unsigned int retained_count = 0;
	unsigned int old_count = sfcp_allocated_pages;
	unsigned int write_index = 0;
	unsigned int color;
	unsigned int i;

	retained = kvmalloc_array(SFBS_MAX_COLOR_RETAINED,
				  sizeof(*retained), GFP_KERNEL);
	if (!retained)
		return -ENOMEM;
	for (color = 0; color < SFBS_COLOR_COUNT; ++color) {
		const struct sfcp_line *group =
			&groups[color * SFBS_GROUP_LINES];

		for (i = 0; i < active_items; ++i)
			retained[retained_count++] = group[i].page;
		retained[retained_count++] =
			group[SFBS_STIMULUS_SLOTS].page;
	}
	sort(retained, retained_count, sizeof(*retained),
	     sfbs_page_compare, NULL);
	for (i = 0; i < old_count; ++i) {
		struct page *page = sfcp_pages[i];

		if (sfbs_page_is_retained(page, retained, retained_count))
			sfcp_pages[write_index++] = page;
		else
			__free_page(page);
	}
	sfcp_allocated_pages = write_index;
	pr_info(SFCP_NAME ": released %u unused pages; retaining %u color-matrix pages\n",
		old_count - write_index, write_index);
	kvfree(retained);
	return 0;
}

static bool sfbs_build_joint_bit_group(const struct sfcp_line *base_group,
		unsigned int active_items, unsigned int bit,
		struct sfcp_line *joint_group, bool *probe_alias,
		unsigned int *missing_member)
{
	unsigned int i;

	*probe_alias = false;
	*missing_member = SFBS_NO_SECOND_BIT;
	for (i = 0; i < active_items; ++i) {
		phys_addr_t target = sfcp_physical(&base_group[i]) ^ BIT_ULL(bit);

		if (!sfbs_find_owned_line(target, &joint_group[i])) {
			*missing_member = i;
			return false;
		}
	}
	if (!sfbs_find_owned_line(sfcp_physical(
			&base_group[SFBS_STIMULUS_SLOTS]) ^ BIT_ULL(bit),
			&joint_group[SFBS_STIMULUS_SLOTS])) {
		*missing_member = SFBS_STIMULUS_SLOTS;
		return false;
	}
	/* A probe-only flip must not turn the probe into a stimulus address. */
	if (sfbs_probe_aliases_stimulus(
			&joint_group[SFBS_STIMULUS_SLOTS],
			base_group, active_items)) {
		*probe_alias = true;
		return false;
	}
	return true;
}

static int sfbs_release_unused_joint_pages(
		const struct sfcp_line *base_group, unsigned int active_items,
		const struct sfcp_line *probe_lines,
		const bool *probe_available,
		const struct sfcp_line *joint_groups,
		const bool *joint_available,
		const struct sfcp_line *extra_group, unsigned int extra_items)
{
	struct page **retained;
	unsigned int retained_count = 0;
	unsigned int old_count = sfcp_allocated_pages;
	unsigned int write_index = 0;
	unsigned int bit;
	unsigned int i;

	retained = kvmalloc_array(SFBS_MAX_JOINT_RETAINED,
				  sizeof(*retained), GFP_KERNEL);
	if (!retained)
		return -ENOMEM;
	for (i = 0; i < active_items; ++i)
		retained[retained_count++] = base_group[i].page;
	retained[retained_count++] =
		base_group[SFBS_STIMULUS_SLOTS].page;
	if (extra_group) {
		for (i = 0; i < extra_items; ++i)
			retained[retained_count++] = extra_group[i].page;
		retained[retained_count++] =
			extra_group[SFBS_STIMULUS_SLOTS].page;
	}
	for (bit = bit_first; bit <= bit_last; ++bit) {
		if (probe_available[bit])
			retained[retained_count++] = probe_lines[bit].page;
	}
	for (bit = bit_first; bit <= bit_last; ++bit) {
		const struct sfcp_line *group;

		if (!joint_available[bit])
			continue;
		group = &joint_groups[bit * SFBS_GROUP_LINES];
		for (i = 0; i < active_items; ++i)
			retained[retained_count++] = group[i].page;
		retained[retained_count++] =
			group[SFBS_STIMULUS_SLOTS].page;
	}
	if (retained_count > SFBS_MAX_JOINT_RETAINED) {
		kvfree(retained);
		return -EOVERFLOW;
	}
	sort(retained, retained_count, sizeof(*retained),
	     sfbs_page_compare, NULL);
	for (i = 0; i < old_count; ++i) {
		struct page *page = sfcp_pages[i];

		if (sfbs_page_is_retained(page, retained, retained_count))
			sfcp_pages[write_index++] = page;
		else
			__free_page(page);
	}
	sfcp_allocated_pages = write_index;
	pr_info(SFCP_NAME ": released %u unused pages; retaining %u owned pages for joint bit scan\n",
		old_count - write_index, write_index);
	kvfree(retained);
	return 0;
}

static bool sfbs_probe_aliases_stimulus(const struct sfcp_line *probe,
					const struct sfcp_line *group,
					unsigned int active_items)
{
	phys_addr_t probe_pa = sfcp_physical(probe);
	unsigned int i;

	for (i = 0; i < active_items; ++i) {
		if (sfcp_physical(&group[i]) == probe_pa)
			return true;
	}
	return false;
}

static int sfbs_release_unused_pages(const struct sfcp_line *group,
				     unsigned int active_items,
				     const struct sfcp_line *flipped,
				     const bool *available)
{
	struct page **retained;
	unsigned int retained_count = 0;
	unsigned int old_count = sfcp_allocated_pages;
	unsigned int write_index = 0;
	unsigned int bit;
	unsigned int i;

	retained = kvmalloc_array(SFBS_MAX_RETAINED_PAGES,
				  sizeof(*retained), GFP_KERNEL);
	if (!retained)
		return -ENOMEM;
	for (i = 0; i < active_items; ++i)
		retained[retained_count++] = group[i].page;
	retained[retained_count++] = group[SFBS_STIMULUS_SLOTS].page;
	for (bit = bit_first; bit <= bit_last; ++bit) {
		if (available[bit])
			retained[retained_count++] = flipped[bit].page;
	}
	if (retained_count > SFBS_MAX_RETAINED_PAGES) {
		kvfree(retained);
		return -EOVERFLOW;
	}
	sort(retained, retained_count, sizeof(*retained),
	     sfbs_page_compare, NULL);
	for (i = 0; i < old_count; ++i) {
		struct page *page = sfcp_pages[i];

		if (sfbs_page_is_retained(page, retained, retained_count))
			sfcp_pages[write_index++] = page;
		else
			__free_page(page);
	}
	sfcp_allocated_pages = write_index;
	pr_info(SFCP_NAME ": released %u unused pages; retaining %u owned pages\n",
		old_count - write_index, write_index);
	kvfree(retained);
	return 0;
}

static int sfbs_release_unused_combo_pages(
		const struct sfcp_line *group, unsigned int active_items,
		const struct sfcp_line *single_lines, const bool *single_available,
		const struct sfbs_pair_target *pairs,
		const struct sfbs_triple_target *triples,
		unsigned int triple_count)
{
	struct page **retained;
	unsigned int retained_count = 0;
	unsigned int old_count = sfcp_allocated_pages;
	unsigned int write_index = 0;
	unsigned int i;
	unsigned int j;

	retained = kvmalloc_array(SFBS_MAX_COMBO_RETAINED,
				  sizeof(*retained), GFP_KERNEL);
	if (!retained)
		return -ENOMEM;
	for (i = 0; i < active_items; ++i)
		retained[retained_count++] = group[i].page;
	retained[retained_count++] = group[SFBS_STIMULUS_SLOTS].page;
	for (i = bit_first; i <= bit_last; ++i) {
		if (single_available[i])
			retained[retained_count++] = single_lines[i].page;
		for (j = i + 1U; j <= bit_last; ++j) {
			const struct sfbs_pair_target *pair =
				&pairs[i * 64U + j];

			if (pair->available)
				retained[retained_count++] = pair->line.page;
		}
	}
	for (i = 0; i < triple_count; ++i) {
		if (triples[i].available)
			retained[retained_count++] = triples[i].line.page;
	}
	if (retained_count > SFBS_MAX_COMBO_RETAINED) {
		kvfree(retained);
		return -EOVERFLOW;
	}
	sort(retained, retained_count, sizeof(*retained),
	     sfbs_page_compare, NULL);
	for (i = 0; i < old_count; ++i) {
		struct page *page = sfcp_pages[i];

		if (sfbs_page_is_retained(page, retained, retained_count))
			sfcp_pages[write_index++] = page;
		else
			__free_page(page);
	}
	sfcp_allocated_pages = write_index;
	pr_info(SFCP_NAME ": released %u unused pages; retaining %u owned pages for pair scan\n",
		old_count - write_index, write_index);
	kvfree(retained);
	return 0;
}

static unsigned int sfbs_build_group(const struct sfcp_line *pool,
				     struct sfcp_line *group)
{
	unsigned int packed = 0;
	unsigned int slot;

	for (slot = 0; slot < SFBS_STIMULUS_SLOTS; ++slot) {
		if (stimulus_mask & BIT_ULL(slot))
			group[packed++] = pool[sfbs_rank[slot]];
	}
	group[SFBS_STIMULUS_SLOTS] =
		pool[sfbs_rank[SFBS_STIMULUS_SLOTS]];
	return packed;
}

static void sfbs_build_unrelated_control(const struct sfcp_line *matching,
					 struct sfcp_line *control,
					 unsigned int active_items,
					 unsigned int control_bit)
{
	unsigned int i;

	for (i = 0; i < active_items; ++i) {
		control[i] = matching[i];
		/* A different line in the same owned page. */
		control[i].offset ^= BIT(control_bit);
	}
	control[SFBS_STIMULUS_SLOTS] =
		matching[SFBS_STIMULUS_SLOTS];
}

static bool sfbs_metric_is_stable(const struct sfbs_metric *metric)
{
	return (u64)metric->successes * 100ULL >=
		(u64)metric->repetitions * stable_percent;
}

static void sfbs_metric_init(struct sfbs_metric *metric,
			     unsigned int repetitions)
{
	memset(metric, 0, sizeof(*metric));
	metric->repetitions = repetitions;
	metric->minimum = U64_MAX;
}

static void sfbs_metric_add(struct sfbs_metric *metric, u64 ticks,
			    const struct sfcp_pmu_delta *pmu)
{
	metric->sum += ticks;
	metric->minimum = min(metric->minimum, ticks);
	metric->maximum = max(metric->maximum, ticks);
	metric->probe_l1d_refill_total += pmu->l1d_refill;
	metric->probe_l1d_tlb_refill_total += pmu->l1d_tlb_refill;
	metric->probe_l2d_refill_total += pmu->l2d_refill;
	metric->probe_bus_access_total += pmu->bus_access;
	metric->probe_ll_cache_rd_total += pmu->ll_cache_rd;
	metric->probe_ll_cache_miss_total += pmu->ll_cache_miss_rd;
	if (pmu->l1d_refill)
		++metric->probe_l1d_refill_samples;
	if (pmu->l1d_tlb_refill)
		++metric->probe_l1d_tlb_refill_samples;
	if (pmu->l2d_refill)
		++metric->probe_l2d_refill_samples;
	if (pmu->ll_cache_rd)
		++metric->probe_ll_cache_rd_samples;
	if (pmu->ll_cache_miss_rd)
		++metric->probe_ll_cache_miss_samples;
	if (ticks < latency_bin1)
		++metric->latency_bins[0];
	else if (ticks < latency_bin2)
		++metric->latency_bins[1];
	else if (ticks < latency_bin3)
		++metric->latency_bins[2];
	else
		++metric->latency_bins[3];
	if (ticks > trigger_threshold)
		++metric->successes;
}

static void sfbs_metric_finish(struct sfbs_metric *metric, u64 *ticks)
{
	metric->mean = div_u64(metric->sum, metric->repetitions);
	sort(ticks, metric->repetitions, sizeof(*ticks),
	     sfbs_u64_compare, NULL);
	metric->median = ticks[(metric->repetitions - 1U) / 2U];
	metric->p90 = ticks[DIV_ROUND_UP(metric->repetitions * 9U, 10U) - 1U];
}

static int sfbs_measure_single(struct sfcp_line *group,
			       unsigned int active_items,
			       unsigned int repetitions,
			       struct sfbs_metric *metric, u64 *ticks)
{
	unsigned int repetition;

	sfbs_metric_init(metric, repetitions);
	for (repetition = 0; repetition < repetitions; ++repetition) {
		struct sfcp_pmu_delta pmu;
		u64 first;
		u64 prefill;
		u64 wait_prefill;

		if (unlikely(kthread_should_stop()))
			return -EINTR;
		ticks[repetition] = sfcp_measure_cross_probe(
			group, active_items, &first, &prefill, &wait_prefill,
			&pmu);
		sfbs_metric_add(metric, ticks[repetition], &pmu);
		if (!(repetition & 0xfU))
			cond_resched();
	}
	sfbs_metric_finish(metric, ticks);
	return 0;
}

static int sfbs_measure_same_single(struct sfcp_line *group,
				     unsigned int active_items,
				     unsigned int repetitions,
				     struct sfbs_metric *metric, u64 *ticks)
{
	unsigned int repetition;

	sfbs_metric_init(metric, repetitions);
	for (repetition = 0; repetition < repetitions; ++repetition) {
		struct sfcp_pmu_delta pmu;
		u64 first;
		u64 prefill;
		u64 wait_prefill;

		if (unlikely(kthread_should_stop()))
			return -EINTR;
		ticks[repetition] = sfcp_measure_same_probe_n(group, active_items,
			&first, &prefill, &wait_prefill, &pmu);
		sfbs_metric_add(metric, ticks[repetition], &pmu);
		if (!(repetition & 0xfU))
			cond_resched();
	}
	sfbs_metric_finish(metric, ticks);
	return 0;
}

static void sfbs_random_group(const struct sfcp_line *pool,
			      unsigned int pool_count,
			      struct sfcp_line *group)
{
	u16 selected[SFBS_GROUP_LINES];
	unsigned int needed = candidate_items + 1U;
	unsigned int used = 0;

	while (used < needed) {
		unsigned int index = sfcp_random_below(pool_count);
		unsigned int i;
		bool duplicate = false;

		for (i = 0; i < used; ++i) {
			if (selected[i] == index) {
				duplicate = true;
				break;
			}
		}
		if (duplicate)
			continue;
		selected[used++] = index;
	}
	for (used = 0; used < candidate_items; ++used)
		group[used] = pool[selected[used]];
	group[SFBS_STIMULUS_SLOTS] = pool[selected[candidate_items]];
}

static int sfbs_find_stable_group(const struct sfcp_line *pool,
				  unsigned int pool_count,
				  struct sfcp_line *group, u64 *ticks,
				  struct sfbs_metric *validation,
				  unsigned int *selected_attempt)
{
	struct sfbs_metric trial;
	unsigned int attempt;
	int ret;

	for (attempt = 1U; attempt <= search_groups; ++attempt) {
		sfbs_random_group(pool, pool_count, group);
		ret = sfbs_measure_single(group, candidate_items,
					  search_repetitions, &trial, ticks);
		if (ret)
			return ret;
		if (!sfbs_metric_is_stable(&trial))
			continue;
		ret = sfbs_measure_single(group, candidate_items,
					  baseline_repetitions, validation,
					  ticks);
		if (ret)
			return ret;
		pr_info(SFCP_NAME ": candidate attempt=%u short=%u/%u long=%u/%u\n",
			attempt, trial.successes, trial.repetitions,
			validation->successes, validation->repetitions);
		if (sfbs_metric_is_stable(validation)) {
			*selected_attempt = attempt;
			return 0;
		}
	}
	pr_err(SFCP_NAME ": no %u-address group passed %u%% in %u attempts; increase candidate_items or search_groups, or change fill_rounds\n",
	       candidate_items, stable_percent, search_groups);
	return -ENOENT;
}

static void sfbs_group_without(const struct sfcp_line *group,
			       unsigned int active_items,
			       unsigned int removed_index,
			       struct sfcp_line *reduced)
{
	unsigned int read_index;
	unsigned int write_index = 0;

	for (read_index = 0; read_index < active_items; ++read_index) {
		if (read_index != removed_index)
			reduced[write_index++] = group[read_index];
	}
	reduced[SFBS_STIMULUS_SLOTS] = group[SFBS_STIMULUS_SLOTS];
}

static int sfbs_reduce_candidate_group(
		struct sfcp_line *group, struct sfcp_line *scratch,
		unsigned int *active_items, u64 *ticks,
		struct sfbs_reduction_step *steps, unsigned int *step_count,
		struct sfbs_metric *final_validation)
{
	int ret;

	*step_count = 0;
	while (*active_items > target_candidate_items) {
		unsigned int start = sfcp_random_below(*active_items);
		unsigned int offset;
		bool removed = false;

		for (offset = 0; offset < *active_items; ++offset) {
			unsigned int index = (start + offset) % *active_items;
			struct sfbs_metric short_metric;
			struct sfbs_metric validation_metric;

			sfbs_group_without(group, *active_items, index, scratch);
			ret = sfbs_measure_single(scratch, *active_items - 1U,
						  reduction_repetitions,
						  &short_metric, ticks);
			if (ret)
				return ret;
			if (!sfbs_metric_is_stable(&short_metric))
				continue;
			ret = sfbs_measure_single(scratch, *active_items - 1U,
						  reduction_validation_repetitions,
						  &validation_metric, ticks);
			if (ret)
				return ret;
			if (!sfbs_metric_is_stable(&validation_metric))
				continue;
			steps[*step_count].removed_pa =
				sfcp_physical(&group[index]);
			steps[*step_count].before_items = *active_items;
			steps[*step_count].after_items = *active_items - 1U;
			steps[*step_count].short_metric = short_metric;
			steps[*step_count].validation_metric = validation_metric;
			++*step_count;
			memcpy(group, scratch, SFBS_GROUP_LINES * sizeof(*group));
			--*active_items;
			removed = true;
			pr_info(SFCP_NAME ": reduction accepted step=%u removed=0x%llx candidates=%u short=%u/%u validation=%u/%u\n",
				*step_count,
				(unsigned long long)steps[*step_count - 1U].removed_pa,
				*active_items, short_metric.successes,
				short_metric.repetitions,
				validation_metric.successes,
				validation_metric.repetitions);
			break;
		}
		if (!removed) {
			pr_err(SFCP_NAME ": reduction stopped at %u candidates; no single deletion passed %u%%\n",
			       *active_items, stable_percent);
			return -ENOENT;
		}
	}
	ret = sfbs_measure_single(group, *active_items,
				  baseline_repetitions,
				  final_validation, ticks);
	if (ret)
		return ret;
	if (!sfbs_metric_is_stable(final_validation)) {
		pr_err(SFCP_NAME ": reduced %u-candidate set failed final validation: %u/%u\n",
		       *active_items, final_validation->successes,
		       final_validation->repetitions);
		return -EAGAIN;
	}
	return 0;
}

/*
 * Reconstruct one genuinely nested sweep after reduction.  The prefix ending
 * at target_candidate_items is the final long-validated group.  Addresses
 * removed by the reducer are appended in reverse order, so every N+1 row is
 * exactly the N row plus one address and N=candidate_items reconstructs the
 * initially selected group.
 */
static int sfbs_build_nested_sweep_group(
		const struct sfcp_line *reduced_group, unsigned int active_items,
		const struct sfbs_reduction_step *steps, unsigned int step_count,
		struct sfcp_line *sweep_group)
{
	unsigned int used;
	unsigned int step;

	memcpy(sweep_group, reduced_group,
	       active_items * sizeof(*sweep_group));
	used = active_items;
	for (step = step_count; step > 0U; --step) {
		if (!sfbs_find_owned_line(steps[step - 1U].removed_pa,
					  &sweep_group[used]))
			return -ENOSPC;
		++used;
	}
	sweep_group[SFBS_STIMULUS_SLOTS] =
		reduced_group[SFBS_STIMULUS_SLOTS];
	return used == candidate_items ? 0 : -EINVAL;
}

static void sfbs_shuffle_sweep_stimuli(struct sfcp_line *group,
		unsigned int items)
{
	unsigned int i;

	/* Fisher-Yates: every pass gets a different nested-prefix ordering. */
	for (i = items; i > 1U; --i) {
		unsigned int other = sfcp_random_below(i);
		struct sfcp_line temporary = group[i - 1U];

		group[i - 1U] = group[other];
		group[other] = temporary;
	}
}

static int sfbs_reduce_color_group(
		struct sfcp_line *base_group, struct sfcp_line *scratch,
		struct sfcp_line *color_groups, unsigned int *active_items,
		u64 *ticks, struct sfbs_reduction_step *steps,
		unsigned int *step_count,
		struct sfbs_metric *final_diagonals)
{
	int ret;

	*step_count = 0U;
	while (*active_items > target_candidate_items) {
		unsigned int start = sfcp_random_below(*active_items);
		unsigned int offset;
		bool removed = false;

		for (offset = 0; offset < *active_items; ++offset) {
			struct sfbs_metric short_metrics[SFBS_COLOR_COUNT];
			struct sfbs_metric validation_metrics[SFBS_COLOR_COUNT];
			unsigned int index = (start + offset) % *active_items;
			unsigned int color;
			unsigned int worst_short = 0U;
			unsigned int worst_validation = 0U;
			bool all_stable = true;

			sfbs_group_without(base_group, *active_items, index, scratch);
			ret = sfbs_build_color_groups(scratch, *active_items - 1U,
				color_groups);
			if (ret)
				return ret;
			for (color = 0; color < SFBS_COLOR_COUNT; ++color) {
				struct sfcp_line *group =
					&color_groups[color * SFBS_GROUP_LINES];

				ret = sfbs_measure_single(group, *active_items - 1U,
					reduction_repetitions, &short_metrics[color], ticks);
				if (ret)
					return ret;
				if (short_metrics[color].successes <
				    short_metrics[worst_short].successes)
					worst_short = color;
				if (!sfbs_metric_is_stable(&short_metrics[color])) {
					all_stable = false;
					break;
				}
			}
			if (!all_stable)
				continue;
			for (color = 0; color < SFBS_COLOR_COUNT; ++color) {
				struct sfcp_line *group =
					&color_groups[color * SFBS_GROUP_LINES];

				ret = sfbs_measure_single(group, *active_items - 1U,
					reduction_validation_repetitions,
					&validation_metrics[color], ticks);
				if (ret)
					return ret;
				if (validation_metrics[color].successes <
				    validation_metrics[worst_validation].successes)
					worst_validation = color;
				if (!sfbs_metric_is_stable(&validation_metrics[color])) {
					all_stable = false;
					break;
				}
			}
			if (!all_stable)
				continue;
			steps[*step_count].removed_pa =
				sfcp_physical(&base_group[index]);
			steps[*step_count].before_items = *active_items;
			steps[*step_count].after_items = *active_items - 1U;
			steps[*step_count].short_metric = short_metrics[worst_short];
			steps[*step_count].validation_metric =
				validation_metrics[worst_validation];
			++*step_count;
			memcpy(base_group, scratch,
			       SFBS_GROUP_LINES * sizeof(*base_group));
			--*active_items;
			removed = true;
			pr_info(SFCP_NAME ": color reduction step=%u candidates=%u worst_short=%u/%u worst_validation=%u/%u\n",
				*step_count, *active_items,
				short_metrics[worst_short].successes,
				short_metrics[worst_short].repetitions,
				validation_metrics[worst_validation].successes,
				validation_metrics[worst_validation].repetitions);
			break;
		}
		if (!removed) {
			pr_err(SFCP_NAME ": four-color reduction stopped at %u candidates\n",
			       *active_items);
			return -ENOENT;
		}
	}
	ret = sfbs_build_color_groups(base_group, *active_items, color_groups);
	if (ret)
		return ret;
	{
	unsigned int color;

	for (color = 0; color < SFBS_COLOR_COUNT; ++color) {
		struct sfcp_line *group =
			&color_groups[color * SFBS_GROUP_LINES];

		ret = sfbs_measure_single(group, *active_items,
			baseline_repetitions, &final_diagonals[color], ticks);
		if (ret)
			return ret;
		if (!sfbs_metric_is_stable(&final_diagonals[color]))
			return -EAGAIN;
	}
	}
	return 0;
}

static int sfbs_measure_pair(struct sfcp_line *original_group,
			     struct sfcp_line *flipped_group,
			     unsigned int active_items,
			     unsigned int repetitions,
			     struct sfbs_metric *original_metric,
			     struct sfbs_metric *flipped_metric,
			     u64 *original_ticks, u64 *flipped_ticks)
{
	unsigned int repetition;

	sfbs_metric_init(original_metric, repetitions);
	sfbs_metric_init(flipped_metric, repetitions);
	for (repetition = 0; repetition < repetitions; ++repetition) {
		struct sfcp_pmu_delta pmu;
		u64 first;
		u64 prefill;
		u64 wait_prefill;

		if (unlikely(kthread_should_stop()))
			return -EINTR;
		if (!(repetition & 1U)) {
			original_ticks[repetition] = sfcp_measure_cross_probe(
				original_group, active_items, &first, &prefill,
				&wait_prefill, &pmu);
			sfbs_metric_add(original_metric,
					original_ticks[repetition], &pmu);
			flipped_ticks[repetition] = sfcp_measure_cross_probe(
				flipped_group, active_items, &first, &prefill,
				&wait_prefill, &pmu);
			sfbs_metric_add(flipped_metric,
					flipped_ticks[repetition], &pmu);
		} else {
			flipped_ticks[repetition] = sfcp_measure_cross_probe(
				flipped_group, active_items, &first, &prefill,
				&wait_prefill, &pmu);
			sfbs_metric_add(flipped_metric,
					flipped_ticks[repetition], &pmu);
			original_ticks[repetition] = sfcp_measure_cross_probe(
				original_group, active_items, &first, &prefill,
				&wait_prefill, &pmu);
			sfbs_metric_add(original_metric,
					original_ticks[repetition], &pmu);
		}
		if (!(repetition & 0xfU))
			cond_resched();
	}
	sfbs_metric_finish(original_metric, original_ticks);
	sfbs_metric_finish(flipped_metric, flipped_ticks);
	return 0;
}

static void sfbs_scrub_group_union(struct sfcp_line **groups,
		unsigned int group_count, unsigned int active_items)
{
	unsigned long irq_flags;
	unsigned int group;
	unsigned int item;

	preempt_disable();
	local_irq_save(irq_flags);
	for (group = 0; group < group_count; ++group) {
		for (item = 0; item < active_items; ++item)
			sfcp_civac(sfcp_address(&groups[group][item]));
		sfcp_civac(sfcp_address(
			&groups[group][SFBS_STIMULUS_SLOTS]));
	}
	dsb(ish);
	isb();
	local_irq_restore(irq_flags);
	preempt_enable();
}

static int sfbs_measure_cross_pair_clean(struct sfcp_line *original_group,
		struct sfcp_line *probe_only_group, unsigned int active_items,
		unsigned int repetitions, unsigned int phase,
		struct sfbs_metric *original_metric,
		struct sfbs_metric *probe_only_metric,
		u64 *original_ticks, u64 *probe_only_ticks)
{
	struct sfcp_line *groups[2] = { original_group, probe_only_group };
	struct sfbs_metric *metrics[2] = {
		original_metric, probe_only_metric
	};
	u64 *ticks[2] = { original_ticks, probe_only_ticks };
	unsigned int repetition;
	unsigned int index;

	for (index = 0; index < 2U; ++index)
		sfbs_metric_init(metrics[index], repetitions);
	for (repetition = 0; repetition < repetitions; ++repetition) {
		unsigned int first_index = (repetition + phase) % 2U;
		unsigned int step;

		if (unlikely(kthread_should_stop()))
			return -EINTR;
		for (step = 0; step < 2U; ++step) {
			struct sfcp_pmu_delta pmu;
			u64 first;
			u64 prefill;
			u64 wait_prefill;

			index = (first_index + step) % 2U;
			sfbs_scrub_group_union(groups, 2U, active_items);
			ticks[index][repetition] = sfcp_measure_cross_probe(
				groups[index], active_items, &first, &prefill,
				&wait_prefill, &pmu);
			sfbs_metric_add(metrics[index], ticks[index][repetition],
				&pmu);
		}
		if (!(repetition & 0xfU))
			cond_resched();
	}
	for (index = 0; index < 2U; ++index)
		sfbs_metric_finish(metrics[index], ticks[index]);
	return 0;
}

static int sfbs_measure_triplet(struct sfcp_line *original_group,
		struct sfcp_line *probe_only_group,
		struct sfcp_line *joint_group, unsigned int active_items,
		unsigned int repetitions, unsigned int phase, bool cross,
		struct sfbs_metric *original_metric,
		struct sfbs_metric *probe_only_metric,
		struct sfbs_metric *joint_metric, u64 *original_ticks,
		u64 *probe_only_ticks, u64 *joint_ticks)
{
	struct sfcp_line *groups[3] = {
		original_group, probe_only_group, joint_group
	};
	struct sfbs_metric *metrics[3] = {
		original_metric, probe_only_metric, joint_metric
	};
	u64 *ticks[3] = { original_ticks, probe_only_ticks, joint_ticks };
	unsigned int repetition;
	unsigned int index;

	/*
	 * The preceding v12 code only flushed the condition being measured.
	 * That allowed the other two color variants to survive in a shared cache,
	 * producing order-dependent 1/3 and 2/3 success plateaus.  Flush the union
	 * before every cell so the randomized order is only a drift control.
	 */

	for (index = 0; index < 3U; ++index)
		sfbs_metric_init(metrics[index], repetitions);
	for (repetition = 0; repetition < repetitions; ++repetition) {
		unsigned int first_index = (repetition + phase) % 3U;
		unsigned int step;

		if (unlikely(kthread_should_stop()))
			return -EINTR;
		for (step = 0; step < 3U; ++step) {
			struct sfcp_pmu_delta pmu;
			u64 first;
			u64 prefill;
			u64 wait_prefill;

			index = (first_index + step) % 3U;
			sfbs_scrub_group_union(groups, 3U, active_items);
			if (cross)
				ticks[index][repetition] = sfcp_measure_cross_probe(
					groups[index], active_items, &first, &prefill,
					&wait_prefill, &pmu);
			else
				ticks[index][repetition] = sfcp_measure_same_probe_n(
					groups[index], active_items, &first, &prefill,
					&wait_prefill, &pmu);
			sfbs_metric_add(metrics[index], ticks[index][repetition],
					&pmu);
		}
		if (!(repetition & 0xfU))
			cond_resched();
	}
	for (index = 0; index < 3U; ++index)
		sfbs_metric_finish(metrics[index], ticks[index]);
	return 0;
}

static int sfbs_measure_locality_quad(struct sfcp_line *baseline_group,
		struct sfcp_line *candidate_group, unsigned int active_items,
		unsigned int repetitions, unsigned int phase,
		struct sfbs_metric *metrics, u64 **ticks)
{
	struct sfcp_line *groups[2] = { baseline_group, candidate_group };
	unsigned int repetition;
	unsigned int index;

	for (index = 0; index < 4U; ++index)
		sfbs_metric_init(&metrics[index], repetitions);
	for (repetition = 0; repetition < repetitions; ++repetition) {
		unsigned int first_index = (repetition + phase) % 4U;
		unsigned int step;

		if (unlikely(kthread_should_stop()))
			return -EINTR;
		for (step = 0; step < 4U; ++step) {
			struct sfcp_line *group;
			struct sfcp_pmu_delta pmu;
			u64 first;
			u64 prefill;
			u64 wait_prefill;
			bool cross;

			index = (first_index + step) % 4U;
			cross = index >= 2U;
			group = (index & 1U) ? candidate_group : baseline_group;
			sfbs_scrub_group_union(groups, 2U, active_items);
			if (cross)
				ticks[index][repetition] = sfcp_measure_cross_probe(
					group, active_items, &first, &prefill,
					&wait_prefill, &pmu);
			else
				ticks[index][repetition] = sfcp_measure_same_probe_n(
					group, active_items, &first, &prefill,
					&wait_prefill, &pmu);
			sfbs_metric_add(&metrics[index], ticks[index][repetition],
					&pmu);
		}
		if (!(repetition & 0xfU))
			cond_resched();
	}
	for (index = 0; index < 4U; ++index)
		sfbs_metric_finish(&metrics[index], ticks[index]);
	return 0;
}

static int sfbs_write_header(struct file *file, loff_t *position,
			     unsigned int pool_count, unsigned int active_items,
			     phys_addr_t base_probe_pa,
			     unsigned int selected_attempt)
{
	char *buffer;
	char sweep_rounds_text[96];
	size_t sweep_rounds_used = 0;
	size_t used;
	unsigned int round_index;
	int ret;

	for (round_index = 0; round_index < item_sweep_rounds_count;
	     ++round_index)
		sweep_rounds_used += scnprintf(
			sweep_rounds_text + sweep_rounds_used,
			sizeof(sweep_rounds_text) - sweep_rounds_used,
			"%s%u", round_index ? ";" : "",
			item_sweep_rounds[round_index]);

	buffer = kmalloc(6144U, GFP_KERNEL);
	if (!buffer)
		return -ENOMEM;

	used = scnprintf(buffer, 6144U,
		"# module,%s\n"
		"# schema_version,18\n"
		"# experiment_run_id,%s\n"
		"# experiment,%s\n"
		"# scan_order,%u\n"
		"# allocation_scope,one_module_load_one_fresh_owned_page_pool\n"
		"# owned_pages_only,true\n"
		"# computed_physical_addresses_are_lookup_keys_only,true\n"
		"# arbitrary_physical_address_mapping,false\n"
		"# counter,%s\n"
		"# counter_calibration_delta,%llu\n"
		"# counter_calibration_method,civac_then_single_touch_of_one_owned_line\n"
		"# counter_calibration_usage,counter_sanity_check_only_not_subtracted_from_probe_ticks\n"
		"# probe_pmu_events_available,%s\n"
		"# ll_cache_events_available,%s\n"
		"# l1d_tlb_refill_event_available,%s\n"
		"# probe_pmu_event_l1d_tlb_refill,0x05\n"
		"# probe_pmu_event_ll_cache_rd,0x36\n"
		"# probe_pmu_event_ll_cache_miss_rd,0x37\n"
		"# probe_pmu_event_source,Arm_architected_common_event_numbers\n"
		"# probe_cpu,%d\n"
		"# stimulus_cpus,%s\n"
		"# same_core_binding,probe_and_candidate_accesses_execute_on_probe_cpu\n"
		"# cross_core_binding,probe_executes_on_probe_cpu_and_candidate_accesses_execute_on_stimulus_cpus\n"
		"# pool_pages,%u\n"
		"# candidate_target_masked,0x%llx\n"
		"# addrmask,0x%llx\n"
		"# owned_candidate_pool,%u\n"
		"# stimulus_mask,0x%llx\n"
		"# initial_candidate_items,%u\n"
		"# target_candidate_items,%u\n"
		"# active_candidates,%u\n"
		"# candidate_selection,%s\n"
		"# candidate_search_attempt,%u\n"
		"# search_groups,%u\n"
		"# search_repetitions,%u\n"
		"# baseline_repetitions,%u\n"
		"# reduction_repetitions,%u\n"
		"# reduction_validation_repetitions,%u\n"
		"# scan_passes,%u\n"
		"# color_bit0,%u\n"
		"# color_bit1,%u\n"
		"# color_repetitions,%u\n"
		"# base_probe_pa,0x%llx\n"
		"# bit_first,%u\n"
		"# bit_last,%u\n"
		"# fill_rounds,%u\n"
		"# fill_rounds_scope,candidate_search_reduction_validation_and_bit_tests\n"
		"# bit_repetitions,%u\n"
		"# item_sweep_first,%u\n"
		"# item_sweep_last,%u\n"
		"# item_count_definition,stimulus_addresses_excluding_probe\n"
		"# item_sweep_repetitions,%u\n"
		"# item_sweep_rounds,%s\n"
		"# item_sweep_rounds_scope,effective_values_for_item_sweep_each_row_records_one_as_fill_rounds\n"
		"# item_sweep_order,descending_from_item_sweep_last_to_item_sweep_first\n"
		"# bit_item_sweep_conditions,same_original_matching;same_probe_only_flipped;cross_original_matching;cross_probe_only_flipped\n"
		"# bit_item_sweep_candidate_order,one_random_permutation_per_scan_pass_with_nested_prefixes_within_each_pass\n"
		"# attribution_control_repetitions,%u\n"
		"# attribution_cross_only_bits,%s\n"
		"# joint_rescue_mask,0x%llx\n"
		"# pair_screen_repetitions,%u\n"
		"# pair_screen_percent,%u\n"
		"# single_max_percent,%u\n"
		"# triple_screen_repetitions,%u\n"
		"# triple_screen_percent,%u\n"
		"# pair_max_percent,%u\n"
		"# trigger_threshold,%u\n"
		"# stable_percent,%u\n"
		"# latency_bins,0..%u;%u..%u;%u..%u;%u..inf\n"
		"# cache_line_bytes,64\n"
		"# intra_line_negative_control_bits,0..5\n"
		"# cross_bit_conditions,original_matching;probe_only_flipped\n"
		"# joint_bit_conditions,joint_group_flipped_for_joint_rescue_mask\n"
		"# selector_rescue_rule,original_and_joint_at_least_stable_percent_and_probe_only_at_most_single_max_percent\n"
		"# selection_validation,matching_candidate_set_before_scan\n"
		"# unrelated_control,same_owned_page_control_bit_not_in_changed_mask\n"
		"# idle_control,stimulus_handshake_with_zero_address_accesses\n"
		"record,scan_pass,test_bit,test_bit2,test_bit3,variant,available,lookup_status,active_candidates,fill_rounds,successes,repetitions,success_percent,stable,mean_ticks,median_ticks,p90_ticks,min_ticks,max_ticks,probe_l1d_refill_total,probe_l1d_tlb_refill_total,probe_l2d_refill_total,probe_bus_access_total,probe_l1d_refill_samples,probe_l1d_tlb_refill_samples,probe_l2d_refill_samples,probe_ll_cache_rd_total,probe_ll_cache_miss_total,probe_ll_cache_rd_samples,probe_ll_cache_miss_samples,latency_lt_bin1,latency_bin1_bin2,latency_bin2_bin3,latency_ge_bin3,probe_pa,paired_probe_pa,changed_mask\n",
		SFCP_NAME, experiment_run_id,
		scan_order == 8U ? "owned_same_cross_item_sweep_only" :
		scan_order == 7U ? "owned_per_bit_same_cross_item_sweep" :
		scan_order == 6U ? "owned_same_cross_sf_attribution" :
		scan_order == 5U ? "owned_probe_and_candidate_joint_bit_scan" :
		scan_order == 4U ? "owned_address_color_matrix" :
		scan_order == 3U ? "owned_probe_triple_bit_xor_rescue" :
			scan_order == 2U ? "owned_probe_double_bit_xor_rescue" :
				"owned_probe_single_bit_sensitivity",
		scan_order, sfcp_counter_name(),
		(unsigned long long)sfcp_counter_calibration_delta,
		sfcp_probe_events_available ? "true" : "false",
		sfcp_ll_events_available ? "true" : "false",
		sfcp_l1d_tlb_event_available ? "true" : "false",
		probe_cpu, stimulus_cpus, pool_pages,
		(unsigned long long)target_low,
		(unsigned long long)sfcp_active_addrmask, pool_count,
		(unsigned long long)stimulus_mask, candidate_items,
		target_candidate_items, active_items,
		auto_find_stable ? "online_random_validated" : "fixed_rank_mask",
		selected_attempt, search_groups, search_repetitions,
		baseline_repetitions, reduction_repetitions,
		reduction_validation_repetitions, scan_passes,
		color_bit0, color_bit1, color_repetitions,
		(unsigned long long)base_probe_pa, bit_first, bit_last,
		fill_rounds, bit_repetitions, item_sweep_first,
		item_sweep_last, item_sweep_repetitions,
		sweep_rounds_text,
		attribution_control_repetitions,
		attribution_cross_only_bits ? "true" : "false",
		(unsigned long long)joint_rescue_mask,
		pair_screen_repetitions,
		pair_screen_percent, single_max_percent,
		triple_screen_repetitions, triple_screen_percent,
		pair_max_percent,
		trigger_threshold, stable_percent,
		latency_bin1 - 1U, latency_bin1, latency_bin2 - 1U,
		latency_bin2, latency_bin3 - 1U, latency_bin3);
	if (used >= 6143U)
		ret = -EOVERFLOW;
	else
		ret = sfcp_write_all(file, buffer, used, position);
	kfree(buffer);
	return ret;
}

static int sfbs_write_metric(struct file *file, loff_t *position,
			     const char *record, unsigned int bit,
			     unsigned int bit2,
			     unsigned int bit3,
			     const char *variant, bool available,
			     const char *lookup_status,
			     unsigned int active_items,
			     const struct sfbs_metric *metric,
			     phys_addr_t probe_pa, phys_addr_t paired_pa,
			     u64 changed_mask)
{
	char row[1024];
	bool stable = metric && sfbs_metric_is_stable(metric);
	size_t used;

	used = scnprintf(row, sizeof(row),
		"%s,%u,%u,%u,%u,%s,%u,%s,%u,%u,%u,%u,%u,%u,"
		"%llu,%llu,%llu,%llu,%llu,"
		"%llu,%llu,%llu,%llu,%u,%u,%u,"
		"%llu,%llu,%u,%u,"
		"%u,%u,%u,%u,0x%llx,0x%llx,0x%llx\n",
		record, sfbs_current_pass, bit, bit2, bit3, variant,
		available, lookup_status,
		active_items,
		fill_rounds,
		metric ? metric->successes : 0U,
		metric ? metric->repetitions : 0U,
		metric && metric->repetitions ?
			(metric->successes * 100U) / metric->repetitions : 0U,
		stable,
		(unsigned long long)(metric ? metric->mean : 0),
		(unsigned long long)(metric ? metric->median : 0),
		(unsigned long long)(metric ? metric->p90 : 0),
		(unsigned long long)(metric ? metric->minimum : 0),
		(unsigned long long)(metric ? metric->maximum : 0),
		(unsigned long long)(metric ? metric->probe_l1d_refill_total : 0),
		(unsigned long long)(metric ? metric->probe_l1d_tlb_refill_total : 0),
		(unsigned long long)(metric ? metric->probe_l2d_refill_total : 0),
		(unsigned long long)(metric ? metric->probe_bus_access_total : 0),
		metric ? metric->probe_l1d_refill_samples : 0U,
		metric ? metric->probe_l1d_tlb_refill_samples : 0U,
		metric ? metric->probe_l2d_refill_samples : 0U,
		(unsigned long long)(metric ? metric->probe_ll_cache_rd_total : 0),
		(unsigned long long)(metric ? metric->probe_ll_cache_miss_total : 0),
		metric ? metric->probe_ll_cache_rd_samples : 0U,
		metric ? metric->probe_ll_cache_miss_samples : 0U,
		metric ? metric->latency_bins[0] : 0U,
		metric ? metric->latency_bins[1] : 0U,
		metric ? metric->latency_bins[2] : 0U,
		metric ? metric->latency_bins[3] : 0U,
		(unsigned long long)probe_pa,
		(unsigned long long)paired_pa,
		(unsigned long long)changed_mask);
	if (used >= sizeof(row) - 1U)
		return -EOVERFLOW;
	return sfcp_write_all(file, row, used, position);
}

static int sfbs_write_candidates(struct file *file, loff_t *position,
				 const struct sfcp_line *group,
				 unsigned int active_items,
				 phys_addr_t base_probe_pa)
{
	unsigned int i;
	int ret = 0;

	for (i = 0; !ret && i < active_items; ++i)
		ret = sfbs_write_metric(file, position, "candidate_member",
			i, SFBS_NO_SECOND_BIT, SFBS_NO_SECOND_BIT,
			"stimulus", true, "owned_exact", active_items, NULL,
			sfcp_physical(&group[i]), base_probe_pa, 0ULL);
	return ret;
}

static int sfbs_write_reduction_trace(
		struct file *file, loff_t *position,
		const struct sfbs_reduction_step *steps,
		unsigned int step_count, phys_addr_t base_probe_pa)
{
	unsigned int i;
	int ret = 0;

	sfbs_current_pass = 0U;
	for (i = 0; !ret && i < step_count; ++i) {
		char short_variant[48];
		char validation_variant[48];

		scnprintf(short_variant, sizeof(short_variant),
			  "drop_short_%u_to_%u", steps[i].before_items,
			  steps[i].after_items);
		scnprintf(validation_variant, sizeof(validation_variant),
			  "drop_validation_%u_to_%u", steps[i].before_items,
			  steps[i].after_items);
		ret = sfbs_write_metric(file, position, "candidate_reduction",
			i + 1U, SFBS_NO_SECOND_BIT, SFBS_NO_SECOND_BIT,
			short_variant, true, "removed_owned_candidate",
			steps[i].after_items, &steps[i].short_metric,
			steps[i].removed_pa, base_probe_pa, 0ULL);
		if (!ret)
			ret = sfbs_write_metric(file, position,
				"candidate_reduction", i + 1U,
				SFBS_NO_SECOND_BIT, SFBS_NO_SECOND_BIT,
				validation_variant, true,
				"removed_owned_candidate", steps[i].after_items,
				&steps[i].validation_metric,
				steps[i].removed_pa, base_probe_pa, 0ULL);
	}
	return ret;
}

static int sfbs_measure_write_pair(struct file *file, loff_t *position,
				   unsigned int bit,
				   const char *original_variant,
				   const char *flipped_variant,
				   struct sfcp_line *original_group,
				   struct sfcp_line *flipped_group,
				   unsigned int active_items,
				   phys_addr_t original_pa,
				   phys_addr_t flipped_pa,
				   u64 *original_ticks, u64 *flipped_ticks,
				   u32 *original_successes,
				   u32 *flipped_successes)
{
	struct sfbs_metric original_metric;
	struct sfbs_metric flipped_metric;
	int ret;

	ret = sfbs_measure_pair(original_group, flipped_group, active_items,
				bit_repetitions,
				&original_metric, &flipped_metric,
				original_ticks, flipped_ticks);
	if (ret)
		return ret;
	if (original_successes)
		*original_successes = original_metric.successes;
	if (flipped_successes)
		*flipped_successes = flipped_metric.successes;
	ret = sfbs_write_metric(file, position, "bit_test", bit,
				SFBS_NO_SECOND_BIT,
				SFBS_NO_SECOND_BIT,
				original_variant, true, "owned_exact",
				active_items, &original_metric,
				original_pa, flipped_pa, BIT_ULL(bit));
	if (!ret)
		ret = sfbs_write_metric(file, position, "bit_test", bit,
				SFBS_NO_SECOND_BIT,
				SFBS_NO_SECOND_BIT,
				flipped_variant, true, "owned_exact",
				active_items, &flipped_metric,
				flipped_pa, original_pa, BIT_ULL(bit));
	return ret;
}

static int sfbs_measure_write_pair_n(
		struct file *file, loff_t *position, const char *record,
		unsigned int bit, unsigned int bit2, unsigned int bit3,
		const char *original_variant, const char *changed_variant,
		struct sfcp_line *original_group, struct sfcp_line *changed_group,
		unsigned int active_items, unsigned int repetitions,
		phys_addr_t original_pa, phys_addr_t changed_pa, u64 changed_mask,
		u64 *original_ticks, u64 *changed_ticks,
		struct sfbs_metric *original_metric,
		struct sfbs_metric *changed_metric)
{
	int ret;

	ret = sfbs_measure_pair(original_group, changed_group, active_items,
				repetitions, original_metric, changed_metric,
				original_ticks, changed_ticks);
	if (ret)
		return ret;
	ret = sfbs_write_metric(file, position, record, bit, bit2, bit3,
				original_variant, true, "owned_exact",
				active_items, original_metric,
				original_pa, changed_pa, changed_mask);
	if (!ret)
		ret = sfbs_write_metric(file, position, record, bit, bit2, bit3,
				changed_variant, true, "owned_exact",
				active_items, changed_metric,
				changed_pa, original_pa, changed_mask);
	return ret;
}

static int sfbs_run_single_experiment(void)
{
	struct sfcp_line *pool = NULL;
	struct sfcp_line *original_group = NULL;
	struct sfcp_line *flipped_group = NULL;
	struct sfcp_line *reduction_scratch = NULL;
	struct sfcp_line *original_control = NULL;
	struct sfcp_line *flipped_control = NULL;
	struct sfcp_line *flipped = NULL;
	struct sfbs_reduction_step *reduction_steps = NULL;
	bool *available = NULL;
	bool *candidate_alias = NULL;
	u64 *original_ticks = NULL;
	u64 *flipped_ticks = NULL;
	struct file *file = NULL;
	loff_t position = 0;
	unsigned int pool_count;
	unsigned int active_items;
	unsigned int required_pool;
	unsigned int selected_attempt = 0;
	unsigned int reduction_step_count = 0;
	unsigned int bit;
	unsigned int pass;
	phys_addr_t base_probe_pa;
	struct sfbs_metric baseline_metric;
	struct sfbs_metric reduced_validation;
	unsigned int max_repetitions;
	int ret;

	if (access_items != SFBS_STIMULUS_SLOTS ||
	    sfcp_active_addrmask != 0x1ffffffULL ||
	    bit_first < SFBS_MIN_TEST_BIT || bit_last > SFBS_MAX_TEST_BIT ||
	    bit_first > bit_last ||
	    !bit_repetitions || bit_repetitions > SFBS_MAX_REPETITIONS ||
	    !candidate_items || candidate_items > SFBS_STIMULUS_SLOTS ||
	    !target_candidate_items ||
	    target_candidate_items > candidate_items ||
	    !reduction_repetitions ||
	    reduction_repetitions > SFBS_MAX_REPETITIONS ||
	    !reduction_validation_repetitions ||
	    reduction_validation_repetitions > SFBS_MAX_REPETITIONS ||
	    !scan_passes || scan_passes > 10U ||
	    !search_groups || !search_repetitions ||
	    search_repetitions > SFBS_MAX_REPETITIONS ||
	    !baseline_repetitions ||
	    baseline_repetitions > SFBS_MAX_REPETITIONS ||
	    !trigger_threshold || !stable_percent || stable_percent > 100U ||
	    !stimulus_mask ||
	    (stimulus_mask & ~GENMASK_ULL(SFBS_STIMULUS_SLOTS - 1U, 0U))) {
		pr_err(SFCP_NAME ": requires access_items=%u addrmask=0x1ffffff, bits %u..%u, and valid paired-test parameters\n",
		       SFBS_STIMULUS_SLOTS, SFBS_MIN_TEST_BIT,
		       SFBS_MAX_TEST_BIT);
		return -EINVAL;
	}

	pool = kvmalloc_array(SFBS_POOL_CAPACITY, sizeof(*pool), GFP_KERNEL);
	original_group = kvmalloc_array(SFBS_GROUP_LINES,
					  sizeof(*original_group), GFP_KERNEL);
	flipped_group = kvmalloc_array(SFBS_GROUP_LINES,
					 sizeof(*flipped_group), GFP_KERNEL);
	reduction_scratch = kvmalloc_array(SFBS_GROUP_LINES,
					 sizeof(*reduction_scratch), GFP_KERNEL);
	original_control = kvmalloc_array(SFBS_GROUP_LINES,
					    sizeof(*original_control), GFP_KERNEL);
	flipped_control = kvmalloc_array(SFBS_GROUP_LINES,
					   sizeof(*flipped_control), GFP_KERNEL);
	flipped = kvcalloc(64U, sizeof(*flipped), GFP_KERNEL);
	reduction_steps = kvcalloc(SFBS_STIMULUS_SLOTS,
				   sizeof(*reduction_steps), GFP_KERNEL);
	available = kvcalloc(64U, sizeof(*available), GFP_KERNEL);
	candidate_alias = kvcalloc(64U, sizeof(*candidate_alias), GFP_KERNEL);
	max_repetitions = max(bit_repetitions,
			      max(search_repetitions,
				  max(baseline_repetitions,
				      reduction_validation_repetitions)));
	original_ticks = kvmalloc_array(max_repetitions,
					 sizeof(*original_ticks), GFP_KERNEL);
	flipped_ticks = kvmalloc_array(bit_repetitions,
					sizeof(*flipped_ticks), GFP_KERNEL);
	if (!pool || !original_group || !flipped_group || !reduction_scratch ||
	    !original_control ||
	    !flipped_control || !flipped ||
	    !reduction_steps || !available || !candidate_alias ||
	    !original_ticks || !flipped_ticks) {
		ret = -ENOMEM;
		goto out;
	}
	sort(sfcp_pages, sfcp_allocated_pages, sizeof(*sfcp_pages),
	     sfbs_page_phys_compare, NULL);
	ret = sfbs_collect_pool(pool, &pool_count);
	if (ret)
		goto out;
	required_pool = auto_find_stable ? candidate_items + 1U :
		sfbs_rank[SFBS_STIMULUS_SLOTS] + 1U;
	if (!auto_find_stable) {
		for (bit = 0; bit < SFBS_STIMULUS_SLOTS; ++bit) {
			if (stimulus_mask & BIT_ULL(bit))
				required_pool = max(required_pool,
						    (unsigned int)sfbs_rank[bit] + 1U);
		}
	}
	if (pool_count < required_pool) {
		pr_err(SFCP_NAME ": selected stimulus/probe ranks need %u owned candidates, got %u; increase pool_pages\n",
		       required_pool, pool_count);
		ret = -ENOSPC;
		goto out;
	}
	sfcp_enable_counter();
	if (!sfcp_counter_calibration_delta) {
		ret = -EOPNOTSUPP;
		goto out_counter;
	}
	memset(&baseline_metric, 0, sizeof(baseline_metric));
	if (auto_find_stable) {
		ret = sfbs_find_stable_group(pool, pool_count, original_group,
					     original_ticks, &baseline_metric,
					     &selected_attempt);
		active_items = candidate_items;
	} else {
		active_items = sfbs_build_group(pool, original_group);
		ret = sfbs_measure_single(original_group, active_items,
					  baseline_repetitions,
					  &baseline_metric, original_ticks);
		if (!ret && !sfbs_metric_is_stable(&baseline_metric))
			ret = -EAGAIN;
	}
	if (ret) {
		pr_err(SFCP_NAME ": baseline candidate group is not stable; bit scan aborted (%u/%u)\n",
		       baseline_metric.successes, baseline_metric.repetitions);
		goto out_counter;
	}
	pr_info(SFCP_NAME ": locked candidate-set selection validation with %u candidates: matching=%u/%u\n",
		active_items, baseline_metric.successes,
		baseline_metric.repetitions);
	memset(&reduced_validation, 0, sizeof(reduced_validation));
	ret = sfbs_reduce_candidate_group(original_group, reduction_scratch,
		&active_items, original_ticks, reduction_steps,
		&reduction_step_count, &reduced_validation);
	if (ret)
		goto out_counter;
	memcpy(flipped_group, original_group,
	       SFBS_GROUP_LINES * sizeof(*flipped_group));
	base_probe_pa = sfcp_physical(
		&original_group[SFBS_STIMULUS_SLOTS]);
	for (bit = bit_first; bit <= bit_last; ++bit) {
		phys_addr_t target_pa = base_probe_pa ^ BIT_ULL(bit);

		available[bit] = sfbs_find_owned_line(target_pa, &flipped[bit]);
		if (available[bit] && sfbs_probe_aliases_stimulus(
				&flipped[bit], original_group, active_items)) {
			candidate_alias[bit] = true;
			available[bit] = false;
		}
		pr_info(SFCP_NAME ": bit=%u target=0x%llx owned=%u candidate_alias=%u\n",
			bit, (unsigned long long)target_pa, available[bit],
			candidate_alias[bit]);
	}
	ret = sfbs_release_unused_pages(original_group, active_items,
					flipped, available);
	if (ret)
		goto out_counter;

	file = filp_open(result_path, O_CREAT | O_TRUNC | O_WRONLY, 0644);
	if (IS_ERR(file)) {
		ret = PTR_ERR(file);
		file = NULL;
		goto out_counter;
	}
	ret = sfbs_write_header(file, &position, pool_count, active_items,
				base_probe_pa, selected_attempt);
	if (!ret)
		ret = sfbs_write_candidates(file, &position, original_group,
					    active_items, base_probe_pa);
	if (!ret)
		ret = sfbs_write_metric(file, &position,
			"selection_validation", 0U, SFBS_NO_SECOND_BIT,
			SFBS_NO_SECOND_BIT,
			"initial_matching",
			true, "owned_exact", candidate_items, &baseline_metric,
			base_probe_pa, base_probe_pa, 0ULL);
	if (!ret)
		ret = sfbs_write_reduction_trace(file, &position,
			reduction_steps, reduction_step_count, base_probe_pa);
	if (!ret)
		ret = sfbs_write_metric(file, &position,
			"selection_validation", 0U, SFBS_NO_SECOND_BIT,
			SFBS_NO_SECOND_BIT, "reduced_matching", true,
			"owned_exact", active_items, &reduced_validation,
			base_probe_pa, base_probe_pa, 0ULL);
	for (pass = 1U; !ret && pass <= scan_passes; ++pass) {
		struct sfbs_metric pass_validation;

		memset(&pass_validation, 0, sizeof(pass_validation));
		sfbs_current_pass = pass;
		pr_info(SFCP_NAME ": starting single-bit scan pass %u/%u on fixed %u-candidate set\n",
			pass, scan_passes, active_items);
		ret = sfbs_measure_single(original_group, active_items,
			baseline_repetitions, &pass_validation, original_ticks);
		if (!ret)
			ret = sfbs_write_metric(file, &position,
				"pass_validation", 0U, SFBS_NO_SECOND_BIT,
				SFBS_NO_SECOND_BIT, "fixed_set_matching", true,
				"owned_exact", active_items, &pass_validation,
				base_probe_pa, base_probe_pa, 0ULL);
		pr_info(SFCP_NAME ": single-bit pass %u baseline=%u/%u stable=%u\n",
			pass, pass_validation.successes,
			pass_validation.repetitions,
			sfbs_metric_is_stable(&pass_validation));
	for (bit = bit_first; !ret && bit <= bit_last; ++bit) {
		phys_addr_t target_pa = base_probe_pa ^ BIT_ULL(bit);
		unsigned int unrelated_bit = bit == 6U ? 7U : 6U;
		u32 matching_original = 0;
		u32 matching_flipped = 0;
		u32 unrelated_original = 0;
		u32 unrelated_flipped = 0;
		u32 idle_original = 0;
		u32 idle_flipped = 0;

		if (!available[bit]) {
			ret = sfbs_write_metric(file, &position, "bit_test", bit,
				SFBS_NO_SECOND_BIT,
				SFBS_NO_SECOND_BIT,
				"flipped", false,
				candidate_alias[bit] ? "candidate_alias" : "not_owned",
				active_items, NULL,
				target_pa, base_probe_pa, BIT_ULL(bit));
			continue;
		}
		flipped_group[SFBS_STIMULUS_SLOTS] = flipped[bit];
		sfbs_build_unrelated_control(original_group, original_control,
					     active_items, unrelated_bit);
		memcpy(flipped_control, original_control,
		       SFBS_GROUP_LINES * sizeof(*flipped_control));
		flipped_control[SFBS_STIMULUS_SLOTS] = flipped[bit];
		ret = sfbs_measure_write_pair(file, &position, bit,
			"original_matching", "flipped_matching",
			original_group, flipped_group, active_items,
			base_probe_pa, target_pa, original_ticks,
			flipped_ticks, &matching_original,
			&matching_flipped);
		if (!ret)
			ret = sfbs_measure_write_pair(file, &position, bit,
				"original_unrelated", "flipped_unrelated",
				original_control, flipped_control, active_items,
				base_probe_pa, target_pa, original_ticks,
				flipped_ticks, &unrelated_original,
				&unrelated_flipped);
		if (!ret)
			ret = sfbs_measure_write_pair(file, &position, bit,
				"original_idle", "flipped_idle",
				original_group, flipped_group, 0U,
				base_probe_pa, target_pa, original_ticks,
				flipped_ticks, &idle_original, &idle_flipped);
		pr_info(SFCP_NAME ": bit=%u repetitions=%u matching(o=%u f=%u) unrelated(o=%u f=%u) idle(o=%u f=%u) status=%s\n",
			bit, bit_repetitions, matching_original, matching_flipped,
			unrelated_original, unrelated_flipped,
			idle_original, idle_flipped, ret ? "failed" : "written");
	}
	}
	sfbs_current_pass = 0U;
	filp_close(file, NULL);
	file = NULL;
	if (!ret)
		pr_info(SFCP_NAME ": wrote owned-page bit sensitivity results to %s\n",
			result_path);

out_counter:
	sfcp_restore_counter();
out:
	if (file)
		filp_close(file, NULL);
	kvfree(flipped_ticks);
	kvfree(original_ticks);
	kvfree(reduction_steps);
	kvfree(candidate_alias);
	kvfree(available);
	kvfree(flipped);
	kvfree(flipped_control);
	kvfree(original_control);
	kvfree(flipped_group);
	kvfree(reduction_scratch);
	kvfree(original_group);
	kvfree(pool);
	return ret;
}

static int sfbs_run_pair_experiment(void)
{
	struct sfcp_line *pool = NULL;
	struct sfcp_line *original_group = NULL;
	struct sfcp_line *changed_group = NULL;
	struct sfcp_line *reduction_scratch = NULL;
	struct sfcp_line *original_control = NULL;
	struct sfcp_line *changed_control = NULL;
	struct sfcp_line *single_lines = NULL;
	bool *single_available = NULL;
	bool *single_alias = NULL;
	struct sfbs_metric *single_metrics = NULL;
	struct sfbs_pair_target *pairs = NULL;
	struct sfbs_triple_target *triples = NULL;
	struct sfbs_reduction_step *reduction_steps = NULL;
	u64 *original_ticks = NULL;
	u64 *changed_ticks = NULL;
	struct file *file = NULL;
	loff_t position = 0;
	unsigned int pool_count;
	unsigned int active_items;
	unsigned int required_pool;
	unsigned int selected_attempt = 0;
	unsigned int selected_pairs = 0;
	unsigned int triple_count = 0;
	unsigned int selected_triples = 0;
	unsigned int reduction_step_count = 0;
	unsigned int max_repetitions;
	unsigned int pass;
	unsigned int i;
	unsigned int j;
	unsigned int k;
	phys_addr_t base_probe_pa;
	struct sfbs_metric baseline_metric;
	struct sfbs_metric reduced_validation;
	int ret;

	if (access_items != SFBS_STIMULUS_SLOTS ||
	    sfcp_active_addrmask != 0x1ffffffULL ||
	    bit_first < SFBS_MIN_TEST_BIT || bit_last > SFBS_MAX_TEST_BIT ||
	    bit_first >= bit_last ||
	    !bit_repetitions || bit_repetitions > SFBS_MAX_REPETITIONS ||
	    !pair_screen_repetitions ||
	    pair_screen_repetitions > SFBS_MAX_REPETITIONS ||
	    !pair_screen_percent || pair_screen_percent > 100U ||
	    single_max_percent > 100U ||
	    (scan_order == 3U &&
	     (!triple_screen_repetitions ||
	      triple_screen_repetitions > SFBS_MAX_REPETITIONS ||
	      !triple_screen_percent || triple_screen_percent > 100U ||
	      pair_max_percent > 100U || bit_last - bit_first + 1U < 3U)) ||
	    !candidate_items || candidate_items > SFBS_STIMULUS_SLOTS ||
	    !target_candidate_items ||
	    target_candidate_items > candidate_items ||
	    !reduction_repetitions ||
	    reduction_repetitions > SFBS_MAX_REPETITIONS ||
	    !reduction_validation_repetitions ||
	    reduction_validation_repetitions > SFBS_MAX_REPETITIONS ||
	    !scan_passes || scan_passes > 10U ||
	    !search_groups || !search_repetitions ||
	    search_repetitions > SFBS_MAX_REPETITIONS ||
	    !baseline_repetitions ||
	    baseline_repetitions > SFBS_MAX_REPETITIONS ||
	    !trigger_threshold || !stable_percent || stable_percent > 100U ||
	    !stimulus_mask ||
	    (stimulus_mask & ~GENMASK_ULL(SFBS_STIMULUS_SLOTS - 1U, 0U))) {
		pr_err(SFCP_NAME ": invalid combination-scan parameters for order=%u and bits %u..%u\n",
		       scan_order,
		       SFBS_MIN_TEST_BIT, SFBS_MAX_TEST_BIT);
		return -EINVAL;
	}

	pool = kvmalloc_array(SFBS_POOL_CAPACITY, sizeof(*pool), GFP_KERNEL);
	original_group = kvmalloc_array(SFBS_GROUP_LINES,
					 sizeof(*original_group), GFP_KERNEL);
	changed_group = kvmalloc_array(SFBS_GROUP_LINES,
					sizeof(*changed_group), GFP_KERNEL);
	reduction_scratch = kvmalloc_array(SFBS_GROUP_LINES,
					sizeof(*reduction_scratch), GFP_KERNEL);
	original_control = kvmalloc_array(SFBS_GROUP_LINES,
					   sizeof(*original_control), GFP_KERNEL);
	changed_control = kvmalloc_array(SFBS_GROUP_LINES,
					  sizeof(*changed_control), GFP_KERNEL);
	single_lines = kvcalloc(64U, sizeof(*single_lines), GFP_KERNEL);
	single_available = kvcalloc(64U, sizeof(*single_available), GFP_KERNEL);
	single_alias = kvcalloc(64U, sizeof(*single_alias), GFP_KERNEL);
	single_metrics = kvcalloc(64U, sizeof(*single_metrics), GFP_KERNEL);
	pairs = kvcalloc(SFBS_PAIR_TABLE_SIZE, sizeof(*pairs), GFP_KERNEL);
	if (scan_order == 3U)
		triples = kvcalloc(SFBS_MAX_TRIPLE_TARGETS,
				   sizeof(*triples), GFP_KERNEL);
	reduction_steps = kvcalloc(SFBS_STIMULUS_SLOTS,
				   sizeof(*reduction_steps), GFP_KERNEL);
	max_repetitions = max(baseline_repetitions,
			      max(bit_repetitions,
				  max(pair_screen_repetitions,
				      max(triple_screen_repetitions,
					  reduction_validation_repetitions))));
	original_ticks = kvmalloc_array(max_repetitions,
					 sizeof(*original_ticks), GFP_KERNEL);
	changed_ticks = kvmalloc_array(max_repetitions,
					sizeof(*changed_ticks), GFP_KERNEL);
	if (!pool || !original_group || !changed_group || !reduction_scratch ||
	    !original_control ||
	    !changed_control || !single_lines || !single_available ||
	    !single_alias || !single_metrics || !pairs || !reduction_steps ||
	    (scan_order == 3U && !triples) || !original_ticks ||
	    !changed_ticks) {
		ret = -ENOMEM;
		goto out;
	}

	sort(sfcp_pages, sfcp_allocated_pages, sizeof(*sfcp_pages),
	     sfbs_page_phys_compare, NULL);
	ret = sfbs_collect_pool(pool, &pool_count);
	if (ret)
		goto out;
	required_pool = auto_find_stable ? candidate_items + 1U :
		sfbs_rank[SFBS_STIMULUS_SLOTS] + 1U;
	if (!auto_find_stable) {
		for (i = 0; i < SFBS_STIMULUS_SLOTS; ++i) {
			if (stimulus_mask & BIT_ULL(i))
				required_pool = max(required_pool,
					(unsigned int)sfbs_rank[i] + 1U);
		}
	}
	if (pool_count < required_pool) {
		pr_err(SFCP_NAME ": selected stimulus/probe ranks need %u owned candidates, got %u; increase pool_pages\n",
		       required_pool, pool_count);
		ret = -ENOSPC;
		goto out;
	}
	sfcp_enable_counter();
	if (!sfcp_counter_calibration_delta) {
		ret = -EOPNOTSUPP;
		goto out_counter;
	}
	memset(&baseline_metric, 0, sizeof(baseline_metric));
	if (auto_find_stable) {
		ret = sfbs_find_stable_group(pool, pool_count, original_group,
					     original_ticks, &baseline_metric,
					     &selected_attempt);
		active_items = candidate_items;
	} else {
		active_items = sfbs_build_group(pool, original_group);
		ret = sfbs_measure_single(original_group, active_items,
					  baseline_repetitions,
					  &baseline_metric, original_ticks);
		if (!ret && !sfbs_metric_is_stable(&baseline_metric))
			ret = -EAGAIN;
	}
	if (ret) {
		pr_err(SFCP_NAME ": candidate-set selection validation failed (%u/%u)\n",
		       baseline_metric.successes, baseline_metric.repetitions);
		goto out_counter;
	}
	pr_info(SFCP_NAME ": locked pair-scan candidate set with %u candidates: matching=%u/%u\n",
		active_items, baseline_metric.successes,
		baseline_metric.repetitions);
	memset(&reduced_validation, 0, sizeof(reduced_validation));
	ret = sfbs_reduce_candidate_group(original_group, reduction_scratch,
		&active_items, original_ticks, reduction_steps,
		&reduction_step_count, &reduced_validation);
	if (ret) {
		pr_err(SFCP_NAME ": could not reduce candidate set from %u to %u while preserving %u%% stability\n",
		       candidate_items, target_candidate_items, stable_percent);
		goto out_counter;
	}
	pr_info(SFCP_NAME ": fixed reduced candidate set with %u candidates: matching=%u/%u\n",
		active_items, reduced_validation.successes,
		reduced_validation.repetitions);
	memcpy(changed_group, original_group,
	       SFBS_GROUP_LINES * sizeof(*changed_group));
	base_probe_pa = sfcp_physical(
		&original_group[SFBS_STIMULUS_SLOTS]);

	for (i = bit_first; i <= bit_last; ++i) {
		phys_addr_t single_pa = base_probe_pa ^ BIT_ULL(i);

		single_available[i] = sfbs_find_owned_line(single_pa,
							   &single_lines[i]);
		if (single_available[i] && sfbs_probe_aliases_stimulus(
				&single_lines[i], original_group, active_items)) {
			single_alias[i] = true;
			single_available[i] = false;
		}
		for (j = i + 1U; j <= bit_last; ++j) {
			struct sfbs_pair_target *pair = &pairs[i * 64U + j];
			phys_addr_t pair_pa = base_probe_pa ^ BIT_ULL(i) ^
				BIT_ULL(j);

			pair->available = sfbs_find_owned_line(pair_pa,
							       &pair->line);
			if (pair->available && sfbs_probe_aliases_stimulus(
					&pair->line, original_group, active_items)) {
				pair->candidate_alias = true;
				pair->available = false;
			}
		}
	}
	if (scan_order == 3U) {
		for (i = bit_first; i <= bit_last; ++i) {
			for (j = i + 1U; j <= bit_last; ++j) {
				for (k = j + 1U; k <= bit_last; ++k) {
					struct sfbs_triple_target *triple;
					phys_addr_t triple_pa;

					if (triple_count == SFBS_MAX_TRIPLE_TARGETS) {
						ret = -E2BIG;
						goto out_counter;
					}
					triple = &triples[triple_count++];
					triple->bit_i = i;
					triple->bit_j = j;
					triple->bit_k = k;
					triple_pa = base_probe_pa ^ BIT_ULL(i) ^
						BIT_ULL(j) ^ BIT_ULL(k);
					triple->available = sfbs_find_owned_line(
						triple_pa, &triple->line);
					if (triple->available &&
					    sfbs_probe_aliases_stimulus(&triple->line,
						original_group, active_items)) {
						triple->candidate_alias = true;
						triple->available = false;
					}
				}
			}
		}
	}
	ret = sfbs_release_unused_combo_pages(original_group, active_items,
					      single_lines, single_available,
					      pairs, triples, triple_count);
	if (ret)
		goto out_counter;

	file = filp_open(result_path, O_CREAT | O_TRUNC | O_WRONLY, 0644);
	if (IS_ERR(file)) {
		ret = PTR_ERR(file);
		file = NULL;
		goto out_counter;
	}
	ret = sfbs_write_header(file, &position, pool_count, active_items,
				base_probe_pa, selected_attempt);
	if (!ret)
		ret = sfbs_write_candidates(file, &position, original_group,
					    active_items, base_probe_pa);
	if (!ret)
		ret = sfbs_write_metric(file, &position,
			"selection_validation", 0U, SFBS_NO_SECOND_BIT,
			SFBS_NO_SECOND_BIT,
			"initial_matching", true, "owned_exact", candidate_items,
			&baseline_metric, base_probe_pa, base_probe_pa, 0ULL);
	if (!ret)
		ret = sfbs_write_reduction_trace(file, &position,
			reduction_steps, reduction_step_count, base_probe_pa);
	if (!ret)
		ret = sfbs_write_metric(file, &position,
			"selection_validation", 0U, SFBS_NO_SECOND_BIT,
			SFBS_NO_SECOND_BIT, "reduced_matching", true,
			"owned_exact", active_items, &reduced_validation,
			base_probe_pa, base_probe_pa, 0ULL);

	for (pass = 1U; !ret && pass <= scan_passes; ++pass) {
		struct sfbs_metric pass_validation;

		memset(&pass_validation, 0, sizeof(pass_validation));
		sfbs_current_pass = pass;
		selected_pairs = 0U;
		selected_triples = 0U;
		memset(single_metrics, 0, 64U * sizeof(*single_metrics));
		for (i = bit_first; i <= bit_last; ++i) {
			for (j = i + 1U; j <= bit_last; ++j) {
				struct sfbs_pair_target *pair = &pairs[i * 64U + j];

				pair->selected = false;
				memset(&pair->screen_original, 0,
				       sizeof(pair->screen_original));
				memset(&pair->screen_double, 0,
				       sizeof(pair->screen_double));
			}
		}
		if (scan_order == 3U) {
			unsigned int triple_index;

			for (triple_index = 0; triple_index < triple_count;
			     ++triple_index) {
				struct sfbs_triple_target *triple = &triples[triple_index];
				triple->selected = false;
				memset(&triple->screen_original, 0,
				       sizeof(triple->screen_original));
				memset(&triple->screen_triple, 0,
				       sizeof(triple->screen_triple));
			}
		}
		pr_info(SFCP_NAME ": starting scan pass %u/%u on fixed %u-candidate set\n",
			pass, scan_passes, active_items);
		ret = sfbs_measure_single(original_group, active_items,
			baseline_repetitions, &pass_validation, original_ticks);
		if (!ret)
			ret = sfbs_write_metric(file, &position,
				"pass_validation", 0U, SFBS_NO_SECOND_BIT,
				SFBS_NO_SECOND_BIT, "fixed_set_matching", true,
				"owned_exact", active_items, &pass_validation,
				base_probe_pa, base_probe_pa, 0ULL);
		pr_info(SFCP_NAME ": pass %u baseline=%u/%u stable=%u\n",
			pass, pass_validation.successes,
			pass_validation.repetitions,
			sfbs_metric_is_stable(&pass_validation));

	/* Validate each constituent single flip once on the locked set. */
	for (i = bit_first; !ret && i <= bit_last; ++i) {
		struct sfbs_metric original_metric;
		phys_addr_t single_pa = base_probe_pa ^ BIT_ULL(i);

		if (!single_available[i]) {
			ret = sfbs_write_metric(file, &position,
				"single_validation", i, SFBS_NO_SECOND_BIT,
				SFBS_NO_SECOND_BIT,
				"single_matching", false,
				single_alias[i] ? "candidate_alias" : "not_owned",
				active_items, NULL, single_pa, base_probe_pa,
				BIT_ULL(i));
			continue;
		}
		changed_group[SFBS_STIMULUS_SLOTS] = single_lines[i];
		ret = sfbs_measure_write_pair_n(file, &position,
			"single_validation", i, SFBS_NO_SECOND_BIT,
			SFBS_NO_SECOND_BIT,
			"original_matching", "single_matching",
			original_group, changed_group, active_items,
			bit_repetitions, base_probe_pa, single_pa,
			BIT_ULL(i), original_ticks, changed_ticks,
			&original_metric, &single_metrics[i]);
		pr_info(SFCP_NAME ": single bit=%u original=%u/%u changed=%u/%u status=%s\n",
			i, original_metric.successes, bit_repetitions,
			single_metrics[i].successes, bit_repetitions,
			ret ? "failed" : "written");
	}

	/* Screen every available pair; retain all screen rows in the CSV. */
	for (i = bit_first; !ret && i <= bit_last; ++i) {
		for (j = i + 1U; !ret && j <= bit_last; ++j) {
			struct sfbs_pair_target *pair = &pairs[i * 64U + j];
			phys_addr_t pair_pa = base_probe_pa ^ BIT_ULL(i) ^
				BIT_ULL(j);
			u64 pair_mask = BIT_ULL(i) ^ BIT_ULL(j);
			bool singles_low;
			bool original_high;
			bool double_high;

			if (!pair->available || !single_available[i] ||
			    !single_available[j]) {
				const char *status = pair->candidate_alias ?
					"candidate_alias" :
					(!pair->available ? "not_owned" :
					 "single_unavailable");

				ret = sfbs_write_metric(file, &position,
					"pair_screen", i, j, SFBS_NO_SECOND_BIT,
					"double_screen",
					false, status, active_items, NULL,
					pair_pa, base_probe_pa, pair_mask);
				continue;
			}
			changed_group[SFBS_STIMULUS_SLOTS] = pair->line;
			ret = sfbs_measure_write_pair_n(file, &position,
				"pair_screen", i, j, SFBS_NO_SECOND_BIT,
				"original_screen", "double_screen",
				original_group, changed_group, active_items,
				pair_screen_repetitions, base_probe_pa, pair_pa,
				pair_mask, original_ticks, changed_ticks,
				&pair->screen_original, &pair->screen_double);
			if (ret)
				break;
			singles_low =
				(u64)single_metrics[i].successes * 100ULL <=
				(u64)single_metrics[i].repetitions * single_max_percent &&
				(u64)single_metrics[j].successes * 100ULL <=
				(u64)single_metrics[j].repetitions * single_max_percent;
			original_high =
				(u64)pair->screen_original.successes * 100ULL >=
				(u64)pair->screen_original.repetitions * stable_percent;
			double_high =
				(u64)pair->screen_double.successes * 100ULL >=
				(u64)pair->screen_double.repetitions *
				pair_screen_percent;
			pair->selected = singles_low && original_high && double_high;
			if (pair->selected)
				++selected_pairs;
			pr_info(SFCP_NAME ": pair=%u,%u screen original=%u/%u double=%u/%u singles=%u,%u selected=%u\n",
				i, j, pair->screen_original.successes,
				pair_screen_repetitions,
				pair->screen_double.successes,
				pair_screen_repetitions,
				single_metrics[i].successes,
				single_metrics[j].successes, pair->selected);
		}
	}
	pr_info(SFCP_NAME ": pair screen selected %u combinations for long validation\n",
		selected_pairs);

	if (scan_order == 3U) {
		unsigned int triple_index;

		for (triple_index = 0; !ret && triple_index < triple_count;
		     ++triple_index) {
			struct sfbs_triple_target *triple = &triples[triple_index];
			struct sfbs_pair_target *pair_ij =
				&pairs[triple->bit_i * 64U + triple->bit_j];
			struct sfbs_pair_target *pair_ik =
				&pairs[triple->bit_i * 64U + triple->bit_k];
			struct sfbs_pair_target *pair_jk =
				&pairs[triple->bit_j * 64U + triple->bit_k];
			phys_addr_t triple_pa = base_probe_pa ^
				BIT_ULL(triple->bit_i) ^ BIT_ULL(triple->bit_j) ^
				BIT_ULL(triple->bit_k);
			u64 triple_mask = BIT_ULL(triple->bit_i) ^
				BIT_ULL(triple->bit_j) ^ BIT_ULL(triple->bit_k);
			bool inputs_available;
			bool singles_low;
			bool pairs_low;
			bool original_high;
			bool triple_high;

			inputs_available = triple->available &&
				single_available[triple->bit_i] &&
				single_available[triple->bit_j] &&
				single_available[triple->bit_k] &&
				pair_ij->available && pair_ik->available &&
				pair_jk->available;
			if (!inputs_available) {
				const char *status = triple->candidate_alias ?
					"candidate_alias" :
					(!triple->available ? "not_owned" :
					 "component_unavailable");

				ret = sfbs_write_metric(file, &position,
					"triple_screen", triple->bit_i,
					triple->bit_j, triple->bit_k,
					"triple_screen", false, status,
					active_items, NULL, triple_pa,
					base_probe_pa, triple_mask);
				continue;
			}
			changed_group[SFBS_STIMULUS_SLOTS] = triple->line;
			ret = sfbs_measure_write_pair_n(file, &position,
				"triple_screen", triple->bit_i, triple->bit_j,
				triple->bit_k, "original_screen", "triple_screen",
				original_group, changed_group, active_items,
				triple_screen_repetitions, base_probe_pa, triple_pa,
				triple_mask, original_ticks, changed_ticks,
				&triple->screen_original,
				&triple->screen_triple);
			if (ret)
				break;
			singles_low =
				(u64)single_metrics[triple->bit_i].successes * 100ULL <=
				(u64)single_metrics[triple->bit_i].repetitions *
					single_max_percent &&
				(u64)single_metrics[triple->bit_j].successes * 100ULL <=
				(u64)single_metrics[triple->bit_j].repetitions *
					single_max_percent &&
				(u64)single_metrics[triple->bit_k].successes * 100ULL <=
				(u64)single_metrics[triple->bit_k].repetitions *
					single_max_percent;
			pairs_low =
				(u64)pair_ij->screen_double.successes * 100ULL <=
				(u64)pair_ij->screen_double.repetitions *
					pair_max_percent &&
				(u64)pair_ik->screen_double.successes * 100ULL <=
				(u64)pair_ik->screen_double.repetitions *
					pair_max_percent &&
				(u64)pair_jk->screen_double.successes * 100ULL <=
				(u64)pair_jk->screen_double.repetitions *
					pair_max_percent;
			original_high =
				(u64)triple->screen_original.successes * 100ULL >=
				(u64)triple->screen_original.repetitions *
					stable_percent;
			triple_high =
				(u64)triple->screen_triple.successes * 100ULL >=
				(u64)triple->screen_triple.repetitions *
					triple_screen_percent;
			triple->selected = singles_low && pairs_low &&
				original_high && triple_high;
			if (triple->selected)
				++selected_triples;
			if (triple->selected || !(triple_index % 100U))
				pr_info(SFCP_NAME ": triple=%u,%u,%u screen original=%u/%u changed=%u/%u singles_low=%u pairs_low=%u selected=%u progress=%u/%u\n",
					triple->bit_i, triple->bit_j,
					triple->bit_k,
					triple->screen_original.successes,
					triple_screen_repetitions,
					triple->screen_triple.successes,
					triple_screen_repetitions, singles_low,
					pairs_low, triple->selected,
					triple_index + 1U, triple_count);
		}
		pr_info(SFCP_NAME ": triple screen selected %u combinations for long validation\n",
			selected_triples);
	}

	/* Long paired validation with address-matched and negative controls. */
	for (i = bit_first; !ret && i <= bit_last; ++i) {
		for (j = i + 1U; !ret && j <= bit_last; ++j) {
			struct sfbs_pair_target *pair = &pairs[i * 64U + j];
			struct sfbs_metric matching_original;
			struct sfbs_metric matching_double;
			struct sfbs_metric unrelated_original;
			struct sfbs_metric unrelated_double;
			struct sfbs_metric idle_original;
			struct sfbs_metric idle_double;
			phys_addr_t pair_pa;
			u64 pair_mask;
			unsigned int control_bit;

			if (!pair->selected)
				continue;
			memset(&matching_original, 0, sizeof(matching_original));
			memset(&matching_double, 0, sizeof(matching_double));
			memset(&unrelated_original, 0, sizeof(unrelated_original));
			memset(&unrelated_double, 0, sizeof(unrelated_double));
			memset(&idle_original, 0, sizeof(idle_original));
			memset(&idle_double, 0, sizeof(idle_double));
			pair_pa = base_probe_pa ^ BIT_ULL(i) ^ BIT_ULL(j);
			pair_mask = BIT_ULL(i) ^ BIT_ULL(j);
			changed_group[SFBS_STIMULUS_SLOTS] = pair->line;
			ret = sfbs_measure_write_pair_n(file, &position,
				"pair_validation", i, j, SFBS_NO_SECOND_BIT,
				"original_matching", "double_matching",
				original_group, changed_group, active_items,
				bit_repetitions, base_probe_pa, pair_pa,
				pair_mask, original_ticks, changed_ticks,
				&matching_original, &matching_double);
			for (control_bit = 6U; control_bit <= 11U;
			     ++control_bit) {
				if (control_bit != i && control_bit != j)
					break;
			}
			if (!ret) {
				sfbs_build_unrelated_control(original_group,
					original_control, active_items, control_bit);
				memcpy(changed_control, original_control,
				       SFBS_GROUP_LINES * sizeof(*changed_control));
				changed_control[SFBS_STIMULUS_SLOTS] = pair->line;
				ret = sfbs_measure_write_pair_n(file, &position,
					"pair_validation", i, j,
					SFBS_NO_SECOND_BIT,
					"original_unrelated", "double_unrelated",
					original_control, changed_control, active_items,
					bit_repetitions, base_probe_pa, pair_pa,
					pair_mask, original_ticks, changed_ticks,
					&unrelated_original, &unrelated_double);
			}
			if (!ret)
				ret = sfbs_measure_write_pair_n(file, &position,
					"pair_validation", i, j,
					SFBS_NO_SECOND_BIT,
					"original_idle", "double_idle",
					original_group, changed_group, 0U,
					bit_repetitions, base_probe_pa, pair_pa,
					pair_mask, original_ticks, changed_ticks,
					&idle_original, &idle_double);
			pr_info(SFCP_NAME ": pair=%u,%u validation matching(o=%u d=%u) unrelated(o=%u d=%u) idle(o=%u d=%u) status=%s\n",
				i, j, matching_original.successes,
				matching_double.successes,
				unrelated_original.successes,
				unrelated_double.successes,
				idle_original.successes, idle_double.successes,
				ret ? "failed" : "written");
		}
	}

	if (scan_order == 3U) {
		unsigned int triple_index;

		for (triple_index = 0; !ret && triple_index < triple_count;
		     ++triple_index) {
			struct sfbs_triple_target *triple = &triples[triple_index];
			struct sfbs_metric original_metric;
			struct sfbs_metric changed_metric;
			struct sfbs_metric unrelated_original;
			struct sfbs_metric unrelated_triple;
			struct sfbs_metric idle_original;
			struct sfbs_metric idle_triple;
			unsigned int pair_bits[3][2];
			unsigned int pair_index;
			unsigned int control_bit;
			phys_addr_t triple_pa;
			u64 triple_mask;

			if (!triple->selected)
				continue;
			memset(&original_metric, 0, sizeof(original_metric));
			memset(&changed_metric, 0, sizeof(changed_metric));
			memset(&unrelated_original, 0, sizeof(unrelated_original));
			memset(&unrelated_triple, 0, sizeof(unrelated_triple));
			memset(&idle_original, 0, sizeof(idle_original));
			memset(&idle_triple, 0, sizeof(idle_triple));
			pair_bits[0][0] = triple->bit_i;
			pair_bits[0][1] = triple->bit_j;
			pair_bits[1][0] = triple->bit_i;
			pair_bits[1][1] = triple->bit_k;
			pair_bits[2][0] = triple->bit_j;
			pair_bits[2][1] = triple->bit_k;
			for (pair_index = 0; !ret && pair_index < 3U;
			     ++pair_index) {
				unsigned int a = pair_bits[pair_index][0];
				unsigned int b = pair_bits[pair_index][1];
				struct sfbs_pair_target *component =
					&pairs[a * 64U + b];
				phys_addr_t component_pa = base_probe_pa ^
					BIT_ULL(a) ^ BIT_ULL(b);
				u64 component_mask = BIT_ULL(a) ^ BIT_ULL(b);
				char original_variant[40];
				char changed_variant[40];

				scnprintf(original_variant, sizeof(original_variant),
					  "original_for_pair_%u_%u", a, b);
				scnprintf(changed_variant, sizeof(changed_variant),
					  "pair_%u_%u_matching", a, b);
				changed_group[SFBS_STIMULUS_SLOTS] = component->line;
				ret = sfbs_measure_write_pair_n(file, &position,
					"triple_component_pair", triple->bit_i,
					triple->bit_j, triple->bit_k,
					original_variant, changed_variant,
					original_group, changed_group, active_items,
					bit_repetitions, base_probe_pa, component_pa,
					component_mask, original_ticks, changed_ticks,
					&original_metric, &changed_metric);
			}
			triple_pa = base_probe_pa ^ BIT_ULL(triple->bit_i) ^
				BIT_ULL(triple->bit_j) ^ BIT_ULL(triple->bit_k);
			triple_mask = BIT_ULL(triple->bit_i) ^
				BIT_ULL(triple->bit_j) ^ BIT_ULL(triple->bit_k);
			changed_group[SFBS_STIMULUS_SLOTS] = triple->line;
			if (!ret)
				ret = sfbs_measure_write_pair_n(file, &position,
					"triple_validation", triple->bit_i,
					triple->bit_j, triple->bit_k,
					"original_matching", "triple_matching",
					original_group, changed_group, active_items,
					bit_repetitions, base_probe_pa, triple_pa,
					triple_mask, original_ticks, changed_ticks,
					&original_metric, &changed_metric);
			for (control_bit = 6U; control_bit <= 11U;
			     ++control_bit) {
				if (control_bit != triple->bit_i &&
				    control_bit != triple->bit_j &&
				    control_bit != triple->bit_k)
					break;
			}
			if (!ret) {
				sfbs_build_unrelated_control(original_group,
					original_control, active_items, control_bit);
				memcpy(changed_control, original_control,
				       SFBS_GROUP_LINES * sizeof(*changed_control));
				changed_control[SFBS_STIMULUS_SLOTS] = triple->line;
				ret = sfbs_measure_write_pair_n(file, &position,
					"triple_validation", triple->bit_i,
					triple->bit_j, triple->bit_k,
					"original_unrelated", "triple_unrelated",
					original_control, changed_control, active_items,
					bit_repetitions, base_probe_pa, triple_pa,
					triple_mask, original_ticks, changed_ticks,
					&unrelated_original, &unrelated_triple);
			}
			if (!ret)
				ret = sfbs_measure_write_pair_n(file, &position,
					"triple_validation", triple->bit_i,
					triple->bit_j, triple->bit_k,
					"original_idle", "triple_idle",
					original_group, changed_group, 0U,
					bit_repetitions, base_probe_pa, triple_pa,
					triple_mask, original_ticks, changed_ticks,
					&idle_original, &idle_triple);
			pr_info(SFCP_NAME ": triple=%u,%u,%u validation matching(o=%u t=%u) unrelated(o=%u t=%u) idle(o=%u t=%u) status=%s\n",
				triple->bit_i, triple->bit_j, triple->bit_k,
				original_metric.successes, changed_metric.successes,
				unrelated_original.successes,
				unrelated_triple.successes,
				idle_original.successes, idle_triple.successes,
				ret ? "failed" : "written");
		}
	}
		pr_info(SFCP_NAME ": completed scan pass %u/%u (pairs=%u triples=%u selected)\n",
			pass, scan_passes, selected_pairs, selected_triples);
	}
	sfbs_current_pass = 0U;
	filp_close(file, NULL);
	file = NULL;
	if (!ret)
		pr_info(SFCP_NAME ": wrote owned-page order-%u XOR results (pairs=%u triples=%u selected) to %s\n",
			scan_order, selected_pairs, selected_triples, result_path);

out_counter:
	sfcp_restore_counter();
out:
	if (file)
		filp_close(file, NULL);
	kvfree(changed_ticks);
	kvfree(original_ticks);
	kvfree(triples);
	kvfree(pairs);
	kvfree(reduction_steps);
	kvfree(single_metrics);
	kvfree(single_alias);
	kvfree(single_available);
	kvfree(single_lines);
	kvfree(changed_control);
	kvfree(original_control);
	kvfree(changed_group);
	kvfree(reduction_scratch);
	kvfree(original_group);
	kvfree(pool);
	return ret;
}

static int sfbs_run_color_matrix_experiment(void)
{
	struct sfcp_line *pool = NULL;
	struct sfcp_line *base_group = NULL;
	struct sfcp_line *reduction_scratch = NULL;
	struct sfcp_line *color_groups = NULL;
	struct sfcp_line *changed_group = NULL;
	struct sfbs_reduction_step *reduction_steps = NULL;
	u64 *reference_ticks = NULL;
	u64 *changed_ticks = NULL;
	struct file *file = NULL;
	loff_t position = 0;
	struct sfbs_metric initial_validation;
	struct sfbs_metric reduced_validation;
	struct sfbs_metric diagonal_validation[SFBS_COLOR_COUNT];
	unsigned int pool_count = 0;
	unsigned int active_items = 0;
	unsigned int selected_attempt = 0;
	unsigned int reduction_step_count = 0;
	unsigned int max_repetitions;
	unsigned int base_color;
	unsigned int color;
	unsigned int pass;
	unsigned int i;
	phys_addr_t base_probe_pa;
	int ret;

	if (access_items != SFBS_STIMULUS_SLOTS ||
	    (sfcp_active_addrmask != 0x1ffffffULL &&
	     sfcp_active_addrmask != 0x7fffffULL) || !auto_find_stable ||
	    color_bit0 < SFBS_MIN_TEST_BIT || color_bit0 > SFBS_MAX_TEST_BIT ||
	    color_bit1 < SFBS_MIN_TEST_BIT || color_bit1 > SFBS_MAX_TEST_BIT ||
	    color_bit0 == color_bit1 ||
	    !color_repetitions || color_repetitions > SFBS_MAX_REPETITIONS ||
	    !candidate_items || candidate_items > SFBS_STIMULUS_SLOTS ||
	    !target_candidate_items || target_candidate_items > candidate_items ||
	    !search_groups || !search_repetitions ||
	    search_repetitions > SFBS_MAX_REPETITIONS ||
	    !baseline_repetitions ||
	    baseline_repetitions > SFBS_MAX_REPETITIONS ||
	    !reduction_repetitions ||
	    reduction_repetitions > SFBS_MAX_REPETITIONS ||
	    !reduction_validation_repetitions ||
	    reduction_validation_repetitions > SFBS_MAX_REPETITIONS ||
	    !scan_passes || scan_passes > 10U ||
	    !trigger_threshold || !stable_percent || stable_percent > 100U) {
		pr_err(SFCP_NAME ": invalid color-matrix parameters\n");
		return -EINVAL;
	}

	pool = kvmalloc_array(SFBS_POOL_CAPACITY, sizeof(*pool), GFP_KERNEL);
	base_group = kvmalloc_array(SFBS_GROUP_LINES,
				    sizeof(*base_group), GFP_KERNEL);
	reduction_scratch = kvmalloc_array(SFBS_GROUP_LINES,
					   sizeof(*reduction_scratch), GFP_KERNEL);
	color_groups = kvmalloc_array(SFBS_COLOR_COUNT * SFBS_GROUP_LINES,
				      sizeof(*color_groups), GFP_KERNEL);
	changed_group = kvmalloc_array(SFBS_GROUP_LINES,
				       sizeof(*changed_group), GFP_KERNEL);
	reduction_steps = kvcalloc(SFBS_STIMULUS_SLOTS,
				   sizeof(*reduction_steps), GFP_KERNEL);
	max_repetitions = max(color_repetitions,
			      max(baseline_repetitions,
				  reduction_validation_repetitions));
	reference_ticks = kvmalloc_array(max_repetitions,
					 sizeof(*reference_ticks), GFP_KERNEL);
	changed_ticks = kvmalloc_array(max_repetitions,
				       sizeof(*changed_ticks), GFP_KERNEL);
	if (!pool || !base_group || !reduction_scratch || !color_groups ||
	    !changed_group || !reduction_steps || !reference_ticks ||
	    !changed_ticks) {
		ret = -ENOMEM;
		goto out;
	}

	sort(sfcp_pages, sfcp_allocated_pages, sizeof(*sfcp_pages),
	     sfbs_page_phys_compare, NULL);
	ret = sfbs_collect_color_pool(pool, &pool_count);
	if (ret)
		goto out;
	if (pool_count < candidate_items + 1U) {
		pr_err(SFCP_NAME ": color matrix needs at least %u complete address quartets, got %u; increase pool_pages or retry after reboot\n",
		       candidate_items + 1U, pool_count);
		ret = -ENOSPC;
		goto out;
	}

	sfcp_enable_counter();
	if (!sfcp_counter_calibration_delta) {
		ret = -EOPNOTSUPP;
		goto out_counter;
	}
	memset(&initial_validation, 0, sizeof(initial_validation));
	ret = sfbs_find_stable_group(pool, pool_count, base_group,
				     reference_ticks, &initial_validation,
				     &selected_attempt);
	active_items = candidate_items;
	if (ret)
		goto out_counter;
	base_probe_pa = sfcp_physical(&base_group[SFBS_STIMULUS_SLOTS]);
	base_color = ((base_probe_pa >> color_bit0) & 1ULL) |
		(((base_probe_pa >> color_bit1) & 1ULL) << 1U);
	memset(&reduced_validation, 0, sizeof(reduced_validation));
	memset(diagonal_validation, 0, sizeof(diagonal_validation));
	ret = sfbs_reduce_color_group(base_group, reduction_scratch,
		color_groups, &active_items, reference_ticks, reduction_steps,
		&reduction_step_count, diagonal_validation);
	if (ret)
		goto out_counter;
	base_probe_pa = sfcp_physical(&base_group[SFBS_STIMULUS_SLOTS]);
	reduced_validation = diagonal_validation[base_color];
	for (color = 0; color < SFBS_COLOR_COUNT; ++color) {
		pr_info(SFCP_NAME ": color diagonal=%u validation=%u/%u\n",
			color, diagonal_validation[color].successes,
			diagonal_validation[color].repetitions);
	}
	ret = sfbs_release_unused_color_pages(color_groups, active_items);
	if (ret)
		goto out_counter;

	file = filp_open(result_path, O_CREAT | O_TRUNC | O_WRONLY, 0644);
	if (IS_ERR(file)) {
		ret = PTR_ERR(file);
		file = NULL;
		goto out_counter;
	}
	ret = sfbs_write_header(file, &position, pool_count, active_items,
				base_probe_pa, selected_attempt);
	if (!ret)
		ret = sfbs_write_candidates(file, &position, base_group,
					    active_items, base_probe_pa);
	if (!ret)
		ret = sfbs_write_metric(file, &position, "selection_validation",
			0U, SFBS_NO_SECOND_BIT, SFBS_NO_SECOND_BIT,
			"initial_matching", true, "owned_exact", candidate_items,
			&initial_validation, base_probe_pa, base_probe_pa, 0ULL);
	if (!ret)
		ret = sfbs_write_reduction_trace(file, &position, reduction_steps,
			reduction_step_count, base_probe_pa);
	if (!ret)
		ret = sfbs_write_metric(file, &position, "selection_validation",
			0U, SFBS_NO_SECOND_BIT, SFBS_NO_SECOND_BIT,
			"reduced_matching", true, "owned_exact", active_items,
			&reduced_validation, base_probe_pa, base_probe_pa, 0ULL);
	for (color = 0; !ret && color < SFBS_COLOR_COUNT; ++color) {
		struct sfcp_line *group =
			&color_groups[color * SFBS_GROUP_LINES];
		phys_addr_t probe_pa =
			sfcp_physical(&group[SFBS_STIMULUS_SLOTS]);

		ret = sfbs_write_metric(file, &position,
			"color_diagonal_validation", color, color, 0U,
			"joint_color_matching", true, "owned_exact",
			active_items, &diagonal_validation[color], probe_pa,
			probe_pa, 0ULL);
		for (i = 0; !ret && i < active_items; ++i)
			ret = sfbs_write_metric(file, &position,
				"color_candidate_member", color, i, 0U,
				"stimulus", true, "owned_exact", active_items,
				NULL, sfcp_physical(&group[i]), probe_pa, 0ULL);
	}

	for (pass = 1U; !ret && pass <= scan_passes; ++pass) {
		unsigned int source;

		sfbs_current_pass = pass;
		for (source = 0; !ret && source < SFBS_COLOR_COUNT; ++source) {
			struct sfcp_line *reference_group =
				&color_groups[source * SFBS_GROUP_LINES];
			phys_addr_t reference_pa = sfcp_physical(
				&reference_group[SFBS_STIMULUS_SLOTS]);
			struct sfbs_metric pass_diagonal;
			unsigned int destination;

			ret = sfbs_measure_single(reference_group, active_items,
				color_repetitions, &pass_diagonal, reference_ticks);
			if (!ret)
				ret = sfbs_write_metric(file, &position,
					"color_matrix", source, source, 0U,
					"diagonal", true, "owned_exact", active_items,
					&pass_diagonal, reference_pa, reference_pa, 0ULL);
			for (destination = 0; !ret && destination < SFBS_COLOR_COUNT;
			     ++destination) {
				struct sfcp_line *destination_group;
				struct sfbs_metric reference_metric;
				struct sfbs_metric cell_metric;
				struct sfbs_metric idle_reference;
				struct sfbs_metric idle_cell;
				phys_addr_t destination_pa;
				u64 changed_mask = 0ULL;
				unsigned int distance;

				if (destination == source)
					continue;
				memset(&reference_metric, 0, sizeof(reference_metric));
				memset(&cell_metric, 0, sizeof(cell_metric));
				memset(&idle_reference, 0, sizeof(idle_reference));
				memset(&idle_cell, 0, sizeof(idle_cell));
				destination_group = &color_groups[
					destination * SFBS_GROUP_LINES];
				destination_pa = sfcp_physical(
					&destination_group[SFBS_STIMULUS_SLOTS]);
				memcpy(changed_group, reference_group,
				       SFBS_GROUP_LINES * sizeof(*changed_group));
				changed_group[SFBS_STIMULUS_SLOTS] =
					destination_group[SFBS_STIMULUS_SLOTS];
				if ((source ^ destination) & 1U)
					changed_mask |= BIT_ULL(color_bit0);
				if ((source ^ destination) & 2U)
					changed_mask |= BIT_ULL(color_bit1);
				distance = hweight32(source ^ destination);
				ret = sfbs_measure_pair(reference_group, changed_group,
					active_items, color_repetitions,
					&reference_metric, &cell_metric,
					reference_ticks, changed_ticks);
				if (!ret)
					ret = sfbs_write_metric(file, &position,
						"color_reference", source, destination,
						distance, "diagonal_paired", true,
						"owned_exact", active_items,
						&reference_metric, reference_pa,
						destination_pa, changed_mask);
				if (!ret)
					ret = sfbs_write_metric(file, &position,
						"color_matrix", source, destination,
						distance, "off_diagonal", true,
						"owned_exact", active_items,
						&cell_metric, destination_pa,
						reference_pa, changed_mask);
				if (!ret)
					ret = sfbs_measure_pair(reference_group,
						changed_group, 0U, color_repetitions,
						&idle_reference, &idle_cell,
						reference_ticks, changed_ticks);
				if (!ret)
					ret = sfbs_write_metric(file, &position,
						"color_idle", source, destination,
						distance, "idle_reference", true,
						"owned_exact", 0U, &idle_reference,
						reference_pa, destination_pa, changed_mask);
				if (!ret)
					ret = sfbs_write_metric(file, &position,
						"color_idle", source, destination,
						distance, "idle_cell", true,
						"owned_exact", 0U, &idle_cell,
						destination_pa, reference_pa, changed_mask);
				pr_info(SFCP_NAME ": color pass=%u source=%u destination=%u distance=%u reference=%u/%u cell=%u/%u idle=%u/%u\n",
					pass, source, destination, distance,
					reference_metric.successes,
					reference_metric.repetitions,
					cell_metric.successes, cell_metric.repetitions,
					idle_cell.successes, idle_cell.repetitions);
			}
		}
	}
	sfbs_current_pass = 0U;
	filp_close(file, NULL);
	file = NULL;
	if (!ret)
		pr_info(SFCP_NAME ": wrote 4x4 owned-page color matrix for bits %u,%u (base_color=%u) to %s\n",
			color_bit0, color_bit1, base_color, result_path);

out_counter:
	sfcp_restore_counter();
out:
	if (file)
		filp_close(file, NULL);
	kvfree(changed_ticks);
	kvfree(reference_ticks);
	kvfree(reduction_steps);
	kvfree(changed_group);
	kvfree(color_groups);
	kvfree(reduction_scratch);
	kvfree(base_group);
	kvfree(pool);
	return ret;
}

static int sfbs_run_joint_bit_experiment(void)
{
	struct sfcp_line *pool = NULL;
	struct sfcp_line *original_group = NULL;
	struct sfcp_line *probe_only_group = NULL;
	struct sfcp_line *reduction_scratch = NULL;
	struct sfcp_line *sweep_group = NULL;
	struct sfcp_line *sweep_baseline = NULL;
	struct sfcp_line *probe_lines = NULL;
	struct sfcp_line *joint_groups = NULL;
	struct sfbs_reduction_step *reduction_steps = NULL;
	bool *probe_available = NULL;
	bool *joint_available = NULL;
	bool *probe_alias = NULL;
	unsigned int *missing_member = NULL;
	u64 *original_ticks = NULL;
	u64 *probe_only_ticks = NULL;
	u64 *joint_ticks = NULL;
	u64 *control_ticks = NULL;
	struct file *file = NULL;
	loff_t position = 0;
	unsigned int pool_count;
	unsigned int active_items;
	unsigned int required_pool;
	unsigned int selected_attempt = 0;
	unsigned int reduction_step_count = 0;
	unsigned int max_repetitions;
	unsigned int available_bits = 0;
	unsigned int bit;
	unsigned int pass;
	phys_addr_t base_probe_pa;
	struct sfbs_metric baseline_metric;
	struct sfbs_metric reduced_validation;
	struct sfbs_metric same_pass_validation;
	int ret;

	if (access_items != SFBS_STIMULUS_SLOTS ||
	    (((scan_order == 6U || scan_order == 8U) &&
	      sfcp_active_addrmask != 0x1ffffffULL &&
	      sfcp_active_addrmask != 0x7fffffULL) ||
	     (scan_order != 6U && scan_order != 8U &&
	      sfcp_active_addrmask != 0x1ffffffULL)) ||
	    bit_first < SFBS_MIN_TEST_BIT || bit_last > SFBS_MAX_TEST_BIT ||
	    bit_first > bit_last ||
	    !bit_repetitions || bit_repetitions > SFBS_MAX_REPETITIONS ||
	    !candidate_items || candidate_items > SFBS_STIMULUS_SLOTS ||
	    !target_candidate_items || target_candidate_items > candidate_items ||
	    !reduction_repetitions ||
	    reduction_repetitions > SFBS_MAX_REPETITIONS ||
	    !reduction_validation_repetitions ||
	    reduction_validation_repetitions > SFBS_MAX_REPETITIONS ||
	    !scan_passes || scan_passes > 10U ||
	    !search_groups || !search_repetitions ||
	    search_repetitions > SFBS_MAX_REPETITIONS ||
	    !baseline_repetitions ||
	    baseline_repetitions > SFBS_MAX_REPETITIONS ||
	    !trigger_threshold || !stable_percent || stable_percent > 100U ||
	    !latency_bin1 || latency_bin1 >= latency_bin2 ||
	    latency_bin2 >= latency_bin3 ||
	    single_max_percent > 100U ||
	    (joint_rescue_mask & ~GENMASK_ULL(SFBS_MAX_TEST_BIT, 0U)) ||
	    ((scan_order == 6U || scan_order == 7U || scan_order == 8U) &&
	     (item_sweep_first > item_sweep_last ||
	      item_sweep_last > candidate_items ||
	      !item_sweep_repetitions ||
	      item_sweep_repetitions > SFBS_MAX_REPETITIONS ||
	      !sfbs_sweep_rounds_are_valid() ||
	      !attribution_control_repetitions ||
	      attribution_control_repetitions > SFBS_MAX_REPETITIONS)) ||
	    !stimulus_mask ||
	    (stimulus_mask & ~GENMASK_ULL(SFBS_STIMULUS_SLOTS - 1U, 0U))) {
		pr_err(SFCP_NAME ": scan_order=%u has invalid mask or measurement parameters\n",
		       scan_order);
		return -EINVAL;
	}

	pool = kvmalloc_array(SFBS_POOL_CAPACITY, sizeof(*pool), GFP_KERNEL);
	original_group = kvmalloc_array(SFBS_GROUP_LINES,
					sizeof(*original_group), GFP_KERNEL);
	probe_only_group = kvmalloc_array(SFBS_GROUP_LINES,
					sizeof(*probe_only_group), GFP_KERNEL);
	reduction_scratch = kvmalloc_array(SFBS_GROUP_LINES,
					sizeof(*reduction_scratch), GFP_KERNEL);
	sweep_group = kvmalloc_array(SFBS_GROUP_LINES,
				    sizeof(*sweep_group), GFP_KERNEL);
	sweep_baseline = kvmalloc_array(SFBS_GROUP_LINES,
				       sizeof(*sweep_baseline), GFP_KERNEL);
	probe_lines = kvcalloc(64U, sizeof(*probe_lines), GFP_KERNEL);
	joint_groups = kvcalloc(64U * SFBS_GROUP_LINES,
				sizeof(*joint_groups), GFP_KERNEL);
	reduction_steps = kvcalloc(SFBS_STIMULUS_SLOTS,
				   sizeof(*reduction_steps), GFP_KERNEL);
	probe_available = kvcalloc(64U, sizeof(*probe_available), GFP_KERNEL);
	joint_available = kvcalloc(64U, sizeof(*joint_available), GFP_KERNEL);
	probe_alias = kvcalloc(64U, sizeof(*probe_alias), GFP_KERNEL);
	missing_member = kvmalloc_array(64U, sizeof(*missing_member), GFP_KERNEL);
	max_repetitions = max(bit_repetitions,
			      max(search_repetitions,
				  max(baseline_repetitions,
				      max(reduction_validation_repetitions,
					  max(item_sweep_repetitions,
					      attribution_control_repetitions)))));
	original_ticks = kvmalloc_array(max_repetitions,
					sizeof(*original_ticks), GFP_KERNEL);
	probe_only_ticks = kvmalloc_array(max_repetitions,
					  sizeof(*probe_only_ticks), GFP_KERNEL);
	joint_ticks = kvmalloc_array(max_repetitions,
				     sizeof(*joint_ticks), GFP_KERNEL);
	control_ticks = kvmalloc_array(max_repetitions,
				       sizeof(*control_ticks), GFP_KERNEL);
	if (!pool || !original_group || !probe_only_group ||
	    !reduction_scratch || !sweep_group || !sweep_baseline ||
	    !probe_lines || !joint_groups || !reduction_steps ||
	    !probe_available || !joint_available ||
	    !probe_alias || !missing_member ||
	    !original_ticks || !probe_only_ticks || !joint_ticks ||
	    !control_ticks) {
		ret = -ENOMEM;
		goto out;
	}

	sort(sfcp_pages, sfcp_allocated_pages, sizeof(*sfcp_pages),
	     sfbs_page_phys_compare, NULL);
	ret = sfbs_collect_pool(pool, &pool_count);
	if (ret)
		goto out;
	required_pool = auto_find_stable ? candidate_items + 1U :
		sfbs_rank[SFBS_STIMULUS_SLOTS] + 1U;
	if (!auto_find_stable) {
		for (bit = 0; bit < SFBS_STIMULUS_SLOTS; ++bit) {
			if (stimulus_mask & BIT_ULL(bit))
				required_pool = max(required_pool,
					(unsigned int)sfbs_rank[bit] + 1U);
		}
	}
	if (pool_count < required_pool) {
		pr_err(SFCP_NAME ": joint scan needs %u owned candidates, got %u; increase pool_pages\n",
		       required_pool, pool_count);
		ret = -ENOSPC;
		goto out;
	}

	sfcp_enable_counter();
	if (!sfcp_counter_calibration_delta) {
		ret = -EOPNOTSUPP;
		goto out_counter;
	}
	memset(&baseline_metric, 0, sizeof(baseline_metric));
	if (auto_find_stable) {
		ret = sfbs_find_stable_group(pool, pool_count, original_group,
			original_ticks, &baseline_metric, &selected_attempt);
		active_items = candidate_items;
	} else {
		active_items = sfbs_build_group(pool, original_group);
		ret = sfbs_measure_single(original_group, active_items,
			baseline_repetitions, &baseline_metric, original_ticks);
		if (!ret && !sfbs_metric_is_stable(&baseline_metric))
			ret = -EAGAIN;
	}
	if (ret) {
		pr_err(SFCP_NAME ": joint scan baseline is not stable (%u/%u)\n",
		       baseline_metric.successes, baseline_metric.repetitions);
		goto out_counter;
	}
	memset(&reduced_validation, 0, sizeof(reduced_validation));
	ret = sfbs_reduce_candidate_group(original_group, reduction_scratch,
		&active_items, original_ticks, reduction_steps,
		&reduction_step_count, &reduced_validation);
	if (ret)
		goto out_counter;
	if (scan_order == 6U || scan_order == 7U || scan_order == 8U) {
		ret = sfbs_build_nested_sweep_group(original_group, active_items,
			reduction_steps, reduction_step_count, sweep_group);
		if (ret)
			goto out_counter;
		sfbs_build_unrelated_control(sweep_group, sweep_baseline,
			candidate_items, 6U);
	}
	base_probe_pa = sfcp_physical(
		&original_group[SFBS_STIMULUS_SLOTS]);
	if (scan_order != 8U) {
		for (bit = bit_first; bit <= bit_last; ++bit) {
		struct sfcp_line *joint_group =
			&joint_groups[bit * SFBS_GROUP_LINES];
		bool rescue_requested = scan_order == 5U ||
			(scan_order == 6U &&
			 (joint_rescue_mask & BIT_ULL(bit)));
		phys_addr_t target_pa = base_probe_pa ^ BIT_ULL(bit);

		missing_member[bit] = SFBS_NO_SECOND_BIT;
		probe_available[bit] = sfbs_find_owned_line(target_pa,
			&probe_lines[bit]);
		if (probe_available[bit] && sfbs_probe_aliases_stimulus(
			&probe_lines[bit],
			scan_order == 7U ? sweep_group : original_group,
			scan_order == 7U ? candidate_items : active_items)) {
			probe_alias[bit] = true;
			probe_available[bit] = false;
		}
		if (rescue_requested)
			joint_available[bit] = sfbs_build_joint_bit_group(
				original_group, active_items, bit, joint_group,
				&probe_alias[bit], &missing_member[bit]);
		if ((scan_order == 5U && joint_available[bit]) ||
		    ((scan_order == 6U || scan_order == 7U) &&
		     probe_available[bit]))
			++available_bits;
		pr_info(SFCP_NAME ": bit=%u probe_available=%u joint_requested=%u joint_available=%u missing_member=%u probe_alias=%u probe=0x%llx\n",
			bit, probe_available[bit], rescue_requested,
			joint_available[bit], missing_member[bit],
			probe_alias[bit],
			(unsigned long long)target_pa);
		}
		if (!available_bits) {
			pr_err(SFCP_NAME ": no requested owned probe/joint target is available in bits %u..%u\n",
			       bit_first, bit_last);
			ret = -ENOSPC;
			goto out_counter;
		}
	}
	ret = sfbs_release_unused_joint_pages(original_group, active_items,
		probe_lines, probe_available, joint_groups, joint_available,
		(scan_order == 6U || scan_order == 7U || scan_order == 8U) ?
			sweep_group : NULL,
		(scan_order == 6U || scan_order == 7U || scan_order == 8U) ?
			candidate_items : 0U);
	if (ret)
		goto out_counter;

	file = filp_open(result_path, O_CREAT | O_TRUNC | O_WRONLY, 0644);
	if (IS_ERR(file)) {
		ret = PTR_ERR(file);
		file = NULL;
		goto out_counter;
	}
	ret = sfbs_write_header(file, &position, pool_count, active_items,
				base_probe_pa, selected_attempt);
	if (!ret)
		ret = sfbs_write_candidates(file, &position, original_group,
					    active_items, base_probe_pa);
	if (!ret)
		ret = sfbs_write_metric(file, &position, "selection_validation",
			0U, SFBS_NO_SECOND_BIT, SFBS_NO_SECOND_BIT,
			"initial_matching", true, "owned_exact", candidate_items,
			&baseline_metric, base_probe_pa, base_probe_pa, 0ULL);
	if (!ret)
		ret = sfbs_write_reduction_trace(file, &position,
			reduction_steps, reduction_step_count, base_probe_pa);
	if (!ret)
		ret = sfbs_write_metric(file, &position, "selection_validation",
			0U, SFBS_NO_SECOND_BIT, SFBS_NO_SECOND_BIT,
			"reduced_matching", true, "owned_exact", active_items,
			&reduced_validation, base_probe_pa, base_probe_pa, 0ULL);
	if (scan_order == 6U || scan_order == 8U) {
		unsigned int items;
		unsigned int round_index;
		unsigned int saved_fill_rounds = fill_rounds;

		for (round_index = 0; !ret &&
		     round_index < item_sweep_rounds_count; ++round_index) {
			fill_rounds = item_sweep_rounds[round_index];
			for (items = item_sweep_last; !ret; --items) {
			struct sfbs_metric metrics[4];
			u64 *ticks[4] = {
				original_ticks, probe_only_ticks,
				joint_ticks, control_ticks
			};
			static const char * const variants[4] = {
				"same_baseline", "same_candidate",
				"cross_baseline", "cross_candidate"
			};
			unsigned int index;

			ret = sfbs_measure_locality_quad(sweep_baseline,
				sweep_group, items, item_sweep_repetitions,
				items + round_index, metrics, ticks);
			for (index = 0; !ret && index < 4U; ++index)
				ret = sfbs_write_metric(file, &position, "item_sweep",
					items, SFBS_NO_SECOND_BIT,
					SFBS_NO_SECOND_BIT, variants[index], true,
					(index & 1U) ? "owned_candidate" :
						"owned_equal_load_noncongruent",
					items, &metrics[index], base_probe_pa,
					base_probe_pa, (index & 1U) ? 0ULL :
						BIT_ULL(6));
			if (!ret)
				pr_info(SFCP_NAME ": item_sweep rounds=%u n=%u same(b=%u c=%u) cross(b=%u c=%u) /%u\n",
					fill_rounds, items, metrics[0].successes,
					metrics[1].successes, metrics[2].successes,
					metrics[3].successes,
					item_sweep_repetitions);
			if (items == item_sweep_first)
				break;
			}
		}
		fill_rounds = saved_fill_rounds;
		if (scan_order == 8U)
			goto scans_done;
	}
	if (scan_order == 7U) {
		unsigned int saved_fill_rounds = fill_rounds;
		unsigned int round_index;
		unsigned int items;
		static const char * const variants[4] = {
			"same_original_matching", "same_probe_only_flipped",
			"cross_original_matching", "cross_probe_only_flipped"
		};

		/*
		 * Each pass shuffles the full candidate set once and then uses nested
		 * prefixes for X.  For every bit and X, compare the original and
		 * probe-only-flipped lookup under both localities.  The four cells are
		 * interleaved and the union of both groups is cleaned before each cell,
		 * so condition order cannot leave a competing probe resident in a
		 * private or shared cache.
		 */
		for (bit = bit_first; !ret && bit <= bit_last; ++bit) {
			if (probe_available[bit])
				continue;
			ret = sfbs_write_metric(file, &position, "bit_item_sweep",
				bit, SFBS_NO_SECOND_BIT, SFBS_NO_SECOND_BIT,
				"probe_only_flipped", false,
				probe_alias[bit] ? "probe_candidate_alias" :
					"flipped_probe_not_owned",
				0U, NULL, base_probe_pa ^ BIT_ULL(bit),
				base_probe_pa, BIT_ULL(bit));
		}
		for (pass = 1U; !ret && pass <= scan_passes; ++pass) {
			sfbs_current_pass = pass;
			sfbs_shuffle_sweep_stimuli(sweep_group, candidate_items);
			for (round_index = 0; !ret &&
			     round_index < item_sweep_rounds_count; ++round_index) {
				fill_rounds = item_sweep_rounds[round_index];
				for (bit = bit_first; !ret && bit <= bit_last; ++bit) {
					phys_addr_t target_pa =
						base_probe_pa ^ BIT_ULL(bit);

					if (!probe_available[bit])
						continue;
					memcpy(probe_only_group, sweep_group,
					       SFBS_GROUP_LINES *
						       sizeof(*probe_only_group));
					probe_only_group[SFBS_STIMULUS_SLOTS] =
						probe_lines[bit];
					for (items = item_sweep_last; !ret; --items) {
						struct sfbs_metric metrics[4];
						u64 *ticks[4] = {
							original_ticks, probe_only_ticks,
							joint_ticks, control_ticks
						};
						unsigned int index;

						ret = sfbs_measure_locality_quad(
							sweep_group, probe_only_group, items,
							item_sweep_repetitions,
							items + bit + pass + round_index,
							metrics, ticks);
						for (index = 0; !ret && index < 4U;
						     ++index)
							ret = sfbs_write_metric(file,
								&position, "bit_item_sweep",
								bit, SFBS_NO_SECOND_BIT,
								SFBS_NO_SECOND_BIT,
								variants[index], true,
								"owned_exact", items,
								&metrics[index],
								(index & 1U) ? target_pa :
									base_probe_pa,
								(index & 1U) ? base_probe_pa :
									target_pa,
								BIT_ULL(bit));
						if (!ret)
							pr_info(SFCP_NAME ": bit_item pass=%u rounds=%u bit=%u x=%u same(o=%u p=%u) cross(o=%u p=%u) /%u\n",
								pass, fill_rounds, bit, items,
								metrics[0].successes,
								metrics[1].successes,
								metrics[2].successes,
								metrics[3].successes,
								item_sweep_repetitions);
						if (items == item_sweep_first)
							break;
					}
				}
			}
		}
		fill_rounds = saved_fill_rounds;
		goto scans_done;
	}

	for (bit = bit_first; !ret && bit <= bit_last; ++bit) {
		struct sfcp_line *joint_group =
			&joint_groups[bit * SFBS_GROUP_LINES];
		bool rescue_requested = scan_order == 5U ||
			(joint_rescue_mask & BIT_ULL(bit));
		unsigned int i;

		if (!probe_available[bit]) {
			const char *status = probe_alias[bit] ?
				"probe_candidate_alias" : "flipped_probe_not_owned";

			ret = sfbs_write_metric(file, &position, "cross_bit_test",
				bit, missing_member[bit], SFBS_NO_SECOND_BIT,
				"probe_only_flipped", false, status,
				active_items, NULL, base_probe_pa ^ BIT_ULL(bit),
				base_probe_pa, BIT_ULL(bit));
		}
		if (!ret && rescue_requested && !joint_available[bit])
			ret = sfbs_write_metric(file, &position, "joint_bit_test",
				bit, missing_member[bit], SFBS_NO_SECOND_BIT,
				"joint_group_flipped", false,
				"incomplete_owned_xor_group", active_items, NULL,
				base_probe_pa ^ BIT_ULL(bit), base_probe_pa,
				BIT_ULL(bit));
		if (!joint_available[bit])
			continue;
		for (i = 0; !ret && i < active_items; ++i)
			ret = sfbs_write_metric(file, &position,
				"joint_candidate_member", bit, i,
				SFBS_NO_SECOND_BIT, "stimulus_xor", true,
				"owned_exact", active_items, NULL,
				sfcp_physical(&joint_group[i]),
				sfcp_physical(&original_group[i]), BIT_ULL(bit));
	}

	for (pass = 1U; !ret && pass <= scan_passes; ++pass) {
		struct sfbs_metric pass_validation;

		sfbs_current_pass = pass;
		ret = sfbs_measure_single(original_group, active_items,
			baseline_repetitions, &pass_validation, original_ticks);
		if (!ret)
			ret = sfbs_write_metric(file, &position, "pass_validation",
				0U, SFBS_NO_SECOND_BIT, SFBS_NO_SECOND_BIT,
				scan_order == 6U ? "cross_fixed_set_matching" :
					"fixed_set_matching", true, "owned_exact",
				active_items, &pass_validation, base_probe_pa,
				base_probe_pa, 0ULL);
		if (!ret && !sfbs_metric_is_stable(&pass_validation)) {
			pr_err(SFCP_NAME ": joint scan pass %u baseline drifted to %u/%u\n",
			       pass, pass_validation.successes,
			       pass_validation.repetitions);
			ret = -EAGAIN;
		}
		if (!ret && scan_order == 6U)
			ret = sfbs_measure_same_single(original_group, active_items,
				baseline_repetitions, &same_pass_validation,
				original_ticks);
		if (!ret && scan_order == 6U)
			ret = sfbs_write_metric(file, &position, "pass_validation",
				0U, SFBS_NO_SECOND_BIT, SFBS_NO_SECOND_BIT,
				"same_fixed_set_matching", true, "owned_exact",
				active_items, &same_pass_validation, base_probe_pa,
				base_probe_pa, 0ULL);
		for (bit = bit_first; !ret && bit <= bit_last; ++bit) {
			struct sfcp_line *joint_group =
				&joint_groups[bit * SFBS_GROUP_LINES];
			bool rescue_requested = scan_order == 5U ||
				(joint_rescue_mask & BIT_ULL(bit));
			bool rescue_available = rescue_requested &&
				joint_available[bit];
			struct sfbs_metric original_metric;
			struct sfbs_metric probe_only_metric;
			struct sfbs_metric joint_metric;
			struct sfbs_metric same_original_metric;
			struct sfbs_metric same_probe_only_metric;
			struct sfbs_metric same_joint_metric;
			struct sfbs_metric idle_original;
			struct sfbs_metric idle_flipped;
			phys_addr_t target_pa = base_probe_pa ^ BIT_ULL(bit);
			const char *classification;

			if (!probe_available[bit] ||
			    (scan_order == 5U && !rescue_available))
				continue;
			memcpy(probe_only_group, original_group,
			       SFBS_GROUP_LINES * sizeof(*probe_only_group));
			probe_only_group[SFBS_STIMULUS_SLOTS] =
				probe_lines[bit];
			memset(&joint_metric, 0, sizeof(joint_metric));
			if (rescue_available)
				ret = sfbs_measure_triplet(original_group,
					probe_only_group, joint_group, active_items,
					bit_repetitions, bit + pass, true,
					&original_metric, &probe_only_metric,
					&joint_metric, original_ticks,
					probe_only_ticks, joint_ticks);
			else
				ret = sfbs_measure_cross_pair_clean(original_group,
					probe_only_group, active_items,
					bit_repetitions, bit + pass,
					&original_metric, &probe_only_metric,
					original_ticks, probe_only_ticks);
			if (ret)
				break;
			ret = sfbs_write_metric(file, &position, "cross_bit_test",
				bit, SFBS_NO_SECOND_BIT, SFBS_NO_SECOND_BIT,
				"original_matching", true, "owned_exact",
				active_items, &original_metric, base_probe_pa,
				target_pa, BIT_ULL(bit));
			if (!ret)
				ret = sfbs_write_metric(file, &position,
					"cross_bit_test", bit, SFBS_NO_SECOND_BIT,
					SFBS_NO_SECOND_BIT, "probe_only_flipped", true,
					"owned_exact", active_items, &probe_only_metric,
					target_pa, base_probe_pa, BIT_ULL(bit));
			if (!ret && rescue_available)
				ret = sfbs_write_metric(file, &position,
					"joint_bit_test", bit, SFBS_NO_SECOND_BIT,
					SFBS_NO_SECOND_BIT, "joint_group_flipped", true,
					"owned_exact", active_items, &joint_metric,
					target_pa, base_probe_pa, BIT_ULL(bit));
			if (ret)
				break;
			if (!sfbs_metric_is_stable(&original_metric))
				classification = "invalid_original_drift";
			else if ((u64)probe_only_metric.successes * 100ULL >
				 (u64)probe_only_metric.repetitions * single_max_percent)
				classification = "probe_only_not_isolated";
			else if (!rescue_requested)
				classification = "probe_only_isolated";
			else if (!rescue_available)
				classification = "joint_unavailable_probe_only_isolated";
			else if (sfbs_metric_is_stable(&joint_metric))
				classification = "selector_rescue";
			else
				classification = "joint_not_restored_inconclusive";
			ret = sfbs_write_metric(file, &position,
				"cross_bit_classification", bit,
				SFBS_NO_SECOND_BIT, SFBS_NO_SECOND_BIT,
				classification, true,
				rescue_available ? "derived_from_triplet" :
					"derived_from_cross_pair",
				active_items, rescue_available ? &joint_metric :
					&probe_only_metric, target_pa,
				base_probe_pa, BIT_ULL(bit));
			if (ret)
				break;
			if (scan_order == 6U && !attribution_cross_only_bits &&
			    rescue_available) {
				const char *locality_class;
				bool cross_rescue;
				bool same_all_low;
				bool same_all_high;
				bool same_rescue;

				ret = sfbs_measure_triplet(original_group,
					probe_only_group, joint_group, active_items,
					bit_repetitions, bit + pass + 1U, false,
					&same_original_metric, &same_probe_only_metric,
					&same_joint_metric, original_ticks,
					probe_only_ticks, joint_ticks);
				if (!ret)
					ret = sfbs_write_metric(file, &position,
						"locality_bit_test", bit,
						SFBS_NO_SECOND_BIT, SFBS_NO_SECOND_BIT,
						"same_original_matching", true,
						"owned_exact", active_items,
						&same_original_metric, base_probe_pa,
						target_pa, BIT_ULL(bit));
				if (!ret)
					ret = sfbs_write_metric(file, &position,
						"locality_bit_test", bit,
						SFBS_NO_SECOND_BIT, SFBS_NO_SECOND_BIT,
						"same_probe_only_flipped", true,
						"owned_exact", active_items,
						&same_probe_only_metric, target_pa,
						base_probe_pa, BIT_ULL(bit));
				if (!ret)
					ret = sfbs_write_metric(file, &position,
						"locality_bit_test", bit,
						SFBS_NO_SECOND_BIT, SFBS_NO_SECOND_BIT,
						"same_joint_group_flipped", true,
						"owned_exact", active_items,
						&same_joint_metric, target_pa,
						base_probe_pa, BIT_ULL(bit));
				if (ret)
					break;
				cross_rescue = sfbs_metric_is_stable(&original_metric) &&
					(u64)probe_only_metric.successes * 100ULL <=
					(u64)probe_only_metric.repetitions *
						single_max_percent &&
					sfbs_metric_is_stable(&joint_metric);
				same_all_high =
					sfbs_metric_is_stable(&same_original_metric) &&
					sfbs_metric_is_stable(&same_probe_only_metric) &&
					sfbs_metric_is_stable(&same_joint_metric);
				same_all_low =
					(u64)same_original_metric.successes * 100ULL <=
						(u64)same_original_metric.repetitions *
							single_max_percent &&
					(u64)same_probe_only_metric.successes * 100ULL <=
						(u64)same_probe_only_metric.repetitions *
							single_max_percent &&
					(u64)same_joint_metric.successes * 100ULL <=
						(u64)same_joint_metric.repetitions *
							single_max_percent;
				same_rescue =
					sfbs_metric_is_stable(&same_original_metric) &&
					(u64)same_probe_only_metric.successes * 100ULL <=
						(u64)same_probe_only_metric.repetitions *
							single_max_percent &&
					sfbs_metric_is_stable(&same_joint_metric);
				if (!sfbs_metric_is_stable(&original_metric))
					locality_class = "invalid_cross_original_drift";
				else if (cross_rescue && same_all_low)
					locality_class = "cross_only_no_same_eviction";
				else if (cross_rescue && same_all_high)
					locality_class = "cross_only_bit_sensitive";
				else if (cross_rescue && same_rescue)
					locality_class = "shared_cache_and_cross_rescue";
				else if (cross_rescue)
					locality_class = "same_path_mixed";
				else if (bit < 6U &&
					 sfbs_metric_is_stable(&original_metric) &&
					 sfbs_metric_is_stable(&probe_only_metric) &&
					 sfbs_metric_is_stable(&joint_metric))
					locality_class = "intra_line_negative_control";
				else
					locality_class = "no_cross_rescue";
				ret = sfbs_write_metric(file, &position,
					"locality_bit_classification", bit,
					SFBS_NO_SECOND_BIT, SFBS_NO_SECOND_BIT,
					locality_class, true, "same_cross_derived",
					active_items, &joint_metric, target_pa,
					base_probe_pa, BIT_ULL(bit));
				pr_info(SFCP_NAME ": locality pass=%u bit=%u same(o=%u p=%u j=%u) cross(o=%u p=%u j=%u) class=%s\n",
					pass, bit, same_original_metric.successes,
					same_probe_only_metric.successes,
					same_joint_metric.successes,
					original_metric.successes,
					probe_only_metric.successes,
					joint_metric.successes, locality_class);
			}
			ret = sfbs_measure_pair(original_group, probe_only_group, 0U,
				scan_order == 6U ? attribution_control_repetitions :
					bit_repetitions,
				&idle_original, &idle_flipped,
				original_ticks, probe_only_ticks);
			if (!ret)
				ret = sfbs_write_metric(file, &position,
					"cross_bit_idle", bit, SFBS_NO_SECOND_BIT,
					SFBS_NO_SECOND_BIT, "original_idle", true,
					"owned_exact", 0U, &idle_original,
					base_probe_pa, target_pa, BIT_ULL(bit));
			if (!ret)
				ret = sfbs_write_metric(file, &position,
					"cross_bit_idle", bit, SFBS_NO_SECOND_BIT,
					SFBS_NO_SECOND_BIT, "flipped_idle", true,
					"owned_exact", 0U, &idle_flipped,
					target_pa, base_probe_pa, BIT_ULL(bit));
			if (!ret && scan_order == 6U &&
			    !attribution_cross_only_bits)
				ret = sfbs_measure_same_single(original_group, 0U,
					attribution_control_repetitions,
					&same_original_metric, original_ticks);
			if (!ret && scan_order == 6U &&
			    !attribution_cross_only_bits)
				ret = sfbs_measure_same_single(probe_only_group, 0U,
					attribution_control_repetitions,
					&same_probe_only_metric, probe_only_ticks);
			if (!ret && scan_order == 6U &&
			    !attribution_cross_only_bits)
				ret = sfbs_write_metric(file, &position,
					"locality_bit_idle", bit,
					SFBS_NO_SECOND_BIT, SFBS_NO_SECOND_BIT,
					"same_original_idle", true, "owned_exact",
					0U, &same_original_metric, base_probe_pa,
					target_pa, BIT_ULL(bit));
			if (!ret && scan_order == 6U &&
			    !attribution_cross_only_bits)
				ret = sfbs_write_metric(file, &position,
					"locality_bit_idle", bit,
					SFBS_NO_SECOND_BIT, SFBS_NO_SECOND_BIT,
					"same_flipped_idle", true, "owned_exact",
					0U, &same_probe_only_metric, target_pa,
					base_probe_pa, BIT_ULL(bit));
			if (rescue_available)
				pr_info(SFCP_NAME ": cross pass=%u bit=%u original=%u/%u probe_only=%u/%u joint=%u/%u class=%s\n",
					pass, bit, original_metric.successes,
					original_metric.repetitions,
					probe_only_metric.successes,
					probe_only_metric.repetitions,
					joint_metric.successes,
					joint_metric.repetitions, classification);
			else
				pr_info(SFCP_NAME ": cross pass=%u bit=%u original=%u/%u probe_only=%u/%u class=%s\n",
					pass, bit, original_metric.successes,
					original_metric.repetitions,
					probe_only_metric.successes,
					probe_only_metric.repetitions,
					classification);
		}
	}
scans_done:
	sfbs_current_pass = 0U;
	if (file) {
		filp_close(file, NULL);
		file = NULL;
	}
	if (!ret) {
		if (scan_order == 8U)
			pr_info(SFCP_NAME
				": wrote owned same/cross item sweep to %s\n",
				result_path);
		else
			pr_info(SFCP_NAME
				": wrote owned %s (%u complete bits) to %s\n",
				scan_order == 7U ?
					"per-bit same/cross item sweep" :
					"cross/joint bit scan",
				available_bits, result_path);
	}

out_counter:
	sfcp_restore_counter();
out:
	if (file)
		filp_close(file, NULL);
	kvfree(joint_ticks);
	kvfree(control_ticks);
	kvfree(probe_only_ticks);
	kvfree(original_ticks);
	kvfree(missing_member);
	kvfree(probe_alias);
	kvfree(joint_available);
	kvfree(probe_available);
	kvfree(reduction_steps);
	kvfree(joint_groups);
	kvfree(probe_lines);
	kvfree(sweep_baseline);
	kvfree(sweep_group);
	kvfree(reduction_scratch);
	kvfree(probe_only_group);
	kvfree(original_group);
	kvfree(pool);
	return ret;
}

static int sfbs_run_experiment(void)
{
	if (scan_order == 1U)
		return sfbs_run_single_experiment();
	if (scan_order == 2U || scan_order == 3U)
		return sfbs_run_pair_experiment();
	if (scan_order == 4U)
		return sfbs_run_color_matrix_experiment();
	if (scan_order == 5U || scan_order == 6U || scan_order == 7U ||
	    scan_order == 8U)
		return sfbs_run_joint_bit_experiment();
	pr_err(SFCP_NAME ": scan_order must be 1, 2, 3, 4, 5, 6, 7, or 8\n");
	return -EINVAL;
}
