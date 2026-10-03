#include "ir_numerics_check.h"
#include "ir_interp.h"
#include "ir_numerics.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define NC_FUEL 4000000000LL
#define NC_MAX_NAMES 128

typedef struct {
  const char *items[NC_MAX_NAMES];
  size_t count;
} NcNames;

typedef struct {
  NumStore *store;
  const char *contract;
  const char *harness;
  long long claims;
  long long elements;
  int failed;
  IRInterpClaim mismatch;
  int order_failed;
  char order_message[700];
  NcNames launched;
} NcSession;

typedef struct {
  char *text;
  size_t length;
  size_t capacity;
} NcText;

static void nc_append(NcText *text, const char *format, ...) {
  va_list args;
  int needed;
  va_start(args, format);
  needed = vsnprintf(NULL, 0, format, args);
  va_end(args);
  if (needed < 0) {
    return;
  }
  if (text->length + (size_t)needed + 1 > text->capacity) {
    size_t grown = text->capacity ? text->capacity * 2 : 1024;
    char *bigger;
    while (grown < text->length + (size_t)needed + 1) grown *= 2;
    bigger = (char *)realloc(text->text, grown);
    if (!bigger) {
      return;
    }
    text->text = bigger;
    text->capacity = grown;
  }
  va_start(args, format);
  vsnprintf(text->text + text->length, text->capacity - text->length, format,
            args);
  va_end(args);
  text->length += (size_t)needed;
}

static void nc_names_add(NcNames *names, const char *name) {
  if (!name) {
    return;
  }
  for (size_t i = 0; i < names->count; i++) {
    if (strcmp(names->items[i], name) == 0) {
      return;
    }
  }
  if (names->count < NC_MAX_NAMES) {
    names->items[names->count++] = name;
  }
}

static int nc_names_has(const NcNames *names, const char *name) {
  for (size_t i = 0; i < names->count; i++) {
    if (strcmp(names->items[i], name) == 0) {
      return 1;
    }
  }
  return 0;
}

static int nc_min_input(const NumStore *store, NumTerm term, int depth,
                        uint32_t *input, uint64_t *offset) {
  NumOp op = num_opcode(store, term);
  int found = 0;
  if (!term || depth > 4) {
    return 0;
  }
  if (op == NUM_INPUT) {
    uint64_t imm = num_imm(store, term);
    *input = (uint32_t)(imm >> 40);
    *offset = imm & ((1ULL << 40) - 1);
    return 1;
  }
  if (op != NUM_CONCAT && op != NUM_BYTE && op != NUM_NIBBLE &&
      op != NUM_CVT && op != NUM_SEXT) {
    return 0;
  }
  for (unsigned i = 0; i < num_arg_count(store, term); i++) {
    uint32_t in = 0;
    uint64_t at = 0;
    if (nc_min_input(store, num_arg(store, term, i), depth + 1, &in, &at) &&
        (!found || (in == *input && at < *offset))) {
      *input = in;
      *offset = at;
      found = 1;
    }
  }
  return found;
}

static int nc_step(const NumStore *store, NumTerm term, NumTerm *previous,
                   unsigned *b_first, unsigned *b_count, NumTerm *b_owner) {
  NumOp op = num_opcode(store, term);
  if (op == NUM_MMA && num_arg_count(store, term) == 33) {
    *previous = num_arg(store, term, 0);
    *b_owner = term;
    *b_first = 17;
    *b_count = 16;
    return 1;
  }
  if (op == NUM_FFMA && num_arg_count(store, term) == 3) {
    for (unsigned i = 0; i < 2; i++) {
      NumTerm scaled = num_arg(store, term, i);
      if (num_opcode(store, scaled) == NUM_BSCALE) {
        NumTerm dot = num_arg(store, scaled, 0);
        if (num_opcode(store, dot) == NUM_IDOT &&
            num_arg_count(store, dot) == 64) {
          *previous = num_arg(store, term, 2);
          *b_owner = dot;
          *b_first = 32;
          *b_count = 32;
          return 1;
        }
      }
    }
    return 0;
  }
  if (op == NUM_FMUL && num_arg_count(store, term) == 2) {
    for (unsigned i = 0; i < 2; i++) {
      NumOp inner = num_opcode(store, num_arg(store, term, i));
      if (inner == NUM_MMA || inner == NUM_FFMA) {
        *previous = num_arg(store, term, i);
        *b_count = 0;
        return 2;
      }
    }
  }
  return 0;
}

