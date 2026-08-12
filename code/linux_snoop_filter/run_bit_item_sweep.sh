#!/bin/sh
set -eu

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)

pool_pages=${POOL_PAGES:-655360}
probe_cpu=${PROBE_CPU:-7}
stimulus_cpu=${STIMULUS_CPU:-6}
candidate_items=${CANDIDATE_ITEMS:-24}
target_candidate_items=${TARGET_CANDIDATE_ITEMS:-16}
search_groups=${SEARCH_GROUPS:-2000}
search_repetitions=${SEARCH_REPETITIONS:-20}
baseline_repetitions=${BASELINE_REPETITIONS:-1000}
reduction_repetitions=${REDUCTION_REPETITIONS:-20}
reduction_validation_repetitions=${REDUCTION_VALIDATION_REPETITIONS:-200}
scan_passes=${SCAN_PASSES:-1}
bit_first=${BIT_FIRST:-0}
bit_last=${BIT_LAST:-24}
item_first=${ITEM_FIRST:-0}
item_last=${ITEM_LAST:-20}
measurement_repetitions=${MEASUREMENT_REPETITIONS:-200}
fill_rounds_sweep=${FILL_ROUNDS_SWEEP:-100}
trigger_threshold=${TRIGGER_THRESHOLD:-150}
stable_percent=${STABLE_PERCENT:-95}
result_path=${RESULT_PATH:-/tmp/sf_bit_item_sweep.csv}
run_id=${RUN_ID:-bit_item_run01}

for argument in "$@"; do
	case "$argument" in
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
	bit_first=*) bit_first=${argument#*=} ;;
	bit_last=*) bit_last=${argument#*=} ;;
	item_first=*) item_first=${argument#*=} ;;
	item_last=*) item_last=${argument#*=} ;;
	measurement_repetitions=*) measurement_repetitions=${argument#*=} ;;
	fill_rounds_sweep=*) fill_rounds_sweep=${argument#*=} ;;
	trigger_threshold=*) trigger_threshold=${argument#*=} ;;
	stable_percent=*) stable_percent=${argument#*=} ;;
	result_path=*) result_path=${argument#*=} ;;
	run_id=*) run_id=${argument#*=} ;;
	*) echo "Unknown option: $argument" >&2; exit 2 ;;
	esac
done

case "$run_id:$result_path" in
*','*) echo "run_id and result_path must not contain commas" >&2; exit 2 ;;
esac

echo "Per-bit X sweep: CPU $stimulus_cpu -> CPU $probe_cpu, bits $bit_first..$bit_last, X=$item_first..$item_last." >&2
echo "X counts stimulus lines only; the probe is an additional line." >&2

exec sh "$script_dir/run_bitscan.sh" \
	scan_order=7 \
	experiment_run_id="$run_id" \
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
	bit_first="$bit_first" \
	bit_last="$bit_last" \
	bit_repetitions="$measurement_repetitions" \
	item_sweep_first="$item_first" \
	item_sweep_last="$item_last" \
	item_sweep_repetitions="$measurement_repetitions" \
	item_sweep_rounds="$fill_rounds_sweep" \
	fill_rounds="$fill_rounds_sweep" \
	trigger_threshold="$trigger_threshold" \
	stable_percent="$stable_percent" \
	result_path="$result_path"
