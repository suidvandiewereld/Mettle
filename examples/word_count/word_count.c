/*
 * C Word Count benchmark - counterpart to word_count.mettle
 *
 * Parses a raw byte buffer to count words (non-empty runs of non-whitespace).
 *
 * Build: build.bat (or: gcc -O2 -o word_count_c.exe word_count.c -lkernel32)
 * Run: word_count_c.exe
 */

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>

#include "../bench_time.h"

static int is_space(unsigned char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

static int64_t word_count(const unsigned char *buf, int64_t len) {
    int64_t count = 0;
    int in_word = 0;

    for (int64_t i = 0; i < len; i++) {
        unsigned char c = buf[i];

        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
            in_word = 0;
        } else {
            if (in_word == 0) {
                count++;
            }
            in_word = 1;
        }
    }

    return count;
}

int main(void) {
    const int64_t buf_size = 262144;
    const char *template = "a b ";
    const size_t template_len = 4;

    unsigned char *buf = (unsigned char *)malloc((size_t)buf_size);
    if (!buf) {
        fprintf(stderr, "malloc failed\n");
        return 1;
    }

    for (int64_t pos = 0; pos < buf_size; ) {
        size_t chunk = (size_t)(buf_size - pos);
        if (chunk > template_len) {
            chunk = template_len;
        }
        memcpy(buf + pos, template, chunk);
        pos += (int64_t)chunk;
    }

    int64_t wc = word_count(buf, buf_size);

    const int passes = 200;
    uint64_t t0 = bench_time_us();
    int64_t total = 0;
    for (int p = 0; p < passes; p++) {
        total += word_count(buf, buf_size);
    }
    uint64_t elapsed_us = bench_time_us() - t0;

    printf("Word count: 256 KB buffer (a b pattern)\n");
    printf("Words = %" PRId64 "\n", wc);
    printf("Benchmark: 200 passes (word_count)\n");
    printf("Total words = %" PRId64 "\n", total);
    printf("Time: %" PRIu64 " us\n", elapsed_us);

    uint64_t per_pass_us = elapsed_us / (uint64_t)passes;
    printf("Per pass: ~%" PRIu64 " us\n", per_pass_us);

    free(buf);
    return 0;
}
