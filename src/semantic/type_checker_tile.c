#include "type_checker_internal.h"
#include <stdio.h>
#include <string.h>

int type_checker_is_tile(const Type *type) {
  return type && type->kind == TYPE_SLICE && type->base_type &&
         type->view_extents[0] > 0 &&
         (type->view_layout == VIEW_LAYOUT_FRAGMENT_A ||
          type->view_layout == VIEW_LAYOUT_FRAGMENT_B ||
          type->view_layout == VIEW_LAYOUT_FRAGMENT_C);
}

int type_checker_is_row_vector(const Type *type) {
  return type_checker_is_tile(type) && type->view_rank == 1;
}

static int tile_is_matrix(const Type *type) {
  return type_checker_is_tile(type) && type->view_rank == 2;
}

const char *type_checker_tile_layout_word(unsigned char layout) {
  switch (layout) {
  case VIEW_LAYOUT_FRAGMENT_A: return "fragment_a";
  case VIEW_LAYOUT_FRAGMENT_B: return "fragment_b";
  case VIEW_LAYOUT_FRAGMENT_C: return "fragment_c";
  default: return "?";
  }
}

Type *type_checker_row_vector_of(TypeChecker *checker, Type *element,
                                 size_t rows, const char *name) {
  Type *vector;
  if (!checker || !element || !rows || !name) {
    return NULL;
  }
  for (size_t i = 0; i < checker->type_table_count; i++) {
    Type *existing = checker->type_table[i];
    if (existing && existing->name && strcmp(existing->name, name) == 0) {
      return existing;
    }
  }
  vector = type_create(TYPE_SLICE, name);
  if (!vector) {
    return NULL;
  }
  vector->base_type = element;
  vector->view_rank = 1;
  vector->view_extents[0] = rows;
  vector->view_layout = VIEW_LAYOUT_FRAGMENT_C;
  vector->size = 8;
  vector->alignment = 8;
  if (!type_alloc_fields(vector, 1)) {
    type_destroy(vector);
    return NULL;
  }
  type_set_field(vector, 0, "data", type_checker_pointer_to(checker, element),
                 0);
  vector->field_offsets[0] = 0;
  return type_checker_canon_type(checker, vector);
}

Type *type_checker_tile_of(TypeChecker *checker, Type *element, size_t rows,
                           size_t columns, unsigned char layout) {
  char name[160];
  if (!checker || !element || !element->name || !rows) {
    return NULL;
  }
  if (columns) {
    snprintf(name, sizeof(name), "%s[%zu,%zu] layout %s", element->name, rows,
             columns, type_checker_tile_layout_word(layout));
  } else {
    snprintf(name, sizeof(name), "%s[%zu] layout fragment_c", element->name,
             rows);
  }
  return type_checker_get_type_by_name(checker, name);
}

static Type *tile_with_element(TypeChecker *checker, const Type *shape,
                               Type *element) {
  return type_checker_tile_of(checker, element, shape->view_extents[0],
                              shape->view_rank == 2 ? shape->view_extents[1]
                                                    : 0,
                              shape->view_layout);
}

static FunctionDeclaration *tile_owner(TypeChecker *checker) {
  ASTNode *node = checker ? checker->current_function_decl : NULL;
  return node && node->type == AST_FUNCTION_DECLARATION
             ? (FunctionDeclaration *)node->data
             : NULL;
}

int type_checker_in_kernel_body(TypeChecker *checker) {
  FunctionDeclaration *owner = tile_owner(checker);
  return owner && owner->is_kernel;
}

static int tile_refuse_outside_kernel(TypeChecker *checker,
                                      SourceLocation location,
                                      const char *what) {
  if (type_checker_in_kernel_body(checker)) {
    return 0;
  }
  type_checker_set_error_at_location(
      checker, location,
      "%s is a subgroup's value held in registers, so it exists only in a "
      "kernel body",
      what);
  return 1;
}

static int tile_element_kind_in(const Type *element, const TypeKind *kinds,
                                size_t count) {
  for (size_t i = 0; element && i < count; i++) {
    if (element->kind == kinds[i]) {
      return 1;
    }
  }
  return 0;
}

