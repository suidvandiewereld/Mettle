/*
 * C binary-search microbenchmark - counterpart to binary_search.mettle
 *
 * Sorted int32[65536], 50000 pseudo-random lookups x 200 passes.
 */

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <inttypes.h>

#include "../bench_time.h"

static volatile int64_t g_bench_sink;

static int32_t lower_bound(const int32_t *arr, int64_t n, int32_t key) {
    int64_t lo = 0;
    int64_t hi = n;
    while (lo < hi) {
        int64_t mid = lo + ((hi - lo) >> 1);
        if (arr[mid] < key) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    return (lo < n) ? arr[lo] : -1;
}

static int64_t search_many(const int32_t *arr, int64_t n, int64_t queries) {
    int64_t sum = 0;
    for (int64_t q = 0; q < queries; q++) {
        int32_t key = (int32_t)(((q * 1103515245LL + 12345LL) % n) * 4 + 1);
        sum += lower_bound(arr, n, key);
    }
    return sum;
}

int main(void) {
    const int64_t n = 65536;
    const int64_t queries = 50000;
    const int passes = 200;

    int32_t *arr = (int32_t *)malloc((size_t)(n * (int64_t)sizeof(int32_t)));
    if (!arr) {
        fprintf(stderr, "malloc failed\n");
        return 1;
    }

    for (int64_t i = 0; i < n; i++) {
        arr[i] = (int32_t)(i * 4 + 1);
    }

    printf("Binary search: %d elements, %" PRId64 " queries x %d passes\n",
           (int)n, queries, passes);

    uint64_t t0 = bench_time_us();
    int64_t bench_sum = 0;
    for (int p = 0; p < passes; p++) {
        bench_sum += search_many(arr, n, queries);
    }
    uint64_t elapsed_us = bench_time_us() - t0;

    g_bench_sink ^= bench_sum;

    printf("Bench sum = %" PRId64 "\n", bench_sum);
    printf("Time: %" PRIu64 " us\n", elapsed_us);

    free(arr);
    return 0;
}
