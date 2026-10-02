#include "ir.h"
#include "../common.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int asm_name_start(char c) {
  return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_';
}

static int asm_name_char(char c) {
  return asm_name_start(c) || (c >= '0' && c <= '9');
}

static int asm_space(char c) {
  return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

static size_t asm_binding_at(const char *at, const char *end, char *name,
                             size_t capacity) {
  const char *cursor = at;
  if (cursor >= end || *cursor != '{') {
    return 0;
  }
  cursor++;
  while (cursor < end && (*cursor == ' ' || *cursor == '\t')) {
    cursor++;
  }
  if (cursor >= end || !asm_name_start(*cursor)) {
    return 0;
  }
  const char *first = cursor;
  while (cursor < end && asm_name_char(*cursor)) {
    cursor++;
  }
  const char *last = cursor;
  while (cursor < end && (*cursor == ' ' || *cursor == '\t')) {
    cursor++;
  }
  if (cursor >= end || *cursor != '}') {
    return 0;
  }
  if (name) {
    size_t length = (size_t)(last - first);
    if (length >= capacity) {
      length = capacity - 1;
    }
    memcpy(name, first, length);
    name[length] = '\0';
  }
  return (size_t)(cursor + 1 - at);
}

typedef struct {
  IRAsmBinding *items;
  size_t count;
  size_t capacity;
  int failed;
} AsmBindingList;

static IRAsmBinding *asm_binding_note(AsmBindingList *list, const char *name,
                                      int reads, int writes) {
  for (size_t i = 0; i < list->count; i++) {
    if (strcmp(list->items[i].name, name) == 0) {
      list->items[i].reads |= (unsigned char)(reads != 0);
      list->items[i].writes |= (unsigned char)(writes != 0);
      return &list->items[i];
    }
  }
  if (list->count == list->capacity) {
    size_t grown = list->capacity ? list->capacity * 2 : 8;
    IRAsmBinding *items = realloc(list->items, grown * sizeof(*items));
    if (!items) {
      list->failed = 1;
      return NULL;
    }
    list->items = items;
    list->capacity = grown;
  }
  IRAsmBinding *binding = &list->items[list->count++];
  memset(binding, 0, sizeof(*binding));
  snprintf(binding->name, sizeof(binding->name), "%s", name);
  binding->reads = (unsigned char)(reads != 0);
  binding->writes = (unsigned char)(writes != 0);
  return binding;
}

static void asm_note_range(AsmBindingList *list, const char *from,
                           const char *to, int reads, int writes) {
  char name[128];
  for (const char *at = from; at < to;) {
    size_t used = asm_binding_at(at, to, name, sizeof(name));
    if (used) {
      asm_binding_note(list, name, reads, writes);
      at += used;
    } else {
      at++;
    }
  }
}

static int asm_opcode_reads_first_operand(const char *base, size_t length) {
  static const char *const names[] = {
      "bar",      "barrier",   "bra",         "brx",           "call",
      "ret",      "exit",      "trap",        "membar",        "fence",
      "nanosleep", "pmevent",  "griddepcontrol", "setmaxnreg", "prefetch",
      "prefetchu", "st",       "red",         "stmatrix",      "cp",
      "discard",  "applypriority"};
  for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
    if (strlen(names[i]) == length && strncmp(names[i], base, length) == 0) {
      return 1;
    }
  }
  return 0;
}

static int asm_opcode_is(const char *base, size_t length, const char *name) {
  return strlen(name) == length && strncmp(base, name, length) == 0;
}

static const char *asm_first_operand_end(const char *from, const char *to) {
  int depth = 0;
  for (const char *at = from; at < to; at++) {
    if (*at == '{' || *at == '[' || *at == '(') {
      depth++;
    } else if (*at == '}' || *at == ']' || *at == ')') {
      depth--;
    } else if (*at == ',' && depth == 0) {
      return at;
    }
  }
  return to;
}