int type_checker_check_tile_type(TypeChecker *checker, Type *type,
                                 SourceLocation location) {
  static const TypeKind operand_elements[] = {TYPE_FLOAT16, TYPE_BFLOAT16};
  static const TypeKind accumulator_elements[] = {TYPE_FLOAT32, TYPE_INT32,
                                                  TYPE_BOOL};
  size_t rows;
  size_t columns;
  if (!type_checker_is_tile(type)) {
    return 1;
  }
  rows = type->view_extents[0];
  columns = type->view_rank == 2 ? type->view_extents[1] : 0;
  if (type->device_space != DEVICE_SPACE_NONE || type->declared_align) {
    type_checker_set_error_at_location(
        checker, location,
        "'%s' is held in registers, so it has no address space or alignment",
        type->name);
    return 0;
  }
  if (type->view_rank == 1) {
    if (type->view_layout != VIEW_LAYOUT_FRAGMENT_C || rows % 16 != 0 ||
        !tile_element_kind_in(type->base_type, accumulator_elements, 3)) {
      type_checker_set_error_at_location(
          checker, location,
          "a row vector is 'T[M] layout fragment_c' with M a multiple of 16 "
          "and T float32, int32 or bool; '%s' is not one",
          type->name);
      return 0;
    }
    return 1;
  }
  if (type->view_rank != 2) {
    type_checker_set_error_at_location(
        checker, location, "a tile has two extents; '%s' has %zu", type->name,
        type->view_rank);
    return 0;
  }
  if (type->view_layout == VIEW_LAYOUT_FRAGMENT_C) {
    if (rows % 16 != 0 || columns % 8 != 0 ||
        !tile_element_kind_in(type->base_type, accumulator_elements, 3)) {
      type_checker_set_error_at_location(
          checker, location,
          "a fragment_c tile is 'T[M, N] layout fragment_c' with M a "
          "multiple of 16, N a multiple of 8, and T float32, int32 or bool; "
          "'%s' is not one",
          type->name);
      return 0;
    }
    return 1;
  }
  if (rows % 16 != 0 || columns % (type->view_layout == VIEW_LAYOUT_FRAGMENT_A
                                       ? 16u
                                       : 8u) != 0 ||
      !tile_element_kind_in(type->base_type, operand_elements, 2)) {
    type_checker_set_error_at_location(
        checker, location,
        "a %s tile holds float16 or bfloat16 with its first extent a "
        "multiple of 16 and its second a multiple of %u; '%s' is not one",
        type_checker_tile_layout_word(type->view_layout),
        type->view_layout == VIEW_LAYOUT_FRAGMENT_A ? 16u : 8u, type->name);
    return 0;
  }
  return 1;
}

int type_checker_check_tile_declaration(TypeChecker *checker,
                                        ASTNode *declaration,
                                        VarDeclaration *var_decl,
                                        Scope *scope, Type *type) {
  if (!type_checker_is_tile(type)) {
    return 1;
  }
  if (!type_checker_in_kernel_body(checker) || !scope ||
      scope->type == SCOPE_GLOBAL) {
    type_checker_set_error_at_location(
        checker, declaration->location,
        "tile '%s' is a subgroup's value held in registers, so it exists "
        "only in a kernel body",
        var_decl->name);
    return 0;
  }
  if (var_decl->is_const || var_decl->is_extern || var_decl->is_exported ||
      var_decl->address_space != AST_ADDRESS_SPACE_DEFAULT) {
    type_checker_set_error_at_location(
        checker, declaration->location,
        "tile '%s' is a register value: a plain local 'var', with no "
        "address space",
        var_decl->name);
    return 0;
  }
  return type_checker_check_tile_type(checker, type, declaration->location);
}

static int tile_scalar_literal(const ASTNode *node) {
  if (!node) {
    return 0;
  }
  if (node->type == AST_NUMBER_LITERAL) {
    return 1;
  }
  if (node->type == AST_UNARY_EXPRESSION) {
    const UnaryExpression *unary = (const UnaryExpression *)node->data;
    return unary && unary->operator && strcmp(unary->operator, "-") == 0 &&
           unary->operand && unary->operand->type == AST_NUMBER_LITERAL;
  }
  return 0;
}

int type_checker_tensor_c_is_zero_literal(const ASTNode *node) {
  const NumberLiteral *literal;
  if (!node || node->type != AST_NUMBER_LITERAL) {
    return 0;
  }
  literal = (const NumberLiteral *)node->data;
  if (!literal) {
    return 0;
  }
  return (literal->is_float ? literal->float_value
                            : (double)literal->int_value) == 0.0;
}

static int tile_scalar_fits(Type *element, Type *scalar, ASTNode *node) {
  if (!element || !scalar || type_checker_is_tile(scalar)) {
    return 0;
  }
  if (type_checker_types_equal(element, scalar)) {
    return 1;
  }
  if (element->kind == TYPE_BOOL) {
    return scalar->kind == TYPE_BOOL;
  }
  if (tile_scalar_literal(node)) {
    return type_checker_is_numeric_type(scalar) &&
           type_checker_is_numeric_type(element);
  }
  return 0;
}

int type_checker_tile_assignable(TypeChecker *checker, Type *dest,
                                 Type *value, ASTNode *value_expr) {
  (void)checker;
  if (!type_checker_is_tile(dest) && !type_checker_is_tile(value)) {
    return -1;
  }
  if (type_checker_is_tile(dest) && type_checker_is_tile(value)) {
    return type_checker_types_equal(dest, value);
  }
  if (type_checker_is_tile(dest)) {
    return tile_scalar_fits(dest->base_type, value, value_expr);
  }
  return 0;
}

static int tile_shapes_broadcast(const Type *a, const Type *b,
                                 const Type **result) {
  if (!type_checker_is_tile(a)) {
    *result = b;
    return 1;
  }
  if (!type_checker_is_tile(b)) {
    *result = a;
    return 1;
  }
  if (a->view_rank == b->view_rank) {
    *result = a;
    return a->view_layout == b->view_layout &&
           a->view_extents[0] == b->view_extents[0] &&
           (a->view_rank == 1 || a->view_extents[1] == b->view_extents[1]);
  }
  {
    const Type *matrix = a->view_rank == 2 ? a : b;
    const Type *vector = a->view_rank == 2 ? b : a;
    *result = matrix;
    return matrix->view_layout == VIEW_LAYOUT_FRAGMENT_C &&
           matrix->view_extents[0] == vector->view_extents[0];
  }
}

