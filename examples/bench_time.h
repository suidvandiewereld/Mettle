/*
 * High-resolution benchmark timing (QueryPerformanceCounter on Windows).
 */
#ifndef METTLE_EXAMPLES_BENCH_TIME_H
#define METTLE_EXAMPLES_BENCH_TIME_H

#include <stdint.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

static LARGE_INTEGER g_bench_qpc_freq;
static int g_bench_qpc_freq_inited = 0;

static void bench_time_init(void) {
    if (!g_bench_qpc_freq_inited) {
        QueryPerformanceFrequency(&g_bench_qpc_freq);
        g_bench_qpc_freq_inited = 1;
    }
}

static uint64_t bench_time_us(void) {
    LARGE_INTEGER counter;
    bench_time_init();
    QueryPerformanceCounter(&counter);
    if (g_bench_qpc_freq.QuadPart == 0) {
        return 0;
    }
    return (uint64_t)((counter.QuadPart * 1000000ULL) / (uint64_t)g_bench_qpc_freq.QuadPart);
}
#else
#include <sys/time.h>

static uint64_t bench_time_us(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (uint64_t)tv.tv_sec * 1000000ULL + (uint64_t)tv.tv_usec;
}
#endif

#endif /* METTLE_EXAMPLES_BENCH_TIME_H */
