#include "type_checker_internal.h"
#include "../ir/ir_explain_ledger.h"
#include <string.h>

struct TypeCheckerGuard {
  ASTNode *condition;
  int negated;
  const char *name;
  int has_min;
  int has_max;
  long long min;
  long long max;
};

typedef struct {
  int has_min;
  int has_max;
  long long min;
  long long max;
} Range;

#define TYPE_CHECKER_PROOF_CEILING 20000000LL

static Range range_unknown(void) {
  Range r;
  r.has_min = 0;
  r.has_max = 0;
  r.min = 0;
  r.max = 0;
  return r;
}

static Range range_exact(long long v) {
  Range r;
  r.has_min = 1;
  r.has_max = 1;
  r.min = v;
  r.max = v;
  return r;
}

static long long shift_right_floor(long long value, int bits) {
  unsigned long long magnitude;
  unsigned long long shifted;
  unsigned long long lost;
  if (bits <= 0) {
    return value;
  }
  if (value >= 0) {
    return value >> bits;
  }
  magnitude = (unsigned long long)(-(value + 1)) + 1ULL;
  shifted = magnitude >> bits;
  lost = magnitude & ((1ULL << bits) - 1ULL);
  if (lost != 0ULL) {
    shifted += 1ULL;
  }
  return -(long long)shifted;
}

static Range range_of_type(const Type *type) {
  Range r = range_unknown();
  long long min = 0;
  unsigned long long max = 0;
  if (!type) {
    return r;
  }
  if (type->refined_base && type->refine_has_range) {
    r.has_min = 1;
    r.has_max = 1;
    r.min = type->refine_min;
    r.max = type->refine_max;
    return r;
  }
  if (type->refined_base) {
    return range_of_type(type->refined_base);
  }
  if (type->kind == TYPE_ENUM ||
      !type_checker_integer_bounds(type, &min, &max)) {
    return r;
  }
  r.has_min = 1;
  r.min = min;
  if (max <= (unsigned long long)LLONG_MAX) {
    r.has_max = 1;
    r.max = (long long)max;
  }
  return r;
}

typedef struct {
  int has_min;
  int has_max;
  double min;
  double max;
  double err;
} FRange;

static FRange frange_unknown(void) {
  FRange r;
  r.has_min = 0;
  r.has_max = 0;
  r.min = 0.0;
  r.max = 0.0;
  r.err = 0.0;
  return r;
}

static FRange frange_exact(double v) {
  FRange r;
  r.has_min = 1;
  r.has_max = 1;
  r.min = v;
  r.max = v;
  r.err = 0.0;
  return r;
}

static double frange_eps(const Type *type) {
  if (type && type->name && strcmp(type->name, "float32") == 0) {
    return 1.1920929e-07;
  }
  return 2.220446049250313e-16;
}

#define FRANGE_NO_BOUND 1.0

static double frange_step(double value, int up) {
  unsigned long long bits;
  if (value == 0.0) {
    bits = 1;
    if (!up) {
      bits |= 0x8000000000000000ull;
    }
    memcpy(&value, &bits, sizeof(value));
    return value;
  }
  memcpy(&bits, &value, sizeof(bits));
  if ((value > 0.0) == (up != 0)) {
    bits++;
  } else {
    bits--;
  }
  memcpy(&value, &bits, sizeof(value));
  return value;
}

typedef struct {
  double value;
  int inexact;
} FExact;

static FExact frange_exact_add(double a, double b) {
  FExact out;
  double sum = a + b;
  double split = sum - a;
  double err = (a - (sum - split)) + (b - split);
  out.value = sum;
  out.inexact = !(err == 0.0);
  return out;
}

static FExact frange_exact_mul(double a, double b) {
  FExact out;
  double product = a * b;
  double scale_a = 134217729.0 * a;
  double a_hi = scale_a - (scale_a - a);
  double a_lo = a - a_hi;
  double scale_b = 134217729.0 * b;
  double b_hi = scale_b - (scale_b - b);
  double b_lo = b - b_hi;
  double err =
      ((a_hi * b_hi - product) + a_hi * b_lo + a_lo * b_hi) + a_lo * b_lo;
  out.value = product;
  out.inexact = !(err == 0.0);
  return out;
}

static FExact frange_exact_scaled_sum(double base, long long trips,
                                      double step, int apply) {
  FExact scaled;
  FExact total;
  if (!apply) {
    total.value = base;
    total.inexact = 0;
    return total;
  }
  scaled = frange_exact_mul((double)trips, step);
  if (trips > 9007199254740992LL || trips < -9007199254740992LL) {
    scaled.inexact = 1;
  }
  total = frange_exact_add(base, scaled.value);
  if (scaled.inexact) {
    total.inexact = 1;
  }
  return total;
}

static double frange_outward(FExact landed, int up) {
  if (!landed.inexact) {
    return landed.value;
  }
  return frange_step(landed.value, up);
}

static int frange_same_sign(const FRange *a, const FRange *b) {
  if (!a->has_min || !a->has_max || !b->has_min || !b->has_max) {
    return 0;
  }
  if (a->min >= 0.0 && b->min >= 0.0) {
    return 1;
  }
  return a->max <= 0.0 && b->max <= 0.0;
}

static void frange_meet(FRange *r, const FRange *other) {
  if (other->has_min && (!r->has_min || other->min > r->min)) {
    r->has_min = 1;
    r->min = other->min;
  }
  if (other->has_max && (!r->has_max || other->max < r->max)) {
    r->has_max = 1;
    r->max = other->max;
  }
  if (other->err > r->err) {
    r->err = other->err;
  }
}

static void range_meet(Range *r, const Range *other) {
  if (other->has_min && (!r->has_min || other->min > r->min)) {
    r->has_min = 1;
    r->min = other->min;
  }
  if (other->has_max && (!r->has_max || other->max < r->max)) {
    r->has_max = 1;
    r->max = other->max;
  }
}

static int is_comparison(const char *op) {
  return op && (strcmp(op, "<") == 0 || strcmp(op, "<=") == 0 ||
                strcmp(op, ">") == 0 || strcmp(op, ">=") == 0 ||
                strcmp(op, "==") == 0 || strcmp(op, "!=") == 0);
}

static const char *flip_operator(const char *op) {
  if (strcmp(op, "<") == 0) {
    return ">";
  }
  if (strcmp(op, "<=") == 0) {
    return ">=";
  }
  if (strcmp(op, ">") == 0) {
    return "<";
  }
  if (strcmp(op, ">=") == 0) {
    return "<=";
  }
  return op;
}

static const char *negate_operator(const char *op) {
  if (strcmp(op, "<") == 0) {
    return ">=";
  }
  if (strcmp(op, "<=") == 0) {
    return ">";
  }
  if (strcmp(op, ">") == 0) {
    return "<=";
  }
  if (strcmp(op, ">=") == 0) {
    return "<";
  }
  if (strcmp(op, "==") == 0) {
    return "!=";
  }
  if (strcmp(op, "!=") == 0) {
    return "==";
  }
  return NULL;
}

static const char *identifier_name(const ASTNode *node) {
  if (!node || node->type != AST_IDENTIFIER || !node->data) {
    return NULL;
  }
  return ((const Identifier *)node->data)->name;
}

static int binary_parts(const ASTNode *node, const char **op, ASTNode **left,
                        ASTNode **right) {
  const BinaryExpression *binary;
  if (!node || node->type != AST_BINARY_EXPRESSION || !node->data) {
    return 0;
  }
  binary = (const BinaryExpression *)node->data;
  *op = binary->operator;
  *left = binary->left;
  *right = binary->right;
  return *op != NULL;
}

static int unary_parts(const ASTNode *node, const char **op,
                       ASTNode **operand) {
  const UnaryExpression *unary;
  if (!node || node->type != AST_UNARY_EXPRESSION || !node->data) {
    return 0;
  }
  unary = (const UnaryExpression *)node->data;
  *op = unary->operator;
  *operand = unary->operand;
  return *op != NULL && *operand != NULL;
}

typedef struct {
  ASTNode *node;
  int negated;
} Atom;

typedef void (*AtomVisitor)(void *ctx, ASTNode *node, int negated);

static void visit_atoms(ASTNode *node, int negated, AtomVisitor visit,
                        void *ctx, int depth) {
  const char *op = NULL;
  ASTNode *left = NULL;
  ASTNode *right = NULL;
  ASTNode *operand = NULL;
  if (!node || depth > 16) {
    return;
  }
  if (unary_parts(node, &op, &operand) && strcmp(op, "!") == 0) {
    visit_atoms(operand, !negated, visit, ctx, depth + 1);
    return;
  }
  if (binary_parts(node, &op, &left, &right)) {
    if (!negated && strcmp(op, "&&") == 0) {
      visit_atoms(left, 0, visit, ctx, depth + 1);
      visit_atoms(right, 0, visit, ctx, depth + 1);
      return;
    }
    if (negated && strcmp(op, "||") == 0) {
      visit_atoms(left, 1, visit, ctx, depth + 1);
      visit_atoms(right, 1, visit, ctx, depth + 1);
      return;
    }
  }
  visit(ctx, node, negated);
}

static int range_of(TypeChecker *checker, ASTNode *expr, Range *out,
                    int depth);

typedef struct {
  TypeChecker *checker;
  const char *name;
  Range *range;
  int depth;
} NarrowContext;

static void narrow_by_comparison(NarrowContext *ctx, const char *op,
                                 ASTNode *other) {
  Range bound;
  if (!range_of(ctx->checker, other, &bound, ctx->depth + 1)) {
    return;
  }
  if (strcmp(op, "<") == 0 && bound.has_max && bound.max > LLONG_MIN) {
    Range r = range_unknown();
    r.has_max = 1;
    r.max = bound.max - 1;
    range_meet(ctx->range, &r);
  } else if (strcmp(op, "<=") == 0 && bound.has_max) {
    Range r = range_unknown();
    r.has_max = 1;
    r.max = bound.max;
    range_meet(ctx->range, &r);
  } else if (strcmp(op, ">") == 0 && bound.has_min && bound.min < LLONG_MAX) {
    Range r = range_unknown();
    r.has_min = 1;
    r.min = bound.min + 1;
    range_meet(ctx->range, &r);
  } else if (strcmp(op, ">=") == 0 && bound.has_min) {
    Range r = range_unknown();
    r.has_min = 1;
    r.min = bound.min;
    range_meet(ctx->range, &r);
  } else if (strcmp(op, "==") == 0) {
    range_meet(ctx->range, &bound);
  } else if (strcmp(op, "!=") == 0 && bound.has_min && bound.has_max &&
             bound.min == bound.max) {
    if (ctx->range->has_min && ctx->range->min == bound.min &&
        bound.min < LLONG_MAX) {
      ctx->range->min = bound.min + 1;
    }
    if (ctx->range->has_max && ctx->range->max == bound.max &&
        bound.max > LLONG_MIN) {
      ctx->range->max = bound.max - 1;
    }
  }
}

static void narrow_visit(void *raw, ASTNode *node, int negated) {
  NarrowContext *ctx = (NarrowContext *)raw;
  const char *op = NULL;
  ASTNode *left = NULL;
  ASTNode *right = NULL;
  const char *left_name;
  const char *right_name;
  if (!binary_parts(node, &op, &left, &right) || !is_comparison(op)) {
    return;
  }
  if (negated) {
    op = negate_operator(op);
    if (!op) {
      return;
    }
  }
  left_name = identifier_name(left);
  right_name = identifier_name(right);
  if (left_name && strcmp(left_name, ctx->name) == 0) {
    narrow_by_comparison(ctx, op, right);
  } else if (right_name && strcmp(right_name, ctx->name) == 0) {
    narrow_by_comparison(ctx, flip_operator(op), left);
  }
}

