#include "msl_interp_internal.h"
#include <string.h>

#define COUNT(array) (sizeof(array) / sizeof((array)[0]))

static const char control_flow_body[] =
  "  uint s = 0u;\n"
  "  for (uint i = 0u; i < 10u; i++) {\n"
  "    if (i == 3u) {\n"
  "      continue;\n"
  "    }\n"
  "    s += i;\n"
  "  }\n"
  "  *(a.out + 0) = s;\n"
  "  uint n = 0u;\n"
  "  while (true) {\n"
  "    n += 1u;\n"
  "    if (n >= 7u) {\n"
  "      break;\n"
  "    }\n"
  "  }\n"
  "  *(a.out + 1) = n;\n"
  "  uint w = 5u;\n"
  "  switch (0) {\n"
  "  default: {\n"
  "    w = 6u;\n"
  "    if (w == 6u) {\n"
  "      break;\n"
  "    }\n"
  "    w = 100u;\n"
  "  }\n"
  "  }\n"
  "  *(a.out + 2) = w;\n"
  "  uint e = 0u;\n"
  "  if (s > 100u) {\n"
  "    e = 1u;\n"
  "  } else {\n"
  "    e = 2u;\n"
  "  }\n"
  "  *(a.out + 3) = e;\n"
  "  {\n"
  "    uint inner = 9u;\n"
  "    *(a.out + 4) = inner;\n"
  "  }\n"
  "  uint c = 0u;\n"
  "  for (uint i = 0u; i < 3u; i++) {\n"
  "    switch (0) {\n"
  "    default: {\n"
  "      if (i == 1u) {\n"
  "        continue;\n"
  "      }\n"
  "      c += 10u;\n"
  "    }\n"
  "    }\n"
  "  }\n"
  "  *(a.out + 5) = c;\n"
  "  bool both = s == 42u && n == 7u;\n"
  "  bool either = s == 0u || w == 6u;\n"
  "  *(a.out + 6) = both ? 1u : 0u;\n"
  "  *(a.out + 7) = either ? 1u : 0u;\n"
  "  *(a.out + 8) = (s > 1u) ? ((n < 3u) ? 5u : 6u) : 7u;\n"
  "  *(a.out + 9) = (!both) ? 1u : 2u;\n";

static const uint32_t control_flow_expect[] = {42, 7, 6, 2, 9, 20, 1, 1, 6, 2};

static const char compound_body[] =
  "  uint x = 1u;\n"
  "  x |= 6u;\n"
  "  x <<= 2u;\n"
  "  x -= 3u;\n"
  "  x *= 3u;\n"
  "  x &= 0x3cu;\n"
  "  x ^= 5u;\n"
  "  x >>= 1u;\n"
  "  *(a.out + 0) = x;\n"
  "  *(a.out + 1) = 10u;\n"
  "  *(a.out + 1) += 5u;\n"
  "  *(a.out + 1) |= 64u;\n"
  "  int k = 5;\n"
  "  k++;\n"
  "  k++;\n"
  "  k--;\n"
  "  *(a.out + 2) = as_type<uint>(k);\n";

static const uint32_t compound_expect[] = {6, 79, 6};

static const char int_ops_body[] =
  "  int x = 17;\n"
  "  int y = (-5);\n"
  "  *(a.out + 0) = as_type<uint>(x + y);\n"
  "  *(a.out + 1) = as_type<uint>(x - y);\n"
  "  *(a.out + 2) = as_type<uint>(x * y);\n"
  "  *(a.out + 3) = as_type<uint>(y / 2);\n"
  "  *(a.out + 4) = as_type<uint>(x % 5);\n"
  "  *(a.out + 5) = as_type<uint>(x & y);\n"
  "  *(a.out + 6) = as_type<uint>(x | y);\n"
  "  *(a.out + 7) = as_type<uint>(x ^ y);\n"
  "  *(a.out + 8) = as_type<uint>(x << 3);\n"
  "  *(a.out + 9) = as_type<uint>(y >> 1);\n"
  "  *(a.out + 10) = (x < y) ? 1u : 0u;\n"
  "  *(a.out + 11) = (x >= y) ? 1u : 0u;\n"
  "  *(a.out + 12) = (y <= y) ? 1u : 0u;\n"
  "  *(a.out + 13) = (x != y) ? 1u : 0u;\n"
  "  *(a.out + 14) = as_type<uint>((-2147483647 - 1) / 3);\n"
  "  *(a.out + 15) = as_type<uint>(x / y);\n"
  "  *(a.out + 16) = as_type<uint>(~x);\n"
  "  *(a.out + 17) = as_type<uint>(-y);\n"
  "  *(a.out + 18) = as_type<uint>(1 << 31);\n"
  "  *(a.out + 19) = (x > y) ? 1u : 0u;\n";

