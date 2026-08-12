#include <prt_task.h>
#include <prt_clk.h>
#include <prt_mem.h>
#include <prt_config_internal.h>
#include <cpu_config.h>
#include <print.h>
#ifdef OS_OPTION_CACHECOLORING
#include <mem/prt_page_external.h>
#endif

typedef struct {
    int re;
    int im;
} FftComplex;

#define FFT_STACK_SIZE 0x8000
#define FFT_NUM_RUNS 5
#define FFT_MIN_LOG2 15
#define FFT_MAX_LOG2 17

#if defined(OS_OPTION_CACHECOLORING)
#define FFT_MEM_PT OS_MEM_CRITICAL_PT
#else
#define FFT_MEM_PT OS_MEM_DEFAULT_PT0
#endif

#if defined(OS_OPTION_SMP)
#define FFT_CORE_MASK (1U << OS_SYS_CORE_PRIMARY)
#endif

static volatile unsigned int g_fft_sink;

static unsigned int fft_rand(unsigned int *seed)
{
    *seed = (*seed * 1664525U) + 1013904223U;
    return *seed;
}

static void *fft_alloc(unsigned long bytes)
{
    return PRT_MemAllocAlign(0, FFT_MEM_PT, bytes, MEM_ADDR_ALIGN_4K);
}

static unsigned int fft_reverse_bits(unsigned int x, unsigned int bits)
{
    unsigned int r = 0;

    for (unsigned int i = 0; i < bits; i++) {
        r = (r << 1) | (x & 1U);
        x >>= 1;
    }

    return r;
}

static void fft_init(FftComplex *data, FftComplex *twiddle,
                     unsigned int n, unsigned int log2n)
{
    unsigned int seed = 0x9e3779b9U ^ n;

    for (unsigned int i = 0; i < n; i++) {
        unsigned int v = fft_rand(&seed);
        data[i].re = (int)(v & 0x7fffU) - 0x4000;
        data[i].im = (int)((v >> 16) & 0x7fffU) - 0x4000;
    }

    /*
     * Q15 twiddle-like values. They are deterministic and bounded; exact
     * trigonometric precision is not important for this throughput benchmark.
     */
    for (unsigned int i = 0; i < n / 2U; i++) {
        unsigned int phase = fft_reverse_bits(i, log2n - 1U);
        int c = (int)((phase * 1103515245U + 12345U) & 0x7fffU) - 0x4000;
        int s = (int)((phase * 214013U + 2531011U) & 0x7fffU) - 0x4000;
        twiddle[i].re = c;
        twiddle[i].im = s;
    }
}

static void fft_bit_reverse(FftComplex *data, unsigned int n, unsigned int log2n)
{
    for (unsigned int i = 0; i < n; i++) {
        unsigned int j = fft_reverse_bits(i, log2n);
        if (j > i) {
            FftComplex tmp = data[i];
            data[i] = data[j];
            data[j] = tmp;
        }
    }
}

static unsigned int fft_kernel(FftComplex *data, FftComplex *twiddle,
                               unsigned int n, unsigned int log2n)
{
    unsigned int checksum = 0;

    fft_bit_reverse(data, n, log2n);

    for (unsigned int step = 2; step <= n; step <<= 1) {
        unsigned int half = step >> 1;
        unsigned int tw_stride = n / step;

        for (unsigned int base = 0; base < n; base += step) {
            for (unsigned int j = 0; j < half; j++) {
                FftComplex *u = &data[base + j];
                FftComplex *v = &data[base + j + half];
                FftComplex w = twiddle[j * tw_stride];
                int vr = v->re;
                int vi = v->im;
                int tr = (int)(((long long)vr * w.re - (long long)vi * w.im) >> 15);
                int ti = (int)(((long long)vr * w.im + (long long)vi * w.re) >> 15);
                int ur = u->re;
                int ui = u->im;

                u->re = ur + tr;
                u->im = ui + ti;
                v->re = ur - tr;
                v->im = ui - ti;
            }
        }
    }

    for (unsigned int i = 0; i < n; i += 257U) {
        checksum ^= (unsigned int)data[i].re;
        checksum = (checksum << 7) | (checksum >> 25);
        checksum ^= (unsigned int)data[i].im;
    }

    return checksum;
}

