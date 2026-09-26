#include "codegen/binary/mir_cfg.h"
#include "../../common.h"

#include <stdlib.h>
#include <string.h>

typedef struct {
  size_t *slots;
  size_t mask;
} MirCfgLabelMap;

int mir_cfg_insn_reads_dst(const MirInst *in) {
  return in->op == MIR_CMOV || in->op == MIR_CMOVCC;
}

static int mir_cfg_operand_vregs(const MirOperand *op, MirVregId *out) {
  int n = 0;
  if (op->kind == MIR_OPK_VREG) {
    out[n++] = op->vreg;
  } else if (op->kind == MIR_OPK_MEM) {
    out[n++] = op->mem.base;
    out[n++] = op->mem.index;
  }
  return n;
}

int mir_cfg_insn_uses(const MirInst *in, MirVregId *out) {
  int n = 0;
  MirVregId tmp[2];
  int k;
  if (in->op == MIR_NOP) {
    return 0;
  }
  if (in->dst.kind == MIR_OPK_MEM || mir_cfg_insn_reads_dst(in)) {
    k = mir_cfg_operand_vregs(&in->dst, tmp);
    for (int j = 0; j < k; j++) {
      out[n++] = tmp[j];
    }
  }
  k = mir_cfg_operand_vregs(&in->a, tmp);
  for (int j = 0; j < k; j++) {
    out[n++] = tmp[j];
  }
  k = mir_cfg_operand_vregs(&in->b, tmp);
  for (int j = 0; j < k; j++) {
    out[n++] = tmp[j];
  }
  return n;
}

MirVregId mir_cfg_insn_def(const MirInst *in) {
  if (in->op != MIR_NOP && in->dst.kind == MIR_OPK_VREG) {
    return in->dst.vreg;
  }
  return MIR_VREG_NONE;
}

static int mir_cfg_ends_block(const MirInst *in) {
  return in->op == MIR_JMP || in->op == MIR_JCC || in->op == MIR_CMPBR ||
         in->op == MIR_FCMPBR || in->op == MIR_JMP_TABLE || in->op == MIR_RET ||
         in->op == MIR_TRAP;
}

static int mir_cfg_is_label(const MirInst *in) {
  return in->op == MIR_LABEL && in->dst.kind == MIR_OPK_LABEL && in->dst.sym;
}

static int mir_cfg_label_map_build(const MirFunction *fn, MirCfgLabelMap *map) {
  size_t label_count = 0;
  size_t slot_count = 16;
  for (size_t i = 0; i < fn->insn_count; i++) {
    if (mir_cfg_is_label(&fn->insns[i])) {
      label_count++;
    }
  }
  while (slot_count < label_count * 2) {
    slot_count *= 2;
  }
  map->slots = (size_t *)calloc(slot_count, sizeof(*map->slots));
  map->mask = slot_count - 1;
  if (!map->slots) {
    return 0;
  }
  for (size_t i = 0; i < fn->insn_count; i++) {
    const MirInst *in = &fn->insns[i];
    size_t h;
    if (!mir_cfg_is_label(in)) {
      continue;
    }
    h = mettle_fnv1a_hash(in->dst.sym) & map->mask;
    while (map->slots[h]) {
      if (strcmp(fn->insns[map->slots[h] - 1].dst.sym, in->dst.sym) == 0) {
        break;
      }
      h = (h + 1) & map->mask;
    }
    if (!map->slots[h]) {
      map->slots[h] = i + 1;
    }
  }
  return 1;
}

static int mir_cfg_label_find(const MirFunction *fn, const MirCfgLabelMap *map,
                              const char *name) {
  size_t h;
  if (!name || !map->slots) {
    return -1;
  }
  h = mettle_fnv1a_hash(name) & map->mask;
  while (map->slots[h]) {
    if (strcmp(fn->insns[map->slots[h] - 1].dst.sym, name) == 0) {
      return (int)(map->slots[h] - 1);
    }
    h = (h + 1) & map->mask;
  }
  return -1;
}

