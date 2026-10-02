typedef struct {
  unsigned long long base;
  const MtlcType *type;
  long long rows;
  long long cols;
  int rank;
  MtlcTypeKind kind;
  int esize;
} IITile;

static int ii_tile_type_is_tile(const MtlcType *type) {
  return ir_tile_operand_is_tile(type);
}

static int ii_tile_describe(const MtlcType *type, IITile *tile) {
  memset(tile, 0, sizeof(*tile));
  if (!ii_tile_type_is_tile(type) || !type->base_type) {
    return 0;
  }
  tile->type = type;
  tile->rows = (long long)type->view_extents[0];
  tile->rank = type->view_extents[1] ? 2 : 1;
  tile->cols = tile->rank == 2 ? (long long)type->view_extents[1] : 1;
  tile->kind = type->base_type->kind;
  tile->esize = (int)(type->base_type->size ? type->base_type->size : 4);
  return 1;
}

static int ii_tile_bind(IRInterpMachine *machine, IIFrame *frame,
                        const IROperand *operand, const MtlcType *type,
                        IITile *tile) {
  IRInterpValue value;
  if (!ii_tile_describe(type, tile)) {
    ii_fail(machine, IR_INTERP_UNSUPPORTED, "tile operand without tile type");
    return 0;
  }
  if (!ii_fetch(machine, frame, operand, &value)) {
    return 0;
  }
  tile->base = (unsigned long long)value.i;
  return 1;
}

static double ii_tile_round(MtlcTypeKind kind, double value) {
  float single = (float)value;
  uint32_t bits;
  switch (kind) {
  case MTLC_TYPE_FLOAT32:
    return (double)single;
  case MTLC_TYPE_FLOAT16:
    memcpy(&bits, &single, 4);
    return (double)mettle_f16bits_to_f32(mettle_f32bits_to_f16bits(bits));
  case MTLC_TYPE_BFLOAT16:
    memcpy(&bits, &single, 4);
    return (double)mettle_bf16bits_to_f32(mettle_f32bits_to_bf16bits(bits));
  case MTLC_TYPE_BOOL:
    return value != 0.0 ? 1.0 : 0.0;
  case MTLC_TYPE_INT32:
    if (value != value) return 0.0;
    return (double)(int32_t)(uint32_t)(long long)value;
  default:
    return value;
  }
}

static int ii_tile_read_bits(IRInterpMachine *machine, unsigned long long addr,
                             MtlcTypeKind kind, int esize, double *out) {
  unsigned long long raw = 0;
  if (!ii_mem_read(machine, addr, esize, &raw)) {
    return 0;
  }
  switch (kind) {
  case MTLC_TYPE_FLOAT32: {
    uint32_t bits = (uint32_t)raw;
    float f;
    memcpy(&f, &bits, 4);
    *out = (double)f;
    return 1;
  }
  case MTLC_TYPE_FLOAT64: {
    double d;
    memcpy(&d, &raw, 8);
    *out = d;
    return 1;
  }
  case MTLC_TYPE_FLOAT16:
    *out = (double)mettle_f16bits_to_f32((uint16_t)raw);
    return 1;
  case MTLC_TYPE_BFLOAT16:
    *out = (double)mettle_bf16bits_to_f32((uint16_t)raw);
    return 1;
  case MTLC_TYPE_INT8:
    *out = (double)(int8_t)raw;
    return 1;
  case MTLC_TYPE_INT16:
    *out = (double)(int16_t)raw;
    return 1;
  case MTLC_TYPE_INT32:
    *out = (double)(int32_t)raw;
    return 1;
  case MTLC_TYPE_UINT8:
  case MTLC_TYPE_UINT16:
  case MTLC_TYPE_UINT32:
  case MTLC_TYPE_BOOL:
    *out = (double)raw;
    return 1;
  default:
    *out = (double)(long long)raw;
    return 1;
  }
}

