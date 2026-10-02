/*
 * C matrix-multiply benchmark - counterpart to matrix_mul.mettle
 *
 * Build: build.bat (or: gcc -O3 -o matrix_mul_c.exe matrix_mul.c -lkernel32)
 */

#include <stdio.h>
#include <stdint.h>
#include <inttypes.h>

#include "../bench_time.h"

#define N 32
#define N2 (N * N)

static volatile int64_t g_bench_sink;

static int mat_idx(int row, int col) {
    return row * N + col;
}

static void fill_matrix(int32_t *m, int seed) {
    for (int row = 0; row < N; row++) {
        for (int col = 0; col < N; col++) {
            m[mat_idx(row, col)] = (int32_t)((row * 131 + col * 17 + seed) % 251);
        }
    }
}

static void matmul(const int32_t *a, const int32_t *b, int32_t *c) {
    for (int row = 0; row < N; row++) {
        for (int col = 0; col < N; col++) {
            int32_t sum = 0;
            for (int k = 0; k < N; k++) {
                sum += a[mat_idx(row, k)] * b[mat_idx(k, col)];
            }
            c[mat_idx(row, col)] = sum;
        }
    }
}

static int64_t trace_matrix(const int32_t *m) {
    int64_t sum = 0;
    for (int i = 0; i < N; i++) {
        sum += (int64_t)m[mat_idx(i, i)];
    }
    return sum;
}

int main(void) {
    const int passes = 200;
    int32_t a[N2];
    int32_t b[N2];
    int32_t c[N2];

    fill_matrix(a, 3);
    fill_matrix(b, 7);

    printf("Matrix multiply: 32x32 int32 (naive)\n");

    matmul(a, b, c);
    int64_t check = trace_matrix(c);
    printf("Trace = %" PRId64 "\n", check);
    printf("Benchmark: %d passes (matmul)\n", passes);

    uint64_t t0 = bench_time_us();
    int64_t bench_sum = 0;
    for (int p = 0; p < passes; p++) {
        matmul(a, b, c);
        bench_sum += trace_matrix(c);
    }
    uint64_t elapsed_us = bench_time_us() - t0;

    g_bench_sink ^= bench_sum;

    printf("Bench sum = %" PRId64 "\n", bench_sum);
    printf("Time: %" PRIu64 " us\n", elapsed_us);
    printf("Per pass: ~%" PRIu64 " us\n", elapsed_us / (uint64_t)passes);

    return 0;
}
