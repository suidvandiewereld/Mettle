#include "msl_interp_internal.h"
#include <math.h>
#include <string.h>

#define COUNT(array) (sizeof(array) / sizeof((array)[0]))

static uint32_t float_bits(float value) {
  uint32_t bits;
  memcpy(&bits, &value, sizeof bits);
  return bits;
}

static float add_float(float a, float b) {
  volatile float sum = a + b;
  return sum;
}

static const char barrier_body[] =
  "  alignas(16) threadgroup uint tile[64];\n"
  "  threadgroup uchar* tp = (threadgroup uchar*)tile;\n"
  "  uint i = tid.x;\n"
  "  *(threadgroup uint*)(tp + (long)(i * 4u)) = i * 3u;\n"
  "  threadgroup_barrier(mem_flags::mem_threadgroup);\n"
  "  *(a.out + i) = *(threadgroup uint*)(tp + (long)((63u - i) * 4u));\n";

static void prepare_barrier(uint32_t *input, uint32_t *expect) {
  uint32_t i;
  (void)input;
  for (i = 0; i < 64; i++) {
    expect[i] = (63u - i) * 3u;
  }
}

static const char reduction_body[] =
  "  alignas(16) threadgroup uint sums[64];\n"
  "  threadgroup uchar* sp = (threadgroup uchar*)sums;\n"
  "  uint i = tid.x;\n"
  "  *(threadgroup uint*)(sp + (long)(i * 4u)) = i;\n"
  "  threadgroup_barrier(mem_flags::mem_threadgroup);\n"
  "  uint stride = 32u;\n"
  "  while (true) {\n"
  "    if (stride == 0u) {\n"
  "      break;\n"
  "    }\n"
  "    if (i < stride) {\n"
  "      uint other = *(threadgroup uint*)(sp + (long)((i + stride) * 4u));\n"
  "      uint mine = *(threadgroup uint*)(sp + (long)(i * 4u));\n"
  "      *(threadgroup uint*)(sp + (long)(i * 4u)) = mine + other;\n"
  "    }\n"
  "    threadgroup_barrier(mem_flags::mem_threadgroup | mem_flags::mem_device);\n"
  "    stride = stride / 2u;\n"
  "  }\n"
  "  *(a.out + i) = *(threadgroup uint*)(sp);\n";

static void prepare_reduction(uint32_t *input, uint32_t *expect) {
  uint32_t i;
  (void)input;
  for (i = 0; i < 64; i++) {
    expect[i] = 2016;
  }
}

static const char arena_source[] =
  "#include <metal_stdlib>\n"
  "using namespace metal;\n"
  "struct A { device uint* out; device uint* in; };\n"
  "kernel void t(constant A& a [[buffer(0)]], threadgroup uchar* arena [[threadgroup(0)]],\n"
  "    uint3 tid [[thread_position_in_threadgroup]], uint3 g [[threadgroup_position_in_grid]]) {\n"
  "  uint i = tid.x;\n"
  "  *(threadgroup uint*)(arena + (long)(i * 4u)) = i + 100u + g.x * 1000u;\n"
  "  threadgroup_barrier(mem_flags::mem_threadgroup);\n"
  "  *(a.out + (g.x * 32u + i)) = *(threadgroup uint*)(arena + (long)(((i + 1u) % 32u) * 4u));\n"
  "}\n";

static void prepare_arena(uint32_t *input, uint32_t *expect) {
  uint32_t g;
  uint32_t i;
  (void)input;
  for (g = 0; g < 2; g++) {
    for (i = 0; i < 32; i++) {
      expect[g * 32 + i] = (i + 1) % 32 + 100 + g * 1000;
    }
  }
}

static const char sum_body[] =
  "  uint i = tid.x;\n"
  "  *(a.out + i) = simd_sum(i);\n"
  "  float f = (float)(i) * 0.5f;\n"
  "  *(a.out + (40u + i)) = as_type<uint>(simd_sum(f));\n";

static void prepare_sum(uint32_t *input, uint32_t *expect) {
  uint32_t i;
  (void)input;
  for (i = 0; i < 40; i++) {
    expect[i] = i < 32 ? 496u : 284u;
    expect[40 + i] = i < 32 ? 0x43780000u : 0x430e0000u;
  }
}

