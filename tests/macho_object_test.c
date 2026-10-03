#include "codegen/binary_emitter.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_SECTIONS 10
#define MAX_SYMBOLS 64
#define MAX_RELOCS 96
#define MAX_MACH_SECTIONS 16

typedef struct {
  size_t index;
  BinarySectionKind kind;
  size_t size;
  size_t memory_size;
  unsigned char *occupied;
} GenSection;

typedef struct {
  char name[32];
  int defined;
  int global;
  size_t section;
  size_t value;
} GenSymbol;

typedef struct {
  size_t section;
  size_t offset;
  BinaryRelocationKind kind;
  size_t symbol;
  int32_t addend;
} GenReloc;

typedef struct {
  int arm64;
  BinaryEmitter *emitter;
  GenSection sections[MAX_SECTIONS];
  size_t section_count;
  GenSymbol symbols[MAX_SYMBOLS];
  size_t symbol_count;
  GenReloc relocs[MAX_RELOCS];
  size_t reloc_count;
} Gen;

typedef struct {
  char segname[17];
  char sectname[17];
  uint64_t addr;
  uint64_t size;
  uint32_t offset;
  uint32_t align;
  uint32_t reloff;
  uint32_t nreloc;
  uint32_t flags;
} MachSect;

typedef struct {
  char name[64];
  uint8_t type;
  uint8_t sect;
  uint64_t value;
} MachSym;

typedef struct {
  unsigned char *bytes;
  size_t size;
  uint32_t cputype;
  MachSect sections[MAX_MACH_SECTIONS];
  uint32_t nsects;
  MachSym *symbols;
  uint32_t nsyms;
  uint32_t ilocal, nlocal, iextdef, nextdef, iundef, nundef;
  int saw_build_version;
} MachFile;

static uint64_t rng_state = 0x9E3779B97F4A7C15ull;
static int failures;
static char failure_context[256];
static const char *scratch_dir = ".";

char *mettle_strdup(const char *text) {
  size_t length = strlen(text) + 1;
  char *copy = (char *)malloc(length);
  if (copy) {
    memcpy(copy, text, length);
  }
  return copy;
}

static uint32_t rng(void) {
  rng_state ^= rng_state << 13;
  rng_state ^= rng_state >> 7;
  rng_state ^= rng_state << 17;
  return (uint32_t)(rng_state >> 16);
}

static uint32_t rng_below(uint32_t n) { return n ? rng() % n : 0; }

static void fail(const char *what) {
  failures++;
  if (failures <= 20) {
    fprintf(stderr, "FAIL (%s): %s\n", failure_context, what);
  }
}

static uint32_t rd32(const unsigned char *p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
         ((uint32_t)p[3] << 24);
}

static uint64_t rd64(const unsigned char *p) {
  return (uint64_t)rd32(p) | ((uint64_t)rd32(p + 4) << 32);
}

static void wr32(unsigned char *p, uint32_t v) {
  p[0] = (unsigned char)v;
  p[1] = (unsigned char)(v >> 8);
  p[2] = (unsigned char)(v >> 16);
  p[3] = (unsigned char)(v >> 24);
}

static void wr64(unsigned char *p, uint64_t v) {
  wr32(p, (uint32_t)v);
  wr32(p + 4, (uint32_t)(v >> 32));
}

static int gen_claim(GenSection *section, size_t offset, size_t width) {
  if (offset + width > section->size) {
    return 0;
  }
  for (size_t i = 0; i < width; i++) {
    if (section->occupied[offset + i]) {
      return 0;
    }
  }
  memset(section->occupied + offset, 1, width);
  return 1;
}

static BinarySectionKind gen_kind(void) {
  static const BinarySectionKind kinds[] = {
      BINARY_SECTION_TEXT,       BINARY_SECTION_TEXT,
      BINARY_SECTION_RDATA,      BINARY_SECTION_DATA,
      BINARY_SECTION_BSS,        BINARY_SECTION_INIT_ARRAY,
      BINARY_SECTION_FINI_ARRAY, BINARY_SECTION_DEBUG};
  return kinds[rng_below(sizeof(kinds) / sizeof(kinds[0]))];
}

static void gen_sections(Gen *g) {
  g->section_count = 1 + rng_below(MAX_SECTIONS - 1);
  for (size_t i = 0; i < g->section_count; i++) {
    GenSection *section = &g->sections[i];
    char name[32];
    size_t alignment = (size_t)1 << rng_below(7);
    size_t size = rng_below(5) == 0 ? 0 : 8 + rng_below(240);
    BinarySectionKind kind = i == 0 ? BINARY_SECTION_TEXT : gen_kind();
    unsigned char *bytes;
    if (kind == BINARY_SECTION_TEXT && g->arm64) {
      alignment = alignment < 4 ? 4 : alignment;
      size &= ~(size_t)3;
    }
    if (kind == BINARY_SECTION_INIT_ARRAY || kind == BINARY_SECTION_FINI_ARRAY) {
      alignment = 8;
      size &= ~(size_t)7;
    }
    snprintf(name, sizeof(name), ".sec%zu", i);
    section->kind = kind;
    section->index = binary_emitter_get_or_create_section(g->emitter, name,
                                                          kind, 0, alignment);
    section->size = size;
    section->occupied = (unsigned char *)calloc(size + 1, 1);
    bytes = (unsigned char *)malloc(size + 1);
    for (size_t b = 0; b < size; b++) {
      bytes[b] = kind == BINARY_SECTION_BSS ? 0 : (unsigned char)rng();
    }
    binary_emitter_append_bytes(g->emitter, section->index, bytes, size, NULL);
    free(bytes);
    section->memory_size = size;
    if (kind == BINARY_SECTION_BSS && rng_below(2)) {
      section->memory_size = size + 1 + rng_below(4000);
      binary_emitter_set_section_virtual_size(g->emitter, section->index,
                                              section->memory_size);
    }
  }
}

