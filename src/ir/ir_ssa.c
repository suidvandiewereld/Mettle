#include "../common.h"
#include "ir.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define IR_SSA_VERSION_PREFIX "__ssa"
#define IR_SSA_BLOCK_LABEL_PREFIX "__ssa_blk_"

int ir_ssa_enabled(void) {
  static int cached = -1;
  if (cached < 0) {
    const char *setting = getenv("METTLE_IR_SSA");
    cached = (setting && *setting && strcmp(setting, "0") != 0) ? 1 : 0;
  }
  return cached;
}

static int ir_ssa_type_is_scalar(const MtlcType *type) {
  if (!type) {
    return 0;
  }
  switch (type->kind) {
  case MTLC_TYPE_INT8:
  case MTLC_TYPE_INT16:
  case MTLC_TYPE_INT32:
  case MTLC_TYPE_INT64:
  case MTLC_TYPE_UINT8:
  case MTLC_TYPE_UINT16:
  case MTLC_TYPE_UINT32:
  case MTLC_TYPE_UINT64:
  case MTLC_TYPE_BOOL:
  case MTLC_TYPE_FLOAT32:
  case MTLC_TYPE_FLOAT64:
  case MTLC_TYPE_POINTER:
  case MTLC_TYPE_FUNCTION_POINTER:
    return 1;
  default:
    return 0;
  }
}

static int ir_ssa_opcode_requires_symbol(IROpcode op) {
  switch (op) {
  case IR_OP_ROTATE_ADD:
  case IR_OP_SIMD_AFFINE_MAP_F64:
  case IR_OP_SIMD_AFFINE_MAP_F32:
  case IR_OP_SIMD_SILU_F32:
  case IR_OP_PREFETCH:
  case IR_OP_NEW:
  case IR_OP_SIMD_FIND:
  case IR_OP_COUNT_WORD_STARTS:
    return 1;
  default:
    return 0;
  }
}

static int ir_ssa_destination_is_pure_write(const IRInstruction *instruction) {
  if (!instruction) {
    return 0;
  }
  switch (instruction->op) {
  case IR_OP_ASSIGN:
  case IR_OP_ADDRESS_OF:
  case IR_OP_LOAD:
  case IR_OP_BINARY:
  case IR_OP_UNARY:
  case IR_OP_ROTATE_ADD:
  case IR_OP_CALL:
  case IR_OP_CALL_INDIRECT:
  case IR_OP_NEW:
  case IR_OP_CAST:
  case IR_OP_SELECT:
  case IR_OP_PHI:
    return 1;
  default:
    return 0;
  }
}

static IROperand *ir_ssa_operand_at(IRInstruction *instruction, size_t index) {
  switch (index) {
  case 0:
    return &instruction->dest;
  case 1:
    return &instruction->lhs;
  case 2:
    return &instruction->rhs;
  default:
    break;
  }
  const size_t argument = index - 3;
  if (!instruction->arguments || argument >= instruction->argument_count) {
    return NULL;
  }
  return &instruction->arguments[argument];
}

static size_t ir_ssa_operand_count(const IRInstruction *instruction) {
  return 3 + instruction->argument_count;
}

typedef struct {
  char *type_name;
  MtlcType *value_type;
  SourceLocation location;
  int is_float;
  int float_bits;
  int is_unsigned;
  unsigned char alias_class;
} IRSsaDeclInfo;

typedef struct {
  unsigned char *promotable;
  uint32_t *declared_at;
  IRSsaDeclInfo *declarations;
  size_t value_count;
} IRSsaCandidates;

static void ir_ssa_candidates_destroy(IRSsaCandidates *candidates) {
  if (candidates->declarations) {
    for (size_t i = 0; i < candidates->value_count; i++) {
      free(candidates->declarations[i].type_name);
    }
  }
  free(candidates->declarations);
  free(candidates->promotable);
  free(candidates->declared_at);
  memset(candidates, 0, sizeof(*candidates));
}

