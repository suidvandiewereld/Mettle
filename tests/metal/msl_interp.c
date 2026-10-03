#include "msl_interp_internal.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
  const char *name;
  int scalar;
  int count;
  int packed;
} MslVectorName;

static const MslVectorName msl_vector_names[] = {
  {"uint3", MSL_S_UINT, 3, 0},
  {"half2", MSL_S_HALF, 2, 0},
  {"float4", MSL_S_FLOAT, 4, 0},
  {"uint4", MSL_S_UINT, 4, 0},
  {"packed_float4", MSL_S_FLOAT, 4, 1},
  {"packed_uint4", MSL_S_UINT, 4, 1},
  {"ulong2", MSL_S_ULONG, 2, 0}
};

static const struct {
  const char *name;
  int scalar;
} msl_matrix_names[] = {
  {"simdgroup_float8x8", MSL_S_FLOAT},
  {"simdgroup_half8x8", MSL_S_HALF},
  {"simdgroup_bfloat8x8", MSL_S_BFLOAT}
};

void msl_vformat_error(char *error, size_t error_size, const char *fmt, va_list ap) {
  if (error == NULL || error_size == 0) {
    return;
  }
  vsnprintf(error, error_size, fmt, ap);
}

void msl_format_error(char *error, size_t error_size, const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  msl_vformat_error(error, error_size, fmt, ap);
  va_end(ap);
}

void *msl_arena_alloc(MslProgram *prog, size_t size) {
  MslChunk *chunk = prog->chunks;
  void *out;
  size = (size + 15u) & ~(size_t)15u;
  if (chunk == NULL || chunk->used + size > chunk->cap) {
    size_t cap = size > 65536u ? size : 65536u;
    MslChunk *fresh = (MslChunk *)malloc(sizeof(MslChunk) + cap + 16u);
    if (fresh == NULL) {
      return NULL;
    }
    fresh->next = chunk;
    fresh->used = 0;
    fresh->cap = cap;
    fresh->data = (unsigned char *)(((uintptr_t)(fresh + 1) + 15u) & ~(uintptr_t)15u);
    prog->chunks = fresh;
    chunk = fresh;
  }
  out = chunk->data + chunk->used;
  chunk->used += size;
  memset(out, 0, size);
  return out;
}

char *msl_arena_strndup(MslProgram *prog, const char *text, size_t len) {
  char *out = (char *)msl_arena_alloc(prog, len + 1);
  if (out == NULL) {
    return NULL;
  }
  memcpy(out, text, len);
  out[len] = 0;
  return out;
}

int msl_record_slots(size_t size) {
  return (int)(2u * ((size + 7u) / 8u));
}

MslType *msl_new_type(MslProgram *prog, int kind) {
  MslType *type = (MslType *)msl_arena_alloc(prog, sizeof(MslType));
  if (type == NULL) {
    return NULL;
  }
  type->kind = kind;
  type->next = prog->types;
  prog->types = type;
  return type;
}

const MslType *msl_pointer_type(MslProgram *prog, int space, const MslType *elem) {
  MslType *type;
  for (type = prog->types; type != NULL; type = type->next) {
    if (type->kind == MSL_K_POINTER && type->space == space && type->elem == elem) {
      return type;
    }
  }
  type = msl_new_type(prog, MSL_K_POINTER);
  if (type == NULL) {
    return NULL;
  }
  type->space = space;
  type->elem = elem;
  type->size = 8;
  type->align = 8;
  type->slots = 2;
  return type;
}

const MslType *msl_array_type(MslProgram *prog, const MslType *elem, int count) {
  MslType *type;
  for (type = prog->types; type != NULL; type = type->next) {
    if (type->kind == MSL_K_ARRAY && type->elem == elem && type->count == count) {
      return type;
    }
  }
  type = msl_new_type(prog, MSL_K_ARRAY);
  if (type == NULL) {
    return NULL;
  }
  type->elem = elem;
  type->count = count;
  type->size = elem->size * (size_t)count;
  type->align = elem->align;
  type->slots = 0;
  return type;
}

const MslType *msl_vector_type(MslProgram *prog, int scalar, int count, int packed) {
  MslType *type;
  for (type = prog->types; type != NULL; type = type->next) {
    if (type->kind == MSL_K_VECTOR && type->scalar == scalar && type->count == count && type->packed == packed) {
      return type;
    }
  }
  return NULL;
}

