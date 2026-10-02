#ifndef PTX_EMITTER_H
#define PTX_EMITTER_H

#include "code_generator.h"
#include "ir/ir.h"
#include <stdio.h>

typedef struct {
  const char *target;
  int isa_major;
  int isa_minor;
  int tensor_tuple_budget;
  int checks;
  int report_types;
} PtxEmitOptions;

int ptx_emit_program(IRProgram *program, CodeGenerator *generator, FILE *out,
                     const PtxEmitOptions *options, char **error);

const char *ptx_tile_report_text(void);

#endif