static int nc_step_offset(const NumStore *store, NumTerm owner, unsigned first,
                          unsigned count, uint32_t *input, uint64_t *offset) {
  int found = 0;
  for (unsigned i = 0; i < count; i++) {
    uint32_t in = 0;
    uint64_t at = 0;
    if (nc_min_input(store, num_arg(store, owner, first + i), 0, &in, &at) &&
        (!found || (in == *input && at < *offset))) {
      *input = in;
      *offset = at;
      found = 1;
    }
  }
  return found;
}

static void nc_check_order(NcSession *session, NumTerm out,
                           const IRInterpClaim *claim) {
  NumStore *store = session->store;
  NumTerm term = out;
  uint32_t inputs[512];
  uint64_t offsets[512];
  NumTerm steps[512];
  uint32_t distinct[8];
  size_t count = 0, kinds = 0;
  for (int guard = 0; term && guard < 100000; guard++) {
    NumTerm previous = 0, owner = 0;
    unsigned first = 0, args = 0;
    int kind = nc_step(store, term, &previous, &first, &args, &owner);
    if (!kind) {
      break;
    }
    if (kind == 1 && count < 512) {
      uint32_t input = 0;
      uint64_t offset = 0;
      if (nc_step_offset(store, owner, first, args, &input, &offset)) {
        size_t k;
        inputs[count] = input;
        offsets[count] = offset;
        steps[count] = term;
        count++;
        for (k = 0; k < kinds && distinct[k] != input; k++) {
        }
        if (k == kinds && kinds < 8) distinct[kinds++] = input;
      }
    }
    term = previous;
  }
  for (size_t k = 0; k < kinds; k++) {
    size_t descents = 0, at = 0, previous = 0, head = 0, newest = 0;
    int seen = 0;
    for (size_t b = count; b-- > 0;) {
      if (inputs[b] != distinct[k]) continue;
      if (!seen) {
        head = b;
        seen = 1;
      } else if (offsets[b] <= offsets[previous]) {
        descents++;
        at = b;
      }
      previous = b;
      newest = b;
    }
    if (descents > 1 ||
        (descents == 1 && offsets[newest] >= offsets[head])) {
      char later[200], earlier[200];
      size_t before = at;
      for (size_t b = at + 1; b < count; b++) {
        if (inputs[b] == distinct[k]) {
          before = b;
          break;
        }
      }
      num_describe(store, steps[at], later, sizeof(later));
      num_describe(store, steps[before], earlier, sizeof(earlier));
      snprintf(session->order_message, sizeof(session->order_message),
               "contract %s accumulates K ascending, and output [%lld][%lld] "
               "of the claim at line %llu visits input %u byte %llu after "
               "byte %llu: the step %s comes after %s",
               session->contract, claim->row, claim->column,
               (unsigned long long)claim->line, distinct[k],
               (unsigned long long)offsets[at],
               (unsigned long long)offsets[before], later, earlier);
      session->order_failed = 1;
      return;
    }
  }
}

static void nc_claim(void *context, const IRInterpClaim *claim) {
  NcSession *session = (NcSession *)context;
  if (claim->summary) {
    session->claims++;
    return;
  }
  session->elements++;
  if (!claim->equal && !session->failed) {
    session->failed = 1;
    session->mismatch = *claim;
  }
  if (!session->order_failed) {
    nc_check_order(session, claim->left, claim);
  }
  if (!session->order_failed && !claim->equal) {
    nc_check_order(session, claim->right, claim);
  }
}

static void nc_launch(void *context, const char *kernel,
                      const long long grid[3], const long long block[3]) {
  NcSession *session = (NcSession *)context;
  (void)grid;
  (void)block;
  nc_names_add(&session->launched, kernel);
}

#define NC_PART_FANOUT 8
#define NC_PART_DEPTH 64
#define NC_PART_VISITS 1000000

typedef struct {
  NumTerm a, b, pa, pb;
} NcPartEntry;

typedef struct {
  NcPartEntry *slots;
  size_t capacity;
  size_t used;
  size_t visits;
} NcPartMemo;

