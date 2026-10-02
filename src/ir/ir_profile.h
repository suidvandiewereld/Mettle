#ifndef IR_PROFILE_H
#define IR_PROFILE_H

#include "ir.h"

int ir_profile_instrument_program(IRProgram *program);
int ir_profile_instrument_operation_counters(IRProgram *program);

int ir_profile_instrument_blocks(IRProgram *program);

int ir_profile_instruction_is_block(const IRInstruction *instruction,
                                    uint32_t *block_id_out);

uint32_t ir_profile_registry_add(IRProgram *program, const char *name,
                                 const char *filename, uint64_t line);

uint32_t ir_profile_register_inline_site(IRProgram *program,
                                         const char *callee_name,
                                         size_t inline_site_id,
                                         SourceLocation call_site);

int ir_profile_instruction_is_enter(const IRInstruction *instruction,
                                    uint32_t *profile_id_out);

int ir_profile_build_enter_instruction(IRInstruction *instruction,
                                       uint32_t profile_id,
                                       SourceLocation location);

#endif
