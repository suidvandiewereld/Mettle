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

static void nc_part(const NumStore *store, NumTerm a, NumTerm b, NumTerm *pa,
                    NumTerm *pb) {
  for (int guard = 0; guard < 100000; guard++) {
    unsigned count = num_arg_count(store, a), i, differing = 0, at = 0;
    if (num_opcode(store, a) != num_opcode(store, b) ||
        num_width(store, a) != num_width(store, b) ||
        num_imm(store, a) != num_imm(store, b) ||
        count != num_arg_count(store, b)) {
      break;
    }
    for (i = 0; i < count; i++) {
      if (num_arg(store, a, i) != num_arg(store, b, i)) {
        if (!differing) at = i;
        differing++;
      }
    }
    if (differing != 1) {
      break;
    }
    a = num_arg(store, a, at);
    b = num_arg(store, b, at);
  }
  *pa = a;
  *pb = b;
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
  char da[600], db[600];
  const char *left = num_function(store, claim->left);
  const char *right = num_function(store, claim->right);
  nc_part(store, claim->left, claim->right, &pa, &pb);
  nc_explain(store, pa, da, sizeof(da));
  nc_explain(store, pb, db, sizeof(db));
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
  nc_fail(failure, "C0001",
          "contract %s: %s and %s compute output [%lld][%lld] of the claim at "
          "line %llu differently. They part at %s, against %s",
          session->contract, left, right, claim->row, claim->column,
          (unsigned long long)claim->line, da, db);
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