static const char minmax_body[] =
  "  uint i = tid.x;\n"
  "  uint v = (i * 7u) % 11u;\n"
  "  *(a.out + i) = simd_min(v) * 1000u + simd_max(v);\n"
  "  float f = (float)(v) - 5.0f;\n"
  "  *(a.out + (40u + i)) = as_type<uint>(simd_max(f));\n"
  "  *(a.out + (80u + i)) = as_type<uint>(simd_min(f));\n";

static void prepare_minmax(uint32_t *input, uint32_t *expect) {
  uint32_t i;
  (void)input;
  for (i = 0; i < 40; i++) {
    uint32_t first = i < 32 ? 0 : 32;
    uint32_t end = i < 32 ? 32 : 40;
    uint32_t lo = 100;
    uint32_t hi = 0;
    uint32_t k;
    for (k = first; k < end; k++) {
      uint32_t v = (k * 7) % 11;
      lo = v < lo ? v : lo;
      hi = v > hi ? v : hi;
    }
    expect[i] = lo * 1000 + hi;
    expect[40 + i] = float_bits((float)hi - 5.0f);
    expect[80 + i] = float_bits((float)lo - 5.0f);
  }
}

static const char prefix_body[] =
  "  uint i = tid.x;\n"
  "  *(a.out + i) = simd_prefix_inclusive_sum(i + 1u);\n"
  "  *(a.out + (40u + i)) = simd_prefix_exclusive_sum(i + 1u);\n"
  "  float f = (float)(i) * 0.25f;\n"
  "  *(a.out + (80u + i)) = as_type<uint>(simd_prefix_inclusive_sum(f));\n"
  "  *(a.out + (120u + i)) = as_type<uint>(simd_prefix_exclusive_sum(f));\n";

static void prepare_prefix(uint32_t *input, uint32_t *expect) {
  uint32_t i;
  uint32_t running = 0;
  float frunning = -0.0f;
  (void)input;
  for (i = 0; i < 40; i++) {
    if (i == 32) {
      running = 0;
      frunning = -0.0f;
    }
    expect[40 + i] = running;
    expect[120 + i] = float_bits(frunning);
    running += i + 1;
    frunning = add_float(frunning, (float)i * 0.25f);
    expect[i] = running;
    expect[80 + i] = float_bits(frunning);
  }
}

static const char exchange_body[] =
  "  uint i = tid.x;\n"
  "  *(a.out + i) = simd_broadcast(i * 2u, (ushort)3);\n"
  "  uint src = lane ^ 1u;\n"
  "  *(a.out + (40u + i)) = simd_shuffle(i + 1000u, (ushort)src);\n"
  "  float fv = (float)(i);\n"
  "  *(a.out + (80u + i)) = as_type<uint>(simd_shuffle(fv, (ushort)((lane + 1u) % 8u)));\n"
  "  *(a.out + (120u + i)) = as_type<uint>(simd_broadcast(fv, (ushort)7));\n";

static void prepare_exchange(uint32_t *input, uint32_t *expect) {
  uint32_t i;
  (void)input;
  for (i = 0; i < 40; i++) {
    uint32_t base = i < 32 ? 0 : 32;
    uint32_t lane = i - base;
    expect[i] = (base + 3) * 2;
    expect[40 + i] = base + (lane ^ 1u) + 1000;
    expect[80 + i] = float_bits((float)(base + (lane + 1) % 8));
    expect[120 + i] = float_bits((float)(base + 7));
  }
}

static const char vote_body[] =
  "  uint i = tid.x;\n"
  "  if (i == 5u) {\n"
  "    return;\n"
  "  }\n"
  "  bool odd = (i & 1u) == 1u;\n"
  "  ulong b = (ulong)(simd_vote::vote_t)simd_ballot(odd);\n"
  "  ulong act = (ulong)(simd_vote::vote_t)simd_active_threads_mask();\n"
  "  *(a.out + (i * 2u)) = (uint)b;\n"
  "  *(a.out + (i * 2u + 1u)) = (uint)act + (uint)(act >> 32u);\n"
  "  *(a.out + (80u + i)) = (simd_any(i == 7u) ? 1u : 0u) + (simd_all(i < 39u) ? 2u : 0u);\n";

