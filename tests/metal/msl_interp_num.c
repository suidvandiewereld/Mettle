#include "msl_interp_internal.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

const MslScalarInfo msl_scalars[MSL_S_COUNT] = {
  {"bool", 1, 1, 0, 0},
  {"char", 1, 1, 1, 0},
  {"uchar", 1, 1, 0, 0},
  {"short", 2, 1, 1, 0},
  {"ushort", 2, 1, 0, 0},
  {"int", 4, 1, 1, 0},
  {"uint", 4, 1, 0, 0},
  {"long", 8, 1, 1, 0},
  {"ulong", 8, 1, 0, 0},
  {"half", 2, 0, 1, 1},
  {"bfloat", 2, 0, 1, 1},
  {"float", 4, 0, 1, 1}
};

float msl_bits_to_float(uint32_t bits) {
  float value;
  memcpy(&value, &bits, sizeof value);
  return value;
}

uint32_t msl_float_to_bits(float value) {
  uint32_t bits;
  memcpy(&bits, &value, sizeof bits);
  return bits;
}

float msl_round_float(double value) {
  volatile float rounded = (float)value;
  return rounded;
}

uint32_t msl_round_format(double value, int sticky, int mant_bits, int exp_bits) {
  uint32_t exp_max = (1u << exp_bits) - 1u;
  int bias = (1 << (exp_bits - 1)) - 1;
  uint32_t sign = signbit(value) ? 1u << (mant_bits + exp_bits) : 0u;
  double magnitude = fabs(value);
  int exponent = 0;
  int lsb;
  int lsb_min = 1 - bias - mant_bits;
  double scaled;
  double whole;
  double frac;
  uint64_t n;
  uint32_t field;
  if (isnan(value)) {
    return sign | (exp_max << mant_bits) | (1u << (mant_bits - 1));
  }
  if (isinf(value)) {
    return sign | (exp_max << mant_bits);
  }
  if (magnitude == 0.0) {
    return sign;
  }
  if (sign) {
    sticky = -sticky;
  }
  frexp(magnitude, &exponent);
  lsb = exponent - 1 - mant_bits;
  if (lsb < lsb_min) {
    lsb = lsb_min;
  }
  scaled = ldexp(magnitude, -lsb);
  whole = floor(scaled);
  frac = scaled - whole;
  n = (uint64_t)whole;
  if (frac > 0.5 || (frac == 0.5 && (sticky > 0 || (sticky == 0 && (n & 1u))))) {
    n++;
  }
  if (n >> (mant_bits + 1)) {
    n >>= 1;
    lsb++;
  }
  if (n < (1ull << mant_bits)) {
    return sign | (uint32_t)n;
  }
  field = (uint32_t)(lsb + mant_bits + bias);
  if (field >= exp_max) {
    return sign | (exp_max << mant_bits);
  }
  return sign | (field << mant_bits) | (uint32_t)(n - (1ull << mant_bits));
}

uint16_t msl_float_to_half(float value) {
  uint32_t bits = msl_float_to_bits(value);
  if ((bits & 0x7f800000u) == 0x7f800000u && (bits & 0x007fffffu) != 0u) {
    return (uint16_t)(((bits >> 16) & 0x8000u) | 0x7e00u | ((bits >> 13) & 0x3ffu));
  }
  return (uint16_t)msl_round_format((double)value, 0, 10, 5);
}

float msl_half_to_float(uint16_t bits) {
  uint32_t sign = ((uint32_t)bits & 0x8000u) << 16;
  uint32_t exponent = ((uint32_t)bits >> 10) & 0x1fu;
  uint32_t mant = (uint32_t)bits & 0x3ffu;
  if (exponent == 0x1fu) {
    return msl_bits_to_float(sign | 0x7f800000u | (mant << 13));
  }
  if (exponent == 0u) {
    float magnitude = (float)ldexp((double)mant, -24);
    return sign ? -magnitude : magnitude;
  }
  return msl_bits_to_float(sign | ((exponent + 112u) << 23) | (mant << 13));
}

uint16_t msl_float_to_bfloat(float value) {
  uint32_t bits = msl_float_to_bits(value);
  if ((bits & 0x7f800000u) == 0x7f800000u && (bits & 0x007fffffu) != 0u) {
    return (uint16_t)((bits >> 16) | 0x0040u);
  }
  return (uint16_t)msl_round_format((double)value, 0, 7, 8);
}

