#!/bin/sh

set -u

module_name="rk3588_sf_crossprobe"
module_path=${MODULE_PATH:-"./${module_name}.ko"}
match_bits_values="13 14 15 16 17 18 19 20 21 22 23 24 25 26"
access_items_values="1 2 4 6 8 12 14 16 18 20 22 24 26 28 30 32"

if [ "$(id -u)" -ne 0 ]; then
	echo "Run this script as root." >&2
	exit 1
fi

if [ ! -f "$module_path" ]; then
	echo "Cannot find module: $module_path" >&2
	echo "Place ${module_name}.ko in the current directory or set MODULE_PATH." >&2
	exit 1
fi

unload_module()
{
	if grep -q "^${module_name} " /proc/modules 2>/dev/null; then
		rmmod "$module_name"
	fi
}

trap unload_module EXIT HUP INT TERM

total=0
passed=0
failed=0

for match_bits in $match_bits_values; do
	for access_items in $access_items_values; do
		total=$((total + 1))
		result_path="/tmp/rk3588_sf_crossprobe_${match_bits}_${access_items}.csv"

		unload_module
		echo "[$total/224] match_bits=$match_bits access_items=$access_items"

		if insmod "$module_path" \
			pool_pages=393216 \
			match_bits="$match_bits" \
			access_items="$access_items" \
			fill_rounds=10 \
			probe_cpu=7 \
			stimulus_cpus=6 \
			baseline_sets=1000 \
			candidate_sets=1000 \
			result_path="$result_path"
		then
			passed=$((passed + 1))
			echo "  OK: $result_path"
			unload_module
		else
			status=$?
			failed=$((failed + 1))
			echo "  FAILED (insmod exit $status); continuing." >&2
			unload_module
		fi
	done
done

echo "Sweep complete: total=$total passed=$passed failed=$failed"