static int ii_tile_write_bits(IRInterpMachine *machine, unsigned long long addr,
                              MtlcTypeKind kind, int esize, double value) {
  unsigned long long raw = 0;
  float single = (float)value;
  uint32_t bits;
  switch (kind) {
  case MTLC_TYPE_FLOAT32:
    memcpy(&bits, &single, 4);
    raw = bits;
    break;
  case MTLC_TYPE_FLOAT64:
    memcpy(&raw, &value, 8);
    break;
  case MTLC_TYPE_FLOAT16:
    memcpy(&bits, &single, 4);
    raw = mettle_f32bits_to_f16bits(bits);
    break;
  case MTLC_TYPE_BFLOAT16:
    memcpy(&bits, &single, 4);
    raw = mettle_f32bits_to_bf16bits(bits);
    break;
  case MTLC_TYPE_BOOL:
    raw = value != 0.0;
    break;
  default:
    raw = (unsigned long long)(long long)value;
    break;
  }
  return ii_mem_write(machine, addr, esize, raw);
}

static int ii_tile_get(IRInterpMachine *machine, const IITile *tile,
                       long long row, long long col, double *out) {
  long long index = tile->rank == 2 ? row * tile->cols + col : row;
  return ii_tile_read_bits(machine,
                           tile->base + (unsigned long long)(index * tile->esize),
                           tile->kind, tile->esize, out);
}

static int ii_tile_set(IRInterpMachine *machine, const IITile *tile,
                       long long row, long long col, double value) {
  long long index = tile->rank == 2 ? row * tile->cols + col : row;
  return ii_tile_write_bits(
      machine, tile->base + (unsigned long long)(index * tile->esize),
      tile->kind, tile->esize, ii_tile_round(tile->kind, value));
}

typedef struct {
  int is_tile;
  IITile tile;
  double scalar;
} IITileOperand;

static int ii_tile_operand(IRInterpMachine *machine, IIFrame *frame,
                           const IRInstruction *insn, size_t index,
                           IITileOperand *out) {
  const MtlcType *type =
      insn->argument_types ? insn->argument_types[index] : NULL;
  memset(out, 0, sizeof(*out));
  if (ii_tile_type_is_tile(type)) {
    out->is_tile = 1;
    return ii_tile_bind(machine, frame, &insn->arguments[index], type,
                        &out->tile);
  }
  {
    IRInterpValue value;
    if (!ii_fetch(machine, frame, &insn->arguments[index], &value)) {
      return 0;
    }
    out->scalar = ii_as_float(&value);
    if (type && type->kind == MTLC_TYPE_FLOAT32) {
      out->scalar = (double)(float)out->scalar;
    }
  }
  return 1;
}

static int ii_tile_operand_at(IRInterpMachine *machine,
                              const IITileOperand *operand, long long row,
                              long long col, double *out) {
  if (!operand->is_tile) {
    *out = operand->scalar;
    return 1;
  }
  return ii_tile_get(machine, &operand->tile, row, col, out);
}

static double ii_tile_max_num(double a, double b, int want_max) {
  if (a != a) return b;
  if (b != b) return a;
  if (a == 0.0 && b == 0.0) {
    unsigned long long a_bits, b_bits;
    memcpy(&a_bits, &a, 8);
    memcpy(&b_bits, &b, 8);
    int a_negative = (a_bits >> 63) != 0;
    int b_negative = (b_bits >> 63) != 0;
    if (want_max) return (a_negative && b_negative) ? -0.0 : 0.0;
    return (a_negative || b_negative) ? -0.0 : 0.0;
  }
  if (want_max) return a > b ? a : b;
  return a < b ? a : b;
}

static int ii_tile_integer_kind(MtlcTypeKind kind) {
  return kind == MTLC_TYPE_INT32 || kind == MTLC_TYPE_BOOL;
}