static int ir_ssa_candidates_build(IRFunction *function,
                                   IRSsaCandidates *candidates) {
  memset(candidates, 0, sizeof(*candidates));
  const size_t value_count = ir_value_table_count(&function->values) + 1;
  candidates->value_count = value_count;
  candidates->promotable = (unsigned char *)calloc(value_count, 1);
  candidates->declared_at = (uint32_t *)malloc(value_count * sizeof(uint32_t));
  candidates->declarations =
      (IRSsaDeclInfo *)calloc(value_count, sizeof(IRSsaDeclInfo));
  if (!candidates->promotable || !candidates->declared_at ||
      !candidates->declarations) {
    ir_ssa_candidates_destroy(candidates);
    return 0;
  }
  for (size_t i = 0; i < value_count; i++) {
    candidates->declared_at[i] = IR_INSTRUCTION_NONE;
  }

  for (size_t i = 0; i < function->instruction_count; i++) {
    IRInstruction *instruction = &function->instructions[i];
    if (instruction->op != IR_OP_DECLARE_LOCAL) {
      continue;
    }
    if (instruction->dest.kind != IR_OPERAND_SYMBOL ||
        instruction->dest.value_id == IR_VALUE_ID_NONE ||
        instruction->dest.value_id >= value_count) {
      continue;
    }
    const uint32_t id = instruction->dest.value_id;
    if (candidates->declared_at[id] != IR_INSTRUCTION_NONE) {
      candidates->promotable[id] = 0;
      continue;
    }
    candidates->declared_at[id] = (uint32_t)i;
    if (!ir_ssa_type_is_scalar(instruction->value_type) ||
        instruction->is_volatile || !instruction->text) {
      continue;
    }
    IRSsaDeclInfo *info = &candidates->declarations[id];
    info->type_name = mettle_strdup(instruction->text);
    info->value_type = instruction->value_type;
    info->location = instruction->location;
    info->is_float = instruction->is_float;
    info->float_bits = instruction->float_bits;
    info->is_unsigned = instruction->is_unsigned;
    info->alias_class = instruction->alias_class;
    if (!info->type_name) {
      continue;
    }
    candidates->promotable[id] = 1;
  }

  for (size_t i = 0; i < function->instruction_count; i++) {
    if (function->instructions[i].op != IR_OP_INLINE_ASM) {
      continue;
    }
    memset(candidates->promotable, 0, value_count);
    return 1;
  }

  for (size_t p = 0; p < function->parameter_count; p++) {
    if (!function->parameter_names || !function->parameter_names[p]) {
      continue;
    }
    const uint32_t id = ir_value_table_lookup(
        &function->values, (unsigned char)IR_OPERAND_SYMBOL,
        function->parameter_names[p]);
    if (id != IR_VALUE_ID_NONE && id < value_count) {
      candidates->promotable[id] = 0;
    }
  }

  for (size_t i = 0; i < function->instruction_count; i++) {
    IRInstruction *instruction = &function->instructions[i];
    const int writes = ir_instruction_writes_destination(instruction);
    const size_t operands = ir_ssa_operand_count(instruction);
    for (size_t j = 0; j < operands; j++) {
      IROperand *operand = ir_ssa_operand_at(instruction, j);
      if (!operand || operand->kind != IR_OPERAND_SYMBOL ||
          operand->value_id == IR_VALUE_ID_NONE ||
          operand->value_id >= value_count) {
        continue;
      }
      const uint32_t id = operand->value_id;
      if (!candidates->promotable[id]) {
        continue;
      }
      if (instruction->is_volatile || instruction->op == IR_OP_INLINE_ASM ||
          instruction->op == IR_OP_GPU_LAUNCH) {
        candidates->promotable[id] = 0;
        continue;
      }
      if (instruction->op == IR_OP_ADDRESS_OF && j == 1) {
        candidates->promotable[id] = 0;
        continue;
      }
      if (ir_ssa_opcode_requires_symbol(instruction->op)) {
        candidates->promotable[id] = 0;
        continue;
      }
      if (j == 0 && instruction->op != IR_OP_DECLARE_LOCAL &&
          (!writes || !ir_ssa_destination_is_pure_write(instruction))) {
        candidates->promotable[id] = 0;
        continue;
      }
    }
  }

  return 1;
}

static uint32_t ir_ssa_fresh_version(IRFunction *function, uint32_t base_id,
                                     unsigned *counter) {
  const char *base = ir_value_table_name(&function->values, base_id);
  if (!base) {
    return IR_VALUE_ID_NONE;
  }
  for (unsigned attempt = 0; attempt < 4096; attempt++) {
    char name[512];
    snprintf(name, sizeof(name), "%s" IR_SSA_VERSION_PREFIX "%u", base,
             (*counter)++);
    if (ir_value_table_lookup(&function->values,
                              (unsigned char)IR_OPERAND_SYMBOL,
                              name) != IR_VALUE_ID_NONE) {
      continue;
    }
    return ir_value_table_intern(&function->values,
                                 (unsigned char)IR_OPERAND_SYMBOL, name);
  }
  return IR_VALUE_ID_NONE;
}

static void ir_ssa_set_operand(IRFunction *function, IROperand *operand,
                               uint32_t id) {
  const char *name = ir_value_table_name(&function->values, id);
  if (!name) {
    return;
  }
  const unsigned char kind = ir_value_table_kind(&function->values, id);
  const int float_bits = operand->float_bits;
  ir_operand_destroy(operand);
  *operand = (kind == IR_OPERAND_TEMP) ? ir_operand_temp(name)
                                       : ir_operand_symbol(name);
  operand->float_bits = float_bits;
  operand->value_id = id;
}

typedef struct {
  uint32_t *entries;
  size_t count;
  size_t capacity;
} IRSsaStack;

static int ir_ssa_stack_push(IRSsaStack *stack, uint32_t value) {
  if (stack->count >= stack->capacity) {
    const size_t capacity = stack->capacity ? stack->capacity * 2 : 8;
    uint32_t *grown =
        (uint32_t *)realloc(stack->entries, capacity * sizeof(uint32_t));
    if (!grown) {
      return 0;
    }
    stack->entries = grown;
    stack->capacity = capacity;
  }
  stack->entries[stack->count++] = value;
  return 1;
}