static size_t nc_part_slot(const NcPartEntry *slots, size_t capacity,
                           NumTerm a, NumTerm b) {
  uint64_t hash = (uint64_t)a * 0x9E3779B97F4A7C15ull ^
                  (uint64_t)b * 0xC2B2AE3D27D4EB4Full;
  size_t i = (size_t)(hash >> 17) & (capacity - 1);
  while (slots[i].a && !(slots[i].a == a && slots[i].b == b)) {
    i = (i + 1) & (capacity - 1);
  }
  return i;
}

static int nc_part_lookup(const NcPartMemo *memo, NumTerm a, NumTerm b,
                          NumTerm *pa, NumTerm *pb) {
  size_t i;
  if (!memo->slots) return 0;
  i = nc_part_slot(memo->slots, memo->capacity, a, b);
  if (!memo->slots[i].a) return 0;
  *pa = memo->slots[i].pa;
  *pb = memo->slots[i].pb;
  return 1;
}

static int nc_part_store(NcPartMemo *memo, NumTerm a, NumTerm b, NumTerm pa,
                         NumTerm pb) {
  size_t i;
  if ((memo->used + 1) * 2 > memo->capacity) {
    size_t capacity = memo->capacity ? memo->capacity * 2 : 1024;
    NcPartEntry *slots = (NcPartEntry *)calloc(capacity, sizeof(NcPartEntry));
    if (!slots) return 0;
    for (size_t k = 0; k < memo->capacity; k++) {
      if (memo->slots[k].a) {
        slots[nc_part_slot(slots, capacity, memo->slots[k].a,
                           memo->slots[k].b)] = memo->slots[k];
      }
    }
    free(memo->slots);
    memo->slots = slots;
    memo->capacity = capacity;
  }
  i = nc_part_slot(memo->slots, memo->capacity, a, b);
  if (!memo->slots[i].a) memo->used++;
  memo->slots[i].a = a;
  memo->slots[i].b = b;
  memo->slots[i].pa = pa;
  memo->slots[i].pb = pb;
  return 1;
}

static int nc_same_site(const NumStore *store, NumTerm x, NumTerm y) {
  const char *fx = num_function(store, x);
  const char *fy = num_function(store, y);
  return x == y ||
         (num_opcode(store, x) == num_opcode(store, y) &&
          num_line(store, x) == num_line(store, y) &&
          (fx == fy || (fx && fy && strcmp(fx, fy) == 0)));
}

#define NC_SUM_LEAVES 4096

static size_t nc_sum_leaves(const NumStore *store, NumTerm term,
                            NumTerm *leaves) {
  NumTerm stack[1024];
  size_t depth = 0, count = 0;
  stack[depth++] = term;
  while (depth) {
    NumTerm t = stack[--depth];
    if (num_opcode(store, t) == NUM_FADD && num_arg_count(store, t) == 2) {
      if (depth + 2 > sizeof(stack) / sizeof(stack[0])) return 0;
      stack[depth++] = num_arg(store, t, 1);
      stack[depth++] = num_arg(store, t, 0);
    } else {
      if (count >= NC_SUM_LEAVES) return 0;
      leaves[count++] = t;
    }
  }
  return count;
}

static int nc_compare_term(const void *a, const void *b) {
  NumTerm x = *(const NumTerm *)a, y = *(const NumTerm *)b;
  return x < y ? -1 : x > y;
}

static size_t nc_reordered_sum(const NumStore *store, NumTerm a, NumTerm b) {
  static NumTerm left[NC_SUM_LEAVES], right[NC_SUM_LEAVES];
  size_t nl, nr;
  if (a == b || num_opcode(store, a) != NUM_FADD ||
      num_opcode(store, b) != NUM_FADD) {
    return 0;
  }
  nl = nc_sum_leaves(store, a, left);
  nr = nc_sum_leaves(store, b, right);
  if (nl < 3 || nl != nr) {
    return 0;
  }
  qsort(left, nl, sizeof(NumTerm), nc_compare_term);
  qsort(right, nr, sizeof(NumTerm), nc_compare_term);
  return memcmp(left, right, nl * sizeof(NumTerm)) == 0 ? nl : 0;
}