static int ii_tile_binary(const char *op, MtlcTypeKind kind, double a,
                          double b, double *out) {
  int integer = ii_tile_integer_kind(kind);
  if (!strcmp(op, "add")) {
    *out = integer ? (double)(int32_t)(uint32_t)((long long)a + (long long)b)
                   : a + b;
  } else if (!strcmp(op, "sub")) {
    *out = integer ? (double)(int32_t)(uint32_t)((long long)a - (long long)b)
                   : a - b;
  } else if (!strcmp(op, "mul")) {
    *out = integer ? (double)(int32_t)(uint32_t)((long long)a * (long long)b)
                   : a * b;
  } else if (!strcmp(op, "div")) {
    if (integer) {
      if ((long long)b == 0) return 0;
      *out = (double)((long long)a / (long long)b);
    } else {
      *out = a / b;
    }
  } else if (!strcmp(op, "lt")) {
    *out = a < b;
  } else if (!strcmp(op, "le")) {
    *out = a <= b;
  } else if (!strcmp(op, "gt")) {
    *out = a > b;
  } else if (!strcmp(op, "ge")) {
    *out = a >= b;
  } else if (!strcmp(op, "eq")) {
    *out = a == b;
  } else if (!strcmp(op, "ne")) {
    *out = a != b;
  } else if (!strcmp(op, "and")) {
    *out = (a != 0.0) && (b != 0.0);
  } else if (!strcmp(op, "or")) {
    *out = (a != 0.0) || (b != 0.0);
  } else if (!strcmp(op, "max")) {
    *out = ii_tile_max_num(a, b, 1);
  } else if (!strcmp(op, "min")) {
    *out = ii_tile_max_num(a, b, 0);
  } else {
    return 0;
  }
  return 1;
}

static int ii_tile_unary(const char *op, double a, double *out) {
  float f = (float)a;
  if (!strcmp(op, "neg")) {
    *out = -a;
  } else if (!strcmp(op, "not")) {
    *out = a == 0.0;
  } else if (!strcmp(op, "exp")) {
    *out = (double)(float)mt_exp((double)f);
  } else if (!strcmp(op, "sqrt")) {
    *out = (double)(float)ii_sqrt((double)f);
  } else if (!strcmp(op, "rsqrt")) {
    *out = (double)(float)(1.0 / ii_sqrt((double)f));
  } else if (!strcmp(op, "abs")) {
    *out = (double)(f < 0.0f ? -f : f);
  } else if (!strcmp(op, "log")) {
    *out = (double)(float)mt_log((double)f);
  } else if (!strcmp(op, "sin")) {
    *out = (double)(float)mt_sin((double)f);
  } else if (!strcmp(op, "cos")) {
    *out = (double)(float)mt_cos((double)f);
  } else {
    return 0;
  }
  return 1;
}

static long long ii_tile_view_offset(const MtlcType *view, long long row,
                                     long long col) {
  long long rows = (long long)view->view_extents[0];
  long long cols = (long long)view->view_extents[1];
  long long esize = view->base_type && view->base_type->size
                        ? (long long)view->base_type->size
                        : 4;
  switch (view->view_layout) {
  case MTLC_VIEW_LAYOUT_COL:
    return col * rows + row;
  case MTLC_VIEW_LAYOUT_INTERLEAVE: {
    long long group = (long long)view->view_layout_param;
    if (group <= 0) return row * cols + col;
    return (col / group) * (rows * group) + row * group + (col % group);
  }
  case MTLC_VIEW_LAYOUT_SWIZZLE32:
  case MTLC_VIEW_LAYOUT_SWIZZLE64:
  case MTLC_VIEW_LAYOUT_SWIZZLE128: {
    long long chunk_bytes = view->view_layout == MTLC_VIEW_LAYOUT_SWIZZLE32   ? 4
                            : view->view_layout == MTLC_VIEW_LAYOUT_SWIZZLE64 ? 8
                                                                           : 16;
    long long chunk = chunk_bytes / esize;
    long long per_row = chunk > 0 ? cols / chunk : 0;
    if (chunk <= 0 || per_row <= 0) return row * cols + col;
    return row * cols + ((col / chunk) ^ (row % per_row)) * chunk + col % chunk;
  }
  default:
    return row * cols + col;
  }
}

static int ii_tile_is_view(const MtlcType *type) {
  return type && type->kind == MTLC_TYPE_POINTER && type->view_extents[0] &&
         type->view_extents[1] && !ii_tile_type_is_tile(type);
}

