#include "msl_interp_compile.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *const msl_reserved[] = {
  "struct", "static", "kernel", "if", "else", "while", "for", "switch", "default", "break", "continue", "return",
  "true", "false", "nullptr", "const", "device", "constant", "threadgroup", "thread", "alignas", "using",
  "namespace", "sizeof", "static_assert", "coherent", "void", "bool", "char", "uchar", "short", "ushort",
  "int", "uint", "long", "ulong", "half", "bfloat", "float", "double", "atomic_uint", "simd_vote", "case",
  "do", "goto", "as_type", "reinterpret_cast", "os_log_default", "precise", "fast", "mem_flags"
};

static const char *const msl_scalar_keywords[MSL_S_COUNT] = {
  "bool", "char", "uchar", "short", "ushort", "int", "uint", "long", "ulong", "half", "bfloat", "float"
};

void mslc_fail(MslCompiler *c, int line, const char *fmt, ...) {
  char message[512];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(message, sizeof message, fmt, ap);
  va_end(ap);
  msl_format_error(c->error, c->error_size, "line %d: %s", line, message);
  longjmp(c->fail, 1);
}

void mslc_enter(MslCompiler *c, int line) {
  if (++c->nesting > 256) {
    mslc_fail(c, line, "nesting deeper than 256 levels");
  }
}

void mslc_leave(MslCompiler *c) {
  c->nesting--;
}

const MslToken *mslc_cur(MslCompiler *c) {
  return &c->toks[c->pos];
}

const MslToken *mslc_ahead(MslCompiler *c, size_t k) {
  size_t index = c->pos + k;
  if (index >= c->count) {
    index = c->count - 1;
  }
  return &c->toks[index];
}

int mslc_tok_is(const MslToken *tok, const char *text) {
  size_t n = strlen(text);
  return (tok->kind == MSL_TK_IDENT || tok->kind == MSL_TK_PUNCT) && tok->len == n && memcmp(tok->text, text, n) == 0;
}

int mslc_at(MslCompiler *c, const char *text) {
  return mslc_tok_is(mslc_cur(c), text);
}

int mslc_accept(MslCompiler *c, const char *text) {
  if (mslc_at(c, text)) {
    if (c->pos + 1 < c->count) {
      c->pos++;
    }
    return 1;
  }
  return 0;
}

static void token_text(const MslToken *tok, char *buffer, size_t size) {
  size_t n = tok->len < size - 1 ? tok->len : size - 1;
  memcpy(buffer, tok->text, n);
  buffer[n] = 0;
}

void mslc_expect(MslCompiler *c, const char *text) {
  char found[64];
  if (mslc_accept(c, text)) {
    return;
  }
  token_text(mslc_cur(c), found, sizeof found);
  mslc_fail(c, mslc_cur(c)->line, "expected '%s' but found '%s'", text, found);
}

void *mslc_alloc(MslCompiler *c, size_t size) {
  void *out = msl_arena_alloc(c->prog, size);
  if (out == NULL) {
    mslc_fail(c, mslc_cur(c)->line, "out of memory");
  }
  return out;
}

void mslc_grow(MslCompiler *c, void **array, size_t count, size_t *cap, size_t elem) {
  size_t fresh;
  void *grown;
  if (count < *cap) {
    return;
  }
  fresh = *cap == 0 ? 16 : *cap * 2;
  grown = realloc(*array, fresh * elem);
  if (grown == NULL) {
    mslc_fail(c, mslc_cur(c)->line, "out of memory");
  }
  *array = grown;
  *cap = fresh;
}

static int is_reserved(const char *name, size_t len) {
  size_t i;
  for (i = 0; i < sizeof(msl_reserved) / sizeof(msl_reserved[0]); i++) {
    if (strlen(msl_reserved[i]) == len && memcmp(msl_reserved[i], name, len) == 0) {
      return 1;
    }
  }
  return 0;
}

const char *mslc_name(MslCompiler *c) {
  const MslToken *tok = mslc_cur(c);
  char found[64];
  token_text(tok, found, sizeof found);
  if (tok->kind != MSL_TK_IDENT) {
    mslc_fail(c, tok->line, "expected a name but found '%s'", found);
  }
  if (is_reserved(tok->text, tok->len) || msl_vector_by_name(c->prog, tok->text, tok->len) != NULL ||
      msl_matrix_by_name(c->prog, tok->text, tok->len) != NULL || msl_named_type(c->prog, tok->text, tok->len) != NULL) {
    mslc_fail(c, tok->line, "'%s' cannot be used as a name", found);
  }
  c->pos++;
  return msl_arena_strndup(c->prog, tok->text, tok->len);
}

uint64_t mslc_int_literal(MslCompiler *c) {
  const MslToken *tok = mslc_cur(c);
  if (tok->kind != MSL_TK_INT) {
    mslc_fail(c, tok->line, "expected an integer literal");
  }
  c->pos++;
  return tok->int_value;
}

size_t mslc_emit(MslCompiler *c, int op, int line) {
  MslFunction *fn = c->fn;
  MslInsn *insn;
  mslc_grow(c, (void **)&fn->code, fn->code_count, &fn->code_cap, sizeof(MslInsn));
  insn = &fn->code[fn->code_count];
  memset(insn, 0, sizeof *insn);
  insn->op = op;
  insn->line = line;
  insn->statement = c->pending_statement;
  c->pending_statement = 0;
  return fn->code_count++;
}

MslInsn *mslc_insn(MslCompiler *c, size_t index) {
  return &c->fn->code[index];
}

size_t mslc_here(MslCompiler *c) {
  return c->fn->code_count;
}

void mslc_patch(MslCompiler *c, size_t index, size_t target) {
  c->fn->code[index].a = (int64_t)target;
}

const char *mslc_tname(const MslType *type, char *buffer, size_t size) {
  msl_type_name(type, buffer, size);
  return buffer;
}

int mslc_space_keyword(const MslToken *tok) {
  if (mslc_tok_is(tok, "device")) {
    return MSL_SP_DEVICE;
  }
  if (mslc_tok_is(tok, "constant")) {
    return MSL_SP_CONSTANT;
  }
  if (mslc_tok_is(tok, "threadgroup")) {
    return MSL_SP_THREADGROUP;
  }
  if (mslc_tok_is(tok, "thread")) {
    return MSL_SP_THREAD;
  }
  return MSL_SP_NONE;
}

