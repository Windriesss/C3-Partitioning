#!/bin/sh
set -eu

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)

exec sh "$script_dir/run_bitscan.sh" \
	scan_order=6 \
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
	item_sweep_first=13 \
	item_sweep_last=20 \
	item_sweep_repetitions=1000 \
	item_sweep_rounds=1000 \
	scan_passes=3 \
	bit_first=16 \
	bit_last=17 \
	bit_repetitions=1000 \
	attribution_control_repetitions=100 \
	latency_bin1=90 \
	latency_bin2=130 \
	latency_bin3=220 \
	single_max_percent=20 \
	fill_rounds=1000 \
	trigger_threshold=150 \
	stable_percent=95 \
	experiment_run_id=run0_cpu6_to_cpu7_attribution \
	result_path=/tmp/rk3588_sf_attribution_v15.csv \
	"$@"