static int ii_tile_memory_element(IRInterpMachine *machine,
                                  unsigned long long base,
                                  const MtlcType *type, long long ld,
                                  long long row, long long col,
                                  unsigned long long *address,
                                  MtlcTypeKind *kind, int *esize) {
  const MtlcType *element = type ? type->base_type : NULL;
  long long offset;
  if (!element) {
    ii_fail(machine, IR_INTERP_UNSUPPORTED, "tile memory without element type");
    return 0;
  }
  *kind = element->kind;
  *esize = (int)(element->size ? element->size : 4);
  offset = ii_tile_is_view(type) ? ii_tile_view_offset(type, row, col)
                                 : row * ld + col;
  *address = base + (unsigned long long)(offset * *esize);
  return 1;
}

static int ii_tile_load_store(IRInterpMachine *machine, IIFrame *frame,
                              const IRInstruction *insn, int store) {
  IITile tile;
  IRInterpValue memory_value;
  IRInterpValue ld_value = ii_int_value(0);
  IRInterpValue rows_value = ii_int_value(0);
  const MtlcType *memory_type;
  const IROperand *memory_operand;
  long long ld;
  long long rows;
  if (insn->argument_count < 3) {
    ii_fail(machine, IR_INTERP_UNSUPPORTED, "malformed tile load or store");
    return 0;
  }
  if (store) {
    memory_operand = &insn->dest;
    memory_type = insn->value_type;
    if (!ii_tile_bind(machine, frame, &insn->arguments[0],
                      insn->argument_types[0], &tile)) {
      return 0;
    }
  } else {
    memory_operand = &insn->arguments[0];
    memory_type = insn->argument_types ? insn->argument_types[0] : NULL;
    if (!ii_tile_bind(machine, frame, &insn->dest, insn->value_type, &tile)) {
      return 0;
    }
  }
  if (!ii_fetch(machine, frame, memory_operand, &memory_value) ||
      (insn->arguments[1].kind != IR_OPERAND_NONE &&
       !ii_fetch(machine, frame, &insn->arguments[1], &ld_value)) ||
      (insn->arguments[2].kind != IR_OPERAND_NONE &&
       !ii_fetch(machine, frame, &insn->arguments[2], &rows_value))) {
    return 0;
  }
  ld = insn->arguments[1].kind != IR_OPERAND_NONE
           ? ld_value.i
           : (memory_type ? (long long)memory_type->view_extents[1] : 0);
  rows = insn->arguments[2].kind != IR_OPERAND_NONE ? rows_value.i
                                                   : tile.rows;
  for (long long r = 0; r < tile.rows; r++) {
    for (long long c = 0; c < tile.cols; c++) {
      unsigned long long address = 0;
      MtlcTypeKind kind;
      int esize;
      double value = 0.0;
      if (r >= rows) {
        if (!store && !ii_tile_set(machine, &tile, r, c, 0.0)) return 0;
        continue;
      }
      if (tile.rank == 2) {
        if (!ii_tile_memory_element(machine,
                                    (unsigned long long)memory_value.i,
                                    memory_type, ld, r, c, &address, &kind,
                                    &esize)) {
          return 0;
        }
      } else {
        const MtlcType *element = memory_type ? memory_type->base_type : NULL;
        if (!element) {
          ii_fail(machine, IR_INTERP_UNSUPPORTED,
                  "row vector memory without element type");
          return 0;
        }
        kind = element->kind;
        esize = (int)(element->size ? element->size : 4);
        address = (unsigned long long)memory_value.i +
                  (unsigned long long)(r * ld * esize);
      }
      if (store) {
        if (!ii_tile_get(machine, &tile, r, c, &value) ||
            !ii_tile_write_bits(machine, address, kind, esize,
                                ii_tile_round(kind, value))) {
          return 0;
        }
      } else {
        if (!ii_tile_read_bits(machine, address, kind, esize, &value) ||
            !ii_tile_set(machine, &tile, r, c, value)) {
          return 0;
        }
      }
    }
  }
  return 1;
}