static int narrowing_enter(TypeChecker *checker, const char *name) {
  size_t i;
  if (!checker || !name) {
    return 0;
  }
  for (i = 0; i < checker->narrowing_count; i++) {
    if (strcmp(checker->narrowing[i], name) == 0) {
      return 0;
    }
  }
  if (checker->narrowing_count >= sizeof(checker->narrowing) /
                                     sizeof(checker->narrowing[0])) {
    return 0;
  }
  checker->narrowing[checker->narrowing_count++] = name;
  return 1;
}

static void narrowing_leave(TypeChecker *checker) {
  if (checker && checker->narrowing_count > 0) {
    checker->narrowing_count--;
  }
}

static void narrow_by_guards(TypeChecker *checker, const char *name,
                             Range *range, int depth) {
  NarrowContext ctx;
  ctx.checker = checker;
  ctx.name = name;
  ctx.range = range;
  ctx.depth = depth;
  for (size_t i = 0; i < checker->guard_count; i++) {
    struct TypeCheckerGuard *guard = &checker->guards[i];
    if (guard->condition) {
      visit_atoms(guard->condition, guard->negated, narrow_visit, &ctx, 0);
    } else if (guard->name && strcmp(guard->name, name) == 0) {
      Range r = range_unknown();
      r.has_min = guard->has_min;
      r.min = guard->min;
      r.has_max = guard->has_max;
      r.max = guard->max;
      range_meet(range, &r);
    }
  }
}

static int add_checked(long long a, long long b, long long *out) {
  return !__builtin_add_overflow(a, b, out);
}

static int sub_checked(long long a, long long b, long long *out) {
  return !__builtin_sub_overflow(a, b, out);
}

static int mul_checked(long long a, long long b, long long *out) {
  return !__builtin_mul_overflow(a, b, out);
}

static void narrow_by_monotone(TypeChecker *checker, const char *name,
                               Range *out, int depth);
static void narrow_by_relation(TypeChecker *checker, const Type *declared,
                               Range *out, int depth);

static int range_of_call(TypeChecker *checker, ASTNode *expr, Range *out,
                    int depth) {
  (void)depth;

  *out = range_unknown();
    CallExpression *call = (CallExpression *)expr->data;
    Symbol *callee = NULL;
    *out = range_of_type(expr->resolved_type);
    if (checker && call && call->is_gpu_index && call->function_name) {
      ASTNode *owner_node = checker->current_function_decl;
      FunctionDeclaration *owner =
          owner_node && owner_node->type == AST_FUNCTION_DECLARATION
              ? (FunctionDeclaration *)owner_node->data
              : NULL;
      int axis = call->function_name[strlen(call->function_name) - 1] == 'y' ? 1
                 : call->function_name[strlen(call->function_name) - 1] == 'z'
                     ? 2
                     : 0;
      int declared = owner && owner->is_kernel ? owner->kernel_block[axis] : 0;
      if (axis > 0 && owner && owner->is_kernel && owner->kernel_block[0] > 0 &&
          declared <= 0) {
        declared = 1;
      }
      if (declared > 0) {
        Range shape = range_unknown();
        if (strncmp(call->function_name, "gpu_tid_", 8) == 0) {
          shape.has_min = 1;
          shape.min = 0;
          shape.has_max = 1;
          shape.max = declared - 1;
        } else if (strncmp(call->function_name, "gpu_ntid_", 9) == 0) {
          shape.has_min = 1;
          shape.min = declared;
          shape.has_max = 1;
          shape.max = declared;
        }
        if (shape.has_min || shape.has_max) {
          range_meet(out, &shape);
          return 1;
        }
      }
      if (strncmp(call->function_name, "gpu_", 4) == 0) {
        Range nonnegative = range_unknown();
        nonnegative.has_min = 1;
        nonnegative.min = 0;
        range_meet(out, &nonnegative);
        return out->has_min || out->has_max;
      }
    }
    if (checker && call && call->function_name && !call->object) {
      callee = symbol_table_lookup(checker->symbol_table, call->function_name);
    }
    if (callee && callee->kind == SYMBOL_FUNCTION && callee->post_state == 2 &&
        callee->post_has_min && callee->post_has_max) {
      Range post;
      post.has_min = 1;
      post.has_max = 1;
      post.min = callee->post_min;
      post.max = callee->post_max;
      range_meet(out, &post);
    }
    return out->has_min || out->has_max;
}

static int range_of_member(TypeChecker *checker, ASTNode *expr, Range *out,
                    int depth) {
  (void)checker;
  (void)depth;

  *out = range_unknown();
    MemberAccess *member = (MemberAccess *)expr->data;
    Type *object_type = member && member->object
                            ? member->object->resolved_type
                            : NULL;
    if (member && member->member && strcmp(member->member, "length") == 0 &&
        object_type) {
      if (object_type->kind == TYPE_ARRAY) {
        *out = range_exact((long long)object_type->array_size);
        return 1;
      }
      if (object_type->kind == TYPE_SLICE || object_type->kind == TYPE_STRING) {
        out->has_min = 1;
        out->min = 0;
        return 1;
      }
    }
    *out = range_of_type(expr->resolved_type);
    return out->has_min || out->has_max;
}

static int range_of_cast(TypeChecker *checker, ASTNode *expr, Range *out,
                    int depth) {

  *out = range_unknown();
    CastExpression *cast = (CastExpression *)expr->data;
    Range target = range_of_type(expr->resolved_type);
    Range inner;
    if (cast && cast->operand && range_of(checker, cast->operand, &inner,
                                          depth + 1) &&
        inner.has_min && inner.has_max && target.has_min && target.has_max &&
        inner.min >= target.min && inner.max <= target.max) {
      *out = inner;
      return 1;
    }
    *out = target;
    return out->has_min || out->has_max;
}

static int range_of_unary(TypeChecker *checker, ASTNode *expr, Range *out,
                    int depth) {
  const char *op = NULL;
  ASTNode *operand = NULL;

  *out = range_unknown();
    if (!unary_parts(expr, &op, &operand)) {
      return 0;
    }
    if (strcmp(op, "-") == 0) {
      Range inner;
      if (range_of(checker, operand, &inner, depth + 1)) {
        if (inner.has_max && inner.max > LLONG_MIN) {
          out->has_min = 1;
          out->min = -inner.max;
        }
        if (inner.has_min && inner.min > LLONG_MIN) {
          out->has_max = 1;
          out->max = -inner.min;
        }
      }
      return out->has_min || out->has_max;
    }
    if (strcmp(op, "!") == 0) {
      out->has_min = 1;
      out->has_max = 1;
      out->min = 0;
      out->max = 1;
      return 1;
    }
    return 0;
}

static void range_binary_add(const Range *l, const Range *r, Range *out) {
  if (l->has_min && r->has_min && add_checked(l->min, r->min, &out->min)) {
    out->has_min = 1;
  }
  if (l->has_max && r->has_max && add_checked(l->max, r->max, &out->max)) {
    out->has_max = 1;
  }
}

static void range_binary_sub(const Range *l, const Range *r, Range *out) {
  if (l->has_min && r->has_max && sub_checked(l->min, r->max, &out->min)) {
    out->has_min = 1;
  }
  if (l->has_max && r->has_min && sub_checked(l->max, r->min, &out->max)) {
    out->has_max = 1;
  }
}

static void range_binary_mul(const Range *l, const Range *r, Range *out) {
  long long products[4];

  if (!l->has_min || !l->has_max || !r->has_min || !r->has_max) {
    return;
  }
  if (!mul_checked(l->min, r->min, &products[0]) ||
      !mul_checked(l->min, r->max, &products[1]) ||
      !mul_checked(l->max, r->min, &products[2]) ||
      !mul_checked(l->max, r->max, &products[3])) {
    return;
  }
  out->min = products[0];
  out->max = products[0];
  for (int i = 1; i < 4; i++) {
    if (products[i] < out->min) {
      out->min = products[i];
    }
    if (products[i] > out->max) {
      out->max = products[i];
    }
  }
  out->has_min = 1;
  out->has_max = 1;
}

static void range_binary_modulo(const Range *l, const Range *r, int have_l,
                                Range *out) {
  if (!r->has_min || !r->has_max || r->min <= 0) {
    return;
  }
  out->has_min = 1;
  out->has_max = 1;
  out->max = r->max - 1;
  out->min = have_l && l->has_min && l->min >= 0 ? 0 : -(r->max - 1);
}

static void range_binary_divide(const Range *l, const Range *r, Range *out) {
  if (!r->has_min || !r->has_max || r->min != r->max || r->min <= 0) {
    return;
  }
  if (l->has_min) {
    out->has_min = 1;
    out->min = l->min / r->min;
  }
  if (l->has_max) {
    out->has_max = 1;
    out->max = l->max / r->min;
  }
}

static int range_constant_mask(const Range *range, long long *mask) {
  if (!range->has_min || !range->has_max || range->min != range->max ||
      range->min < 0) {
    return 0;
  }
  *mask = range->min;
  return 1;
}

static void range_binary_and(const Range *l, const Range *r, int have_l,
                             int have_r, Range *out) {
  long long mask = 0;

  if ((have_r && range_constant_mask(r, &mask)) ||
      (have_l && range_constant_mask(l, &mask))) {
    out->has_min = 1;
    out->has_max = 1;
    out->min = 0;
    out->max = mask;
    return;
  }
  if (!have_l || !have_r || !l->has_min || !r->has_min || l->min < 0 ||
      r->min < 0 || (!l->has_max && !r->has_max)) {
    return;
  }
  out->has_min = 1;
  out->min = 0;
  out->has_max = 1;
  out->max = l->has_max && r->has_max ? (l->max < r->max ? l->max : r->max)
                                      : (l->has_max ? l->max : r->max);
}

static void range_binary_shift_right(const Range *l, const Range *r,
                                     Range *out) {
  if (!l->has_min || !r->has_min || !r->has_max || r->min != r->max ||
      r->min < 0 || r->min >= 63) {
    return;
  }
  out->has_min = 1;
  out->min = shift_right_floor(l->min, (int)r->min);
  if (l->has_max) {
    out->has_max = 1;
    out->max = shift_right_floor(l->max, (int)r->min);
  }
}

static void range_binary_operator(const char *op, const Range *l,
                                  const Range *r, int have_l, int have_r,
                                  Range *out) {
  int both = have_l && have_r;

  if (strcmp(op, "+") == 0 && both) {
    range_binary_add(l, r, out);
  } else if (strcmp(op, "-") == 0 && both) {
    range_binary_sub(l, r, out);
  } else if (strcmp(op, "*") == 0 && both) {
    range_binary_mul(l, r, out);
  } else if (strcmp(op, "%") == 0 && have_r) {
    range_binary_modulo(l, r, have_l, out);
  } else if (strcmp(op, "/") == 0 && both) {
    range_binary_divide(l, r, out);
  } else if (strcmp(op, "&") == 0) {
    range_binary_and(l, r, have_l, have_r, out);
  } else if (strcmp(op, ">>") == 0 && both) {
    range_binary_shift_right(l, r, out);
  }
}

static int range_of_binary(TypeChecker *checker, ASTNode *expr, Range *out,
                           int depth) {
  const char *op = NULL;
  ASTNode *left = NULL;
  ASTNode *right = NULL;
  Range l;
  Range r;
  int have_l;
  int have_r;

  *out = range_unknown();
  if (!binary_parts(expr, &op, &left, &right)) {
    return 0;
  }
  if (is_comparison(op) || strcmp(op, "&&") == 0 || strcmp(op, "||") == 0) {
    out->has_min = 1;
    out->has_max = 1;
    out->min = 0;
    out->max = 1;
    return 1;
  }
  have_l = range_of(checker, left, &l, depth + 1);
  have_r = range_of(checker, right, &r, depth + 1);
  range_binary_operator(op, &l, &r, have_l, have_r, out);
  if (!out->has_min && !out->has_max) {
    *out = range_of_type(expr->resolved_type);
  }
  return out->has_min || out->has_max;
}

