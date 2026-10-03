#include "binary_emitter.h"
#include "binary_emitter_internal.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MACHO_MAGIC_64 0xfeedfacfu
#define MACHO_FILETYPE_OBJECT 0x1u
#define MACHO_CPU_TYPE_X86_64 0x01000007u
#define MACHO_CPU_TYPE_ARM64 0x0100000cu
#define MACHO_CPU_SUBTYPE_X86_64_ALL 3u
#define MACHO_CPU_SUBTYPE_ARM64_ALL 0u

#define MACHO_LC_SYMTAB 0x2u
#define MACHO_LC_DYSYMTAB 0xbu
#define MACHO_LC_SEGMENT_64 0x19u
#define MACHO_LC_BUILD_VERSION 0x32u
#define MACHO_PLATFORM_MACOS 1u
#define MACHO_MIN_OS_VERSION 0x000b0000u

#define MACHO_HEADER_SIZE 32u
#define MACHO_SEGMENT_SIZE 72u
#define MACHO_SECTION_SIZE 80u
#define MACHO_BUILD_VERSION_SIZE 24u
#define MACHO_SYMTAB_SIZE 24u
#define MACHO_DYSYMTAB_SIZE 80u
#define MACHO_NLIST_SIZE 16u
#define MACHO_RELOC_SIZE 8u

#define MACHO_S_REGULAR 0x0u
#define MACHO_S_ZEROFILL 0x1u
#define MACHO_S_MOD_INIT_FUNC_POINTERS 0x9u
#define MACHO_S_MOD_TERM_FUNC_POINTERS 0xau
#define MACHO_S_ATTR_PURE_INSTRUCTIONS 0x80000000u
#define MACHO_S_ATTR_SOME_INSTRUCTIONS 0x00000400u

#define MACHO_N_EXT 0x01u
#define MACHO_N_SECT 0x0eu

#define MACHO_X86_64_RELOC_UNSIGNED 0u
#define MACHO_X86_64_RELOC_SIGNED 1u
#define MACHO_X86_64_RELOC_BRANCH 2u
#define MACHO_X86_64_RELOC_GOT_LOAD 3u
#define MACHO_X86_64_RELOC_SIGNED_1 6u
#define MACHO_X86_64_RELOC_SIGNED_2 7u
#define MACHO_X86_64_RELOC_SIGNED_4 8u

#define MACHO_ARM64_RELOC_UNSIGNED 0u
#define MACHO_ARM64_RELOC_BRANCH26 2u
#define MACHO_ARM64_RELOC_PAGE21 3u
#define MACHO_ARM64_RELOC_PAGEOFF12 4u
#define MACHO_ARM64_RELOC_GOT_LOAD_PAGE21 5u
#define MACHO_ARM64_RELOC_GOT_LOAD_PAGEOFF12 6u
#define MACHO_ARM64_RELOC_ADDEND 10u

#define MACHO_SLOT_TEXT 0
#define MACHO_SLOT_TEXT_CONST 1
#define MACHO_SLOT_DATA_CONST 2
#define MACHO_SLOT_DATA 3
#define MACHO_SLOT_INIT 4
#define MACHO_SLOT_FINI 5
#define MACHO_SLOT_BSS 6
#define MACHO_SLOT_COUNT 7
#define MACHO_SLOT_NONE (-1)

typedef struct {
  const char *segname;
  const char *sectname;
  uint32_t flags;
  int zerofill;
} MachSlotSpec;

static const MachSlotSpec MACH_SLOTS[MACHO_SLOT_COUNT] = {
    {"__TEXT", "__text",
     MACHO_S_REGULAR | MACHO_S_ATTR_PURE_INSTRUCTIONS |
         MACHO_S_ATTR_SOME_INSTRUCTIONS,
     0},
    {"__TEXT", "__const", MACHO_S_REGULAR, 0},
    {"__DATA", "__const", MACHO_S_REGULAR, 0},
    {"__DATA", "__data", MACHO_S_REGULAR, 0},
    {"__DATA", "__mod_init_func", MACHO_S_MOD_INIT_FUNC_POINTERS, 0},
    {"__DATA", "__mod_term_func", MACHO_S_MOD_TERM_FUNC_POINTERS, 0},
    {"__DATA", "__bss", MACHO_S_ZEROFILL, 1},
};

