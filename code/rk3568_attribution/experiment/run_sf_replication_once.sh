#!/bin/sh
set -eu

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)

run_id=${RUN_ID:-run01}
output_dir=${OUTPUT_DIR:-/tmp/rk3588_sf_replication}
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
measurement_repetitions=${MEASUREMENT_REPETITIONS:-1000}
fill_rounds=${FILL_ROUNDS:-1000}
scan_passes=${SCAN_PASSES:-3}
trigger_threshold=${TRIGGER_THRESHOLD:-150}
stable_percent=${STABLE_PERCENT:-95}
item_sweep_first=${ITEM_SWEEP_FIRST:-0}
item_sweep_last=${ITEM_SWEEP_LAST:-20}
bit_first=${BIT_FIRST:-0}
bit_last=${BIT_LAST:-24}
attribution_cross_only_bits=${ATTRIBUTION_CROSS_ONLY_BITS:-1}
color_bit0=${COLOR_BIT0:-16}
color_bit1=${COLOR_BIT1:-17}
joint_rescue_mask=${JOINT_RESCUE_MASK:-}
matrix_allocation_retries=${MATRIX_ALLOCATION_RETRIES:-10}
attribution_allocation_retries=${ATTRIBUTION_ALLOCATION_RETRIES:-10}
addrmask=${ADDRMASK:-0x1ffffff}
target_low=${TARGET_LOW:-0x138e900}

for argument in "$@"; do
	case "$argument" in
	run_id=*) run_id=${argument#*=} ;;
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
	measurement_repetitions=*) measurement_repetitions=${argument#*=} ;;
	fill_rounds=*) fill_rounds=${argument#*=} ;;
	scan_passes=*) scan_passes=${argument#*=} ;;
	trigger_threshold=*) trigger_threshold=${argument#*=} ;;
	stable_percent=*) stable_percent=${argument#*=} ;;
	item_sweep_first=*) item_sweep_first=${argument#*=} ;;
	item_sweep_last=*) item_sweep_last=${argument#*=} ;;
	bit_first=*) bit_first=${argument#*=} ;;
	bit_last=*) bit_last=${argument#*=} ;;
	attribution_cross_only_bits=*) attribution_cross_only_bits=${argument#*=} ;;
	color_bit0=*) color_bit0=${argument#*=} ;;
	color_bit1=*) color_bit1=${argument#*=} ;;
	joint_rescue_mask=*) joint_rescue_mask=${argument#*=} ;;
	matrix_allocation_retries=*) matrix_allocation_retries=${argument#*=} ;;
	attribution_allocation_retries=*) attribution_allocation_retries=${argument#*=} ;;
	addrmask=*) addrmask=${argument#*=} ;;
	target_low=*) target_low=${argument#*=} ;;
	*) echo "Unknown option: $argument" >&2; exit 2 ;;
	esac
done

case "$color_bit0:$color_bit1" in
*[!0-9:]*|:*|*:) echo "color_bit0 and color_bit1 must be integers" >&2; exit 2 ;;
esac
if [ "$color_bit0" -gt 47 ] || [ "$color_bit1" -gt 47 ] || \
   [ "$color_bit0" -ge "$color_bit1" ]; then
	echo "color_bit0 and color_bit1 must satisfy 0 <= bit0 < bit1 <= 47" >&2
	exit 2
fi
if [ "$addrmask" = "0x7fffff" ] && \
   { [ "$bit_last" -gt 22 ] || [ "$color_bit1" -gt 22 ]; }; then
	echo "With addrmask=0x7fffff, bit_last and both color bits must stay within PA[22:0]" >&2
	exit 2
fi
if [ -z "$joint_rescue_mask" ]; then
	joint_rescue_mask=$(printf '0x%x' \
		$(((1 << color_bit0) | (1 << color_bit1))))
fi

case "$run_id" in
*[,/[:space:]]*|'')
	echo "run_id must be a non-empty CSV/file-safe token" >&2
	exit 2
	;;
esac
case "$output_dir" in
*,*) echo "output_dir must not contain a comma" >&2; exit 2 ;;
esac