float msl_bfloat_to_float(uint16_t bits) {
  return msl_bits_to_float((uint32_t)bits << 16);
}

double msl_float_kind_to_double(int scalar, uint64_t bits) {
  if (scalar == MSL_S_HALF) {
    return (double)msl_half_to_float((uint16_t)bits);
  }
  if (scalar == MSL_S_BFLOAT) {
    return (double)msl_bfloat_to_float((uint16_t)bits);
  }
  return (double)msl_bits_to_float((uint32_t)bits);
}

uint64_t msl_double_to_float_kind(int scalar, double value, int sticky) {
  if (scalar == MSL_S_HALF) {
    return msl_round_format(value, sticky, 10, 5);
  }
  if (scalar == MSL_S_BFLOAT) {
    return msl_round_format(value, sticky, 7, 8);
  }
  if (sticky != 0 && !isnan(value)) {
    return msl_round_format(value, sticky, 23, 8);
  }
  return msl_float_to_bits(msl_round_float(value));
}

static uint64_t sign_extend(uint64_t raw, int bits) {
  uint64_t mask = bits == 64 ? ~0ull : ((1ull << bits) - 1u);
  uint64_t value = raw & mask;
  if (bits < 64 && (value >> (bits - 1)) & 1u) {
    value |= ~mask;
  }
  return value;
}

uint64_t msl_canonical(int scalar, uint64_t raw) {
  switch (scalar) {
    case MSL_S_BOOL: return raw != 0;
    case MSL_S_CHAR: return sign_extend(raw, 8);
    case MSL_S_UCHAR: return raw & 0xffu;
    case MSL_S_SHORT: return sign_extend(raw, 16);
    case MSL_S_USHORT: return raw & 0xffffu;
    case MSL_S_INT: return sign_extend(raw, 32);
    case MSL_S_UINT: return raw & 0xffffffffu;
    case MSL_S_HALF: return raw & 0xffffu;
    case MSL_S_BFLOAT: return raw & 0xffffu;
    case MSL_S_FLOAT: return raw & 0xffffffffu;
    default: return raw;
  }
}

static int add_overflows(int64_t a, int64_t b) {
  return (b > 0 && a > INT64_MAX - b) || (b < 0 && a < INT64_MIN - b);
}

static int sub_overflows(int64_t a, int64_t b) {
  return (b < 0 && a > INT64_MAX + b) || (b > 0 && a < INT64_MIN + b);
}

static int mul_overflows(int64_t a, int64_t b) {
  if (a == 0 || b == 0) {
    return 0;
  }
  if (a == -1) {
    return b == INT64_MIN;
  }
  if (b == -1) {
    return a == INT64_MIN;
  }
  if (a > 0) {
    return b > 0 ? a > INT64_MAX / b : b < INT64_MIN / a;
  }
  return b > 0 ? a < INT64_MIN / b : a < INT64_MAX / b;
}

static int fail_msg(char *msg, size_t msg_size, const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  msl_vformat_error(msg, msg_size, fmt, ap);
  va_end(ap);
  return 0;
}

static int compare_op(int op, int less, int equal, uint64_t *out) {
  switch (op) {
    case MSL_BIN_EQ: *out = equal; return 1;
    case MSL_BIN_NE: *out = !equal; return 1;
    case MSL_BIN_LT: *out = less; return 1;
    case MSL_BIN_LE: *out = less || equal; return 1;
    case MSL_BIN_GT: *out = !less && !equal; return 1;
    case MSL_BIN_GE: *out = !less; return 1;
    default: return 0;
  }
}

