#!/bin/sh

set -u

module_name="rk3588_sf_crossprobe"
module_path=${MODULE_PATH:-"./${module_name}.ko"}
stress_ng=${STRESS_NG:-"./stress-ng"}
result_dir=${RESULT_DIR:-"/tmp/sf_evidence"}

# Focused wait/pressure screen. Override any list through the environment.
match_bits_values=${MATCH_BITS_VALUES:-"25"}
access_items_values=${ACCESS_ITEMS_VALUES:-"32"}
prefill_wait_us_values=${PREFILL_WAIT_US_VALUES:-"0 1 5 10 50 100"}
conditions=${CONDITIONS:-"none five8m_cpu0_4 six8m_cpu0_5"}
repetitions=${REPETITIONS:-3}
sample_sets=${SAMPLE_SETS:-300}
pool_pages=${POOL_PAGES:-393216}
fill_rounds=${FILL_ROUNDS:-10}
probe_cpu=${PROBE_CPU:-7}
stimulus_cpus=${STIMULUS_CPUS:-6}
pressure_settle_seconds=${PRESSURE_SETTLE_SECONDS:-3}

pressure_pids=""

if [ "$(id -u)" -ne 0 ]; then
	echo "Run this script as root." >&2
	exit 1
fi

if [ ! -f "$module_path" ]; then
	echo "Cannot find module: $module_path" >&2
	exit 1
fi

if [ ! -x "$stress_ng" ]; then
	echo "Cannot execute stress-ng: $stress_ng" >&2
	exit 1
fi

mkdir -p "$result_dir"

module_loaded()
{
	grep -q "^${module_name} " /proc/modules 2>/dev/null
}

unload_module()
{
	if module_loaded; then
		rmmod "$module_name"
	fi
}

stop_pressure()
{
	for pid in $pressure_pids; do
		kill "$pid" 2>/dev/null || true
	done
	for pid in $pressure_pids; do
		wait "$pid" 2>/dev/null || true
	done
	pressure_pids=""
}

start_vm_worker()
{
	cpu=$1
	bytes=$2
	log_path=$3

	taskset -c "$cpu" "$stress_ng" \
		--vm 1 --vm-bytes "$bytes" --vm-keep \
		--metrics-brief --timeout 1d >>"$log_path" 2>&1 &
	pressure_pids="$pressure_pids $!"
}

start_pressure()
{
	condition=$1
	log_path=$2

	pressure_pids=""
	: >"$log_path"
	case "$condition" in
	none)
		return 0
		;;
	small_cpu4)
		start_vm_worker 4 2M "$log_path"
		;;
	large_cpu4)
		start_vm_worker 4 512M "$log_path"
		;;
	large_cpu0)
		start_vm_worker 0 512M "$log_path"
		;;
	five8m_cpu0_4)
		for cpu in 0 1 2 3 4; do
			start_vm_worker "$cpu" 8M "$log_path"
		done
		;;
	six8m_cpu0_5)
		for cpu in 0 1 2 3 4 5; do
			start_vm_worker "$cpu" 8M "$log_path"
		done
		;;
	*)
		echo "Unknown pressure condition: $condition" >&2
		return 1
		;;
	esac
	sleep "$pressure_settle_seconds"
	for pid in $pressure_pids; do
		if ! kill -0 "$pid" 2>/dev/null; then
			echo "Pressure process exited early: $condition pid=$pid" >&2
			stop_pressure
			return 1
		fi
	done
}

cleanup()
{
	unload_module
	stop_pressure
}

trap cleanup EXIT HUP INT TERM

{
	echo "date=$(date -Iseconds 2>/dev/null || date)"
	echo "uname=$(uname -a)"
	echo "module_path=$module_path"
	echo "stress_ng=$stress_ng"
	echo "match_bits_values=$match_bits_values"
	echo "access_items_values=$access_items_values"
	echo "prefill_wait_us_values=$prefill_wait_us_values"
	echo "conditions=$conditions"
	echo "repetitions=$repetitions"
	echo "sample_sets=$sample_sets"
	echo "pool_pages=$pool_pages"
	echo "probe_cpu=$probe_cpu"
	echo "stimulus_cpus=$stimulus_cpus"
	echo "sched_rt_period_us=$(cat /proc/sys/kernel/sched_rt_period_us)"
	echo "sched_rt_runtime_us=$(cat /proc/sys/kernel/sched_rt_runtime_us)"
} >"$result_dir/run_metadata.txt"

for cpu_dir in /sys/devices/system/cpu/cpu[0-9]*; do
	cpu_name=${cpu_dir##*/}
	{
		echo "[$cpu_name]"
		for field in cluster_id core_id core_siblings_list thread_siblings_list; do
			path="$cpu_dir/topology/$field"
			if [ -r "$path" ]; then
				echo "$field=$(cat "$path")"
			fi
		done
		for cache_dir in "$cpu_dir"/cache/index*; do
			if [ -d "$cache_dir" ]; then
				echo "cache=$(cat "$cache_dir/level")/$(cat "$cache_dir/type")/$(cat "$cache_dir/size")/shared=$(cat "$cache_dir/shared_cpu_list")"
			fi
		done
	} >>"$result_dir/cpu_topology.txt"
done

total=0
passed=0
failed=0

rep=1
while [ "$rep" -le "$repetitions" ]; do
	for match_bits in $match_bits_values; do
		for access_items in $access_items_values; do
			for prefill_wait_us in $prefill_wait_us_values; do
				for condition in $conditions; do
				total=$((total + 1))
				stem="sf_${condition}_mb${match_bits}_ai${access_items}_wu${prefill_wait_us}_r${rep}"
				csv_path="$result_dir/${stem}.csv"
				stress_log="$result_dir/${stem}.stress.log"
				kernel_log="$result_dir/${stem}.kernel.log"

				echo "[$total] condition=$condition match_bits=$match_bits access_items=$access_items prefill_wait_us=$prefill_wait_us repetition=$rep"
				unload_module
				stop_pressure

				if ! start_pressure "$condition" "$stress_log"; then
					failed=$((failed + 1))
					echo "$stem,pressure_start_failed" >>"$result_dir/failures.csv"
					continue
				fi

				if insmod "$module_path" \
					pool_pages="$pool_pages" \
					match_bits="$match_bits" \
					access_items="$access_items" \
					fill_rounds="$fill_rounds" \
					prefill_wait_us="$prefill_wait_us" \
					probe_cpu="$probe_cpu" \
					stimulus_cpus="$stimulus_cpus" \
					baseline_sets="$sample_sets" \
					candidate_sets="$sample_sets" \
					result_path="$csv_path"
				then
					passed=$((passed + 1))
					echo "  OK: $csv_path"
				else
					status=$?
					failed=$((failed + 1))
					echo "$stem,insmod_exit_$status" >>"$result_dir/failures.csv"
					echo "  FAILED: insmod exit $status" >&2
				fi

				dmesg | tail -n 80 >"$kernel_log"
				if grep -q "RT throttling activated" "$kernel_log"; then
					echo "$stem,rt_throttling_seen_in_kernel_log" \
						>>"$result_dir/warnings.csv"
					echo "  WARNING: RT throttling appears in kernel log" >&2
				fi
				unload_module
				stop_pressure
				sleep 1
				done
			done
		done
	done
	rep=$((rep + 1))
done

echo "Sweep complete: total=$total passed=$passed failed=$failed"
echo "Results: $result_dir"
