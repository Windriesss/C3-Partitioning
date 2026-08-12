# Source Code Guide

Author: HuaijiGao

## Included components

| Path | Contents |
|---|---|
| `code/linux_snoop_filter/` | Linux kernel modules, board-side campaign scripts, and offline analysis programs for RK3588/RK3568 snoop-filter experiments |
| `code/rk3568_attribution/kernel/` | Owned-page kernel-module source and Makefile used by the RK3568 attribution campaign |
| `code/rk3568_attribution/experiment/` | Board-side campaign, allocation, retry, and manifest scripts |
| `code/rtos_workloads/fft/` | Deterministic fixed-point FFT cache benchmark |
| `code/rtos_workloads/franka/` | Periodic Franka Cartesian-impedance workload and controller kernel |
| `code/rtos_workloads/SOURCE_INDEX.csv` | RTOS source paths and entry points relative to `code/rtos_workloads/` |

The Linux snoop-filter package includes the item-sweep, bit-scan, joint-flip,
color-matrix, PMU-attribution, cross-probe, ablation, and sampling modules and
their launch/analysis utilities. Its detailed build and board-run notes are in
`code/linux_snoop_filter/README.md`.

The FFT RTOS entry point is `fft_cache_test_demo()`. The Franka workload entry
point is `timer_rt_test_demo()`; its controller implementation is in
`franka_cartesian_impedance_kernel.c` and `.h`. More integration notes are in
`code/rtos_workloads/README.md`.

## Build scope and limitation

These source files are experiment and workload excerpts. They are not a
complete Xen-UniProton source tree and are not an independently buildable
Xen-UniProton system. Compilation and execution require the original Xen and
UniProton sources, matching Linux kernel build tree, board-support headers,
memory-partition and linker configuration, Arm64 cross-toolchain, application
build integration, privileged deployment environment, and workload binaries.
Those components are not included or reconstructed in this package.