static const MslType *base_type(MslCompiler *c, const MslToken *tok) {
  int s;
  const MslType *type;
  if (tok->kind != MSL_TK_IDENT) {
    return NULL;
  }
  for (s = 0; s < MSL_S_COUNT; s++) {
    if (mslc_tok_is(tok, msl_scalar_keywords[s])) {
      return c->prog->scalar_types[s];
    }
  }
  if (mslc_tok_is(tok, "void")) {
    return c->prog->void_type;
  }
  if (mslc_tok_is(tok, "atomic_uint")) {
    return c->prog->atomic_type;
  }
  type = msl_vector_by_name(c->prog, tok->text, tok->len);
  if (type == NULL) {
    type = msl_matrix_by_name(c->prog, tok->text, tok->len);
  }
  if (type == NULL) {
    type = msl_named_type(c->prog, tok->text, tok->len);
  }
  return type;
}

int mslc_starts_type(MslCompiler *c, size_t offset) {
  const MslToken *tok = mslc_ahead(c, offset);
  if (tok->kind != MSL_TK_IDENT) {
    return 0;
  }
  if (mslc_tok_is(tok, "const") || mslc_tok_is(tok, "coherent") || mslc_tok_is(tok, "simd_vote") ||
      mslc_tok_is(tok, "double") || mslc_space_keyword(tok) != MSL_SP_NONE) {
    return 1;
  }
  return base_type(c, tok) != NULL;
}

static void skip_qualifiers(MslCompiler *c) {
  for (;;) {
    if (mslc_accept(c, "const")) {
      continue;
    }
    if (mslc_at(c, "coherent")) {
      c->pos++;
      mslc_expect(c, "(");
      mslc_expect(c, "device");
      mslc_expect(c, ")");
      continue;
    }
    break;
  }
}

static const MslType *parse_base(MslCompiler *c, MslTypeSpec *spec) {
  const MslToken *tok = mslc_cur(c);
  const MslType *type;
  char found[64];
  if (mslc_tok_is(tok, "simd_vote")) {
    c->pos++;
    mslc_expect(c, "::");
    mslc_expect(c, "vote_t");
    spec->vote = 1;
    return c->prog->scalar_types[MSL_S_ULONG];
  }
  if (mslc_tok_is(tok, "double")) {
    mslc_fail(c, tok->line, "double is outside the dialect");
  }
  type = base_type(c, tok);
  if (type == NULL) {
    token_text(tok, found, sizeof found);
    mslc_fail(c, tok->line, "unknown type '%s'", found);
  }
  c->pos++;
  return type;
}

static int next_pointer_space(MslCompiler *c) {
  size_t save = c->pos;
  int space;
  skip_qualifiers(c);
  space = mslc_space_keyword(mslc_cur(c));
  if (space != MSL_SP_NONE && mslc_tok_is(mslc_ahead(c, 1), "*")) {
    c->pos++;
    return space;
  }
  c->pos = save;
  return MSL_SP_NONE;
}

MslTypeSpec mslc_parse_type(MslCompiler *c) {
  MslTypeSpec spec;
  int line = mslc_cur(c)->line;
  int space;
  int pointered = 0;
  memset(&spec, 0, sizeof spec);
  skip_qualifiers(c);
  space = mslc_space_keyword(mslc_cur(c));
  if (space != MSL_SP_NONE) {
    c->pos++;
  }
  skip_qualifiers(c);
  spec.type = parse_base(c, &spec);
  while (mslc_at(c, "*")) {
    c->pos++;
    if (space == MSL_SP_NONE) {
      mslc_fail(c, line, "pointer type needs an address space");
    }
    if (spec.type->kind == MSL_K_VOID || spec.vote) {
      mslc_fail(c, line, "pointer to this type is outside the dialect");
    }
    spec.type = msl_pointer_type(c->prog, space, spec.type);
    if (spec.type == NULL) {
      mslc_fail(c, line, "out of memory");
    }
    pointered = 1;
    space = next_pointer_space(c);
    if (space == MSL_SP_NONE) {
      break;
    }
  }
  spec.space = pointered ? MSL_SP_NONE : space;
  return spec;
}

int mslc_local_space(const MslLocal *local) {
  if (local->storage == MSL_VAR_GROUP) {
    return MSL_SP_THREADGROUP;
  }
  if (local->storage == MSL_VAR_ARGS) {
    return MSL_SP_CONSTANT;
  }
  return MSL_SP_THREAD;
}

int mslc_find_local(MslCompiler *c, const char *name, size_t len) {
  size_t i = c->bind_count;
  while (i > 0) {
    const MslBinding *bind = &c->binds[--i];
    if (strlen(bind->name) == len && memcmp(bind->name, name, len) == 0) {
      return bind->local;
    }
  }
  return -1;
}

MslFunction *mslc_find_function(MslCompiler *c, const char *name, size_t len) {
  size_t i;
  for (i = 0; i < c->prog->function_count; i++) {
    MslFunction *fn = c->prog->functions[i];
    if (strlen(fn->name) == len && memcmp(fn->name, name, len) == 0) {
      return fn;
    }
  }
  return NULL;
}

void mslc_add_callee(MslCompiler *c, MslFunction *fn) {
  size_t i;
  MslFunction *self = c->fn;
  for (i = 0; i < self->callee_count; i++) {
    if (self->callees[i] == fn) {
      return;
    }
  }
  mslc_grow(c, (void **)&self->callees, self->callee_count, &self->callee_cap, sizeof(MslFunction *));
  self->callees[self->callee_count++] = fn;
}

static int add_local(MslCompiler *c, const char *name, const MslType *type, int storage, size_t align, int line) {
  MslFunction *fn = c->fn;
  MslLocal *local;
  size_t i;
  for (i = 0; i < c->bind_count; i++) {
    if (c->binds[i].depth == c->scope_depth && strcmp(c->binds[i].name, name) == 0) {
      mslc_fail(c, line, "'%s' is already declared in this scope", name);
    }
  }
  mslc_grow(c, (void **)&fn->locals, fn->local_count, &fn->local_cap, sizeof(MslLocal));
  local = &fn->locals[fn->local_count];
  memset(local, 0, sizeof *local);
  local->name = name;
  local->type = type;
  local->storage = storage;
  local->align = align > type->align ? align : type->align;
  local->line = line;
  mslc_grow(c, (void **)&c->binds, c->bind_count, &c->bind_cap, sizeof(MslBinding));
  c->binds[c->bind_count].name = name;
  c->binds[c->bind_count].local = (int)fn->local_count;
  c->binds[c->bind_count].depth = c->scope_depth;
  c->bind_count++;
  return (int)fn->local_count++;
}

static void push_scope(MslCompiler *c) {
  c->scope_depth++;
}

static void pop_scope(MslCompiler *c) {
  while (c->bind_count > 0 && c->binds[c->bind_count - 1].depth == c->scope_depth) {
    c->bind_count--;
  }
  c->scope_depth--;
}