static const uint32_t int_ops_expect[] = {
  12, 22, 0xffffffabu, 0xfffffffeu, 2, 17, 0xfffffffbu, 0xffffffeau, 136, 0xfffffffdu,
  0, 1, 1, 1, 0xd5555556u, 0xfffffffdu, 0xffffffeeu, 5, 0x80000000u, 1
};

static const char uint_ops_body[] =
  "  uint m = 4294967295u;\n"
  "  *(a.out + 0) = m + 1u;\n"
  "  *(a.out + 1) = 0u - 1u;\n"
  "  *(a.out + 2) = m * m;\n"
  "  *(a.out + 3) = m / 16u;\n"
  "  *(a.out + 4) = m % 7u;\n"
  "  *(a.out + 5) = m >> 31u;\n"
  "  *(a.out + 6) = 3u << 31u;\n"
  "  *(a.out + 7) = ~5u;\n"
  "  *(a.out + 8) = (m > 1u) ? 1u : 0u;\n"
  "  *(a.out + 9) = 0xf0u ^ 0xffu;\n"
  "  ushort h = (ushort)65535;\n"
  "  *(a.out + 10) = as_type<uint>(h + h);\n"
  "  uchar b = (uchar)200;\n"
  "  *(a.out + 11) = as_type<uint>(b * b);\n"
  "  *(a.out + 12) = as_type<uint>(-b);\n"
  "  *(a.out + 13) = -m;\n"
  "  *(a.out + 14) = m & 0xff00u;\n"
  "  *(a.out + 15) = 0x10u | 0x01u;\n";

static const uint32_t uint_ops_expect[] = {
  0, 0xffffffffu, 1, 0x0fffffffu, 3, 1, 0x80000000u, 0xfffffffau, 1, 0x0f, 131070, 40000, 0xffffff38u, 1, 0xff00u, 0x11u
};

static const char long_ops_body[] =
  "  long p = 3000000000l;\n"
  "  long q = p * 3l;\n"
  "  ulong u = 18446744073709551615ul;\n"
  "  ulong v = u + 2ul;\n"
  "  *(a.out + 0) = (uint)q;\n"
  "  *(a.out + 1) = (uint)(q >> 32);\n"
  "  *(a.out + 2) = (uint)v;\n"
  "  *(a.out + 3) = (uint)(u >> 40ul);\n"
  "  long neg = (-7l);\n"
  "  *(a.out + 4) = as_type<uint>((int)(neg / 2l));\n"
  "  ulong big = (ulong)(-1);\n"
  "  *(a.out + 5) = (uint)(big >> 33u);\n"
  "  *(a.out + 6) = (uint)(as_type<ulong>(neg));\n"
  "  *(a.out + 7) = (uint)(u * u);\n"
  "  *(a.out + 8) = (neg < 0l) ? 1u : 0u;\n";

static const uint32_t long_ops_expect[] = {0x18711a00u, 2, 1, 0xffffff, 0xfffffffdu, 0x7fffffff, 0xfffffff9u, 1, 1};

