#include "../common.h"
#include "ir.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define IR_SSA_OPT_MAX_ROUNDS 16

static unsigned long long g_ssa_trivial_phis = 0;
static unsigned long long g_ssa_dead_phis = 0;
static unsigned long long g_ssa_uses_rewritten = 0;

static int ir_ssa_opt_stats_enabled(void) {
  static int cached = -1;
  if (cached < 0) {
    cached = getenv("METTLE_SSA_STATS") ? 1 : 0;
  }
  return cached;
}

void ir_ssa_opt_report_stats(void) {
  if (!ir_ssa_opt_stats_enabled()) {
    return;
  }
  fprintf(stderr, "SSA-OPT\ttrivial_phis\t%llu\n", g_ssa_trivial_phis);
  fprintf(stderr, "SSA-OPT\tdead_phis\t%llu\n", g_ssa_dead_phis);
  fprintf(stderr, "SSA-OPT\tuses_rewritten\t%llu\n", g_ssa_uses_rewritten);
}

static int ir_ssa_opt_function_is_eligible(const IRFunction *function,
                                           size_t *phi_count_out) {
  size_t phis = 0;
  for (size_t i = 0; i < function->instruction_count; i++) {
    const IROpcode op = function->instructions[i].op;
    if (op == IR_OP_INLINE_ASM) {
      return 0;
    }
    if (op == IR_OP_PHI) {
      phis++;
    }
  }
  if (phi_count_out) {
    *phi_count_out = phis;
  }
  return phis > 0;
}

static int ir_ssa_opt_operand_is_versioned(const IRFunction *function,
                                           const IROperand *operand) {
  if (!operand || !ir_operand_is_value(operand) ||
      operand->value_id == IR_VALUE_ID_NONE) {
    return 0;
  }
  const char *name = ir_value_table_name(&function->values, operand->value_id);
  return name && strstr(name, "__ssa") != NULL;
}

