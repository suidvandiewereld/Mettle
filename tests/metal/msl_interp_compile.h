#ifndef MSL_INTERP_COMPILE_H
#define MSL_INTERP_COMPILE_H

#include "msl_interp_internal.h"

enum { MSL_E_VALUE, MSL_E_VAR, MSL_E_MEM, MSL_E_CONST, MSL_E_VOID };

typedef struct {
  int kind;
  const MslType *type;
  int local;
  int space;
  int is_call;
  uint64_t value;
} MslExpr;

typedef struct {
  const MslType *type;
  int space;
  int vote;
} MslTypeSpec;

typedef struct {
  const char *name;
  int local;
  size_t depth;
} MslBinding;

typedef struct {
  int is_loop;
  size_t continue_target;
  size_t *breaks;
  size_t break_count;
  size_t break_cap;
} MslJumpScope;

typedef struct {
  MslProgram *prog;
  MslToken *toks;
  size_t count;
  size_t pos;
  char *error;
  size_t error_size;
  jmp_buf fail;
  MslFunction *fn;
  MslBinding *binds;
  size_t bind_count;
  size_t bind_cap;
  size_t scope_depth;
  MslJumpScope *jumps;
  size_t jump_count;
  size_t jump_cap;
  int pending_statement;
  int saw_using;
  int nesting;
  void *scratch;
} MslCompiler;

void mslc_fail(MslCompiler *c, int line, const char *fmt, ...);
void mslc_enter(MslCompiler *c, int line);
void mslc_leave(MslCompiler *c);
const MslToken *mslc_cur(MslCompiler *c);
const MslToken *mslc_ahead(MslCompiler *c, size_t k);
int mslc_tok_is(const MslToken *tok, const char *text);
int mslc_at(MslCompiler *c, const char *text);
int mslc_accept(MslCompiler *c, const char *text);
void mslc_expect(MslCompiler *c, const char *text);
const char *mslc_name(MslCompiler *c);
void *mslc_alloc(MslCompiler *c, size_t size);
void mslc_grow(MslCompiler *c, void **array, size_t count, size_t *cap, size_t elem);
size_t mslc_emit(MslCompiler *c, int op, int line);
MslInsn *mslc_insn(MslCompiler *c, size_t index);
size_t mslc_here(MslCompiler *c);
void mslc_patch(MslCompiler *c, size_t index, size_t target);
int mslc_space_keyword(const MslToken *tok);
int mslc_starts_type(MslCompiler *c, size_t offset);
MslTypeSpec mslc_parse_type(MslCompiler *c);
int mslc_find_local(MslCompiler *c, const char *name, size_t len);
uint64_t mslc_int_literal(MslCompiler *c);
MslExpr mslc_expr(MslCompiler *c);
MslExpr mslc_value_expr(const MslType *type);
void mslc_rvalue(MslCompiler *c, MslExpr *e, int line);
void mslc_check_assign(MslCompiler *c, const MslType *target, const MslType *source, int line, const char *what);
const char *mslc_tname(const MslType *type, char *buffer, size_t size);
void mslc_store(MslCompiler *c, MslExpr target, int line);
void mslc_check_writable(MslCompiler *c, MslExpr target, int line);
MslExpr mslc_binary(MslCompiler *c, int op, MslExpr l, MslExpr r, int line);
void mslc_add_callee(MslCompiler *c, MslFunction *fn);
MslFunction *mslc_find_function(MslCompiler *c, const char *name, size_t len);
int mslc_local_space(const MslLocal *local);
MslExpr mslc_builtin_call(MslCompiler *c, const MslToken *name_tok, const char *name, int line);
MslExpr mslc_os_log(MslCompiler *c, int line);
int mslc_is_builtin(const char *name);

#endif
