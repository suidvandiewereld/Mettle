#include "ir_lowering_internal.h"
#include "frontend/mtlc_frontend.h"

static int tile_type_is(Type *type) { return type_checker_is_tile(type); }

static long long tile_element_count(Type *type) {
  long long count = (long long)type->view_extents[0];
  if (type->view_rank == 2) {
    count *= (long long)type->view_extents[1];
  }
  return count;
}

static int tile_emit_allocation(IRLoweringContext *context,
                                IRFunction *function, Type *type,
                                const IROperand *dest,
                                SourceLocation location) {
  IRInstruction alloc = {0};
  alloc.op = IR_OP_ADDRESS_SPACE_ALLOC;
  alloc.location = location;
  alloc.dest = *dest;
  alloc.rhs = ir_operand_int(tile_element_count(type));
  alloc.text = type->base_type->name;
  alloc.value_type = mtlc_type_from_frontend(type);
  alloc.address_space = MTLC_ADDRESS_SPACE_PRIVATE;
  if (!alloc.value_type) {
    ir_set_error(context, "Unable to lower tile type '%s'", type->name);
    return 0;
  }
  return ir_emit(context, function, &alloc);
}

static int tile_new_temp(IRLoweringContext *context, IRFunction *function,
                         Type *type, SourceLocation location,
                         IROperand *out) {
  if (!ir_make_temp_operand(context, out)) {
    return 0;
  }
  if (!tile_emit_allocation(context, function, type, out, location)) {
    ir_operand_destroy(out);
    return 0;
  }
  return 1;
}

static int tile_emit(IRLoweringContext *context, IRFunction *function,
                     const char *op, const IROperand *dest, Type *dest_type,
                     IROperand *arguments, MtlcType **argument_types,
                     size_t count, SourceLocation location,
                     const MtlcTensorMmaDesc *mma) {
  IRInstruction instruction = {0};
  IRTensorAux tensor;
  IROperand operands[8];
  MtlcType *types[8];
  if (count + 1 > sizeof(operands) / sizeof(operands[0])) {
    ir_set_error(context, "A tile operation has too many operands");
    return 0;
  }
  operands[0] = *dest;
  types[0] = mtlc_type_from_frontend(dest_type);
  for (size_t i = 0; i < count; i++) {
    operands[i + 1] = arguments[i];
    types[i + 1] = argument_types ? argument_types[i] : NULL;
  }
  instruction.op = IR_OP_TILE;
  instruction.location = location;
  instruction.text = (char *)op;
  instruction.dest = ir_operand_none();
  instruction.value_type = types[0];
  instruction.arguments = operands;
  instruction.argument_types = types;
  instruction.argument_count = count + 1;
  if (mma) {
    ir_instruction_tensor_attach(&instruction, &tensor);
    tensor.mma = *mma;
  }
  return ir_emit(context, function, &instruction);
}

static int tile_is_literal(ASTNode *node, double *value) {
  if (node && node->type == AST_NUMBER_LITERAL) {
    NumberLiteral *literal = (NumberLiteral *)node->data;
    *value = literal->is_float ? literal->float_value
                               : (double)literal->int_value;
    return 1;
  }
  if (node && node->type == AST_UNARY_EXPRESSION) {
    UnaryExpression *unary = (UnaryExpression *)node->data;
    if (unary && unary->operator && strcmp(unary->operator, "-") == 0 &&
        tile_is_literal(unary->operand, value)) {
      *value = -*value;
      return 1;
    }
  }
  return 0;
}

static int tile_scalar_operand(IRLoweringContext *context,
                               IRFunction *function, ASTNode *node,
                               Type *element, IROperand *out,
                               MtlcType **out_type) {
  double value = 0.0;
  if (element && tile_is_literal(node, &value)) {
    if (element->kind == TYPE_FLOAT32) {
      *out = ir_operand_float_sized(value, 32);
    } else if (element->kind == TYPE_FLOAT64) {
      *out = ir_operand_float_sized(value, 64);
    } else {
      *out = ir_operand_int((long long)value);
    }
    *out_type = mtlc_type_from_frontend(element);
    return 1;
  }
  if (!ir_lower_expression(context, function, node, out)) {
    return 0;
  }
  *out_type = node->resolved_type ? mtlc_type_from_frontend(node->resolved_type)
                                  : NULL;
  return 1;
}

