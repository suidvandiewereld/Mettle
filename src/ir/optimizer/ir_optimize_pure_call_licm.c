#include "ir_optimize_internal.h"
#include "../../common.h"
#include "../ir_purity.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_pure_licm_counter;

static int pure_licm_is_simd_marker(const IRInstruction *inst) {
  return inst && inst->op == IR_OP_NOP && inst->text &&
         strncmp(inst->text, IR_SIMD_MARKER_PREFIX,
                 strlen(IR_SIMD_MARKER_PREFIX)) == 0;
}

static int pure_licm_is_runtime_trap_call(const IRInstruction *inst) {
  return inst && inst->op == IR_OP_CALL && inst->text &&
         (strcmp(inst->text, "mettle_crash_trap_ex") == 0 ||
          strcmp(inst->text, "meth_runtime_debug_trap") == 0);
}

static int pure_licm_name_written(const IRFunction *function, size_t lo,
                                  size_t hi, const char *name,
                                  IROperandKind kind) {
  for (size_t k = lo; k < hi; k++) {
    const IRInstruction *inst = &function->instructions[k];
    if (ir_instruction_writes_destination(inst) && inst->dest.name &&
        inst->dest.kind == kind && strcmp(inst->dest.name, name) == 0) {
      return 1;
    }
  }
  return 0;
}

static int pure_licm_operand_invariant(const IRFunction *function, size_t lo,
                                       size_t hi, const IROperand *op) {
  if (!op) {
    return 1;
  }
  switch (op->kind) {
  case IR_OPERAND_INT:
  case IR_OPERAND_FLOAT:
  case IR_OPERAND_STRING:
  case IR_OPERAND_NONE:
    return 1;
  case IR_OPERAND_TEMP:
  case IR_OPERAND_SYMBOL:
    return op->name &&
           !pure_licm_name_written(function, lo, hi, op->name, op->kind);
  default:
    return 0;
  }
}

static int pure_licm_writes_global(const IRFunction *function,
                                   const IRInstruction *inst) {
  if (!ir_instruction_writes_symbol(inst) || !inst->dest.name) {
    return 0;
  }
  if (ir_function_symbol_is_parameter(function, inst->dest.name)) {
    return 0;
  }
  return ir_function_local_declared_type(function, inst->dest.name) == NULL;
}

static int pure_licm_range_side_effect_free(IRProgram *program,
                                            const IRFunction *function,
                                            size_t lo, size_t hi) {
  for (size_t k = lo; k < hi; k++) {
    const IRInstruction *inst = &function->instructions[k];
    if (pure_licm_writes_global(function, inst)) {
      return 0;
    }
    switch (inst->op) {
    case IR_OP_NOP:
    case IR_OP_LABEL:
    case IR_OP_JUMP:
    case IR_OP_BRANCH_ZERO:
    case IR_OP_BRANCH_EQ:
    case IR_OP_DECLARE_LOCAL:
    case IR_OP_ASSIGN:
    case IR_OP_ADDRESS_OF:
    case IR_OP_LOAD:
    case IR_OP_BINARY:
    case IR_OP_UNARY:
    case IR_OP_ROTATE_ADD:
    case IR_OP_CAST:
    case IR_OP_RETURN:
      break;
    case IR_OP_CALL: {
      if (pure_licm_is_runtime_trap_call(inst)) {
        break;
      }
      IRFunction *callee =
          inst->text ? ir_program_find_function(program, inst->text) : NULL;
      if (!callee || !callee->is_readonly_inferred) {
        return 0;
      }
      break;
    }
    default:
      return 0;
    }
  }
  return 1;
}

static int pure_licm_callee_hoistable(IRProgram *program,
                                      const IRFunction *callee) {
  return callee && pure_licm_range_side_effect_free(
                       program, callee, 0, callee->instruction_count);
}

static size_t pure_licm_find_backedge(const IRFunction *function,
                                      size_t header_index,
                                      const char *loop_label) {
  return ir_pass_loop_latch(function, header_index, loop_label);
}