const MslType *msl_vector_by_name(MslProgram *prog, const char *name, size_t len) {
  size_t i;
  for (i = 0; i < sizeof(msl_vector_names) / sizeof(msl_vector_names[0]); i++) {
    if (strlen(msl_vector_names[i].name) == len && memcmp(msl_vector_names[i].name, name, len) == 0) {
      return msl_vector_type(prog, msl_vector_names[i].scalar, msl_vector_names[i].count, msl_vector_names[i].packed);
    }
  }
  return NULL;
}

const MslType *msl_matrix_type(MslProgram *prog, int scalar) {
  MslType *type;
  for (type = prog->types; type != NULL; type = type->next) {
    if (type->kind == MSL_K_MATRIX && type->scalar == scalar) {
      return type;
    }
  }
  return NULL;
}

const MslType *msl_matrix_by_name(MslProgram *prog, const char *name, size_t len) {
  size_t i;
  for (i = 0; i < sizeof(msl_matrix_names) / sizeof(msl_matrix_names[0]); i++) {
    if (strlen(msl_matrix_names[i].name) == len && memcmp(msl_matrix_names[i].name, name, len) == 0) {
      return msl_matrix_type(prog, msl_matrix_names[i].scalar);
    }
  }
  return NULL;
}

const MslType *msl_named_type(MslProgram *prog, const char *name, size_t len) {
  size_t i;
  for (i = 0; i < prog->named_count; i++) {
    const MslType *type = prog->named[i];
    if (strlen(type->name) == len && memcmp(type->name, name, len) == 0) {
      return type;
    }
  }
  return NULL;
}

const char *msl_space_name(int space) {
  switch (space) {
    case MSL_SP_DEVICE: return "device";
    case MSL_SP_CONSTANT: return "constant";
    case MSL_SP_THREADGROUP: return "threadgroup";
    case MSL_SP_THREAD: return "thread";
    default: return "no";
  }
}

static const char *simple_type_name(const MslType *type) {
  switch (type->kind) {
    case MSL_K_VOID: return "void";
    case MSL_K_SCALAR: return msl_scalars[type->scalar].name;
    case MSL_K_ATOMIC: return "atomic_uint";
    case MSL_K_VOTE: return "simd_vote";
    case MSL_K_NULLPTR: return "nullptr_t";
    case MSL_K_FLAGS: return "mem_flags";
    case MSL_K_ORDER: return "memory_order";
    case MSL_K_SCOPE: return "thread_scope";
    default: return type->name != NULL ? type->name : "?";
  }
}

void msl_type_name(const MslType *type, char *buffer, size_t size) {
  char inner[160];
  if (type == NULL) {
    snprintf(buffer, size, "?");
    return;
  }
  if (type->kind == MSL_K_POINTER) {
    msl_type_name(type->elem, inner, sizeof inner);
    snprintf(buffer, size, "%s %s*", msl_space_name(type->space), inner);
    return;
  }
  if (type->kind == MSL_K_ARRAY) {
    msl_type_name(type->elem, inner, sizeof inner);
    snprintf(buffer, size, "%s[%d]", inner, type->count);
    return;
  }
  snprintf(buffer, size, "%s", simple_type_name(type));
}

static int add_vector_types(MslProgram *prog) {
  size_t i;
  for (i = 0; i < sizeof(msl_vector_names) / sizeof(msl_vector_names[0]); i++) {
    const MslVectorName *entry = &msl_vector_names[i];
    MslType *type = msl_new_type(prog, MSL_K_VECTOR);
    size_t elem = msl_scalars[entry->scalar].size;
    if (type == NULL) {
      return 0;
    }
    type->name = entry->name;
    type->scalar = entry->scalar;
    type->count = entry->count;
    type->packed = entry->packed;
    type->elem = prog->scalar_types[entry->scalar];
    type->size = entry->packed ? elem * (size_t)entry->count : elem * (size_t)(entry->count == 3 ? 4 : entry->count);
    type->align = entry->packed ? elem : type->size;
    type->slots = entry->count;
  }
  return 1;
}

static int add_matrix_types(MslProgram *prog) {
  size_t i;
  for (i = 0; i < sizeof(msl_matrix_names) / sizeof(msl_matrix_names[0]); i++) {
    MslType *type = msl_new_type(prog, MSL_K_MATRIX);
    size_t elem = msl_scalars[msl_matrix_names[i].scalar].size;
    if (type == NULL) {
      return 0;
    }
    type->name = msl_matrix_names[i].name;
    type->scalar = msl_matrix_names[i].scalar;
    type->elem = prog->scalar_types[type->scalar];
    type->size = elem * 64u;
    type->align = elem;
    type->slots = msl_record_slots(type->size);
  }
  return 1;
}

