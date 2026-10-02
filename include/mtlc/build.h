
#ifndef MTLC_BUILD_H
#define MTLC_BUILD_H

#include "diag.h"
#include "intrinsic.h"
#include "module.h"
#include "tensor.h"
#include "type.h"

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct MtlcBuilder MtlcBuilder;
typedef struct MtlcFn MtlcFn;

typedef int MtlcValue;
#define MTLC_NO_VALUE (-1)

typedef struct {
  MtlcValue a;
  MtlcValue b;
  MtlcValue c;
  MtlcValue d;
  MtlcValue metadata;
  MtlcValue a_scale;
  MtlcValue b_scale;
  unsigned runtime_stride_mask;
  MtlcValue a_leading_dimension;
  MtlcValue b_leading_dimension;
  MtlcValue c_leading_dimension;
  MtlcValue d_leading_dimension;
  /* Read only when the descriptor's c_scale_mode is PER_ROW. */
  MtlcValue c_scale;
} MtlcTensorMmaOperands;

typedef struct {
  MtlcTensorMmaOperands matrix;
  MtlcValue row_origin;
  MtlcValue column_origin;
  MtlcValue problem_m;
  MtlcValue problem_n;
  MtlcValue problem_k;
} MtlcTensorMatmulOperands;

typedef struct {
  MtlcValue destination;
  MtlcValue bias;
  MtlcValue alpha;
  MtlcValue beta;
  MtlcValue clamp_min;
  MtlcValue clamp_max;
  MtlcValue leading_dimension;
  MtlcValue bias_leading_dimension;
} MtlcTensorEpilogueOperands;

typedef struct {
  MtlcValue destination;
  MtlcValue source;
  MtlcValue prepared_view;
  MtlcValue coordinates[MTLC_TENSOR_MAX_RANK];
} MtlcTensorTransferOperands;

typedef struct {
  MtlcValue x;
  MtlcValue y;
  MtlcValue z;
} MtlcDim3;

MtlcBuilder *mtlc_builder_create(void);
void mtlc_builder_destroy(MtlcBuilder *builder);

void mtlc_builder_set_diagnostic_handler(MtlcBuilder *builder,
                                         MtlcDiagHandler handler,
                                         void *user_data);

int mtlc_builder_ok(const MtlcBuilder *builder);

const char *mtlc_builder_error(const MtlcBuilder *builder);

int mtlc_fn_ok(const MtlcFn *fn);

MtlcFn *mtlc_builder_function(MtlcBuilder *builder, const char *name,
                             const MtlcType *return_type,
                             const char *const *param_names,
                             const MtlcType *const *param_types,
                             size_t param_count, int is_extern);

int mtlc_builder_declare_function(MtlcBuilder *builder, const char *name,
                                  const MtlcType *return_type,
                                  const char *const *param_names,
                                  const MtlcType *const *param_types,
                                  size_t param_count);

int mtlc_fn_set_inline(MtlcFn *fn);

int mtlc_fn_set_inline_required(MtlcFn *fn);

int mtlc_fn_set_noinline(MtlcFn *fn);

int mtlc_fn_set_pure(MtlcFn *fn);

MtlcFn *mtlc_builder_kernel(MtlcBuilder *builder, const char *name,
                           const char *const *param_names,
                           const MtlcType *const *param_types,
                           size_t param_count);

void mtlc_builder_global(MtlcBuilder *builder, const char *name,
                        const MtlcType *type, long long init_value,
                        int is_extern);

MtlcValue mtlc_fn_param(MtlcFn *fn, size_t index);

MtlcValue mtlc_const_int(MtlcFn *fn, const MtlcType *type, long long value);

MtlcValue mtlc_const_float(MtlcFn *fn, const MtlcType *type, double value);

MtlcValue mtlc_local(MtlcFn *fn, const char *name, const MtlcType *type);

MtlcValue mtlc_address_space_alloc(MtlcFn *fn, const char *name,
                                   const MtlcType *element_type, size_t count,
                                   MtlcAddressSpace address_space);

