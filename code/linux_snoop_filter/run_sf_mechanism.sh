#!/bin/sh
set -u

# Deterministic, address-auditable replacement for stress-ng pressure.
# The module exports every filler physical address as a filler_line CSV row.

MODULE_PATH=${MODULE_PATH:-./rk3588_sf_crossprobe.ko}
RESULT_DIR=${RESULT_DIR:-/tmp/sf_mechanism}
PHASE=${PHASE:-map}
POOL_PAGES=${POOL_PAGES:-393216}
MATCH_BITS=${MATCH_BITS:-25}
ACCESS_ITEMS=${ACCESS_ITEMS:-32}
FILL_ROUNDS=${FILL_ROUNDS:-10}
PREFILL_WAIT_US=${PREFILL_WAIT_US:-50}
PROBE_CPU=${PROBE_CPU:-7}
STIMULUS_CPUS=${STIMULUS_CPUS:-6}
FILLER_CPUS=${FILLER_CPUS:-0-5}
FILLER_ROUNDS=${FILLER_ROUNDS:-1}
SAMPLE_SETS=${SAMPLE_SETS:-500}
REPETITIONS=${REPETITIONS:-3}
TARGET_LOW_VALUES=${TARGET_LOW_VALUES:-0x1ae1fc0 0x103efc0 0x8cbfc0}
MAP_FILLER_BITS=${MAP_FILLER_BITS:-12 16 18 20 21 22 23 24}
MAP_FILLER_ITEMS=${MAP_FILLER_ITEMS:-32}
FOCUS_FILLER_BITS=${FOCUS_FILLER_BITS:-23}
OCCUPANCY_ITEMS=${OCCUPANCY_ITEMS:-0 4 8 16 24 32 48 64}
ACCESS_ITEM_VALUES=${ACCESS_ITEM_VALUES:-1 8 16 24 32}
ACCESS_FILLER_ITEMS=${ACCESS_FILLER_ITEMS:-32}

mkdir -p "$RESULT_DIR" || exit 1

unload_module()
{
	if grep -q '^rk3588_sf_crossprobe ' /proc/modules 2>/dev/null; then
		rmmod rk3588_sf_crossprobe
	fi
}

run_one()
{
	phase_name=$1
	target=$2
	filler_bits=$3
	filler_count=$4
	access_count=$5
	rep=$6
	target_name=$(printf '%s' "$target" | sed 's/^0x//')
	condition="${phase_name}_fb${filler_bits}_fi${filler_count}_t${target_name}"
	stem="sf_${condition}_mb${MATCH_BITS}_ai${access_count}_wu${PREFILL_WAIT_US}_r${rep}"
	csv_path="$RESULT_DIR/$stem.csv"
	kernel_log="$RESULT_DIR/$stem.kernel.log"

	echo "phase=$phase_name target=$target filler_bits=$filler_bits filler_items=$filler_count access_items=$access_count repetition=$rep"
	unload_module

	if [ "$filler_count" -eq 0 ]; then
		insmod "$MODULE_PATH" \
			pool_pages="$POOL_PAGES" \
			match_bits="$MATCH_BITS" \
			target_low="$target" \
			access_items="$access_count" \
			fill_rounds="$FILL_ROUNDS" \
			prefill_wait_us="$PREFILL_WAIT_US" \
			probe_cpu="$PROBE_CPU" \
			stimulus_cpus="$STIMULUS_CPUS" \
			baseline_sets="$SAMPLE_SETS" \
			candidate_sets="$SAMPLE_SETS" \
			result_path="$csv_path"
	else
		insmod "$MODULE_PATH" \
			pool_pages="$POOL_PAGES" \
			match_bits="$MATCH_BITS" \
			target_low="$target" \
			access_items="$access_count" \
			fill_rounds="$FILL_ROUNDS" \
			prefill_wait_us="$PREFILL_WAIT_US" \
			probe_cpu="$PROBE_CPU" \
			stimulus_cpus="$STIMULUS_CPUS" \
			filler_cpus="$FILLER_CPUS" \
			filler_items="$filler_count" \
			filler_match_bits="$filler_bits" \
			filler_rounds="$FILLER_ROUNDS" \
			baseline_sets="$SAMPLE_SETS" \
			candidate_sets="$SAMPLE_SETS" \
			result_path="$csv_path"
	fi
	status=$?
	dmesg | tail -n 100 >"$kernel_log"
	unload_module
	if [ "$status" -ne 0 ]; then
		echo "$stem,insmod_exit_$status" >>"$RESULT_DIR/failures.csv"
		return "$status"
	fi
	return 0
}

{
	echo "date=$(date -Iseconds)"
	echo "phase=$PHASE"
	echo "module=$MODULE_PATH"
	echo "match_bits=$MATCH_BITS"
	echo "probe_cpu=$PROBE_CPU"
	echo "stimulus_cpus=$STIMULUS_CPUS"
	echo "filler_cpus=$FILLER_CPUS"
	echo "targets=$TARGET_LOW_VALUES"
	echo "map_filler_bits=$MAP_FILLER_BITS"
	echo "occupancy_items=$OCCUPANCY_ITEMS"
	uname -a
} >"$RESULT_DIR/config.txt"

total=0
passed=0
rep=1
while [ "$rep" -le "$REPETITIONS" ]; do
	for target in $TARGET_LOW_VALUES; do
		if [ "$PHASE" = map ] || [ "$PHASE" = all ]; then
			run_one map "$target" 12 0 "$ACCESS_ITEMS" "$rep" &&
				passed=$((passed + 1))
			total=$((total + 1))
			for filler_bits in $MAP_FILLER_BITS; do
				run_one map "$target" "$filler_bits" \
					"$MAP_FILLER_ITEMS" "$ACCESS_ITEMS" "$rep" &&
					passed=$((passed + 1))
				total=$((total + 1))
			done
		fi

		if [ "$PHASE" = occupancy ] || [ "$PHASE" = all ]; then
			for filler_count in $OCCUPANCY_ITEMS; do
				run_one occupancy "$target" "$FOCUS_FILLER_BITS" \
					"$filler_count" "$ACCESS_ITEMS" "$rep" &&
					passed=$((passed + 1))
				total=$((total + 1))
			done
		fi

		if [ "$PHASE" = access ] || [ "$PHASE" = all ]; then
			for access_count in $ACCESS_ITEM_VALUES; do
				run_one access "$target" "$FOCUS_FILLER_BITS" \
					"$ACCESS_FILLER_ITEMS" "$access_count" "$rep" &&
					passed=$((passed + 1))
				total=$((total + 1))
			done
		fi
	done
	rep=$((rep + 1))
done

echo "total=$total passed=$passed failed=$((total - passed))" |
	tee "$RESULT_DIR/summary.txt"
[ "$total" -eq "$passed" ]