static int power_of_two(uint64_t value) {
  return value != 0 && (value & (value - 1)) == 0;
}

static void register_named(MslCompiler *c, MslType *type, int line) {
  MslProgram *prog = c->prog;
  if (msl_named_type(prog, type->name, strlen(type->name)) != NULL) {
    mslc_fail(c, line, "struct '%s' is already defined", type->name);
  }
  mslc_grow(c, (void **)&prog->named, prog->named_count, &prog->named_cap, sizeof(MslType *));
  prog->named[prog->named_count++] = type;
}

static void parse_record(MslCompiler *c, int line) {
  uint64_t align;
  uint64_t bytes;
  const char *name;
  MslType *type;
  mslc_expect(c, "(");
  align = mslc_int_literal(c);
  mslc_expect(c, ")");
  name = mslc_name(c);
  mslc_expect(c, "{");
  mslc_expect(c, "uchar");
  mslc_expect(c, "bytes");
  mslc_expect(c, "[");
  bytes = mslc_int_literal(c);
  mslc_expect(c, "]");
  mslc_expect(c, ";");
  mslc_expect(c, "}");
  mslc_expect(c, ";");
  if (!power_of_two(align) || align > 256) {
    mslc_fail(c, line, "alignas(%llu) must be a power of two no larger than 256", (unsigned long long)align);
  }
  if (bytes == 0 || bytes > 65536) {
    mslc_fail(c, line, "record size %llu is out of range", (unsigned long long)bytes);
  }
  type = msl_new_type(c->prog, MSL_K_RECORD);
  if (type == NULL) {
    mslc_fail(c, line, "out of memory");
  }
  type->name = name;
  type->align = (size_t)align;
  type->size = (size_t)((bytes + align - 1) / align * align);
  type->slots = msl_record_slots(type->size);
  register_named(c, type, line);
}

static int field_type_ok(const MslType *type) {
  if (type->kind == MSL_K_SCALAR || type->kind == MSL_K_VECTOR || type->kind == MSL_K_RECORD) {
    return 1;
  }
  return type->kind == MSL_K_POINTER && (type->space == MSL_SP_DEVICE || type->space == MSL_SP_CONSTANT);
}

static void parse_field(MslCompiler *c, size_t count, size_t *cap, size_t *offset, size_t *align) {
  int line = mslc_cur(c)->line;
  MslTypeSpec spec = mslc_parse_type(c);
  MslField *fields;
  const char *name;
  size_t i;
  char tn[160];
  if (spec.space != MSL_SP_NONE || spec.vote || !field_type_ok(spec.type)) {
    mslc_fail(c, line, "field type '%s' is outside the dialect", mslc_tname(spec.type, tn, sizeof tn));
  }
  name = mslc_name(c);
  mslc_expect(c, ";");
  mslc_grow(c, &c->scratch, count, cap, sizeof(MslField));
  fields = (MslField *)c->scratch;
  for (i = 0; i < count; i++) {
    if (strcmp(fields[i].name, name) == 0) {
      mslc_fail(c, line, "duplicate field '%s'", name);
    }
  }
  *offset = (*offset + spec.type->align - 1) / spec.type->align * spec.type->align;
  fields[count].name = name;
  fields[count].type = spec.type;
  fields[count].offset = *offset;
  *offset += spec.type->size;
  if (spec.type->align > *align) {
    *align = spec.type->align;
  }
}

static void parse_fields(MslCompiler *c, MslType *type) {
  size_t count = 0;
  size_t cap = 0;
  size_t offset = 0;
  size_t align = 1;
  free(c->scratch);
  c->scratch = NULL;
  while (!mslc_at(c, "}")) {
    parse_field(c, count, &cap, &offset, &align);
    count++;
  }
  type->fields = (MslField *)mslc_alloc(c, (count == 0 ? 1 : count) * sizeof(MslField));
  if (count > 0) {
    memcpy(type->fields, c->scratch, count * sizeof(MslField));
  }
  free(c->scratch);
  c->scratch = NULL;
  type->field_count = count;
  type->align = align;
  type->size = (offset + align - 1) / align * align;
}

static void parse_struct(MslCompiler *c) {
  int line = mslc_cur(c)->line;
  MslType *type;
  mslc_expect(c, "struct");
  if (mslc_accept(c, "alignas")) {
    parse_record(c, line);
    return;
  }
  type = msl_new_type(c->prog, MSL_K_ARGS);
  if (type == NULL) {
    mslc_fail(c, line, "out of memory");
  }
  type->name = mslc_name(c);
  mslc_expect(c, "{");
  parse_fields(c, type);
  mslc_expect(c, "}");
  mslc_expect(c, ";");
  register_named(c, type, line);
}

static void parse_static_assert(MslCompiler *c) {
  int line = mslc_cur(c)->line;
  MslTypeSpec spec;
  uint64_t expected;
  const MslToken *text;
  mslc_expect(c, "static_assert");
  mslc_expect(c, "(");
  mslc_expect(c, "sizeof");
  mslc_expect(c, "(");
  spec = mslc_parse_type(c);
  mslc_expect(c, ")");
  mslc_expect(c, "==");
  expected = mslc_int_literal(c);
  mslc_expect(c, ",");
  text = mslc_cur(c);
  if (text->kind != MSL_TK_STRING) {
    mslc_fail(c, text->line, "static_assert needs a message string");
  }
  c->pos++;
  mslc_expect(c, ")");
  mslc_expect(c, ";");
  if (spec.space != MSL_SP_NONE || spec.vote || spec.type->size == 0) {
    mslc_fail(c, line, "sizeof of this type is outside the dialect");
  }
  if (spec.type->size != expected) {
    mslc_fail(c, line, "static_assert failed: sizeof is %llu, not %llu: %s", (unsigned long long)spec.type->size,
              (unsigned long long)expected, text->string_value);
  }
}

static void compile_statement(MslCompiler *c);
static void compile_declaration(MslCompiler *c);
static void compile_simple(MslCompiler *c, const char *terminator);

static void compile_block_items(MslCompiler *c) {
  while (!mslc_at(c, "}")) {
    if (mslc_cur(c)->kind == MSL_TK_EOF) {
      mslc_fail(c, mslc_cur(c)->line, "unexpected end of file inside a block");
    }
    compile_statement(c);
  }
  c->pos++;
}

static void compile_block(MslCompiler *c) {
  mslc_expect(c, "{");
  push_scope(c);
  compile_block_items(c);
  pop_scope(c);
}

