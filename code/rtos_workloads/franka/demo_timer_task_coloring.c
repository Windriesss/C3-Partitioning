/*
 * Copyright (c) 2024, Greater Bay Area National Center of Technology Innovation
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Change Logs:
 * Date           Author       Notes
 * 2025-12-16     HuaijiGao    Initial version
 * 2026-03-04     HuaijiGao    Moved to demos/common
 */

#include <prt_typedef.h>
#include <prt_hwi.h>
#include <prt_sem.h>
#include <prt_clk.h>
#include <prt_config.h>
#include <prt_config_internal.h>
#include <prt_task.h>
#include <prt_atomic.h>
#include <prt_clk.h>
#include <print.h> 
#include <prt_sys_external.h>
#include <rtdevice.h>

#include "demo.h"
#include "cache_partitioning.h"
#include "prt_page_external.h"
#include "franka_cartesian_impedance_kernel.h"


#define US_PER_SEC 1000000ULL
#define NS_PER_SEC 1000000000ULL

#define TEST_DURATION_S (1 * 600)

#define TASK_DURATION_US 1000
#define REGION_LENGTH 1000

/*
 * Board-specific demo.h must define the hardware-timer device name, for
 * example "timer0" or "timer6".
 */
#ifndef DEMO_COLORING_TIMER_DEV
#error "Please define DEMO_COLORING_TIMER_DEV in your board's demo.h"
#endif

USER_CRIT_DATA static rt_device_t timer_dev = RT_NULL;

USER_CRIT_DATA static SemHandle timer_sem;

USER_CRIT_DATA static U64 t0 = 0;
static U64 t0_min = 0xffffffff;
static U64 t0_max = 0;
static U64 t0_min_round = 0;
static U64 t0_max_round = 0;

USER_CRIT_DATA static U64 t1 = 0;
static U64 t1_min = 0xffffffff;
static U64 t1_max = 0;
static U64 t1_min_round = 0;
static U64 t1_max_round = 0;

USER_CRIT_DATA static U64 t2 = 0;
static U64 t2_min = 0xffffffff;
static U64 t2_max = 0;
static U64 t2_min_round = 0;
static U64 t2_max_round = 0;

USER_CRIT_DATA static U64 robot_exec_min_ns = ~0ULL;
USER_CRIT_DATA static U64 robot_exec_max_ns;
USER_CRIT_DATA static U64 robot_exec_sum_ns;
USER_CRIT_DATA static U64 robot_exec_samples;

static U64 t0_region[REGION_LENGTH];
static U64 t1_region[REGION_LENGTH];
static U64 t2_region[REGION_LENGTH];

static U32 OsSysClock;

USER_CRIT_DATA static int taskpendcnt = 0;

static int error_cnt;

USER_CRIT_DATA static U64 int_cnt = 0;

#define FRANKA_TRACE_SAMPLES 8U
/*
 * Deterministic emulation of Franka state-interface inputs. These samples are
 * replayed directly; no robot plant dynamics or state integration is modeled.
 */
USER_CRIT_DATA static double g_franka_q_trace[FRANKA_TRACE_SAMPLES][FRANKA_JOINTS] = {
    {0.000, -0.350, 0.000, -1.200, 0.000, 0.900, 0.000},
    {0.012, -0.342, 0.006, -1.188, 0.004, 0.894, 0.003},
    {0.021, -0.337, 0.011, -1.176, 0.008, 0.887, 0.006},
    {0.027, -0.335, 0.014, -1.168, 0.011, 0.881, 0.008},
    {0.025, -0.338, 0.012, -1.170, 0.010, 0.883, 0.007},
    {0.017, -0.344, 0.008, -1.180, 0.006, 0.889, 0.005},
    {0.007, -0.349, 0.003, -1.192, 0.002, 0.896, 0.002},
    {-0.004, -0.352, -0.002, -1.203, -0.001, 0.902, -0.001}};
USER_CRIT_DATA static double g_franka_dq_trace[FRANKA_TRACE_SAMPLES][FRANKA_JOINTS] = {
    {0.00, 0.00, 0.00, 0.00, 0.00, 0.00, 0.00},
    {0.12, 0.08, 0.06, 0.12, 0.04, -0.06, 0.03},
    {0.09, 0.05, 0.05, 0.12, 0.04, -0.07, 0.03},
    {0.06, 0.02, 0.03, 0.08, 0.03, -0.06, 0.02},
    {-0.02, -0.03, -0.02, -0.02, -0.01, 0.02, -0.01},
    {-0.08, -0.06, -0.04, -0.10, -0.04, 0.06, -0.02},
    {-0.10, -0.05, -0.05, -0.12, -0.04, 0.07, -0.03},
    {-0.11, -0.03, -0.05, -0.11, -0.03, 0.06, -0.03}};
