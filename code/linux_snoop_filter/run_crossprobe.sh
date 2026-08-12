#!/bin/sh
set -eu

module_name="rk3588_sf_crossprobe"
script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
module_path=${MODULE_PATH:-"$script_dir/kernel/$module_name.ko"}

if [ ! -f "$module_path" ] && [ -f "$script_dir/$module_name.ko" ]; then
	module_path="$script_dir/$module_name.ko"
fi

if [ ! -f "$module_path" ]; then
	echo "Cannot find $module_name.ko" >&2
	echo "Set MODULE_PATH=/absolute/path/$module_name.ko if needed." >&2
	exit 1
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

insmod "$module_path" "$@"
