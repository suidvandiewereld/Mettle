#ifndef CODEGEN_BINARY_MIR_MACHINE_H
#define CODEGEN_BINARY_MIR_MACHINE_H

#include "codegen/binary/mir.h"

#include <stddef.h>

typedef enum {
  MIR_OPF_PURE_DEF = 1u << 0,
  MIR_OPF_COMMUTATIVE = 1u << 1,
  MIR_OPF_COALESCE_CANDIDATE = 1u << 2,
  MIR_OPF_CALL_BARRIER = 1u << 3,
  MIR_OPF_REAL_CALL = 1u << 4,
  MIR_OPF_INLINE_KERNEL = 1u << 5,
  MIR_OPF_PINS_RCX_WHEN_SHIFT_IS_VARIABLE = 1u << 6,
  MIR_OPF_PINS_RCX_WHEN_COMPARE_IS_UNORDERED = 1u << 7,
  MIR_OPF_CLOBBERS_EVERY_GP = 1u << 8,
  MIR_OPF_CLOBBERS_LISTED_BY_KERNEL = 1u << 9,
  MIR_OPF_LABEL_BRANCH = 1u << 10,
  MIR_OPF_CONDITIONAL_BRANCH = 1u << 11
} MirOpFlag;

typedef struct {
  unsigned flags;
  unsigned fixed_gp;
} MirOpInfo;

typedef struct {
  MirRegClass rclass;
  const int *allocation_order;
  size_t allocation_order_count;
} MirRegBank;

typedef struct {
  const char *name;
  const MirRegBank *banks;
  size_t bank_count;
  unsigned pinned_gp;
  int gp_register_count;
} MirMachine;

const MirOpInfo *mir_op_info(MirOpcode op);
int mir_op_has(MirOpcode op, unsigned flags);
unsigned mir_op_fixed_gp(MirOpcode op);
unsigned mir_inst_fixed_gp(const MirInst *in);
int mir_inst_pins_gp(const MirInst *in, int reg);

const MirMachine *mir_machine(void);
int mir_machine_gp_is_pinned(int reg);

#endif