USER_CRIT_DATA static FrankaCartesianInput g_franka_input = {
    .position = {0.410, 0.015, 0.510},
    .orientation = {0.9987502604, 0.000, 0.0499791693, 0.000},
    .desired_position = {0.400, 0.000, 0.500},
    .desired_orientation = {1.000, 0.000, 0.000, 0.000},
    .q_nullspace = {0.000, -0.350, 0.000, -1.200, 0.000, 0.900, 0.000},
    .coriolis = {0.20, -0.12, 0.08, 0.25, -0.05, 0.03, 0.01},
    .jacobian = {
        {0.000, -0.320, 0.015, 0.250, -0.010, 0.120, 0.000},
        {0.310, 0.010, 0.260, -0.015, 0.100, 0.005, 0.040},
        {0.020, 0.280, -0.010, -0.200, 0.015, 0.080, 0.010},
        {0.000, 0.050, 0.120, -0.030, 0.220, 0.040, 0.960},
        {0.080, 0.940, -0.060, 0.880, 0.030, 0.750, -0.020},
        {0.990, -0.040, 0.930, 0.070, 0.820, -0.050, 0.110}
    }
};
USER_CRIT_DATA static FrankaCartesianKernel g_franka_kernel;

typedef struct {
    U64 ll_cache_rd;
    U64 ll_cache_miss_rd;
    U64 l2d_cache_refill;
} PmuLlcSnapshot;

typedef struct {
    U64 prev;
    U64 curr;
} PmuSingleCounterSnapshot;

USER_CRIT_DATA PmuLlcSnapshot g_pmu_point_a = {0};
USER_CRIT_DATA PmuLlcSnapshot g_pmu_point_b = {0};

USER_CRIT_DATA PmuSingleCounterSnapshot g_l2d_cache_inval_point = {0};

USER_CRIT_DATA volatile int g_pmu_point_a_valid = 0;
USER_CRIT_DATA volatile int g_pmu_point_b_valid = 0;
USER_CRIT_DATA volatile int g_pmu_measure_armed = 0;

USER_CRIT_DATA volatile int g_l2d_cache_inval_valid = 0;

USER_CRIT_DATA PmuLlcSnapshot diff = {0};
PmuLlcSnapshot max_diff = {0};
USER_CRIT_DATA PmuSingleCounterSnapshot l2d_cache_inval_diff = {0};
PmuSingleCounterSnapshot l2d_cache_inval_max_diff = {0};
USER_CRIT_DATA double miss_rate = 0.0;
USER_CRIT_DATA U64 ll_cache_rd_diff_sum = 0;
USER_CRIT_DATA U64 ll_cache_miss_rd_diff_sum = 0;
USER_CRIT_DATA U64 l2d_cache_refill_diff_sum = 0;
USER_CRIT_DATA U64 l2d_cache_inval_diff_sum = 0;
USER_CRIT_DATA U64 ll_cache_diff_sample_cnt = 0;

void enable_pmu_llc_rd_miss(void)
{
    __asm__ volatile(
        /* Enable the PMU and reset event and cycle counters. */
        "MRS x0, PMCR_EL0\n"
        "ORR x0, x0, #0x7\n"
        "MSR PMCR_EL0, x0\n"

        /* Event counter 0: LL_CACHE_RD (0x36). */
        "MOV x0, #0x0036\n"
        "MSR PMEVTYPER0_EL0, x0\n"
        "MSR PMEVCNTR0_EL0, xzr\n"

        /* Event counter 1: LL_CACHE_MISS_RD (0x37). */
        "MOV x0, #0x0037\n"
        "MSR PMEVTYPER1_EL0, x0\n"
        "MSR PMEVCNTR1_EL0, xzr\n"

        /* Event counter 2: L2D_CACHE_REFILL (0x17). */
        "MOV x0, #0x0017\n"
        "MSR PMEVTYPER2_EL0, x0\n"
        "MSR PMEVCNTR2_EL0, xzr\n"

        /* Enable event counters 0 through 2. */
        "MRS x0, PMCNTENSET_EL0\n"
        "ORR x0, x0, #0x7\n"
        "MSR PMCNTENSET_EL0, x0\n"

        "ISB\n"
        :
        :
        : "x0", "memory");
}