MtlcValue mtlc_dynamic_workgroup_view(MtlcFn *fn, const char *name,
                                      const MtlcType *element_type);

MtlcValue mtlc_global_ref(MtlcFn *fn, const char *name);

void mtlc_assign(MtlcFn *fn, MtlcValue dest, MtlcValue value);

typedef enum {
  MTLC_BINOP_ADD,
  MTLC_BINOP_SUB,
  MTLC_BINOP_MUL,
  MTLC_BINOP_DIV,
  MTLC_BINOP_REM,
  MTLC_BINOP_EQ,
  MTLC_BINOP_NE,
  MTLC_BINOP_LT,
  MTLC_BINOP_LE,
  MTLC_BINOP_GT,
  MTLC_BINOP_GE,
  MTLC_BINOP_LOGICAL_AND,
  MTLC_BINOP_LOGICAL_OR,
  MTLC_BINOP_BIT_AND,
  MTLC_BINOP_BIT_OR,
  MTLC_BINOP_BIT_XOR,
  MTLC_BINOP_SHL,
  MTLC_BINOP_SHR
} MtlcBinaryOp;

typedef enum {
  MTLC_UNOP_NEGATE,
  MTLC_UNOP_LOGICAL_NOT,
  MTLC_UNOP_BITWISE_NOT
} MtlcUnaryOp;

const char *mtlc_binary_op_name(MtlcBinaryOp op);
const char *mtlc_unary_op_name(MtlcUnaryOp op);

MtlcValue mtlc_binary_op(MtlcFn *fn, MtlcBinaryOp op, MtlcValue lhs,
                         MtlcValue rhs, const MtlcType *result_type);

MtlcValue mtlc_unary_op(MtlcFn *fn, MtlcUnaryOp op, MtlcValue operand,
                        const MtlcType *result_type);

MtlcValue mtlc_binary(MtlcFn *fn, const char *op, MtlcValue lhs, MtlcValue rhs,
                     const MtlcType *result_type);
MtlcValue mtlc_unary(MtlcFn *fn, const char *op, MtlcValue operand,
                    const MtlcType *result_type);

MtlcValue mtlc_call(MtlcFn *fn, const char *callee, const MtlcValue *args,
                   size_t arg_count, const MtlcType *return_type);

MtlcValue mtlc_intrinsic(MtlcFn *fn, MtlcIntrinsic intrinsic,
                         const MtlcValue *args, size_t arg_count,
                         const MtlcType *return_type);

MtlcValue mtlc_intrinsic_memory(MtlcFn *fn, MtlcIntrinsic intrinsic,
                                const MtlcValue *args, size_t arg_count,
                                const MtlcType *return_type,
                                MtlcAddressSpace address_space,
                                MtlcMemoryOrder order,
                                MtlcMemoryScope scope);

MtlcValue mtlc_atomic_compare_exchange(
    MtlcFn *fn, MtlcIntrinsic intrinsic, const MtlcValue args[4],
    const MtlcType *return_type, MtlcAddressSpace address_space,
    MtlcMemoryOrder success_order, MtlcMemoryOrder failure_order,
    MtlcMemoryScope scope);

void mtlc_workgroup_barrier(MtlcFn *fn, MtlcMemoryOrder order,
                            unsigned memory_regions);

void mtlc_async_copy_workgroup(MtlcFn *fn, MtlcValue destination,
                               MtlcValue source,
                               const MtlcType *element_type,
                               uint32_t element_count,
                               uint32_t transaction_bytes,
                               MtlcAsyncCache cache);

void mtlc_async_copy_commit(MtlcFn *fn);

void mtlc_async_copy_wait(MtlcFn *fn, uint32_t pending_groups);

void mtlc_tensor_transfer_workgroup(
    MtlcFn *fn, const MtlcTensorTransferDesc *desc,
    const MtlcTensorTransferOperands *operands);

