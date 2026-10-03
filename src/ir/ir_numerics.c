#include "ir_numerics.h"
#include "../common.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define NUM_MAX_ARGS 96u

typedef struct {
  uint8_t op;
  uint8_t width;
  uint8_t facts;
  uint8_t spare;
  uint32_t count;
  uint32_t args;
  uint32_t line;
  uint32_t function;
  uint32_t next;
  uint64_t imm;
} NumNode;

struct NumStore {
  NumNode *nodes;
  size_t node_count;
  size_t node_capacity;
  NumTerm *pool;
  size_t pool_count;
  size_t pool_capacity;
  uint32_t *buckets;
  size_t bucket_count;
  const char **functions;
  size_t function_count;
  size_t function_capacity;
  uint32_t line;
  uint32_t function;
  int failed;
};

static uint64_t num_mask(unsigned width) {
  return width >= 64 ? ~0ULL : ((1ULL << width) - 1ULL);
}

static int64_t num_signed(uint64_t bits, unsigned width) {
  if (width >= 64) {
    return (int64_t)bits;
  }
  bits &= num_mask(width);
  if (bits & (1ULL << (width - 1))) {
    bits |= ~num_mask(width);
  }
  return (int64_t)bits;
}

static float num_f32(uint64_t bits) {
  uint32_t word = (uint32_t)bits;
  float value;
  memcpy(&value, &word, 4);
  return value;
}

static uint64_t num_f32_bits(float value) {
  uint32_t word;
  memcpy(&word, &value, 4);
  return word;
}

static double num_f64(uint64_t bits) {
  double value;
  memcpy(&value, &bits, 8);
  return value;
}

static uint64_t num_f64_bits(double value) {
  uint64_t bits;
  memcpy(&bits, &value, 8);
  return bits;
}

static unsigned num_const_facts(unsigned width, uint64_t bits) {
  unsigned facts = 0;
  uint64_t sign, magnitude, exponent_mask, mantissa_mask;
  if (width == 16) {
    sign = 0x8000u;
    exponent_mask = 0x7C00u;
    mantissa_mask = 0x03FFu;
  } else if (width == 32) {
    sign = 0x80000000u;
    exponent_mask = 0x7F800000u;
    mantissa_mask = 0x007FFFFFu;
  } else if (width == 64) {
    sign = 0x8000000000000000ULL;
    exponent_mask = 0x7FF0000000000000ULL;
    mantissa_mask = 0x000FFFFFFFFFFFFFULL;
  } else {
    return NUM_FACT_ALL;
  }
  magnitude = bits & ~sign & num_mask(width);
  if ((bits & exponent_mask) == exponent_mask) {
    facts |= (bits & mantissa_mask) ? NUM_FACT_NAN : NUM_FACT_INF;
  }
  if (magnitude == 0) {
    facts |= NUM_FACT_ZERO;
    if (bits & sign) {
      facts |= NUM_FACT_NEGZERO;
    }
  }
  if ((bits & sign) && !(facts & NUM_FACT_NAN)) {
    facts |= NUM_FACT_NEG;
  }
  return facts;
}

static uint64_t num_hash(unsigned op, unsigned width, uint64_t imm,
                         const NumTerm *args, unsigned count) {
  uint64_t h = 1469598103934665603ULL;
  h = (h ^ op) * 1099511628211ULL;
  h = (h ^ width) * 1099511628211ULL;
  h = (h ^ (imm & 0xFFFFFFFFULL)) * 1099511628211ULL;
  h = (h ^ (imm >> 32)) * 1099511628211ULL;
  for (unsigned i = 0; i < count; i++) {
    h = (h ^ args[i]) * 1099511628211ULL;
  }
  return h ^ (h >> 29);
}

NumStore *num_store_create(void) {
  NumStore *store = (NumStore *)calloc(1, sizeof(NumStore));
  if (!store) {
    return NULL;
  }
  store->node_capacity = 1024;
  store->nodes = (NumNode *)calloc(store->node_capacity, sizeof(NumNode));
  store->pool_capacity = 4096;
  store->pool = (NumTerm *)malloc(store->pool_capacity * sizeof(NumTerm));
  store->bucket_count = 2048;
  store->buckets = (uint32_t *)calloc(store->bucket_count, sizeof(uint32_t));
  store->function_capacity = 16;
  store->functions =
      (const char **)calloc(store->function_capacity, sizeof(const char *));
  if (!store->nodes || !store->pool || !store->buckets || !store->functions) {
    num_store_destroy(store);
    return NULL;
  }
  store->node_count = 1;
  store->function_count = 1;
  store->functions[0] = "";
  return store;
}

void num_store_destroy(NumStore *store) {
  if (!store) {
    return;
  }
  free(store->nodes);
  free(store->pool);
  free(store->buckets);
  free(store->functions);
  free(store);
}

void num_store_set_site(NumStore *store, uint32_t line, const char *function) {
  size_t i;
  if (!store) {
    return;
  }
  store->line = line;
  if (!function) {
    store->function = 0;
    return;
  }
  if (store->function < store->function_count &&
      store->functions[store->function] == function) {
    return;
  }
  for (i = 0; i < store->function_count; i++) {
    if (store->functions[i] == function ||
        strcmp(store->functions[i], function) == 0) {
      store->function = (uint32_t)i;
      return;
    }
  }
  if (store->function_count == store->function_capacity) {
    size_t grown = store->function_capacity * 2;
    const char **table = (const char **)realloc(
        (void *)store->functions, grown * sizeof(const char *));
    if (!table) {
      store->failed = 1;
      return;
    }
    store->functions = table;
    store->function_capacity = grown;
  }
  store->functions[store->function_count] = function;
  store->function = (uint32_t)store->function_count++;
}

int num_store_failed(const NumStore *store) { return !store || store->failed; }

static int num_rehash(NumStore *store) {
  size_t count = store->bucket_count * 2;
  uint32_t *buckets = (uint32_t *)calloc(count, sizeof(uint32_t));
  if (!buckets) {
    return 0;
  }
  for (size_t i = 1; i < store->node_count; i++) {
    NumNode *node = &store->nodes[i];
    uint64_t h = num_hash(node->op, node->width, node->imm,
                          &store->pool[node->args], node->count);
    size_t b = (size_t)(h & (count - 1));
    node->next = buckets[b];
    buckets[b] = (uint32_t)i;
  }
  free(store->buckets);
  store->buckets = buckets;
  store->bucket_count = count;
  return 1;
}