static void gen_symbols(Gen *g) {
  for (size_t i = 0; i < g->section_count; i++) {
    GenSymbol *symbol = &g->symbols[g->symbol_count++];
    snprintf(symbol->name, sizeof(symbol->name), "anchor%zu", i);
    symbol->defined = 1;
    symbol->global = 0;
    symbol->section = i;
    symbol->value = 0;
  }
  while (g->symbol_count < MAX_SYMBOLS - 8 && rng_below(8) != 0) {
    GenSymbol *symbol = &g->symbols[g->symbol_count];
    if (rng_below(3) == 0) {
      snprintf(symbol->name, sizeof(symbol->name), "ext_%u_%zu",
               rng_below(1000), g->symbol_count);
      symbol->defined = 0;
    } else {
      size_t section = rng_below((uint32_t)g->section_count);
      snprintf(symbol->name, sizeof(symbol->name), "%c%u_%zu",
               (char)('a' + rng_below(26)), rng_below(100), g->symbol_count);
      symbol->defined = 1;
      symbol->global = (int)rng_below(2);
      symbol->section = section;
      symbol->value = rng_below((uint32_t)g->sections[section].memory_size + 1);
    }
    g->symbol_count++;
  }
  for (size_t i = 0; i < g->symbol_count; i++) {
    GenSymbol *symbol = &g->symbols[i];
    if (symbol->defined) {
      binary_emitter_define_symbol(
          g->emitter, symbol->name,
          symbol->global ? BINARY_SYMBOL_GLOBAL : BINARY_SYMBOL_LOCAL,
          g->sections[symbol->section].index, symbol->value, 0);
    } else {
      binary_emitter_declare_external(g->emitter, symbol->name);
    }
  }
}

static int symbol_usable(const Gen *g, size_t symbol, int from_debug) {
  const GenSymbol *s = &g->symbols[symbol];
  if (!s->defined) {
    return 1;
  }
  return from_debug || g->sections[s->section].kind != BINARY_SECTION_DEBUG;
}

static void gen_x64_site(GenSection *section, unsigned char *data,
                         size_t offset, GenReloc *reloc, int undefined) {
  uint32_t form = rng_below(5);
  if (undefined && section->kind == BINARY_SECTION_TEXT && offset >= 3 &&
      rng_below(2)) {
    data[offset - 3] = (unsigned char)(rng_below(2) ? 0x48 : 0x4C);
    data[offset - 2] = 0x8D;
    data[offset - 1] = (unsigned char)(0x05 | (rng_below(8) << 3));
    reloc->addend = rng_below(4) ? 0 : 8;
    return;
  }
  if (section->kind != BINARY_SECTION_TEXT || offset < 2) {
    reloc->addend = (int32_t)rng_below(64) - 16;
    return;
  }
  switch (form) {
  case 0:
    data[offset - 1] = 0xE8;
    reloc->addend = 0;
    break;
  case 1:
    data[offset - 1] = 0xE9;
    reloc->addend = (int32_t)rng_below(3) * 4;
    break;
  case 2:
    data[offset - 2] = 0x0F;
    data[offset - 1] = (unsigned char)(0x80 | rng_below(16));
    reloc->addend = 0;
    break;
  case 3: {
    static const int32_t trailers[] = {-1, -2, -4};
    data[offset - 1] = 0x05;
    reloc->addend = trailers[rng_below(3)];
    break;
  }
  default:
    data[offset - 1] = (unsigned char)(0x05 | (rng_below(8) << 3));
    reloc->addend = (int32_t)rng_below(256) - 64;
    break;
  }
}

static uint32_t gen_arm64_insn(BinaryRelocationKind kind) {
  uint32_t garbage = rng();
  switch (kind) {
  case BINARY_RELOCATION_ARM64_CALL26:
    return (rng_below(2) ? 0x94000000u : 0x14000000u) | (garbage & 0x03FFFFFFu);
  case BINARY_RELOCATION_ARM64_ADR_PREL_PG_HI21:
    return 0x90000000u | (garbage & 0x60FFFFE0u) | rng_below(31);
  case BINARY_RELOCATION_ARM64_ADD_ABS_LO12_NC:
  default:
    return 0x91000000u | (garbage & 0x003FFC00u) | (rng_below(31) << 5) |
           rng_below(31);
  }
}