static void prepare_vote(uint32_t *input, uint32_t *expect) {
  uint32_t i;
  (void)input;
  for (i = 0; i < 40; i++) {
    int first = i < 32;
    if (i == 5) {
      expect[10] = 0xcdcdcdcdu;
      expect[11] = 0xcdcdcdcdu;
      expect[85] = 0xcdcdcdcdu;
      continue;
    }
    expect[i * 2] = first ? 0xaaaaaa8au : 0xaau;
    expect[i * 2 + 1] = first ? 0xffffffdfu : 0xffu;
    expect[80 + i] = first ? 3u : 0u;
  }
}

static const char loop_body[] =
  "  uint i = tid.x;\n"
  "  uint acc = 0u;\n"
  "  for (uint k = 0u; k < i % 4u + 1u; k++) {\n"
  "    acc += simd_sum(1u);\n"
  "  }\n"
  "  *(a.out + i) = acc;\n";

static void prepare_loop(uint32_t *input, uint32_t *expect) {
  static const uint32_t totals[4] = {32, 56, 72, 80};
  uint32_t i;
  (void)input;
  for (i = 0; i < 32; i++) {
    expect[i] = totals[i % 4];
  }
}

static const char attributes_source[] =
  "#include <metal_stdlib>\n"
  "using namespace metal;\n"
  "struct A { device uint* out; device uint* in; };\n"
  "kernel void t(constant A& a [[buffer(0)]],\n"
  "    uint3 tid [[thread_position_in_threadgroup]], uint3 g [[threadgroup_position_in_grid]],\n"
  "    uint3 nt [[threads_per_threadgroup]], uint3 ng [[threadgroups_per_grid]],\n"
  "    uint3 gid [[thread_position_in_grid]], uint3 gsz [[threads_per_grid]],\n"
  "    uint lane [[thread_index_in_simdgroup]], uint lanes [[threads_per_simdgroup]],\n"
  "    uint sg [[simdgroup_index_in_threadgroup]], uint sgs [[simdgroups_per_threadgroup]],\n"
  "    uint lin [[thread_index_in_threadgroup]]) {\n"
  "  simdgroup_barrier(mem_flags::mem_none);\n"
  "  uint base = (g.x * 32u + lin) * 4u;\n"
  "  *(a.out + base) = gid.x + gid.y * 100u;\n"
  "  *(a.out + (base + 1u)) = lane + lanes * 100u + sg * 10000u;\n"
  "  *(a.out + (base + 2u)) = sgs + nt.x * 10u + nt.y * 100u + ng.x * 1000u + gsz.x * 10000u + gsz.y * 1000000u;\n"
  "  *(a.out + (base + 3u)) = simd_sum(1u) + tid.y * 100u;\n"
  "}\n";

static void prepare_attributes(uint32_t *input, uint32_t *expect) {
  uint32_t g;
  uint32_t lin;
  (void)input;
  for (g = 0; g < 2; g++) {
    for (lin = 0; lin < 32; lin++) {
      uint32_t base = (g * 32 + lin) * 4;
      uint32_t x = lin % 8;
      uint32_t y = lin / 8;
      expect[base] = g * 8 + x + y * 100;
      expect[base + 1] = lin % 16 + 1600 + (lin / 16) * 10000;
      expect[base + 2] = 2 + 80 + 400 + 2000 + 160000 + 4000000;
      expect[base + 3] = 16 + y * 100;
    }
  }
}

static const char mixed_body[] =
  "  uint i = tid.x;\n"
  "  uint s = simd_sum(i);\n"
  "  alignas(16) threadgroup uint parts[2];\n"
  "  threadgroup uchar* pp = (threadgroup uchar*)parts;\n"
  "  if (lane == 0u) {\n"
  "    *(threadgroup uint*)(pp + (long)((i / 32u) * 4u)) = s;\n"
  "  }\n"
  "  threadgroup_barrier(mem_flags::mem_threadgroup);\n"
  "  uint other = *(threadgroup uint*)(pp + (long)((1u - i / 32u) * 4u));\n"
  "  *(a.out + i) = simd_max(other + lane);\n";

static void prepare_mixed(uint32_t *input, uint32_t *expect) {
  uint32_t i;
  (void)input;
  for (i = 0; i < 64; i++) {
    expect[i] = (i < 32 ? 1520u : 496u) + 31u;
  }
}