typedef struct {
  unsigned char *bytes;
  size_t size;
  size_t capacity;
} MachBuffer;

typedef struct {
  int used;
  uint8_t number;
  uint64_t alignment;
  uint64_t size;
  uint64_t addr;
  uint32_t offset;
  uint32_t reloff;
  uint32_t nreloc;
  MachBuffer data;
  MachBuffer relocs;
} MachSlot;

typedef struct {
  const char *name;
  size_t source;
  int group;
} MachSymbolRef;

typedef struct {
  BinaryEmitter *emitter;
  int arm64;
  MachSlot slots[MACHO_SLOT_COUNT];
  int *section_slot;
  uint64_t *section_base;
  uint32_t *symbol_index;
  MachSymbolRef *order;
  size_t order_count;
  size_t local_count;
  size_t extdef_count;
  size_t undef_count;
  MachBuffer strtab;
  uint32_t *name_offset;
  int failed;
} MachWriter;

static void mach_fail(MachWriter *w, const char *message) {
  if (!w->failed) {
    binary_emitter_record_error(w->emitter, message);
  }
  w->failed = 1;
}

static int mach_reserve(MachBuffer *buffer, size_t extra) {
  size_t need = buffer->size + extra;
  size_t next;
  unsigned char *grown;
  if (need <= buffer->capacity) {
    return 1;
  }
  next = buffer->capacity ? buffer->capacity : 256;
  while (next < need) {
    next *= 2;
  }
  grown = (unsigned char *)realloc(buffer->bytes, next);
  if (!grown) {
    return 0;
  }
  buffer->bytes = grown;
  buffer->capacity = next;
  return 1;
}

static int mach_append(MachBuffer *buffer, const void *bytes, size_t size) {
  if (!mach_reserve(buffer, size)) {
    return 0;
  }
  if (size) {
    if (bytes) {
      memcpy(buffer->bytes + buffer->size, bytes, size);
    } else {
      memset(buffer->bytes + buffer->size, 0, size);
    }
  }
  buffer->size += size;
  return 1;
}

static void mach_put16(unsigned char *p, uint16_t v) {
  p[0] = (unsigned char)v;
  p[1] = (unsigned char)(v >> 8);
}

static void mach_put32(unsigned char *p, uint32_t v) {
  p[0] = (unsigned char)v;
  p[1] = (unsigned char)(v >> 8);
  p[2] = (unsigned char)(v >> 16);
  p[3] = (unsigned char)(v >> 24);
}

static void mach_put64(unsigned char *p, uint64_t v) {
  mach_put32(p, (uint32_t)v);
  mach_put32(p + 4, (uint32_t)(v >> 32));
}

static uint32_t mach_get32(const unsigned char *p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
         ((uint32_t)p[3] << 24);
}

static uint64_t mach_align_up(uint64_t value, uint64_t alignment) {
  if (alignment <= 1) {
    return value;
  }
  return (value + alignment - 1) / alignment * alignment;
}

static uint32_t mach_log2(uint64_t value) {
  uint32_t result = 0;
  while (((uint64_t)1 << result) < value && result < 15) {
    result++;
  }
  return result;
}

static uint64_t mach_section_size(const BinarySection *section) {
  if (section->kind == BINARY_SECTION_BSS &&
      section->virtual_size > section->size) {
    return section->virtual_size;
  }
  return section->size;
}

static int mach_section_has_relocations(const BinaryEmitter *e, size_t index) {
  for (size_t i = 0; i < e->relocation_count; i++) {
    if (e->relocations[i].section_index == index) {
      return 1;
    }
  }
  return 0;
}

static int mach_slot_for(const BinaryEmitter *e, size_t index) {
  switch (e->sections[index].kind) {
  case BINARY_SECTION_TEXT:
    return MACHO_SLOT_TEXT;
  case BINARY_SECTION_RDATA:
    return mach_section_has_relocations(e, index) ? MACHO_SLOT_DATA_CONST
                                                  : MACHO_SLOT_TEXT_CONST;
  case BINARY_SECTION_DATA:
    return MACHO_SLOT_DATA;
  case BINARY_SECTION_BSS:
    return MACHO_SLOT_BSS;
  case BINARY_SECTION_INIT_ARRAY:
    return MACHO_SLOT_INIT;
  case BINARY_SECTION_FINI_ARRAY:
    return MACHO_SLOT_FINI;
  case BINARY_SECTION_DEBUG:
  default:
    return MACHO_SLOT_NONE;
  }
}