static int ii_tile_row_reduce(IRInterpMachine *machine, const IITile *dest,
                              const IITile *source, int sum) {
  for (long long r = 0; r < source->rows; r++) {
    double result = 0.0;
    if (sum) {
      double classes[4] = {0.0, 0.0, 0.0, 0.0};
      int started[4] = {0, 0, 0, 0};
      for (long long c = 0; c < source->cols; c++) {
        long long cls = (c / 2) % 4;
        double value;
        if (!ii_tile_get(machine, source, r, c, &value)) return 0;
        if (!started[cls]) {
          classes[cls] = value;
          started[cls] = 1;
        } else {
          classes[cls] = ii_tile_round(source->kind, classes[cls] + value);
        }
      }
      result = ii_tile_round(
          source->kind,
          ii_tile_round(source->kind, classes[0] + classes[1]) +
              ii_tile_round(source->kind, classes[2] + classes[3]));
    } else {
      for (long long c = 0; c < source->cols; c++) {
        double value;
        if (!ii_tile_get(machine, source, r, c, &value)) return 0;
        result = c == 0 ? value : ii_tile_max_num(result, value, 1);
      }
    }
    if (!ii_tile_set(machine, dest, r, 0, result)) return 0;
  }
  return 1;
}

static int ii_tile_mma_operand_value(IRInterpMachine *machine,
                                     const IITileOperand *a_tile,
                                     unsigned long long a_base,
                                     const MtlcType *a_type, long long lda,
                                     long long row, long long k,
                                     double *out) {
  unsigned long long address;
  MtlcTypeKind kind;
  int esize;
  if (a_tile->is_tile) {
    return ii_tile_get(machine, &a_tile->tile, row, k, out);
  }
  if (!ii_tile_memory_element(machine, a_base, a_type, lda, row, k, &address,
                              &kind, &esize)) {
    return 0;
  }
  if (kind == MTLC_TYPE_UINT16) kind = MTLC_TYPE_FLOAT16;
  return ii_tile_read_bits(machine, address, kind, esize, out);
}

static int ii_tile_mma(IRInterpMachine *machine, IIFrame *frame,
                       const IRInstruction *insn) {
  const MtlcTensorMmaDesc *desc = &IR_TENSOR_MMA(insn);
  IITile d;
  IITileOperand a, c, scale;
  IRInterpValue b_value, a_value = ii_int_value(0), ldb_value = ii_int_value(0);
  const MtlcType *b_type = insn->argument_types[1];
  const MtlcType *a_type = insn->argument_types[0];
  long long ldb;
  int has_scale = insn->argument_count > 4 &&
                  insn->arguments[4].kind != IR_OPERAND_NONE;
  MtlcTypeKind b_kind_override =
      desc->b_element == MTLC_TENSOR_ELEMENT_BFLOAT16 ? MTLC_TYPE_BFLOAT16
                                                      : MTLC_TYPE_FLOAT16;
  if (!ii_tile_bind(machine, frame, &insn->dest, insn->value_type, &d) ||
      !ii_tile_operand(machine, frame, insn, 0, &a) ||
      !ii_tile_operand(machine, frame, insn, 2, &c) ||
      !ii_fetch(machine, frame, &insn->arguments[1], &b_value) ||
      (has_scale && !ii_tile_operand(machine, frame, insn, 4, &scale))) {
    return 0;
  }
  if (!a.is_tile && !ii_fetch(machine, frame, &insn->arguments[0], &a_value)) {
    return 0;
  }
  if (insn->arguments[3].kind != IR_OPERAND_NONE) {
    if (!ii_fetch(machine, frame, &insn->arguments[3], &ldb_value)) return 0;
    ldb = ldb_value.i;
  } else {
    ldb = ii_tile_is_view(b_type) ? (long long)b_type->view_extents[1]
                                  : (long long)desc->b_leading_dimension;
  }
  for (long long r = 0; r < (long long)desc->m; r++) {
    for (long long n = 0; n < (long long)desc->n; n++) {
      double accumulator;
      if (!ii_tile_operand_at(machine, &c, r, n, &accumulator)) return 0;
      accumulator = (double)(float)accumulator;
      if (has_scale) {
        double s;
        if (!ii_tile_operand_at(machine, &scale, r, 0, &s)) return 0;
        accumulator = (double)(float)(accumulator * s);
      }
      for (long long step = 0; step < (long long)desc->k; step += 16) {
        double sum = accumulator;
        for (long long k = step; k < step + 16 && k < (long long)desc->k; k++) {
          double av, bv;
          unsigned long long address;
          MtlcTypeKind kind;
          int esize;
          long long br = desc->b_layout == MTLC_TENSOR_LAYOUT_ROW_MAJOR ? k : n;
          long long bc = desc->b_layout == MTLC_TENSOR_LAYOUT_ROW_MAJOR ? n : k;
          if (!ii_tile_mma_operand_value(machine, &a,
                                         (unsigned long long)a_value.i, a_type,
                                         (long long)desc->a_leading_dimension,
                                         r, k, &av) ||
              !ii_tile_memory_element(machine,
                                      (unsigned long long)b_value.i, b_type,
                                      ldb, br, bc, &address, &kind, &esize) ||
              !ii_tile_read_bits(machine, address, b_kind_override, esize,
                                 &bv)) {
            return 0;
          }
          sum += av * bv;
        }
        accumulator = (double)(float)sum;
      }
      if (!ii_tile_set(machine, &d, r, n, accumulator)) return 0;
    }
  }
  return 1;
}