static MslJumpScope *push_jump(MslCompiler *c, int is_loop, size_t continue_target) {
  MslJumpScope *scope;
  mslc_grow(c, (void **)&c->jumps, c->jump_count, &c->jump_cap, sizeof(MslJumpScope));
  scope = &c->jumps[c->jump_count++];
  memset(scope, 0, sizeof *scope);
  scope->is_loop = is_loop;
  scope->continue_target = continue_target;
  return scope;
}

static void pop_jump(MslCompiler *c, size_t end) {
  MslJumpScope *scope = &c->jumps[c->jump_count - 1];
  size_t i;
  for (i = 0; i < scope->break_count; i++) {
    mslc_patch(c, scope->breaks[i], end);
  }
  free(scope->breaks);
  c->jump_count--;
}

static void compile_condition(MslCompiler *c, int line) {
  MslExpr cond;
  char tn[160];
  mslc_expect(c, "(");
  cond = mslc_expr(c);
  mslc_rvalue(c, &cond, line);
  if (cond.type != c->prog->scalar_types[MSL_S_BOOL]) {
    mslc_fail(c, line, "condition has type '%s', not bool", mslc_tname(cond.type, tn, sizeof tn));
  }
  mslc_expect(c, ")");
}

static void compile_if(MslCompiler *c) {
  int line = mslc_cur(c)->line;
  size_t skip;
  c->pos++;
  compile_condition(c, line);
  skip = mslc_emit(c, MSL_OP_JUMP_FALSE, line);
  compile_block(c);
  if (mslc_accept(c, "else")) {
    size_t over = mslc_emit(c, MSL_OP_JUMP, line);
    mslc_patch(c, skip, mslc_here(c));
    if (!mslc_at(c, "{")) {
      mslc_fail(c, mslc_cur(c)->line, "else must be followed by a braced block");
    }
    compile_block(c);
    mslc_patch(c, over, mslc_here(c));
  } else {
    mslc_patch(c, skip, mslc_here(c));
  }
}

static void emit_back_jump(MslCompiler *c, size_t target, int line) {
  size_t jump = mslc_emit(c, MSL_OP_JUMP, line);
  mslc_insn(c, jump)->a = (int64_t)target;
  mslc_insn(c, jump)->statement = 1;
}

static void compile_while(MslCompiler *c) {
  int line = mslc_cur(c)->line;
  size_t top = mslc_here(c);
  size_t exit;
  c->pos++;
  compile_condition(c, line);
  exit = mslc_emit(c, MSL_OP_JUMP_FALSE, line);
  push_jump(c, 1, top);
  compile_block(c);
  emit_back_jump(c, top, line);
  mslc_patch(c, exit, mslc_here(c));
  pop_jump(c, mslc_here(c));
}

static void compile_for(MslCompiler *c) {
  int line = mslc_cur(c)->line;
  size_t top;
  size_t exit;
  size_t to_body;
  size_t step;
  MslExpr cond;
  char tn[160];
  c->pos++;
  mslc_expect(c, "(");
  push_scope(c);
  if (!mslc_starts_type(c, 0)) {
    mslc_fail(c, line, "for loop must declare its counter");
  }
  compile_declaration(c);
  top = mslc_here(c);
  cond = mslc_expr(c);
  mslc_rvalue(c, &cond, line);
  if (cond.type != c->prog->scalar_types[MSL_S_BOOL]) {
    mslc_fail(c, line, "for condition has type '%s', not bool", mslc_tname(cond.type, tn, sizeof tn));
  }
  mslc_expect(c, ";");
  exit = mslc_emit(c, MSL_OP_JUMP_FALSE, line);
  to_body = mslc_emit(c, MSL_OP_JUMP, line);
  step = mslc_here(c);
  c->pending_statement = 1;
  compile_simple(c, ")");
  emit_back_jump(c, top, line);
  mslc_patch(c, to_body, mslc_here(c));
  push_jump(c, 1, step);
  compile_block(c);
  emit_back_jump(c, step, line);
  mslc_patch(c, exit, mslc_here(c));
  pop_jump(c, mslc_here(c));
  pop_scope(c);
}

static void compile_switch(MslCompiler *c) {
  int line = mslc_cur(c)->line;
  c->pos++;
  mslc_expect(c, "(");
  if (mslc_cur(c)->kind != MSL_TK_INT || mslc_cur(c)->int_value != 0 || mslc_cur(c)->len != 1) {
    mslc_fail(c, line, "only 'switch (0) { default: { ... } }' is in the dialect");
  }
  c->pos++;
  mslc_expect(c, ")");
  mslc_expect(c, "{");
  mslc_expect(c, "default");
  mslc_expect(c, ":");
  push_jump(c, 0, 0);
  compile_block(c);
  mslc_expect(c, "}");
  pop_jump(c, mslc_here(c));
}

static void compile_break(MslCompiler *c) {
  int line = mslc_cur(c)->line;
  MslJumpScope *scope;
  size_t jump;
  c->pos++;
  mslc_expect(c, ";");
  if (c->jump_count == 0) {
    mslc_fail(c, line, "break outside a loop or switch");
  }
  jump = mslc_emit(c, MSL_OP_JUMP, line);
  scope = &c->jumps[c->jump_count - 1];
  mslc_grow(c, (void **)&scope->breaks, scope->break_count, &scope->break_cap, sizeof(size_t));
  scope->breaks[scope->break_count++] = jump;
}

static void compile_continue(MslCompiler *c) {
  int line = mslc_cur(c)->line;
  size_t i = c->jump_count;
  c->pos++;
  mslc_expect(c, ";");
  while (i > 0) {
    MslJumpScope *scope = &c->jumps[--i];
    if (scope->is_loop) {
      size_t jump = mslc_emit(c, MSL_OP_JUMP, line);
      mslc_insn(c, jump)->a = (int64_t)scope->continue_target;
      return;
    }
  }
  mslc_fail(c, line, "continue outside a loop");
}

static void compile_return(MslCompiler *c) {
  int line = mslc_cur(c)->line;
  MslExpr value;
  size_t insn;
  c->pos++;
  if (c->fn->ret->kind == MSL_K_VOID) {
    if (!mslc_at(c, ";")) {
      mslc_fail(c, line, "void function '%s' returns a value", c->fn->name);
    }
    c->pos++;
    mslc_emit(c, MSL_OP_RETURN, line);
    return;
  }
  if (mslc_at(c, ";")) {
    mslc_fail(c, line, "function '%s' must return a value", c->fn->name);
  }
  value = mslc_expr(c);
  mslc_rvalue(c, &value, line);
  mslc_check_assign(c, c->fn->ret, value.type, line, "return");
  mslc_expect(c, ";");
  insn = mslc_emit(c, MSL_OP_RETURN_VALUE, line);
  mslc_insn(c, insn)->t1 = c->fn->ret;
}

