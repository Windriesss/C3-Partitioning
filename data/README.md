# Dataset Guide

Author: HuaijiGao

This directory contains all measurements used by the plotting programs. Paths
below are relative to the package root.

## Dataset composition and platforms

| Path | Platform and contents |
|---|---|
| `data/response_time/main/rk3588/` | RK3588 1,000 Hz response-time, PMU, ablation, YOLOv8n, and FFT measurements |
| `data/response_time/main/rk3568/` | RK3568 1,000 Hz response-time measurements |
| `data/response_time/low_rate/{rk3588,rk3568}/` | 100 Hz and 10 Hz response-time measurements used by the appendix |
| `data/snoop_filter/rk3588/pa17_16_five_allocations/` | RK3588 five-allocation item, bit, and color-matrix campaign |
| `data/snoop_filter/rk3568/low23_threefig_five_allocations/` | RK3568 five-allocation item, bit, and color-matrix campaign |
| `data/rk3568_attribution/raw/five_allocations/` | RK3568 PA[22:0]-matched latency and PMU attribution campaign |
| `data/rk3568_attribution/processed/` | Plot-ready attribution summary generated from the raw campaign |
| `data/supplementary/rk3568_bit_item/` | One-allocation RK3568 PA[0:24] bit-by-item diagnostic |

The snoop-filter and attribution environment files record Linux 5.10.0+ SMP
PREEMPT_RT on AArch64. RK3588 uses stimulus CPU6 and probe CPU7; RK3568 uses
stimulus CPU2 and probe CPU3. The response-time summary JSON files identify the
platform and frequency but do not consistently record exact Xen, UniProton,
Linux, compiler, or stress-ng versions. The supplementary CSV records RK3568,
stimulus CPU2, probe CPU3, and Arm PMCCNTR cycles, but no complete software
environment. No missing version is inferred.

## Response-time files and fields

Response-time directories contain `*_summary.json`, `*_rtos.log`, and
`*_oee.log`. Summary JSON files store relative log filenames, run status,
platform, experiment, workload, working set, frequency, duration, and expected
and observed sample counts. Plotters reject incomplete records according to
their embedded validity checks.

| Field | Meaning and unit |
|---|---|
| `vm_bytes` / `vm_<size>` | GPOS pressure working-set size, bytes with K/M/G suffixes |
| `sample_hz` | RTOS activation or sampling frequency, hertz |
| `elapsed_seconds` | Run wall-clock duration, seconds |
| `expected_pmu_samples`, `minimum_pmu_samples`, `last_pmu_samples` | PMU sample counts |
| `t1_max`, `t2_max` | Maximum controller timing values, microseconds |
| `t2_jitter` | Peak-to-peak controller response-time variation, microseconds |
| `avg_ll_miss_rd` | Average LL-cache read misses per measured activation |
| `avg_l2_refill` | Average L2D-cache refills per measured activation |
| `avg_l2_inval` | Average L2D-cache invalidations per measured activation |
| `bogo_ops` / `oee_bogo_ops` | stress-ng bogo-operation count |
| `rtos_copy_bogo_ops` | RTOS memory-pressure copy-round count |
| `status` | `complete`, `failed`, or `rerun_consumed` |

Histogram bin centers are response time in microseconds; bin values are integer
counts. Main response-time runs use 1,000 Hz. Appendix response-time runs use
100 Hz or 10 Hz and nominal 600 s measurement windows.

## Snoop-filter and attribution fields

Raw CSV files begin with `# key,value` metadata followed by the CSV header.

| Field | Meaning and unit |
|---|---|
| `record` | Measurement family: item sweep, bit scan, or color matrix |
| `variant` | Same/cross-core and original/flipped or candidate/baseline condition |
| `active_candidates` | Number of stimulus addresses X; excludes the probe |
| `test_bit` | Tested physical-address bit |
| `stimulus_color`, `probe_color` | Two-bit color identifiers for matrix cells |
| `mean_ticks` | Mean final-probe latency in PMCCNTR cycles |
| `successes`, `repetitions` | Threshold-event count and represented trials |
| `pass` | Randomized scan-pass index |
| `available`, `lookup_status` | Exact owned-page availability and reason |

Only pages allocated and retained by the module are accessed. Unavailable
exact owned-page targets remain missing and are never replaced with zero.

## Statistical methods

- FFT motivation: arithmetic mean and sample standard deviation (`ddof=1`) of
  matching 1,536 KiB observations.
- YOLOv8n timing/PMU motivation: values from the last complete RTOS block in
  each accepted run.
- Main 1,000 Hz Jresp: run-level median of `t2_jitter`; 20,000-resample
  percentile bootstrap for the median with seed 20260729 and 2.5/97.5
  percentiles; intervals appear only when `n >= 5`.
- 16 MiB response-time CCDF: complete-run histograms pooled by platform,
  interference scenario, and method; reverse cumulative counts are normalized.
- RK3588 PMU: median across accepted complete runs at each condition and
  working-set point.
- Main RK3588 ablation: median, minimum, and maximum across accepted complete
  runs for each cell.
- Appendix 100/10 Hz Jresp and ablation: one selected complete run per plotted
  cell. No confidence interval is claimed; processed files explicitly record
  the run count and unavailable interval.
- Main snoop-filter and RK3568 attribution campaigns: allocation is the
  statistical unit; curves report the median with allocation minimum--maximum
  envelope. Bit-scan passes are averaged or repetition-weighted within an
  allocation before allocations are summarized. Matrix success percentages
  pool `successes` and `repetitions`.
- Supplementary 25-panel diagnostic: median of five pass-level `mean_ticks`
  values with pass minimum--maximum envelope. It is a single-allocation
  diagnostic, not an allocation-level confidence interval.

Attribution PMU incidence is
`100 * probe_<event>_samples / repetitions`; event-rate fields ending in
`_events_per_window` are event totals divided by repetitions. Latency remains
in PMCCNTR cycles and incidence is a percentage.

See `../DATA_INDEX.csv` for the exact data used by every figure and table and
`../scripts/README.md` for commands and expected outputs.