static int tile_operand(IRLoweringContext *context, IRFunction *function,
                        ASTNode *node, Type *element, IROperand *out,
                        MtlcType **out_type) {
  if (node->resolved_type && tile_type_is(node->resolved_type)) {
    *out_type = mtlc_type_from_frontend(node->resolved_type);
    return ir_lower_tile_expression(context, function, node, NULL, out);
  }
  return tile_scalar_operand(context, function, node, element, out, out_type);
}

static Type *tile_scalar_element(ASTNode *a, ASTNode *b) {
  if (a && a->resolved_type && tile_type_is(a->resolved_type)) {
    return a->resolved_type->base_type;
  }
  if (b && b->resolved_type && tile_type_is(b->resolved_type)) {
    return b->resolved_type->base_type;
  }
  return NULL;
}

static const char *tile_binary_name(const char *op) {
  static const struct {
    const char *source;
    const char *ir;
  } names[] = {{"+", "add"}, {"-", "sub"}, {"*", "mul"}, {"/", "div"},
               {"<", "lt"},  {"<=", "le"}, {">", "gt"},  {">=", "ge"},
               {"==", "eq"}, {"!=", "ne"}, {"&&", "and"}, {"||", "or"}};
  for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
    if (strcmp(op, names[i].source) == 0) {
      return names[i].ir;
    }
  }
  return NULL;
}

static const char *tile_math_name(const char *name) {
  static const struct {
    const char *source;
    const char *ir;
  } names[] = {{"expf", "exp"},   {"sqrtf", "sqrt"}, {"rsqrtf", "rsqrt"},
               {"fabsf", "abs"},  {"logf", "log"},   {"sinf", "sin"},
               {"cosf", "cos"}};
  for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
    if (strcmp(name, names[i].source) == 0) {
      return names[i].ir;
    }
  }
  return NULL;
}

static void tile_release(IROperand *operands, size_t count) {
  for (size_t i = 0; i < count; i++) {
    ir_operand_destroy(&operands[i]);
  }
}

static int tile_result(IRLoweringContext *context, IRFunction *function,
                       Type *type, const IROperand *dest,
                       SourceLocation location, IROperand *out) {
  if (dest) {
    *out = ir_operand_copy(dest);
    return out->kind != IR_OPERAND_NONE;
  }
  return tile_new_temp(context, function, type, location, out);
}