static int range_of_identifier(TypeChecker *checker, ASTNode *expr, Range *out,
                    int depth) {

  *out = range_unknown();
    Identifier *identifier = (Identifier *)expr->data;
    Symbol *symbol = identifier
                         ? type_checker_resolve_identifier(checker, identifier)
                         : NULL;
    if (symbol && symbol->kind == SYMBOL_CONSTANT) {
      *out = range_exact(symbol->data.constant.value);
      return 1;
    }
    if (symbol && symbol->has_constant_value && !symbol->constant_is_float) {
      *out = range_exact(symbol->constant_integer_value);
      return 1;
    }
    {
      Type *type = expr->resolved_type ? expr->resolved_type
                                       : (symbol ? symbol->type : NULL);
      if (!type || type->kind == TYPE_ENUM ||
          !type_checker_is_integer_type(type)) {
        return 0;
      }
      *out = range_of_type(type);
    }
    if (identifier && identifier->name &&
        narrowing_enter(checker, identifier->name)) {
      Type *declared = expr->resolved_type ? expr->resolved_type
                                           : (symbol ? symbol->type : NULL);
      narrow_by_guards(checker, identifier->name, out, depth);
      narrow_by_monotone(checker, identifier->name, out, depth);
      narrow_by_relation(checker, declared, out, depth);
      narrowing_leave(checker);
    }
    return out->has_min || out->has_max;
}

static int range_of(TypeChecker *checker, ASTNode *expr, Range *out,
                    int depth) {
  if (checker) {
    checker->proof_steps++;
    if (checker->proof_steps > TYPE_CHECKER_PROOF_CEILING) {
      checker->proof_ceiling_hit = 1;
      *out = range_unknown();
      return 0;
    }
  }
  *out = range_unknown();
  if (!expr || depth > 32) {
    return 0;
  }
  switch (expr->type) {
  case AST_NUMBER_LITERAL: {
    NumberLiteral *literal = (NumberLiteral *)expr->data;
    if (!literal || literal->is_float) {
      return 0;
    }
    *out = range_exact(literal->int_value);
    return 1;
  }
  case AST_IDENTIFIER:
    return range_of_identifier(checker, expr, out, depth);

  case AST_BINARY_EXPRESSION:
    return range_of_binary(checker, expr, out, depth);

  case AST_UNARY_EXPRESSION:
    return range_of_unary(checker, expr, out, depth);

  case AST_CAST_EXPRESSION:
    return range_of_cast(checker, expr, out, depth);

  case AST_MEMBER_ACCESS:
    return range_of_member(checker, expr, out, depth);

  case AST_FUNCTION_CALL:
    return range_of_call(checker, expr, out, depth);

  default:
    *out = range_of_type(expr->resolved_type);
    return out->has_min || out->has_max;
  }
}

static void narrow_by_relation(TypeChecker *checker, const Type *declared,
                               Range *out, int depth) {
  const Type *layer;
  if (!checker || !declared || depth > 8) {
    return;
  }
  for (layer = declared; layer && layer->refined_base;
       layer = layer->refined_base) {
    NarrowContext ctx;
    if (!layer->refine_relational || !layer->refinement) {
      continue;
    }
    ctx.checker = checker;
    ctx.name = layer->refine_binding ? layer->refine_binding : "value";
    ctx.range = out;
    ctx.depth = depth;
    visit_atoms(layer->refinement, 0, narrow_visit, &ctx, 0);
  }
}

static int monotone_scan_node(const ASTNode *node, const char *name,
                              int *direction) {
  if (node->type == AST_UNARY_EXPRESSION) {
    const char *op = NULL;
    ASTNode *operand = NULL;
    if (unary_parts((ASTNode *)node, &op, &operand) && strcmp(op, "&") == 0 &&
        identifier_name(operand) &&
        strcmp(identifier_name(operand), name) == 0) {
      return 0;
    }
  }
  if (node->type == AST_ASSIGNMENT) {
    const Assignment *assign = (const Assignment *)node->data;
    if (assign && assign->variable_name && !assign->target &&
        strcmp(assign->variable_name, name) == 0) {
      const char *op = NULL;
      ASTNode *left = NULL;
      ASTNode *right = NULL;
      const NumberLiteral *step = NULL;
      int step_direction = 0;
      if (!assign->value || !binary_parts(assign->value, &op, &left, &right)) {
        return 0;
      }
      if (strcmp(op, "+") != 0 && strcmp(op, "-") != 0) {
        return 0;
      }
      if (!identifier_name(left) || strcmp(identifier_name(left), name) != 0) {
        return 0;
      }
      if (!right || right->type != AST_NUMBER_LITERAL) {
        return 0;
      }
      step = (const NumberLiteral *)right->data;
      if (!step || step->is_float || step->int_value == 0) {
        return 0;
      }
      step_direction = (step->int_value > 0) ? 1 : -1;
      if (strcmp(op, "-") == 0) {
        step_direction = -step_direction;
      }
      if (*direction != 0 && *direction != step_direction) {
        return 0;
      }
      *direction = step_direction;
    }
  }
  return 1;
}

static int monotone_scan(const ASTNode *node, const char *name,
                         int *direction) {
  if (!node) {
    return 1;
  }
  if (!monotone_scan_node(node, name, direction)) {
    return 0;
  }
  for (size_t i = 0; i < node->child_count; i++) {
    if (!monotone_scan(node->children[i], name, direction)) {
      return 0;
    }
  }
  return 1;
}

static int monotone_step_node(const ASTNode *node, const char *name,
                              long long *step) {
  if (node->type == AST_ASSIGNMENT) {
    const Assignment *assign = (const Assignment *)node->data;
    if (assign && assign->variable_name && !assign->target &&
        strcmp(assign->variable_name, name) == 0) {
      const char *op = NULL;
      ASTNode *left = NULL;
      ASTNode *right = NULL;
      const NumberLiteral *literal = NULL;
      long long moved;
      if (!assign->value || !binary_parts(assign->value, &op, &left, &right) ||
          !identifier_name(left) ||
          strcmp(identifier_name(left), name) != 0 || !right ||
          right->type != AST_NUMBER_LITERAL) {
        return 0;
      }
      literal = (const NumberLiteral *)right->data;
      if (!literal || literal->is_float) {
        return 0;
      }
      moved = strcmp(op, "-") == 0 ? -literal->int_value : literal->int_value;
      if (*step != 0 && *step != moved) {
        return 0;
      }
      *step = moved;
    }
  }
  return 1;
}

static int monotone_step(const ASTNode *node, const char *name,
                         long long *step) {
  if (!node) {
    return 1;
  }
  if (!monotone_step_node(node, name, step)) {
    return 0;
  }
  for (size_t i = 0; i < node->child_count; i++) {
    if (!monotone_step(node->children[i], name, step)) {
      return 0;
    }
  }
  return 1;
}

static int monotone_declaration_node(const ASTNode *node,
                                     const char *name) {
  if (node->type == AST_VAR_DECLARATION) {
    const VarDeclaration *decl = (const VarDeclaration *)node->data;
    return decl && decl->name && strcmp(decl->name, name) == 0 &&
           decl->initializer;
  }
  return 0;
}

static const ASTNode *monotone_declaration_scan(const ASTNode *node,
                                                const char *name,
                                                size_t *seen) {
  const ASTNode *found = NULL;
  if (!node) {
    return NULL;
  }
  if (monotone_declaration_node(node, name)) {
    (*seen)++;
    found = node;
  }
  for (size_t i = 0; i < node->child_count; i++) {
    const ASTNode *deeper =
        monotone_declaration_scan(node->children[i], name, seen);
    if (deeper && !found) {
      found = deeper;
    }
  }
  return found;
}

static const ASTNode *monotone_declaration(const ASTNode *node,
                                           const char *name) {
  size_t seen = 0;
  const ASTNode *found = monotone_declaration_scan(node, name, &seen);
  return seen == 1 ? found : NULL;
}

static int accumulator_node(ASTNode *node, const char *name, int *ok,
                            ASTNode **addend) {
  *addend = NULL;
  if (node->type == AST_ASSIGNMENT) {
    Assignment *assign = (Assignment *)node->data;
    if (assign && assign->variable_name && !assign->target &&
        strcmp(assign->variable_name, name) == 0) {
      const char *op = NULL;
      ASTNode *left = NULL;
      ASTNode *right = NULL;
      if (!assign->value || !binary_parts(assign->value, &op, &left, &right) ||
          strcmp(op, "+") != 0 || !identifier_name(left) ||
          strcmp(identifier_name(left), name) != 0) {
        *ok = 0;
        return 0;
      }
      *addend = right;
      return 1;
    }
  }
  return 0;
}

static ASTNode *accumulator_addend(ASTNode *node, const char *name, int *ok) {
  ASTNode *found = NULL;
  if (!node || !*ok) {
    return NULL;
  }
  accumulator_node(node, name, ok, &found);
  if (!*ok) {
    return NULL;
  }
  for (size_t i = 0; i < node->child_count; i++) {
    ASTNode *deeper = accumulator_addend(node->children[i], name, ok);
    if (deeper) {
      if (found) {
        *ok = 0;
        return NULL;
      }
      found = deeper;
    }
  }
  return found;
}

static int constant_range(TypeChecker *checker, const ASTNode *expr, Range *out,
                          int depth) {
  *out = range_unknown();
  if (!expr || depth > 8) {
    return 0;
  }
  switch (expr->type) {
  case AST_NUMBER_LITERAL: {
    const NumberLiteral *literal = (const NumberLiteral *)expr->data;
    if (!literal || literal->is_float) {
      return 0;
    }
    *out = range_exact(literal->int_value);
    return 1;
  }
  case AST_IDENTIFIER: {
    Identifier *identifier = (Identifier *)expr->data;
    Symbol *symbol = identifier
                         ? type_checker_resolve_identifier(checker, identifier)
                         : NULL;
    if (symbol && symbol->kind == SYMBOL_CONSTANT) {
      *out = range_exact(symbol->data.constant.value);
      return 1;
    }
    if (symbol && symbol->has_constant_value && !symbol->constant_is_float) {
      *out = range_exact(symbol->constant_integer_value);
      return 1;
    }
    if (symbol && symbol->type && symbol->type->refined_base &&
        symbol->type->refine_has_range) {
      *out = range_of_type(symbol->type);
      return out->has_max;
    }
    return 0;
  }
  case AST_CAST_EXPRESSION: {
    const CastExpression *cast = (const CastExpression *)expr->data;
    return cast && cast->operand &&
           constant_range(checker, cast->operand, out, depth + 1);
  }
  case AST_UNARY_EXPRESSION: {
    const char *op = NULL;
    ASTNode *operand = NULL;
    Range inner;
    if (!unary_parts((ASTNode *)expr, &op, &operand) || strcmp(op, "-") != 0 ||
        !constant_range(checker, operand, &inner, depth + 1) ||
        !inner.has_min || !inner.has_max || inner.min == LLONG_MIN) {
      return 0;
    }
    *out = range_exact(-inner.min);
    return 1;
  }
  default:
    return 0;
  }
}

typedef struct {
  char *name;
  ASTNode **nodes;
  size_t count;
  size_t capacity;
} MovementSlot;

typedef struct {
  const ASTNode *decl;
  MovementSlot *slots;
  size_t mask;
} MovementIndex;

static const char *movement_key(const ASTNode *node) {
  if (node->type == AST_UNARY_EXPRESSION) {
    const char *op = NULL;
    ASTNode *operand = NULL;
    if (unary_parts((ASTNode *)node, &op, &operand) && strcmp(op, "&") == 0) {
      return identifier_name(operand);
    }
    return NULL;
  }
  if (node->type == AST_ASSIGNMENT) {
    const Assignment *assign = (const Assignment *)node->data;
    return assign && !assign->target ? assign->variable_name : NULL;
  }
  if (node->type == AST_VAR_DECLARATION) {
    const VarDeclaration *decl = (const VarDeclaration *)node->data;
    return decl ? decl->name : NULL;
  }
  return NULL;
}