static int mir_cfg_edge_add(MirCfg *cfg, int *head, int to) {
  if (cfg->edge_count == cfg->edge_capacity) {
    size_t cap = cfg->edge_capacity ? cfg->edge_capacity * 2 : 64;
    int *n1 = (int *)realloc(cfg->edge_next, cap * sizeof(int));
    int *n2;
    if (!n1) {
      return 0;
    }
    cfg->edge_next = n1;
    n2 = (int *)realloc(cfg->edge_to, cap * sizeof(int));
    if (!n2) {
      return 0;
    }
    cfg->edge_to = n2;
    cfg->edge_capacity = cap;
  }
  cfg->edge_to[cfg->edge_count] = to;
  cfg->edge_next[cfg->edge_count] = *head;
  *head = (int)cfg->edge_count;
  cfg->edge_count++;
  return 1;
}

static int mir_cfg_split_blocks(MirCfg *cfg) {
  const MirFunction *fn = cfg->fn;
  size_t nblocks = 0;
  cfg->block_of = (int *)malloc(fn->insn_count * sizeof(int));
  if (!cfg->block_of) {
    return 0;
  }
  for (size_t i = 0; i < fn->insn_count; i++) {
    int leader = i == 0 || mir_cfg_is_label(&fn->insns[i]) ||
                 mir_cfg_ends_block(&fn->insns[i - 1]);
    if (leader) {
      nblocks++;
    }
    cfg->block_of[i] = (int)nblocks - 1;
  }
  cfg->blocks = (MirCfgBlock *)calloc(nblocks, sizeof(MirCfgBlock));
  if (!cfg->blocks) {
    return 0;
  }
  cfg->block_count = nblocks;
  for (size_t b = 0; b < nblocks; b++) {
    cfg->blocks[b].succ_head = -1;
    cfg->blocks[b].pred_head = -1;
    cfg->blocks[b].idom = -1;
    cfg->blocks[b].rpo = -1;
    cfg->blocks[b].loop_header = -1;
  }
  for (size_t i = 0; i < fn->insn_count; i++) {
    MirCfgBlock *blk = &cfg->blocks[cfg->block_of[i]];
    if (i == 0 || cfg->block_of[i - 1] != cfg->block_of[i]) {
      blk->start = (int)i;
    }
    blk->end = (int)i + 1;
  }
  return 1;
}

static int mir_cfg_link(MirCfg *cfg, size_t from, int target_insn) {
  int to;
  if (target_insn < 0) {
    return 0;
  }
  to = cfg->block_of[target_insn];
  return mir_cfg_edge_add(cfg, &cfg->blocks[from].succ_head, to) &&
         mir_cfg_edge_add(cfg, &cfg->blocks[to].pred_head, (int)from);
}

static int mir_cfg_link_block(MirCfg *cfg, const MirCfgLabelMap *map,
                              size_t b) {
  const MirFunction *fn = cfg->fn;
  const MirInst *last = &fn->insns[cfg->blocks[b].end - 1];
  int fall = 1;
  if (last->op == MIR_JMP) {
    fall = 0;
    if (!mir_cfg_link(cfg, b, mir_cfg_label_find(fn, map, last->dst.sym))) {
      return 0;
    }
  } else if (last->op == MIR_JCC || last->op == MIR_CMPBR ||
             last->op == MIR_FCMPBR) {
    if (!mir_cfg_link(cfg, b, mir_cfg_label_find(fn, map, last->dst.sym))) {
      return 0;
    }
  } else if (last->op == MIR_JMP_TABLE) {
    const MirJumpTable *table = (const MirJumpTable *)last->aux;
    fall = 0;
    if (!table) {
      return 0;
    }
    for (size_t t = 0; t < table->count; t++) {
      if (!mir_cfg_link(cfg, b, mir_cfg_label_find(fn, map, table->labels[t]))) {
        return 0;
      }
    }
  } else if (last->op == MIR_RET || last->op == MIR_TRAP) {
    fall = 0;
  }
  if (fall && b + 1 < cfg->block_count) {
    return mir_cfg_link(cfg, b, cfg->blocks[b + 1].start);
  }
  return 1;
}