static void gen_relocations(Gen *g) {
  size_t attempts = 40 + rng_below(80);
  for (size_t a = 0; a < attempts && g->reloc_count < MAX_RELOCS; a++) {
    size_t section_number = rng_below((uint32_t)g->section_count);
    GenSection *section = &g->sections[section_number];
    BinarySection *raw = binary_emitter_get_section(g->emitter, section->index);
    GenReloc *reloc = &g->relocs[g->reloc_count];
    int debug = section->kind == BINARY_SECTION_DEBUG;
    size_t width;
    size_t offset;
    size_t symbol = rng_below((uint32_t)g->symbol_count);
    if (section->kind == BINARY_SECTION_BSS || section->size < 8 ||
        !symbol_usable(g, symbol, debug)) {
      continue;
    }
    if (section->kind == BINARY_SECTION_TEXT) {
      if (g->arm64) {
        static const BinaryRelocationKind kinds[] = {
            BINARY_RELOCATION_ARM64_CALL26,
            BINARY_RELOCATION_ARM64_ADR_PREL_PG_HI21,
            BINARY_RELOCATION_ARM64_ADD_ABS_LO12_NC};
        reloc->kind = kinds[rng_below(3)];
      } else {
        reloc->kind = BINARY_RELOCATION_REL32;
      }
    } else if (!g->arm64 && rng_below(3) == 0 &&
               section->kind != BINARY_SECTION_INIT_ARRAY &&
               section->kind != BINARY_SECTION_FINI_ARRAY) {
      reloc->kind = BINARY_RELOCATION_REL32;
    } else {
      reloc->kind = BINARY_RELOCATION_ADDR64;
    }
    width = reloc->kind == BINARY_RELOCATION_ADDR64 ? 8 : 4;
    offset = rng_below((uint32_t)(section->size - width + 1));
    if (g->arm64 && section->kind == BINARY_SECTION_TEXT) {
      offset &= ~(size_t)3;
    }
    if (section->kind == BINARY_SECTION_INIT_ARRAY ||
        section->kind == BINARY_SECTION_FINI_ARRAY) {
      offset &= ~(size_t)7;
    }
    if (!g->arm64 && section->kind == BINARY_SECTION_TEXT && offset >= 3) {
      if (!gen_claim(section, offset - 3, width + 3)) {
        continue;
      }
    } else if (!g->arm64 && section->kind == BINARY_SECTION_TEXT &&
               offset >= 2) {
      if (!gen_claim(section, offset - 2, width + 2)) {
        continue;
      }
    } else if (!gen_claim(section, offset, width)) {
      continue;
    }
    reloc->section = section_number;
    reloc->offset = offset;
    reloc->symbol = symbol;
    reloc->addend = 0;
    if (g->arm64) {
      if (reloc->kind == BINARY_RELOCATION_ADDR64) {
        reloc->addend = (int32_t)rng_below(4096) - 1024;
      } else {
        reloc->addend = rng_below(2) ? 0 : (int32_t)rng_below(4096) * 4 - 4096;
        if (!g->symbols[symbol].defined &&
            reloc->kind != BINARY_RELOCATION_ARM64_CALL26) {
          reloc->addend = 0;
        }
        wr32(raw->data + offset, gen_arm64_insn(reloc->kind));
      }
    } else if (reloc->kind == BINARY_RELOCATION_REL32) {
      gen_x64_site(section, raw->data, offset, reloc,
                   !g->symbols[symbol].defined);
    } else {
      reloc->addend = (int32_t)rng_below(4096) - 1024;
    }
    binary_emitter_add_relocation(g->emitter, section->index, offset,
                                  reloc->kind, g->symbols[symbol].name,
                                  reloc->addend);
    g->reloc_count++;
  }
}

static int parse_mach(MachFile *m, const char *path) {
  FILE *file = fopen(path, "rb");
  uint32_t ncmds;
  size_t cursor;
  memset(m, 0, sizeof(*m));
  if (!file) {
    return 0;
  }
  fseek(file, 0, SEEK_END);
  m->size = (size_t)ftell(file);
  fseek(file, 0, SEEK_SET);
  m->bytes = (unsigned char *)malloc(m->size + 1);
  if (fread(m->bytes, 1, m->size, file) != m->size) {
    fclose(file);
    return 0;
  }
  fclose(file);
  if (m->size < 32 || rd32(m->bytes) != 0xfeedfacfu ||
      rd32(m->bytes + 12) != 1) {
    fail("bad Mach-O header");
    return 0;
  }
  m->cputype = rd32(m->bytes + 4);
  ncmds = rd32(m->bytes + 16);
  if (32 + rd32(m->bytes + 20) > m->size) {
    fail("load commands run past the file");
    return 0;
  }
  cursor = 32;
  for (uint32_t c = 0; c < ncmds; c++) {
    const unsigned char *p = m->bytes + cursor;
    uint32_t cmd = rd32(p);
    uint32_t cmdsize = rd32(p + 4);
    if (cmdsize < 8 || cmdsize % 8 || cursor + cmdsize > 32 + rd32(m->bytes + 20)) {
      fail("bad load command size");
      return 0;
    }
    if (cmd == 0x19) {
      uint64_t fileoff = rd64(p + 40);
      uint64_t filesize = rd64(p + 48);
      m->nsects = rd32(p + 64);
      if (cmdsize != 72 + 80 * m->nsects || m->nsects > MAX_MACH_SECTIONS) {
        fail("segment command size disagrees with nsects");
        return 0;
      }
      if (fileoff + filesize > m->size) {
        fail("segment file range runs past the file");
      }
      for (uint32_t s = 0; s < m->nsects; s++) {
        const unsigned char *q = p + 72 + 80 * s;
        MachSect *sect = &m->sections[s];
        memcpy(sect->sectname, q, 16);
        memcpy(sect->segname, q + 16, 16);
        sect->addr = rd64(q + 32);
        sect->size = rd64(q + 40);
        sect->offset = rd32(q + 48);
        sect->align = rd32(q + 52);
        sect->reloff = rd32(q + 56);
        sect->nreloc = rd32(q + 60);
        sect->flags = rd32(q + 64);
        if ((sect->flags & 0xff) != 1 &&
            (sect->offset < fileoff ||
             sect->offset + sect->size > fileoff + filesize)) {
          fail("section data lies outside the segment file range");
        }
        if (sect->addr % ((uint64_t)1 << sect->align)) {
          fail("section address is not aligned");
        }
        if (sect->nreloc && (uint64_t)sect->reloff + 8ull * sect->nreloc > m->size) {
          fail("relocations run past the file");
        }
      }
    } else if (cmd == 0x2) {
      uint32_t symoff = rd32(p + 8);
      uint32_t stroff = rd32(p + 16);
      uint32_t strsize = rd32(p + 20);
      m->nsyms = rd32(p + 12);
      if ((uint64_t)symoff + 16ull * m->nsyms > m->size ||
          (uint64_t)stroff + strsize > m->size || strsize < 2) {
        fail("symbol table runs past the file");
        return 0;
      }
      if (m->bytes[stroff] != ' ' || m->bytes[stroff + 1] != 0) {
        fail("string table does not start with a space and a NUL");
      }
      m->symbols = (MachSym *)calloc(m->nsyms + 1, sizeof(MachSym));
      for (uint32_t s = 0; s < m->nsyms; s++) {
        const unsigned char *q = m->bytes + symoff + 16 * s;
        uint32_t strx = rd32(q);
        if (strx >= strsize) {
          fail("symbol name offset is outside the string table");
          continue;
        }
        snprintf(m->symbols[s].name, sizeof(m->symbols[s].name), "%s",
                 (const char *)m->bytes + stroff + strx);
        m->symbols[s].type = q[4];
        m->symbols[s].sect = q[5];
        m->symbols[s].value = rd64(q + 8);
      }
    } else if (cmd == 0xb) {
      if (cmdsize != 80) {
        fail("dysymtab size");
      }
      m->ilocal = rd32(p + 8);
      m->nlocal = rd32(p + 12);
      m->iextdef = rd32(p + 16);
      m->nextdef = rd32(p + 20);
      m->iundef = rd32(p + 24);
      m->nundef = rd32(p + 28);
    } else if (cmd == 0x32) {
      if (rd32(p + 8) != 1) {
        fail("build version platform is not macOS");
      }
      m->saw_build_version = 1;
    }
    cursor += cmdsize;
  }
  return 1;
}