static void nc_reorder_narrow(const NumStore *store, NumTerm *a, NumTerm *b) {
  for (int guard = 0; guard < 100000; guard++) {
    unsigned na = num_arg_count(store, *a), nb = num_arg_count(store, *b);
    int moved = 0;
    for (unsigned i = 0; i < na && !moved; i++) {
      for (unsigned j = 0; j < nb && !moved; j++) {
        NumTerm x = num_arg(store, *a, i), y = num_arg(store, *b, j);
        if (nc_reordered_sum(store, x, y)) {
          *a = x;
          *b = y;
          moved = 1;
        }
      }
    }
    if (!moved) return;
  }
}

static unsigned nc_interchangeable(const NumStore *store, NumTerm term) {
  NumOp op = num_opcode(store, term);
  unsigned count = num_arg_count(store, term);
  uint64_t imm = num_imm(store, term);
  if (op == NUM_FMAX || op == NUM_FMIN) return count;
  if ((op == NUM_FADD || op == NUM_FMUL || op == NUM_FFMA) && count >= 2) {
    return 2;
  }
  if ((op == NUM_ADD || op == NUM_MUL || op == NUM_AND || op == NUM_OR ||
       op == NUM_XOR) &&
      count == 2) {
    return 2;
  }
  if ((op == NUM_FCMP || op == NUM_ICMP) && count == 2 &&
      (imm == NUM_CMP_EQ || imm == NUM_CMP_NE)) {
    return 2;
  }
  return 0;
}

static unsigned nc_differing(const NumStore *store, NumTerm a, NumTerm b,
                             unsigned *which_a, unsigned *which_b) {
  unsigned count = num_arg_count(store, a), loose = nc_interchangeable(store, a);
  unsigned char used_a[128], used_b[128];
  unsigned differing = 0, i, j, next_b = 0;
  if (count > 128) loose = 0;
  memset(used_a, 0, sizeof(used_a));
  memset(used_b, 0, sizeof(used_b));
  for (i = 0; i < loose; i++) {
    for (j = 0; j < loose; j++) {
      if (!used_b[j] && num_arg(store, a, i) == num_arg(store, b, j)) {
        used_a[i] = 1;
        used_b[j] = 1;
        break;
      }
    }
  }
  for (i = 0; i < count; i++) {
    unsigned at_b = i;
    if (i < loose) {
      if (used_a[i]) continue;
      while (next_b < loose && used_b[next_b]) next_b++;
      at_b = next_b++;
    } else if (num_arg(store, a, i) == num_arg(store, b, i)) {
      continue;
    }
    if (differing < NC_PART_FANOUT) {
      which_a[differing] = i;
      which_b[differing] = at_b;
    }
    differing++;
  }
  return differing;
}

static int nc_both_constant(const NumStore *store, NumTerm a, NumTerm b) {
  return num_opcode(store, a) == NUM_CONST && num_opcode(store, b) == NUM_CONST;
}

static void nc_part_walk(const NumStore *store, NcPartMemo *memo, NumTerm a,
                         NumTerm b, unsigned depth, NumTerm *pa, NumTerm *pb) {
  for (int guard = 0; guard < 100000; guard++) {
    unsigned count = num_arg_count(store, a), differing;
    unsigned which[NC_PART_FANOUT], which_b[NC_PART_FANOUT];
    NumTerm ca = 0, cb = 0;
    int agree = 1;
    if (num_opcode(store, a) != num_opcode(store, b) ||
        num_width(store, a) != num_width(store, b) ||
        num_imm(store, a) != num_imm(store, b) ||
        count != num_arg_count(store, b)) {
      break;
    }
    differing = nc_differing(store, a, b, which, which_b);
    if (differing == 1) {
      ca = num_arg(store, a, which[0]);
      cb = num_arg(store, b, which_b[0]);
      if (nc_both_constant(store, ca, cb)) {
        break;
      }
      a = ca;
      b = cb;
      continue;
    }
    if (differing > 1 && nc_reordered_sum(store, a, b)) {
      nc_reorder_narrow(store, &a, &b);
      break;
    }
    if (differing == 0 || differing > NC_PART_FANOUT ||
        depth >= NC_PART_DEPTH || memo->visits >= NC_PART_VISITS) {
      break;
    }
    for (unsigned k = 0; k < differing && agree; k++) {
      NumTerm xa = num_arg(store, a, which[k]);
      NumTerm xb = num_arg(store, b, which_b[k]);
      NumTerm ra = 0, rb = 0;
      if (nc_both_constant(store, xa, xb)) {
        agree = 0;
        break;
      }
      if (!nc_part_lookup(memo, xa, xb, &ra, &rb)) {
        memo->visits++;
        nc_part_walk(store, memo, xa, xb, depth + 1, &ra, &rb);
        if (!nc_part_store(memo, xa, xb, ra, rb)) {
          agree = 0;
          break;
        }
      }
      if (k == 0) {
        ca = ra;
        cb = rb;
      } else if (!nc_same_site(store, ca, ra) || !nc_same_site(store, cb, rb)) {
        agree = 0;
      }
    }
    if (agree) {
      a = ca;
      b = cb;
    }
    break;
  }
  *pa = a;
  *pb = b;
}