void mtlc_tensor_mma(MtlcFn *fn, const MtlcTensorMmaDesc *desc,
                     MtlcValue a, MtlcValue b, MtlcValue c, MtlcValue d);

void mtlc_tensor_mma_ex(MtlcFn *fn, const MtlcTensorMmaDesc *desc,
                        const MtlcTensorMmaOperands *operands);

void mtlc_tensor_mma_chain(MtlcFn *fn, const MtlcTensorMmaDesc *desc,
                           const MtlcTensorMmaOperands *tiles,
                           size_t tile_count);

void mtlc_tensor_mma_strided(MtlcFn *fn, const MtlcTensorMmaDesc *desc,
                             MtlcValue a, MtlcValue b,
                             MtlcValue c, MtlcValue d,
                             MtlcValue lda, MtlcValue ldb,
                             MtlcValue ldc, MtlcValue ldd);

void mtlc_tensor_matmul(MtlcFn *fn, const MtlcTensorMmaDesc *desc,
                        const MtlcTensorMatmulOperands *operands);

void mtlc_tensor_epilogue(
    MtlcFn *fn, const MtlcTensorEpilogueDesc *desc,
    const MtlcTensorEpilogueOperands *operands);

void mtlc_gpu_launch(MtlcFn *fn, MtlcValue kernel_handle, MtlcDim3 grid,
                     MtlcDim3 block, MtlcValue dynamic_shared_bytes,
                     MtlcValue stream, const MtlcValue *args,
                     const MtlcType *const *arg_types, size_t arg_count);

MtlcValue mtlc_function_address(MtlcFn *fn, const char *name);

MtlcValue mtlc_call_indirect(MtlcFn *fn, MtlcValue callee,
                             const MtlcValue *args, size_t arg_count,
                             const MtlcType *return_type);

MtlcValue mtlc_cast(MtlcFn *fn, MtlcValue value, const MtlcType *type);

MtlcValue mtlc_address_of(MtlcFn *fn, MtlcValue storage,
                         const MtlcType *pointer_type);

MtlcValue mtlc_load(MtlcFn *fn, MtlcValue address, const MtlcType *elem_type);

void mtlc_store(MtlcFn *fn, MtlcValue address, MtlcValue value,
               const MtlcType *elem_type);

MtlcValue mtlc_element_address(MtlcFn *fn, MtlcValue base,
                               const MtlcType *pointer_type, MtlcValue index);

MtlcValue mtlc_load_element(MtlcFn *fn, MtlcValue base,
                            const MtlcType *pointer_type, MtlcValue index);
void mtlc_store_element(MtlcFn *fn, MtlcValue base,
                        const MtlcType *pointer_type, MtlcValue index,
                        MtlcValue value);

MtlcValue mtlc_field_address(MtlcFn *fn, MtlcValue base,
                             const MtlcType *struct_pointer_type,
                             size_t field_index);

MtlcValue mtlc_load_field(MtlcFn *fn, MtlcValue base,
                          const MtlcType *struct_pointer_type,
                          size_t field_index);
void mtlc_store_field(MtlcFn *fn, MtlcValue base,
                      const MtlcType *struct_pointer_type, size_t field_index,
                      MtlcValue value);

typedef int MtlcLabel;
#define MTLC_NO_LABEL (-1)

MtlcLabel mtlc_label_new(MtlcFn *fn, const char *hint);

void mtlc_label_here(MtlcFn *fn, MtlcLabel label);

void mtlc_jump_to(MtlcFn *fn, MtlcLabel label);

void mtlc_branch_if_zero_to(MtlcFn *fn, MtlcValue cond, MtlcLabel label);

void mtlc_label(MtlcFn *fn, const char *label);
void mtlc_jump(MtlcFn *fn, const char *label);
void mtlc_branch_if_zero(MtlcFn *fn, MtlcValue cond, const char *label);

void mtlc_return(MtlcFn *fn, MtlcValue value);

MtlcModule *mtlc_builder_finish(MtlcBuilder *builder);

#ifdef __cplusplus
}
#endif

#endif
