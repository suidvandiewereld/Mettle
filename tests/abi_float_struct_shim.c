#include <stdint.h>

typedef struct { float f0; } OneF;
typedef struct { float f0; float f1; } TwoF;
typedef struct { float f0; double f1; } FD;

OneF abi_export_one(int32_t x);
TwoF abi_export_two(int32_t x);
int64_t abi_export_spill(FD a0, OneF a1, int8_t a2, OneF a3, int8_t a4, FD a5,
                         OneF a6, uint32_t a7, FD a8, int32_t w);

static int64_t f32b(float f) { union { float f; uint32_t u; } v; v.f = f; return v.u; }
static int64_t f64b(double d) { union { double d; int64_t u; } v; v.d = d; return v.u; }

int32_t mettle_abi_float_struct_drive(void) {
  OneF one = abi_export_one(10);
  if (one.f0 != 10.5f) { return 1; }
  TwoF two = abi_export_two(3);
  if (two.f0 != 3.25f || two.f1 != 6.5f) { return 2; }
  FD a0 = {1.5f, 2.5}; OneF a1 = {3.5f}; OneF a3 = {4.5f};
  FD a5 = {5.5f, 6.5}; OneF a6 = {7.5f}; FD a8 = {8.5f, 9.5};
  int64_t want[12] = {f32b(1.5f), f64b(2.5), f32b(3.5f), -3, f32b(4.5f), 7,
                      f32b(5.5f), f64b(6.5), f32b(7.5f), 123456, f32b(8.5f),
                      f64b(9.5)};
  for (int w = 0; w < 12; w++) {
    if (abi_export_spill(a0, a1, -3, a3, 7, a5, a6, 123456, a8, w) != want[w]) {
      return 10 + w;
    }
  }
  return 0;
}

int64_t mettle_abi_spill_sum(FD a0, OneF a1, int8_t a2, OneF a3, int8_t a4,
                             FD a5, OneF a6, uint32_t a7, FD a8) {
  return (int64_t)(a0.f0 + a0.f1 + a1.f0 + a2 + a3.f0 + a4 + a5.f0 + a5.f1 +
                   a6.f0 + a7 + a8.f0 * 100.0f + a8.f1 * 1000.0);
}