static const char casts_body[] =
  "  float f1 = 2.75f;\n"
  "  float f2 = (-2.75f);\n"
  "  *(a.out + 0) = as_type<uint>((int)f1);\n"
  "  *(a.out + 1) = as_type<uint>((int)f2);\n"
  "  *(a.out + 2) = (uint)3.99f;\n"
  "  *(a.out + 3) = as_type<uint>((float)(int)(-3));\n"
  "  *(a.out + 4) = as_type<uint>((float)4294967295u);\n"
  "  *(a.out + 5) = as_type<uint>((float)16777217);\n"
  "  *(a.out + 6) = as_type<uint>((float)16777219);\n"
  "  *(a.out + 7) = as_type<uint>((int)(char)(uchar)200u);\n"
  "  *(a.out + 8) = (uint)(uchar)(int)(-1);\n"
  "  *(a.out + 9) = as_type<uint>((int)(short)70000);\n"
  "  *(a.out + 10) = (uint)(bool)5;\n"
  "  *(a.out + 11) = (uint)((ulong)(-1) >> 63u);\n"
  "  *(a.out + 12) = as_type<uint>((float)(long)(-9007199254740993l));\n"
  "  *(a.out + 13) = (uint)(ushort)(half)65504.0f;\n"
  "  *(a.out + 14) = as_type<uint>((float)(half)0.1f);\n"
  "  *(a.out + 15) = as_type<uint>((int)(-0.5f) + 5);\n"
  "  *(a.out + 16) = as_type<uint>((float)(ulong)18446744073709551615ul);\n"
  "  *(a.out + 17) = (uint)(ulong)1.0e10f;\n"
  "  *(a.out + 18) = as_type<uint>((int)(half)(-2.5f));\n"
  "  *(a.out + 19) = (uint)(bool)0.5f;\n";

static const uint32_t casts_expect[] = {
  2, 0xfffffffeu, 3, 0xc0400000u, 0x4f800000u, 0x4b800000u, 0x4b800002u, 0xffffffc8u, 255, 4464,
  1, 1, 0xda000000u, 65504, 0x3dccc000u, 5, 0x5f800000u, 0x540be400u, 0xfffffffeu, 1
};

static const char float_ops_body[] =
  "  float x = 0.1f;\n"
  "  float y = 0.2f;\n"
  "  float z = 0.0f;\n"
  "  *(a.out + 0) = as_type<uint>(x + y);\n"
  "  *(a.out + 1) = as_type<uint>(1.0f / 3.0f);\n"
  "  *(a.out + 2) = as_type<uint>(x * y);\n"
  "  *(a.out + 3) = as_type<uint>(x - y);\n"
  "  *(a.out + 4) = (x < y) ? 1u : 0u;\n"
  "  *(a.out + 5) = as_type<uint>(-x);\n"
  "  *(a.out + 6) = as_type<uint>(3.00000001e+38f * 10.0f);\n"
  "  *(a.out + 7) = as_type<uint>(as_type<float>(*(a.in + 0)) * as_type<float>(*(a.in + 1)));\n"
  "  *(a.out + 8) = as_type<uint>(as_type<float>(*(a.in + 2)) * 0.5f);\n"
  "  *(a.out + 9) = (z / z != z / z) ? 1u : 0u;\n"
  "  *(a.out + 10) = (z / z == z / z) ? 7u : 3u;\n"
  "  *(a.out + 11) = as_type<uint>(1.0f / z);\n"
  "  *(a.out + 12) = as_type<uint>(16777216.0f + 1.0f);\n"
  "  *(a.out + 13) = as_type<uint>(-z);\n"
  "  *(a.out + 14) = (z == -z) ? 1u : 0u;\n";

static const uint32_t float_ops_input[] = {1, 0x3f800000u, 0x00800000u};

static const uint32_t float_ops_expect[] = {
  0x3e99999au, 0x3eaaaaabu, 0x3ca3d70bu, 0xbdcccccdu, 1, 0xbdcccccdu, 0x7f800000u, 0, 0, 1, 3, 0x7f800000u,
  0x4b800000u, 0x80000000u, 1
};

