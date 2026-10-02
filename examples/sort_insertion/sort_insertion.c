/*
 * C insertion-sort benchmark - counterpart to sort_insertion.mettle
 *
 * Build: build.bat (or: gcc -O3 -o sort_insertion_c.exe sort_insertion.c -lkernel32)
 */

#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <inttypes.h>

#include "../bench_time.h"

#define DATA_LEN 512

static volatile int64_t g_bench_sink;

static void fill_data(int32_t *data, int seed) {
    for (int i = 0; i < DATA_LEN; i++) {
        data[i] = (int32_t)((i * 1103515245 + seed + 12345) % 1024);
    }
}

static void insertion_sort(int32_t *data, int len) {
    for (int i = 1; i < len; i++) {
        int32_t key = data[i];
        int j = i - 1;
        while (j >= 0 && data[j] > key) {
            data[j + 1] = data[j];
            j--;
        }
        data[j + 1] = key;
    }
}

static int64_t sum_array(const int32_t *data, int len) {
    int64_t sum = 0;
    for (int i = 0; i < len; i++) {
        sum += (int64_t)data[i];
    }
    return sum;
}

int main(void) {
    const int passes = 200;
    int32_t data[DATA_LEN];
    int32_t scratch[DATA_LEN];

    fill_data(data, 42);

    printf("Insertion sort: 512 int32 values\n");

    memcpy(scratch, data, sizeof(data));
    insertion_sort(scratch, DATA_LEN);
    int64_t check = sum_array(scratch, DATA_LEN);
    printf("Sorted sum = %" PRId64 "\n", check);
    printf("Benchmark: %d passes (insertion_sort)\n", passes);

    uint64_t t0 = bench_time_us();
    int64_t bench_sum = 0;
    for (int p = 0; p < passes; p++) {
        memcpy(scratch, data, sizeof(data));
        insertion_sort(scratch, DATA_LEN);
        bench_sum += sum_array(scratch, DATA_LEN);
    }
    uint64_t elapsed_us = bench_time_us() - t0;

    g_bench_sink ^= bench_sum;

    printf("Bench sum = %" PRId64 "\n", bench_sum);
    printf("Time: %" PRIu64 " us\n", elapsed_us);
    printf("Per pass: ~%" PRIu64 " us\n", elapsed_us / (uint64_t)passes);

    return 0;
}