static int mir_cfg_build_edges(MirCfg *cfg) {
  MirCfgLabelMap map;
  int ok = 1;
  if (!mir_cfg_label_map_build(cfg->fn, &map)) {
    return 0;
  }
  for (size_t b = 0; ok && b < cfg->block_count; b++) {
    ok = mir_cfg_link_block(cfg, &map, b);
  }
  free(map.slots);
  return ok;
}

static int mir_cfg_build_rpo(MirCfg *cfg) {
  size_t n = cfg->block_count;
  int *stack = (int *)malloc(n * sizeof(int));
  int *edge = (int *)malloc(n * sizeof(int));
  unsigned char *seen = (unsigned char *)calloc(n, 1);
  size_t sp = 0;
  size_t post = 0;
  cfg->rpo_order = (int *)malloc(n * sizeof(int));
  if (!stack || !edge || !seen || !cfg->rpo_order) {
    free(stack);
    free(edge);
    free(seen);
    return 0;
  }
  stack[sp] = 0;
  edge[sp] = cfg->blocks[0].succ_head;
  seen[0] = 1;
  sp++;
  while (sp > 0) {
    int b = stack[sp - 1];
    int e = edge[sp - 1];
    if (e >= 0) {
      int to = cfg->edge_to[e];
      edge[sp - 1] = cfg->edge_next[e];
      if (!seen[to]) {
        seen[to] = 1;
        stack[sp] = to;
        edge[sp] = cfg->blocks[to].succ_head;
        sp++;
      }
      continue;
    }
    cfg->rpo_order[post++] = b;
    sp--;
  }
  cfg->rpo_count = post;
  for (size_t k = 0; k < post / 2; k++) {
    int t = cfg->rpo_order[k];
    cfg->rpo_order[k] = cfg->rpo_order[post - 1 - k];
    cfg->rpo_order[post - 1 - k] = t;
  }
  for (size_t k = 0; k < post; k++) {
    cfg->blocks[cfg->rpo_order[k]].rpo = (int)k;
  }
  free(stack);
  free(edge);
  free(seen);
  return 1;
}

static int mir_cfg_intersect(const MirCfg *cfg, int a, int b) {
  while (a != b) {
    while (cfg->blocks[a].rpo > cfg->blocks[b].rpo) {
      a = cfg->blocks[a].idom;
    }
    while (cfg->blocks[b].rpo > cfg->blocks[a].rpo) {
      b = cfg->blocks[b].idom;
    }
  }
  return a;
}

static void mir_cfg_build_dominators(MirCfg *cfg) {
  int changed = 1;
  cfg->blocks[0].idom = 0;
  while (changed) {
    changed = 0;
    for (size_t k = 1; k < cfg->rpo_count; k++) {
      int b = cfg->rpo_order[k];
      int new_idom = -1;
      for (int e = cfg->blocks[b].pred_head; e >= 0; e = cfg->edge_next[e]) {
        int p = cfg->edge_to[e];
        if (cfg->blocks[p].idom < 0) {
          continue;
        }
        new_idom = new_idom < 0 ? p : mir_cfg_intersect(cfg, p, new_idom);
      }
      if (new_idom >= 0 && cfg->blocks[b].idom != new_idom) {
        cfg->blocks[b].idom = new_idom;
        changed = 1;
      }
    }
  }
}

static int mir_cfg_dominates(const MirCfg *cfg, int a, int b) {
  while (b >= 0) {
    if (a == b) {
      return 1;
    }
    if (cfg->blocks[b].idom == b) {
      return 0;
    }
    b = cfg->blocks[b].idom;
  }
  return 0;
}

static void mir_cfg_loop_members(const MirCfg *cfg, int header, int tail,
                                 unsigned char *member, int *work) {
  size_t wp = 0;
  memset(member, 0, cfg->block_count);
  member[header] = 1;
  if (!member[tail]) {
    member[tail] = 1;
    work[wp++] = tail;
  }
  while (wp > 0) {
    int b = work[--wp];
    for (int e = cfg->blocks[b].pred_head; e >= 0; e = cfg->edge_next[e]) {
      int p = cfg->edge_to[e];
      if (!member[p]) {
        member[p] = 1;
        work[wp++] = p;
      }
    }
  }
}