static int mach_find_symbol(const MachFile *m, const char *name) {
  char prefixed[64];
  snprintf(prefixed, sizeof(prefixed), "_%s", name);
  for (uint32_t i = 0; i < m->nsyms; i++) {
    if (strcmp(m->symbols[i].name, prefixed) == 0) {
      return (int)i;
    }
  }
  return -1;
}

static uint64_t undefined_address(const char *name) {
  uint64_t h = 1469598103934665603ull;
  for (const char *p = name; *p; p++) {
    h = (h ^ (unsigned char)*p) * 1099511628211ull;
  }
  return 0x100000000ull + (h & 0xFFFFFF0ull);
}

static uint64_t got_address(const char *name) {
  return 0x200000000ull + (undefined_address(name) & 0xFFFF8ull);
}

static int x64_is_lea(const unsigned char *field, size_t offset) {
  return offset >= 3 && (field[-3] & 0xFB) == 0x48 && field[-2] == 0x8D &&
         (field[-1] & 0xC7) == 0x05;
}

static uint64_t mach_symbol_address(const MachFile *m, uint32_t index) {
  const MachSym *symbol = &m->symbols[index];
  if ((symbol->type & 0x0e) == 0) {
    return undefined_address(symbol->name + 1);
  }
  return symbol->value;
}

static void put_branch26(unsigned char *p, int64_t delta) {
  uint32_t insn = rd32(p) & 0xFC000000u;
  wr32(p, insn | ((uint32_t)(delta >> 2) & 0x03FFFFFFu));
}

static void put_page21(unsigned char *p, uint64_t target, uint64_t place) {
  int64_t pages = (int64_t)((target & ~0xFFFull) - (place & ~0xFFFull)) >> 12;
  uint32_t imm = (uint32_t)pages & 0x1FFFFFu;
  uint32_t insn = rd32(p) & 0x9F00001Fu;
  wr32(p, insn | ((imm & 3u) << 29) | ((imm >> 2) << 5));
}

static void put_lo12(unsigned char *p, uint64_t target) {
  uint32_t insn = rd32(p) & 0xFFC003FFu;
  wr32(p, insn | ((uint32_t)(target & 0xFFFu) << 10));
}