static NumTerm num_intern(NumStore *store, NumOp op, unsigned width,
                          uint64_t imm, const NumTerm *source,
                          unsigned count, unsigned facts) {
  NumTerm args[NUM_MAX_ARGS];
  uint64_t h;
  size_t b;
  NumNode *node;
  if (!store || store->failed || count > NUM_MAX_ARGS) {
    if (store) store->failed = 1;
    return 0;
  }
  for (unsigned i = 0; i < count; i++) {
    args[i] = source[i];
    if (!args[i]) {
      store->failed = 1;
      return 0;
    }
  }
  h = num_hash(op, width, imm, args, count);
  b = (size_t)(h & (store->bucket_count - 1));
  for (uint32_t i = store->buckets[b]; i; i = store->nodes[i].next) {
    node = &store->nodes[i];
    if (node->op == op && node->width == width && node->imm == imm &&
        node->count == count &&
        (count == 0 ||
         memcmp(&store->pool[node->args], args, count * sizeof(NumTerm)) ==
             0)) {
      return i;
    }
  }
  if (store->node_count >= 0xFFFFFFF0u) {
    store->failed = 1;
    return 0;
  }
  if (store->node_count == store->node_capacity) {
    size_t grown = store->node_capacity * 2;
    NumNode *table = (NumNode *)realloc(store->nodes, grown * sizeof(NumNode));
    if (!table) {
      store->failed = 1;
      return 0;
    }
    store->nodes = table;
    store->node_capacity = grown;
  }
  if (store->pool_count + count > store->pool_capacity) {
    size_t grown = store->pool_capacity * 2;
    NumTerm *pool;
    while (grown < store->pool_count + count) grown *= 2;
    pool = (NumTerm *)realloc(store->pool, grown * sizeof(NumTerm));
    if (!pool) {
      store->failed = 1;
      return 0;
    }
    store->pool = pool;
    store->pool_capacity = grown;
  }
  node = &store->nodes[store->node_count];
  node->op = (uint8_t)op;
  node->width = (uint8_t)width;
  node->facts = (uint8_t)facts;
  node->spare = 0;
  node->count = count;
  node->args = (uint32_t)store->pool_count;
  node->line = store->line;
  node->function = store->function;
  node->imm = imm;
  memcpy(&store->pool[store->pool_count], args, count * sizeof(NumTerm));
  store->pool_count += count;
  node->next = store->buckets[b];
  store->buckets[b] = (uint32_t)store->node_count;
  store->node_count++;
  if (store->node_count * 2 > store->bucket_count && !num_rehash(store)) {
    store->failed = 1;
    return 0;
  }
  return (NumTerm)(store->node_count - 1);
}

static const NumNode *num_node(const NumStore *store, NumTerm term) {
  if (!store || !term || term >= store->node_count) {
    return NULL;
  }
  return &store->nodes[term];
}

unsigned num_width(const NumStore *store, NumTerm term) {
  const NumNode *node = num_node(store, term);
  return node ? node->width : 0;
}

NumOp num_opcode(const NumStore *store, NumTerm term) {
  const NumNode *node = num_node(store, term);
  return node ? (NumOp)node->op : (NumOp)0;
}

uint64_t num_imm(const NumStore *store, NumTerm term) {
  const NumNode *node = num_node(store, term);
  return node ? node->imm : 0;
}

unsigned num_arg_count(const NumStore *store, NumTerm term) {
  const NumNode *node = num_node(store, term);
  return node ? node->count : 0;
}

NumTerm num_arg(const NumStore *store, NumTerm term, unsigned index) {
  const NumNode *node = num_node(store, term);
  return node && index < node->count ? store->pool[node->args + index] : 0;
}

int num_constant(const NumStore *store, NumTerm term, uint64_t *bits) {
  const NumNode *node = num_node(store, term);
  if (!node || node->op != NUM_CONST) {
    return 0;
  }
  if (bits) *bits = node->imm;
  return 1;
}

unsigned num_facts(const NumStore *store, NumTerm term) {
  const NumNode *node = num_node(store, term);
  return node ? node->facts : NUM_FACT_ALL;
}

uint32_t num_line(const NumStore *store, NumTerm term) {
  const NumNode *node = num_node(store, term);
  return node ? node->line : 0;
}

const char *num_function(const NumStore *store, NumTerm term) {
  const NumNode *node = num_node(store, term);
  return node && node->function < store->function_count
             ? store->functions[node->function]
             : "";
}

size_t num_term_count(const NumStore *store) {
  return store ? store->node_count - 1 : 0;
}

NumTerm num_const(NumStore *store, unsigned width, uint64_t bits) {
  bits &= num_mask(width);
  return num_intern(store, NUM_CONST, width, bits, NULL, 0,
                    num_const_facts(width, bits));
}

NumTerm num_input(NumStore *store, uint32_t input, uint64_t offset) {
  return num_intern(store, NUM_INPUT, 8,
                    ((uint64_t)input << 40) | (offset & ((1ULL << 40) - 1)),
                    NULL, 0, NUM_FACT_ALL);
}

NumTerm num_byte(NumStore *store, NumTerm word, unsigned index) {
  const NumNode *node = num_node(store, word);
  uint64_t bits;
  if (!node) {
    if (store) store->failed = 1;
    return 0;
  }
  if (index * 8u >= node->width) {
    return num_const(store, 8, 0);
  }
  if (node->width == 8 && index == 0) {
    return word;
  }
  if (num_constant(store, word, &bits)) {
    return num_const(store, 8, (bits >> (8u * index)) & 0xFFu);
  }
  if (node->op == NUM_CONCAT) {
    return num_arg(store, word, index);
  }
  if (node->op == NUM_SEXT && index * 8u < node->imm) {
    return num_byte(store, store->pool[node->args], index);
  }
  return num_intern(store, NUM_BYTE, 8, index, &word, 1, NUM_FACT_ALL);
}