static int mir_cfg_is_back_edge(const MirCfg *cfg, size_t from, int to) {
  if (cfg->blocks[from].rpo < 0 || cfg->blocks[to].rpo < 0 ||
      cfg->blocks[to].rpo > cfg->blocks[from].rpo) {
    return 0;
  }
  return mir_cfg_dominates(cfg, to, (int)from);
}

static void mir_cfg_assign_headers(MirCfg *cfg, int header,
                                   const unsigned char *member) {
  for (size_t b = 0; b < cfg->block_count; b++) {
    int cur = cfg->blocks[b].loop_header;
    if (!member[b]) {
      continue;
    }
    if (cur < 0 || cfg->blocks[header].loop_depth > cfg->blocks[cur].loop_depth) {
      cfg->blocks[b].loop_header = header;
    }
  }
}

static void mir_cfg_visit_loops(MirCfg *cfg, unsigned char *member, int *work,
                                int assign) {
  for (size_t b = 0; b < cfg->block_count; b++) {
    for (int e = cfg->blocks[b].succ_head; e >= 0; e = cfg->edge_next[e]) {
      int h = cfg->edge_to[e];
      if (!mir_cfg_is_back_edge(cfg, b, h)) {
        continue;
      }
      mir_cfg_loop_members(cfg, h, (int)b, member, work);
      if (assign) {
        mir_cfg_assign_headers(cfg, h, member);
        continue;
      }
      for (size_t k = 0; k < cfg->block_count; k++) {
        if (member[k]) {
          cfg->blocks[k].loop_depth++;
        }
      }
    }
  }
}

static int mir_cfg_build_loops(MirCfg *cfg) {
  size_t n = cfg->block_count;
  unsigned char *member = (unsigned char *)malloc(n);
  int *work = (int *)malloc(n * sizeof(int));
  if (!member || !work) {
    free(member);
    free(work);
    return 0;
  }
  mir_cfg_visit_loops(cfg, member, work, 0);
  mir_cfg_visit_loops(cfg, member, work, 1);
  free(member);
  free(work);
  cfg->insn_depth = (unsigned char *)calloc(cfg->fn->insn_count, 1);
  if (!cfg->insn_depth) {
    return 0;
  }
  for (size_t i = 0; i < cfg->fn->insn_count; i++) {
    int d = cfg->blocks[cfg->block_of[i]].loop_depth;
    cfg->insn_depth[i] = (unsigned char)(d > 255 ? 255 : d);
  }
  return 1;
}

static int mir_cfg_alloc_sets(MirCfg *cfg) {
  size_t cells = cfg->block_count * cfg->words;
  cfg->use = (unsigned long long *)calloc(cells, sizeof(unsigned long long));
  cfg->def = (unsigned long long *)calloc(cells, sizeof(unsigned long long));
  cfg->live_in = (unsigned long long *)calloc(cells, sizeof(unsigned long long));
  cfg->live_out =
      (unsigned long long *)calloc(cells, sizeof(unsigned long long));
  cfg->defd_in = (unsigned long long *)calloc(cells, sizeof(unsigned long long));
  cfg->defd_out =
      (unsigned long long *)calloc(cells, sizeof(unsigned long long));
  return cfg->use && cfg->def && cfg->live_in && cfg->live_out &&
         cfg->defd_in && cfg->defd_out;
}

static void mir_cfg_local_sets(MirCfg *cfg) {
  const MirFunction *fn = cfg->fn;
  for (size_t b = 0; b < cfg->block_count; b++) {
    unsigned long long *u = cfg->use + b * cfg->words;
    unsigned long long *d = cfg->def + b * cfg->words;
    for (int i = cfg->blocks[b].start; i < cfg->blocks[b].end; i++) {
      const MirInst *in = &fn->insns[i];
      MirVregId ids[6];
      int n = mir_cfg_insn_uses(in, ids);
      MirVregId def = mir_cfg_insn_def(in);
      for (int k = 0; k < n; k++) {
        if (ids[k] >= 0 && (size_t)ids[k] < fn->vreg_count &&
            !mir_cfg_set_get(d, (size_t)ids[k])) {
          mir_cfg_set_add(u, (size_t)ids[k]);
        }
      }
      if (def >= 0 && (size_t)def < fn->vreg_count) {
        mir_cfg_set_add(d, (size_t)def);
      }
    }
  }
}