typedef struct {
  IRFunction *function;
  const IRSsaCandidates *candidates;
  const IRBasicBlock *blocks;
  size_t block_count;
  const IRDomTree *dom;
  IRSsaStack *stacks;
  uint32_t *phi_origin;
  uint32_t *created_versions;
  uint32_t *created_bases;
  size_t created_count;
  size_t created_capacity;
  unsigned counter;
  int failed;
} IRSsaRenamer;

static int ir_ssa_record_version(IRSsaRenamer *renamer, uint32_t version,
                                 uint32_t base) {
  if (renamer->created_count >= renamer->created_capacity) {
    const size_t capacity =
        renamer->created_capacity ? renamer->created_capacity * 2 : 32;
    uint32_t *versions = (uint32_t *)realloc(renamer->created_versions,
                                             capacity * sizeof(uint32_t));
    if (!versions) {
      return 0;
    }
    renamer->created_versions = versions;
    uint32_t *bases = (uint32_t *)realloc(renamer->created_bases,
                                          capacity * sizeof(uint32_t));
    if (!bases) {
      return 0;
    }
    renamer->created_bases = bases;
    renamer->created_capacity = capacity;
  }
  renamer->created_versions[renamer->created_count] = version;
  renamer->created_bases[renamer->created_count] = base;
  renamer->created_count++;
  return 1;
}

static uint32_t ir_ssa_reaching(const IRSsaRenamer *renamer, uint32_t id) {
  const IRSsaStack *stack = &renamer->stacks[id];
  return stack->count ? stack->entries[stack->count - 1] : id;
}

static void ir_ssa_rename_block(IRSsaRenamer *renamer, size_t block_index) {
  if (renamer->failed || block_index >= renamer->block_count) {
    return;
  }
  IRFunction *function = renamer->function;
  const IRSsaCandidates *candidates = renamer->candidates;
  const IRBasicBlock *block = &renamer->blocks[block_index];

  size_t *saved = (size_t *)calloc(candidates->value_count, sizeof(size_t));
  if (!saved) {
    renamer->failed = 1;
    return;
  }
  for (size_t i = 0; i < candidates->value_count; i++) {
    saved[i] = renamer->stacks[i].count;
  }

  const size_t start = block->first_instruction;
  for (size_t k = 0; k < block->instruction_count; k++) {
    const size_t index = start + k;
    IRInstruction *instruction = &function->instructions[index];
    const int writes = ir_instruction_writes_destination(instruction);

    if (instruction->op == IR_OP_DECLARE_LOCAL) {
      if (instruction->dest.value_id != IR_VALUE_ID_NONE &&
          instruction->dest.value_id < candidates->value_count &&
          candidates->promotable[instruction->dest.value_id] &&
          !ir_ssa_stack_push(&renamer->stacks[instruction->dest.value_id],
                             instruction->dest.value_id)) {
        renamer->failed = 1;
      }
      continue;
    }

    if (instruction->op != IR_OP_PHI) {
      const size_t operands = ir_ssa_operand_count(instruction);
      for (size_t j = writes ? 1 : 0; j < operands; j++) {
        IROperand *operand = ir_ssa_operand_at(instruction, j);
        if (!operand || operand->kind != IR_OPERAND_SYMBOL ||
            operand->value_id == IR_VALUE_ID_NONE ||
            operand->value_id >= candidates->value_count ||
            !candidates->promotable[operand->value_id]) {
          continue;
        }
        const uint32_t reaching = ir_ssa_reaching(renamer, operand->value_id);
        if (reaching != operand->value_id) {
          ir_ssa_set_operand(function, operand, reaching);
        }
      }
    }

    if (!writes || instruction->dest.value_id == IR_VALUE_ID_NONE ||
        instruction->dest.value_id >= candidates->value_count ||
        !candidates->promotable[instruction->dest.value_id]) {
      continue;
    }

    const uint32_t base = (instruction->op == IR_OP_PHI)
                              ? renamer->phi_origin[index]
                              : instruction->dest.value_id;
    if (base == IR_VALUE_ID_NONE || base >= candidates->value_count) {
      continue;
    }
    const uint32_t version =
        ir_ssa_fresh_version(function, base, &renamer->counter);
    if (version == IR_VALUE_ID_NONE) {
      renamer->failed = 1;
      break;
    }
    ir_ssa_set_operand(function, &instruction->dest, version);
    if (!ir_ssa_record_version(renamer, version, base) ||
        !ir_ssa_stack_push(&renamer->stacks[base], version)) {
      renamer->failed = 1;
      break;
    }
  }

  for (size_t s = 0; s < block->successor_count && !renamer->failed; s++) {
    const size_t successor = block->successors[s];
    if (successor >= renamer->block_count) {
      continue;
    }
    const IRBasicBlock *target = &renamer->blocks[successor];
    size_t slot = SIZE_MAX;
    for (size_t p = 0; p < target->predecessor_count; p++) {
      if (target->predecessors[p] == block_index) {
        slot = p;
        break;
      }
    }
    if (slot == SIZE_MAX) {
      continue;
    }
    const size_t target_start = target->first_instruction;
    for (size_t k = 0; k < target->instruction_count; k++) {
      const size_t index = target_start + k;
      IRInstruction *phi = &function->instructions[index];
      if (phi->op != IR_OP_PHI) {
        continue;
      }
      const uint32_t base = renamer->phi_origin[index];
      if (base == IR_VALUE_ID_NONE || base >= candidates->value_count) {
        continue;
      }
      if (slot * 2 >= phi->argument_count) {
        continue;
      }
      const uint32_t reaching = ir_ssa_reaching(renamer, base);
      ir_ssa_set_operand(function, &phi->arguments[slot * 2], reaching);
    }
  }

  for (size_t child = renamer->dom->child_head[block_index];
       child != IR_BLOCK_NONE && !renamer->failed;
       child = renamer->dom->child_next[child]) {
    ir_ssa_rename_block(renamer, child);
  }

  for (size_t i = 0; i < candidates->value_count; i++) {
    renamer->stacks[i].count = saved[i];
  }
  free(saved);
}

