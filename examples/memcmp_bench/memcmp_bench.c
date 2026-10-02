/*
 * C memcmp microbenchmark - counterpart to memcmp_bench.mettle
 *
 * Byte-compare 256 KB in 4 KB chunks, 200 passes (manual loop, not libc memcmp).
 */

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>

#include "../bench_time.h"

static volatile int64_t g_bench_sink;

static int64_t compare_bytes(const unsigned char *a, const unsigned char *b, int64_t len) {
    for (int64_t i = 0; i < len; i++) {
        if (a[i] != b[i]) {
            return (int64_t)a[i] - (int64_t)b[i];
        }
    }
    return 0;
}

static int64_t compare_chunks(const unsigned char *a, const unsigned char *b,
                              int64_t total, int64_t chunk) {
    int64_t sum = 0;
    for (int64_t off = 0; off < total; off += chunk) {
        int64_t len = chunk;
        if (off + len > total) {
            len = total - off;
        }
        sum += compare_bytes(a + off, b + off, len);
    }
    return sum;
}

int main(void) {
    const int64_t buf_size = 262144;
    const int64_t chunk = 4096;
    const char *template = "0123456789abcdef";
    const size_t template_len = 16;

    unsigned char *a = (unsigned char *)malloc((size_t)buf_size);
    unsigned char *b = (unsigned char *)malloc((size_t)buf_size);
    if (!a || !b) {
        fprintf(stderr, "malloc failed\n");
        return 1;
    }

    for (int64_t pos = 0; pos < buf_size; pos += (int64_t)template_len) {
        size_t tchunk = (size_t)(buf_size - pos);
        if (tchunk > template_len) {
            tchunk = template_len;
        }
        memcpy(a + pos, template, tchunk);
        memcpy(b + pos, template, tchunk);
    }
    for (int64_t i = 0; i < buf_size; i += 8192) {
        b[i] ^= 0x01;
    }

    printf("Byte compare: 256 KB in 4 KB chunks x 200 passes\n");

    const int passes = 200;
    uint64_t t0 = bench_time_us();
    int64_t bench_sum = 0;
    for (int p = 0; p < passes; p++) {
        bench_sum += compare_chunks(a, b, buf_size, chunk);
    }
    uint64_t elapsed_us = bench_time_us() - t0;

    g_bench_sink ^= bench_sum;

    printf("Bench sum = %" PRId64 "\n", bench_sum);
    printf("Time: %" PRIu64 " us\n", elapsed_us);

    free(a);
    free(b);
    return 0;
}