static size_t movement_count(const ASTNode *node) {
  size_t count = 0;
  if (!node) {
    return 0;
  }
  if (movement_key(node)) {
    count++;
  }
  for (size_t i = 0; i < node->child_count; i++) {
    count += movement_count(node->children[i]);
  }
  return count;
}

static MovementSlot *movement_slot(MovementIndex *index, const char *name,
                                   int claim) {
  size_t h = mettle_fnv1a_hash(name) & index->mask;
  while (index->slots[h].name) {
    if (strcmp(index->slots[h].name, name) == 0) {
      return &index->slots[h];
    }
    h = (h + 1u) & index->mask;
  }
  if (!claim) {
    return NULL;
  }
  index->slots[h].name = mettle_strdup(name);
  return index->slots[h].name ? &index->slots[h] : NULL;
}

static int movement_fill(MovementIndex *index, ASTNode *node) {
  const char *key;
  if (!node) {
    return 1;
  }
  key = movement_key(node);
  if (key) {
    MovementSlot *slot = movement_slot(index, key, 1);
    if (!slot) {
      return 0;
    }
    if (slot->count >= slot->capacity) {
      size_t capacity = slot->capacity ? slot->capacity * 2 : 4;
      ASTNode **grown =
          (ASTNode **)realloc(slot->nodes, capacity * sizeof(ASTNode *));
      if (!grown) {
        return 0;
      }
      slot->nodes = grown;
      slot->capacity = capacity;
    }
    slot->nodes[slot->count++] = node;
  }
  for (size_t i = 0; i < node->child_count; i++) {
    if (!movement_fill(index, node->children[i])) {
      return 0;
    }
  }
  return 1;
}

void type_checker_movement_index_drop(TypeChecker *checker) {
  MovementIndex *index = checker ? (MovementIndex *)checker->movement_index
                                 : NULL;
  if (!index) {
    return;
  }
  for (size_t h = 0; index->slots && h <= index->mask; h++) {
    free(index->slots[h].name);
    free(index->slots[h].nodes);
  }
  free(index->slots);
  free(index);
  checker->movement_index = NULL;
}

static const MovementIndex *movement_index_for(TypeChecker *checker,
                                               ASTNode *body) {
  MovementIndex *index = (MovementIndex *)checker->movement_index;
  size_t capacity = 16;
  size_t count;
  if (index && index->decl == checker->current_function_decl) {
    return index;
  }
  type_checker_movement_index_drop(checker);
  count = movement_count(body);
  while (capacity < count * 2) {
    capacity *= 2;
  }
  index = (MovementIndex *)calloc(1, sizeof(MovementIndex));
  if (!index) {
    return NULL;
  }
  index->slots = (MovementSlot *)calloc(capacity, sizeof(MovementSlot));
  index->mask = capacity - 1;
  index->decl = checker->current_function_decl;
  checker->movement_index = index;
  if (!index->slots || !movement_fill(index, body)) {
    type_checker_movement_index_drop(checker);
    return NULL;
  }
  return index;
}

static void movement_from_index(const MovementIndex *index, const char *name,
                                Symbol *symbol) {
  MovementSlot *slot = movement_slot((MovementIndex *)index, name, 0);
  size_t count = slot ? slot->count : 0;
  size_t seen = 0;
  const ASTNode *declaration = NULL;
  int ok = 1;
  ASTNode *addend = NULL;
  for (size_t k = 0; k < count; k++) {
    if (monotone_declaration_node(slot->nodes[k], name)) {
      seen++;
      if (!declaration) {
        declaration = slot->nodes[k];
      }
    }
  }
  symbol->move_declaration = seen == 1 ? (ASTNode *)declaration : NULL;
  for (size_t k = 0; k < count; k++) {
    if (!monotone_scan_node(slot->nodes[k], name, &symbol->move_direction)) {
      symbol->move_direction = 0;
      break;
    }
  }
  for (size_t k = 0; k < count; k++) {
    if (!monotone_step_node(slot->nodes[k], name, &symbol->move_step)) {
      symbol->move_step = 0;
      break;
    }
  }
  for (size_t k = 0; k < count && ok; k++) {
    ASTNode *here = NULL;
    if (accumulator_node(slot->nodes[k], name, &ok, &here)) {
      if (addend) {
        ok = 0;
        break;
      }
      addend = here;
    }
  }
  symbol->move_addend = ok ? addend : NULL;
}

static Symbol *movement_of(TypeChecker *checker, const char *name) {
  Symbol *symbol;
  const ASTNode *body;
  if (!checker || !name || !checker->symbol_table) {
    return NULL;
  }
  symbol = symbol_table_lookup(checker->symbol_table, name);
  if (!symbol) {
    return NULL;
  }
  if (symbol->move_computed) {
    return symbol;
  }
  symbol->move_computed = 1;
  symbol->move_direction = 0;
  symbol->move_step = 0;
  symbol->move_declaration = NULL;
  symbol->move_addend = NULL;
  body = checker->current_function_decl;
  if (!body || body->type != AST_FUNCTION_DECLARATION || !body->data) {
    return symbol;
  }
  body = ((const FunctionDeclaration *)body->data)->body;
  {
    const MovementIndex *index = movement_index_for(checker, (ASTNode *)body);
    if (index) {
      movement_from_index(index, name, symbol);
      return symbol;
    }
  }
  symbol->move_declaration = (ASTNode *)monotone_declaration(body, name);
  if (!monotone_scan(body, name, &symbol->move_direction)) {
    symbol->move_direction = 0;
  }
  if (!monotone_step(body, name, &symbol->move_step)) {
    symbol->move_step = 0;
  }
  {
    int ok = 1;
    ASTNode *addend = accumulator_addend((ASTNode *)body, name, &ok);
    symbol->move_addend = ok ? addend : NULL;
  }
  return symbol;
}

static void narrow_by_monotone(TypeChecker *checker, const char *name,
                               Range *out, int depth) {
  const ASTNode *declaration;
  const VarDeclaration *decl;
  Symbol *movement;
  int direction;
  Range initial;
  if (!checker || !name || checker->monotone_busy || depth > 8) {
    return;
  }
  movement = movement_of(checker, name);
  if (!movement) {
    return;
  }
  declaration = movement->move_declaration;
  direction = movement->move_direction;
  if (!declaration) {
    return;
  }
  decl = (const VarDeclaration *)declaration->data;
  if (direction == 0) {
    return;
  }
  if (!constant_range(checker, decl->initializer, &initial, 0)) {
    return;
  }
  if (direction > 0 && initial.has_min) {
    Range bound = range_unknown();
    bound.has_min = 1;
    bound.min = initial.min;
    range_meet(out, &bound);
  } else if (direction < 0 && initial.has_max) {
    Range bound = range_unknown();
    bound.has_max = 1;
    bound.max = initial.max;
    range_meet(out, &bound);
  }
}

size_t type_checker_loop_trip_depth(const TypeChecker *checker) {
  return checker ? checker->loop_trip_count : 0;
}

void type_checker_pop_loop_trip(TypeChecker *checker, size_t depth) {
  if (checker && checker->loop_trip_count > depth) {
    checker->loop_trip_count = depth;
  }
}

int type_checker_push_loop_trip(TypeChecker *checker, ASTNode *condition,
                                ASTNode *body) {
  const char *counter;
  const ASTNode *declaration;
  const char *op = NULL;
  ASTNode *left = NULL;
  ASTNode *right = NULL;
  Range limit;
  Range initial;
  int direction = 0;
  long long step = 0;
  long long trips;
  if (!checker || !condition || !body) {
    return 0;
  }
  if (!binary_parts(condition, &op, &left, &right) ||
      (strcmp(op, "<") != 0 && strcmp(op, "<=") != 0)) {
    return 0;
  }
  counter = identifier_name(left);
  if (!counter) {
    return 0;
  }
  {
    Symbol *movement = movement_of(checker, counter);
    if (!movement || !movement->move_declaration) {
      return 0;
    }
    declaration = movement->move_declaration;
    direction = movement->move_direction;
    step = movement->move_step;
  }
  if (direction <= 0 || step <= 0) {
    return 0;
  }
  if (!constant_range(checker, right, &limit, 0) || !limit.has_max) {
    return 0;
  }

  if (!constant_range(checker,
                      ((const VarDeclaration *)declaration->data)->initializer,
                      &initial, 0) ||
      !initial.has_min) {
    return 0;
  }
  trips = limit.max - initial.min;
  if (trips < 0) {
    trips = 0;
  }
  trips = (trips + step - 1) / step;
  if (strcmp(op, "<=") == 0) {
    trips++;
  }
  if (trips < 0 || trips > 1000000000) {
    return 0;
  }
  if (checker->loop_trip_count == checker->loop_trip_capacity) {
    size_t grown =
        checker->loop_trip_capacity ? checker->loop_trip_capacity * 2 : 4;
    void *table = realloc(checker->loop_trips,
                          grown * sizeof(*checker->loop_trips));
    if (!table) {
      return 0;
    }
    checker->loop_trips = table;
    checker->loop_trip_capacity = grown;
  }
  checker->loop_trips[checker->loop_trip_count].body = body;
  checker->loop_trips[checker->loop_trip_count].trips = trips;
  checker->loop_trip_count++;
  return 1;
}

void type_checker_note_return_range(TypeChecker *checker, ASTNode *value) {
  Symbol *fn = checker ? checker->current_function : NULL;
  Range r;
  if (!fn || fn->kind != SYMBOL_FUNCTION || fn->post_state == 3) {
    return;
  }
  if (!value || !range_of(checker, value, &r, 0) || !r.has_min || !r.has_max) {
    fn->post_state = 3;
    fn->post_has_min = 0;
    fn->post_has_max = 0;
    return;
  }
  if (fn->post_state != 1) {
    fn->post_state = 1;
    fn->post_has_min = 1;
    fn->post_has_max = 1;
    fn->post_min = r.min;
    fn->post_max = r.max;
    return;
  }
  if (r.min < fn->post_min) {
    fn->post_min = r.min;
  }
  if (r.max > fn->post_max) {
    fn->post_max = r.max;
  }
}

static FRange frange_of_type(const Type *type) {
  FRange r = frange_unknown();
  if (!type) {
    return r;
  }
  if (type->refined_base && type->refine_has_frange) {
    r.has_min = 1;
    r.has_max = 1;
    r.min = type->refine_fmin;
    r.max = type->refine_fmax;
    r.err = type->refine_ferr;
    return r;
  }
  if (type->refined_base) {
    return frange_of_type(type->refined_base);
  }
  return r;
}

static int frange_of(TypeChecker *checker, ASTNode *expr, FRange *out,
                     int depth);
static void fnarrow_by_accumulator(TypeChecker *checker, const char *name,
                                   FRange *out, int depth);

typedef struct {
  TypeChecker *checker;
  const char *name;
  FRange *range;
  int depth;
} FNarrowContext;

static void fnarrow_by_comparison(FNarrowContext *ctx, const char *op,
                                  ASTNode *other) {
  FRange bound;
  if (!frange_of(ctx->checker, other, &bound, ctx->depth + 1)) {
    return;
  }
  if ((strcmp(op, "<") == 0 || strcmp(op, "<=") == 0) && bound.has_max) {
    FRange r = frange_unknown();
    r.has_max = 1;
    r.max = bound.max;
    frange_meet(ctx->range, &r);
  } else if ((strcmp(op, ">") == 0 || strcmp(op, ">=") == 0) &&
             bound.has_min) {
    FRange r = frange_unknown();
    r.has_min = 1;
    r.min = bound.min;
    frange_meet(ctx->range, &r);
  } else if (strcmp(op, "==") == 0) {
    frange_meet(ctx->range, &bound);
  }
}