static void mach_collect_sections(MachWriter *w) {
  BinaryEmitter *e = w->emitter;
  size_t count = e->section_count ? e->section_count : 1;
  w->section_slot = (int *)calloc(count, sizeof(int));
  w->section_base = (uint64_t *)calloc(count, sizeof(uint64_t));
  if (!w->section_slot || !w->section_base) {
    mach_fail(w, "Out of memory preparing Mach-O sections");
    return;
  }
  for (size_t i = 0; i < e->section_count; i++) {
    const BinarySection *section = &e->sections[i];
    int slot_index = mach_slot_for(e, i);
    MachSlot *slot;
    uint64_t alignment = section->alignment ? section->alignment : 1;
    uint64_t size = mach_section_size(section);
    uint64_t start;
    w->section_slot[i] = slot_index;
    if (slot_index == MACHO_SLOT_NONE) {
      continue;
    }
    slot = &w->slots[slot_index];
    if (!slot->used) {
      slot->used = 1;
      slot->alignment = 1;
    }
    if (alignment > slot->alignment) {
      slot->alignment = alignment;
    }
    start = mach_align_up(slot->size, alignment);
    w->section_base[i] = start;
    if (!MACH_SLOTS[slot_index].zerofill) {
      unsigned char fill = slot_index == MACHO_SLOT_TEXT ? 0x90 : 0;
      if (w->arm64) {
        fill = 0;
      }
      while (slot->data.size < start) {
        if (!mach_append(&slot->data, &fill, 1)) {
          mach_fail(w, "Out of memory copying Mach-O section data");
          return;
        }
      }
      if (!mach_append(&slot->data, section->data, section->size) ||
          !mach_append(&slot->data, NULL, (size_t)(size - section->size))) {
        mach_fail(w, "Out of memory copying Mach-O section data");
        return;
      }
    }
    slot->size = start + size;
  }
}

static void mach_layout_addresses(MachWriter *w, uint32_t *nsects,
                                  uint64_t *vmsize) {
  uint64_t addr = 0;
  uint8_t number = 1;
  *nsects = 0;
  for (int i = 0; i < MACHO_SLOT_COUNT; i++) {
    MachSlot *slot = &w->slots[i];
    if (!slot->used) {
      continue;
    }
    slot->number = number++;
    addr = mach_align_up(addr, slot->alignment);
    slot->addr = addr;
    addr += slot->size;
    (*nsects)++;
  }
  *vmsize = addr;
}

static int mach_compare_symbols(const void *a, const void *b) {
  const MachSymbolRef *left = (const MachSymbolRef *)a;
  const MachSymbolRef *right = (const MachSymbolRef *)b;
  int order;
  if (left->group != right->group) {
    return left->group < right->group ? -1 : 1;
  }
  if (left->group == 0) {
    return left->source < right->source ? -1 : (left->source > right->source);
  }
  order = strcmp(left->name ? left->name : "", right->name ? right->name : "");
  if (order) {
    return order;
  }
  return left->source < right->source ? -1 : (left->source > right->source);
}