static int signed_binary(int bits, int op, int64_t x, int64_t y, uint64_t *out, char *msg, size_t msg_size) {
  int64_t lo = bits == 32 ? INT32_MIN : INT64_MIN;
  int64_t hi = bits == 32 ? INT32_MAX : INT64_MAX;
  const char *tname = bits == 32 ? "int" : "long";
  int64_t r = 0;
  int overflow = 0;
  if (compare_op(op, x < y, x == y, out)) {
    return 1;
  }
  switch (op) {
    case MSL_BIN_ADD: overflow = bits == 64 ? add_overflows(x, y) : 0; r = overflow ? 0 : x + y; break;
    case MSL_BIN_SUB: overflow = bits == 64 ? sub_overflows(x, y) : 0; r = overflow ? 0 : x - y; break;
    case MSL_BIN_MUL: overflow = bits == 64 ? mul_overflows(x, y) : 0; r = overflow ? 0 : x * y; break;
    case MSL_BIN_DIV:
      if (y == 0) return fail_msg(msg, msg_size, "%s division by zero", tname);
      if (x == lo && y == -1) return fail_msg(msg, msg_size, "%s division overflows (%lld / -1)", tname, (long long)x);
      r = x / y;
      break;
    case MSL_BIN_MOD:
      if (y == 0) return fail_msg(msg, msg_size, "%s remainder by zero", tname);
      if (x < 0 || y < 0) return fail_msg(msg, msg_size, "%% with a negative operand (%lld %% %lld)", (long long)x, (long long)y);
      r = x % y;
      break;
    case MSL_BIN_AND: r = x & y; break;
    case MSL_BIN_OR: r = x | y; break;
    case MSL_BIN_XOR: r = x ^ y; break;
    default: return fail_msg(msg, msg_size, "bad integer operator");
  }
  if (overflow || r < lo || r > hi) {
    static const char *const names[] = {"+", "-", "*"};
    return fail_msg(msg, msg_size, "signed %s overflow in %lld %s %lld", tname, (long long)x, names[op], (long long)y);
  }
  *out = (uint64_t)r;
  return 1;
}

static int unsigned_binary(int bits, int op, uint64_t x, uint64_t y, uint64_t *out, char *msg, size_t msg_size) {
  uint64_t mask = bits == 32 ? 0xffffffffull : ~0ull;
  uint64_t r = 0;
  if (compare_op(op, x < y, x == y, out)) {
    return 1;
  }
  switch (op) {
    case MSL_BIN_ADD: r = x + y; break;
    case MSL_BIN_SUB: r = x - y; break;
    case MSL_BIN_MUL: r = x * y; break;
    case MSL_BIN_DIV:
      if (y == 0) return fail_msg(msg, msg_size, "unsigned division by zero");
      r = x / y;
      break;
    case MSL_BIN_MOD:
      if (y == 0) return fail_msg(msg, msg_size, "unsigned remainder by zero");
      r = x % y;
      break;
    case MSL_BIN_AND: r = x & y; break;
    case MSL_BIN_OR: r = x | y; break;
    case MSL_BIN_XOR: r = x ^ y; break;
    default: return fail_msg(msg, msg_size, "bad integer operator");
  }
  *out = r & mask;
  return 1;
}

int msl_int_binary(int scalar, int op, uint64_t a, uint64_t b, uint64_t *out, char *msg, size_t msg_size) {
  switch (scalar) {
    case MSL_S_BOOL:
      if (compare_op(op, a < b, a == b, out)) return 1;
      return fail_msg(msg, msg_size, "bad bool operator");
    case MSL_S_INT: return signed_binary(32, op, (int64_t)a, (int64_t)b, out, msg, msg_size);
    case MSL_S_LONG: return signed_binary(64, op, (int64_t)a, (int64_t)b, out, msg, msg_size);
    case MSL_S_UINT: return unsigned_binary(32, op, a, b, out, msg, msg_size);
    case MSL_S_ULONG: return unsigned_binary(64, op, a, b, out, msg, msg_size);
    default: return fail_msg(msg, msg_size, "integer operator on an unpromoted type");
  }
}

static int64_t arithmetic_shift_right(int64_t x, unsigned count) {
  if (x >= 0) {
    return x >> count;
  }
  return -1 - ((-1 - x) >> count);
}

int msl_int_shift(int scalar, int left, uint64_t a, int count_scalar, uint64_t count, uint64_t *out, char *msg, size_t msg_size) {
  int bits = (int)msl_scalars[scalar].size * 8;
  int is_signed = msl_scalars[scalar].is_signed;
  uint64_t mask = bits == 64 ? ~0ull : ((1ull << bits) - 1u);
  if (msl_scalars[count_scalar].is_signed && (int64_t)count < 0) {
    return fail_msg(msg, msg_size, "negative shift count %lld", (long long)(int64_t)count);
  }
  if (count >= (uint64_t)bits) {
    return fail_msg(msg, msg_size, "shift count %llu is not less than the %d-bit operand width", (unsigned long long)count, bits);
  }
  if (!left) {
    *out = is_signed ? (uint64_t)arithmetic_shift_right((int64_t)a, (unsigned)count) : (a & mask) >> count;
    return 1;
  }
  if (!is_signed) {
    *out = (a << count) & mask;
    return 1;
  }
  if ((int64_t)a < 0) {
    return fail_msg(msg, msg_size, "left shift of negative value %lld", (long long)(int64_t)a);
  }
  if (count > 0 && ((a & mask) >> (bits - (int)count)) != 0u) {
    return fail_msg(msg, msg_size, "signed left shift %lld << %llu overflows", (long long)(int64_t)a, (unsigned long long)count);
  }
  *out = sign_extend(a << count, bits);
  return 1;
}

