#include "msl_interp_compile.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef MslExpr (*MslBuiltinFn)(MslCompiler *c, int id, int line);

static MslExpr math_unary(MslCompiler *c, int id, int line);
static MslExpr math_fmod(MslCompiler *c, int id, int line);
static MslExpr math_fma(MslCompiler *c, int id, int line);
static MslExpr atomic_access(MslCompiler *c, int id, int line);
static MslExpr atomic_cas(MslCompiler *c, int id, int line);
static MslExpr atomic_fence(MslCompiler *c, int id, int line);
static MslExpr barrier(MslCompiler *c, int id, int line);
static MslExpr simd_reduce(MslCompiler *c, int id, int line);
static MslExpr simd_lane(MslCompiler *c, int id, int line);
static MslExpr simd_bool(MslCompiler *c, int id, int line);
static MslExpr simd_active(MslCompiler *c, int id, int line);
static MslExpr matrix_io(MslCompiler *c, int id, int line);
static MslExpr matrix_mac(MslCompiler *c, int id, int line);

static const struct {
  const char *name;
  MslBuiltinFn fn;
  int id;
} msl_builtins[] = {
  {"precise::sqrt", math_unary, MSL_BI_SQRT}, {"precise::rsqrt", math_unary, MSL_BI_RSQRT},
  {"precise::sin", math_unary, MSL_BI_SIN}, {"precise::cos", math_unary, MSL_BI_COS},
  {"precise::log", math_unary, MSL_BI_LOG}, {"precise::exp", math_unary, MSL_BI_EXP},
  {"fast::sqrt", math_unary, MSL_BI_SQRT}, {"fast::rsqrt", math_unary, MSL_BI_RSQRT},
  {"fast::sin", math_unary, MSL_BI_SIN}, {"fast::cos", math_unary, MSL_BI_COS},
  {"fast::log", math_unary, MSL_BI_LOG}, {"fast::exp", math_unary, MSL_BI_EXP},
  {"fabs", math_unary, MSL_BI_FABS}, {"fmod", math_fmod, MSL_BI_FMOD}, {"fma", math_fma, MSL_BI_FMA},
  {"atomic_load_explicit", atomic_access, MSL_BI_ATOMIC_LOAD},
  {"atomic_store_explicit", atomic_access, MSL_BI_ATOMIC_STORE},
  {"atomic_fetch_add_explicit", atomic_access, MSL_BI_ATOMIC_ADD},
  {"atomic_fetch_sub_explicit", atomic_access, MSL_BI_ATOMIC_SUB},
  {"atomic_fetch_min_explicit", atomic_access, MSL_BI_ATOMIC_MIN},
  {"atomic_fetch_max_explicit", atomic_access, MSL_BI_ATOMIC_MAX},
  {"atomic_fetch_and_explicit", atomic_access, MSL_BI_ATOMIC_AND},
  {"atomic_fetch_or_explicit", atomic_access, MSL_BI_ATOMIC_OR},
  {"atomic_fetch_xor_explicit", atomic_access, MSL_BI_ATOMIC_XOR},
  {"atomic_exchange_explicit", atomic_access, MSL_BI_ATOMIC_XCHG},
  {"atomic_compare_exchange_weak_explicit", atomic_cas, MSL_BI_ATOMIC_CAS},
  {"atomic_thread_fence", atomic_fence, 0},
  {"threadgroup_barrier", barrier, MSL_SY_BARRIER}, {"simdgroup_barrier", barrier, MSL_SY_SIMD_BARRIER},
  {"simd_sum", simd_reduce, MSL_SY_SUM}, {"simd_min", simd_reduce, MSL_SY_MIN}, {"simd_max", simd_reduce, MSL_SY_MAX},
  {"simd_prefix_inclusive_sum", simd_reduce, MSL_SY_PREFIX_INC},
  {"simd_prefix_exclusive_sum", simd_reduce, MSL_SY_PREFIX_EXC},
  {"simd_broadcast", simd_lane, MSL_SY_BROADCAST}, {"simd_shuffle", simd_lane, MSL_SY_SHUFFLE},
  {"simd_ballot", simd_bool, MSL_SY_BALLOT}, {"simd_any", simd_bool, MSL_SY_ANY}, {"simd_all", simd_bool, MSL_SY_ALL},
  {"simd_active_threads_mask", simd_active, MSL_SY_ACTIVE},
  {"simdgroup_load", matrix_io, MSL_SY_MAT_LOAD}, {"simdgroup_store", matrix_io, MSL_SY_MAT_STORE},
  {"simdgroup_multiply_accumulate", matrix_mac, MSL_SY_MAT_MAC}
};