static int tile_refuse_operand_layout(TypeChecker *checker, const Type *type,
                                      SourceLocation location) {
  if (type_checker_is_tile(type) &&
      type->view_layout != VIEW_LAYOUT_FRAGMENT_C) {
    type_checker_set_error_at_location(
        checker, location,
        "element-wise math runs on fragment_c tiles and row vectors, and "
        "'%s' is laid out %s; compute in a fragment_c tile and cast it to "
        "%s",
        type->name, type_checker_tile_layout_word(type->view_layout),
        type_checker_tile_layout_word(type->view_layout));
    return 1;
  }
  return 0;
}

static void tile_report_shape_mismatch(TypeChecker *checker,
                                       SourceLocation location,
                                       const Type *a, const Type *b) {
  type_checker_set_error_at_location(
      checker, location,
      "'%s' and '%s' do not combine element by element: two tiles need the "
      "same extents and layout, and a row vector needs the tile's row count",
      a && a->name ? a->name : "?", b && b->name ? b->name : "?");
}

static Type *tile_element_of(const Type *type) {
  return type_checker_is_tile(type) ? type->base_type : (Type *)type;
}

static int tile_check_elements(TypeChecker *checker, SourceLocation location,
                               Type *left, ASTNode *left_node, Type *right,
                               ASTNode *right_node, Type **element) {
  Type *le = tile_element_of(left);
  Type *re = tile_element_of(right);
  if (type_checker_is_tile(left) && type_checker_is_tile(right)) {
    if (!type_checker_types_equal(le, re)) {
      type_checker_set_error_at_location(
          checker, location,
          "'%s' holds %s and '%s' holds %s; cast one tile so both hold the "
          "same element",
          left->name, le->name, right->name, re->name);
      return 0;
    }
    *element = le;
    return 1;
  }
  if (type_checker_is_tile(left)) {
    if (!tile_scalar_fits(le, right, right_node)) {
      type_checker_set_error_at_location(
          checker, right_node ? right_node->location : location,
          "a scalar combined with '%s' must be a %s or a literal; this is %s",
          left->name, le->name, right && right->name ? right->name : "?");
      return 0;
    }
    *element = le;
    return 1;
  }
  if (!tile_scalar_fits(re, left, left_node)) {
    type_checker_set_error_at_location(
        checker, left_node ? left_node->location : location,
        "a scalar combined with '%s' must be a %s or a literal; this is %s",
        right->name, re->name, left && left->name ? left->name : "?");
    return 0;
  }
  *element = re;
  return 1;
}

Type *type_checker_tile_binary(TypeChecker *checker, BinaryExpression *binop,
                               Type *left, Type *right,
                               SourceLocation location) {
  const char *op = binop->operator;
  const Type *shape = NULL;
  Type *element = NULL;
  int arithmetic = !strcmp(op, "+") || !strcmp(op, "-") || !strcmp(op, "*") ||
                   !strcmp(op, "/");
  int comparison = !strcmp(op, "<") || !strcmp(op, "<=") || !strcmp(op, ">") ||
                   !strcmp(op, ">=") || !strcmp(op, "==") || !strcmp(op, "!=");
  int logical = !strcmp(op, "&&") || !strcmp(op, "||");
  if (tile_refuse_outside_kernel(checker, location, "a tile")) {
    return NULL;
  }
  if (tile_refuse_operand_layout(checker, left, binop->left->location) ||
      tile_refuse_operand_layout(checker, right, binop->right->location)) {
    return NULL;
  }
  if (!arithmetic && !comparison && !logical) {
    type_checker_set_error_at_location(
        checker, location,
        "'%s' does not apply to tiles; tiles take + - * /, comparisons, && "
        "and ||",
        op);
    return NULL;
  }
  if (!tile_shapes_broadcast(left, right, &shape)) {
    tile_report_shape_mismatch(checker, location, left, right);
    return NULL;
  }
  if (!tile_check_elements(checker, location, left, binop->left, right,
                           binop->right, &element)) {
    return NULL;
  }
  if (arithmetic && (element->kind == TYPE_BOOL ||
                     !type_checker_is_numeric_type(element))) {
    type_checker_set_error_at_location(
        checker, location, "'%s' needs numeric elements; these are %s", op,
        element->name);
    return NULL;
  }
  if (logical && element->kind != TYPE_BOOL) {
    type_checker_set_error_at_location(
        checker, location, "'%s' needs bool elements; these are %s", op,
        element->name);
    return NULL;
  }
  if (comparison) {
    return tile_with_element(checker, shape, checker->builtin_bool);
  }
  return tile_with_element(checker, shape, element);
}