USER_CRIT_TEXT static inline unsigned long long read_ll_cache_rd(void)
{
    unsigned long long v;
    __asm__ volatile("MRS %0, PMEVCNTR0_EL0" : "=r"(v));
    return v;
}

USER_CRIT_TEXT static inline unsigned long long read_ll_cache_miss_rd(void)
{
    unsigned long long v;
    __asm__ volatile("MRS %0, PMEVCNTR1_EL0" : "=r"(v));
    return v;
}

USER_CRIT_TEXT static inline unsigned long long read_l2d_cache_refill(void)
{
    unsigned long long v;
    __asm__ volatile("MRS %0, PMEVCNTR2_EL0" : "=r"(v));
    return v;
}

USER_CRIT_TEXT static inline unsigned long long read_l2d_cache_inval(void)
{
    unsigned long long v;
    __asm__ volatile("MRS %0, PMEVCNTR3_EL0" : "=r"(v));
    return v;
}

void enable_pmu_l2d_cache_inval(void)
{
    __asm__ volatile(
        /* Enable the PMU and reset event and cycle counters. */
        "MRS x0, PMCR_EL0\n"
        "ORR x0, x0, #0x7\n"
        "MSR PMCR_EL0, x0\n"

        /* Event counter 3: L2D_CACHE_INVAL (0x58). */
        "MOV x0, #0x0058\n"
        "MSR PMEVTYPER3_EL0, x0\n"
        "MSR PMEVCNTR3_EL0, xzr\n"

        /* Enable event counter 3. */
        "MRS x0, PMCNTENSET_EL0\n"
        "ORR x0, x0, #0x8\n"
        "MSR PMCNTENSET_EL0, x0\n"

        "ISB\n"
        :
        :
        : "x0", "memory");
}



/* PMU begin/end markers are safe in interrupt context; reporting is deferred. */
USER_CRIT_TEXT void pmu_mark_begin(void)
{
    if (!g_pmu_measure_armed) {
        return;
    }

    g_pmu_measure_armed = 0;
    g_pmu_point_a.ll_cache_rd = read_ll_cache_rd();
    g_pmu_point_a.ll_cache_miss_rd = read_ll_cache_miss_rd();
    g_pmu_point_a.l2d_cache_refill = read_l2d_cache_refill();
    g_pmu_point_a_valid = 1;
    g_pmu_point_b_valid = 0;
}

USER_CRIT_TEXT void pmu_mark_end(void)
{
    if (!g_pmu_point_a_valid) {
        return;
    }

    g_pmu_point_b.ll_cache_miss_rd = read_ll_cache_miss_rd();
    g_pmu_point_b.ll_cache_rd = read_ll_cache_rd();
    g_pmu_point_b.l2d_cache_refill = read_l2d_cache_refill();
    g_pmu_point_b_valid = 1;
}

USER_CRIT_TEXT int pmu_calc_begin_end_diff(PmuLlcSnapshot *diff, double *miss_rate)
{
    if (!g_pmu_point_a_valid || !g_pmu_point_b_valid || diff == NULL || miss_rate == NULL) {
        return -1;
    }

    diff->ll_cache_rd = (g_pmu_point_b.ll_cache_rd >= g_pmu_point_a.ll_cache_rd) ?
        (g_pmu_point_b.ll_cache_rd - g_pmu_point_a.ll_cache_rd) : 0;
    diff->ll_cache_miss_rd = (g_pmu_point_b.ll_cache_miss_rd >= g_pmu_point_a.ll_cache_miss_rd) ?
        (g_pmu_point_b.ll_cache_miss_rd - g_pmu_point_a.ll_cache_miss_rd) : 0;
    diff->l2d_cache_refill = (g_pmu_point_b.l2d_cache_refill >= g_pmu_point_a.l2d_cache_refill) ?
        (g_pmu_point_b.l2d_cache_refill - g_pmu_point_a.l2d_cache_refill) : 0;

    *miss_rate = (diff->ll_cache_rd == 0) ? 0.0 :
        ((double)diff->ll_cache_miss_rd * 100.0 / (double)diff->ll_cache_rd);

    return 0;
}