static void apply_mach_relocations(const MachFile *m, unsigned char *image,
                                   int arm64) {
  for (uint32_t s = 0; s < m->nsects; s++) {
    const MachSect *sect = &m->sections[s];
    int64_t pending = 0;
    int has_pending = 0;
    for (uint32_t r = 0; r < sect->nreloc; r++) {
      const unsigned char *q = m->bytes + sect->reloff + 8 * r;
      uint32_t address = rd32(q);
      uint32_t info = rd32(q + 4);
      uint32_t symbolnum = info & 0xFFFFFFu;
      int pcrel = (info >> 24) & 1;
      uint32_t length = (info >> 25) & 3;
      int external = (info >> 27) & 1;
      uint32_t type = info >> 28;
      unsigned char *p = image + sect->addr + address;
      uint64_t place = sect->addr + address;
      uint64_t target;
      if (address + (length == 3 ? 8u : 4u) > sect->size) {
        fail("relocation address outside its section");
        continue;
      }
      if (arm64 && type == 10) {
        if (external || pcrel || length != 2 || has_pending) {
          fail("malformed ARM64_RELOC_ADDEND");
        }
        pending = (int32_t)(symbolnum << 8) >> 8;
        has_pending = 1;
        continue;
      }
      if (!external || symbolnum >= m->nsyms) {
        fail("relocation is not extern or names a bad symbol");
        continue;
      }
      target = mach_symbol_address(m, symbolnum);
      if (arm64) {
        int64_t addend = has_pending ? pending : 0;
        has_pending = 0;
        switch (type) {
        case 5:
          if (!pcrel || length != 2 || (rd32(p) & 0x60FFFFE0u) || addend ||
              (m->symbols[symbolnum].type & 0x0e) != 0) {
            fail("GOT_LOAD_PAGE21 form");
          }
          put_page21(p, got_address(m->symbols[symbolnum].name + 1), place);
          break;
        case 6:
          if (pcrel || length != 2 || (rd32(p) & 0xFFFFFC00u) != 0xF9400000u ||
              addend || (m->symbols[symbolnum].type & 0x0e) != 0) {
            fail("GOT_LOAD_PAGEOFF12 form");
          }
          wr32(p, rd32(p) |
                      ((uint32_t)((got_address(m->symbols[symbolnum].name + 1) &
                                   0xFFFu) >> 3) << 10));
          break;
        case 0:
          if (pcrel || length != 3) {
            fail("ARM64_RELOC_UNSIGNED form");
          }
          if (addend) {
            fail("ADDEND before UNSIGNED");
          }
          wr64(p, target + rd64(p));
          break;
        case 2:
          if (!pcrel || length != 2 || (rd32(p) & 0x03FFFFFFu)) {
            fail("BRANCH26 form or nonzero in-place immediate");
          }
          put_branch26(p, (int64_t)(target + addend - place));
          break;
        case 3:
          if (!pcrel || length != 2 || (rd32(p) & 0x60FFFFE0u)) {
            fail("PAGE21 form or nonzero in-place immediate");
          }
          put_page21(p, target + addend, place);
          break;
        case 4:
          if (pcrel || length != 2 || (rd32(p) & 0x003FFC00u) ||
              (rd32(p) & 0x7F800000u) != 0x11000000u) {
            fail("PAGEOFF12 form, instruction or nonzero in-place immediate");
          }
          put_lo12(p, target + addend);
          break;
        default:
          fail("unexpected arm64 relocation type");
          break;
        }
      } else {
        int32_t inplace = (int32_t)rd32(p);
        int64_t value;
        switch (type) {
        case 3:
          if (!pcrel || length != 2 || inplace != 0 || p[-2] != 0x8B ||
              (m->symbols[symbolnum].type & 0x0e) != 0) {
            fail("GOT_LOAD form");
          }
          value = (int64_t)got_address(m->symbols[symbolnum].name + 1) -
                  (int64_t)(place + 4);
          wr32(p, (uint32_t)value);
          break;
        case 0:
          if (pcrel || length != 3) {
            fail("X86_64_RELOC_UNSIGNED form");
          }
          wr64(p, target + rd64(p));
          break;
        case 1:
        case 2:
          if (!pcrel || length != 2) {
            fail("SIGNED/BRANCH form");
          }
          value = (int64_t)target + inplace - (int64_t)(place + 4);
          wr32(p, (uint32_t)value);
          break;
        case 6:
        case 7:
        case 8: {
          int64_t n = type == 6 ? 1 : (type == 7 ? 2 : 4);
          if (!pcrel || length != 2) {
            fail("SIGNED_n form");
          }
          value = (int64_t)target + (inplace + n) - (int64_t)(place + 4 + n);
          wr32(p, (uint32_t)value);
          break;
        }
        default:
          fail("unexpected x86-64 relocation type");
          break;
        }
      }
    }
    if (has_pending) {
      fail("dangling ARM64_RELOC_ADDEND");
    }
  }
}

static int section_slot_name(const Gen *g, size_t section, int has_relocs,
                             const char **segname, const char **sectname) {
  switch (g->sections[section].kind) {
  case BINARY_SECTION_TEXT:
    *segname = "__TEXT";
    *sectname = "__text";
    return 1;
  case BINARY_SECTION_RDATA:
    *segname = has_relocs ? "__DATA" : "__TEXT";
    *sectname = "__const";
    return 1;
  case BINARY_SECTION_DATA:
    *segname = "__DATA";
    *sectname = "__data";
    return 1;
  case BINARY_SECTION_BSS:
    *segname = "__DATA";
    *sectname = "__bss";
    return 1;
  case BINARY_SECTION_INIT_ARRAY:
    *segname = "__DATA";
    *sectname = "__mod_init_func";
    return 1;
  case BINARY_SECTION_FINI_ARRAY:
    *segname = "__DATA";
    *sectname = "__mod_term_func";
    return 1;
  default:
    return 0;
  }
}

static int gen_section_has_relocs(const Gen *g, size_t section) {
  for (size_t r = 0; r < g->reloc_count; r++) {
    if (g->relocs[r].section == section) {
      return 1;
    }
  }
  return 0;
}

static uint64_t gen_symbol_address(const Gen *g, const uint64_t *anchors,
                                   size_t symbol) {
  const GenSymbol *s = &g->symbols[symbol];
  if (!s->defined) {
    return undefined_address(s->name);
  }
  return anchors[s->section] + s->value;
}

static void expected_relocations(const Gen *g, const uint64_t *anchors,
                                 unsigned char *image) {
  for (size_t r = 0; r < g->reloc_count; r++) {
    const GenReloc *reloc = &g->relocs[r];
    const GenSymbol *symbol = &g->symbols[reloc->symbol];
    const GenSection *section = &g->sections[reloc->section];
    uint64_t place;
    uint64_t target;
    unsigned char *p;
    if (section->kind == BINARY_SECTION_DEBUG) {
      continue;
    }
    place = anchors[reloc->section] + reloc->offset;
    target = gen_symbol_address(g, anchors, reloc->symbol);
    p = image + place;
    if (!symbol->defined && section->kind == BINARY_SECTION_TEXT) {
      uint64_t got = got_address(symbol->name);
      if (reloc->kind == BINARY_RELOCATION_ARM64_ADR_PREL_PG_HI21) {
        put_page21(p, got, place);
        continue;
      }
      if (reloc->kind == BINARY_RELOCATION_ARM64_ADD_ABS_LO12_NC) {
        wr32(p, 0xF9400000u | (rd32(p) & 0x3FFu) |
                    ((uint32_t)((got & 0xFFFu) >> 3) << 10));
        continue;
      }
      if (reloc->kind == BINARY_RELOCATION_REL32 && reloc->addend == 0 &&
          x64_is_lea(p, reloc->offset) && !(p[-1] == 0xE8 || p[-1] == 0xE9)) {
        p[-2] = 0x8B;
        wr32(p, (uint32_t)((int64_t)got - (int64_t)(place + 4)));
        continue;
      }
    }
    switch (reloc->kind) {
    case BINARY_RELOCATION_REL32:
      wr32(p, (uint32_t)((int64_t)target + reloc->addend - 4 - (int64_t)place));
      break;
    case BINARY_RELOCATION_ADDR64:
      wr64(p, target + (uint64_t)(int64_t)reloc->addend);
      break;
    case BINARY_RELOCATION_ARM64_CALL26:
      put_branch26(p, (int64_t)(target + reloc->addend - place));
      break;
    case BINARY_RELOCATION_ARM64_ADR_PREL_PG_HI21:
      put_page21(p, target + reloc->addend, place);
      break;
    case BINARY_RELOCATION_ARM64_ADD_ABS_LO12_NC:
      put_lo12(p, target + reloc->addend);
      break;
    default:
      break;
    }
  }
}