int ir_lower_tile_expression(IRLoweringContext *context, IRFunction *function,
                             ASTNode *expression, const IROperand *dest,
                             IROperand *out) {
  Type *type = expression ? expression->resolved_type : NULL;
  IROperand arguments[3] = {{0}, {0}, {0}};
  MtlcType *argument_types[3] = {NULL, NULL, NULL};
  size_t count = 0;
  const char *op = NULL;
  IROperand target = {0};
  int ok = 0;

  if (!type || !tile_type_is(type)) {
    ir_set_error(context, "A tile expression reached lowering without a tile "
                          "type");
    return 0;
  }

  switch (expression->type) {
  case AST_IDENTIFIER: {
    Identifier *identifier = (Identifier *)expression->data;
    IROperand self = ir_operand_symbol(
        ir_local_ir_name(context, identifier->name));
    if (!dest || (dest->name && self.name &&
                  strcmp(dest->name, self.name) == 0)) {
      *out = self;
      return 1;
    }
    arguments[0] = self;
    argument_types[0] = mtlc_type_from_frontend(type);
    ok = tile_emit(context, function, "copy", dest, type, arguments,
                   argument_types, 1, expression->location, NULL);
    ir_operand_destroy(&arguments[0]);
    *out = ir_operand_copy(dest);
    return ok;
  }
  case AST_BINARY_EXPRESSION: {
    BinaryExpression *binary = (BinaryExpression *)expression->data;
    Type *element = tile_scalar_element(binary->left, binary->right);
    op = tile_binary_name(binary->operator);
    if (!op ||
        !tile_operand(context, function, binary->left, element, &arguments[0],
                      &argument_types[0]) ||
        !tile_operand(context, function, binary->right, element,
                      &arguments[1], &argument_types[1])) {
      tile_release(arguments, 2);
      if (!op) ir_set_error(context, "Unknown tile operator");
      return 0;
    }
    count = 2;
    break;
  }
  case AST_UNARY_EXPRESSION: {
    UnaryExpression *unary = (UnaryExpression *)expression->data;
    op = strcmp(unary->operator, "!") == 0 ? "not" : "neg";
    if (!tile_operand(context, function, unary->operand, NULL, &arguments[0],
                      &argument_types[0])) {
      return 0;
    }
    count = 1;
    break;
  }
  case AST_CAST_EXPRESSION: {
    CastExpression *cast = (CastExpression *)expression->data;
    op = "cast";
    if (!tile_operand(context, function, cast->operand, NULL, &arguments[0],
                      &argument_types[0])) {
      return 0;
    }
    count = 1;
    break;
  }
  case AST_FUNCTION_CALL: {
    CallExpression *call = (CallExpression *)expression->data;
    Type *element = NULL;
    switch (call->tile_builtin) {
    case TILE_BUILTIN_ROW:
      op = "row";
      break;
    case TILE_BUILTIN_COL:
      op = "col";
      break;
    case TILE_BUILTIN_ROW_MAX:
    case TILE_BUILTIN_ROW_SUM:
      op = call->tile_builtin == TILE_BUILTIN_ROW_MAX ? "row_max" : "row_sum";
      if (!tile_operand(context, function, call->arguments[0], NULL,
                        &arguments[0], &argument_types[0])) {
        return 0;
      }
      count = 1;
      break;
    case TILE_BUILTIN_SELECT:
      op = "select";
      element = type->base_type;
      if (!tile_operand(context, function, call->arguments[0], NULL,
                        &arguments[0], &argument_types[0]) ||
          !tile_operand(context, function, call->arguments[1], element,
                        &arguments[1], &argument_types[1]) ||
          !tile_operand(context, function, call->arguments[2], element,
                        &arguments[2], &argument_types[2])) {
        tile_release(arguments, 3);
        return 0;
      }
      count = 3;
      break;
    case TILE_BUILTIN_MAX:
    case TILE_BUILTIN_MIN:
      op = call->tile_builtin == TILE_BUILTIN_MAX ? "max" : "min";
      element = type->base_type;
      if (!tile_operand(context, function, call->arguments[0], element,
                        &arguments[0], &argument_types[0]) ||
          !tile_operand(context, function, call->arguments[1], element,
                        &arguments[1], &argument_types[1])) {
        tile_release(arguments, 2);
        return 0;
      }
      count = 2;
      break;
    case TILE_BUILTIN_MATH:
      op = tile_math_name(call->function_name);
      if (!op || !tile_operand(context, function, call->arguments[0], NULL,
                               &arguments[0], &argument_types[0])) {
        if (!op) ir_set_error(context, "Unknown tile math function");
        return 0;
      }
      count = 1;
      break;
    default:
      ir_set_error(context, "Call '%s' reached tile lowering",
                   call->function_name ? call->function_name : "?");
      return 0;
    }
    break;
  }
  default:
    ir_set_error(context, "Unsupported tile expression");
    return 0;
  }

  if (!tile_result(context, function, type, dest, expression->location,
                   &target)) {
    tile_release(arguments, count);
    return 0;
  }
  ok = tile_emit(context, function, op, &target, type, arguments,
                 argument_types, count, expression->location, NULL);
  tile_release(arguments, count);
  if (!ok) {
    ir_operand_destroy(&target);
    return 0;
  }
  *out = target;
  return 1;
}