static NumTerm num_sext_under(NumStore *store, const NumTerm *bytes,
                              unsigned count) {
  const NumNode *top = num_node(store, bytes[count - 1]);
  NumTerm wide;
  if (!top || top->op != NUM_BYTE || top->imm != count - 1) {
    return 0;
  }
  wide = store->pool[top->args];
  if (num_opcode(store, wide) != NUM_SEXT ||
      num_imm(store, wide) >= 8u * count) {
    return 0;
  }
  for (unsigned i = 0; i + 1 < count; i++) {
    if (num_byte(store, wide, i) != bytes[i]) {
      return 0;
    }
  }
  return wide;
}

NumTerm num_concat(NumStore *store, const NumTerm *bytes, unsigned count) {
  int all_const = 1, same = 1;
  uint64_t value = 0;
  NumTerm word = 0;
  if (count == 0 || count > 8) {
    if (store) store->failed = 1;
    return 0;
  }
  if (count == 1) {
    return bytes[0];
  }
  for (unsigned i = 0; i < count; i++) {
    uint64_t bits;
    const NumNode *node = num_node(store, bytes[i]);
    if (!node || node->width != 8) {
      store->failed = 1;
      return 0;
    }
    if (num_constant(store, bytes[i], &bits)) {
      value |= bits << (8u * i);
    } else {
      all_const = 0;
    }
    if (node->op != NUM_BYTE || node->imm != i) {
      same = 0;
    } else if (i == 0) {
      word = num_arg(store, bytes[i], 0);
    } else if (num_arg(store, bytes[i], 0) != word) {
      same = 0;
    }
  }
  if (all_const) {
    return num_const(store, 8u * count, value);
  }
  if (same && word && num_width(store, word) == 8u * count) {
    return word;
  }
  word = num_sext_under(store, bytes, count);
  if (word && num_width(store, word) == 8u * count) {
    return word;
  }
  return num_intern(store, NUM_CONCAT, 8u * count, 0, bytes, count,
                    NUM_FACT_ALL);
}

static unsigned num_bytes_of(NumStore *store, NumTerm term, NumTerm *out) {
  unsigned width = num_width(store, term);
  unsigned count = width / 8u;
  if (count == 0 || count > 8 || width % 8u) {
    store->failed = 1;
    return 0;
  }
  if (num_opcode(store, term) == NUM_CONCAT) {
    for (unsigned i = 0; i < count; i++) out[i] = num_arg(store, term, i);
    return count;
  }
  for (unsigned i = 0; i < count; i++) out[i] = num_byte(store, term, i);
  return count;
}

NumTerm num_resize(NumStore *store, NumTerm value, unsigned width,
                   int is_signed) {
  unsigned from = num_width(store, value);
  NumTerm bytes[8];
  uint64_t bits;
  if (!from || width == 0 || width > 64 || width % 8u) {
    if (store) store->failed = 1;
    return 0;
  }
  if (from == width) {
    return value;
  }
  if (num_constant(store, value, &bits)) {
    return num_const(store, width,
                     is_signed ? (uint64_t)num_signed(bits, from) : bits);
  }
  if (width < from) {
    num_bytes_of(store, value, bytes);
    return num_concat(store, bytes, width / 8u);
  }
  {
    unsigned have = num_bytes_of(store, value, bytes);
    uint64_t top = 0;
    NumTerm wide;
    if (!have) {
      return 0;
    }
    if (!is_signed || num_constant(store, bytes[have - 1], &top)) {
      NumTerm fill = num_const(store, 8, is_signed && (top & 0x80u) ? 0xFF : 0);
      for (unsigned i = have; i < width / 8u; i++) {
        bytes[i] = fill;
      }
      return num_concat(store, bytes, width / 8u);
    }
    wide = num_sext_under(store, bytes, have);
    if (wide) {
      return num_resize(store, num_arg(store, wide, 0), width, 1);
    }
  }
  return num_intern(store, NUM_SEXT, width, from, &value, 1, NUM_FACT_ALL);
}

static int num_is_float_const(NumStore *store, NumTerm term, unsigned width,
                              double want, int want_negative_zero) {
  uint64_t bits;
  double value;
  if (!num_constant(store, term, &bits)) {
    return 0;
  }
  value = width == 32 ? (double)num_f32(bits) : num_f64(bits);
  if (value != want) {
    return 0;
  }
  if (want == 0.0) {
    int negative = width == 32 ? (bits & 0x80000000u) != 0
                               : (bits & 0x8000000000000000ULL) != 0;
    return negative == want_negative_zero;
  }
  return 1;
}

static NumTerm num_byte_op(NumStore *store, NumOp op, NumTerm x, NumTerm y) {
  uint64_t xv = 0, yv = 0;
  int xc = num_constant(store, x, &xv);
  int yc = num_constant(store, y, &yv);
  NumTerm args[2];
  if (xc && yc) {
    return num_const(store, 8,
                     op == NUM_AND ? (xv & yv)
                     : op == NUM_OR ? (xv | yv)
                                    : (xv ^ yv));
  }
  if (op == NUM_AND) {
    if ((xc && xv == 0) || (yc && yv == 0)) return num_const(store, 8, 0);
    if (xc && xv == 0xFF) return y;
    if (yc && yv == 0xFF) return x;
    if (x == y) return x;
  } else if (op == NUM_OR) {
    if (xc && xv == 0) return y;
    if (yc && yv == 0) return x;
    if ((xc && xv == 0xFF) || (yc && yv == 0xFF)) return num_const(store, 8, 0xFF);
    if (x == y) return x;
  } else {
    if (xc && xv == 0) return y;
    if (yc && yv == 0) return x;
    if (x == y) return num_const(store, 8, 0);
  }
  args[0] = x < y ? x : y;
  args[1] = x < y ? y : x;
  return num_intern(store, op, 8, 0, args, 2, NUM_FACT_ALL);
}

