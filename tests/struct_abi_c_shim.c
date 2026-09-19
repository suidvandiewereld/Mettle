#include <stdint.h>

typedef struct {
  int32_t a;
  int32_t b;
  int32_t c;
} ThreeI32;

typedef struct {
  uint8_t a;
  uint8_t b;
  uint8_t c;
} Odd3;

int32_t mettle_struct_abi_c_sum_three(ThreeI32 t) {
  return t.a + t.b + t.c;
}

ThreeI32 mettle_struct_abi_c_make_three(int32_t a, int32_t b, int32_t c) {
  ThreeI32 t;
  t.a = a;
  t.b = b;
  t.c = c;
  return t;
}

int32_t mettle_struct_abi_c_sum_odd3(Odd3 o) {
  return (int32_t)o.a + (int32_t)o.b + (int32_t)o.c;
}

Odd3 mettle_struct_abi_c_make_odd3(uint8_t a, uint8_t b, uint8_t c) {
  Odd3 o;
  o.a = a;
  o.b = b;
  o.c = c;
  return o;
}

typedef struct {
  int32_t a, b, c, d, e, f, g, h;
} Big32;

typedef struct {
  double x, y;
} TwoF64;

typedef struct {
  int64_t i;
  double d;
} MixedI64F64;

int32_t mettle_struct_abi_c_sum_big32(Big32 v) {
  return v.a + v.b + v.c + v.d + v.e + v.f + v.g + v.h;
}

int32_t mettle_struct_abi_c_sum_two_f64(TwoF64 v) {
  return (int32_t)(v.x + v.y);
}

int32_t mettle_struct_abi_c_sum_mixed(MixedI64F64 v) {
  return (int32_t)(v.i + (int64_t)v.d);
}

typedef struct {
  uint8_t r, g, b, a;
} Rgba;

typedef struct {
  int32_t a, b;
} PairI32;

typedef struct {
  float x, y;
} TwoF32;

int32_t mettle_struct_abi_c_sum_rgba(Rgba c) {
  return c.r + c.g * 10 + c.b * 100 + c.a * 1000;
}

Rgba mettle_struct_abi_c_make_rgba(uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
  Rgba c;
  c.r = r;
  c.g = g;
  c.b = b;
  c.a = a;
  return c;
}

int32_t mettle_struct_abi_c_sum_pair(PairI32 p) {
  return p.a * 7 + p.b * 13;
}

int32_t mettle_struct_abi_c_sum_two_f32(TwoF32 v) {
  return (int32_t)(v.x * 10.0f + v.y * 100.0f);
}

int32_t mettle_struct_abi_c_sum_spilled(int32_t a, int32_t b, int32_t c,
                                        int32_t d, int32_t e, int32_t f,
                                        PairI32 p, TwoF32 v, Rgba k) {
  return a + b + c + d + e + f + mettle_struct_abi_c_sum_pair(p) +
         mettle_struct_abi_c_sum_two_f32(v) + mettle_struct_abi_c_sum_rgba(k);
}