static int ir_ssa_ensure_block_labels(IRFunction *function, int *changed) {
  size_t block_count = 0;
  const IRBasicBlock *blocks = ir_function_blocks(function, &block_count);
  if (!blocks || block_count == 0) {
    return 1;
  }

  size_t missing = 0;
  for (size_t b = 0; b < block_count; b++) {
    if (blocks[b].predecessor_count >= 1 && !blocks[b].label) {
      missing++;
    }
  }
  if (missing == 0) {
    return 1;
  }

  IRInstruction *grown = (IRInstruction *)malloc(
      (function->instruction_count + missing) * sizeof(IRInstruction));
  if (!grown) {
    return 0;
  }

  size_t write = 0;
  unsigned counter = 0;
  for (size_t b = 0; b < block_count; b++) {
    if (blocks[b].predecessor_count >= 1 && !blocks[b].label) {
      char name[128];
      snprintf(name, sizeof(name), IR_SSA_BLOCK_LABEL_PREFIX "%u", counter++);
      IRInstruction label;
      memset(&label, 0, sizeof(label));
      label.op = IR_OP_LABEL;
      label.text = mettle_strdup(name);
      if (blocks[b].instruction_count > 0) {
        label.location =
            function->instructions[blocks[b].first_instruction].location;
      }
      grown[write++] = label;
    }
    for (size_t k = 0; k < blocks[b].instruction_count; k++) {
      grown[write++] = function->instructions[blocks[b].first_instruction + k];
    }
  }

  free(function->instructions);
  function->instructions = grown;
  function->instruction_count = write;
  function->instruction_capacity = write;
  ir_function_clear_cfg(function);
  ir_function_number_values(function);
  if (changed) {
    *changed = 1;
  }
  return 1;
}

static void ir_ssa_audit_declarations(IRFunction *function, const char *stage) {
  if (!getenv("METTLE_IR_SSA_DEBUG")) {
    return;
  }
  IRValueTable declared;
  ir_value_table_init(&declared);
  for (size_t p = 0; p < function->parameter_count; p++) {
    if (function->parameter_names && function->parameter_names[p]) {
      ir_value_table_intern(&declared, (unsigned char)IR_OPERAND_SYMBOL,
                            function->parameter_names[p]);
    }
  }
  for (size_t i = 0; i < function->instruction_count; i++) {
    const IRInstruction *in = &function->instructions[i];
    if (in->op == IR_OP_DECLARE_LOCAL && in->dest.kind == IR_OPERAND_SYMBOL &&
        in->dest.name) {
      ir_value_table_intern(&declared, (unsigned char)IR_OPERAND_SYMBOL,
                            in->dest.name);
    }
  }
  for (size_t i = 0; i < function->instruction_count; i++) {
    IRInstruction *in = &function->instructions[i];
    const size_t operands = ir_ssa_operand_count(in);
    for (size_t j = 0; j < operands; j++) {
      IROperand *operand = ir_ssa_operand_at(in, j);
      if (!operand || operand->kind != IR_OPERAND_SYMBOL || !operand->name) {
        continue;
      }
      if (!strstr(operand->name, IR_SSA_VERSION_PREFIX)) {
        continue;
      }
      if (ir_value_table_lookup(&declared, (unsigned char)IR_OPERAND_SYMBOL,
                                operand->name) == IR_VALUE_ID_NONE) {
        fprintf(stderr,
                "ssa %s in %s: instruction %zu operand %zu names '%s' with no "
                "declaration\n",
                stage, function->name ? function->name : "?", i, j,
                operand->name);
      }
    }
  }
  ir_value_table_clear(&declared);
}

