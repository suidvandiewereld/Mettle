
#ifndef MTLC_TENSOR_H
#define MTLC_TENSOR_H

#include "memory.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
  MTLC_TENSOR_ELEMENT_INVALID = 0,
  MTLC_TENSOR_ELEMENT_FLOAT16,
  MTLC_TENSOR_ELEMENT_BFLOAT16,
  MTLC_TENSOR_ELEMENT_TFLOAT32,
  MTLC_TENSOR_ELEMENT_FLOAT32,
  MTLC_TENSOR_ELEMENT_FLOAT64,
  MTLC_TENSOR_ELEMENT_FLOAT8_E4M3,
  MTLC_TENSOR_ELEMENT_FLOAT8_E5M2,
  MTLC_TENSOR_ELEMENT_FLOAT6_E2M3,
  MTLC_TENSOR_ELEMENT_FLOAT6_E3M2,
  MTLC_TENSOR_ELEMENT_FLOAT4_E2M1,
  MTLC_TENSOR_ELEMENT_SCALE_UE8M0,
  MTLC_TENSOR_ELEMENT_SCALE_UE4M3,
  MTLC_TENSOR_ELEMENT_INT8,
  MTLC_TENSOR_ELEMENT_UINT8,
  MTLC_TENSOR_ELEMENT_INT4,
  MTLC_TENSOR_ELEMENT_UINT4,
  MTLC_TENSOR_ELEMENT_BIT1,
  MTLC_TENSOR_ELEMENT_INT32
} MtlcTensorElement;

typedef enum {
  MTLC_TENSOR_LAYOUT_INVALID = 0,
  MTLC_TENSOR_LAYOUT_ROW_MAJOR,
  MTLC_TENSOR_LAYOUT_COLUMN_MAJOR
} MtlcTensorLayout;

typedef enum {
  MTLC_TENSOR_MATH_MULTIPLY_ADD = 0,
  MTLC_TENSOR_MATH_XOR_POPCOUNT,
  MTLC_TENSOR_MATH_AND_POPCOUNT
} MtlcTensorMathMode;

typedef enum {
  MTLC_TENSOR_SPARSITY_DENSE = 0,
  MTLC_TENSOR_SPARSITY_STRUCTURED_1_TO_2,
  MTLC_TENSOR_SPARSITY_STRUCTURED_2_TO_4,
  MTLC_TENSOR_SPARSITY_STRUCTURED_4_TO_8
} MtlcTensorSparsity;

typedef enum {
  MTLC_TENSOR_ROUND_DEFAULT = 0,
  MTLC_TENSOR_ROUND_NEAREST_EVEN,
  MTLC_TENSOR_ROUND_TOWARD_ZERO,
  MTLC_TENSOR_ROUND_DOWN,
  MTLC_TENSOR_ROUND_UP
} MtlcTensorRounding;

typedef enum {
  MTLC_TENSOR_OVERFLOW_WRAP = 0,
  MTLC_TENSOR_OVERFLOW_SATURATE_FINITE
} MtlcTensorOverflow;

typedef enum {
  MTLC_TENSOR_SCALE_NONE = 0,
  MTLC_TENSOR_SCALE_PER_TENSOR,
  MTLC_TENSOR_SCALE_BLOCK_16,
  MTLC_TENSOR_SCALE_BLOCK_32,
  MTLC_TENSOR_SCALE_PER_ROW
} MtlcTensorScaleMode;

/* HALVES packs 4-bit elements in 16-byte groups of 32 by linear storage
   position p: element p is in byte 16 * (p / 32) + p % 16, in its low
   nibble when p % 32 < 16 and its high nibble otherwise -- the block layout
   of GGML's Q4_0/Q4_1/IQ4_NL. */
typedef enum {
  MTLC_TENSOR_PACKING_LOGICAL = 0,
  MTLC_TENSOR_PACKING_DENSE_SUBBYTE,
  MTLC_TENSOR_PACKING_HALVES
} MtlcTensorPacking;

typedef enum {
  MTLC_TENSOR_BIAS_NONE = 0,
  MTLC_TENSOR_BIAS_PER_ROW,
  MTLC_TENSOR_BIAS_PER_COLUMN,
  MTLC_TENSOR_BIAS_MATRIX
} MtlcTensorBiasMode;

typedef enum {
  MTLC_TENSOR_ACTIVATION_IDENTITY = 0,
  MTLC_TENSOR_ACTIVATION_RELU,
  MTLC_TENSOR_ACTIVATION_CLAMP
} MtlcTensorActivation;

#define MTLC_TENSOR_MAX_RANK 5u

typedef enum {
  MTLC_TENSOR_TRANSFER_GLOBAL_TO_WORKGROUP = 0,
  MTLC_TENSOR_TRANSFER_WORKGROUP_TO_GLOBAL
} MtlcTensorTransferDirection;

typedef enum {

  MTLC_TENSOR_BOUNDS_ZERO = 0
} MtlcTensorBoundsMode;

typedef struct {
  uint8_t rank;
  MtlcTensorTransferDirection direction;
  MtlcTensorElement element;
  MtlcTensorPacking packing;
  MtlcTensorBoundsMode bounds;
  MtlcMemoryScope scope;
  uint64_t global_extent[MTLC_TENSOR_MAX_RANK];
  uint64_t global_stride_bytes[MTLC_TENSOR_MAX_RANK];
  uint32_t tile_extent[MTLC_TENSOR_MAX_RANK];
  uint32_t element_stride[MTLC_TENSOR_MAX_RANK];
} MtlcTensorTransferDesc;