static void mir_cfg_liveness(MirCfg *cfg) {
  size_t words = cfg->words;
  int changed = 1;
  while (changed) {
    changed = 0;
    for (size_t bi = cfg->block_count; bi > 0; bi--) {
      size_t b = bi - 1;
      unsigned long long *out = cfg->live_out + b * words;
      unsigned long long *in = cfg->live_in + b * words;
      const unsigned long long *u = cfg->use + b * words;
      const unsigned long long *d = cfg->def + b * words;
      for (int e = cfg->blocks[b].succ_head; e >= 0; e = cfg->edge_next[e]) {
        const unsigned long long *s = cfg->live_in + (size_t)cfg->edge_to[e] * words;
        for (size_t w = 0; w < words; w++) {
          out[w] |= s[w];
        }
      }
      for (size_t w = 0; w < words; w++) {
        unsigned long long next = u[w] | (out[w] & ~d[w]);
        if (next != in[w]) {
          in[w] = next;
          changed = 1;
        }
      }
    }
  }
}

static void mir_cfg_seed_entry_defs(MirCfg *cfg) {
  const MirFunction *fn = cfg->fn;
  unsigned long long *entry = cfg->defd_in;
  for (size_t p = 0; p < fn->param_count; p++) {
    MirVregId v = fn->params[p].vreg;
    MirVregId s = fn->params[p].sysv_storage;
    if (v >= 0 && (size_t)v < fn->vreg_count) {
      mir_cfg_set_add(entry, (size_t)v);
    }
    if (s >= 0 && (size_t)s < fn->vreg_count) {
      mir_cfg_set_add(entry, (size_t)s);
    }
  }
  if (fn->returns_indirect && fn->indirect_return_vreg >= 0 &&
      (size_t)fn->indirect_return_vreg < fn->vreg_count) {
    mir_cfg_set_add(entry, (size_t)fn->indirect_return_vreg);
  }
}

static int mir_cfg_build_order(MirCfg *cfg) {
  size_t n = 0;
  cfg->order = (int *)malloc(cfg->block_count * sizeof(int));
  if (!cfg->order) {
    return 0;
  }
  for (size_t k = 0; k < cfg->rpo_count; k++) {
    cfg->order[n++] = cfg->rpo_order[k];
  }
  for (size_t b = 0; b < cfg->block_count; b++) {
    if (cfg->blocks[b].rpo < 0) {
      cfg->order[n++] = (int)b;
    }
  }
  return 1;
}

static void mir_cfg_definedness(MirCfg *cfg) {
  size_t words = cfg->words;
  int changed = 1;
  mir_cfg_seed_entry_defs(cfg);
  while (changed) {
    changed = 0;
    for (size_t k = 0; k < cfg->block_count; k++) {
      size_t b = cfg->order[k];
      unsigned long long *in = cfg->defd_in + b * words;
      unsigned long long *out = cfg->defd_out + b * words;
      const unsigned long long *d = cfg->def + b * words;
      for (int e = cfg->blocks[b].pred_head; e >= 0; e = cfg->edge_next[e]) {
        const unsigned long long *p =
            cfg->defd_out + (size_t)cfg->edge_to[e] * words;
        for (size_t w = 0; w < words; w++) {
          in[w] |= p[w];
        }
      }
      for (size_t w = 0; w < words; w++) {
        unsigned long long next = in[w] | d[w];
        if (next != out[w]) {
          out[w] = next;
          changed = 1;
        }
      }
    }
  }
}