static void nc_part(const NumStore *store, NumTerm a, NumTerm b, NumTerm *pa,
                    NumTerm *pb) {
  NcPartMemo memo;
  memset(&memo, 0, sizeof(memo));
  nc_part_walk(store, &memo, a, b, 0, pa, pb);
  free(memo.slots);
}

static void nc_operand(const NumStore *store, NumTerm a, NumTerm b, char *out,
                       size_t size) {
  NumOp op = num_opcode(store, a);
  unsigned count = num_arg_count(store, a);
  char da[200], db[200];
  out[0] = '\0';
  if ((op != NUM_MMA && op != NUM_IDOT) || op != num_opcode(store, b) ||
      count != num_arg_count(store, b)) {
    return;
  }
  unsigned first = op == NUM_MMA ? 1 : 0;
  unsigned half = (count - first) / 2;
  for (unsigned i = first; i < count; i++) {
    NumTerm x = num_arg(store, a, i), y = num_arg(store, b, i);
    if (x == y) continue;
    num_describe(store, x, da, sizeof(da));
    num_describe(store, y, db, sizeof(db));
    snprintf(out, size, ": %s element %u is [%s] on one side and [%s] on "
             "the other", i - first < half ? "A" : "B", (i - first) % half,
             da, db);
    return;
  }
  if (first && num_arg(store, a, 0) != num_arg(store, b, 0)) {
    num_describe(store, num_arg(store, a, 0), da, sizeof(da));
    num_describe(store, num_arg(store, b, 0), db, sizeof(db));
    snprintf(out, size, ": the accumulator is [%s] on one side and [%s] on "
             "the other", da, db);
  }
}

static void nc_explain(const NumStore *store, NumTerm term, char *out,
                       size_t size) {
  size_t used;
  unsigned count = num_arg_count(store, term);
  NumOp op = num_opcode(store, term);
  num_describe(store, term, out, size);
  if (op == NUM_CONST || op == NUM_INPUT || op == NUM_CONCAT ||
      op == NUM_MMA || op == NUM_IDOT || count == 0 || count > 3) {
    return;
  }
  used = strlen(out);
  snprintf(out + used, size - used, ", of ");
  for (unsigned i = 0; i < count; i++) {
    char inner[200];
    num_describe(store, num_arg(store, term, i), inner, sizeof(inner));
    used = strlen(out);
    snprintf(out + used, size - used, "%s[%s]",
             i == 0 ? "" : (i + 1 == count ? " and " : ", "), inner);
  }
}

static size_t nc_chain_offsets(const NumStore *store, NumTerm term,
                               uint64_t *offsets, size_t capacity) {
  size_t count = 0;
  for (int guard = 0; term && guard < 100000; guard++) {
    NumTerm previous = 0, owner = 0;
    unsigned first = 0, args = 0;
    int kind = nc_step(store, term, &previous, &first, &args, &owner);
    if (!kind) {
      break;
    }
    if (kind == 1) {
      uint32_t input = 0;
      uint64_t offset = 0;
      if (count < capacity &&
          nc_step_offset(store, owner, first, args, &input, &offset)) {
        offsets[count++] = offset;
      }
    }
    term = previous;
  }
  return count;
}

static int nc_compare_u64(const void *a, const void *b) {
  uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
  return x < y ? -1 : x > y;
}

static int nc_same_steps(const NumStore *store, NumTerm a, NumTerm b) {
  uint64_t left[512], right[512];
  size_t nl = nc_chain_offsets(store, a, left, 512);
  size_t nr = nc_chain_offsets(store, b, right, 512);
  if (nl == 0 || nl != nr) {
    return 0;
  }
  qsort(left, nl, sizeof(uint64_t), nc_compare_u64);
  qsort(right, nr, sizeof(uint64_t), nc_compare_u64);
  return memcmp(left, right, nl * sizeof(uint64_t)) == 0;
}