static const char float_subnormal_body[] =
  "  float d = as_type<float>(*(a.in + 0));\n"
  "  float nd = as_type<float>(*(a.in + 1));\n"
  "  float one = as_type<float>(*(a.in + 2));\n"
  "  float p = as_type<float>(*(a.in + 3));\n"
  "  float q = as_type<float>(*(a.in + 4));\n"
  "  half2 h = as_type<half2>(*(a.in + 5));\n"
  "  *(a.out + 0) = as_type<uint>(d * one);\n"
  "  *(a.out + 1) = as_type<uint>(nd * one);\n"
  "  *(a.out + 2) = (d > 0.0f) ? 1u : 0u;\n"
  "  *(a.out + 3) = (d == 0.0f) ? 1u : 0u;\n"
  "  *(a.out + 4) = as_type<uint>(fabs(nd));\n"
  "  *(a.out + 5) = as_type<uint>(-d);\n"
  "  *(a.out + 6) = as_type<uint>(precise::sqrt(d));\n"
  "  *(a.out + 7) = as_type<uint>(p * q);\n"
  "  *(a.out + 8) = as_type<uint>(fma(p, q, 0.0f));\n"
  "  *(a.out + 9) = as_type<uint>(fma(p, q, one));\n"
  "  *(a.out + 10) = (uint)as_type<ushort>(h.x * h.y);\n"
  "  *(a.out + 11) = (uint)as_type<ushort>(h.x + h.x);\n"
  "  *(a.out + 12) = as_type<uint>((one > 0.0f) ? d : 0.0f);\n";

static const uint32_t float_subnormal_input[] = {0x0020aac8u, 0x8020aac8u, 0x3f800000u, 0x1e3ce508u, 0x1fec1e4au, 0x3c000001u};

static const uint32_t float_subnormal_expect[] = {
  0, 0x80000000u, 0, 1, 0x0020aac8u, 0x8020aac8u, 0, 0, 0, 0x3f800000u, 0x0001, 0x0002, 0x0020aac8u
};

#define HB(expr) "(uint)as_type<ushort>(" expr ")"

static const char half_conv_body[] =
  "  *(a.out + 0) = " HB("(half)65520.0f") ";\n"
  "  *(a.out + 1) = " HB("(half)65519.996f") ";\n"
  "  *(a.out + 2) = " HB("(half)65504.0f") ";\n"
  "  *(a.out + 3) = " HB("(half)as_type<float>(0x33800000u)") ";\n"
  "  *(a.out + 4) = " HB("(half)as_type<float>(0x33000000u)") ";\n"
  "  *(a.out + 5) = " HB("(half)as_type<float>(0x33c00000u)") ";\n"
  "  *(a.out + 6) = " HB("(half)as_type<float>(0x33400000u)") ";\n"
  "  *(a.out + 7) = " HB("(half)(-0.0f)") ";\n"
  "  *(a.out + 8) = " HB("(half)as_type<float>(0x38800000u)") ";\n"
  "  *(a.out + 9) = " HB("(half)as_type<float>(0x387fc000u)") ";\n"
  "  *(a.out + 10) = as_type<uint>((float)as_type<half>((ushort)1));\n"
  "  *(a.out + 11) = as_type<uint>((float)as_type<half>((ushort)31744));\n"
  "  *(a.out + 12) = " HB("(half)as_type<float>(0x7fc00000u)") ";\n"
  "  *(a.out + 13) = " HB("(half)as_type<float>(0x3f801000u)") ";\n"
  "  *(a.out + 14) = " HB("(half)as_type<float>(0x3f803000u)") ";\n"
  "  *(a.out + 15) = " HB("(bfloat)as_type<float>(0x3f808000u)") ";\n"
  "  *(a.out + 16) = " HB("(bfloat)as_type<float>(0x3f818000u)") ";\n"
  "  *(a.out + 17) = " HB("(bfloat)as_type<float>(0x7f7fffffu)") ";\n"
  "  *(a.out + 18) = " HB("(bfloat)as_type<float>(0x00018000u)") ";\n"
  "  *(a.out + 19) = " HB("(bfloat)as_type<float>(0x00008000u)") ";\n"
  "  *(a.out + 20) = as_type<uint>((float)as_type<bfloat>((ushort)16457));\n"
  "  *(a.out + 21) = " HB("(bfloat)as_type<float>(0x7f800001u)") ";\n"
  "  *(a.out + 22) = " HB("(half)(bfloat)1.5f") ";\n"
  "  *(a.out + 23) = " HB("(half)(int)(-3)") ";\n"
  "  *(a.out + 24) = " HB("(half)65535u") ";\n"
  "  *(a.out + 25) = " HB("(half)2049") ";\n"
  "  *(a.out + 26) = " HB("(half)2051") ";\n"
  "  *(a.out + 27) = " HB("(half)as_type<float>(0xc77ff000u)") ";\n"
  "  *(a.out + 28) = " HB("(bfloat)as_type<float>(0x807fffffu)") ";\n"
  "  *(a.out + 29) = " HB("(half)as_type<float>(0x33000001u)") ";\n"
  "  *(a.out + 30) = " HB("(half)as_type<float>(0x477fefffu)") ";\n"
  "  *(a.out + 31) = as_type<uint>((float)as_type<half>((ushort)1023));\n";

