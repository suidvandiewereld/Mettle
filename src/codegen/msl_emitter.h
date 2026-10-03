#ifndef MSL_EMITTER_H
#define MSL_EMITTER_H

#include "ir/ir.h"
#include <stdio.h>

typedef struct {
  int version_major;
  int version_minor;
  int fast_math;
  int gpu_checks;
} MslEmitOptions;

void msl_emit_default_options(MslEmitOptions *options);

int msl_parse_version(const char *text, MslEmitOptions *options);

int msl_emit_program(IRProgram *program, FILE *out,
                     const MslEmitOptions *options, char **error);

#endif