static void asm_classify_statement(AsmBindingList *list, const char *from,
                                   const char *to, int *has_label,
                                   int *has_branch) {
  const char *at = from;
  for (;;) {
    while (at < to && asm_space(*at)) {
      at++;
    }
    if (at < to && *at == '{' && !asm_binding_at(at, to, NULL, 0)) {
      at++;
      continue;
    }
    if (at < to && *at == '}') {
      at++;
      continue;
    }
    if (at < to && asm_name_start(*at) && !asm_binding_at(at, to, NULL, 0)) {
      const char *name_end = at;
      while (name_end < to && (asm_name_char(*name_end) || *name_end == '$')) {
        name_end++;
      }
      const char *colon = name_end;
      while (colon < to && (*colon == ' ' || *colon == '\t')) {
        colon++;
      }
      if (colon < to && *colon == ':') {
        *has_label = 1;
        at = colon + 1;
        continue;
      }
    }
    break;
  }
  if (at >= to) {
    return;
  }
  if (*at == '.') {
    asm_note_range(list, at, to, 1, 1);
    return;
  }
  int guarded = 0;
  if (*at == '@') {
    guarded = 1;
    at++;
    if (at < to && *at == '!') {
      at++;
    }
    size_t used = asm_binding_at(at, to, NULL, 0);
    if (used) {
      asm_note_range(list, at, at + used, 1, 0);
      at += used;
    } else {
      while (at < to && (asm_name_char(*at) || *at == '%' || *at == '$')) {
        at++;
      }
    }
    while (at < to && asm_space(*at)) {
      at++;
    }
  }
  const char *opcode = at;
  while (at < to && (asm_name_char(*at) || *at == '.')) {
    at++;
  }
  if (at == opcode) {
    asm_note_range(list, opcode, to, 1, 1);
    return;
  }
  const char *dot = memchr(opcode, '.', (size_t)(at - opcode));
  size_t base_length = (size_t)((dot ? dot : at) - opcode);
  if (asm_opcode_is(opcode, base_length, "bra") ||
      asm_opcode_is(opcode, base_length, "brx") ||
      asm_opcode_is(opcode, base_length, "call") ||
      asm_opcode_is(opcode, base_length, "ret")) {
    *has_branch = 1;
  }
  while (at < to && asm_space(*at)) {
    at++;
  }
  const char *first_end = asm_first_operand_end(at, to);
  if ((at < to && *at == '[') ||
      asm_opcode_reads_first_operand(opcode, base_length)) {
    asm_note_range(list, at, to, 1, 0);
    return;
  }
  int accumulates = asm_opcode_is(opcode, base_length, "wgmma");
  asm_note_range(list, at, first_end, guarded || accumulates, 1);
  asm_note_range(list, first_end, to, 1, 0);
}

static char *asm_strip_comments(const char *text) {
  size_t length = strlen(text);
  char *copy = malloc(length + 1);
  if (!copy) {
    return NULL;
  }
  memcpy(copy, text, length + 1);
  for (size_t i = 0; i < length; i++) {
    if (copy[i] == '/' && i + 1 < length && copy[i + 1] == '/') {
      while (i < length && copy[i] != '\n') {
        copy[i++] = ' ';
      }
    } else if (copy[i] == '/' && i + 1 < length && copy[i + 1] == '*') {
      while (i < length && !(copy[i] == '*' && i + 1 < length &&
                             copy[i + 1] == '/')) {
        copy[i++] = ' ';
      }
      if (i < length) {
        copy[i] = ' ';
        if (i + 1 < length) {
          copy[i + 1] = ' ';
        }
        i++;
      }
    }
  }
  return copy;
}