static int value_type_ok(const MslType *type) {
  return type->kind == MSL_K_SCALAR || type->kind == MSL_K_VECTOR || type->kind == MSL_K_POINTER ||
         type->kind == MSL_K_RECORD || type->kind == MSL_K_MATRIX;
}

static void compile_array_declaration(MslCompiler *c, const MslType *elem, int space, size_t align, int line) {
  const char *name = mslc_name(c);
  uint64_t count;
  const MslType *array;
  int local;
  char tn[160];
  mslc_expect(c, "[");
  count = mslc_int_literal(c);
  mslc_expect(c, "]");
  mslc_expect(c, ";");
  if (count == 0 || count > 1048576) {
    mslc_fail(c, line, "array size %llu is out of range", (unsigned long long)count);
  }
  if (elem->kind != MSL_K_SCALAR && elem->kind != MSL_K_VECTOR && elem->kind != MSL_K_RECORD && elem->kind != MSL_K_MATRIX) {
    mslc_fail(c, line, "array of '%s' is outside the dialect", mslc_tname(elem, tn, sizeof tn));
  }
  if (space != MSL_SP_THREAD && space != MSL_SP_THREADGROUP && space != MSL_SP_NONE) {
    mslc_fail(c, line, "%s arrays are outside the dialect", msl_space_name(space));
  }
  if (space == MSL_SP_THREADGROUP && !c->fn->is_kernel) {
    mslc_fail(c, line, "threadgroup array '%s' outside kernel scope", name);
  }
  array = msl_array_type(c->prog, elem, (int)count);
  if (array == NULL) {
    mslc_fail(c, line, "out of memory");
  }
  local = add_local(c, name, array, space == MSL_SP_THREADGROUP ? MSL_VAR_GROUP : MSL_VAR_THREAD, align, line);
  if (space != MSL_SP_THREADGROUP) {
    size_t insn = mslc_emit(c, MSL_OP_UNINIT_LOCAL, line);
    mslc_insn(c, insn)->a = local;
  }
}

static size_t parse_alignas(MslCompiler *c, int line) {
  uint64_t align;
  mslc_expect(c, "alignas");
  mslc_expect(c, "(");
  align = mslc_int_literal(c);
  mslc_expect(c, ")");
  if (!power_of_two(align) || align > 256) {
    mslc_fail(c, line, "alignas(%llu) must be a power of two no larger than 256", (unsigned long long)align);
  }
  return (size_t)align;
}

static void compile_initializer(MslCompiler *c, int local, const MslType *type, int line) {
  MslExpr value;
  size_t insn;
  char tn[160];
  if (mslc_at(c, "{")) {
    c->pos++;
    mslc_expect(c, "}");
    if (type->kind != MSL_K_RECORD) {
      mslc_fail(c, line, "'= {}' initializes records only, not '%s'", mslc_tname(type, tn, sizeof tn));
    }
    insn = mslc_emit(c, MSL_OP_ZERO, line);
    mslc_insn(c, insn)->t1 = type;
  } else {
    value = mslc_expr(c);
    mslc_rvalue(c, &value, line);
    mslc_check_assign(c, type, value.type, line, "initialization");
  }
  insn = mslc_emit(c, MSL_OP_STORE_LOCAL, line);
  mslc_insn(c, insn)->a = local;
  mslc_insn(c, insn)->t1 = type;
}

static void compile_declaration(MslCompiler *c) {
  int line = mslc_cur(c)->line;
  int space = mslc_space_keyword(mslc_cur(c));
  MslTypeSpec spec;
  const char *name;
  int local;
  char tn[160];
  if ((space == MSL_SP_THREADGROUP || space == MSL_SP_THREAD) && mslc_tok_is(mslc_ahead(c, 1), "alignas")) {
    size_t align;
    c->pos++;
    align = parse_alignas(c, line);
    spec = mslc_parse_type(c);
    if (spec.space != MSL_SP_NONE || spec.vote || spec.type->kind == MSL_K_POINTER) {
      mslc_fail(c, line, "malformed array declaration");
    }
    compile_array_declaration(c, spec.type, space, align, line);
    return;
  }
  spec = mslc_parse_type(c);
  if (mslc_cur(c)->kind == MSL_TK_IDENT && mslc_tok_is(mslc_ahead(c, 1), "[")) {
    if (spec.vote) {
      mslc_fail(c, line, "malformed array declaration");
    }
    compile_array_declaration(c, spec.type, spec.space, 1, line);
    return;
  }
  if (spec.space != MSL_SP_NONE) {
    mslc_fail(c, line, "%s qualifier on a non-pointer local is outside the dialect", msl_space_name(spec.space));
  }
  if (spec.vote || !value_type_ok(spec.type)) {
    mslc_fail(c, line, "local of type '%s' is outside the dialect", mslc_tname(spec.type, tn, sizeof tn));
  }
  name = mslc_name(c);
  local = add_local(c, name, spec.type, MSL_VAR_THREAD, 1, line);
  if (mslc_accept(c, "=")) {
    compile_initializer(c, local, spec.type, line);
  } else {
    size_t insn;
    if (spec.type->kind != MSL_K_RECORD && spec.type->kind != MSL_K_MATRIX) {
      mslc_fail(c, line, "'%s %s' must be initialized", mslc_tname(spec.type, tn, sizeof tn), name);
    }
    insn = mslc_emit(c, MSL_OP_UNINIT_LOCAL, line);
    mslc_insn(c, insn)->a = local;
  }
  mslc_expect(c, ";");
}

static int compound_op(const MslToken *tok) {
  static const struct {
    const char *text;
    int op;
  } table[] = {
    {"+=", MSL_BIN_ADD}, {"-=", MSL_BIN_SUB}, {"*=", MSL_BIN_MUL}, {"|=", MSL_BIN_OR}, {"&=", MSL_BIN_AND},
    {"^=", MSL_BIN_XOR}, {"<<=", MSL_BIN_SHL}, {">>=", MSL_BIN_SHR}
  };
  size_t i;
  for (i = 0; i < sizeof(table) / sizeof(table[0]); i++) {
    if (mslc_tok_is(tok, table[i].text)) {
      return table[i].op;
    }
  }
  return -1;
}

static void load_for_update(MslCompiler *c, MslExpr target, int line) {
  MslExpr copy = target;
  if (target.kind == MSL_E_MEM) {
    size_t dup = mslc_emit(c, MSL_OP_DUP, line);
    mslc_insn(c, dup)->a = 2;
  }
  mslc_rvalue(c, &copy, line);
}