int ir_promote_scalar_locals_pass(IRFunction *function, int *changed) {
  if (!function || function->instruction_count == 0 || !ir_ssa_enabled()) {
    return 1;
  }

  ir_function_number_values(function);

  IRSsaCandidates candidates;
  if (!ir_ssa_candidates_build(function, &candidates)) {
    return 1;
  }

  size_t promotable_count = 0;
  for (size_t i = 0; i < candidates.value_count; i++) {
    if (candidates.promotable[i]) {
      promotable_count++;
    }
  }
  if (promotable_count == 0) {
    ir_ssa_candidates_destroy(&candidates);
    return 1;
  }
  ir_ssa_candidates_destroy(&candidates);

  if (!ir_ssa_ensure_block_labels(function, changed)) {
    return 1;
  }
  if (!ir_ssa_candidates_build(function, &candidates)) {
    return 1;
  }

  const IRAnalysis *analysis = ir_function_analysis(function);
  size_t block_count = 0;
  const IRBasicBlock *blocks = ir_function_blocks(function, &block_count);
  if (!analysis || !analysis->dom.built || !analysis->instruction_block ||
      !blocks || block_count == 0) {
    ir_ssa_candidates_destroy(&candidates);
    return 1;
  }

  unsigned char *has_phi =
      (unsigned char *)calloc(block_count * candidates.value_count, 1);
  size_t *worklist = (size_t *)malloc(block_count * sizeof(size_t));
  unsigned char *on_list = (unsigned char *)calloc(block_count, 1);
  if (!has_phi || !worklist || !on_list) {
    free(has_phi);
    free(worklist);
    free(on_list);
    ir_ssa_candidates_destroy(&candidates);
    return 1;
  }

  size_t phi_total = 0;
  for (uint32_t id = 1; id < (uint32_t)candidates.value_count; id++) {
    if (!candidates.promotable[id]) {
      continue;
    }
    memset(on_list, 0, block_count);
    size_t head = 0;
    size_t tail = 0;
    for (size_t i = 0; i < function->instruction_count; i++) {
      const IRInstruction *instruction = &function->instructions[i];
      if (!ir_instruction_writes_destination(instruction) ||
          instruction->dest.value_id != id) {
        continue;
      }
      const size_t block = analysis->instruction_block[i];
      if (block == IR_BLOCK_NONE || on_list[block]) {
        continue;
      }
      on_list[block] = 1;
      worklist[tail++] = block;
    }

    while (head < tail) {
      const size_t block = worklist[head++];
      size_t frontier_count = 0;
      const size_t *frontier =
          ir_function_dominance_frontier(function, block, &frontier_count);
      for (size_t k = 0; k < frontier_count; k++) {
        const size_t target = frontier[k];
        if (target >= block_count ||
            blocks[target].predecessor_count < 2 ||
            has_phi[target * candidates.value_count + id]) {
          continue;
        }
        has_phi[target * candidates.value_count + id] = 1;
        phi_total++;
        if (!on_list[target] && tail < block_count) {
          on_list[target] = 1;
          worklist[tail++] = target;
        }
      }
    }
  }

  free(worklist);
  free(on_list);

  for (uint32_t id = 1; id < (uint32_t)candidates.value_count; id++) {
    if (!candidates.promotable[id]) {
      continue;
    }
    int critical = 0;
    for (size_t b = 0; b < block_count && !critical; b++) {
      if (!has_phi[b * candidates.value_count + id]) {
        continue;
      }
      for (size_t p = 0; p < blocks[b].predecessor_count; p++) {
        const size_t pred = blocks[b].predecessors[p];
        if (pred < block_count && blocks[pred].successor_count > 1) {
          critical = 1;
          break;
        }
      }
    }
    if (!critical) {
      continue;
    }
    candidates.promotable[id] = 0;
    for (size_t b = 0; b < block_count; b++) {
      if (has_phi[b * candidates.value_count + id]) {
        has_phi[b * candidates.value_count + id] = 0;
        phi_total--;
      }
    }
  }

  IRInstruction *grown = (IRInstruction *)malloc(
      (function->instruction_count + phi_total + 1) * sizeof(IRInstruction));
  uint32_t *phi_origin = (uint32_t *)malloc(
      (function->instruction_count + phi_total + 1) * sizeof(uint32_t));
  if (!grown || !phi_origin) {
    free(grown);
    free(phi_origin);
    free(has_phi);
    ir_ssa_candidates_destroy(&candidates);
    return 1;
  }

  size_t write = 0;
  for (size_t b = 0; b < block_count; b++) {
    const size_t start = blocks[b].first_instruction;
    const size_t count = blocks[b].instruction_count;
    size_t emitted = 0;
    if (count > 0 && function->instructions[start].op == IR_OP_LABEL) {
      phi_origin[write] = IR_VALUE_ID_NONE;
      grown[write++] = function->instructions[start];
      emitted = 1;
    }
    for (uint32_t id = 1; id < (uint32_t)candidates.value_count; id++) {
      if (!has_phi[b * candidates.value_count + id]) {
        continue;
      }
      const IRSsaDeclInfo *info = &candidates.declarations[id];
      const char *base_name = ir_value_table_name(&function->values, id);
      IRInstruction phi;
      memset(&phi, 0, sizeof(phi));
      phi.op = IR_OP_PHI;
      phi.location = info->location;
      phi.dest = ir_operand_symbol(base_name);
      phi.dest.value_id = id;
      phi.value_type = info->value_type;
      phi.is_float = info->is_float;
      phi.float_bits = info->float_bits;
      phi.is_unsigned = info->is_unsigned;
      phi.alias_class = info->alias_class;
      phi.argument_count = blocks[b].predecessor_count * 2;
      phi.arguments =
          (IROperand *)calloc(phi.argument_count, sizeof(IROperand));
      if (!phi.arguments) {
        phi.argument_count = 0;
      } else {
        for (size_t p = 0; p < blocks[b].predecessor_count; p++) {
          const size_t pred = blocks[b].predecessors[p];
          phi.arguments[p * 2] = ir_operand_symbol(base_name);
          phi.arguments[p * 2].value_id = id;
          const char *label = (pred < block_count) ? blocks[pred].label : NULL;
          phi.arguments[p * 2 + 1] =
              label ? ir_operand_label(label) : ir_operand_none();
        }
      }
      phi_origin[write] = id;
      grown[write++] = phi;
    }
    for (size_t k = emitted; k < count; k++) {
      phi_origin[write] = IR_VALUE_ID_NONE;
      grown[write++] = function->instructions[start + k];
    }
  }

  free(has_phi);
  if (getenv("METTLE_IR_SSA_DEBUG")) {
    size_t tiled = 0;
    for (size_t b = 0; b < block_count; b++) {
      tiled += blocks[b].instruction_count;
    }
    fprintf(stderr,
            "ssa %s: before %zu blocks %zu tiled %zu phis %zu wrote %zu\n",
            function->name ? function->name : "?", function->instruction_count,
            block_count, tiled, phi_total, write);
  }
  free(function->instructions);
  function->instructions = grown;
  function->instruction_count = write;
  function->instruction_capacity = write;
  ir_function_clear_cfg(function);

  blocks = ir_function_blocks(function, &block_count);
  analysis = ir_function_analysis(function);
  if (!blocks || !analysis || !analysis->dom.built) {
    free(phi_origin);
    ir_ssa_candidates_destroy(&candidates);
    return 1;
  }

  IRSsaRenamer renamer;
  memset(&renamer, 0, sizeof(renamer));
  renamer.function = function;
  renamer.candidates = &candidates;
  renamer.blocks = blocks;
  renamer.block_count = block_count;
  renamer.dom = &analysis->dom;
  renamer.phi_origin = phi_origin;
  renamer.stacks =
      (IRSsaStack *)calloc(candidates.value_count, sizeof(IRSsaStack));
  if (!renamer.stacks) {
    free(phi_origin);
    ir_ssa_candidates_destroy(&candidates);
    return 1;
  }

  ir_ssa_rename_block(&renamer, function->entry_block);

  for (size_t i = 0; i < candidates.value_count; i++) {
    free(renamer.stacks[i].entries);
  }
  free(renamer.stacks);
  free(phi_origin);

  if (renamer.created_count > 0) {
    IRInstruction *declared = (IRInstruction *)malloc(
        (function->instruction_count + renamer.created_count) *
        sizeof(IRInstruction));
    if (declared) {
      size_t out = 0;
      size_t head = 0;
      if (function->instruction_count > 0 &&
          function->instructions[0].op == IR_OP_LABEL) {
        declared[out++] = function->instructions[0];
        head = 1;
      }
      for (size_t k = 0; k < renamer.created_count; k++) {
        const uint32_t base = renamer.created_bases[k];
        const IRSsaDeclInfo *origin = &candidates.declarations[base];
        const char *name =
            ir_value_table_name(&function->values, renamer.created_versions[k]);
        IRInstruction declaration;
        memset(&declaration, 0, sizeof(declaration));
        declaration.op = IR_OP_DECLARE_LOCAL;
        declaration.location = origin->location;
        declaration.dest = ir_operand_symbol(name);
        declaration.dest.value_id = renamer.created_versions[k];
        declaration.text = mettle_strdup(origin->type_name);
        declaration.value_type = origin->value_type;
        declaration.is_float = origin->is_float;
        declaration.float_bits = origin->float_bits;
        declaration.is_unsigned = origin->is_unsigned;
        declaration.alias_class = origin->alias_class;
        declared[out++] = declaration;
      }
      for (size_t k = head; k < function->instruction_count; k++) {
        declared[out++] = function->instructions[k];
      }
      free(function->instructions);
      function->instructions = declared;
      function->instruction_count = out;
      function->instruction_capacity = out;
    }
  }

  free(renamer.created_versions);
  free(renamer.created_bases);
  ir_ssa_candidates_destroy(&candidates);

  ir_function_clear_cfg(function);
  ir_function_number_values(function);
  ir_ssa_audit_declarations(function, "promote");
  if (changed) {
    *changed = 1;
  }
  return 1;
}