static void fnarrow_visit(void *raw, ASTNode *node, int negated) {
  FNarrowContext *ctx = (FNarrowContext *)raw;
  const char *op = NULL;
  ASTNode *left = NULL;
  ASTNode *right = NULL;
  const char *left_name;
  const char *right_name;
  if (!binary_parts(node, &op, &left, &right) || !is_comparison(op)) {
    return;
  }
  if (negated) {
    op = negate_operator(op);
    if (!op) {
      return;
    }
  }
  left_name = identifier_name(left);
  right_name = identifier_name(right);
  if (left_name && strcmp(left_name, ctx->name) == 0) {
    fnarrow_by_comparison(ctx, op, right);
  } else if (right_name && strcmp(right_name, ctx->name) == 0) {
    fnarrow_by_comparison(ctx, flip_operator(op), left);
  }
}

static void fnarrow_by_guards(TypeChecker *checker, const char *name,
                              FRange *range, int depth) {
  FNarrowContext ctx;
  ctx.checker = checker;
  ctx.name = name;
  ctx.range = range;
  ctx.depth = depth;
  for (size_t i = 0; i < checker->guard_count; i++) {
    struct TypeCheckerGuard *guard = &checker->guards[i];
    if (guard->condition) {
      visit_atoms(guard->condition, guard->negated, fnarrow_visit, &ctx, 0);
    }
  }
}

static int frange_of_binary(TypeChecker *checker, ASTNode *expr, FRange *out,
                     int depth) {
  const char *op = NULL;
  ASTNode *left = NULL;
  ASTNode *right = NULL;

  *out = frange_unknown();
    FRange l;
    FRange r;
    double eps;
    if (!binary_parts(expr, &op, &left, &right)) {
      return 0;
    }
    if (!frange_of(checker, left, &l, depth + 1) ||
        !frange_of(checker, right, &r, depth + 1) || !l.has_min ||
        !l.has_max || !r.has_min || !r.has_max) {
      return 0;
    }
    eps = frange_eps(expr->resolved_type);
    if (strcmp(op, "+") == 0) {
      out->has_min = 1;
      out->has_max = 1;
      out->min = frange_outward(frange_exact_add(l.min, r.min), 0);
      out->max = frange_outward(frange_exact_add(l.max, r.max), 1);
      out->err = frange_same_sign(&l, &r) ? l.err + r.err + eps
                                          : FRANGE_NO_BOUND;
      return 1;
    } else if (strcmp(op, "-") == 0) {
      out->has_min = 1;
      out->has_max = 1;
      out->min = frange_outward(frange_exact_add(l.min, -r.max), 0);
      out->max = frange_outward(frange_exact_add(l.max, -r.min), 1);
      out->err = FRANGE_NO_BOUND;
      return 1;
    } else if (strcmp(op, "*") == 0) {
      FExact p[4];
      FExact lo;
      FExact hi;
      p[0] = frange_exact_mul(l.min, r.min);
      p[1] = frange_exact_mul(l.min, r.max);
      p[2] = frange_exact_mul(l.max, r.min);
      p[3] = frange_exact_mul(l.max, r.max);
      lo = p[0];
      hi = p[0];
      for (int i = 1; i < 4; i++) {
        if (p[i].value < lo.value) {
          lo = p[i];
        }
        if (p[i].value > hi.value) {
          hi = p[i];
        }
      }
      out->min = frange_outward(lo, 0);
      out->max = frange_outward(hi, 1);
      out->has_min = 1;
      out->has_max = 1;
      out->err = l.err + r.err + eps;
      return 1;
    }
    return 0;
}

static int frange_of_identifier(TypeChecker *checker, ASTNode *expr, FRange *out,
                     int depth) {

  *out = frange_unknown();
    Identifier *identifier = (Identifier *)expr->data;
    Symbol *symbol = identifier
                         ? type_checker_resolve_identifier(checker, identifier)
                         : NULL;
    Type *type = expr->resolved_type ? expr->resolved_type
                                     : (symbol ? symbol->type : NULL);
    if (symbol && symbol->has_constant_value && symbol->constant_is_float) {
      *out = frange_exact(symbol->constant_float_value);
      return 1;
    }
    *out = frange_of_type(type);
    if (identifier && identifier->name &&
        narrowing_enter(checker, identifier->name)) {
      fnarrow_by_accumulator(checker, identifier->name, out, depth);
      fnarrow_by_guards(checker, identifier->name, out, depth);
      narrowing_leave(checker);
    }
    return out->has_min || out->has_max;
}

static int frange_of(TypeChecker *checker, ASTNode *expr, FRange *out,
                     int depth) {
  const char *op = NULL;
  ASTNode *operand = NULL;
  if (checker) {
    checker->proof_steps++;
    if (checker->proof_steps > TYPE_CHECKER_PROOF_CEILING) {
      checker->proof_ceiling_hit = 1;
      *out = frange_unknown();
      return 0;
    }
  }
  *out = frange_unknown();
  if (!expr || depth > 32) {
    return 0;
  }
  switch (expr->type) {
  case AST_NUMBER_LITERAL: {
    NumberLiteral *literal = (NumberLiteral *)expr->data;
    if (!literal) {
      return 0;
    }
    *out = frange_exact(literal->is_float ? literal->float_value
                                          : (double)literal->int_value);
    return 1;
  }
  case AST_IDENTIFIER:
    return frange_of_identifier(checker, expr, out, depth);

  case AST_BINARY_EXPRESSION:
    return frange_of_binary(checker, expr, out, depth);

  case AST_UNARY_EXPRESSION: {
    FRange inner;
    if (!unary_parts(expr, &op, &operand) || strcmp(op, "-") != 0) {
      return 0;
    }
    if (!frange_of(checker, operand, &inner, depth + 1) || !inner.has_min ||
        !inner.has_max) {
      return 0;
    }
    out->has_min = 1;
    out->has_max = 1;
    out->min = -inner.max;
    out->max = -inner.min;
    out->err = inner.err;
    return 1;
  }
  case AST_CAST_EXPRESSION: {
    CastExpression *cast = (CastExpression *)expr->data;
    if (cast && cast->operand &&
        frange_of(checker, cast->operand, out, depth + 1)) {
      out->err += frange_eps(expr->resolved_type);
      return out->has_min || out->has_max;
    }
    *out = frange_of_type(expr->resolved_type);
    return out->has_min || out->has_max;
  }
  default:
    *out = frange_of_type(expr->resolved_type);
    return out->has_min || out->has_max;
  }
}

static void fnarrow_by_accumulator(TypeChecker *checker, const char *name,
                                   FRange *out, int depth) {
  const ASTNode *declaration;
  ASTNode *addend;
  FRange initial;
  FRange step;
  FRange widened;
  long long trips;
  int ok = 1;
  if (!checker || !name || checker->monotone_busy || depth > 8 ||
      checker->loop_trip_count == 0) {
    return;
  }
  trips = checker->loop_trips[checker->loop_trip_count - 1].trips;
  {
    Symbol *movement = movement_of(checker, name);
    if (!movement) {
      return;
    }
    declaration = movement->move_declaration;
    addend = movement->move_addend;
  }
  (void)ok;
  if (!declaration || !addend) {
    return;
  }
  checker->monotone_busy = 1;
  if (!frange_of(checker, ((const VarDeclaration *)declaration->data)->initializer,
                 &initial, depth + 1) ||
      !frange_of(checker, addend, &step, depth + 1)) {
    checker->monotone_busy = 0;
    return;
  }
  checker->monotone_busy = 0;
  if (!initial.has_min || !initial.has_max || !step.has_min ||
      !step.has_max || step.err >= FRANGE_NO_BOUND ||
      initial.err >= FRANGE_NO_BOUND) {
    return;
  }
  widened = frange_unknown();
  widened.has_min = 1;
  widened.has_max = 1;
  widened.min = frange_outward(
      frange_exact_scaled_sum(initial.min, trips, step.min, step.min < 0.0), 0);
  widened.max = frange_outward(
      frange_exact_scaled_sum(initial.max, trips, step.max, step.max > 0.0), 1);
  widened.err = initial.err + trips * (step.err + frange_eps(NULL));
  if (widened.err >= FRANGE_NO_BOUND) {
    widened.err = FRANGE_NO_BOUND;
  }
  frange_meet(out, &widened);
}

static int predicate_excludes_zero(const ASTNode *node, const char *binding) {
  const char *op = NULL;
  ASTNode *left = NULL;
  ASTNode *right = NULL;
  const char *name;
  const NumberLiteral *literal;
  if (!node) {
    return 0;
  }
  if (binary_parts((ASTNode *)node, &op, &left, &right) &&
      strcmp(op, "&&") == 0) {
    return predicate_excludes_zero(left, binding) ||
           predicate_excludes_zero(right, binding);
  }
  if (!binary_parts((ASTNode *)node, &op, &left, &right) ||
      !is_comparison(op)) {
    return 0;
  }
  name = identifier_name(left);
  if (!name || strcmp(name, binding ? binding : "value") != 0) {
    name = identifier_name(right);
    if (!name || strcmp(name, binding ? binding : "value") != 0) {
      return 0;
    }
    op = flip_operator(op);
    right = left;
  }
  if (!right || right->type != AST_NUMBER_LITERAL) {
    return 0;
  }
  literal = (const NumberLiteral *)right->data;
  if (!literal || literal->is_float || literal->int_value != 0) {
    return 0;
  }
  return strcmp(op, "!=") == 0 || strcmp(op, ">") == 0 ||
         strcmp(op, "<") == 0;
}

int type_checker_type_excludes_zero(const struct Type *type) {
  const Type *layer = (const Type *)type;
  for (; layer && layer->refined_base; layer = layer->refined_base) {
    if (layer->refine_has_range &&
        (layer->refine_min > 0 || layer->refine_max < 0)) {
      return 1;
    }
    if (layer->refinement &&
        predicate_excludes_zero(layer->refinement, layer->refine_binding)) {
      return 1;
    }
  }
  return 0;
}

int type_checker_float_bound(const struct Type *type, double *min, double *max,
                             double *err) {
  const Type *layer = (const Type *)type;
  for (; layer; layer = layer->refined_base) {
    if (layer->refine_has_frange) {
      if (min) {
        *min = layer->refine_fmin;
      }
      if (max) {
        *max = layer->refine_fmax;
      }
      if (err) {
        *err = layer->refine_ferr;
      }
      return 1;
    }
  }
  return 0;
}

int type_checker_expression_range(TypeChecker *checker, ASTNode *expr,
                                  int *has_min, long long *min, int *has_max,
                                  long long *max) {
  Range r;
  if (!checker || !expr) {
    return 0;
  }
  if (!range_of(checker, expr, &r, 0)) {
    return 0;
  }
  *has_min = r.has_min;
  *min = r.min;
  *has_max = r.has_max;
  *max = r.max;
  return 1;
}

static int guard_grow(TypeChecker *checker) {
  if (checker->guard_count < checker->guard_capacity) {
    return 1;
  }
  size_t next = checker->guard_capacity ? checker->guard_capacity * 2 : 16;
  struct TypeCheckerGuard *grown =
      realloc(checker->guards, next * sizeof(struct TypeCheckerGuard));
  if (!grown) {
    return 0;
  }
  checker->guards = grown;
  checker->guard_capacity = next;
  return 1;
}

int type_checker_push_guard(TypeChecker *checker, ASTNode *condition,
                            int negated) {
  if (!checker || !condition || !guard_grow(checker)) {
    return 0;
  }
  memset(&checker->guards[checker->guard_count], 0,
         sizeof(struct TypeCheckerGuard));
  checker->guards[checker->guard_count].condition = condition;
  checker->guards[checker->guard_count].negated = negated;
  checker->guard_count++;
  checker->guard_pushes++;
  return 1;
}

int type_checker_push_range_guard(TypeChecker *checker, const char *name,
                                  int has_min, long long min, int has_max,
                                  long long max) {
  if (!checker || !name || !guard_grow(checker)) {
    return 0;
  }
  memset(&checker->guards[checker->guard_count], 0,
         sizeof(struct TypeCheckerGuard));
  checker->guards[checker->guard_count].name = name;
  checker->guards[checker->guard_count].has_min = has_min;
  checker->guards[checker->guard_count].min = min;
  checker->guards[checker->guard_count].has_max = has_max;
  checker->guards[checker->guard_count].max = max;
  checker->guard_count++;
  checker->guard_pushes++;
  return 1;
}