mkdir -p "$output_dir"
pair_tag="cpu${stimulus_cpu}_to_cpu${probe_cpu}"
attribution_id="${run_id}_${pair_tag}_attribution"
matrix_id="${run_id}_${pair_tag}_matrix"
attribution_csv="$output_dir/${attribution_id}.csv"
matrix_csv="$output_dir/${matrix_id}.csv"
manifest="$output_dir/${run_id}_${pair_tag}_manifest.csv"
environment="$output_dir/${run_id}_${pair_tag}_environment.txt"

printf '%s\n' \
	"Starting fresh-allocation replication $run_id ($stimulus_cpu -> $probe_cpu)." \
	"Candidate constraint: (PA & $addrmask) == $target_low." \
	"Phase 1/2: X=${item_sweep_first}..${item_sweep_last} same/cross sweep, cross-only bit ${bit_first}..${bit_last}, and PA[$color_bit1]/PA[$color_bit0] joint rescue (up to $attribution_allocation_retries allocations)." >&2

case "$attribution_allocation_retries" in
''|*[!0-9]*|0) echo "attribution_allocation_retries must be a positive integer" >&2; exit 2 ;;
esac
rm -f "$attribution_csv"
attribution_attempt=1
attribution_completed=0
while [ "$attribution_attempt" -le "$attribution_allocation_retries" ]; do
	attribution_attempt_id="${attribution_id}_allocation${attribution_attempt}"
	printf 'Attribution allocation attempt %s/%s.\n' \
		"$attribution_attempt" "$attribution_allocation_retries" >&2
	if sh "$script_dir/run_bitscan.sh" \
	scan_order=6 \
	experiment_run_id="$attribution_attempt_id" \
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
	item_sweep_first="$item_sweep_first" \
	item_sweep_last="$item_sweep_last" \
	item_sweep_repetitions="$measurement_repetitions" \
	item_sweep_rounds="$fill_rounds" \
	scan_passes="$scan_passes" \
	bit_first="$bit_first" \
	bit_last="$bit_last" \
	bit_repetitions="$measurement_repetitions" \
	attribution_cross_only_bits="$attribution_cross_only_bits" \
	color_bit0="$color_bit0" \
	color_bit1="$color_bit1" \
	joint_rescue_mask="$joint_rescue_mask" \
	attribution_control_repetitions=100 \
	latency_bin1=90 \
	latency_bin2=130 \
	latency_bin3=220 \
	single_max_percent=20 \
	fill_rounds="$fill_rounds" \
	trigger_threshold="$trigger_threshold" \
	stable_percent="$stable_percent" \
	result_path="$attribution_csv"; then
		if grep -q "^# addrmask,$addrmask$" "$attribution_csv" && \
		   grep -q "^# candidate_target_masked,$target_low$" "$attribution_csv"; then
			attribution_completed=1
			break
		fi
		echo "Attribution CSV failed candidate-constraint validation." >&2
	fi
	printf 'Attribution allocation %s was unsuitable; retrying with a newly allocated owned-page pool.\n' \
		"$attribution_attempt" >&2
	attribution_attempt=$((attribution_attempt + 1))
done
if [ "$attribution_completed" -ne 1 ]; then
	echo "No attribution allocation completed after $attribution_allocation_retries attempts." >&2
	exit 1
fi

printf '%s\n' \
	"Phase 2/2: fresh-allocation PA[$color_bit1:$color_bit0] 4x4 color matrix (up to $matrix_allocation_retries allocations)." >&2

