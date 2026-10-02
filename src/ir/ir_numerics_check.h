#ifndef METTLE_IR_NUMERICS_CHECK_H
#define METTLE_IR_NUMERICS_CHECK_H

#include "ir.h"

typedef struct {
  char code[8];
  char message[2400];
} IRNumericsFailure;

int ir_numerics_program_has_contracts(const IRProgram *program);
int ir_numerics_check(IRProgram *program, IRNumericsFailure *failure,
                      char **report);
void ir_numerics_drop_harnesses(IRProgram *program);

#endif