static void mach_collect_symbols(MachWriter *w) {
  BinaryEmitter *e = w->emitter;
  size_t count = e->symbol_count ? e->symbol_count : 1;
  w->symbol_index = (uint32_t *)malloc(count * sizeof(uint32_t));
  w->order = (MachSymbolRef *)calloc(count, sizeof(MachSymbolRef));
  w->name_offset = (uint32_t *)calloc(count, sizeof(uint32_t));
  if (!w->symbol_index || !w->order || !w->name_offset) {
    mach_fail(w, "Out of memory preparing Mach-O symbols");
    return;
  }
  for (size_t i = 0; i < e->symbol_count; i++) {
    const BinarySymbol *symbol = &e->symbols[i];
    MachSymbolRef *ref;
    int group;
    w->symbol_index[i] = UINT32_MAX;
    if (symbol->section_index == BINARY_EMITTER_SECTION_INDEX_NONE) {
      group = 2;
    } else if (symbol->section_index >= e->section_count) {
      mach_fail(w, "Mach-O symbol refers to an invalid section");
      return;
    } else if (w->section_slot[symbol->section_index] == MACHO_SLOT_NONE) {
      continue;
    } else {
      group = symbol->binding == BINARY_SYMBOL_LOCAL ? 0 : 1;
    }
    ref = &w->order[w->order_count++];
    ref->name = symbol->name;
    ref->source = i;
    ref->group = group;
    if (group == 0) {
      w->local_count++;
    } else if (group == 1) {
      w->extdef_count++;
    } else {
      w->undef_count++;
    }
  }
  qsort(w->order, w->order_count, sizeof(MachSymbolRef), mach_compare_symbols);
  if (!mach_append(&w->strtab, " ", 2)) {
    mach_fail(w, "Out of memory building the Mach-O string table");
    return;
  }
  for (size_t i = 0; i < w->order_count; i++) {
    const char *name = w->order[i].name ? w->order[i].name : "";
    w->symbol_index[w->order[i].source] = (uint32_t)i;
    w->name_offset[i] = (uint32_t)w->strtab.size;
    if (!mach_append(&w->strtab, "_", 1) ||
        !mach_append(&w->strtab, name, strlen(name) + 1)) {
      mach_fail(w, "Out of memory building the Mach-O string table");
      return;
    }
  }
  while (w->strtab.size % 8) {
    if (!mach_append(&w->strtab, NULL, 1)) {
      mach_fail(w, "Out of memory building the Mach-O string table");
      return;
    }
  }
}

static int mach_add_reloc(MachSlot *slot, uint32_t address, uint32_t symbolnum,
                          int pcrel, uint32_t length, int external,
                          uint32_t type) {
  unsigned char entry[MACHO_RELOC_SIZE];
  uint32_t info = (symbolnum & 0xffffffu) | ((uint32_t)(pcrel ? 1 : 0) << 24) |
                  ((length & 3u) << 25) | ((uint32_t)(external ? 1 : 0) << 27) |
                  ((type & 15u) << 28);
  mach_put32(entry, address);
  mach_put32(entry + 4, info);
  if (!mach_append(&slot->relocs, entry, sizeof(entry))) {
    return 0;
  }
  slot->nreloc++;
  return 1;
}

static int mach_x64_is_branch(const MachSlot *slot, int slot_index,
                              uint64_t address, size_t section_offset) {
  const unsigned char *p = slot->data.bytes + address;
  if (slot_index != MACHO_SLOT_TEXT || section_offset < 1) {
    return 0;
  }
  if (p[-1] == 0xE8 || p[-1] == 0xE9) {
    return 1;
  }
  return section_offset >= 2 && p[-2] == 0x0F && (p[-1] & 0xF0) == 0x80;
}

static int mach_x64_is_lea(const MachSlot *slot, int slot_index,
                           uint64_t address, size_t section_offset) {
  const unsigned char *p = slot->data.bytes + address;
  if (slot_index != MACHO_SLOT_TEXT || section_offset < 3) {
    return 0;
  }
  return (p[-3] & 0xFB) == 0x48 && p[-2] == 0x8D && (p[-1] & 0xC7) == 0x05;
}

