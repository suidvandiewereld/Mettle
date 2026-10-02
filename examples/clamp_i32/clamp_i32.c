/*
 * C clamp-i32 microbenchmark - counterpart to clamp_i32.mettle
 *
 * dst[i] = clamp(src[i], -100, 100) over 65536 int32 values, 200 passes.
 */

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <inttypes.h>

#include "../bench_time.h"

static volatile int64_t g_bench_sink;

static int32_t clamp_i32(int32_t v, int32_t lo, int32_t hi) {
    if (v < lo) {
        return lo;
    }
    if (v > hi) {
        return hi;
    }
    return v;
}

static int64_t clamp_copy(const int32_t *src, int32_t *dst, int64_t n) {
    int64_t sum = 0;
    for (int64_t i = 0; i < n; i++) {
        int32_t v = clamp_i32(src[i], -100, 100);
        dst[i] = v;
        sum += (int64_t)v;
    }
    return sum;
}

int main(void) {
    const int64_t n = 65536;
    const int passes = 200;

    int32_t *src = (int32_t *)malloc((size_t)(n * (int64_t)sizeof(int32_t)));
    int32_t *dst = (int32_t *)malloc((size_t)(n * (int64_t)sizeof(int32_t)));
    if (!src || !dst) {
        fprintf(stderr, "malloc failed\n");
        return 1;
    }

    for (int64_t i = 0; i < n; i++) {
        src[i] = (int32_t)((i * 37 + 11) % 997 - 498);
    }

    printf("Clamp i32: %d values x %d passes\n", (int)n, passes);

    uint64_t t0 = bench_time_us();
    int64_t bench_sum = 0;
    for (int p = 0; p < passes; p++) {
        bench_sum += clamp_copy(src, dst, n);
    }
    uint64_t elapsed_us = bench_time_us() - t0;

    g_bench_sink ^= bench_sum;

    printf("Bench sum = %" PRId64 "\n", bench_sum);
    printf("Time: %" PRIu64 " us\n", elapsed_us);

    free(src);
    free(dst);
    return 0;
}
