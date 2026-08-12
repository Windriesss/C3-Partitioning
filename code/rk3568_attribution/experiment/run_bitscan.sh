#!/bin/sh
set -eu

module_name="rk3588_sf_bitscan"
script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
module_path=${MODULE_PATH:-}
caller_home=""

if [ -z "$module_path" ] && [ -n "${SUDO_USER:-}" ] && \
   [ "$SUDO_USER" != "root" ] && command -v getent >/dev/null 2>&1; then
	caller_home=$(getent passwd "$SUDO_USER" | cut -d: -f6)
fi
if [ -z "$module_path" ] && [ -n "$caller_home" ] && \
   [ -f "$caller_home/$module_name.ko" ]; then
	module_path="$caller_home/$module_name.ko"
elif [ -z "$module_path" ] && [ -n "${HOME:-}" ] && \
     [ -f "$HOME/$module_name.ko" ]; then
	module_path="$HOME/$module_name.ko"
elif [ -z "$module_path" ] && [ -f "$script_dir/kernel/$module_name.ko" ]; then
	module_path="$script_dir/kernel/$module_name.ko"
elif [ -z "$module_path" ] && [ -f "$script_dir/$module_name.ko" ]; then
	module_path="$script_dir/$module_name.ko"
fi
if [ ! -f "$module_path" ]; then
	echo "Cannot find $module_name.ko" >&2
	echo "Set MODULE_PATH=/absolute/path/$module_name.ko if needed." >&2
	exit 1
fi

echo "Using module: $module_path" >&2
if command -v modinfo >/dev/null 2>&1; then
	actual_name=$(modinfo -F name "$module_path" 2>/dev/null || true)
	actual_abi=$(modinfo -F sfbs_abi "$module_path" 2>/dev/null || true)
	actual_parameters=$(modinfo -F parm "$module_path" 2>/dev/null || true)
	missing_parameters=""
	for required_parameter in bit_first bit_last bit_repetitions \
		scan_order pair_screen_repetitions pair_screen_percent \
		single_max_percent triple_screen_repetitions \
		triple_screen_percent pair_max_percent \
		trigger_threshold stable_percent stimulus_mask pool_pages \
		probe_cpu stimulus_cpus fill_rounds result_path auto_find_stable \
		candidate_items search_groups search_repetitions \
		baseline_repetitions target_candidate_items \
		reduction_repetitions reduction_validation_repetitions \
		scan_passes color_bit0 color_bit1 color_repetitions \
		item_sweep_first item_sweep_last item_sweep_repetitions \
		item_sweep_rounds experiment_run_id \
		attribution_control_repetitions latency_bin1 latency_bin2 \
		latency_bin3 attribution_cross_only_bits joint_rescue_mask; do
		if ! printf '%s\n' "$actual_parameters" |
		     grep -q "^${required_parameter}:"; then
			missing_parameters="$missing_parameters $required_parameter"
		fi
	done
	if [ "$actual_name" != "$module_name" ] ||
	   [ "$actual_abi" != "owned-probe-bitscan-v20" ] ||
	   [ -n "$missing_parameters" ]; then
		echo "Refusing unexpected module binary: $module_path" >&2
		echo "  name: ${actual_name:-unknown}" >&2
		echo "  sfbs_abi: ${actual_abi:-missing}" >&2
		echo "  missing bit-scan parameters:${missing_parameters:- none}" >&2
		exit 1
	fi
else
	echo "Warning: modinfo is unavailable; module identity was not checked." >&2
fi

cleanup()
{
	if grep -q "^$module_name " /proc/modules 2>/dev/null; then
		rmmod "$module_name"
	fi
}
trap cleanup EXIT HUP INT TERM

if grep -q "^$module_name " /proc/modules 2>/dev/null; then
	echo "$module_name is already loaded; unloading it first." >&2
	rmmod "$module_name"
fi

echo "Starting owned-page probe/SF-selection scan; insmod waits for completion." >&2
insmod "$module_path" "$@"
