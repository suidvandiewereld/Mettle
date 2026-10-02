/*
 * C prime-count benchmark - counterpart to prime_count.mettle
 *
 * Build: build.bat (or: gcc -O3 -o prime_count_c.exe prime_count.c -lkernel32)
 */

#include <stdio.h>
#include <stdint.h>
#include <inttypes.h>

#include "../bench_time.h"

static volatile int64_t g_bench_sink;

static int is_prime(int64_t n) {
    if (n < 2) {
        return 0;
    }
    for (int64_t d = 2; d * d <= n; d++) {
        if (n % d == 0) {
            return 0;
        }
    }
    return 1;
}

static int64_t count_primes(int64_t limit) {
    int64_t count = 0;
    for (int64_t n = 2; n <= limit; n++) {
        if (is_prime(n)) {
            count++;
        }
    }
    return count;
}

int main(void) {
    const int64_t limit = 50000;
    const int passes = 200;

    printf("Prime count: trial division up to 50000\n");

    int64_t primes = count_primes(limit);
    printf("Primes = %" PRId64 "\n", primes);
    printf("Benchmark: %d passes (count_primes)\n", passes);

    uint64_t t0 = bench_time_us();
    int64_t bench_sum = 0;
    for (int p = 0; p < passes; p++) {
        bench_sum += count_primes(limit);
    }
    uint64_t elapsed_us = bench_time_us() - t0;

    g_bench_sink ^= bench_sum;

    printf("Bench sum = %" PRId64 "\n", bench_sum);
    printf("Time: %" PRIu64 " us\n", elapsed_us);
    printf("Per pass: ~%" PRIu64 " us\n", elapsed_us / (uint64_t)passes);

    return 0;
}
