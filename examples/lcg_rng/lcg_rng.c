/*
 * C LCG RNG microbenchmark - counterpart to lcg_rng.mettle
 *
 * Linear congruential generator, 1M iterations x 200 passes.
 */

#include <stdio.h>
#include <stdint.h>
#include <inttypes.h>

#include "../bench_time.h"

static volatile int64_t g_bench_sink;

static int64_t lcg_pass(int64_t seed, int64_t iters) {
    uint32_t state = (uint32_t)seed;
    int64_t sum = 0;
    for (int64_t i = 0; i < iters; i++) {
        state = state * 1103515245u + 12345u;
        sum += (int64_t)(state & 0x7fffffffu);
    }
    return sum;
}

int main(void) {
    const int64_t iters = 1000000;
    const int passes = 200;

    printf("LCG RNG: %" PRId64 " iterations x %d passes\n", iters, passes);

    uint64_t t0 = bench_time_us();
    int64_t bench_sum = 0;
    for (int p = 0; p < passes; p++) {
        bench_sum += lcg_pass(0xC0FFEE + p, iters);
    }
    uint64_t elapsed_us = bench_time_us() - t0;

    g_bench_sink ^= bench_sum;

    printf("Bench sum = %" PRId64 "\n", bench_sum);
    printf("Time: %" PRIu64 " us\n", elapsed_us);

    return 0;
}