size_t type_checker_guard_depth(const TypeChecker *checker) {
  return checker ? checker->guard_count : 0;
}

void type_checker_pop_guards(TypeChecker *checker, size_t depth) {
  if (checker && depth <= checker->guard_count) {
    checker->guard_count = depth;
  }
}

static int nodes_equal(const ASTNode *a, const ASTNode *b, const char *hole,
                       const ASTNode *filler, int depth);

static int nodes_equal_children(const ASTNode *a, const ASTNode *b,
                                const char *hole, const ASTNode *filler,
                                int depth) {
  if (a->child_count != b->child_count) {
    return 0;
  }
  for (size_t i = 0; i < a->child_count; i++) {
    if (!nodes_equal(a->children[i], b->children[i], hole, filler,
                     depth + 1)) {
      return 0;
    }
  }
  return 1;
}

static int nodes_equal_call(const ASTNode *a, const ASTNode *b, const char *hole,
                       const ASTNode *filler, int depth) {
    const CallExpression *ca = (const CallExpression *)a->data;
    const CallExpression *cb = (const CallExpression *)b->data;
    if (!ca || !cb || !ca->function_name || !cb->function_name ||
        strcmp(ca->function_name, cb->function_name) != 0 ||
        ca->argument_count != cb->argument_count ||
        (ca->object == NULL) != (cb->object == NULL)) {
      return 0;
    }
    if (ca->object && !nodes_equal(ca->object, cb->object, hole, filler,
                                   depth + 1)) {
      return 0;
    }
    for (size_t i = 0; i < ca->argument_count; i++) {
      if (!nodes_equal(ca->arguments[i], cb->arguments[i], hole, filler,
                       depth + 1)) {
        return 0;
      }
    }
    return 1;
}

static int nodes_equal(const ASTNode *a, const ASTNode *b, const char *hole,
                       const ASTNode *filler, int depth) {
  if (!a || !b || depth > 32) {
    return a == b;
  }
  if (hole && a->type == AST_IDENTIFIER) {
    const char *name = identifier_name(a);
    if (name && strcmp(name, hole) == 0) {
      return nodes_equal(filler, b, NULL, NULL, depth + 1);
    }
  }
  if (a->type != b->type) {
    return 0;
  }
  switch (a->type) {
  case AST_IDENTIFIER: {
    const char *na = identifier_name(a);
    const char *nb = identifier_name(b);
    return na && nb && strcmp(na, nb) == 0;
  }
  case AST_NUMBER_LITERAL: {
    const NumberLiteral *la = (const NumberLiteral *)a->data;
    const NumberLiteral *lb = (const NumberLiteral *)b->data;
    if (!la || !lb || la->is_float != lb->is_float) {
      return 0;
    }
    return la->is_float ? la->float_value == lb->float_value
                        : la->int_value == lb->int_value;
  }
  case AST_STRING_LITERAL: {
    const StringLiteral *la = (const StringLiteral *)a->data;
    const StringLiteral *lb = (const StringLiteral *)b->data;
    return la && lb && la->value && lb->value &&
           strcmp(la->value, lb->value) == 0;
  }
  case AST_BINARY_EXPRESSION: {
    const char *oa = NULL;
    const char *ob = NULL;
    ASTNode *la = NULL;
    ASTNode *ra = NULL;
    ASTNode *lb = NULL;
    ASTNode *rb = NULL;
    if (!binary_parts(a, &oa, &la, &ra) || !binary_parts(b, &ob, &lb, &rb) ||
        strcmp(oa, ob) != 0) {
      return 0;
    }
    return nodes_equal(la, lb, hole, filler, depth + 1) &&
           nodes_equal(ra, rb, hole, filler, depth + 1);
  }
  case AST_UNARY_EXPRESSION: {
    const char *oa = NULL;
    const char *ob = NULL;
    ASTNode *pa = NULL;
    ASTNode *pb = NULL;
    if (!unary_parts(a, &oa, &pa) || !unary_parts(b, &ob, &pb) ||
        strcmp(oa, ob) != 0) {
      return 0;
    }
    return nodes_equal(pa, pb, hole, filler, depth + 1);
  }
  case AST_MEMBER_ACCESS: {
    const MemberAccess *ma = (const MemberAccess *)a->data;
    const MemberAccess *mb = (const MemberAccess *)b->data;
    if (!ma || !mb || !ma->member || !mb->member ||
        strcmp(ma->member, mb->member) != 0) {
      return 0;
    }
    return nodes_equal(ma->object, mb->object, hole, filler, depth + 1);
  }
  case AST_FUNCTION_CALL:
    return nodes_equal_call(a, b, hole, filler, depth);

  case AST_INDEX_EXPRESSION:
  case AST_CAST_EXPRESSION:
    return nodes_equal_children(a, b, hole, filler, depth);
  default:
    return 0;
  }
}

typedef struct {
  const ASTNode *pattern;
  int pattern_negated;
  const ASTNode *filler;
  const char *binding;
  int found;
} MatchContext;

static void match_visit(void *raw, ASTNode *node, int negated) {
  MatchContext *ctx = (MatchContext *)raw;
  const char *op = NULL;
  ASTNode *left = NULL;
  ASTNode *right = NULL;
  if (ctx->found) {
    return;
  }
  if (negated == ctx->pattern_negated &&
      nodes_equal(ctx->pattern, node, ctx->binding, ctx->filler, 0)) {
    ctx->found = 1;
    return;
  }
  if (negated != ctx->pattern_negated && binary_parts(node, &op, &left, &right) &&
      is_comparison(op)) {
    const char *pop = NULL;
    ASTNode *pleft = NULL;
    ASTNode *pright = NULL;
    const char *flipped = negate_operator(op);
    if (flipped && binary_parts(ctx->pattern, &pop, &pleft, &pright) &&
        strcmp(pop, flipped) == 0 &&
        nodes_equal(pleft, left, ctx->binding, ctx->filler, 0) &&
        nodes_equal(pright, right, ctx->binding, ctx->filler, 0)) {
      ctx->found = 1;
    }
  }
}

static int guards_prove_atom(TypeChecker *checker, ASTNode *atom,
                             int atom_negated, ASTNode *filler,
                             const char *binding) {
  MatchContext ctx;
  ctx.pattern = atom;
  ctx.pattern_negated = atom_negated;
  ctx.filler = filler;
  ctx.binding = binding;
  ctx.found = 0;
  for (size_t i = 0; i < checker->guard_count && !ctx.found; i++) {
    struct TypeCheckerGuard *guard = &checker->guards[i];
    if (guard->condition) {
      visit_atoms(guard->condition, guard->negated, match_visit, &ctx, 0);
    }
  }
  return ctx.found;
}

static int comparison_holds(const char *op, const Range *value,
                            const Range *bound) {
  if (strcmp(op, "<") == 0) {
    return value->has_max && bound->has_min && value->max < bound->min;
  }
  if (strcmp(op, "<=") == 0) {
    return value->has_max && bound->has_min && value->max <= bound->min;
  }
  if (strcmp(op, ">") == 0) {
    return value->has_min && bound->has_max && value->min > bound->max;
  }
  if (strcmp(op, ">=") == 0) {
    return value->has_min && bound->has_max && value->min >= bound->max;
  }
  if (strcmp(op, "==") == 0) {
    return value->has_min && value->has_max && bound->has_min &&
           bound->has_max && value->min == value->max &&
           bound->min == bound->max && value->min == bound->min;
  }
  if (strcmp(op, "!=") == 0) {
    return (value->has_max && bound->has_min && value->max < bound->min) ||
           (value->has_min && bound->has_max && value->min > bound->max);
  }
  return 0;
}

static void describe(const ASTNode *node, char *out, size_t capacity,
                     int depth);

static void describe_append(char *out, size_t capacity, const char *text) {
  size_t used = strlen(out);
  if (used + 1 < capacity) {
    snprintf(out + used, capacity - used, "%s", text);
  }
}

static void describe(const ASTNode *node, char *out, size_t capacity,
                     int depth) {
  const char *op = NULL;
  ASTNode *left = NULL;
  ASTNode *right = NULL;
  ASTNode *operand = NULL;
  char scratch[64];
  if (!node || depth > 8) {
    describe_append(out, capacity, "...");
    return;
  }
  switch (node->type) {
  case AST_IDENTIFIER:
    describe_append(out, capacity, identifier_name(node) ? identifier_name(node)
                                                         : "?");
    return;
  case AST_NUMBER_LITERAL: {
    const NumberLiteral *literal = (const NumberLiteral *)node->data;
    if (literal && literal->is_float) {
      snprintf(scratch, sizeof(scratch), "%g", literal->float_value);
    } else {
      snprintf(scratch, sizeof(scratch), "%lld",
               literal ? literal->int_value : 0);
    }
    describe_append(out, capacity, scratch);
    return;
  }
  case AST_STRING_LITERAL:
    describe_append(out, capacity, "\"...\"");
    return;
  case AST_BINARY_EXPRESSION:
    if (binary_parts(node, &op, &left, &right)) {
      describe(left, out, capacity, depth + 1);
      describe_append(out, capacity, " ");
      describe_append(out, capacity, op);
      describe_append(out, capacity, " ");
      describe(right, out, capacity, depth + 1);
    }
    return;
  case AST_UNARY_EXPRESSION:
    if (unary_parts(node, &op, &operand)) {
      describe_append(out, capacity, op);
      describe(operand, out, capacity, depth + 1);
    }
    return;
  case AST_MEMBER_ACCESS: {
    const MemberAccess *member = (const MemberAccess *)node->data;
    if (member) {
      describe(member->object, out, capacity, depth + 1);
      describe_append(out, capacity, ".");
      describe_append(out, capacity, member->member ? member->member : "?");
    }
    return;
  }
  case AST_FUNCTION_CALL: {
    const CallExpression *call = (const CallExpression *)node->data;
    if (call) {
      describe_append(out, capacity,
                      call->written_name ? call->written_name
                                         : (call->function_name
                                                ? call->function_name
                                                : "?"));
      describe_append(out, capacity, "(");
      for (size_t i = 0; i < call->argument_count; i++) {
        if (i) {
          describe_append(out, capacity, ", ");
        }
        describe(call->arguments[i], out, capacity, depth + 1);
      }
      describe_append(out, capacity, ")");
    }
    return;
  }
  case AST_CAST_EXPRESSION: {
    const CastExpression *cast = (const CastExpression *)node->data;
    if (cast) {
      describe_append(out, capacity, "(");
      describe_append(out, capacity, cast->type_name ? cast->type_name : "?");
      describe_append(out, capacity, ")");
      describe(cast->operand, out, capacity, depth + 1);
    }
    return;
  }
  case AST_INDEX_EXPRESSION: {
    const ArrayIndexExpression *index = (const ArrayIndexExpression *)node->data;
    if (index) {
      describe(index->array, out, capacity, depth + 1);
      describe_append(out, capacity, "[");
      describe(index->index, out, capacity, depth + 1);
      describe_append(out, capacity, "]");
    }
    return;
  }
  default:
    describe_append(out, capacity, "<expression>");
    return;
  }
}

typedef struct {
  TypeChecker *checker;
  ASTNode *expr;
  const char *binding;
  Range value_range;
  int have_range;
  int failed;
  ASTNode *failed_atom;
  int failed_negated;
  long long steps;
  char route[192];
  int is_float;
  FRange fvalue;
  int have_frange;
} ProveContext;