static int mach_x64_reloc(MachWriter *w, MachSlot *slot, int slot_index,
                          uint64_t address, const BinaryRelocation *reloc,
                          uint32_t symbol, int undefined) {
  unsigned char *p = slot->data.bytes + address;
  uint32_t type;
  switch (reloc->kind) {
  case BINARY_RELOCATION_REL32:
    if (mach_x64_is_branch(slot, slot_index, address, reloc->offset)) {
      type = MACHO_X86_64_RELOC_BRANCH;
    } else if (undefined && reloc->addend == 0 &&
               mach_x64_is_lea(slot, slot_index, address, reloc->offset)) {
      p[-2] = 0x8B;
      type = MACHO_X86_64_RELOC_GOT_LOAD;
    } else if (reloc->addend == -1) {
      type = MACHO_X86_64_RELOC_SIGNED_1;
    } else if (reloc->addend == -2) {
      type = MACHO_X86_64_RELOC_SIGNED_2;
    } else if (reloc->addend == -4) {
      type = MACHO_X86_64_RELOC_SIGNED_4;
    } else {
      type = MACHO_X86_64_RELOC_SIGNED;
    }
    mach_put32(p, (uint32_t)reloc->addend);
    return mach_add_reloc(slot, (uint32_t)address, symbol, 1, 2, 1, type);
  case BINARY_RELOCATION_ADDR64:
    if (slot_index == MACHO_SLOT_TEXT) {
      mach_fail(w, "Mach-O __text cannot hold an absolute address");
      return 0;
    }
    mach_put64(p, (uint64_t)(int64_t)reloc->addend);
    return mach_add_reloc(slot, (uint32_t)address, symbol, 0, 3, 1,
                          MACHO_X86_64_RELOC_UNSIGNED);
  default:
    mach_fail(w, "Unsupported relocation kind for x86-64 Mach-O output");
    return 0;
  }
}

static int mach_arm64_addend(MachWriter *w, MachSlot *slot, uint64_t address,
                             int32_t addend) {
  if (addend == 0) {
    return 1;
  }
  if (addend < -0x800000 || addend > 0x7fffff) {
    mach_fail(w, "arm64 Mach-O relocation addend does not fit in 24 bits");
    return 0;
  }
  return mach_add_reloc(slot, (uint32_t)address, (uint32_t)addend, 0, 2, 0,
                        MACHO_ARM64_RELOC_ADDEND);
}

static int mach_arm64_got_reloc(MachWriter *w, MachSlot *slot,
                                uint64_t address, const BinaryRelocation *reloc,
                                uint32_t symbol) {
  unsigned char *p = slot->data.bytes + address;
  uint32_t insn = mach_get32(p);
  if (reloc->addend != 0) {
    mach_fail(w, "arm64 Mach-O reaches an undefined symbol through the GOT, "
                 "which cannot carry an offset");
    return 0;
  }
  if (reloc->kind == BINARY_RELOCATION_ARM64_ADR_PREL_PG_HI21) {
    mach_put32(p, insn & 0x9F00001Fu);
    return mach_add_reloc(slot, (uint32_t)address, symbol, 1, 2, 1,
                          MACHO_ARM64_RELOC_GOT_LOAD_PAGE21);
  }
  if ((insn & 0xFFC00000u) != 0x91000000u) {
    mach_fail(w, "arm64 Mach-O GOT load needs a 64-bit add of the page offset");
    return 0;
  }
  mach_put32(p, 0xF9400000u | (insn & 0x3FFu));
  return mach_add_reloc(slot, (uint32_t)address, symbol, 0, 2, 1,
                        MACHO_ARM64_RELOC_GOT_LOAD_PAGEOFF12);
}

static int mach_arm64_reloc(MachWriter *w, MachSlot *slot, int slot_index,
                            uint64_t address, const BinaryRelocation *reloc,
                            uint32_t symbol, int undefined) {
  unsigned char *p = slot->data.bytes + address;
  uint32_t insn;
  if (undefined && (reloc->kind == BINARY_RELOCATION_ARM64_ADR_PREL_PG_HI21 ||
                    reloc->kind == BINARY_RELOCATION_ARM64_ADD_ABS_LO12_NC)) {
    return mach_arm64_got_reloc(w, slot, address, reloc, symbol);
  }
  switch (reloc->kind) {
  case BINARY_RELOCATION_ARM64_CALL26:
    insn = mach_get32(p) & 0xFC000000u;
    mach_put32(p, insn);
    return mach_arm64_addend(w, slot, address, reloc->addend) &&
           mach_add_reloc(slot, (uint32_t)address, symbol, 1, 2, 1,
                          MACHO_ARM64_RELOC_BRANCH26);
  case BINARY_RELOCATION_ARM64_ADR_PREL_PG_HI21:
    insn = mach_get32(p) & 0x9F00001Fu;
    mach_put32(p, insn);
    return mach_arm64_addend(w, slot, address, reloc->addend) &&
           mach_add_reloc(slot, (uint32_t)address, symbol, 1, 2, 1,
                          MACHO_ARM64_RELOC_PAGE21);
  case BINARY_RELOCATION_ARM64_ADD_ABS_LO12_NC:
    insn = mach_get32(p) & 0xFFC003FFu;
    mach_put32(p, insn);
    return mach_arm64_addend(w, slot, address, reloc->addend) &&
           mach_add_reloc(slot, (uint32_t)address, symbol, 0, 2, 1,
                          MACHO_ARM64_RELOC_PAGEOFF12);
  case BINARY_RELOCATION_ADDR64:
    if (slot_index == MACHO_SLOT_TEXT) {
      mach_fail(w, "Mach-O __text cannot hold an absolute address");
      return 0;
    }
    mach_put64(p, (uint64_t)(int64_t)reloc->addend);
    return mach_add_reloc(slot, (uint32_t)address, symbol, 0, 3, 1,
                          MACHO_ARM64_RELOC_UNSIGNED);
  default:
    mach_fail(w, "Unsupported relocation kind for arm64 Mach-O output");
    return 0;
  }
}