USER_CRIT_TEXT void pmu_dump_begin_end_diff(const char *tag, const PmuLlcSnapshot *diff, double miss_rate)
{
    if (diff == NULL)
    {
        return;
    }
    if (miss_rate == -1.0)
    {
        miss_rate = (diff->ll_cache_rd == 0) ? 0.0 : ((double)diff->ll_cache_miss_rd * 100.0 / (double)diff->ll_cache_rd);
    }
    PRT_Printf("[%s] DIFF LL_CACHE_RD: %llu, DIFF LL_CACHE_MISS_RD: %llu, MISS_RATE: %.5f%%\n",
               (tag == NULL) ? "begin->end" : tag,
               (unsigned long long)diff->ll_cache_rd,
               (unsigned long long)diff->ll_cache_miss_rd,
               miss_rate);
    PRT_Printf("[%s] DIFF L2D_CACHE_REFILL: %llu\n",
               (tag == NULL) ? "begin->end" : tag,
               (unsigned long long)diff->l2d_cache_refill);
}

USER_CRIT_TEXT void pmu_l2d_cache_inval_init(void)
{
    g_l2d_cache_inval_point.prev = read_l2d_cache_inval();
    g_l2d_cache_inval_point.curr = g_l2d_cache_inval_point.prev;
    g_l2d_cache_inval_valid = 1;
}

USER_CRIT_TEXT void pmu_l2d_cache_inval_mark_end(void)
{
    if (!g_l2d_cache_inval_valid) {
        return;
    }

    g_l2d_cache_inval_point.curr = read_l2d_cache_inval();
}

USER_CRIT_TEXT int pmu_l2d_cache_inval_calc_diff(PmuSingleCounterSnapshot *diff)
{
    if (!g_l2d_cache_inval_valid || diff == NULL) {
        return -1;
    }

    diff->curr = (g_l2d_cache_inval_point.curr >= g_l2d_cache_inval_point.prev) ?
        (g_l2d_cache_inval_point.curr - g_l2d_cache_inval_point.prev) : 0;
    diff->prev = g_l2d_cache_inval_point.curr;
    g_l2d_cache_inval_point.prev = g_l2d_cache_inval_point.curr;
    return 0;
}

USER_CRIT_TEXT void pmu_l2d_cache_inval_dump_diff(const char *tag, const PmuSingleCounterSnapshot *diff)
{
    if (diff == NULL) {
        return;
    }

    PRT_Printf("[%s] DIFF L2D_CACHE_INVAL: %llu\n",
               (tag == NULL) ? "begin->end" : tag,
               (unsigned long long)diff->curr);
}

USER_CRIT_TEXT static inline U64 get_current_time_ns(void)
{
    rt_hwtimerval_t tv;
    rt_device_read(timer_dev, 0, &tv, sizeof(tv));
    U64 cur_timer_ns = tv.sec * 1000000000LL + 1000LL * tv.usec +  tv.nsec;
    U64 last_int_ns = int_cnt * TASK_DURATION_US * 1000;

    return cur_timer_ns - last_int_ns;
}

USER_CRIT_TEXT static rt_err_t my_timeout_callback(rt_device_t dev, rt_size_t size)
{
    int_cnt++;
    t0 = get_current_time_ns();
    PRT_SemPost(timer_sem);
    return 0;
}

static int TIMER_Init(U64 task_us)
{
    rt_hwtimer_mode_t mode;

    timer_dev = rt_device_find(DEMO_COLORING_TIMER_DEV);
    if (timer_dev == RT_NULL)
    {
        PRT_Printf("timer device not found!\n");
        return -1;
    }
    rt_device_open(timer_dev, RT_DEVICE_OFLAG_RDWR);
    rt_device_set_rx_indicate(timer_dev, my_timeout_callback);
    mode = HWTIMER_MODE_PERIOD;
    rt_device_control(timer_dev, HWTIMER_CTRL_MODE_SET, &mode);

    rt_hwtimerval_t timeout_s;
    timeout_s.sec = 0;
    timeout_s.usec = task_us;
    rt_device_write(timer_dev, 0, &timeout_s, sizeof(timeout_s));
    return 0;
}

#define TEST_MEMSIZE                 (1024 * 1024 * 10)
#define PRESSURE_REPORT_INTERVAL_MS   2000
#define PRESSURE_STATUS_ALLOC_FAILED  (-1)
#define PRESSURE_STATUS_NOT_STARTED   0
#define PRESSURE_STATUS_RUNNING       1