static int tile_fill(IRLoweringContext *context, IRFunction *function,
                     Type *type, const IROperand *dest, ASTNode *value,
                     SourceLocation location) {
  IROperand argument = {0};
  MtlcType *argument_type = NULL;
  int ok;
  if (value) {
    if (!tile_scalar_operand(context, function, value, type->base_type,
                             &argument, &argument_type)) {
      return 0;
    }
  } else {
    argument = type->base_type->kind == TYPE_FLOAT32
                   ? ir_operand_float_sized(0.0, 32)
                   : ir_operand_int(0);
    argument_type = mtlc_type_from_frontend(type->base_type);
  }
  ok = tile_emit(context, function, "fill", dest, type, &argument,
                 &argument_type, 1, location, NULL);
  ir_operand_destroy(&argument);
  return ok;
}

int ir_lower_tile_store_into(IRLoweringContext *context, IRFunction *function,
                             Type *type, const IROperand *dest, ASTNode *value,
                             SourceLocation location) {
  IROperand ignored = {0};
  int ok;
  if (!value->resolved_type || !tile_type_is(value->resolved_type)) {
    return tile_fill(context, function, type, dest, value, location);
  }
  ok = ir_lower_tile_expression(context, function, value, dest, &ignored);
  ir_operand_destroy(&ignored);
  return ok;
}

int ir_lower_tile_declaration(IRLoweringContext *context, IRFunction *function,
                              ASTNode *statement, VarDeclaration *declaration,
                              Type *type) {
  const char *name =
      ir_local_bind(context, declaration->name, ir_backend_type_name(
                                                    declaration->type_name));
  IROperand dest = ir_operand_symbol(name);
  int ok = tile_emit_allocation(context, function, type, &dest,
                                statement->location);
  if (ok) {
    ok = declaration->initializer
             ? ir_lower_tile_store_into(context, function, type, &dest,
                                        declaration->initializer,
                                        statement->location)
             : tile_fill(context, function, type, &dest, NULL,
                         statement->location);
  }
  ir_operand_destroy(&dest);
  return ok;
}

static ASTNode *tile_named(CallExpression *call, const char *name) {
  for (size_t i = 0; i < call->argument_count; i++) {
    if (call->argument_names && call->argument_names[i] &&
        strcmp(call->argument_names[i], name) == 0) {
      return call->arguments[i];
    }
  }
  return NULL;
}

static int tile_optional_integer(IRLoweringContext *context,
                                 IRFunction *function, ASTNode *node,
                                 IROperand *out, MtlcType **out_type) {
  if (!node) {
    *out = ir_operand_none();
    *out_type = NULL;
    return 1;
  }
  if (!ir_lower_expression(context, function, node, out)) {
    return 0;
  }
  *out_type = node->resolved_type ? mtlc_type_from_frontend(node->resolved_type)
                                  : NULL;
  return 1;
}