static void compile_update(MslCompiler *c, MslExpr target, int op, int line) {
  MslExpr current;
  MslExpr rhs;
  MslExpr result;
  char a[160];
  char b[160];
  mslc_check_writable(c, target, line);
  load_for_update(c, target, line);
  current = mslc_value_expr(target.type);
  if (op < 0) {
    int increment = mslc_at(c, "++");
    size_t insn;
    c->pos++;
    if (target.type->kind != MSL_K_SCALAR || !msl_scalars[target.type->scalar].is_int || target.type->scalar == MSL_S_BOOL) {
      mslc_fail(c, line, "++ and -- need an integer, not '%s'", mslc_tname(target.type, a, sizeof a));
    }
    insn = mslc_emit(c, MSL_OP_CONST, line);
    mslc_insn(c, insn)->a = 1;
    mslc_insn(c, insn)->t1 = target.type;
    rhs = mslc_value_expr(target.type);
    op = increment ? MSL_BIN_ADD : MSL_BIN_SUB;
  } else {
    c->pos++;
    rhs = mslc_expr(c);
    mslc_rvalue(c, &rhs, line);
  }
  result = mslc_binary(c, op, current, rhs, line);
  if (result.type != target.type) {
    mslc_fail(c, line, "compound assignment produces '%s' for a '%s' target", mslc_tname(result.type, a, sizeof a),
              mslc_tname(target.type, b, sizeof b));
  }
  mslc_store(c, target, line);
}

static void compile_simple(MslCompiler *c, const char *terminator) {
  int line = mslc_cur(c)->line;
  MslExpr target = mslc_expr(c);
  int op = compound_op(mslc_cur(c));
  if (mslc_at(c, "=")) {
    MslExpr value;
    c->pos++;
    mslc_check_writable(c, target, line);
    value = mslc_expr(c);
    mslc_rvalue(c, &value, line);
    mslc_check_assign(c, target.type, value.type, line, "assignment");
    mslc_store(c, target, line);
  } else if (op >= 0 || mslc_at(c, "++") || mslc_at(c, "--")) {
    compile_update(c, target, op, line);
  } else {
    if (!target.is_call) {
      mslc_fail(c, line, "expression statement must be a call or an assignment");
    }
    if (target.kind == MSL_E_VALUE && target.type->slots > 0) {
      size_t insn = mslc_emit(c, MSL_OP_POP, line);
      mslc_insn(c, insn)->a = target.type->slots;
    }
  }
  mslc_expect(c, terminator);
}

static int starts_declaration(MslCompiler *c) {
  const MslToken *tok = mslc_cur(c);
  if (!mslc_starts_type(c, 0)) {
    return 0;
  }
  if (mslc_tok_is(tok, "simd_vote")) {
    return 0;
  }
  if (mslc_space_keyword(tok) != MSL_SP_NONE || mslc_tok_is(tok, "const") || mslc_tok_is(tok, "coherent")) {
    return 1;
  }
  return !mslc_tok_is(mslc_ahead(c, 1), "(");
}

static void compile_statement(MslCompiler *c) {
  const MslToken *tok = mslc_cur(c);
  mslc_enter(c, tok->line);
  c->pending_statement = 1;
  if (mslc_tok_is(tok, "{")) {
    compile_block(c);
  } else if (mslc_tok_is(tok, "if")) {
    compile_if(c);
  } else if (mslc_tok_is(tok, "while")) {
    compile_while(c);
  } else if (mslc_tok_is(tok, "for")) {
    compile_for(c);
  } else if (mslc_tok_is(tok, "switch")) {
    compile_switch(c);
  } else if (mslc_tok_is(tok, "break")) {
    compile_break(c);
  } else if (mslc_tok_is(tok, "continue")) {
    compile_continue(c);
  } else if (mslc_tok_is(tok, "return")) {
    compile_return(c);
  } else if (mslc_tok_is(tok, ";")) {
    mslc_fail(c, tok->line, "empty statement is outside the dialect");
  } else if (starts_declaration(c)) {
    compile_declaration(c);
  } else {
    compile_simple(c, ";");
  }
  c->pending_statement = 0;
  mslc_leave(c);
}

static MslFunction *new_function(MslCompiler *c, const char *name, int line) {
  MslProgram *prog = c->prog;
  MslFunction *fn = (MslFunction *)mslc_alloc(c, sizeof(MslFunction));
  fn->name = name;
  fn->line = line;
  mslc_grow(c, (void **)&prog->functions, prog->function_count, &prog->function_cap, sizeof(MslFunction *));
  prog->functions[prog->function_count++] = fn;
  return fn;
}

static int kernel_attribute(const MslToken *tok, const MslType **wanted, MslProgram *prog) {
  static const struct {
    const char *name;
    int attr;
    int vector;
  } table[] = {
    {"thread_position_in_threadgroup", MSL_ATTR_TID, 1}, {"threadgroup_position_in_grid", MSL_ATTR_CTAID, 1},
    {"threads_per_threadgroup", MSL_ATTR_NTID, 1}, {"threadgroups_per_grid", MSL_ATTR_NCTAID, 1},
    {"thread_position_in_grid", MSL_ATTR_GID, 1}, {"threads_per_grid", MSL_ATTR_NGRID, 1},
    {"thread_index_in_simdgroup", MSL_ATTR_LANE, 0}, {"threads_per_simdgroup", MSL_ATTR_LANES, 0},
    {"simdgroup_index_in_threadgroup", MSL_ATTR_SIMD_INDEX, 0},
    {"simdgroups_per_threadgroup", MSL_ATTR_SIMD_COUNT, 0}, {"thread_index_in_threadgroup", MSL_ATTR_LINEAR, 0}
  };
  size_t i;
  for (i = 0; i < sizeof(table) / sizeof(table[0]); i++) {
    if (mslc_tok_is(tok, table[i].name)) {
      *wanted = table[i].vector ? msl_vector_type(prog, MSL_S_UINT, 3, 0) : prog->scalar_types[MSL_S_UINT];
      return table[i].attr;
    }
  }
  return MSL_ATTR_NONE;
}