static int num_integer_compare(const NumStore *store, uint64_t imm,
                               const NumTerm *args, uint64_t a, uint64_t b) {
  int result = 0;
  uint64_t ua = a & num_mask(num_width(store, args[0]));
  uint64_t ub = b & num_mask(num_width(store, args[1]));
  int64_t xa = num_signed(a, num_width(store, args[0]));
  int64_t xb = num_signed(b, num_width(store, args[1]));
  switch ((NumCompare)imm) {
  case NUM_CMP_EQ: result = ua == ub; break;
  case NUM_CMP_NE: result = ua != ub; break;
  case NUM_CMP_LT: result = xa < xb; break;
  case NUM_CMP_LE: result = xa <= xb; break;
  case NUM_CMP_GT: result = xa > xb; break;
  case NUM_CMP_GE: result = xa >= xb; break;
  case NUM_CMP_ULT: result = ua < ub; break;
  case NUM_CMP_ULE: result = ua <= ub; break;
  case NUM_CMP_UGT: result = ua > ub; break;
  case NUM_CMP_UGE: result = ua >= ub; break;
  default: break;
  }
  return result;
}

static int num_integer_fold(const NumStore *store, NumOp op, unsigned width,
                            uint64_t imm, const NumTerm *args, uint64_t a,
                            uint64_t b, uint64_t *value) {
  uint64_t mask = num_mask(width);
  int64_t sa = num_signed(a, width), sb = num_signed(b, width);
  switch (op) {
  case NUM_ADD: *value = a + b; return 1;
  case NUM_SUB: *value = a - b; return 1;
  case NUM_MUL: *value = a * b; return 1;
  case NUM_AND: *value = a & b; return 1;
  case NUM_OR: *value = a | b; return 1;
  case NUM_XOR: *value = a ^ b; return 1;
  case NUM_SHL: *value = b >= width ? 0 : a << b; return 1;
  case NUM_LSHR: *value = b >= width ? 0 : (a & mask) >> b; return 1;
  case NUM_ASHR:
    *value = (uint64_t)(sa >> (b >= width ? width - 1 : b));
    return 1;
  case NUM_UDIV:
    if ((b & mask) != 0) {
      *value = (a & mask) / (b & mask);
      return 1;
    }
    return 0;
  case NUM_UREM:
    if ((b & mask) != 0) {
      *value = (a & mask) % (b & mask);
      return 1;
    }
    return 0;
  case NUM_SDIV:
    if (sb != 0 && !(sb == -1 && sa == INT64_MIN)) {
      *value = (uint64_t)(sa / sb);
      return 1;
    }
    return 0;
  case NUM_SREM:
    if (sb != 0 && !(sb == -1 && sa == INT64_MIN)) {
      *value = (uint64_t)(sa % sb);
      return 1;
    }
    return 0;
  case NUM_ICMP:
    *value = (uint64_t)num_integer_compare(store, imm, args, a, b);
    return 1;
  default:
    return 0;
  }
}

static NumTerm num_integer_bytewise(NumStore *store, NumOp op,
                                    const NumTerm *args) {
  NumTerm x[8], y[8], r[8];
  unsigned n = num_bytes_of(store, args[0], x);
  num_bytes_of(store, args[1], y);
  for (unsigned i = 0; i < n; i++) r[i] = num_byte_op(store, op, x[i], y[i]);
  return num_concat(store, r, n);
}

static NumTerm num_integer_byte_shift(NumStore *store, NumOp op,
                                      NumTerm value, uint64_t b) {
  NumTerm x[8], r[8];
  unsigned n = num_bytes_of(store, value, x);
  unsigned shift = (unsigned)(b / 8u);
  for (unsigned i = 0; i < n; i++) {
    if (op == NUM_SHL) {
      r[i] = i >= shift ? x[i - shift] : num_const(store, 8, 0);
    } else {
      r[i] = i + shift < n ? x[i + shift] : num_const(store, 8, 0);
    }
  }
  return num_concat(store, r, n);
}

static NumTerm num_integer_binary(NumStore *store, NumOp op, unsigned width,
                                  uint64_t imm, const NumTerm *args, int ac,
                                  uint64_t a, int bc, uint64_t b) {
  uint64_t mask = num_mask(width);
  NumTerm ordered[2];
  if ((op == NUM_ADD || op == NUM_SUB || op == NUM_OR || op == NUM_XOR ||
       op == NUM_SHL || op == NUM_LSHR || op == NUM_ASHR) &&
      bc && (b & mask) == 0) {
    return args[0];
  }
  if (op == NUM_ADD && ac && (a & mask) == 0) return args[1];
  if (op == NUM_MUL && bc && (b & mask) == 1) return args[0];
  if (op == NUM_MUL && ac && (a & mask) == 1) return args[1];
  if (op == NUM_MUL && ((bc && (b & mask) == 0) || (ac && (a & mask) == 0)))
    return num_const(store, width, 0);
  if (op == NUM_ADD || op == NUM_MUL || op == NUM_AND || op == NUM_OR ||
      op == NUM_XOR ||
      (op == NUM_ICMP && (imm == NUM_CMP_EQ || imm == NUM_CMP_NE))) {
    ordered[0] = args[0] < args[1] ? args[0] : args[1];
    ordered[1] = args[0] < args[1] ? args[1] : args[0];
    return num_intern(store, op, width, imm, ordered, 2, NUM_FACT_ALL);
  }
  return num_intern(store, op, width, imm, args, 2, NUM_FACT_ALL);
}

static NumTerm num_integer(NumStore *store, NumOp op, unsigned width,
                           uint64_t imm, const NumTerm *args,
                           unsigned count) {
  uint64_t a = 0, b = 0, folded = 0;
  int ac = count > 0 && num_constant(store, args[0], &a);
  int bc = count > 1 && num_constant(store, args[1], &b);
  if (count == 2 && ac && bc &&
      num_integer_fold(store, op, width, imm, args, a, b, &folded)) {
    return num_const(store, width, folded);
  }
  if ((op == NUM_AND || op == NUM_OR || op == NUM_XOR) && count == 2 &&
      width % 8u == 0 && num_width(store, args[0]) == width &&
      num_width(store, args[1]) == width) {
    return num_integer_bytewise(store, op, args);
  }
  if ((op == NUM_SHL || op == NUM_LSHR) && count == 2 && bc &&
      width % 8u == 0 && num_width(store, args[0]) == width) {
    if (b >= width) {
      return num_const(store, width, 0);
    }
    if (b % 8u == 0) {
      return num_integer_byte_shift(store, op, args[0], b);
    }
  }
  if (count == 2) {
    return num_integer_binary(store, op, width, imm, args, ac, a, bc, b);
  }
  return num_intern(store, op, width, imm, args, count, NUM_FACT_ALL);
}