int mslc_is_builtin(const char *name) {
  size_t i;
  for (i = 0; i < sizeof(msl_builtins) / sizeof(msl_builtins[0]); i++) {
    if (strcmp(msl_builtins[i].name, name) == 0) {
      return 1;
    }
  }
  return 0;
}

MslExpr mslc_builtin_call(MslCompiler *c, const MslToken *name_tok, const char *name, int line) {
  size_t i;
  (void)name_tok;
  for (i = 0; i < sizeof(msl_builtins) / sizeof(msl_builtins[0]); i++) {
    if (strcmp(msl_builtins[i].name, name) == 0) {
      MslExpr out;
      mslc_expect(c, "(");
      out = msl_builtins[i].fn(c, msl_builtins[i].id, line);
      mslc_expect(c, ")");
      out.is_call = 1;
      return out;
    }
  }
  mslc_fail(c, line, "unknown function '%s'", name);
  return mslc_value_expr(NULL);
}

static MslExpr void_expr(MslCompiler *c) {
  MslExpr e = mslc_value_expr(c->prog->void_type);
  e.kind = MSL_E_VOID;
  return e;
}

static MslExpr arg_value(MslCompiler *c, int first, int line) {
  MslExpr e;
  if (!first) {
    mslc_expect(c, ",");
  }
  e = mslc_expr(c);
  mslc_rvalue(c, &e, line);
  return e;
}

static void arg_typed(MslCompiler *c, int first, const MslType *want, int line, const char *what) {
  MslExpr e = arg_value(c, first, line);
  char a[160];
  char b[160];
  if (e.type != want) {
    mslc_fail(c, line, "%s has type '%s', not '%s'", what, mslc_tname(e.type, a, sizeof a), mslc_tname(want, b, sizeof b));
  }
}

static uint64_t arg_const(MslCompiler *c, int first, int kind, int line, const char *what) {
  MslExpr e;
  if (!first) {
    mslc_expect(c, ",");
  }
  e = mslc_expr(c);
  if (e.kind != MSL_E_CONST || e.type->kind != kind) {
    mslc_fail(c, line, "%s must be a constant of the right kind", what);
  }
  return e.value;
}

static size_t emit_call(MslCompiler *c, int op, int id, const MslType *t1, const MslType *t2, int line) {
  size_t insn = mslc_emit(c, op, line);
  mslc_insn(c, insn)->a = id;
  mslc_insn(c, insn)->t1 = t1;
  mslc_insn(c, insn)->t2 = t2;
  return insn;
}

static const MslType *scalar(MslCompiler *c, int s) {
  return c->prog->scalar_types[s];
}

static MslExpr math_unary(MslCompiler *c, int id, int line) {
  arg_typed(c, 1, scalar(c, MSL_S_FLOAT), line, "math argument");
  emit_call(c, MSL_OP_BUILTIN, id, scalar(c, MSL_S_FLOAT), NULL, line);
  return mslc_value_expr(scalar(c, MSL_S_FLOAT));
}

static MslExpr math_fmod(MslCompiler *c, int id, int line) {
  arg_typed(c, 1, scalar(c, MSL_S_FLOAT), line, "fmod argument");
  arg_typed(c, 0, scalar(c, MSL_S_FLOAT), line, "fmod argument");
  emit_call(c, MSL_OP_BUILTIN, id, scalar(c, MSL_S_FLOAT), NULL, line);
  return mslc_value_expr(scalar(c, MSL_S_FLOAT));
}

