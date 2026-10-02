/*
 * C memset microbenchmark - counterpart to memset_bench.mettle
 *
 * memset 256 KB buffer with rotating byte, 200 passes.
 */

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>

#include "../bench_time.h"

static volatile int64_t g_bench_sink;

static int64_t fill_buffer(unsigned char *buf, int64_t len, int fill) {
    memset(buf, fill, (size_t)len);
    return (int64_t)buf[0] + (int64_t)buf[len - 1] + (int64_t)fill;
}

int main(void) {
    const int64_t buf_size = 262144;

    unsigned char *buf = (unsigned char *)malloc((size_t)buf_size);
    if (!buf) {
        fprintf(stderr, "malloc failed\n");
        return 1;
    }

    printf("Memset: 256 KB buffer x 200 passes\n");

    const int passes = 200;
    uint64_t t0 = bench_time_us();
    int64_t bench_sum = 0;
    for (int p = 0; p < passes; p++) {
        bench_sum += fill_buffer(buf, buf_size, (p * 37 + 11) & 0xFF);
    }
    uint64_t elapsed_us = bench_time_us() - t0;

    g_bench_sink ^= bench_sum;

    printf("Bench sum = %" PRId64 "\n", bench_sum);
    printf("Time: %" PRIu64 " us\n", elapsed_us);

    free(buf);
    return 0;
}