static float num_max_num(float x, float y, int want_max) {
  if (x != x) return y;
  if (y != y) return x;
  if (x == y) {
    int x_negative = (num_f32_bits(x) & 0x80000000u) != 0;
    return x_negative == want_max ? y : x;
  }
  return (x > y) == want_max ? x : y;
}

static unsigned num_float_facts(NumStore *store, NumOp op, uint64_t imm,
                                const NumTerm *args, unsigned count) {
  unsigned f0 = count > 0 ? num_facts(store, args[0]) : NUM_FACT_ALL;
  unsigned f1 = count > 1 ? num_facts(store, args[1]) : NUM_FACT_ALL;
  unsigned f2 = count > 2 ? num_facts(store, args[2]) : NUM_FACT_ALL;
  unsigned facts = 0;
  switch (op) {
  case NUM_FADD:
    facts = NUM_FACT_INF;
    if ((f0 | f1) & NUM_FACT_NAN || (f0 & f1 & NUM_FACT_INF)) facts |= NUM_FACT_NAN;
    if ((f0 | f1) & NUM_FACT_NEG) facts |= NUM_FACT_NEG;
    if ((f0 & f1 & NUM_FACT_ZERO) || ((f0 | f1) & NUM_FACT_NEG)) facts |= NUM_FACT_ZERO;
    if (f0 & f1 & NUM_FACT_NEGZERO) facts |= NUM_FACT_NEGZERO;
    return facts;
  case NUM_FMUL:
    facts = NUM_FACT_INF | NUM_FACT_ZERO;
    if ((f0 | f1) & NUM_FACT_NAN || (f0 & NUM_FACT_ZERO && f1 & NUM_FACT_INF) ||
        (f0 & NUM_FACT_INF && f1 & NUM_FACT_ZERO))
      facts |= NUM_FACT_NAN;
    if ((f0 | f1) & NUM_FACT_NEG) facts |= NUM_FACT_NEG | NUM_FACT_NEGZERO;
    return facts;
  case NUM_FFMA:
    facts = NUM_FACT_ALL & ~NUM_FACT_NEGZERO;
    if (((f0 | f1) & NUM_FACT_NEG) && (f2 & NUM_FACT_NEGZERO)) facts |= NUM_FACT_NEGZERO;
    return facts;
  case NUM_FSQRT:
    facts = f0 & (NUM_FACT_ZERO | NUM_FACT_NEGZERO | NUM_FACT_INF);
    if (f0 & (NUM_FACT_NAN | NUM_FACT_NEG)) facts |= NUM_FACT_NAN;
    if (f0 & NUM_FACT_NEGZERO) facts |= NUM_FACT_NEG;
    return facts;
  case NUM_FABS:
    return f0 & (NUM_FACT_ZERO | NUM_FACT_NAN | NUM_FACT_INF);
  case NUM_FMAX:
  case NUM_FMIN: {
    unsigned all = NUM_FACT_ALL, any = 0;
    for (unsigned i = 0; i < count; i++) {
      unsigned f = num_facts(store, args[i]);
      all &= f;
      any |= f;
    }
    facts = (any & (NUM_FACT_ZERO | NUM_FACT_NEGZERO | NUM_FACT_INF)) |
            (all & NUM_FACT_NAN);
    facts |= (op == NUM_FMAX ? all : any) & NUM_FACT_NEG;
    return facts;
  }
  case NUM_SELECT:
    return f1 | f2;
  case NUM_CVT:
    if (imm == NUM_CVT_S64_TO_F32 || imm == NUM_CVT_U64_TO_F32 ||
        imm == NUM_CVT_S64_TO_F64 || imm == NUM_CVT_U64_TO_F64)
      return NUM_FACT_NEG | NUM_FACT_ZERO;
    return NUM_FACT_ALL;
  case NUM_APPROX:
    if (imm == NUM_APPROX_EX2)
      return NUM_FACT_ZERO | NUM_FACT_INF | (f0 & NUM_FACT_NAN);
    return NUM_FACT_ALL;
  default:
    return NUM_FACT_ALL;
  }
}

static int num_fold_convert(NumStore *store, uint64_t imm, uint64_t v,
                            NumTerm *out) {
  switch ((NumConvert)imm) {
  case NUM_CVT_F16_TO_F32:
    *out = num_const(store, 32, mettle_f16bits_to_f32bits((uint16_t)v));
    return 1;
  case NUM_CVT_BF16_TO_F32:
    *out = num_const(store, 32, mettle_bf16bits_to_f32bits((uint16_t)v));
    return 1;
  case NUM_CVT_F32_TO_F16:
    *out = num_const(store, 16, mettle_f32bits_to_f16bits((uint32_t)v));
    return 1;
  case NUM_CVT_F32_TO_BF16:
    *out = num_const(store, 16, mettle_f32bits_to_bf16bits((uint32_t)v));
    return 1;
  case NUM_CVT_F32_TO_F64:
    *out = num_const(store, 64, num_f64_bits((double)num_f32(v)));
    return 1;
  case NUM_CVT_F64_TO_F32:
    *out = num_const(store, 32, num_f32_bits((float)num_f64(v)));
    return 1;
  case NUM_CVT_S64_TO_F32:
    *out = num_const(store, 32, num_f32_bits((float)(int64_t)v));
    return 1;
  case NUM_CVT_U64_TO_F32:
    *out = num_const(store, 32, num_f32_bits((float)v));
    return 1;
  case NUM_CVT_S64_TO_F64:
    *out = num_const(store, 64, num_f64_bits((double)(int64_t)v));
    return 1;
  case NUM_CVT_U64_TO_F64:
    *out = num_const(store, 64, num_f64_bits((double)v));
    return 1;
  default:
    return 0;
  }
}