static void fft_task_entry(uintptr_t param1, uintptr_t param2,
                           uintptr_t param3, uintptr_t param4)
{
    (void)param1;
    (void)param2;
    (void)param3;
    (void)param4;

    PRT_Printf("Starting fft_task...sleep 25s\n");
    PRT_TaskDelay(25000);
    PRT_Printf("open interrupt and realtime performance control\n");

    register long num_result __asm__("x0") = 12;
    register long __arg1 __asm__("x1") = 0x3ff;
    register long __arg2 __asm__("x2") = 0;
    __asm__ volatile("hvc #0x4a48" : "+r"(num_result), "+r"(__arg1), "+r"(__arg2) : : "memory");

    while (1) {
        PRT_Printf("\n========================================================\n");
        PRT_Printf(" FFT Cache Benchmark: 32K/64K/128K fixed-point FFT\n");
        PRT_Printf(" Working set ~= data[N] + twiddle[N/2], mem_pt=%u\n", FFT_MEM_PT);
        PRT_Printf("========================================================\n");

        for (unsigned int log2n = FFT_MIN_LOG2; log2n <= FFT_MAX_LOG2; log2n++) {
            unsigned int n = 1U << log2n;
            unsigned long data_bytes = (unsigned long)n * sizeof(FftComplex);
            unsigned long twiddle_bytes = (unsigned long)(n / 2U) * sizeof(FftComplex);
            unsigned long workset_bytes = data_bytes + twiddle_bytes;
            FftComplex *data = (FftComplex *)fft_alloc(data_bytes);
            FftComplex *twiddle = (FftComplex *)fft_alloc(twiddle_bytes);

            if (!data || !twiddle) {
                PRT_Printf("Failed to allocate FFT buffers: N=%u workset=%lu KB\n",
                           n, workset_bytes / 1024UL);
                if (data) PRT_MemFree(0, data);
                if (twiddle) PRT_MemFree(0, twiddle);
                break;
            }

            U64 total_ns = 0;
            for (unsigned int run = 0; run < FFT_NUM_RUNS; run++) {
                fft_init(data, twiddle, n, log2n);

                U64 t1 = PRT_ClkGetCycleCount64();
                unsigned int checksum = fft_kernel(data, twiddle, n, log2n);
                U64 t2 = PRT_ClkGetCycleCount64();

                total_ns += (U64)PRT_ClkCycle2Ns(t2 - t1);
                g_fft_sink ^= checksum;
                PRT_TaskDelay(1);
            }

            U64 avg_ns = total_ns / (U64)FFT_NUM_RUNS;
            U64 butterflies = ((U64)n * (U64)log2n) / 2ULL;
            PRT_Printf("[FFT] N=%6u | workset=%5lu KB | avg=%8llu us | ns/bfly=%4llu | sink=0x%x\n",
                       n, workset_bytes / 1024UL, avg_ns / 1000ULL,
                       butterflies == 0 ? 0 : avg_ns / butterflies, g_fft_sink);

            PRT_MemFree(0, data);
            PRT_MemFree(0, twiddle);
            PRT_TaskDelay(10);
        }

        PRT_Printf("========================================================\n");
        PRT_Printf(" FFT Benchmark Round End\n");
        PRT_Printf("========================================================\n");
        PRT_TaskDelay(1000);
    }
}

void fft_cache_test_demo(void)
{
    U32 ret;
    struct TskInitParam param = {0};
    TskHandle taskPid;

    param.stackAddr = 0;
    param.taskEntry = (TskEntryFunc)fft_task_entry;
    param.taskPrio = 25;
    param.name = "FFTCacheTest";
    param.stackSize = FFT_STACK_SIZE;

    ret = PRT_TaskCreate(&taskPid, &param);
    if (ret != OS_OK) {
        PRT_Printf("Create FFT cache test task failed, ret: 0x%x!\n", ret);
        return;
    }
#if defined(OS_OPTION_SMP)
    PRT_TaskCoreBind(taskPid, FFT_CORE_MASK);
#endif
    PRT_TaskResume(taskPid);
}