static int nc_is_split(const NumStore *store, NumTerm term) {
  NumTerm previous, owner;
  unsigned first, count;
  return num_opcode(store, term) == NUM_FADD &&
         nc_step(store, num_arg(store, term, 0), &previous, &first, &count,
                 &owner) &&
         nc_step(store, num_arg(store, term, 1), &previous, &first, &count,
                 &owner);
}

static void nc_fail(IRNumericsFailure *failure, const char *code,
                    const char *format, ...) {
  va_list args;
  snprintf(failure->code, sizeof(failure->code), "%s", code);
  va_start(args, format);
  vsnprintf(failure->message, sizeof(failure->message), format, args);
  va_end(args);
}

static void nc_mismatch(const NcSession *session, IRNumericsFailure *failure) {
  const NumStore *store = session->store;
  const IRInterpClaim *claim = &session->mismatch;
  NumTerm pa = 0, pb = 0;
  char da[600], db[600], operand[500];
  const char *left = num_function(store, claim->left);
  const char *right = num_function(store, claim->right);
  nc_part(store, claim->left, claim->right, &pa, &pb);
  nc_explain(store, pa, da, sizeof(da));
  nc_explain(store, pb, db, sizeof(db));
  nc_operand(store, pa, pb, operand, sizeof(operand));
  if (num_opcode(store, claim->left) == NUM_CONCAT ||
      num_opcode(store, claim->right) == NUM_CONCAT) {
    nc_fail(failure, "C0001",
            "contract %s: output [%lld][%lld] of the claim at line %llu is "
            "%s on one side and %s on the other: a side no kernel wrote",
            session->contract, claim->row, claim->column,
            (unsigned long long)claim->line, da, db);
    return;
  }
  if (nc_is_split(store, pa) || nc_is_split(store, pb)) {
    nc_fail(failure, "C0004",
            "contract %s accumulates K in one ascending chain, and %s splits "
            "it into partial sums: output [%lld][%lld] of the claim at line "
            "%llu is %s on one side and %s on the other",
            session->contract, nc_is_split(store, pa) ? left : right,
            claim->row, claim->column, (unsigned long long)claim->line, da,
            db);
    return;
  }
  if (nc_same_steps(store, pa, pb)) {
    nc_fail(failure, "C0004",
            "contract %s accumulates K ascending, and %s and %s visit the "
            "same K steps in different orders for output [%lld][%lld] of the "
            "claim at line %llu: %s on one side, %s on the other",
            session->contract, left, right, claim->row, claim->column,
            (unsigned long long)claim->line, da, db);
    return;
  }
  if (nc_reordered_sum(store, pa, pb)) {
    nc_fail(failure, "C0001",
            "contract %s: %s and %s compute output [%lld][%lld] of the claim "
            "at line %llu differently. They add the same %zu terms in a "
            "different order: %s, against %s",
            session->contract, left, right, claim->row, claim->column,
            (unsigned long long)claim->line,
            nc_reordered_sum(store, pa, pb), da, db);
    return;
  }
  nc_fail(failure, "C0001",
          "contract %s: %s and %s compute output [%lld][%lld] of the claim at "
          "line %llu differently. They part at %s, against %s%s",
          session->contract, left, right, claim->row, claim->column,
          (unsigned long long)claim->line, da, db, operand);
}

int ir_numerics_program_has_contracts(const IRProgram *program) {
  if (!program) {
    return 0;
  }
  for (size_t i = 0; i < program->function_count; i++) {
    if (program->functions[i] && program->functions[i]->numerics_contract) {
      return 1;
    }
  }
  return 0;
}

static const char *nc_premises =
    "  rests on: the PTX backend emits each operation as the instruction its "
    "term names; ptxas keeps the value of .rn arithmetic; an MMA step's "
    "result for an element depends only on that element's row of A, column of "
    "B and accumulator, the same on every SM; the kernels are race-free\n";