static U64 pressure_copy_rounds;
static U64 pressure_copy_bytes;
static U64 pressure_verify_checksum;
static int pressure_task_status = PRESSURE_STATUS_NOT_STARTED;

static void pressurTaskEntry() 
{
    char *src = PRT_MemAllocAlign(0, OS_MEM_NORMAL_PT, TEST_MEMSIZE, MEM_ADDR_ALIGN_4K);
    char *dst = PRT_MemAllocAlign(0, OS_MEM_NORMAL_PT, TEST_MEMSIZE, MEM_ADDR_ALIGN_4K);
    if (!src || !dst) {
        pressure_task_status = PRESSURE_STATUS_ALLOC_FAILED;
        PRT_Printf("pressurTaskEntry: PRT_MemAllocAlign(NORMAL) failed\n");
        return;
    }

    PRT_Printf("pressurTaskEntry start \n");
    PRT_Printf("src addr: 0x%p\n", src);
    PRT_Printf("dst addr: 0x%p \n", dst);
    PRT_Printf("TEST_MEMSIZE: 0x%x \n", TEST_MEMSIZE);
    pressure_task_status = PRESSURE_STATUS_RUNNING;

    while(1)
    {
        uint64_t iteration_checksum = 0;

        for(int i = 0; i < TEST_MEMSIZE; i++)
        {
            src[i] = (char)(i * 2);
        }
        memcpy(dst, src, TEST_MEMSIZE);

        for(int i = 0; i < TEST_MEMSIZE; i++)
        {
            iteration_checksum += (unsigned char)dst[i];
        }

        /*
         * Publish a round only after fill, memcpy and verification all finish.
         * copy_bytes counts logical memcpy payload, not total memory traffic.
         */
        PRT_AtomicAddU64(&pressure_verify_checksum, iteration_checksum);
        PRT_AtomicAddU64(&pressure_copy_bytes, (uint64_t)TEST_MEMSIZE);
        PRT_AtomicAddU64(&pressure_copy_rounds, 1);
    }
}

static int create_task_pressure()
{
    U32 ret;
    struct TskInitParam param = {0};
    TskHandle taskPid;

    for(int i = 0; i < 1; i++) 
    {
        /* Keep the pressure-task stack in the NORMAL memory partition. */
        param.stackAddr = (uintptr_t)PRT_MemAllocAlign(0, OS_MEM_NORMAL_PT, 0x2000, MEM_ADDR_ALIGN_016);
        param.taskEntry = (TskEntryFunc)pressurTaskEntry;
        param.taskPrio = 40;
        param.name = "TestTask";
        param.stackSize = 0x2000;

        ret = PRT_TaskCreate(&taskPid, &param);
        if (ret)
        {
            return ret;
        }

        ret = PRT_TaskResume(taskPid);
        if (ret)
        {
            return ret;
        }
    }
    return 0;
}

#define UPDATE_MIN_MAX(val, minv, maxv, minr, maxr) \
    do { \
        if ((val) < (minv)) { (minv) = (val); (minr) = taskpendcnt; } \
        if ((val) > (maxv)) { (maxv) = (val); (maxr) = taskpendcnt; } \
    } while (0)


static void call_hvc(int x)
{
    register long num_result asm("x0") = x;
    __asm__ __volatile__(
        "hvc #0x4a48"
        : "+r"(num_result)
        : : "memory");
}