uint64_t msl_flush_subnormal(int scalar, uint64_t bits) {
  if (scalar == MSL_S_FLOAT && (bits & 0x7f800000u) == 0) {
    return bits & 0x80000000u;
  }
  return bits;
}

int msl_float_binary(int scalar, int op, uint64_t a, uint64_t b, uint64_t *out, char *msg, size_t msg_size) {
  double x = msl_float_kind_to_double(scalar, msl_flush_subnormal(scalar, a));
  double y = msl_float_kind_to_double(scalar, msl_flush_subnormal(scalar, b));
  double r;
  switch (op) {
    case MSL_BIN_ADD: r = x + y; break;
    case MSL_BIN_SUB: r = x - y; break;
    case MSL_BIN_MUL: r = x * y; break;
    case MSL_BIN_DIV: r = x / y; break;
    case MSL_BIN_EQ: *out = x == y; return 1;
    case MSL_BIN_NE: *out = x != y; return 1;
    case MSL_BIN_LT: *out = x < y; return 1;
    case MSL_BIN_LE: *out = x <= y; return 1;
    case MSL_BIN_GT: *out = x > y; return 1;
    case MSL_BIN_GE: *out = x >= y; return 1;
    default: return fail_msg(msg, msg_size, "operator needs integers");
  }
  *out = msl_flush_subnormal(scalar, msl_double_to_float_kind(scalar, r, 0));
  return 1;
}

int msl_unary(int scalar, int op, uint64_t a, uint64_t *out, char *msg, size_t msg_size) {
  if (op == MSL_UN_LNOT) {
    *out = !a;
    return 1;
  }
  if (msl_scalars[scalar].is_float) {
    *out = a ^ (scalar == MSL_S_FLOAT ? 0x80000000ull : 0x8000ull);
    return 1;
  }
  if (op == MSL_UN_BNOT) {
    *out = msl_canonical(scalar, ~a);
    return 1;
  }
  if (msl_scalars[scalar].is_signed) {
    int64_t x = (int64_t)a;
    if ((scalar == MSL_S_INT && x == INT32_MIN) || (scalar == MSL_S_LONG && x == INT64_MIN)) {
      return fail_msg(msg, msg_size, "signed negation overflows for %lld", (long long)x);
    }
    *out = (uint64_t)(-x);
    return 1;
  }
  *out = msl_canonical(scalar, (uint64_t)0 - a);
  return 1;
}

static int float_to_int(int to, double d, uint64_t *out, char *msg, size_t msg_size) {
  int bits = (int)msl_scalars[to].size * 8;
  double limit = ldexp(1.0, msl_scalars[to].is_signed ? bits - 1 : bits);
  double low = msl_scalars[to].is_signed ? -limit : 0.0;
  if (to == MSL_S_BOOL) {
    *out = d != 0.0;
    return 1;
  }
  if (isnan(d) || !(trunc(d) >= low) || !(trunc(d) < limit)) {
    return fail_msg(msg, msg_size, "float value %g is out of range for %s", d, msl_scalars[to].name);
  }
  d = trunc(d);
  if (msl_scalars[to].is_signed) {
    *out = msl_canonical(to, (uint64_t)(int64_t)d);
  } else {
    *out = (uint64_t)d;
  }
  return 1;
}

static uint64_t int_to_float(int to, int is_signed, uint64_t v) {
  int negative = is_signed && (int64_t)v < 0;
  uint64_t magnitude = negative ? (uint64_t)0 - v : v;
  double d;
  int sticky = 0;
  if (to == MSL_S_FLOAT) {
    float f = is_signed ? (float)(int64_t)v : (float)v;
    return msl_float_to_bits(f);
  }
  d = (double)magnitude;
  if (d >= 18446744073709551616.0) {
    sticky = -1;
  } else {
    uint64_t back = (uint64_t)d;
    sticky = magnitude > back ? 1 : (magnitude < back ? -1 : 0);
  }
  if (negative) {
    d = -d;
    sticky = -sticky;
  }
  return msl_double_to_float_kind(to, d, sticky);
}

