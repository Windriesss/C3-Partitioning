#!/bin/sh
set -eu

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)

# Campaign-level parameters used by the five PA[22:0]-matched RK3568
# allocations in data/raw/five_allocations.
export MODULE_PATH=${MODULE_PATH:-/root/rk3588_sf_bitscan.ko}
export POOL_PAGES=${POOL_PAGES:-329376}
export CANDIDATE_ITEMS=${CANDIDATE_ITEMS:-20}
export TARGET_CANDIDATE_ITEMS=${TARGET_CANDIDATE_ITEMS:-16}
export SEARCH_GROUPS=${SEARCH_GROUPS:-2000}
export SEARCH_REPETITIONS=${SEARCH_REPETITIONS:-20}
export BASELINE_REPETITIONS=${BASELINE_REPETITIONS:-1000}
export REDUCTION_REPETITIONS=${REDUCTION_REPETITIONS:-20}
export REDUCTION_VALIDATION_REPETITIONS=${REDUCTION_VALIDATION_REPETITIONS:-200}
export MEASUREMENT_REPETITIONS=${MEASUREMENT_REPETITIONS:-1000}
export FILL_ROUNDS=${FILL_ROUNDS:-1000}
export SCAN_PASSES=${SCAN_PASSES:-3}
export BIT_FIRST=${BIT_FIRST:-0}
export BIT_LAST=${BIT_LAST:-22}
export ATTRIBUTION_CROSS_ONLY_BITS=${ATTRIBUTION_CROSS_ONLY_BITS:-1}
export ATTRIBUTION_ALLOCATION_RETRIES=${ATTRIBUTION_ALLOCATION_RETRIES:-10}
export MATRIX_ALLOCATION_RETRIES=${MATRIX_ALLOCATION_RETRIES:-10}
export ADDRMASK=${ADDRMASK:-0x7fffff}
export TARGET_LOW=${TARGET_LOW:-0x38e900}
export COLOR_BIT0=${COLOR_BIT0:-13}
export COLOR_BIT1=${COLOR_BIT1:-14}
export JOINT_RESCUE_MASK=${JOINT_RESCUE_MASK:-0x6000}

exec sh "$script_dir/run_sf_replication_campaign.sh" \
	run_count="${RUN_COUNT:-5}" \
	start_run="${START_RUN:-1}" \
	cpu_pairs="${CPU_PAIRS:-2:3}" \
	output_dir="${OUTPUT_DIR:-/root/sf-rk3568-low23-threefig-full}" \
	attribution_allocation_retries="$ATTRIBUTION_ALLOCATION_RETRIES" \
	matrix_allocation_retries="$MATRIX_ALLOCATION_RETRIES" \
	color_bit0="$COLOR_BIT0" \
	color_bit1="$COLOR_BIT1" \
	joint_rescue_mask="$JOINT_RESCUE_MASK" \
	addrmask="$ADDRMASK" \
	target_low="$TARGET_LOW"