USER_CRIT_TEXT static int hrtimer_test_demo()
{  
    U32 ret;
    int first = 1;
    int io = 0;
    S64 task_cycles = TASK_DURATION_US;
    OsSysClock = OsSysGetClock();

    PRT_Printf("HRTIMER NOSTRESS TEST Demo\n");
    PRT_Printf("OsSysGetClock():%lu\n", OsSysClock);
    PRT_Printf("task_cycles:%d \n", task_cycles);
    PRT_Printf("=======begin========\n\n");

    ret = PRT_SemCreate(0, &timer_sem);
    if (ret != OS_OK)
    {
        PRT_Printf("creeate timer_sem failed\n");
        return -1;
    }

    PRT_Printf("cache_partitioning_task_stack done\n");

    FrankaCartesianImpedanceKernelInit(&g_franka_kernel);
    TIMER_Init(TASK_DURATION_US);
 
    first = 10;
    while(1)
    {
        g_pmu_point_a_valid = 0;
        g_pmu_point_b_valid = 0;
        g_pmu_measure_armed = 1;
        PRT_SemPend(timer_sem, OS_WAIT_FOREVER);
        taskpendcnt++; 
        
        unsigned int franka_sample =
            (unsigned int)(g_franka_kernel.update_count & (FRANKA_TRACE_SAMPLES - 1U));
        for (int joint = 0; joint < FRANKA_JOINTS; ++joint) {
            g_franka_input.q[joint] = g_franka_q_trace[franka_sample][joint];
            g_franka_input.dq[joint] = g_franka_dq_trace[franka_sample][joint];
        }
        t1 = get_current_time_ns();
        FrankaCartesianImpedanceKernelStep(&g_franka_kernel, &g_franka_input);
        t2 = get_current_time_ns();
        U64 robot_exec_ns = t2 - t1;

        pmu_mark_end();
        pmu_l2d_cache_inval_mark_end();

        if (!first) 
        {
            if (robot_exec_ns < robot_exec_min_ns) {
                robot_exec_min_ns = robot_exec_ns;
            }
            if (robot_exec_ns > robot_exec_max_ns) {
                robot_exec_max_ns = robot_exec_ns;
            }
            robot_exec_sum_ns += robot_exec_ns;
            robot_exec_samples++;
            int pmu_ret = pmu_calc_begin_end_diff(&diff, &miss_rate);
            if (pmu_ret == 0) {
                ll_cache_rd_diff_sum += diff.ll_cache_rd;
                ll_cache_miss_rd_diff_sum += diff.ll_cache_miss_rd;
                l2d_cache_refill_diff_sum += diff.l2d_cache_refill;
                ll_cache_diff_sample_cnt++;
                if (t1 > t1_max) {
                    max_diff = diff;
                }
            }

            if (pmu_l2d_cache_inval_calc_diff(&l2d_cache_inval_diff) == 0) {
                l2d_cache_inval_diff_sum += l2d_cache_inval_diff.curr;
                if (l2d_cache_inval_diff.curr > l2d_cache_inval_max_diff.curr) {
                    l2d_cache_inval_max_diff = l2d_cache_inval_diff;
                }
            }
            
            UPDATE_MIN_MAX(t0, t0_min, t0_max, t0_min_round, t0_max_round);
            UPDATE_MIN_MAX(t1, t1_min, t1_max, t1_min_round, t1_max_round);
            UPDATE_MIN_MAX(t2, t2_min, t2_max, t2_min_round, t2_max_round);

            
            U64 vals[3] = { t0, t1, t2 };
            U64 *regions[3] = { t0_region, t1_region, t2_region };
            for (int i = 0; i < 3; ++i) {
                U64 us = vals[i] / 1000;
                if (us >= (U64)REGION_LENGTH) {
                    error_cnt++;
                } else {
                    regions[i][us]++;
                }
            }
            
        }
        else {
            first--; 
            taskpendcnt = 0;
        }

        if(taskpendcnt >= TEST_DURATION_S * US_PER_SEC / TASK_DURATION_US) {
            rt_device_close(timer_dev);
        }
    }
}

static float ns2us(U64 ns)
{
    return ns / 1000.0;
}