static MslExpr math_fma(MslCompiler *c, int id, int line) {
  const MslType *half2 = msl_vector_type(c->prog, MSL_S_HALF, 2, 0);
  const MslType *real = scalar(c, MSL_S_FLOAT);
  MslExpr first = arg_value(c, 1, line);
  const MslType *want = first.type == real ? real : half2;
  char a[160];
  if (first.type != want) {
    mslc_fail(c, line, "fma argument has type '%s', not 'float' or 'half2'",
              mslc_tname(first.type, a, sizeof a));
  }
  arg_typed(c, 0, want, line, "fma argument");
  arg_typed(c, 0, want, line, "fma argument");
  emit_call(c, MSL_OP_BUILTIN, id, want, NULL, line);
  return mslc_value_expr(want);
}

static const MslType *atomic_pointer(MslCompiler *c, int line) {
  MslExpr e = arg_value(c, 1, line);
  char tn[160];
  if (e.type->kind != MSL_K_POINTER || e.type->elem->kind != MSL_K_ATOMIC ||
      (e.type->space != MSL_SP_DEVICE && e.type->space != MSL_SP_THREADGROUP)) {
    mslc_fail(c, line, "atomic function needs a device or threadgroup atomic_uint pointer, not '%s'", mslc_tname(e.type, tn, sizeof tn));
  }
  return e.type;
}

static MslExpr atomic_access(MslCompiler *c, int id, int line) {
  const MslType *pointer = atomic_pointer(c, line);
  const MslType *uint_type = scalar(c, MSL_S_UINT);
  if (id != MSL_BI_ATOMIC_LOAD) {
    arg_typed(c, 0, uint_type, line, "atomic operand");
  }
  arg_const(c, 0, MSL_K_ORDER, line, "memory order");
  emit_call(c, MSL_OP_BUILTIN, id, pointer, NULL, line);
  return id == MSL_BI_ATOMIC_STORE ? void_expr(c) : mslc_value_expr(uint_type);
}

static MslExpr atomic_cas(MslCompiler *c, int id, int line) {
  const MslType *pointer = atomic_pointer(c, line);
  const MslType *uint_type = scalar(c, MSL_S_UINT);
  arg_typed(c, 0, msl_pointer_type(c->prog, MSL_SP_THREAD, uint_type), line, "expected-value pointer");
  arg_typed(c, 0, uint_type, line, "desired value");
  arg_const(c, 0, MSL_K_ORDER, line, "success order");
  arg_const(c, 0, MSL_K_ORDER, line, "failure order");
  emit_call(c, MSL_OP_BUILTIN, id, pointer, NULL, line);
  return mslc_value_expr(scalar(c, MSL_S_BOOL));
}

static MslExpr atomic_fence(MslCompiler *c, int id, int line) {
  (void)id;
  arg_const(c, 1, MSL_K_FLAGS, line, "fence flags");
  arg_const(c, 0, MSL_K_ORDER, line, "fence order");
  arg_const(c, 0, MSL_K_SCOPE, line, "fence scope");
  return void_expr(c);
}

static MslExpr barrier(MslCompiler *c, int id, int line) {
  arg_const(c, 1, MSL_K_FLAGS, line, "barrier flags");
  emit_call(c, MSL_OP_SYNC, id, NULL, NULL, line);
  return void_expr(c);
}

static void require_simd_value(MslCompiler *c, const MslType *type, int line) {
  char tn[160];
  if (type != scalar(c, MSL_S_UINT) && type != scalar(c, MSL_S_FLOAT)) {
    mslc_fail(c, line, "SIMD-group function on '%s' is outside the dialect (uint and float only)", mslc_tname(type, tn, sizeof tn));
  }
}