static int ii_exec_tile_view(IRInterpMachine *machine, IIFrame *frame,
                             const IRInstruction *insn);

static int ii_tile_check_agreement(IRInterpMachine *machine,
                                   const IRInstruction *insn,
                                   const IRInterpValue *base,
                                   const MtlcType *type) {
  IIGpuGrid *gpu = &machine->gpu;
  IIGpuThread *thread = machine->gpu_current;
  IITile tile;
  IITileTrace *trace;
  IIBuffer *buffer;
  long long offset = 0;
  long long bytes;
  unsigned long long hash = 1469598103934665603ULL;
  if (!gpu->active || !gpu->tile_traces || !thread ||
      !ii_tile_describe(type, &tile)) {
    return 1;
  }
  bytes = tile.rows * tile.cols * tile.esize;
  buffer = ii_addr_to_buffer(machine, (unsigned long long)base->i, (int)bytes,
                             &offset);
  if (!buffer) {
    return 1;
  }
  for (long long i = 0; i < bytes; i++) {
    hash ^= buffer->data[offset + i];
    hash *= 1099511628211ULL;
  }
  if ((size_t)(gpu->lane / II_GPU_SUBGROUP) >= gpu->tile_trace_warps) {
    return 1;
  }
  trace = &gpu->tile_traces[gpu->lane / II_GPU_SUBGROUP];
  if (thread->tile_cursor < trace->count) {
    IITileMark *mark = &trace->marks[thread->tile_cursor++];
    if (mark->line == insn->location.line && mark->hash != hash) {
      snprintf(machine->detail, sizeof(machine->detail),
               "the tile written at line %llu differs between work items "
               "%lld and %lld of one subgroup: a scalar it is built from varies",
               (unsigned long long)insn->location.line, mark->lane, gpu->lane);
      machine->status = IR_INTERP_TRAP;
      return 0;
    }
    return 1;
  }
  if (trace->count == trace->capacity) {
    size_t next = trace->capacity ? trace->capacity * 2 : 64;
    IITileMark *grown =
        (IITileMark *)realloc(trace->marks, next * sizeof(*grown));
    if (!grown) {
      return 1;
    }
    trace->marks = grown;
    trace->capacity = next;
  }
  trace->marks[trace->count].line = insn->location.line;
  trace->marks[trace->count].hash = hash;
  trace->marks[trace->count].lane = gpu->lane;
  trace->count++;
  thread->tile_cursor++;
  return 1;
}

