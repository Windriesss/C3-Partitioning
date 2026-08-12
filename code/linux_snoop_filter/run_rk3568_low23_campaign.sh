#!/bin/sh
set -eu

# RK3568 fresh-allocation campaign with PA[22:0] fixed.  This mode records
# only the ordinary same/cross baseline/candidate item sweep.  It performs no
# probe-bit or joint-group flips.

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)

run_count=${RUN_COUNT:-5}
start_run=${START_RUN:-1}
allocation_retries=${ALLOCATION_RETRIES:-10}
output_dir=${OUTPUT_DIR:-/root/sf-rk3568-low23-itemsweep}
pool_pages=${POOL_PAGES:-329376}
probe_cpu=${PROBE_CPU:-3}
stimulus_cpu=${STIMULUS_CPU:-2}
candidate_items=${CANDIDATE_ITEMS:-20}
target_candidate_items=${TARGET_CANDIDATE_ITEMS:-16}
search_groups=${SEARCH_GROUPS:-2000}
search_repetitions=${SEARCH_REPETITIONS:-20}
baseline_repetitions=${BASELINE_REPETITIONS:-1000}
reduction_repetitions=${REDUCTION_REPETITIONS:-20}
reduction_validation_repetitions=${REDUCTION_VALIDATION_REPETITIONS:-200}
scan_passes=${SCAN_PASSES:-1}
measurement_repetitions=${MEASUREMENT_REPETITIONS:-1000}
fill_rounds_sweep=${FILL_ROUNDS_SWEEP:-100}
trigger_threshold=${TRIGGER_THRESHOLD:-150}
stable_percent=${STABLE_PERCENT:-95}
addrmask=${ADDRMASK:-0x7fffff}
target_low=${TARGET_LOW:-0x38e900}

for argument in "$@"; do
	case "$argument" in
	run_count=*) run_count=${argument#*=} ;;
	start_run=*) start_run=${argument#*=} ;;
	allocation_retries=*) allocation_retries=${argument#*=} ;;
	output_dir=*) output_dir=${argument#*=} ;;
	pool_pages=*) pool_pages=${argument#*=} ;;
	probe_cpu=*) probe_cpu=${argument#*=} ;;
	stimulus_cpu=*) stimulus_cpu=${argument#*=} ;;
	candidate_items=*) candidate_items=${argument#*=} ;;
	target_candidate_items=*) target_candidate_items=${argument#*=} ;;
	search_groups=*) search_groups=${argument#*=} ;;
	search_repetitions=*) search_repetitions=${argument#*=} ;;
	baseline_repetitions=*) baseline_repetitions=${argument#*=} ;;
	reduction_repetitions=*) reduction_repetitions=${argument#*=} ;;
	reduction_validation_repetitions=*) reduction_validation_repetitions=${argument#*=} ;;
	scan_passes=*) scan_passes=${argument#*=} ;;
	measurement_repetitions=*) measurement_repetitions=${argument#*=} ;;
	fill_rounds_sweep=*) fill_rounds_sweep=${argument#*=} ;;
	trigger_threshold=*) trigger_threshold=${argument#*=} ;;
	stable_percent=*) stable_percent=${argument#*=} ;;
	addrmask=*) addrmask=${argument#*=} ;;
	target_low=*) target_low=${argument#*=} ;;
	*) echo "Unknown option: $argument" >&2; exit 2 ;;
	esac
done

case "$run_count:$start_run:$allocation_retries" in
*[!0-9:]*|0:*|*:0:*|*:*:0)
	echo "run_count, start_run, and allocation_retries must be positive integers" >&2
	exit 2
	;;
esac
if [ "$start_run" -gt "$run_count" ]; then
	echo "start_run must not exceed run_count" >&2
	exit 2
fi

mkdir -p "$output_dir"
manifest="$output_dir/campaign_manifest.csv"
if [ ! -f "$manifest" ]; then
	printf '%s\n' 'run_id,allocation_attempt,stimulus_cpu,probe_cpu,addrmask,target_low,candidate_items,target_candidate_items,scan_passes,repetitions,fill_rounds,result_path' > "$manifest"
fi

run=$start_run
while [ "$run" -le "$run_count" ]; do
	run_tag=$(printf 'run%02d' "$run")
	result_path="$output_dir/rk3568_low23_${run_tag}_cpu${stimulus_cpu}_to_cpu${probe_cpu}.csv"
	attempt=1
	completed=0
	while [ "$attempt" -le "$allocation_retries" ]; do
		experiment_id="rk3568_low23_${run_tag}_allocation${attempt}"
		printf 'Low-23 fresh allocation %s, attempt %s/%s.\n' \
			"$run_tag" "$attempt" "$allocation_retries" >&2
		rm -f "$result_path"
		if sh "$script_dir/run_bitscan.sh" \
			scan_order=8 \
			experiment_run_id="$experiment_id" \
			addrmask="$addrmask" \
			target_low="$target_low" \
			pool_pages="$pool_pages" \
			probe_cpu="$probe_cpu" \
			stimulus_cpus="$stimulus_cpu" \
			auto_find_stable=1 \
			candidate_items="$candidate_items" \
			target_candidate_items="$target_candidate_items" \
			search_groups="$search_groups" \
			search_repetitions="$search_repetitions" \
			baseline_repetitions="$baseline_repetitions" \
			reduction_repetitions="$reduction_repetitions" \
			reduction_validation_repetitions="$reduction_validation_repetitions" \
			scan_passes="$scan_passes" \
			item_sweep_first=0 \
			item_sweep_last=20 \
			item_sweep_repetitions="$measurement_repetitions" \
			item_sweep_rounds="$fill_rounds_sweep" \
			fill_rounds="$fill_rounds_sweep" \
			trigger_threshold="$trigger_threshold" \
			stable_percent="$stable_percent" \
			result_path="$result_path"; then
			if grep -q "^# addrmask,$addrmask$" "$result_path" && \
			   grep -q "^# candidate_target_masked,$target_low$" "$result_path"; then
				completed=1
				break
			fi
			echo "Completed CSV failed low-23 metadata validation; retrying." >&2
		fi
		attempt=$((attempt + 1))
	done
	if [ "$completed" -ne 1 ]; then
		echo "No suitable allocation for $run_tag after $allocation_retries attempts." >&2
		exit 1
	fi
	printf '%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s\n' \
		"$run_tag" "$attempt" "$stimulus_cpu" "$probe_cpu" \
		"$addrmask" "$target_low" "$candidate_items" \
		"$target_candidate_items" "$scan_passes" \
		"$measurement_repetitions" "$fill_rounds_sweep" "$result_path" \
		>> "$manifest"
	printf 'Completed %s: %s\n' "$run_tag" "$result_path" >&2
	run=$((run + 1))
done

printf '%s\n' \
	"RK3568 low-23 campaign completed in $output_dir." \
	"Use item_sweep/{same,cross}_candidate for the low-23 curves." \
	"No address-bit flip is performed in scan_order=8." >&2