int ir_numerics_check(IRProgram *program, IRNumericsFailure *failure,
                      char **report) {
  NcNames contracts;
  NcText text;
  memset(&contracts, 0, sizeof(contracts));
  memset(&text, 0, sizeof(text));
  memset(failure, 0, sizeof(*failure));
  if (report) {
    *report = NULL;
  }
  for (size_t i = 0; i < program->function_count; i++) {
    IRFunction *fn = program->functions[i];
    if (fn && fn->numerics_contract) {
      nc_names_add(&contracts, fn->numerics_contract);
    }
  }
  for (size_t c = 0; c < contracts.count; c++) {
    const char *contract = contracts.items[c];
    NcNames covered;
    long long elements = 0, claims = 0;
    size_t harnesses = 0, terms = 0;
    memset(&covered, 0, sizeof(covered));
    for (size_t i = 0; i < program->function_count; i++) {
      IRFunction *fn = program->functions[i];
      NcSession session;
      IRInterpMachine *machine;
      IRInterpValue result;
      IRInterpStatus status;
      if (!fn || fn->is_kernel || !fn->numerics_contract ||
          strcmp(fn->numerics_contract, contract) != 0) {
        continue;
      }
      harnesses++;
      memset(&session, 0, sizeof(session));
      session.contract = contract;
      session.harness = fn->name;
      session.store = num_store_create();
      machine = ir_interp_create(program);
      if (!session.store || !machine) {
        num_store_destroy(session.store);
        ir_interp_destroy(machine);
        nc_fail(failure, "C0002", "contract %s: out of memory", contract);
        free(text.text);
        return 0;
      }
      ir_interp_enable_numerics(machine, session.store, nc_claim, &session);
      ir_interp_set_launch_hook(machine, nc_launch, &session);
      memset(&result, 0, sizeof(result));
      status = ir_interp_run(machine, fn, NULL, 0, &result, NC_FUEL);
      terms += num_term_count(session.store);
      if (status != IR_INTERP_OK || num_store_failed(session.store)) {
        const char *where = NULL;
        ir_interp_symbolic_site(machine, &where);
        nc_fail(failure, "C0002",
                "contract %s cannot be decided: harness %s stopped at %s",
                contract, fn->name,
                num_store_failed(session.store)
                    ? "a computation the term store could not hold"
                    : ir_interp_status_detail(machine));
        ir_interp_enable_numerics(machine, NULL, NULL, NULL);
        ir_interp_destroy(machine);
        num_store_destroy(session.store);
        free(text.text);
        return 0;
      }
      if (session.failed) {
        nc_mismatch(&session, failure);
      } else if (session.order_failed) {
        nc_fail(failure, "C0004", "%s", session.order_message);
      } else if (session.claims == 0) {
        nc_fail(failure, "C0003",
                "harness %s of contract %s makes no claim; compare the "
                "kernels' outputs with numerics_same",
                fn->name, contract);
      }
      ir_interp_enable_numerics(machine, NULL, NULL, NULL);
      ir_interp_destroy(machine);
      num_store_destroy(session.store);
      if (failure->code[0]) {
        free(text.text);
        return 0;
      }
      for (size_t k = 0; k < session.launched.count; k++) {
        nc_names_add(&covered, session.launched.items[k]);
      }
      elements += session.elements;
      claims += session.claims;
    }
    for (size_t i = 0; i < program->function_count; i++) {
      IRFunction *fn = program->functions[i];
      if (!fn || !fn->is_kernel || !fn->numerics_contract ||
          strcmp(fn->numerics_contract, contract) != 0) {
        continue;
      }
      if (!nc_names_has(&covered, fn->name)) {
        nc_fail(failure, "C0003",
                "kernel %s declares contract %s, and %s: the contract is "
                "unproven for it",
                fn->name, contract,
                harnesses ? "no harness of the contract launches it"
                          : "the contract has no harness");
        free(text.text);
        return 0;
      }
    }
    nc_append(&text, "numerics contract %s: proven for", contract);
    for (size_t k = 0; k < covered.count; k++) {
      nc_append(&text, "%s %s", k ? "," : "", covered.items[k]);
    }
    nc_append(&text,
              "\n  %lld outputs of %lld claim%s, bit for bit, at the shapes "
              "%s launch%s; other shapes are unproven (%llu terms)\n%s",
              elements, claims, claims == 1 ? "" : "s",
              harnesses == 1 ? "its harness" : "its harnesses",
              harnesses == 1 ? "es" : "", (unsigned long long)terms,
              nc_premises);
  }
  if (report) {
    *report = text.text;
  } else {
    free(text.text);
  }
  return 1;
}