typedef enum {
  MTLC_TENSOR_RUNTIME_STRIDE_NONE = 0,
  MTLC_TENSOR_RUNTIME_STRIDE_A = 1u << 0,
  MTLC_TENSOR_RUNTIME_STRIDE_B = 1u << 1,
  MTLC_TENSOR_RUNTIME_STRIDE_C = 1u << 2,
  MTLC_TENSOR_RUNTIME_STRIDE_D = 1u << 3,
  MTLC_TENSOR_RUNTIME_STRIDE_ALL = (1u << 4) - 1u
} MtlcTensorRuntimeStride;

typedef struct {
  uint16_t m;
  uint16_t n;
  uint16_t k;
  MtlcTensorMathMode math_mode;
  MtlcTensorSparsity sparsity;
  MtlcTensorElement a_element;
  MtlcTensorElement b_element;
  MtlcTensorElement accumulator_element;
  MtlcTensorElement result_element;
  MtlcTensorLayout a_layout;
  MtlcTensorLayout b_layout;
  MtlcTensorLayout c_layout;
  MtlcTensorLayout d_layout;
  uint32_t a_leading_dimension;
  uint32_t b_leading_dimension;
  uint32_t c_leading_dimension;
  uint32_t d_leading_dimension;
  MtlcTensorRounding rounding;
  MtlcTensorOverflow overflow;
  MtlcTensorScaleMode a_scale_mode;
  MtlcTensorScaleMode b_scale_mode;
  MtlcTensorElement a_scale_element;
  MtlcTensorElement b_scale_element;
  MtlcTensorPacking a_packing;
  MtlcTensorPacking b_packing;

  uint32_t a_scale_leading_dimension;
  uint32_t b_scale_leading_dimension;
  uint8_t transpose_a;
  uint8_t transpose_b;
  MtlcMemoryScope scope;
  /* An unsigned operand's value is its stored integer minus its zero
     point (UINT4 and UINT8 only; 0 elsewhere). */
  uint8_t a_zero_point;
  uint8_t b_zero_point;
  /* PER_ROW: D = A * B + diag(c_scale) * C, with c_scale an f32 vector of
     M row factors -- the online-softmax rescale of a running accumulator.
     Its pointer is the last operand. NONE: D = A * B + C. */
  MtlcTensorScaleMode c_scale_mode;
  /* 0, or 32, 64 or 128: the operand sits in workgroup memory permuted as
     a tensor-map load with that swizzle leaves it -- by byte address p,
     the 16-byte chunk at p is stored at p ^ (((p >> 7) & (S/16 - 1)) << 4),
     with S/16 - 1 = 1, 3 or 7. Its rows are then exactly S bytes. */
  uint8_t a_swizzle;
  uint8_t b_swizzle;
  /* INVALID, or FLOAT16 / BFLOAT16 under f32 block scales: every scale
     value is exactly representable in that narrower format (widened when
     it was stored), a runtime precondition. A backend may then scale as
     exactly as it would scales stored narrow. */
  MtlcTensorElement a_scale_values;
  MtlcTensorElement b_scale_values;
} MtlcTensorMmaDesc;

typedef struct {
  uint16_t m;
  uint16_t n;
  MtlcTensorElement element;
  MtlcTensorLayout layout;
  uint32_t leading_dimension;
  MtlcTensorBiasMode bias_mode;
  MtlcTensorLayout bias_layout;
  uint32_t bias_leading_dimension;
  MtlcTensorActivation activation;
  uint8_t scale_output;
  uint8_t scale_bias;
  MtlcMemoryScope scope;
} MtlcTensorEpilogueDesc;

static inline MtlcTensorMmaDesc
mtlc_tensor_mma_f16_f32_m16n16k16_desc(void) {
  MtlcTensorMmaDesc desc = {0};
  desc.m = 16;
  desc.n = 16;
  desc.k = 16;
  desc.a_element = MTLC_TENSOR_ELEMENT_FLOAT16;
  desc.b_element = MTLC_TENSOR_ELEMENT_FLOAT16;
  desc.accumulator_element = MTLC_TENSOR_ELEMENT_FLOAT32;
  desc.result_element = MTLC_TENSOR_ELEMENT_FLOAT32;
  desc.a_layout = MTLC_TENSOR_LAYOUT_ROW_MAJOR;
  desc.b_layout = MTLC_TENSOR_LAYOUT_COLUMN_MAJOR;
  desc.c_layout = MTLC_TENSOR_LAYOUT_ROW_MAJOR;
  desc.d_layout = MTLC_TENSOR_LAYOUT_ROW_MAJOR;
  desc.a_leading_dimension = 16;
  desc.b_leading_dimension = 16;
  desc.c_leading_dimension = 16;
  desc.d_leading_dimension = 16;
  desc.scope = MTLC_MEMORY_SCOPE_SUBGROUP;
  return desc;
}

int mtlc_tensor_mma_desc_is_valid(const MtlcTensorMmaDesc *desc);

int mtlc_tensor_epilogue_desc_is_valid(
    const MtlcTensorEpilogueDesc *desc);

int mtlc_tensor_transfer_desc_is_valid(const MtlcTensorTransferDesc *desc);

#ifdef __cplusplus
}
#endif

#endif