static MslExpr simd_reduce(MslCompiler *c, int id, int line) {
  MslExpr value = arg_value(c, 1, line);
  require_simd_value(c, value.type, line);
  emit_call(c, MSL_OP_SYNC, id, value.type, NULL, line);
  return mslc_value_expr(value.type);
}

static MslExpr simd_lane(MslCompiler *c, int id, int line) {
  MslExpr value = arg_value(c, 1, line);
  require_simd_value(c, value.type, line);
  arg_typed(c, 0, scalar(c, MSL_S_USHORT), line, "lane argument");
  emit_call(c, MSL_OP_SYNC, id, value.type, NULL, line);
  return mslc_value_expr(value.type);
}

static MslExpr simd_bool(MslCompiler *c, int id, int line) {
  arg_typed(c, 1, scalar(c, MSL_S_BOOL), line, "predicate");
  emit_call(c, MSL_OP_SYNC, id, scalar(c, MSL_S_BOOL), NULL, line);
  return mslc_value_expr(id == MSL_SY_BALLOT ? c->prog->vote_type : scalar(c, MSL_S_BOOL));
}

static MslExpr simd_active(MslCompiler *c, int id, int line) {
  emit_call(c, MSL_OP_SYNC, id, NULL, NULL, line);
  return mslc_value_expr(c->prog->vote_type);
}

static const MslType *matrix_ref(MslCompiler *c, int first, int line) {
  MslExpr e;
  char tn[160];
  size_t insn;
  if (!first) {
    mslc_expect(c, ",");
  }
  e = mslc_expr(c);
  if (e.type == NULL || e.type->kind != MSL_K_MATRIX) {
    mslc_fail(c, line, "expected a simdgroup matrix, not '%s'", mslc_tname(e.type, tn, sizeof tn));
  }
  if (e.kind == MSL_E_VAR) {
    insn = mslc_emit(c, MSL_OP_ADDR_LOCAL, line);
    mslc_insn(c, insn)->a = e.local;
    mslc_insn(c, insn)->b = MSL_SP_THREAD;
  } else if (e.kind != MSL_E_MEM || e.space != MSL_SP_THREAD) {
    mslc_fail(c, line, "simdgroup matrix operand must be a thread variable");
  }
  return e.type;
}

static MslExpr matrix_io(MslCompiler *c, int id, int line) {
  const MslType *matrix = matrix_ref(c, 1, line);
  MslExpr pointer = arg_value(c, 0, line);
  char tn[160];
  if (pointer.type->kind != MSL_K_POINTER || pointer.type->elem != matrix->elem ||
      (pointer.type->space != MSL_SP_DEVICE && pointer.type->space != MSL_SP_THREADGROUP)) {
    mslc_fail(c, line, "%s needs a device or threadgroup %s pointer, not '%s'", id == MSL_SY_MAT_LOAD ? "simdgroup_load" : "simdgroup_store",
              matrix->elem->name, mslc_tname(pointer.type, tn, sizeof tn));
  }
  arg_typed(c, 0, scalar(c, MSL_S_ULONG), line, "elements per row");
  arg_typed(c, 0, msl_vector_type(c->prog, MSL_S_ULONG, 2, 0), line, "matrix origin");
  arg_typed(c, 0, scalar(c, MSL_S_BOOL), line, "transpose flag");
  emit_call(c, MSL_OP_SYNC, id, matrix, pointer.type, line);
  return void_expr(c);
}

static MslExpr matrix_mac(MslCompiler *c, int id, int line) {
  const MslType **types = (const MslType **)mslc_alloc(c, 4 * sizeof(MslType *));
  size_t insn;
  int i;
  for (i = 0; i < 4; i++) {
    types[i] = matrix_ref(c, i == 0, line);
  }
  if (types[0]->scalar == MSL_S_BFLOAT || types[3]->scalar == MSL_S_BFLOAT) {
    mslc_fail(c, line, "simdgroup_multiply_accumulate C and D must be float or half matrices");
  }
  insn = emit_call(c, MSL_OP_SYNC, id, types[0], types[1], line);
  mslc_insn(c, insn)->p = types;
  return void_expr(c);
}