static void rt_result_dump(void)
{
    PRT_Printf("\n===========================================================\n");
    PRT_Printf("taskpendcnt:%ld\n", taskpendcnt);
    PRT_Printf("t0:%llu ns\n", t0);
    PRT_Printf("t1:%llu ns\n", t1);
    PRT_Printf("t2:%llu ns\n", t2);
    PRT_Printf("benchmark: Franka-derived Cartesian-impedance kernel, input trace:%u, timer period:%.3f ms\n",
               FRANKA_TRACE_SAMPLES,
               (double)TASK_DURATION_US / 1000.0);
    PRT_Printf("controller source: franka_ros2 jazzy@73a1501d, updates:%llu, SVD sweeps:%u\n",
               (unsigned long long)g_franka_kernel.update_count,
               g_franka_kernel.svd_sweeps);
    PRT_Printf("pose error:[%.6f %.6f %.6f %.6f %.6f %.6f]\n",
               g_franka_kernel.pose_error[0], g_franka_kernel.pose_error[1],
               g_franka_kernel.pose_error[2], g_franka_kernel.pose_error[3],
               g_franka_kernel.pose_error[4], g_franka_kernel.pose_error[5]);
    PRT_Printf("tau[0:6]:[%.6f %.6f %.6f %.6f %.6f %.6f %.6f]\n",
               g_franka_kernel.tau_command[0], g_franka_kernel.tau_command[1],
               g_franka_kernel.tau_command[2], g_franka_kernel.tau_command[3],
               g_franka_kernel.tau_command[4], g_franka_kernel.tau_command[5],
               g_franka_kernel.tau_command[6]);
    PRT_Printf("kernel exec min/avg/max: %.2f / %.2f / %.2f us, samples:%llu\n",
               (robot_exec_samples == 0) ? 0.0 : ns2us(robot_exec_min_ns),
               (robot_exec_samples == 0) ? 0.0 :
                   ns2us(robot_exec_sum_ns) / (double)robot_exec_samples,
               (robot_exec_samples == 0) ? 0.0 : ns2us(robot_exec_max_ns),
               (unsigned long long)robot_exec_samples);

    static U64 pressure_prev_rounds;
    static U64 pressure_prev_bytes;
    static U64 pressure_prev_report_ns;
    U64 pressure_report_ns = PRT_ClkCycle2Ns(PRT_ClkGetCycleCount64());
    U64 pressure_rounds = PRT_AtomicAddU64Rtn(&pressure_copy_rounds, 0);
    U64 pressure_bytes = PRT_AtomicAddU64Rtn(&pressure_copy_bytes, 0);
    U64 pressure_checksum = PRT_AtomicAddU64Rtn(&pressure_verify_checksum, 0);
    U64 pressure_round_delta = pressure_rounds - pressure_prev_rounds;
    U64 pressure_byte_delta = pressure_bytes - pressure_prev_bytes;
    double pressure_window_s = (pressure_prev_report_ns == 0) ?
        ((double)PRESSURE_REPORT_INTERVAL_MS / 1000.0) :
        ((double)(pressure_report_ns - pressure_prev_report_ns) / (double)NS_PER_SEC);
    double pressure_mib_total = (double)pressure_bytes / (1024.0 * 1024.0);
    double pressure_mib_per_s =
        (double)pressure_byte_delta / (1024.0 * 1024.0) / pressure_window_s;
    double pressure_rounds_per_s = (double)pressure_round_delta / pressure_window_s;

    PRT_Printf("pressure status:%d, copy rounds total/window:%llu/%llu, %.2f rounds/s\n",
               pressure_task_status,
               (unsigned long long)pressure_rounds,
               (unsigned long long)pressure_round_delta,
               pressure_rounds_per_s);
    PRT_Printf("pressure logical copy:%.2f MiB total, approx %.2f MiB/s, checksum:%llu\n",
               pressure_mib_total,
               pressure_mib_per_s,
               (unsigned long long)pressure_checksum);
    PRT_Printf("pressure report window:%.6f s\n", pressure_window_s);
    pressure_prev_rounds = pressure_rounds;
    pressure_prev_bytes = pressure_bytes;
    pressure_prev_report_ns = pressure_report_ns;

    const char *names[] = { "t0_region", "t1_region", "t2_region" };
    U64 *regions[] = { t0_region, t1_region, t2_region };
    for (int r = 0; r < 3; ++r)
    {
        PRT_Printf("%s>\n", names[r]);
        for (int i = 0; i < REGION_LENGTH; ++i)
        {
            U64 val = regions[r][i];
            if (val != 0)
            {
                PRT_Printf("[%d]:%llu\n", i, (unsigned long long)val);
            }
        }
        PRT_Printf("\n");
    }

    PRT_Printf("t0_min: %.2f us, round:%llu\n", ns2us(t0_min), t0_min_round);
    PRT_Printf("t0_max: %.2f us, round:%llu\n", ns2us(t0_max), t0_max_round);
    PRT_Printf("t0_jitter: %.2f us\n\n", ns2us(t0_max - t0_min));

    PRT_Printf("t1_min: %.2f us, round:%llu\n", ns2us(t1_min), t1_min_round);
    PRT_Printf("t1_max: %.2f us, round:%llu\n", ns2us(t1_max), t1_max_round);
    PRT_Printf("t1_jitter: %.2f us\n\n", ns2us(t1_max - t1_min));

    PRT_Printf("t2_min: %.2f us, round:%llu\n", ns2us(t2_min), t2_min_round);
    PRT_Printf("t2_max: %.2f us, round:%llu\n", ns2us(t2_max), t2_max_round);
    PRT_Printf("t2_jitter: %.2f us\n\n", ns2us(t2_max - t2_min));

    if (ll_cache_diff_sample_cnt > 0) {
        double avg_ll_cache_rd = (double)ll_cache_rd_diff_sum / (double)ll_cache_diff_sample_cnt;
        double avg_ll_cache_miss_rd = (double)ll_cache_miss_rd_diff_sum / (double)ll_cache_diff_sample_cnt;
        double avg_l2d_cache_refill = (double)l2d_cache_refill_diff_sum / (double)ll_cache_diff_sample_cnt;
        double avg_l2d_cache_inval = (double)l2d_cache_inval_diff_sum / (double)ll_cache_diff_sample_cnt;
        PRT_Printf("PMU diff samples: %llu\n", (unsigned long long)ll_cache_diff_sample_cnt);
        PRT_Printf("AVG DIFF LL_CACHE_RD: %.5f\n", avg_ll_cache_rd);
        PRT_Printf("AVG DIFF LL_CACHE_MISS_RD: %.5f\n", avg_ll_cache_miss_rd);
        PRT_Printf("AVG DIFF L2D_CACHE_REFILL: %.5f\n", avg_l2d_cache_refill);
        PRT_Printf("AVG DIFF L2D_CACHE_INVAL: %.5f\n", avg_l2d_cache_inval);
    } else {
        PRT_Printf("PMU diff samples: 0\n");
    }
}