static const char helper_source[] =
  "#include <metal_stdlib>\n"
  "using namespace metal;\n"
  "struct A { device uint* out; device uint* in; };\n"
  "static uint warp_total(uint v) {\n"
  "  return simd_sum(v);\n"
  "}\n"
  "kernel void t(constant A& a [[buffer(0)]], uint3 tid [[thread_position_in_threadgroup]]) {\n"
  "  uint i = tid.x;\n"
  "  uint s = 0u;\n"
  "  for (uint k = 0u; k < 2u; k++) {\n"
  "    s += warp_total(i + k);\n"
  "  }\n"
  "  *(a.out + i) = s + warp_total(1u);\n"
  "}\n";

static void prepare_helper(uint32_t *input, uint32_t *expect) {
  uint32_t i;
  (void)input;
  for (i = 0; i < 40; i++) {
    expect[i] = i < 32 ? 496u + 528u + 32u : 284u + 292u + 8u;
  }
}

static const char atomics_body[] =
  "  device atomic_uint* c = (device atomic_uint*)a.out;\n"
  "  uint i = tid.x;\n"
  "  uint old = atomic_fetch_add_explicit(c, 1u, memory_order_relaxed);\n"
  "  atomic_fetch_sub_explicit(c + 1, 2u, memory_order_relaxed);\n"
  "  atomic_fetch_max_explicit(c + 2, i * 3u, memory_order_relaxed);\n"
  "  atomic_fetch_min_explicit(c + 3, i + 5u, memory_order_relaxed);\n"
  "  atomic_fetch_and_explicit(c + 4, ~(1u << (i % 32u)), memory_order_relaxed);\n"
  "  atomic_fetch_or_explicit(c + 5, 1u << (i % 32u), memory_order_relaxed);\n"
  "  atomic_fetch_xor_explicit(c + 6, i + 1u, memory_order_relaxed);\n"
  "  uint prev = atomic_exchange_explicit(c + 7, i, memory_order_relaxed);\n"
  "  atomic_thread_fence(mem_flags::mem_device, memory_order_seq_cst, thread_scope_device);\n"
  "  alignas(16) threadgroup uint counter[1];\n"
  "  threadgroup atomic_uint* tc = (threadgroup atomic_uint*)((threadgroup uchar*)counter);\n"
  "  if (i == 0u) {\n"
  "    atomic_store_explicit(tc, 0u, memory_order_relaxed);\n"
  "  }\n"
  "  threadgroup_barrier(mem_flags::mem_threadgroup);\n"
  "  atomic_fetch_add_explicit(tc, 2u, memory_order_relaxed);\n"
  "  threadgroup_barrier(mem_flags::mem_threadgroup);\n"
  "  if (i == 0u) {\n"
  "    atomic_store_explicit(c + 8, atomic_load_explicit(tc, memory_order_relaxed), memory_order_relaxed);\n"
  "  }\n"
  "  *(a.out + (16u + i)) = old * 1000u + prev;\n";

static const uint32_t atomics_initial[80] = {0, 1000, 0, 0xffffffffu, 0xffffffffu, 0, 0, 0, 0};

static const char atomics_ordered_body[] =
  "  device atomic_uint* c = (device atomic_uint*)a.out;\n"
  "  uint i = tid.x;\n"
  "  atomic_fetch_or_explicit(c, 1u << (i % 32u), memory_order_acq_rel);\n"
  "  atomic_fetch_xor_explicit(c + 1, i + 1u, memory_order_seq_cst);\n"
  "  atomic_fetch_add_explicit(c + 2, 1u, memory_order_release);\n"
  "  threadgroup_barrier(mem_flags::mem_device);\n"
  "  if (i == 0u) {\n"
  "    atomic_store_explicit(c + 3, atomic_load_explicit(c + 2, memory_order_acquire), memory_order_release);\n"
  "  }\n";

static const uint32_t atomics_ordered_expect[] = {0xffffffffu, 32, 32, 32};

static void prepare_atomics(uint32_t *input, uint32_t *expect) {
  static const uint32_t head[9] = {64, 872, 189, 5, 0, 0xffffffffu, 64, 63, 128};
  uint32_t i;
  (void)input;
  memcpy(expect, head, sizeof head);
  for (i = 0; i < 64; i++) {
    expect[16 + i] = i * 1000 + (i == 0 ? 0 : i - 1);
  }
}

