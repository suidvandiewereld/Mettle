/*
 * C Grep benchmark - counterpart to grep.mettle
 *
 * Counts lines containing "ERROR" in a 1 MiB log buffer, 200 passes.
 * Uses the same uint64 load + 5-byte mask compare as grep.mettle (memcmp tail only).
 *
 * Build: build.bat (or: gcc -O2 -o grep_c.exe grep.c -lkernel32)
 * Run: grep_c.exe
 */

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>

#include "../bench_time.h"

#define PATTERN "ERROR"
#define PATTERN_LEN 5
#define PATTERN_U64 0x524F525245ULL
#define PATTERN_MASK 0xFFFFFFFFFFULL
#define GREP_BUF_SIZE 1048576
#define GREP_PASSES 200

static volatile int64_t g_bench_sink;

static int pattern_matches(const unsigned char *buf, int64_t i, int64_t len) {
    if (i + PATTERN_LEN > len) {
        return 0;
    }
    if (i + 8 <= len) {
        uint64_t val;
        memcpy(&val, buf + i, sizeof(val));
        return (int)(((val & PATTERN_MASK) == PATTERN_U64));
    }
    return memcmp(buf + i, PATTERN, PATTERN_LEN) == 0;
}

__attribute__((noinline)) static int64_t grep_count(const unsigned char *buf, int64_t len) {
    int64_t count = 0;
    int found = 0;
    for (int64_t i = 0; i < len; i++) {
        int c = buf[i];
        if (c == '\n') {
            if (found) {
                count++;
            }
            found = 0;
        } else {
            if (!found && c == 'E' && pattern_matches(buf, i, len)) {
                found = 1;
            }
        }
    }
    if (found) {
        count++;
    }
    return count;
}

int main(void) {
    const int64_t buf_size = GREP_BUF_SIZE;
    unsigned char *buf = (unsigned char *)malloc((size_t)buf_size);
    if (!buf) {
        fprintf(stderr, "malloc failed\n");
        return 1;
    }

    const char *block = "INFO 12345\nERROR 12345\nINFO 12345\nINFO 12345\nINFO 1234\n";
    const size_t block_len = 55;
    for (int64_t pos = 0; pos < buf_size; pos += (int64_t)block_len) {
        size_t chunk = (size_t)(buf_size - pos);
        if (chunk > block_len) {
            chunk = block_len;
        }
        memcpy(buf + pos, block, chunk);
    }

    printf("Grep: count lines containing ERROR (1 MiB log buffer)\n");

    int64_t matches = grep_count(buf, buf_size);
    printf("Matches = %" PRId64 "\n", matches);

    const int passes = GREP_PASSES;
    printf("Benchmark: %d passes (grep_count)\n", passes);

    uint64_t t0 = bench_time_us();
    int64_t total = 0;
    for (int p = 0; p < passes; p++) {
        int64_t off = (int64_t)(p & 127);
        total += grep_count(buf + off, buf_size - off);
    }
    uint64_t elapsed_us = bench_time_us() - t0;

    g_bench_sink ^= total;

    printf("Total matches = %" PRId64 "\n", total);
    printf("Time: %" PRIu64 " us\n", elapsed_us);

    uint64_t per_pass_us = elapsed_us / (uint64_t)passes;
    printf("Per pass: ~%" PRIu64 " us\n", per_pass_us);

    free(buf);
    return 0;
}