static const uint32_t half_conv_expect[] = {
  0x7c00, 0x7bff, 0x7bff, 0x0001, 0x0000, 0x0002, 0x0001, 0x8000, 0x0400, 0x03ff, 0x33800000u, 0x7f800000u, 0x7e00, 0x3c00,
  0x3c02, 0x3f80, 0x3f82, 0x7f80, 0x0002, 0x0000, 0x40490000u, 0x7fc0, 0x3e00, 0xc200, 0x7c00, 0x6800, 0x6802, 0xfc00, 0x8080,
  0x0001, 0x7bff, 0x387fc000u
};

static const char half_arith_body[] =
  "  half h1 = (half)2048.0f;\n"
  "  half h2 = (half)1.0f;\n"
  "  half h3 = (half)3.0f;\n"
  "  *(a.out + 0) = " HB("h1 + h2") ";\n"
  "  *(a.out + 1) = " HB("h2 / h3") ";\n"
  "  half2 p = half2(h1, h3);\n"
  "  half2 q = half2(h2, h3);\n"
  "  half2 r = p + q;\n"
  "  *(a.out + 2) = as_type<uint>(r);\n"
  "  *(a.out + 3) = as_type<uint>(p * q);\n"
  "  half big = (half)300.0f;\n"
  "  *(a.out + 4) = " HB("big * big") ";\n"
  "  *(a.out + 5) = as_type<uint>((float)(r.y));\n"
  "  bfloat b1 = (bfloat)1.0f;\n"
  "  bfloat b2 = (bfloat)0.00390625f;\n"
  "  *(a.out + 6) = " HB("b1 + b2") ";\n"
  "  *(a.out + 7) = as_type<uint>(-p);\n"
  "  *(a.out + 8) = (h1 > h2) ? 1u : 0u;\n"
  "  *(a.out + 9) = " HB("h3 - h1") ";\n"
  "  *(a.out + 10) = as_type<uint>(p - q);\n"
  "  *(a.out + 11) = as_type<uint>(p / q);\n";

static const uint32_t half_arith_expect[] = {
  0x6800, 0x3555, 0x46006800u, 0x48806800u, 0x7c00, 0x40c00000u, 0x3f80, 0xc200e800u, 1, 0xe7fd, 0x000067ffu, 0x3c006800u
};

static const char fma_body[] =
  "  half2 x = half2(as_type<half>((ushort)15361), as_type<half>((ushort)23552));\n"
  "  half2 y = half2(as_type<half>((ushort)15362), as_type<half>((ushort)23552));\n"
  "  half2 z = half2(as_type<half>((ushort)48128), as_type<half>((ushort)0));\n"
  "  *(a.out + 0) = as_type<uint>(fma(x, y, z));\n"
  "  half2 u = x * y;\n"
  "  *(a.out + 1) = as_type<uint>(u + z);\n"
  "  half2 a2 = half2(as_type<half>((ushort)1), as_type<half>((ushort)15360));\n"
  "  half2 b2 = half2(as_type<half>((ushort)14336), as_type<half>((ushort)15360));\n"
  "  half2 c2 = half2(as_type<half>((ushort)1), as_type<half>((ushort)32768));\n"
  "  *(a.out + 2) = as_type<uint>(fma(a2, b2, c2));\n"
  "  half2 a3 = half2(as_type<half>((ushort)15360), as_type<half>((ushort)32768));\n"
  "  half2 b3 = half2(as_type<half>((ushort)32768), as_type<half>((ushort)15360));\n"
  "  half2 c3 = half2(as_type<half>((ushort)0), as_type<half>((ushort)32768));\n"
  "  *(a.out + 3) = as_type<uint>(fma(a3, b3, c3));\n";