static int accept_atomics_any_order(const uint32_t *out, const uint32_t *expect) {
  uint8_t old_seen[64];
  uint8_t held[64];
  uint32_t i;
  memset(old_seen, 0, sizeof old_seen);
  memset(held, 0, sizeof held);
  for (i = 0; i < 9; i++) {
    if (i != 7 && out[i] != expect[i]) {
      return 0;
    }
  }
  for (i = 0; i < 64; i++) {
    uint32_t old = out[16 + i] / 1000u;
    uint32_t prev = out[16 + i] % 1000u;
    if (old >= 64 || old_seen[old] || prev >= 64) {
      return 0;
    }
    old_seen[old] = 1;
    held[prev]++;
  }
  if (out[7] >= 64) {
    return 0;
  }
  held[out[7]]++;
  for (i = 0; i < 64; i++) {
    if (held[i] != (i == 0 ? 2 : 1)) {
      return 0;
    }
  }
  return 1;
}

static const char cas_loop_body[] =
  "  device atomic_uint* c = (device atomic_uint*)a.out;\n"
  "  uint i = tid.x;\n"
  "  uint attempts = 0u;\n"
  "  while (true) {\n"
  "    uint expected = atomic_load_explicit(c, memory_order_relaxed);\n"
  "    attempts += 1u;\n"
  "    if (atomic_compare_exchange_weak_explicit(c, &expected, expected + 1u, memory_order_relaxed, memory_order_relaxed)) {\n"
  "      break;\n"
  "    }\n"
  "  }\n"
  "  atomic_fetch_add_explicit(c + 1, attempts, memory_order_relaxed);\n"
  "  threadgroup_barrier(mem_flags::mem_device);\n"
  "  if (i == 0u) {\n"
  "    uint total = atomic_load_explicit(c + 1, memory_order_relaxed);\n"
  "    *(a.out + 2) = (total > 64u) ? 1u : 0u;\n"
  "    *(a.out + 1) = 0u;\n"
  "  }\n";

static const uint32_t zeros[8] = {0};
static const uint32_t cas_loop_expect[] = {64, 0, 1};

static const char cas_body[] =
  "  device atomic_uint* c = (device atomic_uint*)a.out;\n"
  "  uint spurious = 0u;\n"
  "  uint honest = 1u;\n"
  "  for (uint k = 0u; k < 20u; k++) {\n"
  "    atomic_store_explicit(c, k, memory_order_relaxed);\n"
  "    while (true) {\n"
  "      uint expected = k;\n"
  "      if (atomic_compare_exchange_weak_explicit(c, &expected, k + 100u, memory_order_relaxed, memory_order_relaxed)) {\n"
  "        break;\n"
  "      }\n"
  "      spurious += 1u;\n"
  "      if (expected != k) {\n"
  "        honest = 0u;\n"
  "      }\n"
  "    }\n"
  "  }\n"
  "  uint wrong = 7u;\n"
  "  bool swapped = atomic_compare_exchange_weak_explicit(c, &wrong, 5u, memory_order_relaxed, memory_order_relaxed);\n"
  "  *(a.out + 1) = honest;\n"
  "  *(a.out + 2) = (spurious > 0u) ? 1u : 0u;\n"
  "  *(a.out + 3) = wrong;\n"
  "  *(a.out + 4) = swapped ? 1u : 0u;\n";

static const uint32_t cas_expect[] = {119, 1, 1, 119, 0};

static const char log_body[] =
  "  int v = (-5);\n"
  "  os_log_default.log(\"tid %u v=%d hex=%x f=%f big=%lu neg=%ld\", tid.x, v, 255u, 1.5f, 10000000000ul, (-7l));\n"
  "  os_log_default.log_fault(\"fault %d %%\", 3);\n"
  "  os_log_default.log(\"w=%5u|%-3d|%08x|%.2f\", 42u, 7, 48879u, 2.5f);\n";

static const char log_expect[] =
  "tid 0 v=-5 hex=ff f=1.500000 big=10000000000 neg=-7\n"
  "fault 3 %\n"
  "w=   42|7  |0000beef|2.50\n"
  "tid 1 v=-5 hex=ff f=1.500000 big=10000000000 neg=-7\n"
  "fault 3 %\n"
  "w=   42|7  |0000beef|2.50\n";

