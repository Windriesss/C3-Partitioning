#!/bin/sh
set -eu

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)

exec sh "$script_dir/run_bitscan.sh" \
	scan_order=5 \
	pool_pages=655360 \
	probe_cpu=7 \
	stimulus_cpus=6 \
	auto_find_stable=1 \
	candidate_items=24 \
	target_candidate_items=16 \
	search_groups=2000 \
	search_repetitions=20 \
	baseline_repetitions=1000 \
	reduction_repetitions=20 \
	reduction_validation_repetitions=200 \
	scan_passes=3 \
	bit_first=6 \
	bit_last=30 \
	bit_repetitions=1000 \
	single_max_percent=20 \
	fill_rounds=1000 \
	trigger_threshold=150 \
	stable_percent=95 \
	result_path=/tmp/rk3588_sf_joint_bitscan_b6_30_v12.csv \
	"$@"