Type *type_checker_tile_unary(TypeChecker *checker, const char *op,
                              Type *operand, SourceLocation location) {
  if (tile_refuse_outside_kernel(checker, location, "a tile") ||
      tile_refuse_operand_layout(checker, operand, location)) {
    return NULL;
  }
  if (!strcmp(op, "-") && operand->base_type->kind != TYPE_BOOL) {
    return operand;
  }
  if (!strcmp(op, "!") && operand->base_type->kind == TYPE_BOOL) {
    return operand;
  }
  if (!strcmp(op, "&")) {
    type_checker_set_error_at_location(
        checker, location,
        "'%s' is held in registers and has no address", operand->name);
    return NULL;
  }
  type_checker_set_error_at_location(
      checker, location, "unary '%s' does not apply to '%s'", op,
      operand->name);
  return NULL;
}

Type *type_checker_tile_cast(TypeChecker *checker, ASTNode *expression,
                             Type *operand, Type *target) {
  if (tile_refuse_outside_kernel(checker, expression->location, "a tile")) {
    return NULL;
  }
  if (!type_checker_is_tile(operand) || !type_checker_is_tile(target)) {
    type_checker_set_error_at_location(
        checker, expression->location,
        "a cast moves a tile to another tile; '%s' to '%s' is not one",
        operand && operand->name ? operand->name : "?",
        target && target->name ? target->name : "?");
    return NULL;
  }
  if (!type_checker_check_tile_type(checker, target, expression->location)) {
    return NULL;
  }
  if (operand->view_rank != target->view_rank ||
      operand->view_extents[0] != target->view_extents[0] ||
      (operand->view_rank == 2 &&
       operand->view_extents[1] != target->view_extents[1])) {
    type_checker_set_error_at_location(
        checker, expression->location,
        "a cast keeps a tile's extents; '%s' and '%s' differ", operand->name,
        target->name);
    return NULL;
  }
  if (operand->view_layout == target->view_layout) {
    if (operand->view_layout != VIEW_LAYOUT_FRAGMENT_C) {
      type_checker_set_error_at_location(
          checker, expression->location,
          "a %s tile is converted from a fragment_c tile; cast the "
          "accumulator",
          type_checker_tile_layout_word(operand->view_layout));
      return NULL;
    }
    return target;
  }
  if (operand->view_layout == VIEW_LAYOUT_FRAGMENT_C &&
      target->view_layout == VIEW_LAYOUT_FRAGMENT_A &&
      operand->view_rank == 2 && operand->base_type->kind == TYPE_FLOAT32) {
    return target;
  }
  type_checker_set_error_at_location(
      checker, expression->location,
      "'%s' is laid out %s and '%s' is laid out %s; the only change of "
      "layout a cast makes is a float32 fragment_c tile into a fragment_a "
      "operand",
      operand->name, type_checker_tile_layout_word(operand->view_layout),
      target->name, type_checker_tile_layout_word(target->view_layout));
  return NULL;
}

static ASTNode *tile_named_argument(CallExpression *call, const char *name,
                                    size_t from) {
  for (size_t i = from; i < call->argument_count; i++) {
    if (call->argument_names && call->argument_names[i] &&
        strcmp(call->argument_names[i], name) == 0) {
      return call->arguments[i];
    }
  }
  return NULL;
}

static int tile_check_named_arguments(TypeChecker *checker, CallExpression *call,
                                      size_t positional, const char *const *allowed,
                                      size_t allowed_count) {
  for (size_t i = 0; i < call->argument_count; i++) {
    const char *name = call->argument_names ? call->argument_names[i] : NULL;
    if (i < positional) {
      if (name) {
        type_checker_set_error_at_location(
            checker, call->arguments[i]->location,
            "'%s' takes %zu positional operands first", call->function_name,
            positional);
        return 0;
      }
      continue;
    }
    if (!name) {
      type_checker_set_error_at_location(
          checker, call->arguments[i]->location,
          "'%s' takes %zu positional operands; the rest are named",
          call->function_name, positional);
      return 0;
    }
    int known = 0;
    for (size_t a = 0; a < allowed_count; a++) {
      if (strcmp(name, allowed[a]) == 0) {
        known = 1;
      }
    }
    if (!known) {
      type_checker_set_error_at_location(
          checker, call->arguments[i]->location,
          "'%s' has no option '%s'", call->function_name, name);
      return 0;
    }
  }
  if (call->argument_count < positional) {
    type_checker_set_error_at_location(
        checker, call->arguments[0] ? call->arguments[0]->location
                                    : (SourceLocation){0},
        "'%s' takes %zu positional operands", call->function_name,
        positional);
    return 0;
  }
  return 1;
}

static int tile_integer_option(TypeChecker *checker, ASTNode *value,
                               const char *what) {
  Type *type;
  if (!value) {
    return 1;
  }
  type = type_checker_infer_type(checker, value);
  if (!type || !type_checker_is_integer_type(type)) {
    type_checker_set_error_at_location(
        checker, value->location, "%s must be an integer", what);
    return 0;
  }
  return 1;
}

static int tile_is_memory_operand(const Type *type) {
  return type && (type->kind == TYPE_POINTER ||
                  (type->kind == TYPE_SLICE && type->view_extents[0] > 0 &&
                   !type_checker_is_tile(type)));
}