int ir_leave_ssa_pass(IRFunction *function, int *changed) {
  if (!function || function->instruction_count == 0) {
    return 1;
  }

  size_t phi_count = 0;
  for (size_t i = 0; i < function->instruction_count; i++) {
    if (function->instructions[i].op == IR_OP_PHI) {
      phi_count++;
    }
  }
  if (phi_count == 0) {
    return 1;
  }

  ir_function_number_values(function);
  size_t block_count = 0;
  const IRBasicBlock *blocks = ir_function_blocks(function, &block_count);
  if (!blocks || block_count == 0) {
    return 1;
  }

  size_t copy_budget = 0;
  for (size_t i = 0; i < function->instruction_count; i++) {
    const IRInstruction *phi = &function->instructions[i];
    if (phi->op == IR_OP_PHI) {
      copy_budget += phi->argument_count;
    }
  }

  IRInstruction *grown = (IRInstruction *)malloc(
      (function->instruction_count + copy_budget * 2 + 8) *
      sizeof(IRInstruction));
  const IRInstruction **sources =
      (const IRInstruction **)malloc((copy_budget + 8) * sizeof(void *));
  unsigned char *done = (unsigned char *)malloc(copy_budget + 8);
  if (!grown || !sources || !done) {
    free(grown);
    free(sources);
    free(done);
    return 1;
  }

  IRValueTable targeted;
  ir_value_table_init(&targeted);
  for (size_t i = 0; i < function->instruction_count; i++) {
    const IRInstruction *in = &function->instructions[i];
    if (!in->text) {
      continue;
    }
    if (in->op == IR_OP_JUMP || in->op == IR_OP_BRANCH_ZERO ||
        in->op == IR_OP_BRANCH_EQ) {
      ir_value_table_intern(&targeted, (unsigned char)IR_OPERAND_LABEL,
                            in->text);
    }
  }

  size_t write = 0;
  unsigned swap_counter = 0;
  size_t swap_count = 0;
  char **swap_names = NULL;
  IRSsaDeclInfo *swap_info = NULL;

  for (size_t b = 0; b < block_count; b++) {
    const size_t start = blocks[b].first_instruction;
    const size_t count = blocks[b].instruction_count;

    size_t terminator = count;
    if (count > 0) {
      const IROpcode last = function->instructions[start + count - 1].op;
      if (last == IR_OP_JUMP || last == IR_OP_RETURN ||
          last == IR_OP_BRANCH_ZERO || last == IR_OP_BRANCH_EQ) {
        terminator = count - 1;
      }
    }

    for (size_t k = 0; k < terminator; k++) {
      IRInstruction *source = &function->instructions[start + k];
      if (source->op == IR_OP_PHI) {
        continue;
      }
      if (source->op == IR_OP_LABEL && source->text &&
          strncmp(source->text, IR_SSA_BLOCK_LABEL_PREFIX,
                  strlen(IR_SSA_BLOCK_LABEL_PREFIX)) == 0 &&
          ir_value_table_lookup(&targeted, (unsigned char)IR_OPERAND_LABEL,
                                source->text) == IR_VALUE_ID_NONE) {
        ir_instruction_destroy(source);
        continue;
      }
      grown[write++] = *source;
    }

    for (size_t sidx = 0; sidx < blocks[b].successor_count; sidx++) {
      const size_t successor = blocks[b].successors[sidx];
      if (successor >= block_count) {
        continue;
      }
      const IRBasicBlock *target = &blocks[successor];
      size_t slot = SIZE_MAX;
      for (size_t p = 0; p < target->predecessor_count; p++) {
        if (target->predecessors[p] == b) {
          slot = p;
          break;
        }
      }
      if (slot == SIZE_MAX) {
        continue;
      }

      size_t pair_count = 0;
      for (size_t k = 0; k < target->instruction_count; k++) {
        const IRInstruction *phi =
            &function->instructions[target->first_instruction + k];
        if (phi->op != IR_OP_PHI || slot * 2 >= phi->argument_count) {
          continue;
        }
        const IROperand *incoming = &phi->arguments[slot * 2];
        if (incoming->kind == IR_OPERAND_NONE ||
            ir_operand_same(incoming, &phi->dest)) {
          continue;
        }
        sources[pair_count] = phi;
        done[pair_count] = 0;
        pair_count++;
      }
      if (pair_count == 0) {
        continue;
      }

      size_t emitted = 0;
      while (emitted < pair_count) {
        int progressed = 0;
        for (size_t i = 0; i < pair_count; i++) {
          if (done[i]) {
            continue;
          }
          const IRInstruction *phi = sources[i];
          int blocked = 0;
          for (size_t j = 0; j < pair_count && !blocked; j++) {
            if (done[j] || j == i) {
              continue;
            }
            if (ir_operand_same(&sources[j]->arguments[slot * 2], &phi->dest)) {
              blocked = 1;
            }
          }
          if (blocked) {
            continue;
          }
          IRInstruction copy;
          memset(&copy, 0, sizeof(copy));
          copy.op = IR_OP_ASSIGN;
          copy.location = phi->location;
          copy.dest = ir_operand_copy(&phi->dest);
          copy.lhs = ir_operand_copy(&phi->arguments[slot * 2]);
          copy.value_type = phi->value_type;
          copy.is_float = phi->is_float;
          copy.float_bits = phi->float_bits;
          copy.is_unsigned = phi->is_unsigned;
          copy.alias_class = phi->alias_class;
          grown[write++] = copy;
          done[i] = 1;
          emitted++;
          progressed = 1;
        }
        if (progressed) {
          continue;
        }

        size_t pick = pair_count;
        for (size_t i = 0; i < pair_count; i++) {
          if (!done[i]) {
            pick = i;
            break;
          }
        }
        if (pick == pair_count) {
          break;
        }
        const IRInstruction *phi = sources[pick];
        char name[256];
        snprintf(name, sizeof(name), "__ssa_swap%u", swap_counter++);
        char **grown_names =
            (char **)realloc(swap_names, (swap_count + 1) * sizeof(char *));
        if (!grown_names) {
          break;
        }
        swap_names = grown_names;
        IRSsaDeclInfo *grown_info = (IRSsaDeclInfo *)realloc(
            swap_info, (swap_count + 1) * sizeof(IRSsaDeclInfo));
        if (!grown_info) {
          break;
        }
        swap_info = grown_info;
        swap_names[swap_count] = mettle_strdup(name);
        memset(&swap_info[swap_count], 0, sizeof(IRSsaDeclInfo));
        swap_info[swap_count].value_type = phi->value_type;
        swap_info[swap_count].location = phi->location;
        swap_info[swap_count].is_float = phi->is_float;
        swap_info[swap_count].float_bits = phi->float_bits;
        swap_info[swap_count].is_unsigned = phi->is_unsigned;
        swap_info[swap_count].alias_class = phi->alias_class;
        swap_count++;

        IRInstruction save;
        memset(&save, 0, sizeof(save));
        save.op = IR_OP_ASSIGN;
        save.location = phi->location;
        save.dest = ir_operand_symbol(name);
        save.lhs = ir_operand_copy(&phi->dest);
        save.value_type = phi->value_type;
        save.is_float = phi->is_float;
        save.float_bits = phi->float_bits;
        save.is_unsigned = phi->is_unsigned;
        save.alias_class = phi->alias_class;
        grown[write++] = save;

        for (size_t j = 0; j < pair_count; j++) {
          if (done[j]) {
            continue;
          }
          if (!ir_operand_same(&sources[j]->arguments[slot * 2], &phi->dest)) {
            continue;
          }
          IRInstruction copy;
          memset(&copy, 0, sizeof(copy));
          copy.op = IR_OP_ASSIGN;
          copy.location = sources[j]->location;
          copy.dest = ir_operand_copy(&sources[j]->dest);
          copy.lhs = ir_operand_symbol(name);
          copy.value_type = sources[j]->value_type;
          copy.is_float = sources[j]->is_float;
          copy.float_bits = sources[j]->float_bits;
          copy.is_unsigned = sources[j]->is_unsigned;
          copy.alias_class = sources[j]->alias_class;
          grown[write++] = copy;
          done[j] = 1;
          emitted++;
        }
      }
    }

    for (size_t k = terminator; k < count; k++) {
      grown[write++] = function->instructions[start + k];
    }
  }

  for (size_t i = 0; i < function->instruction_count; i++) {
    if (function->instructions[i].op == IR_OP_PHI) {
      ir_instruction_destroy(&function->instructions[i]);
    }
  }

  free(function->instructions);
  function->instructions = grown;
  function->instruction_count = write;
  function->instruction_capacity = write;

  if (swap_count > 0) {
    IRInstruction *declared = (IRInstruction *)malloc(
        (function->instruction_count + swap_count) * sizeof(IRInstruction));
    if (declared) {
      size_t out = 0;
      size_t head = 0;
      if (function->instruction_count > 0 &&
          function->instructions[0].op == IR_OP_LABEL) {
        declared[out++] = function->instructions[0];
        head = 1;
      }
      for (size_t k = 0; k < swap_count; k++) {
        IRInstruction declaration;
        memset(&declaration, 0, sizeof(declaration));
        declaration.op = IR_OP_DECLARE_LOCAL;
        declaration.location = swap_info[k].location;
        declaration.dest = ir_operand_symbol(swap_names[k]);
        declaration.text = mettle_strdup(
            (swap_info[k].value_type && swap_info[k].value_type->name)
                ? swap_info[k].value_type->name
                : "int64");
        declaration.value_type = swap_info[k].value_type;
        declaration.is_float = swap_info[k].is_float;
        declaration.float_bits = swap_info[k].float_bits;
        declaration.is_unsigned = swap_info[k].is_unsigned;
        declaration.alias_class = swap_info[k].alias_class;
        declared[out++] = declaration;
      }
      for (size_t k = head; k < function->instruction_count; k++) {
        declared[out++] = function->instructions[k];
      }
      free(function->instructions);
      function->instructions = declared;
      function->instruction_count = out;
      function->instruction_capacity = out;
    }
  }

  for (size_t k = 0; k < swap_count; k++) {
    free(swap_names[k]);
  }
  free(swap_names);
  free(swap_info);
  free(sources);
  free(done);
  ir_value_table_clear(&targeted);

  ir_function_clear_cfg(function);
  ir_function_number_values(function);
  ir_ssa_audit_declarations(function, "leave");
  if (changed) {
    *changed = 1;
  }
  return 1;
}

