/*
 * C Sum-of-Squares benchmark - counterpart to sum_squares.mettle
 *
 * Computes 1² + 2² + ... + n² for n=100000.
 * Benchmarks 50 full passes.
 *
 * Build: build.bat (or: gcc -O2 -o sum_squares_c.exe sum_squares.c -lkernel32)
 * Run: sum_squares_c.exe
 */

#include <stdio.h>
#include <stdint.h>
#include <inttypes.h>

#include "../bench_time.h"

static int64_t sum_squares(int64_t n) {
    int64_t sum = 0;
    for (int64_t i = 1; i <= n; i++) {
        sum += i * i;
    }
    return sum;
}

int main(void) {
    printf("Sum of squares: 1²+2²+...+100000²\n");

    int64_t result = sum_squares(100000);
    printf("Sum = %" PRId64 "\n", result);

    const int passes = 200;
    printf("Benchmark: 200 passes (sum_squares 100000 each)\n");

    volatile int64_t bench_n = 100000;

    uint64_t t0 = bench_time_us();
    int64_t bench_sum = 0;
    for (int p = 0; p < passes; p++) {
        bench_sum += sum_squares(bench_n);
    }
    uint64_t elapsed_us = bench_time_us() - t0;

    printf("Bench sum = %" PRId64 "\n", bench_sum);
    printf("Time: %" PRIu64 " us\n", elapsed_us);

    uint64_t per_pass_us = elapsed_us / (uint64_t)passes;
    printf("Per pass: ~%" PRIu64 " us\n", per_pass_us);

    return 0;
}