static int tile_converts(const Type *from, const Type *to) {
  if (!from || !to) {
    return 0;
  }
  if (from->kind == to->kind) {
    return 1;
  }
  return (to->kind == TYPE_FLOAT16 || to->kind == TYPE_BFLOAT16 ||
          to->kind == TYPE_FLOAT32) &&
         (from->kind == TYPE_FLOAT32 || from->kind == TYPE_FLOAT16 ||
          from->kind == TYPE_BFLOAT16);
}

static int tile_check_memory(TypeChecker *checker, ASTNode *node, Type *type,
                             Type *tile, int store, ASTNode *ld) {
  Type *element;
  if (!tile_is_memory_operand(type)) {
    type_checker_set_error_at_location(
        checker, node->location,
        "a tile %s memory through a pointer or a shaped view; this is '%s'",
        store ? "goes to" : "comes from",
        type && type->name ? type->name : "?");
    return 0;
  }
  element = type->base_type;
  if (store ? !tile_converts(tile->base_type, element)
            : !tile_converts(element, tile->base_type)) {
    type_checker_set_error_at_location(
        checker, node->location,
        "'%s' holds %s and this memory holds %s; a tile moves to and from "
        "memory of its own element, or between float32, float16 and bfloat16",
        tile->name, tile->base_type->name, element ? element->name : "?");
    return 0;
  }
  if (type->kind == TYPE_POINTER && !ld) {
    type_checker_set_error_at_location(
        checker, node->location,
        "through a plain pointer the row stride is not in the type; pass "
        "ld: elements");
    return 0;
  }
  if (type->kind == TYPE_SLICE && type->view_rank == 2 && tile->view_rank == 2 &&
      (type->view_extents[0] < tile->view_extents[0] ||
       type->view_extents[1] < tile->view_extents[1])) {
    type_checker_set_error_at_location(
        checker, node->location,
        "'%s' is smaller than '%s'", type->name, tile->name);
    return 0;
  }
  return 1;
}

static Type *tile_load_store(TypeChecker *checker, ASTNode *expression,
                             CallExpression *call, int store) {
  static const char *const options[] = {"ld", "rows"};
  ASTNode *tile_node;
  ASTNode *memory_node;
  Type *tile;
  Type *memory;
  ASTNode *ld;
  if (!tile_check_named_arguments(checker, call, 2, options, 2)) {
    return NULL;
  }
  tile_node = call->arguments[store ? 1 : 0];
  memory_node = call->arguments[store ? 0 : 1];
  tile = type_checker_infer_type(checker, tile_node);
  memory = type_checker_infer_type(checker, memory_node);
  if (!tile || !memory) {
    return NULL;
  }
  if (!type_checker_is_tile(tile)) {
    type_checker_set_error_at_location(
        checker, tile_node->location, "%s needs a tile here; this is '%s'",
        call->function_name, tile->name ? tile->name : "?");
    return NULL;
  }
  if (!store && tile_node->type != AST_IDENTIFIER) {
    type_checker_set_error_at_location(
        checker, tile_node->location,
        "tile_load writes a tile variable; name one");
    return NULL;
  }
  if (tile->view_layout == VIEW_LAYOUT_FRAGMENT_B) {
    type_checker_set_error_at_location(
        checker, tile_node->location,
        "a fragment_b operand stays in memory and tensor_mma reads it there");
    return NULL;
  }
  if (store && tile->view_layout != VIEW_LAYOUT_FRAGMENT_C) {
    type_checker_set_error_at_location(
        checker, tile_node->location,
        "tile_store writes a fragment_c tile or a row vector; '%s' is an "
        "operand",
        tile->name);
    return NULL;
  }
  ld = tile_named_argument(call, "ld", 2);
  if (tile->view_rank == 1 && !ld) {
    type_checker_set_error_at_location(
        checker, expression->location,
        "a row vector moves element i at ld * i; pass ld: elements");
    return NULL;
  }
  if (!tile_check_memory(checker, memory_node, memory, tile, store, ld) ||
      !tile_integer_option(checker, ld, "ld") ||
      !tile_integer_option(checker, tile_named_argument(call, "rows", 2),
                           "rows")) {
    return NULL;
  }
  call->tile_builtin = store ? TILE_BUILTIN_STORE : TILE_BUILTIN_LOAD;
  return checker->builtin_void;
}

static Type *tile_index_builtin(TypeChecker *checker, ASTNode *expression,
                                CallExpression *call, int column) {
  Type *tile;
  if (call->argument_count != 1 ||
      (call->argument_names && call->argument_names[0])) {
    type_checker_set_error_at_location(
        checker, expression->location, "%s takes one tile",
        call->function_name);
    return NULL;
  }
  tile = type_checker_infer_type(checker, call->arguments[0]);
  if (!tile) {
    return NULL;
  }
  if (!type_checker_is_tile(tile) ||
      tile->view_layout != VIEW_LAYOUT_FRAGMENT_C ||
      (column && tile->view_rank != 2)) {
    type_checker_set_error_at_location(
        checker, call->arguments[0]->location,
        "%s gives each element's %s of a fragment_c tile%s; this is '%s'",
        call->function_name, column ? "column" : "row",
        column ? "" : " or row vector", tile->name ? tile->name : "?");
    return NULL;
  }
  call->tile_builtin = column ? TILE_BUILTIN_COL : TILE_BUILTIN_ROW;
  return tile_with_element(checker, tile, checker->builtin_int32);
}

