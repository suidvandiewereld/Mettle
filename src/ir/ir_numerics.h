#ifndef METTLE_IR_NUMERICS_H
#define METTLE_IR_NUMERICS_H

#include <stddef.h>
#include <stdint.h>

typedef uint32_t NumTerm;

typedef enum {
  NUM_CONST = 1,
  NUM_INPUT,
  NUM_BYTE,
  NUM_CONCAT,
  NUM_ADD,
  NUM_SUB,
  NUM_MUL,
  NUM_AND,
  NUM_OR,
  NUM_XOR,
  NUM_SHL,
  NUM_LSHR,
  NUM_ASHR,
  NUM_UDIV,
  NUM_SDIV,
  NUM_UREM,
  NUM_SREM,
  NUM_SEXT,
  NUM_ICMP,
  NUM_FADD,
  NUM_FSUB,
  NUM_FMUL,
  NUM_FDIV,
  NUM_FFMA,
  NUM_FSQRT,
  NUM_FNEG,
  NUM_FABS,
  NUM_FMAX,
  NUM_FMIN,
  NUM_FCMP,
  NUM_SELECT,
  NUM_CVT,
  NUM_APPROX,
  NUM_IDOT,
  NUM_MMA,
  NUM_NIBBLE,
  NUM_BSCALE,
  NUM_OPCODE_COUNT
} NumOp;

typedef enum {
  NUM_CMP_EQ = 1,
  NUM_CMP_NE,
  NUM_CMP_LT,
  NUM_CMP_LE,
  NUM_CMP_GT,
  NUM_CMP_GE,
  NUM_CMP_ULT,
  NUM_CMP_ULE,
  NUM_CMP_UGT,
  NUM_CMP_UGE
} NumCompare;

typedef enum {
  NUM_CVT_F16_TO_F32 = 1,
  NUM_CVT_BF16_TO_F32,
  NUM_CVT_F32_TO_F16,
  NUM_CVT_F32_TO_BF16,
  NUM_CVT_F32_TO_F64,
  NUM_CVT_F64_TO_F32,
  NUM_CVT_S64_TO_F32,
  NUM_CVT_U64_TO_F32,
  NUM_CVT_S64_TO_F64,
  NUM_CVT_U64_TO_F64,
  NUM_CVT_F32_TO_S64,
  NUM_CVT_F64_TO_S64,
  NUM_CVT_F32_TO_U64,
  NUM_CVT_F64_TO_U64
} NumConvert;

typedef enum {
  NUM_APPROX_EX2 = 1,
  NUM_APPROX_LG2,
  NUM_APPROX_RSQRT,
  NUM_APPROX_SIN,
  NUM_APPROX_COS,
  NUM_APPROX_TANH
} NumApprox;

typedef enum {
  NUM_FACT_NEG = 1,
  NUM_FACT_ZERO = 2,
  NUM_FACT_NEGZERO = 4,
  NUM_FACT_NAN = 8,
  NUM_FACT_INF = 16,
  NUM_FACT_ALL = 31
} NumFact;

typedef struct NumStore NumStore;

NumStore *num_store_create(void);
void num_store_destroy(NumStore *store);
void num_store_set_site(NumStore *store, uint32_t line, const char *function);
int num_store_failed(const NumStore *store);

NumTerm num_const(NumStore *store, unsigned width, uint64_t bits);
NumTerm num_input(NumStore *store, uint32_t input, uint64_t offset);
NumTerm num_byte(NumStore *store, NumTerm word, unsigned index);
NumTerm num_concat(NumStore *store, const NumTerm *bytes, unsigned count);
NumTerm num_op(NumStore *store, NumOp op, unsigned width, uint64_t imm,
               const NumTerm *args, unsigned count);
NumTerm num_resize(NumStore *store, NumTerm value, unsigned width,
                   int is_signed);

unsigned num_width(const NumStore *store, NumTerm term);
NumOp num_opcode(const NumStore *store, NumTerm term);
uint64_t num_imm(const NumStore *store, NumTerm term);
unsigned num_arg_count(const NumStore *store, NumTerm term);
NumTerm num_arg(const NumStore *store, NumTerm term, unsigned index);
int num_constant(const NumStore *store, NumTerm term, uint64_t *bits);
unsigned num_facts(const NumStore *store, NumTerm term);
uint32_t num_line(const NumStore *store, NumTerm term);
const char *num_function(const NumStore *store, NumTerm term);
size_t num_term_count(const NumStore *store);
const char *num_op_name(const NumStore *store, NumTerm term);
void num_describe(const NumStore *store, NumTerm term, char *out,
                  size_t size);

#endif