static int tile_lower_load_store(IRLoweringContext *context,
                                 IRFunction *function, ASTNode *expression,
                                 CallExpression *call, int store) {
  IROperand arguments[3] = {{0}, {0}, {0}};
  MtlcType *argument_types[3] = {NULL, NULL, NULL};
  ASTNode *tile_node = call->arguments[store ? 1 : 0];
  ASTNode *memory_node = call->arguments[store ? 0 : 1];
  IROperand memory = {0};
  IROperand target = {0};
  int ok = 0;
  if (!ir_lower_expression(context, function, memory_node, &memory)) {
    return 0;
  }
  if (!tile_operand(context, function, tile_node, NULL, &arguments[0],
                    &argument_types[0]) ||
      !tile_optional_integer(context, function, tile_named(call, "ld"),
                             &arguments[1], &argument_types[1]) ||
      !tile_optional_integer(context, function, tile_named(call, "rows"),
                             &arguments[2], &argument_types[2])) {
    ir_operand_destroy(&memory);
    tile_release(arguments, 3);
    return 0;
  }
  if (store) {
    ok = tile_emit(context, function, "store", &memory,
                   memory_node->resolved_type, arguments, argument_types, 3,
                   expression->location, NULL);
  } else {
    MtlcType *swap_type = argument_types[0];
    target = arguments[0];
    arguments[0] = memory;
    argument_types[0] =
        memory_node->resolved_type
            ? mtlc_type_from_frontend(memory_node->resolved_type)
            : NULL;
    memory = ir_operand_none();
    (void)swap_type;
    ok = tile_emit(context, function, "load", &target, tile_node->resolved_type,
                   arguments, argument_types, 3, expression->location, NULL);
    ir_operand_destroy(&target);
  }
  ir_operand_destroy(&memory);
  tile_release(arguments, 3);
  return ok;
}

static int tile_lower_mma(IRLoweringContext *context, IRFunction *function,
                          ASTNode *expression, CallExpression *call) {
  IROperand arguments[5] = {{0}, {0}, {0}, {0}, {0}};
  MtlcType *argument_types[5] = {NULL, NULL, NULL, NULL, NULL};
  ASTNode *d_node = call->arguments[3];
  Type *d_type = d_node->resolved_type;
  IROperand d = {0};
  int ok;
  if (!tile_operand(context, function, call->arguments[0], NULL, &arguments[0],
                    &argument_types[0]) ||
      !tile_operand(context, function, call->arguments[1], NULL, &arguments[1],
                    &argument_types[1])) {
    tile_release(arguments, 2);
    return 0;
  }
  if (call->tensor_c_zero) {
    arguments[2] = ir_operand_float_sized(0.0, 32);
    argument_types[2] = mtlc_type_from_frontend(d_type->base_type);
  } else if (!tile_operand(context, function, call->arguments[2], NULL,
                           &arguments[2], &argument_types[2])) {
    tile_release(arguments, 2);
    return 0;
  }
  if (!tile_optional_integer(
          context, function,
          call->tensor_b_stride_argument != SIZE_MAX
              ? call->arguments[call->tensor_b_stride_argument]
              : NULL,
          &arguments[3], &argument_types[3])) {
    tile_release(arguments, 3);
    return 0;
  }
  if (call->tensor_c_scale_argument != SIZE_MAX) {
    if (!tile_operand(context, function,
                      call->arguments[call->tensor_c_scale_argument], NULL,
                      &arguments[4], &argument_types[4])) {
      tile_release(arguments, 4);
      return 0;
    }
  } else {
    arguments[4] = ir_operand_none();
  }
  if (!ir_lower_tile_expression(context, function, d_node, NULL, &d)) {
    tile_release(arguments, 5);
    return 0;
  }
  ok = tile_emit(context, function, "mma", &d, d_type, arguments,
                 argument_types, 5, expression->location,
                 &call->tensor_mma_desc);
  ir_operand_destroy(&d);
  tile_release(arguments, 5);
  return ok;
}

int ir_lower_tile_call(IRLoweringContext *context, IRFunction *function,
                       ASTNode *expression, IROperand *out_value) {
  CallExpression *call = (CallExpression *)expression->data;
  *out_value = ir_operand_none();
  if (call->tile_builtin == TILE_BUILTIN_LOAD ||
      call->tile_builtin == TILE_BUILTIN_STORE) {
    return tile_lower_load_store(context, function, expression, call,
                                 call->tile_builtin == TILE_BUILTIN_STORE);
  }
  return tile_lower_mma(context, function, expression, call);
}

int ir_call_is_tile_statement(const CallExpression *call) {
  return call && (call->tile_builtin == TILE_BUILTIN_LOAD ||
                  call->tile_builtin == TILE_BUILTIN_STORE ||
                  (call->is_tensor_mma && call->tensor_tile_mask));
}