static void check_symbols(const Gen *g, const MachFile *m) {
  uint32_t locals = 0, extdefs = 0, undefs = 0;
  if (m->ilocal != 0 || m->iextdef != m->nlocal ||
      m->iundef != m->nlocal + m->nextdef ||
      m->nlocal + m->nextdef + m->nundef != m->nsyms) {
    fail("dysymtab ranges do not tile the symbol table");
  }
  for (uint32_t i = 0; i < m->nsyms; i++) {
    const MachSym *s = &m->symbols[i];
    if (s->name[0] != '_') {
      fail("symbol without the underscore prefix");
    }
    if (i < m->nlocal) {
      if (s->type != 0x0e || s->sect == 0) {
        fail("local symbol type");
      }
      locals++;
    } else if (i < m->nlocal + m->nextdef) {
      if (s->type != 0x0f || s->sect == 0) {
        fail("external definition type");
      }
      if (i > m->iextdef && strcmp(m->symbols[i - 1].name, s->name) > 0) {
        fail("external definitions are not sorted");
      }
      extdefs++;
    } else {
      if (s->type != 0x01 || s->sect != 0 || s->value != 0) {
        fail("undefined symbol type");
      }
      if (i > m->iundef && strcmp(m->symbols[i - 1].name, s->name) > 0) {
        fail("undefined symbols are not sorted");
      }
      undefs++;
    }
  }
  for (size_t i = 0; i < g->symbol_count; i++) {
    const GenSymbol *gs = &g->symbols[i];
    int found = mach_find_symbol(m, gs->name);
    int debug = gs->defined && g->sections[gs->section].kind == BINARY_SECTION_DEBUG;
    if (debug) {
      if (found >= 0) {
        fail("symbol in a debug section survived");
      }
      continue;
    }
    if (found < 0) {
      fail("symbol missing");
      continue;
    }
    if (!gs->defined && (uint32_t)found < m->iundef) {
      fail("undefined symbol outside the undefined range");
    }
    if (gs->defined && gs->global &&
        ((uint32_t)found < m->iextdef || (uint32_t)found >= m->iundef)) {
      fail("global symbol outside the external range");
    }
    if (gs->defined && !gs->global && (uint32_t)found >= m->nlocal) {
      fail("local symbol outside the local range");
    }
  }
  (void)locals;
  (void)extdefs;
  (void)undefs;
}

static void check_relocation_types(const Gen *g, const MachFile *m,
                                   const uint64_t *anchors,
                                   const unsigned char *original_text_prefix) {
  (void)original_text_prefix;
  for (size_t r = 0; r < g->reloc_count; r++) {
    const GenReloc *reloc = &g->relocs[r];
    const GenSection *section = &g->sections[reloc->section];
    BinarySection *raw;
    uint64_t place;
    int matched = 0;
    if (g->arm64 || reloc->kind != BINARY_RELOCATION_REL32 ||
        section->kind == BINARY_SECTION_DEBUG) {
      continue;
    }
    raw = binary_emitter_get_section(g->emitter, section->index);
    place = anchors[reloc->section] + reloc->offset;
    for (uint32_t s = 0; s < m->nsects && !matched; s++) {
      const MachSect *sect = &m->sections[s];
      for (uint32_t k = 0; k < sect->nreloc; k++) {
        const unsigned char *q = m->bytes + sect->reloff + 8 * k;
        uint32_t type = rd32(q + 4) >> 28;
        uint32_t expected;
        const unsigned char *code = raw->data + reloc->offset;
        if (sect->addr + rd32(q) != place) {
          continue;
        }
        matched = 1;
        if (section->kind == BINARY_SECTION_TEXT && reloc->offset >= 1 &&
            (code[-1] == 0xE8 || code[-1] == 0xE9 ||
             (reloc->offset >= 2 && code[-2] == 0x0F &&
              (code[-1] & 0xF0) == 0x80))) {
          expected = 2;
        } else if (section->kind == BINARY_SECTION_TEXT &&
                   !g->symbols[reloc->symbol].defined && reloc->addend == 0 &&
                   x64_is_lea(code, reloc->offset)) {
          expected = 3;
        } else if (reloc->addend == -1) {
          expected = 6;
        } else if (reloc->addend == -2) {
          expected = 7;
        } else if (reloc->addend == -4) {
          expected = 8;
        } else {
          expected = 1;
        }
        if (type != expected) {
          fail("x86-64 relocation type choice");
        }
        break;
      }
    }
    if (!matched) {
      fail("relocation missing from the Mach-O file");
    }
  }
}