void mir_cfg_free(MirCfg *cfg) {
  free(cfg->blocks);
  free(cfg->block_of);
  free(cfg->edge_next);
  free(cfg->edge_to);
  free(cfg->rpo_order);
  free(cfg->order);
  free(cfg->use);
  free(cfg->def);
  free(cfg->live_in);
  free(cfg->live_out);
  free(cfg->defd_in);
  free(cfg->defd_out);
  free(cfg->insn_depth);
  memset(cfg, 0, sizeof(*cfg));
}

int mir_cfg_build(MirCfg *cfg, const MirFunction *fn) {
  memset(cfg, 0, sizeof(*cfg));
  cfg->fn = fn;
  cfg->words = (fn->vreg_count + 63) / 64;
  if (fn->insn_count == 0 || fn->vreg_count == 0) {
    return 0;
  }
  if (!mir_cfg_split_blocks(cfg) || !mir_cfg_build_edges(cfg) ||
      !mir_cfg_build_rpo(cfg)) {
    mir_cfg_free(cfg);
    return 0;
  }
  mir_cfg_build_dominators(cfg);
  if (!mir_cfg_build_order(cfg) || !mir_cfg_build_loops(cfg) ||
      !mir_cfg_alloc_sets(cfg)) {
    mir_cfg_free(cfg);
    return 0;
  }
  mir_cfg_local_sets(cfg);
  mir_cfg_liveness(cfg);
  mir_cfg_definedness(cfg);
  free(cfg->use);
  free(cfg->def);
  cfg->use = NULL;
  cfg->def = NULL;
  return 1;
}

int mir_cfg_cursor_init(MirCfgCursor *cur, const MirCfg *cfg) {
  memset(cur, 0, sizeof(*cur));
  cur->cfg = cfg;
  cur->live = (unsigned long long *)calloc(cfg->words, sizeof(unsigned long long));
  cur->defd = (unsigned long long *)calloc(cfg->words, sizeof(unsigned long long));
  cur->count = (int *)calloc(cfg->fn->vreg_count, sizeof(int));
  return cur->live && cur->defd && cur->count;
}

void mir_cfg_cursor_free(MirCfgCursor *cur) {
  free(cur->live);
  free(cur->defd);
  free(cur->count);
  memset(cur, 0, sizeof(*cur));
}

void mir_cfg_cursor_start_block(MirCfgCursor *cur, size_t block) {
  const MirCfg *cfg = cur->cfg;
  const MirCfgBlock *blk = &cfg->blocks[block];
  cur->block = block;
  cur->at = (size_t)blk->end;
  memcpy(cur->live, cfg->live_out + block * cfg->words,
         cfg->words * sizeof(unsigned long long));
  memcpy(cur->defd, cfg->defd_out + block * cfg->words,
         cfg->words * sizeof(unsigned long long));
  for (int i = blk->start; i < blk->end; i++) {
    MirVregId d = mir_cfg_insn_def(&cfg->fn->insns[i]);
    if (d >= 0 && (size_t)d < cfg->fn->vreg_count) {
      cur->count[d]++;
    }
  }
}

void mir_cfg_cursor_step_back(MirCfgCursor *cur) {
  const MirCfg *cfg = cur->cfg;
  const MirInst *in;
  MirVregId ids[6];
  MirVregId d;
  int n;
  if (cur->at == 0 || (int)cur->at <= cfg->blocks[cur->block].start) {
    return;
  }
  cur->at--;
  in = &cfg->fn->insns[cur->at];
  d = mir_cfg_insn_def(in);
  if (d >= 0 && (size_t)d < cfg->fn->vreg_count) {
    mir_cfg_set_clear(cur->live, (size_t)d);
    cur->count[d]--;
    if (cur->count[d] == 0 &&
        !mir_cfg_set_get(cfg->defd_in + cur->block * cfg->words, (size_t)d)) {
      mir_cfg_set_clear(cur->defd, (size_t)d);
    }
  }
  n = mir_cfg_insn_uses(in, ids);
  for (int k = 0; k < n; k++) {
    if (ids[k] >= 0 && (size_t)ids[k] < cfg->fn->vreg_count) {
      mir_cfg_set_add(cur->live, (size_t)ids[k]);
    }
  }
}