static int parse_param_attribute(MslCompiler *c, const MslType *type, int is_reference, int line) {
  const MslToken *tok;
  const MslType *wanted = NULL;
  int attr;
  char tn[160];
  mslc_expect(c, "[");
  mslc_expect(c, "[");
  tok = mslc_cur(c);
  c->pos++;
  if (mslc_tok_is(tok, "buffer") || mslc_tok_is(tok, "threadgroup")) {
    int is_buffer = mslc_tok_is(tok, "buffer");
    uint64_t index;
    mslc_expect(c, "(");
    index = mslc_int_literal(c);
    mslc_expect(c, ")");
    mslc_expect(c, "]");
    mslc_expect(c, "]");
    if (index != 0) {
      mslc_fail(c, line, "only index 0 is in the dialect");
    }
    if (is_buffer && (!is_reference || type->kind != MSL_K_ARGS)) {
      mslc_fail(c, line, "[[buffer(0)]] must bind 'constant ARGS& name'");
    }
    if (!is_buffer && (is_reference || type->kind != MSL_K_POINTER || type->space != MSL_SP_THREADGROUP)) {
      mslc_fail(c, line, "[[threadgroup(0)]] must bind a threadgroup pointer");
    }
    return is_buffer ? MSL_ATTR_BUFFER : MSL_ATTR_ARENA;
  }
  attr = kernel_attribute(tok, &wanted, c->prog);
  mslc_expect(c, "]");
  mslc_expect(c, "]");
  if (attr == MSL_ATTR_NONE) {
    mslc_fail(c, line, "kernel parameter attribute is outside the dialect");
  }
  if (is_reference || type != wanted) {
    mslc_fail(c, line, "attribute needs type '%s'", mslc_tname(wanted, tn, sizeof tn));
  }
  return attr;
}

static void parse_kernel_param(MslCompiler *c, uint32_t *seen) {
  int line = mslc_cur(c)->line;
  MslTypeSpec spec = mslc_parse_type(c);
  int is_reference = 0;
  const char *name;
  int attr;
  int local;
  if (mslc_accept(c, "&")) {
    if (spec.space != MSL_SP_CONSTANT || spec.type->kind != MSL_K_ARGS) {
      mslc_fail(c, line, "only 'constant ARGS&' references are in the dialect");
    }
    is_reference = 1;
  } else if (spec.space != MSL_SP_NONE || spec.vote) {
    mslc_fail(c, line, "malformed kernel parameter");
  }
  name = mslc_name(c);
  attr = parse_param_attribute(c, spec.type, is_reference, line);
  if (*seen & (1u << attr)) {
    mslc_fail(c, line, "duplicate kernel parameter attribute");
  }
  *seen |= 1u << attr;
  local = add_local(c, name, spec.type, attr == MSL_ATTR_BUFFER ? MSL_VAR_ARGS : MSL_VAR_THREAD, 1, line);
  c->fn->locals[local].attr = attr;
}

static void parse_helper_param(MslCompiler *c, int prototype) {
  int line = mslc_cur(c)->line;
  MslTypeSpec spec = mslc_parse_type(c);
  const char *name = "";
  char tn[160];
  if (spec.space != MSL_SP_NONE || spec.vote || !value_type_ok(spec.type)) {
    mslc_fail(c, line, "parameter type '%s' is outside the dialect", mslc_tname(spec.type, tn, sizeof tn));
  }
  if (!prototype || mslc_cur(c)->kind == MSL_TK_IDENT) {
    name = mslc_name(c);
  }
  if (prototype) {
    MslFunction *fn = c->fn;
    mslc_grow(c, (void **)&fn->locals, fn->local_count, &fn->local_cap, sizeof(MslLocal));
    memset(&fn->locals[fn->local_count], 0, sizeof(MslLocal));
    fn->locals[fn->local_count].name = name;
    fn->locals[fn->local_count].type = spec.type;
    fn->locals[fn->local_count].align = spec.type->align;
    fn->locals[fn->local_count].line = line;
    fn->local_count++;
    return;
  }
  add_local(c, name, spec.type, MSL_VAR_THREAD, 1, line);
}

static int is_prototype(MslCompiler *c) {
  size_t depth = 0;
  size_t k = 0;
  for (;;) {
    const MslToken *tok = mslc_ahead(c, k++);
    if (tok->kind == MSL_TK_EOF) {
      return 0;
    }
    if (mslc_tok_is(tok, "(")) {
      depth++;
    } else if (mslc_tok_is(tok, ")")) {
      depth--;
      if (depth == 0) {
        return mslc_tok_is(mslc_ahead(c, k), ";");
      }
    }
  }
}

static void parse_params(MslCompiler *c, int is_kernel, int prototype) {
  uint32_t seen = 0;
  mslc_expect(c, "(");
  if (mslc_accept(c, ")")) {
    return;
  }
  for (;;) {
    if (is_kernel) {
      parse_kernel_param(c, &seen);
    } else {
      parse_helper_param(c, prototype);
    }
    if (mslc_accept(c, ")")) {
      return;
    }
    mslc_expect(c, ",");
  }
}

static void check_signature(MslCompiler *c, const MslFunction *old, const MslFunction *fresh, int line) {
  size_t i;
  if (old->ret != fresh->ret || old->param_count != fresh->param_count) {
    mslc_fail(c, line, "'%s' does not match its earlier declaration", fresh->name);
  }
  for (i = 0; i < old->param_count; i++) {
    if (old->locals[i].type != fresh->locals[i].type) {
      mslc_fail(c, line, "parameter %u of '%s' does not match its earlier declaration", (unsigned)(i + 1), fresh->name);
    }
  }
}

static void reset_function_state(MslCompiler *c) {
  c->bind_count = 0;
  c->scope_depth = 1;
  c->jump_count = 0;
}

static void finish_body(MslCompiler *c, int line) {
  if (c->fn->ret->kind == MSL_K_VOID) {
    mslc_emit(c, MSL_OP_RETURN, line);
  } else {
    mslc_emit(c, MSL_OP_FALL_OFF, line);
  }
}

static MslFunction *function_header(MslCompiler *c, int is_kernel, int line, int *prototype, MslFunction **existing) {
  MslTypeSpec ret;
  const char *name;
  MslFunction *scratch;
  char tn[160];
  c->pos++;
  ret = mslc_parse_type(c);
  if (ret.space != MSL_SP_NONE || ret.vote || (ret.type->kind != MSL_K_VOID && !value_type_ok(ret.type))) {
    mslc_fail(c, line, "return type '%s' is outside the dialect", mslc_tname(ret.type, tn, sizeof tn));
  }
  if (is_kernel && ret.type->kind != MSL_K_VOID) {
    mslc_fail(c, line, "kernel must return void");
  }
  name = mslc_name(c);
  *existing = mslc_find_function(c, name, strlen(name));
  *prototype = is_prototype(c);
  if (is_kernel && *prototype) {
    mslc_fail(c, line, "kernel prototypes are outside the dialect");
  }
  if (*existing != NULL && ((*existing)->defined || (*existing)->is_kernel || is_kernel)) {
    mslc_fail(c, line, "function '%s' is already defined", name);
  }
  scratch = (MslFunction *)mslc_alloc(c, sizeof(MslFunction));
  scratch->name = name;
  scratch->ret = ret.type;
  scratch->is_kernel = is_kernel;
  scratch->line = line;
  c->fn = scratch;
  reset_function_state(c);
  parse_params(c, is_kernel, *prototype);
  scratch->param_count = scratch->local_count;
  if (*existing != NULL) {
    check_signature(c, *existing, scratch, line);
  }
  return scratch;
}