static int num_fold_float32(NumStore *store, NumOp op, uint64_t imm,
                            const uint64_t *v, NumTerm *out) {
  float a = num_f32(v[0]), b = num_f32(v[1]), r;
  switch (op) {
  case NUM_FADD: r = a + b; break;
  case NUM_FSUB: r = a - b; break;
  case NUM_FMUL: r = a * b; break;
  case NUM_FDIV: r = a / b; break;
  case NUM_FFMA: r = mettle_fmaf_exact(a, b, num_f32(v[2])); break;
  case NUM_FNEG: r = -a; break;
  case NUM_FABS: r = a < 0.0f || (a == 0.0f && (v[0] & 0x80000000u)) ? -a : a; break;
  case NUM_FMAX: r = num_max_num(a, b, 1); break;
  case NUM_FMIN: r = num_max_num(a, b, 0); break;
  case NUM_FCMP: {
    int result = 0;
    switch ((NumCompare)imm) {
    case NUM_CMP_EQ: result = a == b; break;
    case NUM_CMP_NE: result = a != b; break;
    case NUM_CMP_LT: result = a < b; break;
    case NUM_CMP_LE: result = a <= b; break;
    case NUM_CMP_GT: result = a > b; break;
    case NUM_CMP_GE: result = a >= b; break;
    default: return 0;
    }
    *out = num_const(store, 64, (uint64_t)result);
    return 1;
  }
  default:
    return 0;
  }
  *out = num_const(store, 32, num_f32_bits(r));
  return 1;
}

static int num_fold_float64(NumStore *store, NumOp op, uint64_t imm,
                            const uint64_t *v, NumTerm *out) {
  double a = num_f64(v[0]), b = num_f64(v[1]), r;
  switch (op) {
  case NUM_FADD: r = a + b; break;
  case NUM_FSUB: r = a - b; break;
  case NUM_FMUL: r = a * b; break;
  case NUM_FDIV: r = a / b; break;
  case NUM_FNEG: r = -a; break;
  case NUM_FCMP: {
    int result = 0;
    switch ((NumCompare)imm) {
    case NUM_CMP_EQ: result = a == b; break;
    case NUM_CMP_NE: result = a != b; break;
    case NUM_CMP_LT: result = a < b; break;
    case NUM_CMP_LE: result = a <= b; break;
    case NUM_CMP_GT: result = a > b; break;
    case NUM_CMP_GE: result = a >= b; break;
    default: return 0;
    }
    *out = num_const(store, 64, (uint64_t)result);
    return 1;
  }
  default:
    return 0;
  }
  *out = num_const(store, 64, num_f64_bits(r));
  return 1;
}

static int num_fold_float(NumStore *store, NumOp op, unsigned width,
                          uint64_t imm, const NumTerm *args, unsigned count,
                          NumTerm *out) {
  uint64_t v[3] = {0, 0, 0};
  for (unsigned i = 0; i < count && i < 3; i++) {
    if (!num_constant(store, args[i], &v[i])) return 0;
  }
  if (count > 3) return 0;
  if (op == NUM_CVT) {
    return num_fold_convert(store, imm, v[0], out);
  }
  if (op == NUM_APPROX) {
    if (imm == NUM_APPROX_EX2 && width == 32 &&
        (v[0] & 0x7FFFFFFFu) == 0) {
      *out = num_const(store, 32, num_f32_bits(1.0f));
      return 1;
    }
    return 0;
  }
  if (width == 32) {
    return num_fold_float32(store, op, imm, v, out);
  }
  if (width == 64) {
    return num_fold_float64(store, op, imm, v, out);
  }
  return 0;
}

static int num_term_less(NumTerm a, NumTerm b) { return a < b; }

static NumTerm num_float_minmax(NumStore *store, NumOp op, unsigned width,
                                uint64_t imm, NumTerm *args, unsigned count) {
  NumTerm flat[NUM_MAX_ARGS];
  unsigned n = 0;
  int have_const = 0;
  float constant = 0.0f;
  for (unsigned i = 0; i < count; i++) {
    if (num_opcode(store, args[i]) == op && num_width(store, args[i]) == width) {
      unsigned inner = num_arg_count(store, args[i]);
      for (unsigned j = 0; j < inner && n < NUM_MAX_ARGS; j++)
        flat[n++] = num_arg(store, args[i], j);
    } else if (n < NUM_MAX_ARGS) {
      flat[n++] = args[i];
    }
  }
  count = 0;
  for (unsigned i = 0; i < n; i++) {
    uint64_t bits;
    if (width == 32 && num_constant(store, flat[i], &bits)) {
      constant = have_const ? num_max_num(constant, num_f32(bits), op == NUM_FMAX)
                            : num_f32(bits);
      have_const = 1;
      continue;
    }
    args[count++] = flat[i];
  }
  if (have_const) args[count++] = num_const(store, 32, num_f32_bits(constant));
  for (unsigned i = 1; i < count; i++) {
    NumTerm key = args[i];
    unsigned j = i;
    while (j > 0 && num_term_less(key, args[j - 1])) {
      args[j] = args[j - 1];
      j--;
    }
    args[j] = key;
  }
  n = 0;
  for (unsigned i = 0; i < count; i++) {
    if (n == 0 || args[n - 1] != args[i]) args[n++] = args[i];
  }
  count = n;
  if (count == 1) return args[0];
  return num_intern(store, op, width, imm, args, count,
                    num_float_facts(store, op, imm, args, count));
}

static int num_float_identity(NumStore *store, NumOp op, unsigned width,
                              const NumTerm *args, unsigned count,
                              NumTerm *out) {
  if (op == NUM_FADD && count == 2) {
    for (int i = 0; i < 2; i++) {
      NumTerm other = args[1 - i];
      if (num_is_float_const(store, args[i], width, 0.0, 1)) {
        *out = other;
        return 1;
      }
      if (num_is_float_const(store, args[i], width, 0.0, 0) &&
          !(num_facts(store, other) & NUM_FACT_NEGZERO)) {
        *out = other;
        return 1;
      }
    }
  }
  if (op == NUM_FSUB && count == 2) {
    if (num_is_float_const(store, args[1], width, 0.0, 0)) {
      *out = args[0];
      return 1;
    }
    if (num_is_float_const(store, args[1], width, 0.0, 1) &&
        !(num_facts(store, args[0]) & NUM_FACT_NEGZERO)) {
      *out = args[0];
      return 1;
    }
  }
  if (op == NUM_FMUL && count == 2) {
    if (num_is_float_const(store, args[1], width, 1.0, 0)) {
      *out = args[0];
      return 1;
    }
    if (num_is_float_const(store, args[0], width, 1.0, 0)) {
      *out = args[1];
      return 1;
    }
  }
  if (op == NUM_FDIV && count == 2 &&
      num_is_float_const(store, args[1], width, 1.0, 0)) {
    *out = args[0];
    return 1;
  }
  return 0;
}