static int pure_licm_label_inside(const IRFunction *function, size_t lo,
                                  size_t hi, const char *name) {
  for (size_t k = lo; k <= hi && k < function->instruction_count; k++) {
    const IRInstruction *inst = &function->instructions[k];
    if (inst->op == IR_OP_LABEL && inst->text &&
        strcmp(inst->text, name) == 0) {
      return 1;
    }
  }
  return 0;
}

static size_t pure_licm_cond_prefix_end(const IRFunction *function,
                                        size_t header, size_t backedge) {
  size_t last_exit_branch = 0;
  size_t limit = header + 25;
  for (size_t k = header + 1; k < backedge; k++) {
    if (k > limit) {
      return 0;
    }
    const IRInstruction *inst = &function->instructions[k];
    switch (inst->op) {
    case IR_OP_NOP:
    case IR_OP_ASSIGN:
    case IR_OP_LOAD:
    case IR_OP_BINARY:
    case IR_OP_UNARY:
    case IR_OP_ROTATE_ADD:
    case IR_OP_CAST:
    case IR_OP_ADDRESS_OF:
      continue;
    case IR_OP_BRANCH_ZERO:
    case IR_OP_BRANCH_EQ:
      if (!inst->text) {
        return 0;
      }
      if (pure_licm_label_inside(function, header, backedge, inst->text)) {
        return 0;
      }
      last_exit_branch = k;
      continue;
    default:
      return last_exit_branch;
    }
  }
  return last_exit_branch;
}

#define PURE_LICM_MAX_GLOBALS 24

typedef struct {
  const char *reads[PURE_LICM_MAX_GLOBALS];
  size_t read_count;
  const char *writes[PURE_LICM_MAX_GLOBALS];
  size_t write_count;
  int ok;
} PureLicmGlobalEffects;

static int pure_licm_effects_add(const char **list, size_t *count,
                                 const char *name) {
  for (size_t i = 0; i < *count; i++) {
    if (strcmp(list[i], name) == 0) {
      return 1;
    }
  }
  if (*count >= PURE_LICM_MAX_GLOBALS) {
    return 0;
  }
  list[(*count)++] = name;
  return 1;
}

static int pure_licm_symbol_is_global(const IRFunction *function,
                                      const char *sym) {
  return sym && !ir_function_symbol_is_parameter(function, sym) &&
         ir_function_local_declared_type(function, sym) == NULL;
}

static void pure_licm_effects_note_read(const IRFunction *function,
                                        const IROperand *op,
                                        PureLicmGlobalEffects *fx) {
  if (op->kind != IR_OPERAND_SYMBOL || !op->name) {
    return;
  }
  if (pure_licm_symbol_is_global(function, op->name) &&
      !pure_licm_effects_add(fx->reads, &fx->read_count, op->name)) {
    fx->ok = 0;
  }
}

static void pure_licm_global_effects(const IRFunction *function,
                                     PureLicmGlobalEffects *fx) {
  memset(fx, 0, sizeof(*fx));
  fx->ok = 1;
  for (size_t k = 0; k < function->instruction_count && fx->ok; k++) {
    const IRInstruction *inst = &function->instructions[k];
    switch (inst->op) {
    case IR_OP_NOP:
    case IR_OP_LABEL:
    case IR_OP_JUMP:
    case IR_OP_DECLARE_LOCAL:
      break;
    case IR_OP_BRANCH_ZERO:
    case IR_OP_BRANCH_EQ:
    case IR_OP_ASSIGN:
    case IR_OP_BINARY:
    case IR_OP_UNARY:
    case IR_OP_ROTATE_ADD:
    case IR_OP_CAST:
    case IR_OP_RETURN:
      pure_licm_effects_note_read(function, &inst->lhs, fx);
      pure_licm_effects_note_read(function, &inst->rhs, fx);
      if (ir_instruction_writes_destination(inst) &&
          ir_operand_is_symbol(&inst->dest) &&
          pure_licm_symbol_is_global(function, inst->dest.name) &&
          !pure_licm_effects_add(fx->writes, &fx->write_count,
                                 inst->dest.name)) {
        fx->ok = 0;
      }
      break;
    case IR_OP_CALL:
      if (!pure_licm_is_runtime_trap_call(inst)) {
        fx->ok = 0;
      }
      break;
    default:
      fx->ok = 0;
      break;
    }
  }
}

