/*
 * C Collatz benchmark - counterpart to collatz.mettle
 *
 * Collatz sequence: n -> n/2 (if even) or 3n+1 (if odd), until n==1.
 * Counts steps for n=1..100000, sums them. Benchmarks 10 full passes.
 *
 * Build: build.bat (or: gcc -O2 -o collatz_c.exe collatz.c -lkernel32)
 * Run: collatz_c.exe
 */

#include <stdio.h>
#include <stdint.h>
#include <inttypes.h>

#include "../bench_time.h"

/* Returns number of steps to reach 1 from n (n >= 1) */
static int64_t collatz_steps(int64_t n) {
    int64_t count = 0;
    int64_t x = n;
    while (x > 1) {
        if (x % 2 == 0) {
            x = x / 2;
        } else {
            x = 3 * x + 1;
        }
        count++;
    }
    return count;
}

int main(void) {
    printf("Collatz: sum of steps for n=1..100000\n");

    int64_t sum = 0;
    for (int64_t n = 1; n <= 100000; n++) {
        sum += collatz_steps(n);
    }
    printf("Sum (1..100000) = %" PRId64 "\n", sum);

    const int passes = 10;
    printf("Benchmark: 10 passes (sum steps 1..100000 each)\n");

    uint64_t t0 = bench_time_us();
    int64_t bench_sum = 0;
    for (int p = 0; p < passes; p++) {
        for (int64_t i = 1; i <= 100000; i++) {
            bench_sum += collatz_steps(i);
        }
    }
    uint64_t elapsed_us = bench_time_us() - t0;

    printf("Bench sum = %" PRId64 "\n", bench_sum);
    printf("Time: %" PRIu64 " us\n", elapsed_us);

    uint64_t per_pass_us = elapsed_us / (uint64_t)passes;
    printf("Per pass: ~%" PRIu64 " us\n", per_pass_us);

    return 0;
}