static int fcomparison_holds(const char *op, const FRange *value,
                             const FRange *bound) {
  double lo;
  double hi;
  if (!value->has_min || !value->has_max) {
    return 0;
  }
  if (value->err >= FRANGE_NO_BOUND) {
    return 0;
  }
  lo = value->min;
  hi = value->max;
  if (strcmp(op, "<") == 0) {
    return bound->has_min && hi < bound->min;
  }
  if (strcmp(op, "<=") == 0) {
    return bound->has_min && hi <= bound->min;
  }
  if (strcmp(op, ">") == 0) {
    return bound->has_max && lo > bound->max;
  }
  if (strcmp(op, ">=") == 0) {
    return bound->has_max && lo >= bound->max;
  }
  if (strcmp(op, "==") == 0) {
    return bound->has_min && bound->has_max && bound->min == bound->max &&
           lo == bound->min && hi == bound->max;
  }
  return 0;
}

static void prove_visit(void *raw, ASTNode *atom, int negated) {
  ProveContext *ctx = (ProveContext *)raw;
  const char *op = NULL;
  ASTNode *left = NULL;
  ASTNode *right = NULL;
  if (ctx->failed) {
    return;
  }
  if (binary_parts(atom, &op, &left, &right) && is_comparison(op)) {
    const char *effective = negated ? negate_operator(op) : op;
    ASTNode *other = NULL;
    const char *use = NULL;
    if (identifier_name(left) &&
        strcmp(identifier_name(left), ctx->binding) == 0) {
      other = right;
      use = effective;
    } else if (identifier_name(right) &&
               strcmp(identifier_name(right), ctx->binding) == 0) {
      other = left;
      use = flip_operator(effective);
    }
    if (other && use && ctx->is_float && ctx->have_frange) {
      FRange bound;
      if (frange_of(ctx->checker, other, &bound, 0) &&
          fcomparison_holds(use, &ctx->fvalue, &bound)) {
        snprintf(ctx->route, sizeof(ctx->route),
                 "the value's interval %g..%g, widened by a relative rounding "
                 "term of %g, settles the comparison",
                 ctx->fvalue.min, ctx->fvalue.max, ctx->fvalue.err);
        return;
      }
    }
    if (other && use && ctx->have_range) {
      Range bound;
      if (range_of(ctx->checker, other, &bound, 0) &&
          comparison_holds(use, &ctx->value_range, &bound)) {
        if (ctx->value_range.has_min && ctx->value_range.has_max) {
          snprintf(ctx->route, sizeof(ctx->route),
                   "the value's range %lld..%lld settles the comparison",
                   ctx->value_range.min, ctx->value_range.max);
        } else {
          snprintf(ctx->route, sizeof(ctx->route),
                   "the value's range settles the comparison");
        }
        return;
      }
    }
  }
  if (!negated && binary_parts(atom, &op, &left, &right) && op &&
      strcmp(op, "==") == 0 && left && right) {
    const char *inner_op = NULL;
    ASTNode *inner_left = NULL;
    ASTNode *inner_right = NULL;
    long long zero = 0;
    if (binary_parts(left, &inner_op, &inner_left, &inner_right) && inner_op &&
        strcmp(inner_op, "%") == 0 && identifier_name(inner_left) &&
        strcmp(identifier_name(inner_left), ctx->binding) == 0 &&
        type_checker_eval_integer_constant_with_checker(ctx->checker, right,
                                                        &zero) &&
        zero == 0) {
      long long divisor = 0;
      if (type_checker_eval_integer_constant_with_checker(
              ctx->checker, inner_right, &divisor) &&
          divisor > 0) {
        size_t known =
            type_checker_expression_multiple_of(ctx->checker, ctx->expr, 0);
        if (known % (size_t)divisor == 0) {
          snprintf(ctx->route, sizeof(ctx->route),
                   "the value is built as a multiple of %zu", known);
          return;
        }
      }
    }
  }
  if (guards_prove_atom(ctx->checker, atom, negated, ctx->expr,
                        ctx->binding)) {
    snprintf(ctx->route, sizeof(ctx->route),
             "a dominating test in scope repeats the predicate");
    return;
  }
  ctx->failed = 1;
  ctx->failed_atom = atom;
  ctx->failed_negated = negated;
}

static int type_checker_is_float_base(const Type *type) {
  const Type *layer = type;
  while (layer && layer->refined_base) {
    layer = layer->refined_base;
  }
  return layer && layer->name &&
         (strcmp(layer->name, "float64") == 0 ||
          strcmp(layer->name, "float32") == 0);
}

static void set_failure(TypeChecker *checker, const char *text) {
  free(checker->refine_failure);
  checker->refine_failure = strdup(text);
}

static char *proof_copy(const char *text) {
  char *copy = strdup(text ? text : "?");
  return copy;
}

static void proof_log(TypeChecker *checker, const Type *layer,
                      const ASTNode *expr, const char *proof, int proven,
                      long long steps, const char *range) {
  TypeCheckerProof *entry;
  char expr_text[160] = "";
  if (checker->proof_log_count == checker->proof_log_capacity) {
    size_t grown =
        checker->proof_log_capacity ? checker->proof_log_capacity * 2 : 16;
    TypeCheckerProof *table = (TypeCheckerProof *)realloc(
        checker->proof_log, grown * sizeof(TypeCheckerProof));
    if (!table) {
      return;
    }
    checker->proof_log = table;
    checker->proof_log_capacity = grown;
  }
  describe((ASTNode *)expr, expr_text, sizeof(expr_text), 0);
  entry = &checker->proof_log[checker->proof_log_count++];
  entry->type_name = proof_copy(layer && layer->name ? layer->name : "?");
  entry->expression = proof_copy(expr_text);
  entry->proof = proof_copy(proof);
  entry->line = expr ? expr->location.line : 0;
  entry->column = expr ? expr->location.column : 0;
  entry->range = proof_copy(range ? range : "no range was needed");
  entry->proven = proven;
  entry->steps = steps;
}

int type_checker_why_proof(const TypeChecker *checker, const char *site,
                           const char *type_name, FILE *out) {
  size_t want_line = 0;
  size_t want_column = 0;
  size_t found = 0;
  if (!checker || !site || !type_name || !out) {
    return 0;
  }
  if (sscanf(site, "%zu:%zu", &want_line, &want_column) < 1) {
    fprintf(out, "`%s` is not a site; write a line or a line:column\n", site);
    return 0;
  }
  for (size_t i = 0; i < checker->proof_log_count; i++) {
    const TypeCheckerProof *p = &checker->proof_log[i];
    if (p->line != want_line) {
      continue;
    }
    if (want_column && p->column != want_column) {
      continue;
    }
    if (strcmp(p->type_name ? p->type_name : "", type_name) != 0) {
      continue;
    }
    found++;
    if (p->proven) {
      fprintf(out, "`%s` at %zu:%zu becomes '%s'.\n",
              p->expression ? p->expression : "?", p->line, p->column,
              p->type_name);
      fprintf(out, "  the range the compiler knew: %s\n",
              p->range ? p->range : "?");
      fprintf(out, "  the proof: %s\n", p->proof ? p->proof : "?");
      fprintf(out, "  it cost %lld prover steps\n", p->steps);
      fprintf(out,
              "This is the same chain and range the refusal would have "
              "printed.\n");
    } else {
      fprintf(out, "`%s` at %zu:%zu does not become '%s'.\n",
              p->expression ? p->expression : "?", p->line, p->column,
              p->type_name);
      fprintf(out, "  %s\n", p->proof ? p->proof : "?");
    }
  }
  if (!found) {
    fprintf(out,
            "no conversion into '%s' at %s: the prover settled %zu "
            "conversion%s in this build, and --report-proofs lists them\n",
            type_name, site, checker->proof_log_count,
            checker->proof_log_count == 1 ? "" : "s");
    return 0;
  }
  return 1;
}

long long type_checker_proof_steps(const TypeChecker *checker) {
  return checker ? checker->proof_steps : 0;
}

int type_checker_proof_ceiling_hit(const TypeChecker *checker) {
  return checker ? checker->proof_ceiling_hit : 0;
}

void type_checker_report_proofs(const TypeChecker *checker, FILE *out) {
  if (!checker || !out) {
    return;
  }
  for (size_t i = 0; i < checker->proof_log_count; i++) {
    const TypeCheckerProof *p = &checker->proof_log[i];
    fprintf(out, "proof %s for `%s` at %zu:%zu: %s, %lld steps (%s)\n",
            p->type_name, p->expression, p->line, p->column,
            p->proven ? "proven" : "refused", p->steps, p->proof);
  }
  fprintf(out,
          "proofs: %zu attempted, %zu proven, %zu refused, %lld steps\n",
          checker->proofs_attempted, checker->proofs_proven,
          checker->proofs_refused, checker->proof_steps);
}

int type_checker_prove_refinement(TypeChecker *checker, Type *refined,
                                  ASTNode *expr) {
  ProveContext ctx;
  Type *layer;
  if (!checker || !refined || !expr) {
    return 0;
  }
  free(checker->refine_failure);
  checker->refine_failure = NULL;
  if (getenv("METTLE_TRUST_REFINEMENTS")) {
    expr->proven_refinement = refined;
    type_checker_bind_predicate_check(checker, refined, expr);
    return 1;
  }
  memset(&ctx, 0, sizeof(ctx));
  ctx.checker = checker;
  ctx.expr = expr;
  ctx.steps = checker->proof_steps;
  snprintf(ctx.route, sizeof(ctx.route), "the value's own type admits it");
  checker->proofs_attempted++;
  if (refined->refine_uniform) {
    const char *why = NULL;
    if (type_checker_expression_is_uniform(checker, expr, &why)) {
      expr->proven_refinement = refined;
      checker->proofs_proven++;
      proof_log(checker, refined, expr,
                "no work-item index reaches the value", 1,
                checker->proof_steps - ctx.steps, "");
      return 1;
    }
    {
      char message[640];
      char expr_text[160] = "";
      describe(expr, expr_text, sizeof(expr_text), 0);
      snprintf(message, sizeof(message),
               "cannot prove `%s` is the same in every work item, which '%s' "
               "requires: %s varies by work item",
               expr_text, refined->name ? refined->name : "?",
               why ? why : "something in it");
      set_failure(checker, message);
      checker->proofs_refused++;
      proof_log(checker, refined, expr, message, 0,
                checker->proof_steps - ctx.steps, "");
      return 0;
    }
  }
  ctx.is_float = type_checker_is_float_base(refined);
  if (ctx.is_float) {
    ctx.have_frange = frange_of(checker, expr, &ctx.fvalue, 0);
  }
  ctx.have_range = range_of(checker, expr, &ctx.value_range, 0);
  for (layer = refined; layer && layer->refined_base; layer = layer->refined_base) {
    if (expr->resolved_type &&
        type_checker_types_equal(expr->resolved_type, layer)) {
      break;
    }
    if (!layer->refinement) {
      continue;
    }
    ctx.binding = layer->refine_binding ? layer->refine_binding : "value";
    visit_atoms(layer->refinement, 0, prove_visit, &ctx, 0);
    if (ctx.failed) {
      char atom_text[160] = "";
      char expr_text[160] = "";
      char message[640];
      char range_text[96] = "";
      describe(ctx.failed_atom, atom_text, sizeof(atom_text), 0);
      describe(expr, expr_text, sizeof(expr_text), 0);
      if (ctx.is_float && ctx.have_frange && ctx.fvalue.has_min &&
          ctx.fvalue.has_max) {
        snprintf(range_text, sizeof(range_text),
                 " (its interval here is %g..%g, with a relative rounding term "
                 "of %g)",
                 ctx.fvalue.min, ctx.fvalue.max, ctx.fvalue.err);
      } else if (ctx.have_range && ctx.value_range.has_min &&
                 ctx.value_range.has_max) {
        snprintf(range_text, sizeof(range_text), " (its range here is %lld..%lld)",
                 ctx.value_range.min, ctx.value_range.max);
      } else if (ctx.have_range && ctx.value_range.has_min) {
        snprintf(range_text, sizeof(range_text), " (all that is known is that it is at least %lld)",
                 ctx.value_range.min);
      } else if (ctx.have_range && ctx.value_range.has_max) {
        snprintf(range_text, sizeof(range_text), " (all that is known is that it is at most %lld)",
                 ctx.value_range.max);
      }
      snprintf(message, sizeof(message),
               "cannot prove `%s%s` for `%s`, which '%s' requires%s",
               ctx.failed_negated ? "!" : "", atom_text, expr_text,
               layer->name ? layer->name : "?", range_text);
      set_failure(checker, message);
      checker->proofs_refused++;
      proof_log(checker, layer, expr, message, 0,
                checker->proof_steps - ctx.steps, range_text);
      return 0;
    }
    {
      char known[96] = "the value's own type";
      if (ctx.have_range && ctx.value_range.has_min &&
          ctx.value_range.has_max) {
        snprintf(known, sizeof(known), "%lld..%lld", ctx.value_range.min,
                 ctx.value_range.max);
      } else if (ctx.have_range && ctx.value_range.has_min) {
        snprintf(known, sizeof(known), "at least %lld", ctx.value_range.min);
      } else if (ctx.have_range && ctx.value_range.has_max) {
        snprintf(known, sizeof(known), "at most %lld", ctx.value_range.max);
      }
      proof_log(checker, layer, expr, ctx.route, 1,
                checker->proof_steps - ctx.steps, known);
    }
    {
      char expr_text[160] = "";
      describe(expr, expr_text, sizeof(expr_text), 0);
      ir_explain_proof_held(layer->name ? layer->name : "?", expr_text,
                            expr->location.line, ctx.route,
                            "the type checker, and by lowering where it "
                            "decides whether an access needs a check");
    }
  }
  expr->proven_refinement = refined;
  type_checker_bind_predicate_check(checker, refined, expr);
  checker->proofs_proven++;
  return 1;
}