static int pure_licm_collapse_idempotent_body(IRProgram *program,
                                              IRFunction *function) {
  for (size_t i = 0; i < function->instruction_count; i++) {
    const IRInstruction *header = &function->instructions[i];
    if (header->op != IR_OP_LABEL || !header->text ||
        !ir_label_is_while_header(header->text)) {
      continue;
    }
    size_t backedge = pure_licm_find_backedge(function, i, header->text);
    if (backedge == (size_t)-1 || backedge <= i + 1) {
      continue;
    }
    size_t cond_end = pure_licm_cond_prefix_end(function, i, backedge);
    if (cond_end == 0) {
      continue;
    }

    size_t group[64];
    size_t group_count = 0;
    const char *stored[PURE_LICM_MAX_GLOBALS];
    size_t stored_count = 0;
    const char *counter = NULL;
    const char *inc_temp = NULL;
    const char *inc_sym = NULL;
    size_t counter_at = 0;
    int shape_ok = 1;
    int call_seen = 0;

    for (size_t k = cond_end + 1; k < backedge && shape_ok; k++) {
      const IRInstruction *inst = &function->instructions[k];
      switch (inst->op) {
      case IR_OP_NOP:
        break;
      case IR_OP_ASSIGN:
        if (inc_temp && !counter && inst->dest.kind == IR_OPERAND_SYMBOL &&
            inst->dest.name && strcmp(inst->dest.name, inc_sym) == 0 &&
            ir_operand_is_temp(&inst->lhs) &&
            strcmp(inst->lhs.name, inc_temp) == 0) {
          counter = inc_sym;
          counter_at = k;
          break;
        }
        if (ir_operand_is_symbol(&inst->dest) &&
            pure_licm_symbol_is_global(function, inst->dest.name) &&
            inst->lhs.kind == IR_OPERAND_INT) {
          if (group_count >= 64 ||
              !pure_licm_effects_add(stored, &stored_count, inst->dest.name)) {
            shape_ok = 0;
          } else {
            group[group_count++] = k;
          }
          break;
        }
        shape_ok = 0;
        break;
      case IR_OP_BINARY:
        if (!counter && !inst->is_float && inst->text &&
            strcmp(inst->text, "+") == 0 &&
            ir_operand_is_symbol(&inst->dest) &&
            !pure_licm_symbol_is_global(function, inst->dest.name) &&
            ir_operand_is_symbol_named(&inst->lhs, inst->dest.name) &&
            inst->rhs.kind == IR_OPERAND_INT) {
          counter = inst->dest.name;
          counter_at = k;
          break;
        }
        if (!counter && !inc_temp && !inst->is_float && inst->text &&
            strcmp(inst->text, "+") == 0 &&
            ir_operand_is_temp(&inst->dest) &&
            ir_operand_is_symbol(&inst->lhs) &&
            !pure_licm_symbol_is_global(function, inst->lhs.name) &&
            inst->rhs.kind == IR_OPERAND_INT) {
          inc_temp = inst->dest.name;
          inc_sym = inst->lhs.name;
          break;
        }
        shape_ok = 0;
        break;
      case IR_OP_CALL: {
        if (pure_licm_is_runtime_trap_call(inst)) {
          shape_ok = 0;
          break;
        }
        IRFunction *callee =
            inst->text ? ir_program_find_function(program, inst->text) : NULL;
        if (!callee || group_count >= 64) {
          shape_ok = 0;
          break;
        }
        PureLicmGlobalEffects fx;
        pure_licm_global_effects(callee, &fx);
        if (!fx.ok) {
          shape_ok = 0;
          break;
        }
        for (size_t a = 0; a < inst->argument_count; a++) {
          if (inst->arguments[a].kind != IR_OPERAND_INT) {
            shape_ok = 0;
          }
        }
        for (size_t r = 0; r < fx.read_count && shape_ok; r++) {
          int pinned = 0;
          for (size_t s = 0; s < stored_count; s++) {
            if (strcmp(stored[s], fx.reads[r]) == 0) {
              pinned = 1;
              break;
            }
          }
          if (!pinned) {
            shape_ok = 0;
          }
        }
        if ((inst->dest.kind == IR_OPERAND_TEMP ||
             inst->dest.kind == IR_OPERAND_SYMBOL) &&
            inst->dest.name) {
          shape_ok = 0;
        }
        if (shape_ok) {
          group[group_count++] = k;
          call_seen = 1;
        }
        break;
      }
      default:
        shape_ok = 0;
        break;
      }
    }
    if (!shape_ok || !counter || !call_seen || group_count == 0) {
      continue;
    }

    {
      int cond_reads_counter = 0;
      int cond_tainted = 0;
      for (size_t k = i + 1; k <= cond_end; k++) {
        const IRInstruction *inst = &function->instructions[k];
        const IROperand *ops[2] = {&inst->lhs, &inst->rhs};
        for (int o = 0; o < 2; o++) {
          if (ops[o]->kind != IR_OPERAND_SYMBOL || !ops[o]->name) {
            continue;
          }
          if (counter && strcmp(ops[o]->name, counter) == 0) {
            cond_reads_counter = 1;
          }
          if (pure_licm_symbol_is_global(function, ops[o]->name)) {
            cond_tainted = 1;
          }
        }
      }
      if (!cond_reads_counter || cond_tainted) {
        continue;
      }
    }

    size_t guard_count = cond_end - i;
    size_t total = guard_count + group_count + 1;
    IRInstruction *clones = calloc(total, sizeof(IRInstruction));
    if (!clones) {
      return 0;
    }
    char skip_name[48];
    snprintf(skip_name, sizeof(skip_name), "licm_skip_%d",
             g_pure_licm_counter++);
    int ok = 1;
    size_t made = 0;
    for (size_t k = 0; k < guard_count && ok; k++) {
      ok = ir_clone_instruction_plain(&function->instructions[i + 1 + k],
                                      &clones[k]);
      if (ok) {
        made++;
        if (clones[k].op == IR_OP_BRANCH_ZERO ||
            clones[k].op == IR_OP_BRANCH_EQ) {
          char *copy = mettle_strdup(skip_name);
          if (copy) {
            mettle_free_string(clones[k].text);
            clones[k].text = copy;
          } else {
            ok = 0;
          }
        }
      }
    }
    for (size_t g = 0; g < group_count && ok; g++) {
      ok = ir_clone_instruction_plain(&function->instructions[group[g]],
                                      &clones[guard_count + g]);
      if (ok) {
        made++;
      }
    }
    if (ok) {
      IRInstruction *skip = &clones[guard_count + group_count];
      skip->op = IR_OP_LABEL;
      skip->location = function->instructions[i].location;
      skip->text = mettle_strdup(skip_name);
      ok = skip->text != NULL;
      if (ok) {
        made++;
      }
    }
    if (!ok) {
      for (size_t d = 0; d < made; d++) {
        ir_instruction_destroy_storage(&clones[d]);
      }
      free(clones);
      return 0;
    }

    SourceLocation loc = function->instructions[group[0]].location;
    for (size_t g = 0; g < group_count; g++) {
      IRInstruction *inst = &function->instructions[group[g]];
      ir_instruction_destroy_storage(inst);
      memset(inst, 0, sizeof(*inst));
      inst->op = IR_OP_NOP;
    }
    size_t insert_idx = i;
    while (insert_idx > 0 && pure_licm_is_simd_marker(
                                 &function->instructions[insert_idx - 1])) {
      insert_idx--;
    }
    for (size_t k = 0; k < total; k++) {
      if (!ir_instruction_insert_move(function, insert_idx, &clones[k])) {
        for (size_t d = k; d < total; d++) {
          ir_instruction_destroy_storage(&clones[d]);
        }
        free(clones);
        return 0;
      }
      insert_idx++;
    }
    free(clones);
    (void)counter_at;
    if (ir_explain_enabled()) {
      ir_explain_remark(
          function->name, "loop body", loc, 1,
          "hoisted above the loop (runs once, not every iteration)",
          "every iteration rewrites the same values: the body pins its "
          "globals to constants and calls only functions whose reads are "
          "those globals, so iterations after the first repeat the first",
          NULL, NULL);
      ir_explain_remark_code("hoisted");
    }
    return 1;
  }
  return 0;
}

