/*
 * C memcpy microbenchmark - counterpart to memcpy_bench.mettle
 *
 * Copy 256 KB src -> dst, 200 passes.
 */

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>

#include "../bench_time.h"

static volatile int64_t g_bench_sink;

static int64_t copy_buffer(unsigned char *dst, const unsigned char *src, int64_t len) {
    memcpy(dst, src, (size_t)len);
    return (int64_t)dst[len - 1] + (int64_t)src[0];
}

int main(void) {
    const int64_t buf_size = 262144;
    const char *template = "0123456789abcdef";
    const size_t template_len = 16;

    unsigned char *src = (unsigned char *)malloc((size_t)buf_size);
    unsigned char *dst = (unsigned char *)malloc((size_t)buf_size);
    if (!src || !dst) {
        fprintf(stderr, "malloc failed\n");
        return 1;
    }

    for (int64_t pos = 0; pos < buf_size; pos += (int64_t)template_len) {
        size_t chunk = (size_t)(buf_size - pos);
        if (chunk > template_len) {
            chunk = template_len;
        }
        memcpy(src + pos, template, chunk);
    }
    memset(dst, 0, (size_t)buf_size);

    printf("Memcpy: 256 KB buffer x 200 passes\n");

    const int passes = 200;
    uint64_t t0 = bench_time_us();
    int64_t bench_sum = 0;
    for (int p = 0; p < passes; p++) {
        bench_sum += copy_buffer(dst, src, buf_size);
    }
    uint64_t elapsed_us = bench_time_us() - t0;

    g_bench_sink ^= bench_sum;

    printf("Bench sum = %" PRId64 "\n", bench_sum);
    printf("Time: %" PRIu64 " us\n", elapsed_us);

    free(src);
    free(dst);
    return 0;
}