static Type *tile_reduce_builtin(TypeChecker *checker, ASTNode *expression,
                                 CallExpression *call, int sum) {
  Type *tile;
  if (call->argument_count != 1 ||
      (call->argument_names && call->argument_names[0])) {
    type_checker_set_error_at_location(
        checker, expression->location, "%s takes one tile",
        call->function_name);
    return NULL;
  }
  tile = type_checker_infer_type(checker, call->arguments[0]);
  if (!tile) {
    return NULL;
  }
  if (!tile_is_matrix(tile) || tile->view_layout != VIEW_LAYOUT_FRAGMENT_C ||
      (tile->base_type->kind != TYPE_FLOAT32 &&
       tile->base_type->kind != TYPE_INT32)) {
    type_checker_set_error_at_location(
        checker, call->arguments[0]->location,
        "%s reduces a float32 or int32 fragment_c tile along its rows; this "
        "is '%s'",
        call->function_name, tile->name ? tile->name : "?");
    return NULL;
  }
  call->tile_builtin = sum ? TILE_BUILTIN_ROW_SUM : TILE_BUILTIN_ROW_MAX;
  return type_checker_tile_of(checker, tile->base_type, tile->view_extents[0],
                              0, VIEW_LAYOUT_FRAGMENT_C);
}

static int tile_math_name(const char *name) {
  static const char *const names[] = {"expf", "sqrtf", "rsqrtf", "fabsf",
                                      "logf", "sinf",  "cosf"};
  for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
    if (strcmp(name, names[i]) == 0) {
      return 1;
    }
  }
  return 0;
}

static Type *tile_elementwise_builtin(TypeChecker *checker, ASTNode *expression,
                                      CallExpression *call) {
  const char *name = call->function_name;
  int is_select = strcmp(name, "select") == 0;
  int is_minmax = strcmp(name, "max") == 0 || strcmp(name, "min") == 0;
  size_t want = is_select ? 3u : is_minmax ? 2u : 1u;
  Type *types[3] = {NULL, NULL, NULL};
  const Type *shape = NULL;
  Type *element = NULL;
  if (call->argument_count != want) {
    type_checker_set_error_at_location(
        checker, expression->location, "%s takes %zu operands", name, want);
    return NULL;
  }
  for (size_t i = 0; i < want; i++) {
    if (call->argument_names && call->argument_names[i]) {
      type_checker_set_error_at_location(
          checker, call->arguments[i]->location, "%s takes positional operands",
          name);
      return NULL;
    }
    types[i] = type_checker_infer_type(checker, call->arguments[i]);
    if (!types[i]) {
      return NULL;
    }
    if (tile_refuse_operand_layout(checker, types[i],
                                   call->arguments[i]->location)) {
      return NULL;
    }
  }
  shape = types[0];
  for (size_t i = 1; i < want; i++) {
    const Type *next = NULL;
    if (!tile_shapes_broadcast(shape, types[i], &next)) {
      tile_report_shape_mismatch(checker, expression->location, shape,
                                 types[i]);
      return NULL;
    }
    shape = next;
  }
  if (!type_checker_is_tile(shape)) {
    return NULL;
  }
  if (is_select) {
    Type *condition = tile_element_of(types[0]);
    if (!condition || condition->kind != TYPE_BOOL) {
      type_checker_set_error_at_location(
          checker, call->arguments[0]->location,
          "select chooses by a bool tile, row vector or bool; this is %s",
          types[0]->name ? types[0]->name : "?");
      return NULL;
    }
    if (!tile_check_elements(checker, expression->location, types[1],
                             call->arguments[1], types[2], call->arguments[2],
                             &element)) {
      return NULL;
    }
    if (!type_checker_is_tile(types[1]) && !type_checker_is_tile(types[2])) {
      element = tile_element_of(types[1]);
      if (tile_scalar_literal(call->arguments[1]) &&
          tile_scalar_literal(call->arguments[2])) {
        type_checker_set_error_at_location(
            checker, expression->location,
            "select between two literals has no element type; make one a "
            "tile or a typed scalar");
        return NULL;
      }
    }
    call->tile_builtin = TILE_BUILTIN_SELECT;
    return tile_with_element(checker, shape, element);
  }
  if (is_minmax) {
    if (!tile_check_elements(checker, expression->location, types[0],
                             call->arguments[0], types[1], call->arguments[1],
                             &element)) {
      return NULL;
    }
    if (element->kind != TYPE_FLOAT32 && element->kind != TYPE_INT32) {
      type_checker_set_error_at_location(
          checker, expression->location,
          "%s compares float32 or int32 elements; these are %s", name,
          element->name);
      return NULL;
    }
    call->tile_builtin = strcmp(name, "max") == 0 ? TILE_BUILTIN_MAX
                                                  : TILE_BUILTIN_MIN;
    return tile_with_element(checker, shape, element);
  }
  if (types[0]->base_type->kind != TYPE_FLOAT32) {
    type_checker_set_error_at_location(
        checker, call->arguments[0]->location,
        "%s applies to float32 elements; '%s' holds %s", name, types[0]->name,
        types[0]->base_type->name);
    return NULL;
  }
  call->tile_builtin = TILE_BUILTIN_MATH;
  return (Type *)shape;
}