static void predicate_rename(ASTNode *node, const char *from,
                             const char *to) {
  if (!node || !from || !to) {
    return;
  }
  if (node->type == AST_IDENTIFIER) {
    Identifier *identifier = (Identifier *)node->data;
    if (identifier && identifier->name && strcmp(identifier->name, from) == 0) {
      identifier->name = (char *)string_intern(to);
    }
  }
  node->resolved_type = NULL;
  for (size_t i = 0; i < node->child_count; i++) {
    predicate_rename(node->children[i], from, to);
  }
}

typedef struct {
  TypeChecker *checker;
  int failed;
  ASTNode *failed_atom;
  int failed_negated;
} HoldsContext;

static void holds_visit(void *raw, ASTNode *atom, int negated) {
  HoldsContext *ctx = (HoldsContext *)raw;
  const char *op = NULL;
  ASTNode *left = NULL;
  ASTNode *right = NULL;
  if (ctx->failed) {
    return;
  }
  if (guards_prove_atom(ctx->checker, atom, negated, NULL, NULL)) {
    return;
  }
  if (binary_parts(atom, &op, &left, &right) && is_comparison(op)) {
    const char *effective = negated ? negate_operator(op) : op;
    Range l;
    Range r;
    if (effective && range_of(ctx->checker, left, &l, 0) &&
        range_of(ctx->checker, right, &r, 0) &&
        comparison_holds(effective, &l, &r)) {
      return;
    }
  }
  ctx->failed = 1;
  ctx->failed_atom = atom;
  ctx->failed_negated = negated;
}

static int condition_holds(TypeChecker *checker, ASTNode *condition,
                           ASTNode **failed_atom, int *failed_negated) {
  HoldsContext ctx;
  ctx.checker = checker;
  ctx.failed = 0;
  ctx.failed_atom = NULL;
  ctx.failed_negated = 0;
  visit_atoms(condition, 0, holds_visit, &ctx, 0);
  if (failed_atom) {
    *failed_atom = ctx.failed_atom;
  }
  if (failed_negated) {
    *failed_negated = ctx.failed_negated;
  }
  return !ctx.failed;
}

static int predicate_replace_field(ASTNode *node, const char *object,
                                   const char *field,
                                   const ASTNode *replacement) {
  int replaced = 0;
  if (!node) {
    return 0;
  }
  for (size_t i = 0; i < node->child_count; i++) {
    ASTNode *child = node->children[i];
    if (child && child->type == AST_MEMBER_ACCESS && child->data) {
      const MemberAccess *member = (const MemberAccess *)child->data;
      const char *base = member->object ? identifier_name(member->object) : NULL;
      if (base && member->member && strcmp(base, object) == 0 &&
          strcmp(member->member, field) == 0) {
        ASTNode *copy = ast_clone_node((ASTNode *)replacement);
        if (copy) {
          ast_destroy_node(child);
          node->children[i] = copy;
          replaced = 1;
          continue;
        }
      }
    }
    replaced |= predicate_replace_field(child, object, field, replacement);
  }
  if (node->type == AST_BINARY_EXPRESSION && node->data &&
      node->child_count >= 2) {
    BinaryExpression *binary = (BinaryExpression *)node->data;
    binary->left = node->children[0];
    binary->right = node->children[1];
  }
  if (node->type == AST_MEMBER_ACCESS && node->data && node->child_count >= 1) {
    MemberAccess *member = (MemberAccess *)node->data;
    member->object = node->children[0];
  }
  return replaced;
}

int type_checker_check_field_write(TypeChecker *checker, ASTNode *object,
                                   const char *field, ASTNode *value,
                                   SourceLocation location) {
  Type *declared = object ? object->resolved_type : NULL;
  const char *object_name = identifier_name(object);
  Type *layer;
  if (!checker || !declared || !object_name || !field || !value) {
    return 1;
  }
  for (layer = declared; layer && layer->refined_base;
       layer = layer->refined_base) {
    ASTNode *clone;
    ASTNode *failed_atom = NULL;
    int failed_negated = 0;
    if (!layer->refinement) {
      continue;
    }
    clone = ast_clone_node(layer->refinement);
    if (!clone) {
      return 1;
    }
    predicate_rename(clone,
                     layer->refine_binding ? layer->refine_binding : "value",
                     object_name);
    predicate_replace_field(clone, object_name, field, value);
    if (condition_holds(checker, clone, &failed_atom, &failed_negated)) {
      ast_destroy_node(clone);
      continue;
    }
    {
      char atom_text[160] = "";
      char message[512];
      describe(failed_atom, atom_text, sizeof(atom_text), 0);
      snprintf(message, sizeof(message),
               "writing `%s.%s` has to leave '%s' true, and `%s%s` is not "
               "proven here",
               object_name, field, layer->name ? layer->name : "?",
               failed_negated ? "!" : "", atom_text);
      type_checker_set_error_at_location(checker, location, "%s", message);
    }
    ast_destroy_node(clone);
    return 0;
  }
  return 1;
}

void type_checker_bind_predicate_check(TypeChecker *checker, Type *refined,
                                       ASTNode *expr) {
  Type *layer;
  if (!checker || !refined || !expr) {
    return;
  }
  for (layer = refined; layer && layer->refined_base;
       layer = layer->refined_base) {
    ASTNode *clone;
    Symbol *value_symbol;
    if (!layer->refinement || layer->refine_has_range ||
        layer->refine_uniform) {
      continue;
    }
    clone = ast_clone_node(layer->refinement);
    if (!clone) {
      return;
    }
    if (!symbol_table_enter_scope(checker->symbol_table, SCOPE_BLOCK)) {
      ast_destroy_node(clone);
      return;
    }
    value_symbol = symbol_create(
        layer->refine_binding ? layer->refine_binding : "value",
        SYMBOL_PARAMETER, layer->refined_base);
    if (!value_symbol ||
        !symbol_table_declare(checker->symbol_table, value_symbol)) {
      if (value_symbol) {
        symbol_destroy(value_symbol);
      }
      symbol_table_exit_scope(checker->symbol_table);
      ast_destroy_node(clone);
      return;
    }
    value_symbol->is_initialized = 1;
    value_symbol->is_used = 1;
    if (!type_checker_infer_type(checker, clone)) {
      symbol_table_exit_scope(checker->symbol_table);
      ast_destroy_node(clone);
      return;
    }
    symbol_table_exit_scope(checker->symbol_table);
    if (identifier_name(expr)) {
      predicate_rename(clone,
                       layer->refine_binding ? layer->refine_binding : "value",
                       identifier_name(expr));
      if (!type_checker_infer_type(checker, clone)) {
        ast_destroy_node(clone);
        return;
      }
      expr->proven_predicate = clone;
      expr->proven_binding = NULL;
      return;
    }
    if (layer->refined_base && layer->refined_base->kind == TYPE_STRUCT) {
      ast_destroy_node(clone);
      return;
    }
    expr->proven_predicate = clone;
    expr->proven_binding =
        layer->refine_binding ? layer->refine_binding : "value";
    return;
  }
}

void type_checker_compute_refinement_range(TypeChecker *checker,
                                           Type *refined) {
  Range r;
  Type *base;
  if (!checker || !refined || !refined->refined_base) {
    return;
  }
  base = refined->refined_base;
  r = range_of_type(base);
  if (type_checker_is_float_base(refined) && refined->refinement) {
    FNarrowContext fctx;
    FRange fr = frange_unknown();
    fctx.checker = checker;
    fctx.name = refined->refine_binding ? refined->refine_binding : "value";
    fctx.range = &fr;
    fctx.depth = 0;
    visit_atoms(refined->refinement, 0, fnarrow_visit, &fctx, 0);
    if (fr.has_min && fr.has_max) {
      refined->refine_has_frange = 1;
      refined->refine_fmin = fr.min;
      refined->refine_fmax = fr.max;
      refined->refine_ferr = fr.err;
    }
    return;
  }
  if (!type_checker_is_integer_type(base) || base->kind == TYPE_ENUM) {
    return;
  }
  if (refined->refinement) {
    NarrowContext ctx;
    ctx.checker = checker;
    ctx.name = refined->refine_binding ? refined->refine_binding : "value";
    ctx.range = &r;
    ctx.depth = 0;
    visit_atoms(refined->refinement, 0, narrow_visit, &ctx, 0);
  }
  if (r.has_min && r.has_max) {
    refined->refine_has_range = 1;
    refined->refine_min = r.min;
    refined->refine_max = r.max;
  }
}

int type_checker_refined_index_fits(const Type *index_type, size_t length) {
  if (!index_type || !index_type->refined_base || !index_type->refine_has_range) {
    return 0;
  }
  return index_type->refine_min >= 0 &&
         index_type->refine_max >= 0 &&
         (unsigned long long)index_type->refine_max < (unsigned long long)length;
}

void type_checker_report_refinement_failure(TypeChecker *checker,
                                            const ASTNode *expr,
                                            SourceLocation location) {
  char help[256];
  if (!checker || !checker->refine_failure) {
    return;
  }
  checker->has_error = 1;
  free(checker->error_message);
  checker->error_message = strdup(checker->refine_failure);
  if (checker->error_reporter) {
    SourceLocation where = expr ? expr->location : location;
    size_t span_length = expr ? type_checker_node_span_length(expr) : 1;
    SourceSpan span = source_span_from_location(where, span_length);
    if (strstr(checker->refine_failure, "varies by work item")) {
      snprintf(help, sizeof(help),
               "build the value out of the launch geometry and the kernel's "
               "own arguments; a work-item index, a subgroup lane and a load "
               "from memory each differ between work items, so nothing built "
               "from one is the same in all of them");
    } else {
      snprintf(help, sizeof(help),
               "guard the value where it is converted, for example `if (x >= 0 "
               "&& x <= 100) { ... }`; the compiler proves what a dominating "
               "test, a constant, or a narrower type establishes, and refuses "
               "to guess past that");
    }
    error_reporter_add_error_with_span_and_suggestion(
        checker->error_reporter, ERROR_TYPE, span, checker->refine_failure,
        help);
    error_reporter_set_last_code(checker->error_reporter, "P0001");
    error_reporter_set_last_label(checker->error_reporter, "unproven here");
  }
  free(checker->refine_failure);
  checker->refine_failure = NULL;
}