static void ir_ssa_opt_write_operand(IRFunction *function, IROperand *operand,
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

static uint32_t ir_ssa_opt_resolve(const uint32_t *replacement, uint32_t id,
                                   size_t count) {
  uint32_t current = id;
  for (size_t step = 0; step < count && current < count; step++) {
    const uint32_t next = replacement[current];
    if (next == IR_VALUE_ID_NONE || next == current) {
      return current;
    }
    current = next;
  }
  return current;
}

static size_t ir_ssa_opt_eliminate_trivial(IRFunction *function,
                                           size_t value_count) {
  uint32_t *replacement =
      (uint32_t *)calloc(value_count, sizeof(uint32_t));
  unsigned char *drop =
      (unsigned char *)calloc(function->instruction_count, sizeof(unsigned char));
  if (!replacement || !drop) {
    free(replacement);
    free(drop);
    return 0;
  }

  size_t eliminated = 0;
  for (size_t i = 0; i < function->instruction_count; i++) {
    IRInstruction *phi = &function->instructions[i];
    if (phi->op != IR_OP_PHI || !phi->arguments) {
      continue;
    }
    const uint32_t self = phi->dest.value_id;
    if (self == IR_VALUE_ID_NONE || self >= value_count) {
      continue;
    }
    uint32_t only = IR_VALUE_ID_NONE;
    int usable = 1;
    for (size_t p = 0; p * 2 < phi->argument_count; p++) {
      const IROperand *incoming = &phi->arguments[p * 2];
      if (incoming->kind == IR_OPERAND_NONE) {
        continue;
      }
      if (!ir_operand_is_value(incoming) ||
          incoming->value_id == IR_VALUE_ID_NONE) {
        usable = 0;
        break;
      }
      if (incoming->value_id == self) {
        continue;
      }
      if (only == IR_VALUE_ID_NONE) {
        only = incoming->value_id;
      } else if (only != incoming->value_id) {
        usable = 0;
        break;
      }
    }
    if (!usable || only == IR_VALUE_ID_NONE || only >= value_count) {
      continue;
    }
    if (!ir_ssa_opt_operand_is_versioned(function, &phi->dest)) {
      continue;
    }
    replacement[self] = only;
    drop[i] = 1;
    eliminated++;
  }

  if (eliminated == 0) {
    free(replacement);
    free(drop);
    return 0;
  }

  for (size_t i = 0; i < function->instruction_count; i++) {
    IRInstruction *phi = &function->instructions[i];
    uint32_t self;
    uint32_t walk;
    size_t step;
    if (!drop[i]) {
      continue;
    }
    self = phi->dest.value_id;
    walk = replacement[self];
    for (step = 0; step < value_count && walk != IR_VALUE_ID_NONE &&
                   walk < value_count;
         step++) {
      if (walk == self) {
        replacement[self] = IR_VALUE_ID_NONE;
        drop[i] = 0;
        eliminated--;
        break;
      }
      if (replacement[walk] == IR_VALUE_ID_NONE) {
        break;
      }
      walk = replacement[walk];
    }
  }

  if (eliminated == 0) {
    free(replacement);
    free(drop);
    return 0;
  }

  for (size_t i = 0; i < function->instruction_count; i++) {
    IRInstruction *instruction = &function->instructions[i];
    if (drop[i]) {
      continue;
    }
    const int writes = ir_instruction_writes_destination(instruction);
    const size_t operands = 3 + instruction->argument_count;
    for (size_t j = writes ? 1 : 0; j < operands; j++) {
      IROperand *operand = NULL;
      if (j == 0) {
        operand = &instruction->dest;
      } else if (j == 1) {
        operand = &instruction->lhs;
      } else if (j == 2) {
        operand = &instruction->rhs;
      } else if (instruction->arguments &&
                 j - 3 < instruction->argument_count) {
        operand = &instruction->arguments[j - 3];
      }
      if (!operand || !ir_operand_is_value(operand) ||
          operand->value_id == IR_VALUE_ID_NONE ||
          operand->value_id >= value_count) {
        continue;
      }
      if (replacement[operand->value_id] == IR_VALUE_ID_NONE) {
        continue;
      }
      const uint32_t target =
          ir_ssa_opt_resolve(replacement, operand->value_id, value_count);
      if (target == operand->value_id || target == IR_VALUE_ID_NONE) {
        continue;
      }
      ir_ssa_opt_write_operand(function, operand, target);
      g_ssa_uses_rewritten++;
    }
  }

  size_t write = 0;
  for (size_t i = 0; i < function->instruction_count; i++) {
    if (drop[i]) {
      ir_instruction_destroy(&function->instructions[i]);
      continue;
    }
    if (write != i) {
      function->instructions[write] = function->instructions[i];
    }
    write++;
  }
  function->instruction_count = write;

  free(replacement);
  free(drop);
  g_ssa_trivial_phis += eliminated;
  return eliminated;
}

static size_t ir_ssa_opt_remove_dead_phis(IRFunction *function,
                                          size_t value_count) {
  size_t *uses = (size_t *)calloc(value_count, sizeof(size_t));
  unsigned char *drop =
      (unsigned char *)calloc(function->instruction_count, sizeof(unsigned char));
  if (!uses || !drop) {
    free(uses);
    free(drop);
    return 0;
  }

  for (size_t i = 0; i < function->instruction_count; i++) {
    IRInstruction *instruction = &function->instructions[i];
    const int writes = ir_instruction_writes_destination(instruction);
    const size_t operands = 3 + instruction->argument_count;
    for (size_t j = writes ? 1 : 0; j < operands; j++) {
      const IROperand *operand = NULL;
      if (j == 0) {
        operand = &instruction->dest;
      } else if (j == 1) {
        operand = &instruction->lhs;
      } else if (j == 2) {
        operand = &instruction->rhs;
      } else if (instruction->arguments &&
                 j - 3 < instruction->argument_count) {
        operand = &instruction->arguments[j - 3];
      }
      if (!operand || !ir_operand_is_value(operand) ||
          operand->value_id == IR_VALUE_ID_NONE ||
          operand->value_id >= value_count) {
        continue;
      }
      uses[operand->value_id]++;
    }
  }

  size_t dead = 0;
  for (size_t i = 0; i < function->instruction_count; i++) {
    IRInstruction *phi = &function->instructions[i];
    if (phi->op != IR_OP_PHI) {
      continue;
    }
    const uint32_t self = phi->dest.value_id;
    if (self == IR_VALUE_ID_NONE || self >= value_count) {
      continue;
    }
    if (uses[self] != 0) {
      continue;
    }
    if (!ir_ssa_opt_operand_is_versioned(function, &phi->dest)) {
      continue;
    }
    drop[i] = 1;
    dead++;
  }

  if (dead == 0) {
    free(uses);
    free(drop);
    return 0;
  }

  size_t write = 0;
  for (size_t i = 0; i < function->instruction_count; i++) {
    if (drop[i]) {
      ir_instruction_destroy(&function->instructions[i]);
      continue;
    }
    if (write != i) {
      function->instructions[write] = function->instructions[i];
    }
    write++;
  }
  function->instruction_count = write;

  free(uses);
  free(drop);
  g_ssa_dead_phis += dead;
  return dead;
}

int ir_ssa_propagate_pass(IRFunction *function, int *changed) {
  if (changed) {
    *changed = 0;
  }
  if (!function || function->instruction_count == 0) {
    return 1;
  }
  size_t phi_count = 0;
  if (!ir_ssa_opt_function_is_eligible(function, &phi_count)) {
    return 1;
  }

  int touched = 0;
  for (int round = 0; round < IR_SSA_OPT_MAX_ROUNDS; round++) {
    if (!ir_function_number_values(function)) {
      return 1;
    }
    const size_t value_count = ir_value_table_count(&function->values) + 1;
    size_t work = ir_ssa_opt_eliminate_trivial(function, value_count);
    work += ir_ssa_opt_remove_dead_phis(function, value_count);
    if (work == 0) {
      break;
    }
    touched = 1;
  }

  if (touched) {
    ir_function_clear_cfg(function);
    ir_function_number_values(function);
    if (changed) {
      *changed = 1;
    }
  }
  return 1;
}