static int pure_licm_hoist_one(IRProgram *program, IRFunction *function) {
  for (size_t i = 0; i < function->instruction_count; i++) {
    const IRInstruction *header = &function->instructions[i];
    if (header->op != IR_OP_LABEL || !header->text ||
        !ir_label_is_while_header(header->text)) {
      continue;
    }
    const char *loop_label = header->text;
    size_t backedge = pure_licm_find_backedge(function, i, loop_label);
    if (backedge == (size_t)-1 || backedge <= i + 1) {
      continue;
    }
    if (!pure_licm_range_side_effect_free(program, function, i + 1, backedge)) {
      continue;
    }

    for (size_t c = i + 1; c < backedge; c++) {
      IRInstruction *call = &function->instructions[c];
      if (call->op != IR_OP_CALL || !call->text) {
        continue;
      }
      if (call->dest.kind != IR_OPERAND_TEMP &&
          call->dest.kind != IR_OPERAND_SYMBOL) {
        continue;
      }
      IRFunction *callee = ir_program_find_function(program, call->text);
      if (!callee) {
        continue;
      }
      int unconditional = callee->is_speculatable_inferred &&
                          pure_licm_callee_hoistable(program, callee);
      size_t guard_end = 0;
      if (!unconditional) {
        if (!callee->is_readonly_inferred) {
          continue;
        }
        guard_end = pure_licm_cond_prefix_end(function, i, backedge);
        if (guard_end == 0 || guard_end >= c) {
          continue;
        }
      }
      int all_invariant = 1;
      for (size_t a = 0; a < call->argument_count; a++) {
        if (!pure_licm_operand_invariant(function, i + 1, backedge,
                                         &call->arguments[a])) {
          all_invariant = 0;
          break;
        }
      }
      if (!all_invariant) {
        continue;
      }

      char temp_name[32];
      snprintf(temp_name, sizeof(temp_name), "licm_pure_%d",
               g_pure_licm_counter++);
      char skip_name[48];
      snprintf(skip_name, sizeof(skip_name), "licm_skip_%d",
               g_pure_licm_counter - 1);

      IRInstruction hoisted;
      if (!ir_clone_instruction_plain(call, &hoisted)) {
        return 0;
      }
      ir_operand_destroy(&hoisted.dest);
      hoisted.dest = ir_operand_temp(temp_name);

      char hoisted_callee[128];
      snprintf(hoisted_callee, sizeof(hoisted_callee), "%s",
               hoisted.text ? hoisted.text : "?");

      size_t guard_count = unconditional ? 0 : guard_end - i;
      IRInstruction *guard_clones = NULL;
      if (guard_count > 0) {
        guard_clones = calloc(guard_count, sizeof(IRInstruction));
        if (!guard_clones) {
          ir_instruction_destroy_storage(&hoisted);
          return 0;
        }
        for (size_t k = 0; k < guard_count; k++) {
          int ok = ir_clone_instruction_plain(&function->instructions[i + 1 + k],
                                              &guard_clones[k]);
          if (ok && (guard_clones[k].op == IR_OP_BRANCH_ZERO ||
                     guard_clones[k].op == IR_OP_BRANCH_EQ)) {
            char *copy = mettle_strdup(skip_name);
            if (copy) {
              mettle_free_string(guard_clones[k].text);
              guard_clones[k].text = copy;
            } else {
              ok = 0;
            }
          }
          if (!ok) {
            for (size_t d = 0; d <= k; d++) {
              ir_instruction_destroy_storage(&guard_clones[d]);
            }
            free(guard_clones);
            ir_instruction_destroy_storage(&hoisted);
            return 0;
          }
        }
      }

      IROperand dest_copy = ir_operand_copy(&call->dest);
      int saved_is_float = call->is_float;
      int saved_float_bits = call->float_bits;
      SourceLocation saved_loc = call->location;
      ir_instruction_destroy_storage(call);
      call->op = IR_OP_ASSIGN;
      call->dest = dest_copy;
      call->lhs = ir_operand_temp(temp_name);
      call->rhs = ir_operand_none();
      call->is_float = saved_is_float;
      call->float_bits = saved_float_bits;
      call->location = saved_loc;

      size_t insert_idx = i;
      while (insert_idx > 0 && pure_licm_is_simd_marker(
                                   &function->instructions[insert_idx - 1])) {
        insert_idx--;
      }
      for (size_t k = 0; k < guard_count; k++) {
        if (!ir_instruction_insert_move(function, insert_idx, &guard_clones[k])) {
          for (size_t d = k; d < guard_count; d++) {
            ir_instruction_destroy_storage(&guard_clones[d]);
          }
          free(guard_clones);
          ir_instruction_destroy_storage(&hoisted);
          return 0;
        }
        insert_idx++;
      }
      free(guard_clones);
      if (!ir_instruction_insert_move(function, insert_idx, &hoisted)) {
        ir_instruction_destroy_storage(&hoisted);
        return 0;
      }
      insert_idx++;
      if (guard_count > 0) {
        IRInstruction skip_label = {0};
        skip_label.op = IR_OP_LABEL;
        skip_label.location = saved_loc;
        skip_label.text = mettle_strdup(skip_name);
        if (!skip_label.text ||
            !ir_instruction_insert_move(function, insert_idx, &skip_label)) {
          ir_instruction_destroy_storage(&skip_label);
          return 0;
        }
      }
      if (ir_explain_enabled()) {
        char entity[160];
        snprintf(entity, sizeof(entity), "call to `%s`", hoisted_callee);
        ir_explain_remark(
            function->name, entity, saved_loc, 1,
            "hoisted out of the loop (runs once, not every iteration)",
            unconditional
                ? "proof: the callee is inferred speculatable (it writes "
                  "nothing anywhere it can reach, cannot fault, and always "
                  "returns) and every argument is loop-invariant; consumed "
                  "by pure-call LICM"
                : "proof: the callee is inferred read-only (it writes nothing "
                  "anywhere it can reach) and every argument is "
                  "loop-invariant; consumed by pure-call LICM, which runs the "
                  "hoisted call under a copy of the loop's entry test",
            NULL, NULL);
        ir_explain_remark_code("hoisted");
      }
      return 1;
    }
  }
  return 0;
}

int ir_hoist_pure_calls_pass(IRProgram *program, int *changed) {
  if (!program) {
    return 0;
  }
  ir_purity_infer(program);
  for (size_t i = 0; i < program->function_count; i++) {
    IRFunction *function = program->functions[i];
    if (!function) {
      continue;
    }
    while (pure_licm_hoist_one(program, function)) {
      if (changed) {
        *changed = 1;
      }
    }
    while (pure_licm_collapse_idempotent_body(program, function)) {
      if (changed) {
        *changed = 1;
      }
    }
  }
  return 1;
}
