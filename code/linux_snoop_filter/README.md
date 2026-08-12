# C3-Partitioning Bruteforce Experiment Source

Author: HuaijiGao

This directory contains the rebuildable Linux kernel modules, board-side
launch scripts, and offline analysis programs used to investigate RK3588 and
RK3568 snoop-filter interference. Generated kernel objects, archives, caches,
and duplicate result bundles are intentionally excluded.

## Source-code scope

This is an experiment-source package, not a complete Xen-UniProton system or a
standalone board image. Kernel-module compilation requires the exact target
Linux kernel build tree and Arm64 cross-toolchain. End-to-end execution also
requires the original Xen/UniProton configuration, board deployment, privileged
module-loading environment, and workload binaries, which are not included.

The canonical data used by the paper's SF figures are stored separately at:

- `../../data/snoop_filter/rk3588/pa17_16_five_allocations/`
- `../../data/snoop_filter/rk3568/low23_threefig_five_allocations/`

## Build

Build against the exact kernel tree and configuration running on the target
board:

```bash
make KDIR=/path/to/kernel/build \
     ARCH=arm64 \
     CROSS_COMPILE=aarch64-linux-gnu-
```

The default `CROSS_COMPILE` value in the Makefile can be overridden. Kernel
modules must not be reused across incompatible kernel builds.

## Kernel modules

- `rk3588_sf_bruteforce.c`: deterministic two-stage candidate search and
  cross-core validation.
- `rk3588_sf_bitscan.c`: owned-page item sweeps, probe-bit scans, joint flips,
  color matrices, and PMU attribution.
- `rk3588_sf_crossprobe.c`: controlled same/cross-core probing, deterministic
  filler pressure, and mechanism experiments.
- `rk3588_sf_ablation.c`: fixed-candidate replacement/removal ablation.
- `rk3588_sf_probe.c`: same-core prime/fill/probe comparison.
- `rk3588_sf_sampler.c`: low-address-matched timing distribution sampler.

All modules operate only on pages allocated and retained by the module. A
computed physical address is used only as a lookup key into that owned pool;
an unavailable exact page is recorded and skipped. The code does not expose an
interface for dereferencing arbitrary physical addresses.

## Board-side entry points

- `run_bitscan.sh`: general driver for `rk3588_sf_bitscan.ko`; accepts module
  parameters as `name=value` arguments.
- `run_bit_item_sweep.sh`: bit-by-item sweep used for the 25-panel RK3568
  supplementary diagnostic.
- `run_joint_bitscan.sh`: original, probe-only-flip, and whole-group joint-flip
  scan (`scan_order=5`).
- `run_sf_attribution.sh`: same/cross-core item sweep and PMU attribution
  (`scan_order=6`).
- `run_rk3568_low23_campaign.sh`: five-allocation RK3568 PA[22:0]-matched item
  sweep.
- `run_sf_replication_once.sh`: one fresh-allocation attribution run plus a
  separately allocated color-matrix run.
- `run_sf_replication_campaign.sh`: repeated replication campaign with run IDs,
  manifests, environment records, and allocation retries.
- `run_crossprobe.sh` and `run_crossprobe_sweep.sh`: controlled cross-probe
  measurements and parameter sweeps.
- `run_sf_evidence.sh`: background-pressure evidence sweep.
- `run_sf_mechanism.sh`: deterministic filler-based mechanism experiment.
- `run_ablation.sh`: fixed-candidate ablation driver.

Most scripts require root privileges to load modules and configure PMU-backed
measurements. Paths such as `/tmp`, module locations, CPU IDs, and pool sizes
are board parameters, not plotting-data indices. Override their documented
environment variables for a different machine. Example:

```sh
MODULE_PATH=/root/rk3588_sf_bitscan.ko \
OUTPUT_DIR=/root/sf-campaign \
RUN_COUNT=5 \
sh run_sf_replication_campaign.sh
```

Run a short smoke test before a long campaign and inspect `dmesg`, the CSV
metadata, and the environment manifest after every module load.

Expected board-side output consists of experiment CSV files, run manifests,
environment records, and kernel messages. Output locations are controlled by
`OUTPUT_DIR` or `RESULT_DIR` in the launch scripts. Offline analysis programs
write summary CSV and SVG/PDF/PNG files according to their command-line
arguments; use `python <script>.py --help` for exact filenames.

## Analysis programs

- `analyze_joint_bitscan.py`: summarize original, probe-only, and joint flips.
- `analyze_sf_attribution.py`: summarize item-sweep and PMU attribution output.
- `analyze_sf_evidence.py` and `analyze_sf_mechanism.py`: summarize controlled
  pressure and deterministic-filler experiments.
- `analyze_sf_replications.py`: aggregate independent allocations.
- `analyze_sf_motivation.py`, `audit_received_v16.py`, and
  `validate_sf_motivation_v16.py`: validate and summarize complete campaigns.
- `plot_*`: distribution, attribution, matrix, and paper-figure plotters.
- `svg_to_vector_pdf.py`: convert the project's simple SVG figures to vector
  PDF without rasterization.

Use `python <script>.py --help` for the accepted input and output arguments.

## Interpretation limits

A physical-address flip that changes cross-core behavior is consistent with a
selector, hash, slice, or home-node input, but does not uniquely recover the
hardware hash function. Likewise, the minimum stable stimulus set observed by
a protocol is not by itself proof of cache or directory associativity. Report
fresh allocations as independent repetitions and keep unavailable owned-page
lookups missing rather than treating them as zero-valued measurements.