static const char matrix_float_body[] =
  "  device float* src = (device float*)a.in;\n"
  "  device float* dst = (device float*)a.out;\n"
  "  simdgroup_float8x8 ma;\n"
  "  simdgroup_float8x8 mb;\n"
  "  simdgroup_float8x8 mt;\n"
  "  simdgroup_float8x8 mc = make_filled_simdgroup_matrix<float, 8, 8>(1.0f);\n"
  "  simdgroup_float8x8 acc[2];\n"
  "  simdgroup_load(ma, src, 16ul, ulong2(0ul, 0ul), false);\n"
  "  simdgroup_load(mb, src, 16ul, ulong2(8ul, 0ul), false);\n"
  "  simdgroup_load(mt, src, 16ul, ulong2(8ul, 0ul), true);\n"
  "  simdgroup_multiply_accumulate(acc[0], ma, mb, mc);\n"
  "  simdgroup_multiply_accumulate(acc[1], ma, mt, mc);\n"
  "  simdgroup_store(acc[0], dst, 8ul, ulong2(0ul, 0ul), false);\n"
  "  simdgroup_store(acc[1], dst, 8ul, ulong2(0ul, 8ul), true);\n";

static float matrix_source(int k) {
  volatile float scaled = (float)(k % 7) * 0.3f;
  volatile float value = scaled - 1.1f;
  return value;
}

static float matrix_dot(const float *src, int row, int col, int transposed) {
  volatile float sum = 1.0f;
  int k;
  for (k = 0; k < 8; k++) {
    double left = (double)src[row * 16 + k];
    double right = transposed ? (double)src[col * 16 + 8 + k] : (double)src[k * 16 + 8 + col];
    volatile double product = left * right;
    sum = (float)((double)sum + product);
  }
  return sum;
}

static void prepare_matrix_float(uint32_t *input, uint32_t *expect) {
  float src[128];
  int k;
  int r;
  int c;
  for (k = 0; k < 128; k++) {
    src[k] = matrix_source(k);
    input[k] = float_bits(src[k]);
  }
  for (r = 0; r < 8; r++) {
    for (c = 0; c < 8; c++) {
      expect[r * 8 + c] = float_bits(matrix_dot(src, r, c, 0));
      expect[(8 + c) * 8 + r] = float_bits(matrix_dot(src, r, c, 1));
    }
  }
}

static const char matrix_half_body[] =
  "  device half* hs = (device half*)a.in;\n"
  "  device bfloat* bs = (device bfloat*)a.in + 64;\n"
  "  device half* ho = (device half*)a.out;\n"
  "  simdgroup_half8x8 ha;\n"
  "  simdgroup_bfloat8x8 hb;\n"
  "  simdgroup_half8x8 hc = make_filled_simdgroup_matrix<half, 8, 8>((half)0.5f);\n"
  "  simdgroup_half8x8 hd;\n"
  "  simdgroup_load(ha, hs, 8ul, ulong2(0ul, 0ul), false);\n"
  "  simdgroup_load(hb, bs, 8ul, ulong2(0ul, 0ul), true);\n"
  "  simdgroup_multiply_accumulate(hd, ha, hb, hc);\n"
  "  simdgroup_store(hd, ho, 8ul, ulong2(0ul, 0ul), false);\n";

static uint16_t exact_half_bits(double value) {
  uint16_t sign = value < 0 ? 0x8000u : 0u;
  double magnitude = fabs(value);
  int exponent = 0;
  if (magnitude == 0.0) {
    return sign;
  }
  while (magnitude >= 2.0) {
    magnitude /= 2.0;
    exponent++;
  }
  while (magnitude < 1.0) {
    magnitude *= 2.0;
    exponent--;
  }
  return (uint16_t)(sign | (uint16_t)((exponent + 15) << 10) | (uint16_t)((magnitude - 1.0) * 1024.0));
}

static uint16_t small_bfloat_bits(int value) {
  return (uint16_t)(float_bits((float)value) >> 16);
}

static void put_half(uint32_t *words, int index, uint16_t bits) {
  int shift = (index % 2) * 16;
  words[index / 2] = (words[index / 2] & ~(0xffffu << shift)) | ((uint32_t)bits << shift);
}