static void finish_prototype(MslCompiler *c, MslFunction *existing, MslFunction *scratch, int line) {
  mslc_expect(c, ";");
  if (existing == NULL) {
    MslFunction *fn = new_function(c, scratch->name, line);
    *fn = *scratch;
  } else {
    free(scratch->locals);
  }
  scratch->locals = NULL;
  c->fn = NULL;
}

static void parse_function(MslCompiler *c, int is_kernel, uint32_t max_threads) {
  int line = mslc_cur(c)->line;
  MslFunction *existing;
  MslFunction *fn;
  int prototype;
  MslFunction *scratch = function_header(c, is_kernel, line, &prototype, &existing);
  if (prototype) {
    finish_prototype(c, existing, scratch, line);
    return;
  }
  fn = existing != NULL ? existing : new_function(c, scratch->name, line);
  free(fn->locals);
  *fn = *scratch;
  scratch->locals = NULL;
  fn->defined = 1;
  fn->max_threads = max_threads;
  c->fn = fn;
  mslc_expect(c, "{");
  compile_block_items(c);
  finish_body(c, c->toks[c->pos > 0 ? c->pos - 1 : 0].line);
  c->fn = NULL;
}

static uint32_t parse_function_attribute(MslCompiler *c) {
  int line = mslc_cur(c)->line;
  uint64_t value;
  mslc_expect(c, "[");
  mslc_expect(c, "[");
  mslc_expect(c, "max_total_threads_per_threadgroup");
  mslc_expect(c, "(");
  value = mslc_int_literal(c);
  mslc_expect(c, ")");
  mslc_expect(c, "]");
  mslc_expect(c, "]");
  if (value == 0 || value > 1024) {
    mslc_fail(c, line, "max_total_threads_per_threadgroup(%llu) is out of range", (unsigned long long)value);
  }
  if (!mslc_at(c, "kernel")) {
    mslc_fail(c, line, "function attribute must precede a kernel");
  }
  return (uint32_t)value;
}

static void parse_using(MslCompiler *c) {
  mslc_expect(c, "using");
  mslc_expect(c, "namespace");
  mslc_expect(c, "metal");
  mslc_expect(c, ";");
  c->saw_using = 1;
}

static void parse_top_level(MslCompiler *c) {
  while (mslc_cur(c)->kind != MSL_TK_EOF) {
    const MslToken *tok = mslc_cur(c);
    char found[64];
    if (mslc_tok_is(tok, "using")) {
      parse_using(c);
    } else if (mslc_tok_is(tok, "struct")) {
      parse_struct(c);
    } else if (mslc_tok_is(tok, "static_assert")) {
      parse_static_assert(c);
    } else if (mslc_tok_is(tok, "static")) {
      parse_function(c, 0, 0);
    } else if (mslc_tok_is(tok, "[")) {
      uint32_t max_threads = parse_function_attribute(c);
      parse_function(c, 1, max_threads);
    } else if (mslc_tok_is(tok, "kernel")) {
      parse_function(c, 1, 0);
    } else {
      token_text(tok, found, sizeof found);
      mslc_fail(c, tok->line, "unexpected '%s' at file scope", found);
    }
  }
}

static void visit_calls(MslCompiler *c, MslFunction *fn) {
  size_t i;
  if (fn->visit == 2) {
    return;
  }
  if (fn->visit == 1) {
    mslc_fail(c, fn->line, "function '%s' is recursive, which MSL does not allow", fn->name);
  }
  fn->visit = 1;
  for (i = 0; i < fn->callee_count; i++) {
    if (!fn->callees[i]->defined) {
      mslc_fail(c, fn->line, "'%s' calls '%s', which is declared but never defined", fn->name, fn->callees[i]->name);
    }
    visit_calls(c, fn->callees[i]);
  }
  fn->visit = 2;
}

static void layout_function(MslCompiler *c, MslFunction *fn) {
  size_t i;
  size_t offset = 0;
  size_t slots = 0;
  fn->local_offsets = (size_t *)calloc(fn->local_count + 1, sizeof(size_t));
  if (fn->local_offsets == NULL) {
    mslc_fail(c, fn->line, "out of memory");
  }
  for (i = 0; i < fn->local_count; i++) {
    const MslLocal *local = &fn->locals[i];
    size_t align = local->align < 16 ? local->align : 16;
    if (local->storage != MSL_VAR_THREAD) {
      continue;
    }
    offset = (offset + align - 1) / align * align;
    fn->local_offsets[i] = offset;
    offset += local->type->size == 0 ? 1 : local->type->size;
  }
  for (i = 0; i < fn->param_count; i++) {
    slots += (size_t)fn->locals[i].type->slots;
  }
  fn->frame_bytes = offset;
  fn->param_slots = slots;
}

static void finish_program(MslCompiler *c) {
  size_t i;
  int line = mslc_cur(c)->line;
  if (!c->prog->saw_include) {
    mslc_fail(c, 1, "missing '#include <metal_stdlib>'");
  }
  if (!c->saw_using) {
    mslc_fail(c, 1, "missing 'using namespace metal;'");
  }
  for (i = 0; i < c->prog->function_count; i++) {
    visit_calls(c, c->prog->functions[i]);
  }
  for (i = 0; i < c->prog->function_count; i++) {
    MslFunction *fn = c->prog->functions[i];
    if (fn->defined) {
      layout_function(c, fn);
    }
  }
  (void)line;
}

int msl_compile(MslProgram *prog, MslToken *tokens, size_t count, char *error, size_t error_size) {
  MslCompiler *c = (MslCompiler *)calloc(1, sizeof(MslCompiler));
  int ok = 0;
  if (c == NULL) {
    msl_format_error(error, error_size, "out of memory");
    return 0;
  }
  c->prog = prog;
  c->toks = tokens;
  c->count = count;
  c->error = error;
  c->error_size = error_size;
  if (setjmp(c->fail) == 0) {
    parse_top_level(c);
    finish_program(c);
    ok = 1;
  } else if (c->fn != NULL && c->fn->locals != NULL && mslc_find_function(c, c->fn->name, strlen(c->fn->name)) != c->fn) {
    free(c->fn->locals);
    c->fn->locals = NULL;
  }
  while (c->jump_count > 0) {
    free(c->jumps[--c->jump_count].breaks);
  }
  free(c->jumps);
  free(c->binds);
  free(c->scratch);
  free(c);
  return ok;
}