static MslType *simple_type(MslProgram *prog, int kind, size_t size, int slots) {
  MslType *type = msl_new_type(prog, kind);
  if (type == NULL) {
    return NULL;
  }
  type->size = size;
  type->align = size == 0 ? 1 : size;
  type->slots = slots;
  return type;
}

static int add_base_types(MslProgram *prog) {
  int s;
  for (s = 0; s < MSL_S_COUNT; s++) {
    MslType *type = simple_type(prog, MSL_K_SCALAR, msl_scalars[s].size, 1);
    if (type == NULL) {
      return 0;
    }
    type->scalar = s;
    type->name = msl_scalars[s].name;
    prog->scalar_types[s] = type;
  }
  prog->void_type = simple_type(prog, MSL_K_VOID, 0, 0);
  prog->nullptr_type = simple_type(prog, MSL_K_NULLPTR, 8, 2);
  prog->vote_type = simple_type(prog, MSL_K_VOTE, 8, 1);
  prog->flags_type = simple_type(prog, MSL_K_FLAGS, 4, 0);
  prog->order_type = simple_type(prog, MSL_K_ORDER, 4, 0);
  prog->scope_type = simple_type(prog, MSL_K_SCOPE, 4, 0);
  prog->atomic_type = simple_type(prog, MSL_K_ATOMIC, 4, 0);
  if (prog->void_type == NULL || prog->nullptr_type == NULL || prog->vote_type == NULL || prog->flags_type == NULL ||
      prog->order_type == NULL || prog->scope_type == NULL || prog->atomic_type == NULL) {
    return 0;
  }
  return add_vector_types(prog) && add_matrix_types(prog);
}

static MslProgram *program_new(void) {
  MslProgram *prog = (MslProgram *)calloc(1, sizeof(MslProgram));
  if (prog == NULL) {
    return NULL;
  }
  prog->next_address = 0x100000000ull;
  if (!add_base_types(prog)) {
    msl_program_free(prog);
    return NULL;
  }
  return prog;
}

static void free_tokens(MslToken *tokens, size_t count) {
  (void)count;
  free(tokens);
}

MslProgram *msl_program_load(const char *source, size_t length, char *error, size_t error_size) {
  MslProgram *prog;
  MslToken *tokens = NULL;
  size_t count = 0;
  if (error != NULL && error_size > 0) {
    error[0] = 0;
  }
  if (source == NULL) {
    msl_format_error(error, error_size, "no source");
    return NULL;
  }
  prog = program_new();
  if (prog == NULL) {
    msl_format_error(error, error_size, "out of memory");
    return NULL;
  }
  if (!msl_lex(prog, source, length, &tokens, &count, error, error_size)) {
    free_tokens(tokens, count);
    msl_program_free(prog);
    return NULL;
  }
  if (!msl_compile(prog, tokens, count, error, error_size)) {
    free_tokens(tokens, count);
    msl_program_free(prog);
    return NULL;
  }
  free_tokens(tokens, count);
  return prog;
}

static void free_function(MslFunction *fn) {
  free(fn->locals);
  free(fn->code);
  free(fn->callees);
  free(fn->local_offsets);
}

void msl_program_free(MslProgram *program) {
  size_t i;
  MslChunk *chunk;
  if (program == NULL) {
    return;
  }
  for (i = 0; i < program->function_count; i++) {
    free_function(program->functions[i]);
  }
  free(program->functions);
  free(program->named);
  free(program->buffers);
  chunk = program->chunks;
  while (chunk != NULL) {
    MslChunk *next = chunk->next;
    free(chunk);
    chunk = next;
  }
  free(program);
}

uint64_t msl_device_register(MslProgram *program, void *host, size_t size) {
  uint64_t address;
  if (program == NULL) {
    return 0;
  }
  if (program->buffer_count == program->buffer_cap) {
    size_t cap = program->buffer_cap == 0 ? 8 : program->buffer_cap * 2;
    MslBuffer *grown = (MslBuffer *)realloc(program->buffers, cap * sizeof(MslBuffer));
    if (grown == NULL) {
      return 0;
    }
    program->buffers = grown;
    program->buffer_cap = cap;
  }
  address = (program->next_address + 255u) & ~(uint64_t)255u;
  program->buffers[program->buffer_count].host = (uint8_t *)host;
  program->buffers[program->buffer_count].size = size;
  program->buffers[program->buffer_count].address = address;
  program->buffer_count++;
  program->next_address = address + (uint64_t)size + 4096u;
  return address;
}