static void prepare_matrix_half(uint32_t *input, uint32_t *expect) {
  int a[64];
  int b[64];
  int k;
  int r;
  int c;
  for (k = 0; k < 64; k++) {
    a[k] = ((k / 8) * 3 + (k % 8) * 5) % 7 - 3;
    b[k] = (k * 5 + 1 + k / 8) % 5 - 2;
    put_half(input, k, exact_half_bits((double)a[k]));
    put_half(input, 64 + k, small_bfloat_bits(b[k]));
  }
  for (r = 0; r < 8; r++) {
    for (c = 0; c < 8; c++) {
      double sum = 0.5;
      int i;
      for (i = 0; i < 8; i++) {
        sum += (double)a[r * 8 + i] * (double)b[c * 8 + i];
      }
      put_half(expect, r * 8 + c, exact_half_bits(sum));
    }
  }
}

const MslCase msl_collective_cases[] = {
  {.name = "barrier_64_threads", .body = barrier_body, .block = {64}, .expect_count = 64, .prepare = prepare_barrier},
  {.name = "barrier_reduction_loop", .body = reduction_body, .block = {64}, .expect_count = 64, .prepare = prepare_reduction},
  {.name = "threadgroup_arena", .body = arena_source, .full = 1, .block = {32}, .grid = {2}, .tg_bytes = 128, .expect_count = 64,
   .prepare = prepare_arena},
  {.name = "simd_sum_partial_group", .body = sum_body, .block = {40}, .expect_count = 80, .prepare = prepare_sum},
  {.name = "simd_min_max", .body = minmax_body, .block = {40}, .expect_count = 120, .prepare = prepare_minmax},
  {.name = "simd_prefix_sums", .body = prefix_body, .block = {40}, .expect_count = 160, .prepare = prepare_prefix},
  {.name = "simd_broadcast_shuffle", .body = exchange_body, .block = {40}, .expect_count = 160, .prepare = prepare_exchange},
  {.name = "simd_votes_with_finished_lane", .body = vote_body, .block = {40}, .expect_count = 120, .prepare = prepare_vote},
  {.name = "simd_sum_in_divergent_loop", .body = loop_body, .block = {32}, .expect_count = 32, .prepare = prepare_loop},
  {.name = "attributes_simd16", .body = attributes_source, .full = 1, .block = {8, 4, 1}, .grid = {2}, .simd_width = 16,
   .expect_count = 256, .prepare = prepare_attributes},
  {.name = "simd_and_barrier_mix", .body = mixed_body, .block = {64}, .expect_count = 64, .prepare = prepare_mixed},
  {.name = "simd_sum_inside_helper", .body = helper_source, .full = 1, .block = {40}, .expect_count = 40, .prepare = prepare_helper},
  {.name = "atomics", .body = atomics_body, .block = {64}, .initial = atomics_initial, .expect_count = 80, .prepare = prepare_atomics,
   .device_accept = accept_atomics_any_order},
  {.name = "atomics_ordered", .body = atomics_ordered_body, .block = {32}, .initial = zeros, .expect = atomics_ordered_expect,
   .expect_count = COUNT(atomics_ordered_expect), .device_skip = "MSL 4.1 ordered atomics; Metal 4.0 does not declare them"},
  {.name = "cas_loop_spurious", .body = cas_loop_body, .block = {64}, .spurious = 1, .initial = zeros,
   .expect = cas_loop_expect, .expect_count = COUNT(cas_loop_expect)},
  {.name = "cas_weak_semantics", .body = cas_body, .spurious = 1, .initial = zeros, .expect = cas_expect,
   .expect_count = COUNT(cas_expect)},
  {.name = "os_log", .body = log_body, .block = {2}, .log = log_expect},
  {.name = "simdgroup_matrix_float", .body = matrix_float_body, .block = {32}, .input_count = 128, .expect_count = 128,
   .prepare = prepare_matrix_float},
  {.name = "simdgroup_matrix_half_bfloat", .body = matrix_half_body, .block = {32}, .input_count = 64, .expect_count = 32,
   .prepare = prepare_matrix_half}
};

const size_t msl_collective_case_count = COUNT(msl_collective_cases);