static const uint32_t fma_expect[] = {0x7c001a01u, 0x7c001a00u, 0x3c000002u, 0x80000000u};

static const char fma_float_body[] =
  "  float x = as_type<float>(0x3f800001u);\n"
  "  float y = as_type<float>(0x3f7ffffeu);\n"
  "  float z = as_type<float>(0x4b800001u);\n"
  "  *(a.out + 0) = as_type<uint>(fma(x, y, z));\n"
  "  *(a.out + 1) = as_type<uint>(fma(x, y, -z));\n"
  "  *(a.out + 2) = as_type<uint>(fma(2.0f, 3.0f, 1.0f));\n";

static const uint32_t fma_float_expect[] = {0x4b800001u, 0xcb800001u, 0x40e00000u};

static const char math_body[] =
  "  *(a.out + 0) = as_type<uint>(precise::sqrt(2.0f));\n"
  "  *(a.out + 1) = as_type<uint>(precise::rsqrt(2.0f));\n"
  "  *(a.out + 2) = as_type<uint>(precise::sqrt(9.0f));\n"
  "  *(a.out + 3) = as_type<uint>(precise::rsqrt(16.0f));\n"
  "  *(a.out + 4) = as_type<uint>(precise::sqrt(0.25f));\n"
  "  *(a.out + 5) = as_type<uint>(fmod((-5.5f), 2.0f));\n"
  "  *(a.out + 6) = as_type<uint>(fast::sqrt(16.0f));\n"
  "  *(a.out + 7) = as_type<uint>(fabs((-1.5f)));\n"
  "  *(a.out + 8) = as_type<uint>(fmod(5.5f, 2.0f));\n"
  "  *(a.out + 9) = as_type<uint>(precise::rsqrt(4.0f));\n"
  "  float m1 = 0.0f - 1.0f;\n"
  "  float s = precise::sqrt(m1);\n"
  "  *(a.out + 10) = (s != s) ? 1u : 0u;\n"
  "  *(a.out + 11) = as_type<uint>(fast::exp(0.0f));\n"
  "  *(a.out + 12) = as_type<uint>(fast::log(1.0f) + fast::sin(0.0f) + fast::cos(0.0f) + fast::rsqrt(1.0f));\n";

static const uint32_t math_expect[] = {
  0x3fb504f3u, 0x3f3504f3u, 0x40400000u, 0x3e800000u, 0x3f000000u, 0xbfc00000u, 0x40800000u, 0x3fc00000u, 0x3fc00000u,
  0x3f000000u, 1, 0x3f800000u, 0x40000000u
};

static const char transcendental_body[] =
  "  float one = as_type<float>(*(a.in + 0));\n"
  "  float two = as_type<float>(*(a.in + 1));\n"
  "  *(a.out + 0) = as_type<uint>(precise::sin(one));\n"
  "  *(a.out + 1) = as_type<uint>(precise::cos(one));\n"
  "  *(a.out + 2) = as_type<uint>(precise::log(two));\n"
  "  *(a.out + 3) = as_type<uint>(precise::exp(one));\n";

static const uint32_t transcendental_input[] = {0x3f800000u, 0x40000000u};

static const uint32_t transcendental_expect[] = {0x3f576aa4u, 0x3f0a5140u, 0x3f317218u, 0x402df854u};