static uint64_t mach_reloc_width(BinaryRelocationKind kind) {
  return kind == BINARY_RELOCATION_ADDR64 ? 8 : 4;
}

static void mach_collect_relocations(MachWriter *w) {
  BinaryEmitter *e = w->emitter;
  for (size_t i = 0; i < e->relocation_count && !w->failed; i++) {
    const BinaryRelocation *reloc = &e->relocations[i];
    int slot_index;
    MachSlot *slot;
    uint64_t address;
    int symbol;
    uint32_t mach_symbol;
    if (reloc->section_index >= e->section_count) {
      mach_fail(w, "Mach-O relocation refers to an invalid section");
      return;
    }
    slot_index = w->section_slot[reloc->section_index];
    if (slot_index == MACHO_SLOT_NONE) {
      continue;
    }
    slot = &w->slots[slot_index];
    if (MACH_SLOTS[slot_index].zerofill) {
      mach_fail(w, "Mach-O zero-fill section cannot carry relocations");
      return;
    }
    address = w->section_base[reloc->section_index] + reloc->offset;
    if (address + mach_reloc_width(reloc->kind) > slot->data.size ||
        address > 0x7fffffffu) {
      mach_fail(w, "Mach-O relocation lies outside its section");
      return;
    }
    symbol = binary_emitter_lookup_symbol_index(e, reloc->symbol_name);
    if (symbol < 0) {
      mach_fail(w, "Mach-O relocation refers to an undefined symbol");
      return;
    }
    mach_symbol = w->symbol_index[symbol];
    if (mach_symbol == UINT32_MAX) {
      mach_fail(w, "Mach-O relocation refers to a symbol in a debug section");
      return;
    }
    if (w->arm64) {
      mach_arm64_reloc(w, slot, slot_index, address, reloc, mach_symbol,
                       w->order[mach_symbol].group == 2);
    } else {
      mach_x64_reloc(w, slot, slot_index, address, reloc, mach_symbol,
                     w->order[mach_symbol].group == 2);
    }
  }
}

static void mach_put_name16(unsigned char *p, const char *name) {
  size_t length = strlen(name);
  memset(p, 0, 16);
  memcpy(p, name, length > 16 ? 16 : length);
}