static void run_case(int arm64, int number) {
  Gen g;
  MachFile m;
  char path[256];
  uint64_t anchors[MAX_SECTIONS];
  uint64_t image_size = 0;
  unsigned char *got;
  unsigned char *want;
  memset(&g, 0, sizeof(g));
  snprintf(failure_context, sizeof(failure_context), "%s case %d",
           arm64 ? "arm64" : "x86-64", number);
  g.arm64 = arm64;
  g.emitter = binary_emitter_create(arm64 ? BINARY_TARGET_FORMAT_MACHO_ARM64
                                          : BINARY_TARGET_FORMAT_MACHO_X64);
  gen_sections(&g);
  gen_symbols(&g);
  gen_relocations(&g);
  snprintf(path, sizeof(path), "%s/macho_case_%s_%d.o", scratch_dir,
           arm64 ? "a" : "x", number);
  if (!binary_emitter_write_object_file(g.emitter, path)) {
    fail(binary_emitter_get_error(g.emitter) ? binary_emitter_get_error(g.emitter)
                                             : "write failed");
    goto done;
  }
  if (!parse_mach(&m, path)) {
    fail("could not parse the written file");
    goto done;
  }
  if (m.cputype != (arm64 ? 0x0100000cu : 0x01000007u)) {
    fail("cputype");
  }
  if (!m.saw_build_version) {
    fail("no LC_BUILD_VERSION");
  }
  for (uint32_t s = 0; s < m.nsects; s++) {
    uint64_t end = m.sections[s].addr + m.sections[s].size;
    if (end > image_size) {
      image_size = end;
    }
    if (s > 0 && m.sections[s].addr < m.sections[s - 1].addr + m.sections[s - 1].size) {
      fail("sections overlap");
    }
    if ((m.sections[s].flags & 0xff) == 1 && s + 1 != m.nsects) {
      fail("zero-fill section is not last");
    }
  }
  for (size_t i = 0; i < g.section_count; i++) {
    const char *segname;
    const char *sectname;
    int has_relocs = gen_section_has_relocs(&g, i);
    int found = mach_find_symbol(&m, g.symbols[i].name);
    int named = section_slot_name(&g, i, has_relocs, &segname, &sectname);
    anchors[i] = 0;
    if (!named) {
      if (found >= 0) {
        fail("debug section anchor survived");
      }
      continue;
    }
    if (found < 0) {
      fail("anchor missing");
      continue;
    }
    anchors[i] = m.symbols[found].value;
    {
      const MachSect *sect = &m.sections[m.symbols[found].sect - 1];
      uint64_t alignment = binary_emitter_get_section(&*g.emitter,
                                                      g.sections[i].index)
                               ->alignment;
      if (strcmp(sect->segname, segname) || strcmp(sect->sectname, sectname)) {
        fail("section landed in the wrong Mach-O section");
      }
      if (anchors[i] % alignment) {
        fail("section start lost its alignment");
      }
      if (anchors[i] < sect->addr ||
          anchors[i] + g.sections[i].memory_size > sect->addr + sect->size) {
        fail("section lies outside its Mach-O section");
      }
    }
  }
  check_symbols(&g, &m);
  for (size_t i = 0; i < g.symbol_count; i++) {
    const GenSymbol *gs = &g.symbols[i];
    int found = mach_find_symbol(&m, gs->name);
    if (gs->defined && found >= 0 && anchors[gs->section] + gs->value != m.symbols[found].value) {
      fail("symbol value");
    }
  }
  check_relocation_types(&g, &m, anchors, NULL);
  got = (unsigned char *)calloc((size_t)image_size + 16, 1);
  want = (unsigned char *)calloc((size_t)image_size + 16, 1);
  for (uint32_t s = 0; s < m.nsects; s++) {
    if ((m.sections[s].flags & 0xff) != 1) {
      memcpy(got + m.sections[s].addr, m.bytes + m.sections[s].offset,
             (size_t)m.sections[s].size);
    }
  }
  memcpy(want, got, (size_t)image_size);
  for (size_t i = 0; i < g.section_count; i++) {
    BinarySection *raw = binary_emitter_get_section(g.emitter, g.sections[i].index);
    if (g.sections[i].kind == BINARY_SECTION_DEBUG ||
        g.sections[i].kind == BINARY_SECTION_BSS) {
      continue;
    }
    memcpy(want + anchors[i], raw->data, raw->size);
  }
  apply_mach_relocations(&m, got, arm64);
  expected_relocations(&g, anchors, want);
  for (size_t i = 0; i < g.section_count; i++) {
    if (g.sections[i].kind == BINARY_SECTION_DEBUG ||
        g.sections[i].kind == BINARY_SECTION_BSS) {
      continue;
    }
    if (memcmp(got + anchors[i], want + anchors[i], g.sections[i].size) != 0) {
      fail("resolved Mach-O bytes differ from ELF-rule resolution");
    }
  }
  free(got);
  free(want);
  free(m.bytes);
  free(m.symbols);
  remove(path);
done:
  for (size_t i = 0; i < g.section_count; i++) {
    free(g.sections[i].occupied);
  }
  binary_emitter_destroy(g.emitter);
}

static void expect_refusal(int arm64, BinarySectionKind kind,
                           BinaryRelocationKind reloc_kind, int32_t addend,
                           uint32_t word, const char *label) {
  BinaryEmitter *emitter = binary_emitter_create(
      arm64 ? BINARY_TARGET_FORMAT_MACHO_ARM64 : BINARY_TARGET_FORMAT_MACHO_X64);
  unsigned char bytes[16] = {0};
  char path[256];
  size_t section = binary_emitter_get_or_create_section(emitter, ".s", kind, 0, 8);
  snprintf(failure_context, sizeof(failure_context), "refusal %s", label);
  wr32(bytes, word);
  binary_emitter_append_bytes(emitter, section, bytes, sizeof(bytes), NULL);
  binary_emitter_declare_external(emitter, "target");
  binary_emitter_add_relocation(emitter, section, 0, reloc_kind, "target", addend);
  snprintf(path, sizeof(path), "%s/macho_refusal.o", scratch_dir);
  if (binary_emitter_write_object_file(emitter, path)) {
    fail("writer accepted what Mach-O cannot express");
    remove(path);
  } else if (!binary_emitter_get_error(emitter)) {
    fail("refusal without a message");
  }
  binary_emitter_destroy(emitter);
}

