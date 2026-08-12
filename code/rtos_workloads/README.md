# RTOS Workload Source

Author: HuaijiGao

This directory contains the RTOS-side workload source used by the
C3-Partitioning evaluation. Each workload is stored in a separate subdirectory,
and `SOURCE_INDEX.csv` records its source path and entry point using paths
relative to this directory.

## Source-code scope

These files are workload excerpts, not a complete RTOS distribution. This
directory is not an independently compilable Xen-UniProton system. Compilation
requires the original UniProton source tree, board-support headers, memory-
partition configuration, Xen interface, linker configuration, toolchain, and
application build integration. Those components are not included or inferred.

## FFT cache benchmark

The first included workload is `fft/fft_cache_test.c`. It implements a
deterministic fixed-point FFT throughput benchmark for 32K, 64K, and 128K
complex-point inputs. Its approximate working set is the input array plus the
half-size twiddle array.

Entry point:

```c
void fft_cache_test_demo(void);
```

The source uses the RTOS `PRT_Task`, `PRT_Clk`, and `PRT_Mem` interfaces. When
`OS_OPTION_CACHECOLORING` is enabled, allocations use `OS_MEM_CRITICAL_PT`;
otherwise they use `OS_MEM_DEFAULT_PT0`. Under SMP, the task is bound to
`OS_SYS_CORE_PRIMARY`.

The source file is preserved as supplied. Integrate it into the RTOS build and
call `fft_cache_test_demo()` from the platform's application initialization
path. The exact build-system registration depends on the board's RTOS source
tree and is therefore not duplicated in this portable material package.

## Franka Cartesian-impedance workload

The `franka/` directory contains the periodic RTOS task and the deterministic
Cartesian-impedance controller kernel:

- `demo_timer_task_coloring.c`: timer-triggered workload, cache-colored task
  and pressure-task allocation, PMU sampling, and experiment reporting;
- `franka_cartesian_impedance_kernel.c`: controller update computation;
- `franka_cartesian_impedance_kernel.h`: controller state and public interface.

The workload entry point is `timer_rt_test_demo()`. The kernel is adapted from
Franka Robotics GmbH's ROS 2 Cartesian impedance example at the pinned commit
documented in the source. The upstream attribution and Apache license notice
are retained. Temporary comments, obsolete parameter alternatives, and
commented-out code have been removed from the packaged copy without changing
the control or measurement logic.

Expected runtime output is emitted through `PRT_Printf`. The FFT workload prints
N, working-set KiB, average microseconds, nanoseconds per butterfly, and a
checksum. The Franka workload prints response-time, jitter, PMU-difference, and
pressure-task statistics. Output capture and board deployment belong to the
larger experimental system and are not included here.
