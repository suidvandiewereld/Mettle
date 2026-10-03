#include "msl_interp_internal.h"
#include <string.h>

#define COUNT(array) (sizeof(array) / sizeof((array)[0]))

#define P10 "(((((((((("
#define C10 "))))))))))"
#define P100 P10 P10 P10 P10 P10 P10 P10 P10 P10 P10
#define C100 C10 C10 C10 C10 C10 C10 C10 C10 C10 C10

static const uint32_t one_word[1] = {0};
static const uint32_t eight_words[8] = {0};
static const uint32_t bad_bool[1] = {2};

static const char recursion_source[] =
  "#include <metal_stdlib>\n"
  "using namespace metal;\n"
  "static uint f(uint x) {\n"
  "  return f(x);\n"
  "}\n"
  "kernel void t(uint3 tid [[thread_position_in_threadgroup]]) {\n"
  "  uint y = f(tid.x);\n"
  "}\n";

static const char missing_return_source[] =
  "#include <metal_stdlib>\n"
  "using namespace metal;\n"
  "static uint f(uint x) {\n"
  "  if (x > 0u) {\n"
  "    return 1u;\n"
  "  }\n"
  "}\n"
  "kernel void t(uint3 tid [[thread_position_in_threadgroup]]) {\n"
  "  uint y = f(tid.x);\n"
  "}\n";

static const char dangling_source[] =
  "#include <metal_stdlib>\n"
  "using namespace metal;\n"
  "static thread uint* leak(uint x) {\n"
  "  uint y = x;\n"
  "  return &y;\n"
  "}\n"
  "kernel void t(uint3 tid [[thread_position_in_threadgroup]]) {\n"
  "  thread uint* p = leak(3u);\n"
  "  uint v = *p;\n"
  "}\n";

static const char static_assert_source[] =
  "#include <metal_stdlib>\n"
  "using namespace metal;\n"
  "struct alignas(4) R { uchar bytes[6]; };\n"
  "static_assert(sizeof(R) == 6, \"record layout\");\n";

static const char max_threads_source[] =
  "#include <metal_stdlib>\n"
  "using namespace metal;\n"
  "[[max_total_threads_per_threadgroup(32)]]\n"
  "kernel void t(uint3 tid [[thread_position_in_threadgroup]]) {\n"
  "  return;\n"
  "}\n";

static const char missing_include_source[] =
  "using namespace metal;\n"
  "kernel void t(uint3 tid [[thread_position_in_threadgroup]]) {\n"
  "}\n";

static const char missing_args_source[] =
  "#include <metal_stdlib>\n"
  "using namespace metal;\n"
  "struct B { device uint* out; device uint* in; int n; };\n"
  "kernel void t(constant B& b [[buffer(0)]]) {\n"
  "}\n";

static const char helper_divergence_source[] =
  "#include <metal_stdlib>\n"
  "using namespace metal;\n"
  "static uint warp_total(uint v) {\n"
  "  return simd_sum(v);\n"
  "}\n"
  "kernel void t(uint lane [[thread_index_in_simdgroup]]) {\n"
  "  uint s = 0u;\n"
  "  if (lane < 16u) {\n"
  "    s = warp_total(1u);\n"
  "  } else {\n"
  "    s = warp_total(2u);\n"
  "  }\n"
  "}\n";

static const char helper_barrier_source[] =
  "#include <metal_stdlib>\n"
  "using namespace metal;\n"
  "static void sync_all() {\n"
  "  threadgroup_barrier(mem_flags::mem_threadgroup);\n"
  "}\n"
  "kernel void t(uint3 tid [[thread_position_in_threadgroup]]) {\n"
  "  if (tid.x < 32u) {\n"
  "    sync_all();\n"
  "  } else {\n"
  "    sync_all();\n"
  "  }\n"
  "}\n";

static const char cross_thread_body[] =
  "  threadgroup alignas(16) ulong slot[1];\n"
  "  uint mine = 7u;\n"
  "  if (tid.x == 0u) {\n"
  "    *(threadgroup ulong*)((threadgroup uchar*)slot) = reinterpret_cast<ulong>(&mine);\n"
  "  }\n"
  "  threadgroup_barrier(mem_flags::mem_threadgroup);\n"
  "  if (tid.x == 1u) {\n"
  "    thread uint* p = reinterpret_cast<thread uint*>(*(threadgroup ulong*)((threadgroup uchar*)slot));\n"
  "    uint v = *p;\n"
  "  }\n"
  "  threadgroup_barrier(mem_flags::mem_threadgroup);\n";

static const char space_mismatch_body[] =
  "  threadgroup alignas(16) uint tg[4];\n"
  "  ulong bits = reinterpret_cast<ulong>((threadgroup uchar*)tg);\n"
  "  device uint* d = reinterpret_cast<device uint*>(bits);\n"
  "  *d = 1u;\n";