static int tile_any_argument(TypeChecker *checker, CallExpression *call) {
  for (size_t i = 0; i < call->argument_count; i++) {
    if (call->argument_names && call->argument_names[i]) {
      continue;
    }
    Type *type = type_checker_infer_type(checker, call->arguments[i]);
    if (type_checker_is_tile(type)) {
      return 1;
    }
  }
  return 0;
}

Type *type_checker_tile_builtin(TypeChecker *checker, ASTNode *expression,
                                CallExpression *call, int *handled) {
  const char *name;
  *handled = 0;
  if (!call || !call->function_name || call->object) {
    return NULL;
  }
  name = call->function_name;
  if (strcmp(name, "tile_load") == 0 || strcmp(name, "tile_store") == 0 ||
      strcmp(name, "tile_row") == 0 || strcmp(name, "tile_col") == 0 ||
      strcmp(name, "row_max") == 0 || strcmp(name, "row_sum") == 0) {
    if (!tile_any_argument(checker, call)) {
      return NULL;
    }
    *handled = 1;
    if (tile_refuse_outside_kernel(checker, expression->location, name)) {
      return NULL;
    }
    if (strcmp(name, "tile_load") == 0) {
      return tile_load_store(checker, expression, call, 0);
    }
    if (strcmp(name, "tile_store") == 0) {
      return tile_load_store(checker, expression, call, 1);
    }
    if (strcmp(name, "tile_row") == 0 || strcmp(name, "tile_col") == 0) {
      return tile_index_builtin(checker, expression, call,
                                strcmp(name, "tile_col") == 0);
    }
    return tile_reduce_builtin(checker, expression, call,
                               strcmp(name, "row_sum") == 0);
  }
  if ((strcmp(name, "select") != 0 && strcmp(name, "max") != 0 &&
       strcmp(name, "min") != 0 && !tile_math_name(name)) ||
      !tile_any_argument(checker, call)) {
    return NULL;
  }
  *handled = 1;
  if (tile_refuse_outside_kernel(checker, expression->location, "a tile")) {
    return NULL;
  }
  return tile_elementwise_builtin(checker, expression, call);
}

int type_checker_refuse_tile_argument(TypeChecker *checker, ASTNode *argument,
                                      Type *type, const char *callee) {
  if (!type_checker_is_tile(type)) {
    return 0;
  }
  type_checker_set_error_at_location(
      checker, argument->location,
      "'%s' cannot take '%s': a tile is a subgroup's value in registers and "
      "does not cross a call",
      callee ? callee : "this function", type->name);
  return 1;
}

