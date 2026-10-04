#include "msl_interp_internal.h"
#include "../../src/runtime/metal_provider.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DEVICE_FILL 0xcdcdcdcdu

typedef struct {
  size_t passed;
  size_t failed;
  size_t skipped;
} Tally;

static char *case_text(const MslCase *tc) {
  size_t head = tc->full ? 0 : strlen(msl_test_header);
  size_t body = strlen(tc->body);
  char *text = (char *)malloc(head + body + 4);
  if (text == NULL) {
    return NULL;
  }
  memcpy(text, msl_test_header, head);
  memcpy(text + head, tc->body, body);
  if (tc->full) {
    text[head + body] = 0;
  } else {
    memcpy(text + head + body, "}\n", 3);
  }
  return text;
}

static const char *skip_reason(const MslCase *tc, int64_t simd_width) {
  if (tc->device_skip != NULL) {
    return tc->device_skip;
  }
  if (tc->error != NULL) {
    return "checks an interpreter diagnostic";
  }
  if (tc->log != NULL) {
    return "checks interpreter log text";
  }
  if (tc->spurious) {
    return "injects spurious CAS failures";
  }
  if (tc->simd_width != 0 && (int64_t)tc->simd_width != simd_width) {
    return "needs another SIMD width";
  }
  if (tc->expect_count == 0) {
    return "has no expected output";
  }
  return NULL;
}

static int64_t ordered_float(uint32_t bits) {
  return (bits & 0x80000000u) ? -(int64_t)(bits & 0x7fffffffu) : (int64_t)bits;
}

static int close_enough(const MslCase *tc, uint32_t got, uint32_t want) {
  int64_t gap;
  if (got == want) {
    return 1;
  }
  if (tc->device_ulp == 0) {
    return 0;
  }
  gap = ordered_float(got) - ordered_float(want);
  return gap >= -(int64_t)tc->device_ulp && gap <= (int64_t)tc->device_ulp;
}

static uint32_t dim(uint32_t value) {
  return value == 0 ? 1u : value;
}

static const char *dump_case;

static int run_on_device(MettleMetal *metal, const MslCase *tc, char *error, size_t error_size) {
  char *text = case_text(tc);
  uint32_t *input = (uint32_t *)calloc(tc->input_count + 1, sizeof(uint32_t));
  uint32_t *expect = (uint32_t *)calloc(tc->expect_count + 1, sizeof(uint32_t));
  uint64_t out_address = mettle_metal_alloc(metal, (tc->expect_count + 1) * sizeof(uint32_t));
  uint64_t in_address = mettle_metal_alloc(metal, (tc->input_count + 1) * sizeof(uint32_t));
  uint32_t *out = out_address ? (uint32_t *)mettle_metal_host(metal, out_address) : NULL;
  uint32_t *in = in_address ? (uint32_t *)mettle_metal_host(metal, in_address) : NULL;
  uint32_t grid[3];
  uint32_t block[3];
  uint8_t args[16];
  int ok = 0;
  size_t i;
  if (text == NULL || input == NULL || expect == NULL || out == NULL || in == NULL) {
    snprintf(error, error_size, "out of memory");
    goto done;
  }
  if (tc->input != NULL) {
    memcpy(input, tc->input, tc->input_count * sizeof(uint32_t));
  }
  if (tc->expect != NULL) {
    memcpy(expect, tc->expect, tc->expect_count * sizeof(uint32_t));
  }
  if (tc->prepare != NULL) {
    tc->prepare(input, expect);
  }
  memcpy(in, input, tc->input_count * sizeof(uint32_t));
  for (i = 0; i < tc->expect_count; i++) {
    out[i] = tc->initial != NULL ? tc->initial[i] : DEVICE_FILL;
  }
  for (i = 0; i < 3; i++) {
    grid[i] = dim(tc->grid[i]);
    block[i] = dim(tc->block[i]);
  }
  memcpy(args, &out_address, 8);
  memcpy(args + 8, &in_address, 8);
  if (!mettle_metal_load(metal, text, strlen(text), 3, 2, error, error_size) ||
      !mettle_metal_launch(metal, "t", grid, block, args, sizeof args, tc->tg_bytes, error, error_size) ||
      !mettle_metal_sync(metal, error, error_size)) {
    goto done;
  }
  if (dump_case != NULL && strcmp(dump_case, tc->name) == 0) {
    for (i = 0; i < tc->input_count; i++) {
      printf("in %u 0x%08x\n", (unsigned)i, (unsigned)in[i]);
    }
    for (i = 0; i < tc->expect_count; i++) {
      printf("out %u 0x%08x 0x%08x\n", (unsigned)i, (unsigned)out[i], (unsigned)expect[i]);
    }
  }
  if (tc->device_accept != NULL) {
    ok = tc->device_accept(out, expect);
    if (!ok) {
      snprintf(error, error_size, "the outputs are not a valid outcome in any thread order");
    }
    goto done;
  }
  ok = 1;
  for (i = 0; i < tc->expect_count; i++) {
    if (!close_enough(tc, out[i], expect[i])) {
      size_t used = strlen(error);
      if (used + 64 < error_size) {
        snprintf(error + used, error_size - used, "%sout[%u] is 0x%08x, want 0x%08x", ok ? "" : "; ", (unsigned)i, (unsigned)out[i],
                 (unsigned)expect[i]);
      }
      ok = 0;
    }
  }
done:
  if (out_address) {
    mettle_metal_free(metal, out_address);
  }
  if (in_address) {
    mettle_metal_free(metal, in_address);
  }
  free(text);
  free(input);
  free(expect);
  return ok;
}

static void run_table(MettleMetal *metal, const char *table, const MslCase *cases, size_t count, int verbose, Tally *tally) {
  int64_t simd_width = mettle_metal_property(metal, METTLE_METAL_PROPERTY_SIMD_WIDTH);
  size_t i;
  for (i = 0; i < count; i++) {
    const MslCase *tc = &cases[i];
    const char *skip = skip_reason(tc, simd_width);
    char error[1024];
    if (skip != NULL) {
      tally->skipped++;
      if (verbose) {
        printf("[SKIP] %s/%s: %s\n", table, tc->name, skip);
      }
      continue;
    }
    error[0] = 0;
    if (run_on_device(metal, tc, error, sizeof error)) {
      tally->passed++;
      printf("[PASS] %s/%s\n", table, tc->name);
    } else {
      tally->failed++;
      printf("[FAIL] %s/%s: %s\n", table, tc->name, error);
    }
  }
}

int main(int argc, char **argv) {
  char error[512];
  int verbose = argc > 1 && strcmp(argv[1], "-v") == 0;
  if (argc > 2 && strcmp(argv[1], "-dump") == 0) {
    dump_case = argv[2];
  }
  Tally tally = {0, 0, 0};
  MettleMetal *metal = mettle_metal_open(error, sizeof error);
  if (metal == NULL) {
    fprintf(stderr, "msl_device_cases: %s\n", error);
    return 2;
  }
  run_table(metal, "value", msl_value_cases, msl_value_case_count, verbose, &tally);
  run_table(metal, "collective", msl_collective_cases, msl_collective_case_count, verbose, &tally);
  printf("msl_device_cases: %u passed, %u failed, %u skipped on %s\n", (unsigned)tally.passed, (unsigned)tally.failed,
         (unsigned)tally.skipped, mettle_metal_device_name(metal));
  mettle_metal_close(metal);
  return tally.failed == 0 ? 0 : 1;
}