const MslCase msl_error_cases[] = {
  {.name = "signed_add_overflow", .body = "  int x = 2147483647;\n  int y = x + 1;\n", .error = "kernel 't' line 6, threadgroup (0,0,0) thread (0,0,0): signed int overflow in 2147483647 + 1"},
  {.name = "signed_sub_overflow", .body = "  int x = (-2147483647 - 1);\n  int y = x - 1;\n", .error = "signed int overflow"},
  {.name = "signed_mul_overflow_long", .body = "  long x = 4611686018427387904l;\n  long y = x * 2l;\n", .error = "signed long overflow"},
  {.name = "signed_negation_overflow", .body = "  int x = (-2147483647 - 1);\n  int y = -x;\n", .error = "negation overflows"},
  {.name = "shift_count_too_large", .body = "  uint s = 32u;\n  uint y = 1u << s;\n", .error = "shift count 32"},
  {.name = "shift_count_negative", .body = "  int s = (-1);\n  uint y = 1u << s;\n", .error = "negative shift count"},
  {.name = "long_shift_count", .body = "  ulong s = 1ul;\n  ulong y = s >> 64u;\n", .error = "64-bit operand width"},
  {.name = "signed_left_shift_overflow", .body = "  int x = 3;\n  int y = x << 31;\n", .error = "overflows"},
  {.name = "left_shift_negative", .body = "  int x = (-1);\n  int y = x << 1;\n", .error = "left shift of negative"},
  {.name = "remainder_negative", .body = "  int x = (-7);\n  int y = x % 2;\n", .error = "negative operand"},
  {.name = "division_by_zero", .body = "  int z = 0;\n  int y = 5 / z;\n", .error = "division by zero"},
  {.name = "unsigned_remainder_by_zero", .body = "  uint z = 0u;\n  uint y = 5u % z;\n", .error = "remainder by zero"},
  {.name = "division_overflow", .body = "  int x = (-2147483647 - 1);\n  int y = x / (-1);\n", .error = "division overflows"},
  {.name = "float_to_int_range", .body = "  float f = 3.0e9f;\n  int y = (int)f;\n", .error = "out of range for int"},
  {.name = "float_to_uint_negative", .body = "  float f = (-1.0f);\n  uint y = (uint)f;\n", .error = "out of range for uint"},
  {.name = "nan_to_int", .body = "  float z = 0.0f;\n  int y = (int)(z / z);\n", .error = "out of range"},
  {.name = "device_out_of_bounds", .body = "  *(a.out + 1) = 1u;\n", .initial = one_word, .expect_count = 1,
   .error = "kernel 't' line 5, threadgroup (0,0,0) thread (0,0,0): out-of-bounds access: 4 bytes at offset 4 of a 4-byte device object"},
  {.name = "device_negative_offset", .body = "  uint v = *(a.in - 1);\n", .input = one_word, .input_count = 1,
   .error = "out-of-bounds"},
  {.name = "misaligned_access", .body = "  *(device uint*)((device uchar*)a.out + 2l) = 1u;\n", .initial = eight_words, .expect_count = 2,
   .error = "misaligned"},
  {.name = "misaligned_vector", .body = "  float4 v = *(device float4*)(a.in + 1);\n", .input = eight_words, .input_count = 8,
   .error = "misaligned"},
  {.name = "write_constant_memory", .body = "  a.out = a.in;\n", .error = "line 5: write through a constant pointer"},
  {.name = "address_space_cast", .body = "  threadgroup alignas(16) uint tg[4];\n  device uint* d = (device uint*)tg;\n",
   .error = "changes the address space"},
  {.name = "address_space_mismatch", .body = space_mismatch_body, .error = "pointer to threadgroup memory used as a device pointer"},
  {.name = "uninitialized_local", .body = "  ulong2 v;\n", .error = "must be initialized"},
  {.name = "uninitialized_matrix_store", .body = "  simdgroup_float8x8 m;\n  simdgroup_store(m, (device float*)a.out, 8ul, ulong2(0ul, 0ul), false);\n",
   .block = {32}, .initial = one_word, .expect_count = 1, .error = "read before every element"},
  {.name = "uninitialized_thread_array", .body = "  thread alignas(16) uint arr[4];\n  uint v = *(thread uint*)((thread uchar*)arr + 4l);\n",
   .error = "read of uninitialized thread memory"},
  {.name = "uninitialized_threadgroup", .body = "  threadgroup alignas(16) uint tg[4];\n  uint v = *(threadgroup uint*)((threadgroup uchar*)tg);\n",
   .error = "uninitialized threadgroup memory"},
  {.name = "bool_not_zero_or_one", .body = "  bool b = *(device bool*)((device uchar*)a.in);\n", .input = bad_bool, .input_count = 1,
   .error = "neither 0 nor 1"},
  {.name = "shuffle_inactive_lane", .body = "  uint v = simd_shuffle(tid.x, (ushort)20);\n", .block = {40}, .error = "which is not active"},
  {.name = "shuffle_out_of_range", .body = "  uint v = simd_shuffle(tid.x, (ushort)40);\n", .block = {32}, .error = "outside the 32-wide"},
  {.name = "broadcast_not_uniform", .body = "  uint v = simd_broadcast(tid.x, (ushort)lane);\n", .block = {32}, .error = "differs"},
  {.name = "different_barriers", .body =
   "  if (tid.x < 32u) {\n    threadgroup_barrier(mem_flags::mem_threadgroup);\n  } else {\n    threadgroup_barrier(mem_flags::mem_threadgroup);\n  }\n",
   .block = {64}, .error = "different barriers"},
  {.name = "barrier_not_reached", .body =
   "  if (tid.x < 16u) {\n    threadgroup_barrier(mem_flags::mem_threadgroup);\n  } else {\n    uint s = simd_sum(1u);\n  }\n",
   .block = {32}, .error = "not reached by every live thread"},
  {.name = "simd_divergence", .body =
   "  uint s = 0u;\n  if (tid.x < 16u) {\n    s = simd_sum(1u);\n  } else {\n    s = simd_max(2u);\n  }\n",
   .block = {32}, .error = "SIMD-group lanes diverged"},
  {.name = "simd_divergence_through_helper", .body = helper_divergence_source, .full = 1, .block = {32},
   .error = "reached through different calls"},
  {.name = "barrier_through_divergent_calls", .body = helper_barrier_source, .full = 1, .block = {64},
   .error = "different barriers"},
  {.name = "matrix_lanes_disagree", .body =
   "  simdgroup_float8x8 m;\n  simdgroup_load(m, (device float*)a.in, 8ul, ulong2((ulong)lane, 0ul), false);\n",
   .block = {32}, .input_count = 128, .error = "different pointer, stride, origin"},
  {.name = "matrix_out_of_bounds", .body =
   "  simdgroup_float8x8 m;\n  simdgroup_load(m, (device float*)a.in, 8ul, ulong2(0ul, 0ul), false);\n",
   .block = {32}, .input_count = 63, .error = "out-of-bounds"},
  {.name = "cross_thread_memory", .body = cross_thread_body, .block = {2}, .error = "thread memory of another thread"},
  {.name = "dangling_pointer", .body = dangling_source, .full = 1, .error = "dangling"},
  {.name = "missing_return", .body = missing_return_source, .full = 1, .error = "reached its end without returning"},
  {.name = "recursion", .body = recursion_source, .full = 1, .error = "recursive"},
  {.name = "step_budget", .body = "  while (true) {\n  }\n", .budget = 100000, .error = "step budget"},
  {.name = "mixed_operand_types", .body = "  uint x = 1u;\n  int y = 2;\n  uint z = x + (uint)y;\n  uint w = x + y;\n",
   .error = "line 8: operands of '+' have different types 'uint' and 'int'"},
  {.name = "mixed_comparison_types", .body = "  uint x = 1u;\n  bool b = x < 2;\n", .error = "different types"},
  {.name = "implicit_conversion", .body = "  uint x = 5;\n", .error = "line 5: initialization of 'uint' from 'int' needs an explicit conversion"},
  {.name = "implicit_float_conversion", .body = "  float f = 0.0f;\n  f = 1;\n", .error = "needs an explicit conversion"},
  {.name = "double_literal", .body = "  float f = 1.0;\n", .error = "line 5: floating literal without 'f' suffix"},
  {.name = "double_type", .body = "  double d = 1.0f;\n", .error = "double is outside the dialect"},
  {.name = "as_type_size_mismatch", .body = "  uint x = as_type<uint>(1ul);\n", .error = "equal size"},
  {.name = "condition_not_bool", .body = "  uint x = 1u;\n  if (x) {\n  }\n", .error = "not bool"},
  {.name = "vote_needs_cast", .body = "  ulong b = simd_ballot(true);\n", .block = {32}, .error = "needs an explicit conversion"},
  {.name = "sizeof_outside_static_assert", .body = "  uint s = (uint)sizeof(uint);\n", .error = "sizeof is only allowed"},
  {.name = "static_assert_fails", .body = static_assert_source, .full = 1, .error = "static_assert failed"},
  {.name = "unknown_identifier", .body = "  uint x = nothing;\n", .error = "unknown identifier 'nothing'"},
  {.name = "unknown_function", .body = "  float x = sqrt(2.0f);\n", .error = "unknown function 'sqrt'"},
  {.name = "max_threads_exceeded", .body = max_threads_source, .full = 1, .block = {64}, .error = "exceeds max_total_threads_per_threadgroup(32)"},
  {.name = "missing_include", .body = missing_include_source, .full = 1, .error = "missing '#include <metal_stdlib>'"},
  {.name = "argument_struct_size", .body = missing_args_source, .full = 1, .error = "argument struct 'B' is 24 bytes"},
  {.name = "nesting_limit", .body = "  uint x = " P100 P100 P100 "1u" C100 C100 C100 ";\n", .error = "nesting deeper than 256"},
  {.name = "float_literal_overflow", .body = "  float f = 1e39f;\n", .error = "out of the range of float"},
  {.name = "block_comment", .body = "  /* no */\n", .error = "block comments are outside the dialect"},
  {.name = "atomic_plain_access", .body = "  device atomic_uint* c = (device atomic_uint*)a.out;\n  uint v = *c;\n",
   .error = "accessed only through atomic functions"}
};

const size_t msl_error_case_count = COUNT(msl_error_cases);