static const char astype_body[] =
  "  *(a.out + 0) = as_type<uint>(1.0f);\n"
  "  *(a.out + 1) = as_type<uint>(as_type<float>(1078530011u) * 2.0f);\n"
  "  half2 hv = as_type<half2>(1006648320u);\n"
  "  *(a.out + 2) = as_type<uint>(hv + hv);\n"
  "  *(a.out + 3) = as_type<uint>((int)as_type<long>(18446744073709551615ul));\n"
  "  *(a.out + 4) = (uint)(ulong2(1ul, 2ul).y);\n"
  "  packed_float4 pk = *(device packed_float4*)(a.in + 1);\n"
  "  float4 f = float4(pk);\n"
  "  *(a.out + 5) = as_type<uint>(f.z);\n"
  "  uint4 u4 = uint4(*(device packed_uint4*)(a.in + 4));\n"
  "  *(a.out + 6) = u4.w;\n"
  "  float4 v4 = *(device float4*)(a.in + 0);\n"
  "  *(a.out + 7) = as_type<uint>(v4.y);\n"
  "  ulong addr = reinterpret_cast<ulong>(a.out);\n"
  "  device uint* back = reinterpret_cast<device uint*>(addr + 32ul);\n"
  "  *back = 77u;\n"
  "  *(a.out + 9) = (back == a.out + 8) ? 1u : 0u;\n"
  "  *(a.out + 10) = as_type<uint4>(v4).x;\n"
  "  *(a.out + 11) = (uint)as_type<ushort>(as_type<half2>(1006648320u).y) + 0u;\n"
  "  *(device float4*)(a.out + 12) = f;\n";

static const uint32_t astype_input[] = {0x3f800000u, 0x40000000u, 0x40400000u, 0x40800000u, 7, 8, 9, 10};

static const uint32_t astype_expect[] = {
  0x3f800000u, 0x40c90fdbu, 0x40004000u, 0xffffffffu, 2, 0x40800000u, 10, 0x40000000u, 77, 1, 0x3f800000u, 0x3c00,
  0x40000000u, 0x40400000u, 0x40800000u, 7
};

static const char records_source[] =
  "#include <metal_stdlib>\n"
  "using namespace metal;\n"
  "#pragma METAL fp math_mode(safe)\n"
  "#pragma METAL fp contract(off)\n"
  "// records are byte blobs\n"
  "struct alignas(4) R { uchar bytes[8]; };\n"
  "static_assert(sizeof(R) == 8, \"record layout\");\n"
  "struct alignas(8) W { uchar bytes[12]; };\n"
  "static_assert(sizeof(W) == 16, \"rounded up\");\n"
  "struct A { device uint* out; device uint* in; };\n"
  "static R make(float x, float y);\n"
  "static float total(R r);\n"
  "static R make(float x, float y) {\n"
  "  R r = {};\n"
  "  thread uchar* p = (thread uchar*)&r;\n"
  "  *(thread float*)(p) = x;\n"
  "  *(thread float*)(p + 4l) = y;\n"
  "  return r;\n"
  "}\n"
  "static float total(R r) {\n"
  "  thread uchar* p = (thread uchar*)&r;\n"
  "  return *(thread float*)(p) + *(thread float*)(p + 4l);\n"
  "}\n"
  "[[max_total_threads_per_threadgroup(64)]]\n"
  "kernel void t(constant A& a [[buffer(0)]]) {\n"
  "  R r1 = make(1.5f, 2.0f);\n"
  "  R r2;\n"
  "  r2 = r1;\n"
  "  *(a.out + 0) = as_type<uint>(total(r2));\n"
  "  R z = {};\n"
  "  *(a.out + 1) = as_type<uint>(total(z));\n"
  "  *(device R*)(a.out + 2) = r1;\n"
  "  R r3 = *(device R*)(a.in);\n"
  "  *(a.out + 4) = as_type<uint>(total(r3));\n"
  "  alignas(16) thread R arr[3];\n"
  "  thread uchar* ap = (thread uchar*)arr;\n"
  "  *(thread R*)(ap + 8l) = make(4.0f, 0.5f);\n"
  "  *(a.out + 5) = as_type<uint>(total(*(thread R*)(ap + 8l)));\n"
  "  W w = {};\n"
  "  *(a.out + 6) = (uint)*(thread uchar*)((thread uchar*)&w + 15l) + 3u;\n"
  "  return;\n"
  "}\n";

static const uint32_t records_input[] = {0x41000000u, 0x3f800000u};
static const uint32_t records_expect[] = {0x40600000u, 0, 0x3fc00000u, 0x40000000u, 0x41100000u, 0x40900000u, 3};