static int ii_exec_tile(IRInterpMachine *machine, IIFrame *frame,
                        const IRInstruction *insn) {
  IRInstruction view = *insn;
  if (!insn->argument_count) {
    ii_fail(machine, IR_INTERP_UNSUPPORTED, "tile operation without operands");
    return 0;
  }
  view.dest = insn->arguments[0];
  view.arguments = insn->arguments + 1;
  view.argument_types = insn->argument_types ? insn->argument_types + 1 : NULL;
  view.argument_count = insn->argument_count - 1;
  if (!ii_exec_tile_view(machine, frame, &view)) {
    return 0;
  }
  if (insn->text && strcmp(insn->text, "store") != 0) {
    IRInterpValue base;
    if (!ii_fetch(machine, frame, &insn->arguments[0], &base)) {
      return 0;
    }
    return ii_tile_check_agreement(machine, insn, &base, insn->value_type);
  }
  return 1;
}

static int ii_exec_tile_view(IRInterpMachine *machine, IIFrame *frame,
                             const IRInstruction *insn) {
  const char *op = insn->text ? insn->text : "";
  IITile dest;
  if (!strcmp(op, "load")) return ii_tile_load_store(machine, frame, insn, 0);
  if (!strcmp(op, "store")) return ii_tile_load_store(machine, frame, insn, 1);
  if (!strcmp(op, "mma")) return ii_tile_mma(machine, frame, insn);
  if (!ii_tile_bind(machine, frame, &insn->dest, insn->value_type, &dest)) {
    return 0;
  }
  if (!strcmp(op, "row_max") || !strcmp(op, "row_sum")) {
    IITileOperand source;
    if (!ii_tile_operand(machine, frame, insn, 0, &source) || !source.is_tile) {
      if (machine->status == IR_INTERP_OK)
        ii_fail(machine, IR_INTERP_UNSUPPORTED, "row reduction of a non-tile");
      return 0;
    }
    return ii_tile_row_reduce(machine, &dest, &source.tile,
                              !strcmp(op, "row_sum"));
  }
  {
    IITileOperand operands[3];
    size_t count = insn->argument_count < 3 ? insn->argument_count : 3;
    for (size_t i = 0; i < count; i++) {
      if (!ii_tile_operand(machine, frame, insn, i, &operands[i])) return 0;
    }
    for (long long r = 0; r < dest.rows; r++) {
      for (long long c = 0; c < dest.cols; c++) {
        double values[3] = {0.0, 0.0, 0.0};
        double result = 0.0;
        for (size_t i = 0; i < count; i++) {
          if (!ii_tile_operand_at(machine, &operands[i], r, c, &values[i]))
            return 0;
        }
        if (!strcmp(op, "fill") || !strcmp(op, "copy") || !strcmp(op, "cast")) {
          result = values[0];
        } else if (!strcmp(op, "row")) {
          result = (double)r;
        } else if (!strcmp(op, "col")) {
          result = (double)c;
        } else if (!strcmp(op, "select")) {
          result = values[0] != 0.0 ? values[1] : values[2];
        } else if (count == 2) {
          MtlcTypeKind kind = operands[0].is_tile ? operands[0].tile.kind
                              : operands[1].is_tile ? operands[1].tile.kind
                                                    : dest.kind;
          if (!ii_tile_binary(op, kind, values[0], values[1], &result)) {
            ii_fail(machine, IR_INTERP_UNSUPPORTED, "unknown tile operation");
            return 0;
          }
          if (kind == MTLC_TYPE_FLOAT32 && dest.kind == MTLC_TYPE_FLOAT32) {
            result = (double)(float)result;
          }
        } else if (count == 1) {
          if (!ii_tile_unary(op, values[0], &result)) {
            ii_fail(machine, IR_INTERP_UNSUPPORTED, "unknown tile operation");
            return 0;
          }
        } else {
          ii_fail(machine, IR_INTERP_UNSUPPORTED, "unknown tile operation");
          return 0;
        }
        if (!ii_tile_set(machine, &dest, r, c, result)) return 0;
      }
    }
  }
  return 1;
}
