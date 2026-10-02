/*
 * C popcount microbenchmark - counterpart to popcount.mettle
 *
 * Population count over 256 KB byte buffer, 200 passes.
 */

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>

#include "../bench_time.h"

static volatile int64_t g_bench_sink;

static int popcount_byte(unsigned char b) {
    int count = 0;
    while (b) {
        count += (int)(b & 1u);
        b = (unsigned char)(b >> 1);
    }
    return count;
}

static int64_t popcount_buffer(const unsigned char *buf, int64_t len) {
    int64_t total = 0;
    for (int64_t i = 0; i < len; i++) {
        total += popcount_byte(buf[i]);
    }
    return total;
}

int main(void) {
    const int64_t buf_size = 262144;
    const char *template = "0123456789abcdef";
    const size_t template_len = 16;
    const int passes = 200;

    unsigned char *buf = (unsigned char *)malloc((size_t)buf_size);
    if (!buf) {
        fprintf(stderr, "malloc failed\n");
        return 1;
    }

    for (int64_t pos = 0; pos < buf_size; pos += (int64_t)template_len) {
        size_t chunk = (size_t)(buf_size - pos);
        if (chunk > template_len) {
            chunk = template_len;
        }
        memcpy(buf + pos, template, chunk);
    }

    printf("Popcount: 256 KB buffer x %d passes\n", passes);

    uint64_t t0 = bench_time_us();
    int64_t bench_sum = 0;
    for (int p = 0; p < passes; p++) {
        bench_sum += popcount_buffer(buf, buf_size);
    }
    uint64_t elapsed_us = bench_time_us() - t0;

    g_bench_sink ^= bench_sum;

    printf("Bench sum = %" PRId64 "\n", bench_sum);
    printf("Time: %" PRIu64 " us\n", elapsed_us);

    free(buf);
    return 0;
}