static int asm_scan(const char *text, AsmBindingList *list, int *has_label,
                    int *has_branch) {
  char *clean = asm_strip_comments(text);
  if (!clean) {
    return 0;
  }
  char name[128];
  const char *end = clean + strlen(clean);
  for (const char *at = clean; at < end;) {
    size_t used = asm_binding_at(at, end, name, sizeof(name));
    if (used) {
      asm_binding_note(list, name, 0, 0);
      at += used;
    } else {
      at++;
    }
  }
  for (const char *at = clean; at < end;) {
    const char *stop = memchr(at, ';', (size_t)(end - at));
    if (!stop) {
      stop = end;
    }
    asm_classify_statement(list, at, stop, has_label, has_branch);
    at = stop < end ? stop + 1 : end;
  }
  free(clean);
  return !list->failed;
}

int ir_inline_asm_defines_label(const char *text) {
  AsmBindingList list = {0};
  int has_label = 0;
  int has_branch = 0;
  int ok = text ? asm_scan(text, &list, &has_label, &has_branch) : 1;
  free(list.items);
  return !ok || has_label;
}

int ir_inline_asm_bindings(const char *text, IRAsmBinding **out,
                           size_t *count) {
  AsmBindingList list = {0};
  int has_label = 0;
  int has_branch = 0;
  *out = NULL;
  *count = 0;
  if (!text) {
    return 1;
  }
  if (!asm_scan(text, &list, &has_label, &has_branch)) {
    free(list.items);
    return 0;
  }
  for (size_t i = 0; i < list.count; i++) {
    if (!list.items[i].reads && !list.items[i].writes) {
      list.items[i].reads = 1;
      list.items[i].writes = 1;
    }
    if (list.items[i].writes && (has_label || has_branch)) {
      list.items[i].reads = 1;
    }
  }
  *out = list.items;
  *count = list.count;
  return 1;
}

static int asm_type_name_is_scalar(const char *type) {
  static const char *const scalars[] = {
      "int8",    "int16",   "int32",    "int64",   "uint8",  "uint16",
      "uint32",  "uint64",  "bool",     "float32", "float64", "float16",
      "bfloat16", "char",   "rawptr",   "cstring"};
  if (!type || !*type) {
    return 0;
  }
  size_t length = strlen(type);
  if (type[length - 1] == '*') {
    return 1;
  }
  for (size_t i = 0; i < sizeof(scalars) / sizeof(scalars[0]); i++) {
    if (strcmp(type, scalars[i]) == 0) {
      return 1;
    }
  }
  return 0;
}

static int asm_type_is_scalar(const MtlcType *type, const char *type_name) {
  if (type) {
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
    case MTLC_TYPE_FLOAT16:
    case MTLC_TYPE_BFLOAT16:
    case MTLC_TYPE_POINTER:
      return 1;
    default:
      return 0;
    }
  }
  return asm_type_name_is_scalar(type_name);
}

typedef struct {
  const char *type_name;
  MtlcType *type;
  const IRInstruction *declaration;
  int found;
  int scalar;
} AsmTarget;

static int asm_name_address_taken(const IRFunction *function,
                                  const char *name) {
  for (size_t i = 0; i < function->instruction_count; i++) {
    const IRInstruction *in = &function->instructions[i];
    if (in->op == IR_OP_ADDRESS_OF && in->lhs.kind == IR_OPERAND_SYMBOL &&
        in->lhs.name && strcmp(in->lhs.name, name) == 0) {
      return 1;
    }
  }
  return 0;
}

