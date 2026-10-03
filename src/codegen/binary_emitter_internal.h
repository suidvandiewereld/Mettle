#ifndef BINARY_EMITTER_INTERNAL_H
#define BINARY_EMITTER_INTERNAL_H

#include "binary_emitter.h"

#define BINARY_EMITTER_SECTION_INDEX_NONE ((size_t)-1)

void binary_emitter_record_error(BinaryEmitter *emitter, const char *message);

int binary_emitter_lookup_symbol_index(const BinaryEmitter *emitter,
                                       const char *name);

int binary_emitter_write_elf_object_file(BinaryEmitter *emitter,
                                         const char *filename);

int binary_emitter_write_macho_object_file(BinaryEmitter *emitter,
                                           const char *filename);

#endif