static int format_arg_ok(int conv, int longs, const MslType *type) {
  int s = type->kind == MSL_K_SCALAR ? type->scalar : -1;
  if (conv == 'd' || conv == 'i') {
    return longs ? s == MSL_S_LONG : (s == MSL_S_INT || s == MSL_S_SHORT || s == MSL_S_CHAR);
  }
  if (conv == 'u' || conv == 'x' || conv == 'X' || conv == 'o') {
    return longs ? s == MSL_S_ULONG : (s == MSL_S_UINT || s == MSL_S_USHORT || s == MSL_S_UCHAR);
  }
  if (strchr("fFeEgGaA", conv) != NULL) {
    return longs == 0 && (s == MSL_S_FLOAT || s == MSL_S_HALF);
  }
  if (conv == 'c') {
    return longs == 0 && (s == MSL_S_INT || s == MSL_S_CHAR || s == MSL_S_UCHAR);
  }
  return -1;
}

static void check_format(MslCompiler *c, const char *fmt, const MslType **types, size_t count, int line) {
  size_t used = 0;
  const char *p = fmt;
  char tn[160];
  while (*p != 0) {
    int longs = 0;
    int ok;
    if (*p++ != '%') {
      continue;
    }
    if (*p == '%') {
      p++;
      continue;
    }
    while (*p != 0 && strchr("-+ #0", *p) != NULL) {
      p++;
    }
    while (isdigit((unsigned char)*p)) {
      p++;
    }
    if (*p == '.') {
      p++;
      while (isdigit((unsigned char)*p)) {
        p++;
      }
    }
    while (*p == 'l') {
      longs++;
      p++;
    }
    if (longs > 2 || *p == 0) {
      mslc_fail(c, line, "malformed os_log conversion");
    }
    if (used >= count) {
      mslc_fail(c, line, "os_log format needs more arguments than the %u given", (unsigned)count);
    }
    ok = format_arg_ok(*p, longs, types[used]);
    if (ok < 0) {
      mslc_fail(c, line, "os_log conversion '%%%c' is outside the dialect", *p);
    }
    if (!ok) {
      mslc_fail(c, line, "os_log conversion '%%%c' does not match argument %u of type '%s'", *p, (unsigned)(used + 1),
                mslc_tname(types[used], tn, sizeof tn));
    }
    used++;
    p++;
  }
  if (used != count) {
    mslc_fail(c, line, "os_log format uses %u arguments but %u were given", (unsigned)used, (unsigned)count);
  }
}

MslExpr mslc_os_log(MslCompiler *c, int line) {
  MslLogInfo *info = (MslLogInfo *)mslc_alloc(c, sizeof(MslLogInfo));
  const MslType *types[32];
  const MslToken *fmt;
  size_t count = 0;
  size_t insn;
  c->pos++;
  mslc_expect(c, ".");
  if (mslc_accept(c, "log_fault")) {
    info->fault = 1;
  } else {
    mslc_expect(c, "log");
  }
  mslc_expect(c, "(");
  fmt = mslc_cur(c);
  if (fmt->kind != MSL_TK_STRING) {
    mslc_fail(c, line, "os_log needs a format string literal");
  }
  c->pos++;
  while (mslc_at(c, ",")) {
    MslExpr arg = arg_value(c, 0, line);
    if (count == 32) {
      mslc_fail(c, line, "too many os_log arguments");
    }
    types[count++] = arg.type;
  }
  mslc_expect(c, ")");
  check_format(c, fmt->string_value, types, count, line);
  info->format = fmt->string_value;
  info->arg_count = count;
  info->arg_types = (const MslType **)mslc_alloc(c, (count + 1) * sizeof(MslType *));
  memcpy(info->arg_types, types, count * sizeof(MslType *));
  insn = mslc_emit(c, MSL_OP_LOG, line);
  mslc_insn(c, insn)->p = info;
  {
    MslExpr out = void_expr(c);
    out.is_call = 1;
    return out;
  }
}