int type_checker_tile_mma_operands(TypeChecker *checker, ASTNode *expression,
                                   CallExpression *call,
                                   const MtlcTensorMmaDesc *desc,
                                   Type **operand_types) {
  static const char *const names[4] = {"A", "B", "C", "D"};
  int any = 0;
  call->tensor_c_zero = 0;
  call->tensor_tile_mask = 0;
  if (tile_scalar_literal(call->arguments[2])) {
    double value = 1.0;
    ASTNode *node = call->arguments[2];
    if (node->type == AST_NUMBER_LITERAL) {
      NumberLiteral *literal = (NumberLiteral *)node->data;
      value = literal->is_float ? literal->float_value
                                : (double)literal->int_value;
    } else {
      value = -1.0;
    }
    if (value != 0.0 || node->type != AST_NUMBER_LITERAL) {
      type_checker_set_error_at_location(
          checker, node->location,
          "a constant C is 0.0, an accumulator that starts at +0.0; this is "
          "another constant");
      return 0;
    }
    call->tensor_c_zero = 1;
  }
  for (size_t i = 0; i < 4; i++) {
    if (type_checker_is_tile(operand_types[i])) {
      call->tensor_tile_mask |= 1u << i;
      any = 1;
    }
  }
  if (!any) {
    return 1;
  }
  if (tile_refuse_outside_kernel(checker, expression->location, "a tile")) {
    return 0;
  }
  if (call->tensor_tile_mask & 2u) {
    type_checker_set_error_at_location(
        checker, call->arguments[1]->location,
        "tensor_mma reads B from memory; pass a pointer or a shaped view");
    return 0;
  }
  if (!(call->tensor_tile_mask & 8u)) {
    type_checker_set_error_at_location(
        checker, call->arguments[3]->location,
        "a tile operand accumulates into a tile; D must be a fragment_c tile");
    return 0;
  }
  if (call->arguments[3]->type != AST_IDENTIFIER) {
    type_checker_set_error_at_location(
        checker, call->arguments[3]->location,
        "tensor_mma writes its D tile; name a tile variable");
    return 0;
  }
  if (!call->tensor_c_zero && !(call->tensor_tile_mask & 4u)) {
    type_checker_set_error_at_location(
        checker, call->arguments[2]->location,
        "with a D tile, C is a tile of the same type or 0.0");
    return 0;
  }
  if (desc->sparsity != MTLC_TENSOR_SPARSITY_DENSE ||
      desc->a_scale_mode != MTLC_TENSOR_SCALE_NONE ||
      desc->b_scale_mode != MTLC_TENSOR_SCALE_NONE || desc->transpose_a ||
      desc->transpose_b ||
      (desc->a_element != MTLC_TENSOR_ELEMENT_FLOAT16 &&
       desc->a_element != MTLC_TENSOR_ELEMENT_BFLOAT16) ||
      desc->b_element != desc->a_element ||
      desc->accumulator_element != MTLC_TENSOR_ELEMENT_FLOAT32 ||
      desc->result_element != MTLC_TENSOR_ELEMENT_FLOAT32 ||
      desc->scope != MTLC_MEMORY_SCOPE_SUBGROUP) {
    type_checker_set_error_at_location(
        checker, expression->location,
        "a tile MMA is dense f16 or bf16 A and B with an f32 accumulator, "
        "subgroup scoped; this descriptor asks for more");
    return 0;
  }
  {
    Type *operand_element = desc->a_element == MTLC_TENSOR_ELEMENT_FLOAT16
                                ? checker->builtin_float16
                                : type_checker_get_type_by_name(checker,
                                                                "bfloat16");
    Type *want[4] = {
        type_checker_tile_of(checker, operand_element, desc->m, desc->k,
                             VIEW_LAYOUT_FRAGMENT_A),
        NULL,
        type_checker_tile_of(checker, checker->builtin_float32, desc->m,
                             desc->n, VIEW_LAYOUT_FRAGMENT_C),
        type_checker_tile_of(checker, checker->builtin_float32, desc->m,
                             desc->n, VIEW_LAYOUT_FRAGMENT_C)};
    for (size_t i = 0; i < 4; i++) {
      if (!(call->tensor_tile_mask & (1u << i))) {
        continue;
      }
      if (!want[i] || !type_checker_types_equal(want[i], operand_types[i])) {
        type_checker_set_error_at_location(
            checker, call->arguments[i]->location,
            "tensor_mma's %s operand for this descriptor is '%s'; this is "
            "'%s'",
            names[i], want[i] && want[i]->name ? want[i]->name : "?",
            operand_types[i]->name ? operand_types[i]->name : "?");
        return 0;
      }
    }
  }
  if (desc->c_scale_mode != MTLC_TENSOR_SCALE_NONE &&
      call->tensor_c_scale_argument != SIZE_MAX) {
    ASTNode *scale = call->arguments[call->tensor_c_scale_argument];
    Type *scale_type = type_checker_infer_type(checker, scale);
    if (type_checker_is_tile(scale_type)) {
      Type *want = type_checker_tile_of(checker, checker->builtin_float32,
                                        desc->m, 0, VIEW_LAYOUT_FRAGMENT_C);
      if (!type_checker_types_equal(want, scale_type)) {
        type_checker_set_error_at_location(
            checker, scale->location,
            "the row scale of a %u-row tile is '%s'; this is '%s'",
            (unsigned)desc->m, want ? want->name : "?", scale_type->name);
        return 0;
      }
      call->tensor_tile_mask |= 16u;
    }
  }
  return 1;
}

static int view_element_matches(const Type *pointer_element,
                                const Type *view_element) {
  if (!pointer_element || !view_element) {
    return 0;
  }
  if (pointer_element->kind == view_element->kind) {
    return 1;
  }
  return pointer_element->kind == TYPE_UINT16 &&
         (view_element->kind == TYPE_FLOAT16 ||
          view_element->kind == TYPE_BFLOAT16);
}

Type *type_checker_view_cast(TypeChecker *checker, ASTNode *expression,
                             ASTNode *operand, Type *pointer, Type *view) {
  size_t reached;
  if (view->view_rank != 2 || type_checker_is_tile(view)) {
    type_checker_set_error_at_location(
        checker, expression->location,
        "a pointer becomes a shaped view of two extents; '%s' is not one",
        view->name);
    return NULL;
  }
  if (!view_element_matches(pointer->base_type, view->base_type)) {
    type_checker_set_error_at_location(
        checker, expression->location,
        "'%s' points at %s and '%s' holds %s; a view keeps the element its "
        "memory holds",
        pointer->name, pointer->base_type ? pointer->base_type->name : "?",
        view->name, view->base_type->name);
    return NULL;
  }
  if (pointer->device_space != DEVICE_SPACE_NONE &&
      pointer->device_space != view->device_space) {
    type_checker_set_error_at_location(
        checker, expression->location,
        "'%s' and '%s' name different memories", pointer->name, view->name);
    return NULL;
  }
  reached = type_checker_address_alignment(checker, operand, 0);
  if (reached < 16) {
    type_checker_set_error_at_location(
        checker, expression->location,
        "a shaped view starts on a 16-byte boundary, so its rows load as "
        "whole 16-byte groups; this address is %zu-byte aligned",
        reached);
    return NULL;
  }
  if ((view->view_extents[1] * (view->base_type->size ? view->base_type->size
                                                       : 1)) %
          16 !=
      0) {
    type_checker_set_error_at_location(
        checker, expression->location,
        "a shaped view's rows are whole 16-byte groups; a row of '%s' is "
        "not",
        view->name);
    return NULL;
  }
  return view;
}
