#include "msl_interp_internal.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MSL_TEST_FILL 0xcdcdcdcdu

const char msl_test_header[] =
  "#include <metal_stdlib>\n"
  "using namespace metal;\n"
  "struct A { device uint* out; device uint* in; };\n"
  "kernel void t(constant A& a [[buffer(0)]], uint3 tid [[thread_position_in_threadgroup]], "
  "uint3 g [[threadgroup_position_in_grid]], uint lane [[thread_index_in_simdgroup]]) {\n";

typedef struct {
  char *text;
  size_t used;
  size_t cap;
} MslTestLog;

typedef struct {
  uint32_t *input;
  uint32_t *expect;
  uint32_t *out;
  char *source;
  MslTestLog log;
} MslTestBuffers;

static void test_log(void *user, const char *text) {
  MslTestLog *log = (MslTestLog *)user;
  size_t n = strlen(text);
  if (log->used + n + 2 > log->cap) {
    size_t cap = (log->used + n + 2) * 2;
    char *grown = (char *)realloc(log->text, cap);
    if (grown == NULL) {
      return;
    }
    log->text = grown;
    log->cap = cap;
  }
  memcpy(log->text + log->used, text, n);
  log->used += n;
  log->text[log->used++] = '\n';
  log->text[log->used] = 0;
}

static char *case_source(const MslCase *tc) {
  size_t n = strlen(tc->body);
  size_t h = tc->full ? 0 : strlen(msl_test_header);
  char *source = (char *)malloc(h + n + 4);
  if (source == NULL) {
    return NULL;
  }
  memcpy(source, msl_test_header, h);
  memcpy(source + h, tc->body, n);
  if (tc->full) {
    source[h + n] = 0;
  } else {
    memcpy(source + h + n, "}\n", 3);
  }
  return source;
}

static int alloc_buffers(const MslCase *tc, MslTestBuffers *b) {
  size_t i;
  memset(b, 0, sizeof *b);
  b->input = (uint32_t *)calloc(tc->input_count + 1, sizeof(uint32_t));
  b->expect = (uint32_t *)calloc(tc->expect_count + 1, sizeof(uint32_t));
  b->out = (uint32_t *)calloc(tc->expect_count + 1, sizeof(uint32_t));
  b->source = case_source(tc);
  if (b->input == NULL || b->expect == NULL || b->out == NULL || b->source == NULL) {
    return 0;
  }
  if (tc->input != NULL) {
    memcpy(b->input, tc->input, tc->input_count * sizeof(uint32_t));
  }
  if (tc->expect != NULL) {
    memcpy(b->expect, tc->expect, tc->expect_count * sizeof(uint32_t));
  }
  for (i = 0; i < tc->expect_count; i++) {
    b->out[i] = tc->initial != NULL ? tc->initial[i] : MSL_TEST_FILL;
  }
  if (tc->prepare != NULL) {
    tc->prepare(b->input, b->expect);
  }
  return 1;
}

static void free_buffers(MslTestBuffers *b) {
  free(b->input);
  free(b->expect);
  free(b->out);
  free(b->source);
  free(b->log.text);
}

static uint32_t dim(uint32_t value) {
  return value == 0 ? 1u : value;
}

static void fill_dispatch(const MslCase *tc, MslTestBuffers *b, const uint8_t *args, MslDispatch *d) {
  int i;
  memset(d, 0, sizeof *d);
  d->kernel = "t";
  for (i = 0; i < 3; i++) {
    d->grid[i] = dim(tc->grid[i]);
    d->block[i] = dim(tc->block[i]);
  }
  d->args = args;
  d->args_size = 16;
  d->threadgroup_bytes = tc->tg_bytes;
  d->simd_width = tc->simd_width;
  d->spurious_cas = tc->spurious;
  d->log = test_log;
  d->log_user = &b->log;
}