static AsmTarget asm_resolve(const IRProgram *program,
                             const IRFunction *function, const char *name) {
  AsmTarget target = {0};
  for (size_t i = 0; i < function->instruction_count; i++) {
    const IRInstruction *in = &function->instructions[i];
    if ((in->op == IR_OP_DECLARE_LOCAL ||
         in->op == IR_OP_ADDRESS_SPACE_ALLOC) &&
        in->dest.kind == IR_OPERAND_SYMBOL && in->dest.name &&
        strcmp(in->dest.name, name) == 0) {
      target.found = 1;
      target.declaration = in;
      target.type = in->value_type;
      target.type_name = in->text;
      target.scalar = in->op == IR_OP_DECLARE_LOCAL &&
                      asm_type_is_scalar(in->value_type, in->text);
      break;
    }
  }
  if (!target.found) {
    const IRModuleSymbol *symbol =
        function->name ? ir_program_lookup_symbol(program, function->name)
                       : NULL;
    for (size_t p = 0; p < function->parameter_count; p++) {
      if (!function->parameter_names || !function->parameter_names[p] ||
          strcmp(function->parameter_names[p], name) != 0) {
        continue;
      }
      target.found = 1;
      target.type_name =
          function->parameter_types ? function->parameter_types[p] : NULL;
      if (symbol && symbol->param_types && p < symbol->param_count) {
        target.type = symbol->param_types[p];
      }
      target.scalar = asm_type_is_scalar(target.type, target.type_name);
      break;
    }
  }
  if (target.found && target.scalar &&
      asm_name_address_taken(function, name)) {
    target.scalar = 0;
  }
  return target;
}

static int asm_type_is_float(const MtlcType *type, const char *type_name,
                             int *bits) {
  MtlcTypeKind kind = type ? type->kind : MTLC_TYPE_VOID;
  if (!type && type_name) {
    if (strcmp(type_name, "float32") == 0) {
      kind = MTLC_TYPE_FLOAT32;
    } else if (strcmp(type_name, "float64") == 0) {
      kind = MTLC_TYPE_FLOAT64;
    }
  }
  if (kind == MTLC_TYPE_FLOAT32) {
    *bits = 32;
    return 1;
  }
  if (kind == MTLC_TYPE_FLOAT64) {
    *bits = 64;
    return 1;
  }
  *bits = 0;
  return 0;
}

static int asm_type_is_unsigned(const MtlcType *type, const char *type_name) {
  if (type) {
    return type->kind == MTLC_TYPE_UINT8 || type->kind == MTLC_TYPE_UINT16 ||
           type->kind == MTLC_TYPE_UINT32 || type->kind == MTLC_TYPE_UINT64 ||
           type->kind == MTLC_TYPE_BOOL;
  }
  return type_name && (strncmp(type_name, "uint", 4) == 0 ||
                       strcmp(type_name, "bool") == 0);
}

static char *asm_error(const char *format, ...) {
  char buffer[512];
  va_list args;
  va_start(args, format);
  vsnprintf(buffer, sizeof(buffer), format, args);
  va_end(args);
  return mettle_strdup(buffer);
}

typedef struct {
  IRInstruction *items;
  size_t count;
  size_t capacity;
} AsmInstructionList;

static int asm_push(AsmInstructionList *list, const IRInstruction *in) {
  if (list->count == list->capacity) {
    size_t grown = list->capacity ? list->capacity * 2 : 64;
    IRInstruction *items = realloc(list->items, grown * sizeof(*items));
    if (!items) {
      return 0;
    }
    list->items = items;
    list->capacity = grown;
  }
  list->items[list->count++] = *in;
  return 1;
}