static NumTerm num_float(NumStore *store, NumOp op, unsigned width,
                         uint64_t imm, const NumTerm *source,
                         unsigned count) {
  NumTerm args[NUM_MAX_ARGS];
  NumTerm folded = 0;
  unsigned facts;
  if (count > NUM_MAX_ARGS) {
    store->failed = 1;
    return 0;
  }
  memcpy(args, source, count * sizeof(NumTerm));
  if (op == NUM_SELECT && count == 3) {
    uint64_t c;
    if (num_constant(store, args[0], &c)) return c ? args[1] : args[2];
    if (args[1] == args[2]) return args[1];
    return num_intern(store, op, width, imm, args, count,
                      num_float_facts(store, op, imm, args, count));
  }
  if (op == NUM_FMAX || op == NUM_FMIN) {
    return num_float_minmax(store, op, width, imm, args, count);
  }
  if (num_fold_float(store, op, width, imm, args, count, &folded)) {
    return folded;
  }
  if (num_float_identity(store, op, width, args, count, &folded)) {
    return folded;
  }
  if (op == NUM_FFMA && count == 3) {
    NumTerm pair[2];
    if (num_is_float_const(store, args[0], width, 1.0, 0)) {
      pair[0] = args[1];
      pair[1] = args[2];
      return num_float(store, NUM_FADD, width, 0, pair, 2);
    }
    if (num_is_float_const(store, args[1], width, 1.0, 0)) {
      pair[0] = args[0];
      pair[1] = args[2];
      return num_float(store, NUM_FADD, width, 0, pair, 2);
    }
    if (num_is_float_const(store, args[2], width, 0.0, 1)) {
      return num_float(store, NUM_FMUL, width, 0, args, 2);
    }
  }
  if (op == NUM_FNEG && count == 1 && num_opcode(store, args[0]) == NUM_FNEG) {
    return num_arg(store, args[0], 0);
  }
  if (op == NUM_FABS && count == 1 &&
      (num_opcode(store, args[0]) == NUM_FABS ||
       num_opcode(store, args[0]) == NUM_FNEG)) {
    NumTerm inner = num_opcode(store, args[0]) == NUM_FABS
                        ? args[0]
                        : num_arg(store, args[0], 0);
    if (num_opcode(store, inner) == NUM_FABS) return inner;
    args[0] = inner;
  }
  if ((op == NUM_FADD || op == NUM_FMUL ||
       (op == NUM_FCMP && (imm == NUM_CMP_EQ || imm == NUM_CMP_NE))) &&
      count == 2 && args[1] < args[0]) {
    NumTerm t = args[0];
    args[0] = args[1];
    args[1] = t;
  }
  if (op == NUM_FFMA && count == 3 && args[1] < args[0]) {
    NumTerm t = args[0];
    args[0] = args[1];
    args[1] = t;
  }
  facts = num_float_facts(store, op, imm, args, count);
  return num_intern(store, op, width, imm, args, count, facts);
}

static NumTerm num_special(NumStore *store, NumOp op, unsigned width,
                           uint64_t imm, const NumTerm *args,
                           unsigned count) {
  uint64_t bits[NUM_MAX_ARGS];
  int all_const = count <= NUM_MAX_ARGS;
  for (unsigned i = 0; i < count && all_const; i++) {
    all_const = num_constant(store, args[i], &bits[i]);
  }
  if (op == NUM_NIBBLE && count == 1 && all_const) {
    unsigned high = (unsigned)(imm & 1u);
    unsigned is_signed = (unsigned)((imm >> 1) & 1u);
    unsigned zero = (unsigned)((imm >> 8) & 0xFFu);
    unsigned nibble = high ? (unsigned)(bits[0] >> 4) & 15u
                           : (unsigned)bits[0] & 15u;
    int value = is_signed ? (int)nibble - (nibble >= 8u ? 16 : 0)
                          : (int)nibble - (int)zero;
    return num_const(store, 8, (uint64_t)(uint8_t)(int8_t)value);
  }
  if (op == NUM_IDOT && all_const && count % 2 == 0) {
    int64_t sum = 0;
    unsigned half = count / 2;
    for (unsigned i = 0; i < half; i++) {
      sum += num_signed(bits[i], num_width(store, args[i])) *
             num_signed(bits[half + i], num_width(store, args[half + i]));
    }
    return num_const(store, width, (uint64_t)sum);
  }
  if (op == NUM_BSCALE && count == 2 && all_const) {
    float start = 12582912.0f;
    float scale = num_f32(bits[1]);
    float biased = start + (float)num_signed(bits[0], num_width(store, args[0]));
    float result = mettle_fmaf_exact(biased, scale, -(start * scale));
    return num_const(store, 32, num_f32_bits(result));
  }
  return num_intern(store, op, width, imm, args, count, NUM_FACT_ALL);
}

NumTerm num_op(NumStore *store, NumOp op, unsigned width, uint64_t imm,
               const NumTerm *args, unsigned count) {
  if (!store || store->failed) {
    return 0;
  }
  for (unsigned i = 0; i < count; i++) {
    if (!args[i]) {
      store->failed = 1;
      return 0;
    }
  }
  switch (op) {
  case NUM_ADD:
  case NUM_SUB:
  case NUM_MUL:
  case NUM_AND:
  case NUM_OR:
  case NUM_XOR:
  case NUM_SHL:
  case NUM_LSHR:
  case NUM_ASHR:
  case NUM_UDIV:
  case NUM_SDIV:
  case NUM_UREM:
  case NUM_SREM:
  case NUM_ICMP:
    return num_integer(store, op, width, imm, args, count);
  case NUM_SEXT:
    return count == 1 ? num_resize(store, args[0], width, 1) : 0;
  case NUM_FADD:
  case NUM_FSUB:
  case NUM_FMUL:
  case NUM_FDIV:
  case NUM_FFMA:
  case NUM_FSQRT:
  case NUM_FNEG:
  case NUM_FABS:
  case NUM_FMAX:
  case NUM_FMIN:
  case NUM_FCMP:
  case NUM_SELECT:
  case NUM_CVT:
  case NUM_APPROX:
    return num_float(store, op, width, imm, args, count);
  case NUM_IDOT:
  case NUM_MMA:
  case NUM_NIBBLE:
  case NUM_BSCALE:
    return num_special(store, op, width, imm, args, count);
  default:
    store->failed = 1;
    return 0;
  }
}