case "$matrix_allocation_retries" in
''|*[!0-9]*|0) echo "matrix_allocation_retries must be a positive integer" >&2; exit 2 ;;
esac
rm -f "$matrix_csv"
matrix_attempt=1
matrix_completed=0
while [ "$matrix_attempt" -le "$matrix_allocation_retries" ]; do
	matrix_attempt_id="${matrix_id}_allocation${matrix_attempt}"
	printf 'Matrix allocation attempt %s/%s.\n' \
		"$matrix_attempt" "$matrix_allocation_retries" >&2
	if sh "$script_dir/run_bitscan.sh" \
		scan_order=4 \
		experiment_run_id="$matrix_attempt_id" \
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
		color_bit0="$color_bit0" \
		color_bit1="$color_bit1" \
		color_repetitions="$measurement_repetitions" \
		bit_first="$color_bit0" \
		bit_last="$color_bit1" \
		bit_repetitions="$measurement_repetitions" \
		fill_rounds="$fill_rounds" \
		trigger_threshold="$trigger_threshold" \
		stable_percent="$stable_percent" \
		result_path="$matrix_csv"; then
		if grep -q "^# addrmask,$addrmask$" "$matrix_csv" && \
		   grep -q "^# candidate_target_masked,$target_low$" "$matrix_csv"; then
			matrix_completed=1
			break
		fi
		echo "Matrix CSV failed candidate-constraint validation." >&2
	fi
	printf 'Matrix allocation %s was unsuitable; retrying with a newly allocated owned-page pool.\n' \
		"$matrix_attempt" >&2
	matrix_attempt=$((matrix_attempt + 1))
done
if [ "$matrix_completed" -ne 1 ]; then
	echo "No matrix allocation reached $target_candidate_items candidates after $matrix_allocation_retries attempts." >&2
	exit 1
fi

manifest_row_format='%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s\n'
{
	printf '%s\n' 'run_id,stimulus_cpu,probe_cpu,allocation,kind,pool_pages,candidate_items,target_candidate_items,search_groups,search_repetitions,baseline_repetitions,reduction_repetitions,reduction_validation_repetitions,measurement_repetitions,fill_rounds,scan_passes,trigger_threshold,stable_percent,item_sweep_first,item_sweep_last,bit_first,bit_last,attribution_cross_only_bits,color_bit0,color_bit1,joint_rescue_mask,result_path,matrix_allocation_attempt'
	printf "$manifest_row_format" "$run_id" "$stimulus_cpu" "$probe_cpu" 1 attribution "$pool_pages" "$candidate_items" "$target_candidate_items" "$search_groups" "$search_repetitions" "$baseline_repetitions" "$reduction_repetitions" "$reduction_validation_repetitions" "$measurement_repetitions" "$fill_rounds" "$scan_passes" "$trigger_threshold" "$stable_percent" "$item_sweep_first" "$item_sweep_last" "$bit_first" "$bit_last" "$attribution_cross_only_bits" "$color_bit0" "$color_bit1" "$joint_rescue_mask" "$attribution_csv" ''
	printf "$manifest_row_format" "$run_id" "$stimulus_cpu" "$probe_cpu" 2 color_matrix "$pool_pages" "$candidate_items" "$target_candidate_items" "$search_groups" "$search_repetitions" "$baseline_repetitions" "$reduction_repetitions" "$reduction_validation_repetitions" "$measurement_repetitions" "$fill_rounds" "$scan_passes" "$trigger_threshold" "$stable_percent" '' '' '' '' '' "$color_bit0" "$color_bit1" '' "$matrix_csv" "$matrix_attempt"
} > "$manifest"

{
	printf 'addrmask,%s\n' "$addrmask"
	printf 'candidate_target_masked,%s\n' "$target_low"
	printf 'attribution_allocation_attempt,%s\n' "$attribution_attempt"
	printf 'matrix_allocation_attempt,%s\n' "$matrix_attempt"
	printf 'collected_utc,'
	date -u '+%Y-%m-%dT%H:%M:%SZ'
	printf 'uname,'
	uname -a
	printf 'kernel_cmdline,'
	tr '\n' ' ' < /proc/cmdline
	printf '\n'
	if [ -n "${MODULE_PATH:-}" ] && [ -f "$MODULE_PATH" ]; then
		if command -v sha256sum >/dev/null 2>&1; then
			sha256sum "$MODULE_PATH"
		fi
		if command -v modinfo >/dev/null 2>&1; then
			modinfo "$MODULE_PATH"
		fi
	fi
} > "$environment"

printf '%s\n' \
	"Replication completed." \
	"  attribution: $attribution_csv" \
	"  color matrix: $matrix_csv" \
	"  manifest: $manifest" \
	"  environment: $environment" >&2
