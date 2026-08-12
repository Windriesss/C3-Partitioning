#!/bin/sh
set -eu

module_name="rk3588_sf_ablation"
script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
module_path=${MODULE_PATH:-}
caller_home=""

if [ -z "$module_path" ] && [ -n "${SUDO_USER:-}" ] && \
   [ "$SUDO_USER" != "root" ] && command -v getent >/dev/null 2>&1; then
	caller_home=$(getent passwd "$SUDO_USER" | cut -d: -f6)
fi

# Quoted "~/..." does not expand in POSIX sh.  Resolve home directories
# explicitly, including the original login user's home after sudo.
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
	actual_description=$(modinfo -F description "$module_path" 2>/dev/null || true)
	actual_abi=$(modinfo -F sfab_abi "$module_path" 2>/dev/null || true)
	actual_parameters=$(modinfo -F parm "$module_path" 2>/dev/null || true)
	missing_parameters=""
	for required_parameter in trial_repetitions validation_repetitions \
		min_success_percent trigger_threshold min_candidate_items \
		use_replacements manifest_mask initial_active_mask fill_rounds_sweep; do
		if ! printf '%s\n' "$actual_parameters" |
		     grep -q "^${required_parameter}:"; then
			missing_parameters="$missing_parameters $required_parameter"
		fi
	done
	if [ "$actual_name" != "$module_name" ] ||
	   [ "$actual_abi" != "fixed-rank-ablation-v7" ] ||
	   [ -n "$missing_parameters" ]; then
		echo "Refusing unexpected module binary: $module_path" >&2
		echo "  name: ${actual_name:-unknown}" >&2
		echo "  description: ${actual_description:-unknown}" >&2
		echo "  sfab_abi: ${actual_abi:-missing}" >&2
		echo "  missing ablation parameters:${missing_parameters:- none}" >&2
		echo "Expected rk3588_sf_ablation.ko with the replacement-ablation parameter ABI." >&2
		exit 1
	fi
	if ! printf '%s\n' "$actual_description" |
	     grep -q "fixed-manifest address-set ablation"; then
		echo "Warning: module description is from an older source revision; parameter ABI passed." >&2
	fi
else
	echo "Warning: modinfo is unavailable; module identity was not preflight-checked." >&2
fi

cleanup()
{
	if grep -q "^$module_name " /proc/modules 2>/dev/null; then
		rmmod "$module_name"
	fi
}

trap cleanup EXIT HUP INT TERM

if grep -q "^$module_name " /proc/modules 2>/dev/null; then
	echo "$module_name is already loaded; unloading the stale instance." >&2
	rmmod "$module_name"
fi

echo "Starting the fixed-manifest address-set ablation; insmod returns after the experiment." >&2
insmod "$module_path" "$@"