static int create_rt_task()
{
    U32 ret;
    struct TskInitParam param = {0};
    TskHandle taskPid;

    for(int i = 0; i < 1; i++)
    {
        /* Keep the real-time task stack in the CRITICAL memory partition. */
        param.stackAddr = (uintptr_t)PRT_MemAllocAlign(0, OS_MEM_CRITICAL_PT, 0x2000, MEM_ADDR_ALIGN_4K);
        param.taskEntry = (TskEntryFunc)hrtimer_test_demo;
        param.taskPrio = 1;
        param.name = "RtTask";
        param.stackSize = 0x2000;

        ret = PRT_TaskCreate(&taskPid, &param);
        if (ret)
        {
            return ret;
        } 

        ret = PRT_TaskResume(taskPid);
        if (ret)
        {
            return ret;
        }
    }
    return 0;
} 

extern int realtime_performance_control(bool open);
void timer_rt_test_demo(void)
{
    PRT_Printf("Starting timer_rt_test_demo...sleep 25s\n");
    PRT_TaskDelay(25000);
    PRT_Printf("open interrupt and realtime performance control\n");

    register long num_result __asm__("x0") = 12;
    register long __arg1 __asm__("x1") = 0x3ff;
    register long __arg2 __asm__("x2") = 0;
    __asm__ volatile("hvc #0x4a48" : "+r"(num_result), "+r"(__arg1), "+r"(__arg2) : : "memory");
    
    /*
     * Cache coloring is initialized by OsCacheColoringInit() in apps_entry().
     * The CRITICAL:NORMAL allocation ratio is 1:7.
     */
#ifdef OS_OPTION_CACHECOLORING
    OsColorHeapDump();
#endif

    enable_pmu_llc_rd_miss();
    enable_pmu_l2d_cache_inval();
    pmu_l2d_cache_inval_init();

    create_task_pressure();


    create_rt_task();

    bool isPasstroughMode = true;
    while(1) {
        
        PRT_TaskDelay(PRESSURE_REPORT_INTERVAL_MS);

        rt_result_dump();
        pmu_dump_begin_end_diff("cur diff", &diff, miss_rate);
        pmu_l2d_cache_inval_dump_diff("cur diff", &l2d_cache_inval_diff);
        pmu_dump_begin_end_diff("max diff", &max_diff, -1.0);
        pmu_l2d_cache_inval_dump_diff("max diff", &l2d_cache_inval_max_diff);
        
        if(taskpendcnt >= TEST_DURATION_S * US_PER_SEC / TASK_DURATION_US && isPasstroughMode) {
            register long num_result __asm__("x0") = 13;
            register long __arg1 __asm__("x1") = 0x3ff;
            register long __arg2 __asm__("x2") = 0;
            __asm__ volatile("hvc #0x4a48" : "+r"(num_result), "+r"(__arg1), "+r"(__arg2) : : "memory");
            
            isPasstroughMode=false;
        }
    }
}