static const char pointers_body[] =
  "  device uchar* src = (device uchar*)a.in;\n"
  "  device uchar* thread* pp = &src;\n"
  "  device uchar* got = *pp;\n"
  "  *(a.out + 0) = *(device uint*)(got);\n"
  "  *(device uchar* device*)(a.out + 2) = src;\n"
  "  device uchar* back = *(device uchar* device*)(a.out + 2);\n"
  "  *(a.out + 1) = *(device uint*)(back + 4l);\n"
  "  *(a.out + 4) = (back == src) ? 1u : 0u;\n"
  "  device uchar* thread* thread* ppp = &pp;\n"
  "  *(a.out + 5) = *(device uint*)(**ppp + 4l);\n"
  "  device uchar* none = nullptr;\n"
  "  *(a.out + 6) = (none == nullptr) ? 1u : 0u;\n"
  "  *(a.out + 7) = (got != none) ? 1u : 0u;\n"
  "  device uchar* dec = (got + 8l) - 4l;\n"
  "  *(a.out + 8) = *(device uint*)(dec);\n"
  "  *(a.out + 2) = 0u;\n"
  "  *(a.out + 3) = 0u;\n";

static const uint32_t pointers_input[] = {11, 22, 33};
static const uint32_t pointers_expect[] = {11, 22, 0, 0, 1, 22, 1, 1, 22};

const MslCase msl_value_cases[] = {
  {.name = "control_flow", .body = control_flow_body, .expect = control_flow_expect, .expect_count = COUNT(control_flow_expect)},
  {.name = "compound_assignment", .body = compound_body, .expect = compound_expect, .expect_count = COUNT(compound_expect)},
  {.name = "int_ops", .body = int_ops_body, .expect = int_ops_expect, .expect_count = COUNT(int_ops_expect)},
  {.name = "uint_ops", .body = uint_ops_body, .expect = uint_ops_expect, .expect_count = COUNT(uint_ops_expect)},
  {.name = "long_ops", .body = long_ops_body, .expect = long_ops_expect, .expect_count = COUNT(long_ops_expect)},
  {.name = "casts", .body = casts_body, .expect = casts_expect, .expect_count = COUNT(casts_expect)},
  {.name = "float_ops", .body = float_ops_body, .input = float_ops_input, .input_count = COUNT(float_ops_input),
   .expect = float_ops_expect, .expect_count = COUNT(float_ops_expect)},
  {.name = "float_subnormals", .body = float_subnormal_body, .input = float_subnormal_input,
   .input_count = COUNT(float_subnormal_input), .expect = float_subnormal_expect,
   .expect_count = COUNT(float_subnormal_expect)},
  {.name = "half_bfloat_conversions", .body = half_conv_body, .expect = half_conv_expect, .expect_count = COUNT(half_conv_expect)},
  {.name = "half_arithmetic", .body = half_arith_body, .expect = half_arith_expect, .expect_count = COUNT(half_arith_expect)},
  {.name = "fma_half2", .body = fma_body, .expect = fma_expect, .expect_count = COUNT(fma_expect)},
  {.name = "fma_float", .body = fma_float_body, .expect = fma_float_expect, .expect_count = COUNT(fma_float_expect)},
  {.name = "math", .body = math_body, .expect = math_expect, .expect_count = COUNT(math_expect)},
  {.name = "math_transcendental", .body = transcendental_body, .input = transcendental_input,
   .input_count = COUNT(transcendental_input), .expect = transcendental_expect, .expect_count = COUNT(transcendental_expect),
   .device_ulp = 4},
  {.name = "as_type_vectors", .body = astype_body, .input = astype_input, .input_count = COUNT(astype_input),
   .expect = astype_expect, .expect_count = COUNT(astype_expect)},
  {.name = "records", .body = records_source, .full = 1, .input = records_input, .input_count = COUNT(records_input),
   .expect = records_expect, .expect_count = COUNT(records_expect)},
  {.name = "pointer_to_pointer", .body = pointers_body, .input = pointers_input, .input_count = COUNT(pointers_input),
   .expect = pointers_expect, .expect_count = COUNT(pointers_expect)}
};

const size_t msl_value_case_count = COUNT(msl_value_cases);