static int outcome_error(const MslCase *tc, const char *message, char *error, size_t error_size) {
  if (tc->error != NULL && strstr(message, tc->error) != NULL) {
    return 1;
  }
  if (tc->error != NULL) {
    msl_format_error(error, error_size, "test '%s': expected an error containing '%s' but got: %s", tc->name, tc->error, message);
  } else {
    msl_format_error(error, error_size, "test '%s' failed: %s", tc->name, message);
  }
  return 0;
}

static int compare_outputs(const MslCase *tc, const MslTestBuffers *b, char *error, size_t error_size) {
  size_t i;
  for (i = 0; i < tc->expect_count; i++) {
    if (b->out[i] != b->expect[i]) {
      msl_format_error(error, error_size, "test '%s': out[%u] is 0x%08x, expected 0x%08x", tc->name, (unsigned)i, (unsigned)b->out[i],
                       (unsigned)b->expect[i]);
      return 0;
    }
  }
  if (tc->device_accept != NULL && !tc->device_accept(b->out, b->expect)) {
    msl_format_error(error, error_size, "test '%s': its device acceptance check rejects the interpreter's outcome", tc->name);
    return 0;
  }
  if (tc->log != NULL && (b->log.text == NULL || strcmp(b->log.text, tc->log) != 0)) {
    msl_format_error(error, error_size, "test '%s': log was \"%s\", expected \"%s\"", tc->name, b->log.text != NULL ? b->log.text : "",
                     tc->log);
    return 0;
  }
  return 1;
}

static int dispatch_case(const MslCase *tc, MslTestBuffers *b, MslProgram *prog, char *error, size_t error_size) {
  char message[1024];
  uint8_t args[16];
  uint64_t out_address = msl_device_register(prog, b->out, tc->expect_count * sizeof(uint32_t));
  uint64_t in_address = msl_device_register(prog, b->input, tc->input_count * sizeof(uint32_t));
  MslDispatch dispatch;
  uint64_t old_budget = msl_set_step_budget(tc->budget);
  int ok;
  memcpy(args, &out_address, 8);
  memcpy(args + 8, &in_address, 8);
  fill_dispatch(tc, b, args, &dispatch);
  ok = msl_dispatch(prog, &dispatch, message, sizeof message);
  msl_set_step_budget(old_budget);
  if (!ok) {
    return outcome_error(tc, message, error, error_size);
  }
  if (tc->error != NULL) {
    msl_format_error(error, error_size, "test '%s': expected an error containing '%s' but the dispatch succeeded", tc->name, tc->error);
    return 0;
  }
  return compare_outputs(tc, b, error, error_size);
}

static int run_case(const MslCase *tc, char *error, size_t error_size) {
  MslTestBuffers b;
  MslProgram *prog;
  char message[1024];
  int ok;
  if (!alloc_buffers(tc, &b)) {
    free_buffers(&b);
    msl_format_error(error, error_size, "test '%s': out of memory", tc->name);
    return 0;
  }
  prog = msl_program_load(b.source, strlen(b.source), message, sizeof message);
  if (prog == NULL) {
    ok = outcome_error(tc, message, error, error_size);
  } else {
    ok = dispatch_case(tc, &b, prog, error, error_size);
    msl_program_free(prog);
  }
  free_buffers(&b);
  return ok;
}

static int run_table(const MslCase *cases, size_t count, size_t *passed, char *error, size_t error_size) {
  size_t i;
  for (i = 0; i < count; i++) {
    if (!run_case(&cases[i], error, error_size)) {
      return 0;
    }
    (*passed)++;
  }
  return 1;
}

int msl_selftest(char *error, size_t error_size) {
  size_t passed = 0;
  if (error != NULL && error_size > 0) {
    error[0] = 0;
  }
  if (!run_table(msl_value_cases, msl_value_case_count, &passed, error, error_size) ||
      !run_table(msl_collective_cases, msl_collective_case_count, &passed, error, error_size) ||
      !run_table(msl_error_cases, msl_error_case_count, &passed, error, error_size)) {
    return 0;
  }
  msl_format_error(error, error_size, "selftest: %u tests passed", (unsigned)passed);
  return 1;
}
