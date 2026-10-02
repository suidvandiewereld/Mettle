/*
 * C scale-i32 microbenchmark - counterpart to scale_i32.mettle
 *
 * dst[i] = src[i] * 3 + 7 over 65536 int32 values, 200 passes.
 */

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <inttypes.h>

#include "../bench_time.h"

static volatile int64_t g_bench_sink;

static int64_t scale_copy(const int32_t *src, int32_t *dst, int64_t n) {
    int64_t sum = 0;
    for (int64_t i = 0; i < n; i++) {
        int32_t v = src[i] * 3 + 7;
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
        src[i] = (int32_t)((i * 29 + 3) % 521);
    }

    printf("Scale i32: %d values x %d passes\n", (int)n, passes);

    uint64_t t0 = bench_time_us();
    int64_t bench_sum = 0;
    for (int p = 0; p < passes; p++) {
        bench_sum += scale_copy(src, dst, n);
    }
    uint64_t elapsed_us = bench_time_us() - t0;

    g_bench_sink ^= bench_sum;

    printf("Bench sum = %" PRId64 "\n", bench_sum);
    printf("Time: %" PRIu64 " us\n", elapsed_us);

    free(src);
    free(dst);
    return 0;
}
