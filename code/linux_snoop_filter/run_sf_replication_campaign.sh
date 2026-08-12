#!/bin/sh
set -eu

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
run_count=${RUN_COUNT:-5}
start_run=${START_RUN:-1}
cpu_pairs=${CPU_PAIRS:-6:7}
output_dir=${OUTPUT_DIR:-/tmp/rk3588_sf_replication}
matrix_allocation_retries=${MATRIX_ALLOCATION_RETRIES:-10}
attribution_allocation_retries=${ATTRIBUTION_ALLOCATION_RETRIES:-10}
color_bit0=${COLOR_BIT0:-16}
color_bit1=${COLOR_BIT1:-17}
joint_rescue_mask=${JOINT_RESCUE_MASK:-}
addrmask=${ADDRMASK:-0x1ffffff}
target_low=${TARGET_LOW:-0x138e900}

for argument in "$@"; do
	case "$argument" in
	run_count=*) run_count=${argument#*=} ;;
	start_run=*) start_run=${argument#*=} ;;
	cpu_pairs=*) cpu_pairs=${argument#*=} ;;
	output_dir=*) output_dir=${argument#*=} ;;
	matrix_allocation_retries=*) matrix_allocation_retries=${argument#*=} ;;
	attribution_allocation_retries=*) attribution_allocation_retries=${argument#*=} ;;
	color_bit0=*) color_bit0=${argument#*=} ;;
	color_bit1=*) color_bit1=${argument#*=} ;;
	joint_rescue_mask=*) joint_rescue_mask=${argument#*=} ;;
	addrmask=*) addrmask=${argument#*=} ;;
	target_low=*) target_low=${argument#*=} ;;
	*) echo "Unknown campaign option: $argument" >&2; exit 2 ;;
	esac
done

case "$start_run:$run_count" in
*[!0-9:]*|0:*|*:0) echo "start_run and run_count must be positive integers" >&2; exit 2 ;;
esac
if [ "$start_run" -gt "$run_count" ]; then
	echo "start_run must not exceed run_count" >&2
	exit 2
fi

mkdir -p "$output_dir"
old_ifs=$IFS
IFS=,
for pair in $cpu_pairs; do
	IFS=$old_ifs
	stimulus_cpu=${pair%%:*}
	probe_cpu=${pair#*:}
	if [ "$stimulus_cpu" = "$pair" ] || [ -z "$probe_cpu" ]; then
		echo "Invalid cpu pair '$pair'; expected stimulus:probe" >&2
		exit 2
	fi
	run=$start_run
	while [ "$run" -le "$run_count" ]; do
		run_id=$(printf 'run%02d' "$run")
		sh "$script_dir/run_sf_replication_once.sh" \
			run_id="$run_id" \
			output_dir="$output_dir" \
			stimulus_cpu="$stimulus_cpu" \
			probe_cpu="$probe_cpu" \
			matrix_allocation_retries="$matrix_allocation_retries" \
			attribution_allocation_retries="$attribution_allocation_retries" \
			color_bit0="$color_bit0" \
			color_bit1="$color_bit1" \
			joint_rescue_mask="$joint_rescue_mask" \
			addrmask="$addrmask" \
			target_low="$target_low"
		run=$((run + 1))
	done
	IFS=,
done
IFS=$old_ifs

printf '%s\n' \
	"Campaign completed in $output_dir." \
	"For strongest independence, collect one run per cold boot instead of executing all runs in one boot." >&2