static const char *num_compare_name(uint64_t imm) {
  switch ((NumCompare)imm) {
  case NUM_CMP_EQ: return "eq";
  case NUM_CMP_NE: return "ne";
  case NUM_CMP_LT: return "lt";
  case NUM_CMP_LE: return "le";
  case NUM_CMP_GT: return "gt";
  case NUM_CMP_GE: return "ge";
  case NUM_CMP_ULT: return "lo";
  case NUM_CMP_ULE: return "ls";
  case NUM_CMP_UGT: return "hi";
  case NUM_CMP_UGE: return "hs";
  default: return "?";
  }
}

static const char *num_float_op_name(const NumNode *node) {
  switch ((NumOp)node->op) {
  case NUM_FADD: return node->width == 64 ? "add.rn.f64" : "add.rn.f32";
  case NUM_FSUB: return node->width == 64 ? "sub.rn.f64" : "sub.rn.f32";
  case NUM_FMUL: return node->width == 64 ? "mul.rn.f64" : "mul.rn.f32";
  case NUM_FDIV: return node->width == 64 ? "div.rn.f64" : "div.rn.f32";
  case NUM_FFMA: return node->width == 64 ? "fma.rn.f64" : "fma.rn.f32";
  case NUM_FSQRT: return "sqrt.rn.f32";
  case NUM_FNEG: return "neg.f32";
  case NUM_FABS: return "abs.f32";
  case NUM_FMAX: return "max.f32";
  case NUM_FMIN: return "min.f32";
  case NUM_FCMP: return "float compare";
  case NUM_SELECT: return "selp";
  case NUM_CVT: return "cvt";
  case NUM_APPROX:
    switch ((NumApprox)node->imm) {
    case NUM_APPROX_EX2: return "ex2.approx.f32";
    case NUM_APPROX_LG2: return "lg2.approx.f32";
    case NUM_APPROX_RSQRT: return "rsqrt.approx.f32";
    case NUM_APPROX_SIN: return "sin.approx.f32";
    case NUM_APPROX_COS: return "cos.approx.f32";
    case NUM_APPROX_TANH: return "tanh.approx.f32";
    default: return "approx";
    }
  default: return "?";
  }
}

const char *num_op_name(const NumStore *store, NumTerm term) {
  const NumNode *node = num_node(store, term);
  if (!node) return "nothing";
  switch ((NumOp)node->op) {
  case NUM_CONST: return "constant";
  case NUM_INPUT: return "input byte";
  case NUM_BYTE: return "byte of";
  case NUM_CONCAT: return "bytes";
  case NUM_ADD: return "add";
  case NUM_SUB: return "sub";
  case NUM_MUL: return "mul";
  case NUM_AND: return "and";
  case NUM_OR: return "or";
  case NUM_XOR: return "xor";
  case NUM_SHL: return "shl";
  case NUM_LSHR: return "shr.u";
  case NUM_ASHR: return "shr.s";
  case NUM_UDIV: return "div.u";
  case NUM_SDIV: return "div.s";
  case NUM_UREM: return "rem.u";
  case NUM_SREM: return "rem.s";
  case NUM_SEXT: return "sign extension";
  case NUM_ICMP: return "integer compare";
  case NUM_IDOT: return "int8 block dot";
  case NUM_MMA: return "mma.m16n8k16 step";
  case NUM_NIBBLE: return "nibble";
  case NUM_BSCALE: return "block scale";
  default: return num_float_op_name(node);
  }
}

void num_describe(const NumStore *store, NumTerm term, char *out,
                  size_t size) {
  const NumNode *node = num_node(store, term);
  if (!out || size == 0) return;
  if (!node) {
    snprintf(out, size, "nothing");
    return;
  }
  if (node->op == NUM_CONST) {
    if (node->width == 32) {
      snprintf(out, size, "the constant %.9g", (double)num_f32(node->imm));
    } else {
      snprintf(out, size, "the constant 0x%llx",
               (unsigned long long)node->imm);
    }
    return;
  }
  if (node->op == NUM_INPUT) {
    snprintf(out, size, "input %u byte %llu", (unsigned)(node->imm >> 40),
             (unsigned long long)(node->imm & ((1ULL << 40) - 1)));
    return;
  }
  if (node->op == NUM_CONCAT || node->op == NUM_CVT) {
    NumTerm inner = node->op == NUM_CVT ? store->pool[node->args] : term;
    const NumNode *bytes = num_node(store, inner);
    if (bytes && bytes->op == NUM_CONCAT) {
      uint64_t first = 0;
      unsigned input = 0, ok = 1;
      for (unsigned i = 0; i < bytes->count && ok; i++) {
        const NumNode *b = num_node(store, store->pool[bytes->args + i]);
        uint64_t offset;
        if (!b || b->op != NUM_INPUT) {
          ok = 0;
          break;
        }
        offset = b->imm & ((1ULL << 40) - 1);
        if (i == 0) {
          first = offset;
          input = (unsigned)(b->imm >> 40);
        } else if ((unsigned)(b->imm >> 40) != input || offset != first + i) {
          ok = 0;
        }
      }
      if (ok) {
        snprintf(out, size, "%sinput %u bytes %llu..%llu",
                 node->op == NUM_CVT ? "converted " : "", input,
                 (unsigned long long)first,
                 (unsigned long long)(first + bytes->count - 1));
        return;
      }
    }
  }
  if (node->op == NUM_ICMP || node->op == NUM_FCMP) {
    snprintf(out, size, "%s.%s at line %u of %s", num_op_name(store, term),
             num_compare_name(node->imm), node->line,
             store->functions[node->function]);
    return;
  }
  if (node->line) {
    snprintf(out, size, "%s at line %u of %s", num_op_name(store, term),
             node->line, store->functions[node->function]);
  } else {
    snprintf(out, size, "%s", num_op_name(store, term));
  }
}