static int asm_bind_instruction(const IRProgram *program, IRFunction *function,
                                IRInstruction *in, AsmInstructionList *out,
                                char **error) {
  IRAsmBinding *bindings = NULL;
  size_t count = 0;
  if (!ir_inline_asm_bindings(in->text, &bindings, &count)) {
    *error = mettle_strdup("PTX asm: out of memory reading bindings");
    return 0;
  }
  AsmTarget *targets = count ? calloc(count, sizeof(*targets)) : NULL;
  if (count && !targets) {
    free(bindings);
    *error = mettle_strdup("PTX asm: out of memory reading bindings");
    return 0;
  }
  for (size_t k = 0; k < count; k++) {
    targets[k] = asm_resolve(program, function, bindings[k].name);
    if (!targets[k].found) {
      *error = asm_error("PTX asm: `{%s}` names no local or parameter of '%s'",
                         bindings[k].name,
                         function->name ? function->name : "?");
    } else if (!targets[k].scalar) {
      *error = asm_error("PTX asm: `{%s}` is not a scalar held in a register "
                         "(an aggregate, or a local whose address is taken)",
                         bindings[k].name);
    }
    if (*error) {
      free(targets);
      free(bindings);
      return 0;
    }
  }
  if (count) {
    in->arguments = calloc(count, sizeof(IROperand));
    in->argument_types = calloc(count, sizeof(MtlcType *));
    if (!in->arguments || !in->argument_types) {
      free(in->arguments);
      free(in->argument_types);
      in->arguments = NULL;
      in->argument_types = NULL;
      free(targets);
      free(bindings);
      *error = mettle_strdup("PTX asm: out of memory binding operands");
      return 0;
    }
    in->argument_count = count;
    for (size_t k = 0; k < count; k++) {
      in->arguments[k] = bindings[k].reads ? ir_operand_symbol(bindings[k].name)
                                           : ir_operand_none();
      in->argument_types[k] = targets[k].type;
    }
  }
  in->asm_operands = 1;
  int ok = asm_push(out, in);
  for (size_t k = 0; ok && k < count; k++) {
    if (!bindings[k].writes) {
      continue;
    }
    IRInstruction result;
    memset(&result, 0, sizeof(result));
    result.op = IR_OP_ASM_RESULT;
    result.location = in->location;
    result.dest = ir_operand_symbol(bindings[k].name);
    result.rhs = ir_operand_int((long long)k);
    result.value_type = targets[k].type;
    result.text = mettle_strdup(targets[k].type_name ? targets[k].type_name
                                                     : "");
    result.is_float = asm_type_is_float(targets[k].type, targets[k].type_name,
                                        &result.float_bits);
    result.is_unsigned =
        asm_type_is_unsigned(targets[k].type, targets[k].type_name);
    if (targets[k].declaration) {
      result.alias_class = targets[k].declaration->alias_class;
    }
    ok = asm_push(out, &result);
  }
  free(targets);
  free(bindings);
  if (!ok) {
    *error = mettle_strdup("PTX asm: out of memory binding operands");
  }
  return ok;
}

static int asm_bind_function(const IRProgram *program, IRFunction *function,
                             char **error) {
  int has_asm = 0;
  for (size_t i = 0; i < function->instruction_count; i++) {
    if (function->instructions[i].op == IR_OP_INLINE_ASM &&
        !function->instructions[i].asm_operands) {
      has_asm = 1;
      break;
    }
  }
  if (!has_asm) {
    return 1;
  }
  AsmInstructionList out = {0};
  size_t done = 0;
  int ok = 1;
  for (; done < function->instruction_count && ok; done++) {
    IRInstruction *in = &function->instructions[done];
    if (in->op == IR_OP_INLINE_ASM && !in->asm_operands) {
      ok = asm_bind_instruction(program, function, in, &out, error);
    } else {
      ok = asm_push(&out, in);
    }
  }
  if (!ok) {
    for (size_t i = 0; i < out.count; i++) {
      if (out.items[i].op == IR_OP_ASM_RESULT) {
        ir_instruction_destroy(&out.items[i]);
      }
    }
    free(out.items);
    return 0;
  }
  free(function->instructions);
  function->instructions = out.items;
  function->instruction_count = out.count;
  function->instruction_capacity = out.capacity;
  ir_function_clear_cfg(function);
  return ir_function_rebuild_cfg(function);
}

int ir_program_bind_device_asm(IRProgram *program, char **error) {
  IRGpuCallGraph graph = {0};
  char *graph_error = NULL;
  *error = NULL;
  if (!program) {
    return 1;
  }
  if (!ir_program_build_gpu_call_graph(program, &graph, &graph_error)) {
    free(graph_error);
    return 1;
  }
  int ok = 1;
  for (size_t i = 0; ok && i < graph.count; i++) {
    IRFunction *function = program->functions[graph.order[i]];
    if (function) {
      ok = asm_bind_function(program, function, error);
    }
  }
  ir_gpu_call_graph_destroy(&graph);
  return ok;
}
