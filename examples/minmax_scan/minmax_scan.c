/*
 * C minmax-scan microbenchmark - counterpart to minmax_scan.mettle
 *
 * Min/max scan over 65536 int32 values, 200 passes.
 */

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <inttypes.h>

#include "../bench_time.h"

static volatile int64_t g_bench_sink;

static int64_t minmax_scan(const int32_t *arr, int64_t n) {
    int32_t minv = arr[0];
    int32_t maxv = arr[0];
    for (int64_t i = 1; i < n; i++) {
        int32_t v = arr[i];
        if (v < minv) {
            minv = v;
        }
        if (v > maxv) {
            maxv = v;
        }
    }
    return (int64_t)minv + (int64_t)maxv;
}

int main(void) {
    const int64_t n = 65536;
    const int passes = 200;

    int32_t *arr = (int32_t *)malloc((size_t)(n * (int64_t)sizeof(int32_t)));
    if (!arr) {
        fprintf(stderr, "malloc failed\n");
        return 1;
    }

    for (int64_t i = 0; i < n; i++) {
        arr[i] = (int32_t)((i * 23 + 7) % 997);
    }

    printf("Min/max scan: %d int32 values x %d passes\n", (int)n, passes);

    uint64_t t0 = bench_time_us();
    int64_t bench_sum = 0;
    for (int p = 0; p < passes; p++) {
        bench_sum += minmax_scan(arr, n);
    }
    uint64_t elapsed_us = bench_time_us() - t0;

    g_bench_sink ^= bench_sum;

    printf("Bench sum = %" PRId64 "\n", bench_sum);
    printf("Time: %" PRIu64 " us\n", elapsed_us);

    free(arr);
    return 0;
}
