/*
 * C byte-hash benchmark - counterpart to byte_hash.mettle
 *
 * djb2 over a 256 KB buffer, 200 passes.
 *
 * Build: build.bat (or: gcc -O3 -o byte_hash_c.exe byte_hash.c -lkernel32)
 * Run: byte_hash_c.exe
 */

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>

#include "../bench_time.h"

static volatile int64_t g_bench_sink;

static int64_t byte_hash(const unsigned char *buf, int64_t len) {
    int64_t hash = 5381;
    for (int64_t i = 0; i < len; i++) {
        hash = hash * 33 + (int64_t)buf[i];
    }
    return hash;
}

int main(void) {
    const int64_t buf_size = 262144;
    const char *template = "a b c d e f g h ";
    const size_t template_len = 16;

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

    printf("Byte hash (djb2): 256 KB buffer\n");

    int64_t hash = byte_hash(buf, buf_size);
    printf("Hash = %" PRId64 "\n", hash);

    const int passes = 200;
    printf("Benchmark: %d passes (byte_hash)\n", passes);

    uint64_t t0 = bench_time_us();
    int64_t bench_sum = 0;
    for (int p = 0; p < passes; p++) {
        bench_sum += byte_hash(buf, buf_size);
    }
    uint64_t elapsed_us = bench_time_us() - t0;

    g_bench_sink ^= bench_sum;

    printf("Bench sum = %" PRId64 "\n", bench_sum);
    printf("Time: %" PRIu64 " us\n", elapsed_us);

    uint64_t per_pass_us = elapsed_us / (uint64_t)passes;
    printf("Per pass: ~%" PRIu64 " us\n", per_pass_us);

    free(buf);
    return 0;
}