static uint64_t float_to_float(int from, int to, uint64_t v) {
  float f = msl_bits_to_float((uint32_t)msl_double_to_float_kind(MSL_S_FLOAT, msl_float_kind_to_double(from, v), 0));
  if (from == MSL_S_FLOAT) {
    f = msl_bits_to_float((uint32_t)v);
  }
  if (to == MSL_S_HALF) {
    return msl_float_to_half(f);
  }
  if (to == MSL_S_BFLOAT) {
    return msl_float_to_bfloat(f);
  }
  return msl_float_to_bits(f);
}

int msl_convert(int from, int to, uint64_t value, uint64_t *out, char *msg, size_t msg_size) {
  int from_float = msl_scalars[from].is_float;
  int to_float = msl_scalars[to].is_float;
  if (!from_float && !to_float) {
    *out = to == MSL_S_BOOL ? (uint64_t)(value != 0) : msl_canonical(to, value);
    return 1;
  }
  if (!from_float) {
    *out = int_to_float(to, msl_scalars[from].is_signed, value);
    return 1;
  }
  if (!to_float) {
    return float_to_int(to, msl_float_kind_to_double(from, value), out, msg, msg_size);
  }
  *out = float_to_float(from, to, value);
  return 1;
}

static void two_sum(double a, double b, double *sum, double *err) {
  volatile double s = a + b;
  volatile double bb = s - a;
  volatile double aa = s - bb;
  *sum = s;
  *err = (a - aa) + (b - bb);
}

uint16_t msl_fma_half(uint16_t a, uint16_t b, uint16_t c) {
  double x = (double)msl_half_to_float(a);
  double y = (double)msl_half_to_float(b);
  double z = (double)msl_half_to_float(c);
  double product = x * y;
  double sum;
  double err;
  int sticky;
  if (isnan(x) || isnan(y) || isnan(z)) {
    return 0x7e00u;
  }
  if (!isfinite(product) || !isfinite(z)) {
    return (uint16_t)msl_round_format(product + z, 0, 10, 5);
  }
  two_sum(product, z, &sum, &err);
  sticky = err > 0.0 ? 1 : (err < 0.0 ? -1 : 0);
  if (sum == 0.0 && sticky == 0) {
    double exact_zero = product + z;
    return (uint16_t)msl_round_format(exact_zero, 0, 10, 5);
  }
  return (uint16_t)msl_round_format(sum, sticky, 10, 5);
}

static uint32_t fma_float_rounded(uint32_t a, uint32_t b, uint32_t c) {
  double x = (double)msl_bits_to_float(a);
  double y = (double)msl_bits_to_float(b);
  double z = (double)msl_bits_to_float(c);
  double product = x * y;
  double sum;
  double err;
  int sticky;
  if (isnan(x) || isnan(y) || isnan(z)) {
    return 0x7fc00000u;
  }
  if (!isfinite(product) || !isfinite(z)) {
    return msl_round_format(product + z, 0, 23, 8);
  }
  two_sum(product, z, &sum, &err);
  sticky = err > 0.0 ? 1 : (err < 0.0 ? -1 : 0);
  if (sum == 0.0 && sticky == 0) {
    double exact_zero = product + z;
    return msl_round_format(exact_zero, 0, 23, 8);
  }
  return msl_round_format(sum, sticky, 23, 8);
}

uint32_t msl_fma_float(uint32_t a, uint32_t b, uint32_t c) {
  uint32_t x = (uint32_t)msl_flush_subnormal(MSL_S_FLOAT, a);
  uint32_t y = (uint32_t)msl_flush_subnormal(MSL_S_FLOAT, b);
  uint32_t z = (uint32_t)msl_flush_subnormal(MSL_S_FLOAT, c);
  return (uint32_t)msl_flush_subnormal(MSL_S_FLOAT, fma_float_rounded(x, y, z));
}

double msl_math_unary(int builtin, double x) {
  switch (builtin) {
    case MSL_BI_SQRT: return sqrt(x);
    case MSL_BI_RSQRT: return 1.0 / sqrt(x);
    case MSL_BI_SIN: return sin(x);
    case MSL_BI_COS: return cos(x);
    case MSL_BI_LOG: return log(x);
    case MSL_BI_EXP: return exp(x);
    default: return fabs(x);
  }
}