static int mach_build_image(MachWriter *w, MachBuffer *image) {
  BinaryEmitter *e = w->emitter;
  uint32_t nsects;
  uint64_t vmsize;
  uint32_t sizeofcmds;
  uint64_t header_end;
  uint64_t max_align = 1;
  uint64_t data_start;
  uint64_t data_end;
  uint64_t cursor;
  uint32_t symoff;
  uint32_t stroff;
  unsigned char *p;
  size_t total;

  mach_layout_addresses(w, &nsects, &vmsize);
  sizeofcmds = MACHO_SEGMENT_SIZE + MACHO_SECTION_SIZE * nsects +
               MACHO_BUILD_VERSION_SIZE + MACHO_SYMTAB_SIZE +
               MACHO_DYSYMTAB_SIZE;
  header_end = MACHO_HEADER_SIZE + sizeofcmds;
  for (int i = 0; i < MACHO_SLOT_COUNT; i++) {
    if (w->slots[i].used && !MACH_SLOTS[i].zerofill &&
        w->slots[i].alignment > max_align) {
      max_align = w->slots[i].alignment;
    }
  }
  data_start = mach_align_up(header_end, max_align);
  data_end = data_start;
  for (int i = 0; i < MACHO_SLOT_COUNT; i++) {
    MachSlot *slot = &w->slots[i];
    if (!slot->used || MACH_SLOTS[i].zerofill) {
      continue;
    }
    slot->offset = (uint32_t)(data_start + slot->addr);
    if (slot->offset + slot->size > data_end) {
      data_end = slot->offset + slot->size;
    }
  }
  cursor = mach_align_up(data_end, 8);
  for (int i = 0; i < MACHO_SLOT_COUNT; i++) {
    MachSlot *slot = &w->slots[i];
    if (!slot->used || slot->nreloc == 0) {
      continue;
    }
    slot->reloff = (uint32_t)cursor;
    cursor += slot->relocs.size;
  }
  cursor = mach_align_up(cursor, 8);
  symoff = (uint32_t)cursor;
  cursor += (uint64_t)w->order_count * MACHO_NLIST_SIZE;
  stroff = (uint32_t)cursor;
  cursor += w->strtab.size;
  if (cursor > 0xffffffffu) {
    mach_fail(w, "Mach-O object is larger than 4 GiB");
    return 0;
  }
  total = (size_t)cursor;
  if (!mach_append(image, NULL, total)) {
    mach_fail(w, "Out of memory writing the Mach-O image");
    return 0;
  }
  p = image->bytes;

  mach_put32(p + 0, MACHO_MAGIC_64);
  mach_put32(p + 4, w->arm64 ? MACHO_CPU_TYPE_ARM64 : MACHO_CPU_TYPE_X86_64);
  mach_put32(p + 8, w->arm64 ? MACHO_CPU_SUBTYPE_ARM64_ALL
                             : MACHO_CPU_SUBTYPE_X86_64_ALL);
  mach_put32(p + 12, MACHO_FILETYPE_OBJECT);
  mach_put32(p + 16, 4);
  mach_put32(p + 20, sizeofcmds);
  mach_put32(p + 24, 0);
  mach_put32(p + 28, 0);
  p += MACHO_HEADER_SIZE;

  mach_put32(p + 0, MACHO_LC_SEGMENT_64);
  mach_put32(p + 4, MACHO_SEGMENT_SIZE + MACHO_SECTION_SIZE * nsects);
  mach_put_name16(p + 8, "");
  mach_put64(p + 24, 0);
  mach_put64(p + 32, vmsize);
  mach_put64(p + 40, data_start);
  mach_put64(p + 48, data_end - data_start);
  mach_put32(p + 56, 7);
  mach_put32(p + 60, 7);
  mach_put32(p + 64, nsects);
  mach_put32(p + 68, 0);
  p += MACHO_SEGMENT_SIZE;
  for (int i = 0; i < MACHO_SLOT_COUNT; i++) {
    MachSlot *slot = &w->slots[i];
    if (!slot->used) {
      continue;
    }
    mach_put_name16(p + 0, MACH_SLOTS[i].sectname);
    mach_put_name16(p + 16, MACH_SLOTS[i].segname);
    mach_put64(p + 32, slot->addr);
    mach_put64(p + 40, slot->size);
    mach_put32(p + 48, MACH_SLOTS[i].zerofill ? 0 : slot->offset);
    mach_put32(p + 52, mach_log2(slot->alignment));
    mach_put32(p + 56, slot->nreloc ? slot->reloff : 0);
    mach_put32(p + 60, slot->nreloc);
    mach_put32(p + 64, MACH_SLOTS[i].flags);
    mach_put32(p + 68, 0);
    mach_put32(p + 72, 0);
    mach_put32(p + 76, 0);
    p += MACHO_SECTION_SIZE;
  }

  mach_put32(p + 0, MACHO_LC_BUILD_VERSION);
  mach_put32(p + 4, MACHO_BUILD_VERSION_SIZE);
  mach_put32(p + 8, MACHO_PLATFORM_MACOS);
  mach_put32(p + 12, MACHO_MIN_OS_VERSION);
  mach_put32(p + 16, 0);
  mach_put32(p + 20, 0);
  p += MACHO_BUILD_VERSION_SIZE;

  mach_put32(p + 0, MACHO_LC_SYMTAB);
  mach_put32(p + 4, MACHO_SYMTAB_SIZE);
  mach_put32(p + 8, symoff);
  mach_put32(p + 12, (uint32_t)w->order_count);
  mach_put32(p + 16, stroff);
  mach_put32(p + 20, (uint32_t)w->strtab.size);
  p += MACHO_SYMTAB_SIZE;

  mach_put32(p + 0, MACHO_LC_DYSYMTAB);
  mach_put32(p + 4, MACHO_DYSYMTAB_SIZE);
  mach_put32(p + 8, 0);
  mach_put32(p + 12, (uint32_t)w->local_count);
  mach_put32(p + 16, (uint32_t)w->local_count);
  mach_put32(p + 20, (uint32_t)w->extdef_count);
  mach_put32(p + 24, (uint32_t)(w->local_count + w->extdef_count));
  mach_put32(p + 28, (uint32_t)w->undef_count);

  for (int i = 0; i < MACHO_SLOT_COUNT; i++) {
    MachSlot *slot = &w->slots[i];
    if (!slot->used || MACH_SLOTS[i].zerofill) {
      continue;
    }
    if (slot->data.size) {
      memcpy(image->bytes + slot->offset, slot->data.bytes, slot->data.size);
    }
    if (slot->nreloc) {
      memcpy(image->bytes + slot->reloff, slot->relocs.bytes,
             slot->relocs.size);
    }
  }

  for (size_t i = 0; i < w->order_count; i++) {
    const MachSymbolRef *ref = &w->order[i];
    const BinarySymbol *symbol = &e->symbols[ref->source];
    unsigned char *entry = image->bytes + symoff + i * MACHO_NLIST_SIZE;
    uint8_t type = 0;
    uint8_t sect = 0;
    uint64_t value = 0;
    if (ref->group != 2) {
      size_t section = symbol->section_index;
      const MachSlot *slot = &w->slots[w->section_slot[section]];
      type = MACHO_N_SECT;
      sect = slot->number;
      value = slot->addr + w->section_base[section] + symbol->value;
    }
    if (ref->group != 0) {
      type |= MACHO_N_EXT;
    }
    mach_put32(entry + 0, w->name_offset[i]);
    entry[4] = type;
    entry[5] = sect;
    mach_put16(entry + 6, 0);
    mach_put64(entry + 8, value);
  }

  memcpy(image->bytes + stroff, w->strtab.bytes, w->strtab.size);
  return 1;
}