static void check_real_relocations(const MachFile *m) {
  int arm64 = m->cputype == 0x0100000cu;
  for (uint32_t s = 0; s < m->nsects; s++) {
    const MachSect *sect = &m->sections[s];
    int text = strcmp(sect->sectname, "__text") == 0;
    int expect_addend_target = 0;
    for (uint32_t r = 0; r < sect->nreloc; r++) {
      const unsigned char *q = m->bytes + sect->reloff + 8 * r;
      uint32_t address = rd32(q);
      uint32_t info = rd32(q + 4);
      uint32_t symbolnum = info & 0xFFFFFFu;
      uint32_t type = info >> 28;
      int external = (info >> 27) & 1;
      int undefined;
      if (address >= sect->size) {
        fail("relocation address outside its section");
        continue;
      }
      if (arm64 && type == 10) {
        expect_addend_target = 1;
        continue;
      }
      if (!external || symbolnum >= m->nsyms) {
        fail("relocation is not extern or names a bad symbol");
        continue;
      }
      undefined = (m->symbols[symbolnum].type & 0x0e) == 0;
      if (text && type == 0) {
        fail("absolute relocation inside __text");
      }
      if (arm64) {
        if ((type == 3 || type == 4) && undefined) {
          fail("ADRP/ADD reach an undefined symbol without the GOT");
        }
        if ((type == 5 || type == 6) && !undefined) {
          fail("GOT relocation against a defined symbol");
        }
        if (expect_addend_target && type != 2 && type != 3 && type != 4) {
          fail("ARM64_RELOC_ADDEND before a relocation that takes none");
        }
        if (type != 0 && type != 2 && type != 3 && type != 4 && type != 5 &&
            type != 6) {
          fail("unexpected arm64 relocation type");
        }
      } else {
        if (type == 3 && !undefined) {
          fail("GOT_LOAD against a defined symbol");
        }
        if (type != 0 && type != 1 && type != 2 && type != 3 && type != 6 &&
            type != 7 && type != 8) {
          fail("unexpected x86-64 relocation type");
        }
      }
      expect_addend_target = 0;
    }
    if (expect_addend_target) {
      fail("dangling ARM64_RELOC_ADDEND");
    }
  }
}

static int check_real_object(const char *path) {
  MachFile m;
  snprintf(failure_context, sizeof(failure_context), "%s", path);
  if (!parse_mach(&m, path)) {
    fail("could not parse the object");
    return 1;
  }
  if (!m.saw_build_version) {
    fail("no LC_BUILD_VERSION");
  }
  if (m.ilocal != 0 || m.iextdef != m.nlocal ||
      m.iundef != m.nlocal + m.nextdef ||
      m.nlocal + m.nextdef + m.nundef != m.nsyms) {
    fail("dysymtab ranges do not tile the symbol table");
  }
  for (uint32_t i = 0; i < m.nsyms; i++) {
    if (m.symbols[i].name[0] != '_') {
      fail("symbol without the underscore prefix");
    }
    if ((m.symbols[i].type & 0x0e) == 0x0e &&
        (m.symbols[i].sect == 0 || m.symbols[i].sect > m.nsects)) {
      fail("defined symbol names no section");
    }
  }
  check_real_relocations(&m);
  printf("%s: %u sections, %u symbols\n", path, m.nsects, m.nsyms);
  free(m.bytes);
  free(m.symbols);
  return 0;
}

int main(int argc, char **argv) {
  int cases;
  if (argc > 2 && strcmp(argv[1], "--check") == 0) {
    for (int i = 2; i < argc; i++) {
      check_real_object(argv[i]);
    }
    printf("macho_object: %d objects checked, %d failures\n", argc - 2,
           failures);
    return failures ? 1 : 0;
  }
  cases = argc > 1 ? atoi(argv[1]) : 2000;
  if (argc > 2) {
    scratch_dir = argv[2];
  }
  for (int i = 0; i < cases; i++) {
    run_case(0, i);
    run_case(1, i);
  }
  expect_refusal(0, BINARY_SECTION_TEXT, BINARY_RELOCATION_ADDR64, 0, 0,
                 "x86-64 absolute address in text");
  expect_refusal(1, BINARY_SECTION_TEXT, BINARY_RELOCATION_ADDR64, 0, 0,
                 "arm64 absolute address in text");
  expect_refusal(1, BINARY_SECTION_TEXT, BINARY_RELOCATION_ARM64_CALL26,
                 0x1000000, 0x94000000u, "arm64 addend past 24 bits");
  expect_refusal(0, BINARY_SECTION_DATA, BINARY_RELOCATION_ADDR32NB, 0, 0,
                 "COFF-only relocation");
  expect_refusal(1, BINARY_SECTION_TEXT, BINARY_RELOCATION_ARM64_ADD_ABS_LO12_NC,
                 8, 0x91000000u, "arm64 GOT load with an offset");
  expect_refusal(1, BINARY_SECTION_TEXT,
                 BINARY_RELOCATION_ARM64_ADR_PREL_PG_HI21, 8, 0x90000000u,
                 "arm64 GOT page with an offset");
  expect_refusal(1, BINARY_SECTION_TEXT, BINARY_RELOCATION_ARM64_ADD_ABS_LO12_NC,
                 0, 0xF9400000u, "arm64 GOT load from a non-add instruction");
  printf("macho_object: %d cases per architecture, %d failures\n", cases,
         failures);
  return failures ? 1 : 0;
}