static void mach_writer_free(MachWriter *w) {
  for (int i = 0; i < MACHO_SLOT_COUNT; i++) {
    free(w->slots[i].data.bytes);
    free(w->slots[i].relocs.bytes);
  }
  free(w->section_slot);
  free(w->section_base);
  free(w->symbol_index);
  free(w->order);
  free(w->name_offset);
  free(w->strtab.bytes);
}

int binary_emitter_write_macho_object_file(BinaryEmitter *emitter,
                                           const char *filename) {
  MachWriter w;
  MachBuffer image = {0};
  FILE *file = NULL;
  int ok = 0;
  memset(&w, 0, sizeof(w));
  w.emitter = emitter;
  w.arm64 = emitter->target_format == BINARY_TARGET_FORMAT_MACHO_ARM64;
  mach_collect_sections(&w);
  if (!w.failed) {
    mach_collect_symbols(&w);
  }
  if (!w.failed) {
    mach_collect_relocations(&w);
  }
  if (!w.failed && mach_build_image(&w, &image)) {
    file = fopen(filename, "wb");
    if (!file) {
      mach_fail(&w, "Failed to open Mach-O output file");
    } else if (fwrite(image.bytes, 1, image.size, file) != image.size) {
      mach_fail(&w, "Failed while writing Mach-O object file");
    } else {
      ok = 1;
    }
  }
  if (file) {
    if (fclose(file) != 0 && ok) {
      mach_fail(&w, "Failed while writing Mach-O object file");
      ok = 0;
    }
    if (!ok) {
      remove(filename);
    }
  }
  free(image.bytes);
  mach_writer_free(&w);
  return ok;
}
