#include "gpu_structure.h"

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define GPU_NO_NODE SIZE_MAX

enum { FRAME_IF = 0, FRAME_LOOP, FRAME_BLOCK };

typedef struct {
  int kind;
  size_t label;
  size_t frame;
} AFrame;

typedef struct {
  size_t frame;
  int kind;
  int tail;
  size_t *escapes;
  size_t escape_count;
  size_t escape_capacity;
} LFrame;

typedef struct {
  const GpuCfgNode *cfg;
  size_t count;
  size_t entry;
  size_t *rpo;
  size_t *order;
  size_t reach_count;
  size_t *idom;
  size_t **preds;
  size_t *pred_count;
  unsigned char *loop_header;
  unsigned char *merge;
  unsigned char *followed;
  unsigned char **loop_body;
  size_t **children;
  size_t *child_count;
  int *frame_kind;
  size_t frame_capacity;
  AFrame *actx;
  size_t actx_count;
  size_t actx_capacity;
  LFrame *lctx;
  size_t lctx_count;
  size_t lctx_capacity;
  GpuStructure *out;
  char *error;
  size_t error_size;
  int failed;
} Builder;

static void fail(Builder *b, const char *format, ...) {
  va_list args;
  if (b->failed) {
    return;
  }
  b->failed = 1;
  if (b->error && b->error_size) {
    va_start(args, format);
    vsnprintf(b->error, b->error_size, format, args);
    va_end(args);
  }
}

static int grow(void **items, size_t *capacity, size_t needed,
                size_t element_size) {
  size_t next;
  void *grown;
  if (needed <= *capacity) {
    return 1;
  }
  next = *capacity ? *capacity * 2 : 8;
  while (next < needed) {
    next *= 2;
  }
  grown = realloc(*items, next * element_size);
  if (!grown) {
    return 0;
  }
  *items = grown;
  *capacity = next;
  return 1;
}

static GpuSNode *new_node(Builder *b, GpuSNodeKind kind) {
  GpuSNode *node;
  GpuStructure *out = b->out;
  if (b->failed) {
    return NULL;
  }
  if (!grow((void **)&out->nodes, &out->node_capacity, out->node_count + 1,
            sizeof(GpuSNode *))) {
    fail(b, "out of memory structuring control flow");
    return NULL;
  }
  node = calloc(1, sizeof(GpuSNode));
  if (!node) {
    fail(b, "out of memory structuring control flow");
    return NULL;
  }
  node->kind = kind;
  node->block = GPU_NO_NODE;
  node->frame = GPU_NO_NODE;
  out->nodes[out->node_count++] = node;
  return node;
}

static void seq_append(Builder *b, GpuSNode *seq, GpuSNode *child) {
  if (!seq || !child || b->failed) {
    return;
  }
  if (child->kind == GPU_SNODE_SEQ) {
    for (size_t i = 0; i < child->child_count; i++) {
      seq_append(b, seq, child->children[i]);
    }
    child->child_count = 0;
    return;
  }
  if (!grow((void **)&seq->children, &seq->child_capacity,
            seq->child_count + 1, sizeof(GpuSNode *))) {
    fail(b, "out of memory structuring control flow");
    return;
  }
  seq->children[seq->child_count++] = child;
}

static size_t successor_count(const GpuCfgNode *node) {
  if (node->exit == GPU_CFG_RETURN) {
    return 0;
  }
  if (node->exit == GPU_CFG_JUMP || node->target[0] == node->target[1]) {
    return 1;
  }
  return 2;
}

static int compute_rpo(Builder *b) {
  size_t *stack = calloc(b->count + 1, sizeof(size_t));
  size_t *next_child = calloc(b->count + 1, sizeof(size_t));
  unsigned char *seen = calloc(b->count + 1, 1);
  size_t *post = calloc(b->count + 1, sizeof(size_t));
  size_t depth = 0;
  size_t post_count = 0;
  if (!stack || !next_child || !seen || !post) {
    free(stack);
    free(next_child);
    free(seen);
    free(post);
    fail(b, "out of memory structuring control flow");
    return 0;
  }
  stack[depth++] = b->entry;
  seen[b->entry] = 1;
  while (depth > 0) {
    size_t node = stack[depth - 1];
    const GpuCfgNode *cfg = &b->cfg[node];
    size_t successors = successor_count(cfg);
    if (next_child[node] < successors) {
      size_t target = cfg->target[next_child[node]++];
      if (target >= b->count) {
        fail(b, "control flow edge %zu -> %zu leaves the function", node,
             target);
        break;
      }
      if (!seen[target]) {
        seen[target] = 1;
        stack[depth++] = target;
      }
      continue;
    }
    post[post_count++] = node;
    depth--;
  }
  if (!b->failed) {
    b->reach_count = post_count;
    for (size_t i = 0; i < b->count; i++) {
      b->rpo[i] = GPU_NO_NODE;
    }
    for (size_t i = 0; i < post_count; i++) {
      size_t node = post[post_count - 1 - i];
      b->order[i] = node;
      b->rpo[node] = i;
    }
  }
  free(stack);
  free(next_child);
  free(seen);
  free(post);
  return !b->failed;
}

static int compute_preds(Builder *b) {
  size_t *capacity = calloc(b->count + 1, sizeof(size_t));
  if (!capacity) {
    fail(b, "out of memory structuring control flow");
    return 0;
  }
  for (size_t i = 0; i < b->reach_count && !b->failed; i++) {
    size_t node = b->order[i];
    const GpuCfgNode *cfg = &b->cfg[node];
    size_t successors = successor_count(cfg);
    for (size_t s = 0; s < successors; s++) {
      size_t target = cfg->target[s];
      if (!grow((void **)&b->preds[target], &capacity[target],
                b->pred_count[target] + 1, sizeof(size_t))) {
        fail(b, "out of memory structuring control flow");
        break;
      }
      b->preds[target][b->pred_count[target]++] = node;
    }
  }
  free(capacity);
  return !b->failed;
}

static size_t intersect(Builder *b, size_t left, size_t right) {
  while (left != right) {
    while (b->rpo[left] > b->rpo[right]) {
      left = b->idom[left];
    }
    while (b->rpo[right] > b->rpo[left]) {
      right = b->idom[right];
    }
  }
  return left;
}

static void compute_dominators(Builder *b) {
  int changed = 1;
  for (size_t i = 0; i < b->count; i++) {
    b->idom[i] = GPU_NO_NODE;
  }
  b->idom[b->entry] = b->entry;
  while (changed) {
    changed = 0;
    for (size_t i = 1; i < b->reach_count; i++) {
      size_t node = b->order[i];
      size_t chosen = GPU_NO_NODE;
      for (size_t p = 0; p < b->pred_count[node]; p++) {
        size_t pred = b->preds[node][p];
        if (b->idom[pred] == GPU_NO_NODE) {
          continue;
        }
        chosen = chosen == GPU_NO_NODE ? pred : intersect(b, pred, chosen);
      }
      if (chosen != GPU_NO_NODE && b->idom[node] != chosen) {
        b->idom[node] = chosen;
        changed = 1;
      }
    }
  }
}

static int dominates(Builder *b, size_t dominator, size_t node) {
  for (;;) {
    if (node == dominator) {
      return 1;
    }
    if (node == b->entry) {
      return 0;
    }
    node = b->idom[node];
  }
}

static int classify(Builder *b) {
  for (size_t i = 0; i < b->reach_count && !b->failed; i++) {
    size_t node = b->order[i];
    size_t forward = 0;
    for (size_t p = 0; p < b->pred_count[node]; p++) {
      size_t pred = b->preds[node][p];
      if (b->rpo[pred] >= b->rpo[node]) {
        if (!dominates(b, node, pred)) {
          fail(b, "irreducible control flow: the edge %zu -> %zu enters a "
                  "loop other than through its header",
               pred, node);
          break;
        }
        b->loop_header[node] = 1;
      } else {
        forward++;
      }
    }
    b->merge[node] = forward >= 2;
  }
  return !b->failed;
}

static int compute_loop_bodies(Builder *b) {
  size_t *stack = calloc(b->count + 1, sizeof(size_t));
  if (!stack) {
    fail(b, "out of memory structuring control flow");
    return 0;
  }
  for (size_t i = 0; i < b->reach_count && !b->failed; i++) {
    size_t header = b->order[i];
    unsigned char *body;
    size_t depth = 0;
    if (!b->loop_header[header]) {
      continue;
    }
    body = calloc(b->count, 1);
    if (!body) {
      fail(b, "out of memory structuring control flow");
      break;
    }
    b->loop_body[header] = body;
    body[header] = 1;
    for (size_t p = 0; p < b->pred_count[header]; p++) {
      size_t pred = b->preds[header][p];
      if (b->rpo[pred] >= b->rpo[header] && !body[pred]) {
        body[pred] = 1;
        stack[depth++] = pred;
      }
    }
    while (depth > 0) {
      size_t node = stack[--depth];
      for (size_t p = 0; p < b->pred_count[node]; p++) {
        size_t pred = b->preds[node][p];
        if (!body[pred]) {
          body[pred] = 1;
          stack[depth++] = pred;
        }
      }
    }
  }
  free(stack);
  return !b->failed;
}

static int compute_children(Builder *b) {
  size_t *capacity = calloc(b->count + 1, sizeof(size_t));
  if (!capacity) {
    fail(b, "out of memory structuring control flow");
    return 0;
  }
  for (size_t i = 1; i < b->reach_count; i++) {
    size_t node = b->order[i];
    size_t parent = b->idom[node];
    if (!grow((void **)&b->children[parent], &capacity[parent],
              b->child_count[parent] + 1, sizeof(size_t))) {
      fail(b, "out of memory structuring control flow");
      break;
    }
    b->children[parent][b->child_count[parent]++] = node;
  }
  free(capacity);
  return !b->failed;
}

static size_t new_frame(Builder *b, int kind) {
  size_t frame = b->out->frame_count;
  if (!grow((void **)&b->frame_kind, &b->frame_capacity, frame + 1,
            sizeof(int))) {
    fail(b, "out of memory structuring control flow");
    return 0;
  }
  b->frame_kind[frame] = kind;
  b->out->frame_count++;
  return frame;
}

static int push_actx(Builder *b, int kind, size_t label, size_t frame) {
  if (!grow((void **)&b->actx, &b->actx_capacity, b->actx_count + 1,
            sizeof(AFrame))) {
    fail(b, "out of memory structuring control flow");
    return 0;
  }
  b->actx[b->actx_count].kind = kind;
  b->actx[b->actx_count].label = label;
  b->actx[b->actx_count].frame = frame;
  b->actx_count++;
  return 1;
}

static GpuSNode *do_tree(Builder *b, size_t node);

static GpuSNode *do_branch(Builder *b, size_t from, size_t to) {
  GpuSNode *br;
  int wanted;
  if (b->failed) {
    return NULL;
  }
  if (b->rpo[to] > b->rpo[from] && !b->merge[to] && !b->followed[to]) {
    return do_tree(b, to);
  }
  wanted = b->rpo[to] <= b->rpo[from] ? FRAME_LOOP : FRAME_BLOCK;
  for (size_t i = b->actx_count; i > 0; i--) {
    AFrame *frame = &b->actx[i - 1];
    if (frame->kind == wanted && frame->label == to) {
      br = new_node(b, GPU_SNODE_BR);
      if (br) {
        br->frame = frame->frame;
      }
      return br;
    }
  }
  fail(b, "control flow edge %zu -> %zu has no enclosing construct", from,
       to);
  return NULL;
}

static GpuSNode *node_within(Builder *b, size_t node, const size_t *merges,
                             size_t merge_count) {
  GpuSNode *seq = new_node(b, GPU_SNODE_SEQ);
  if (!seq) {
    return NULL;
  }
  if (merge_count > 0) {
    size_t follow = merges[0];
    size_t frame = new_frame(b, FRAME_BLOCK);
    GpuSNode *block = new_node(b, GPU_SNODE_BLOCK);
    if (!block || !push_actx(b, FRAME_BLOCK, follow, frame)) {
      return NULL;
    }
    block->frame = frame;
    block->body = node_within(b, node, merges + 1, merge_count - 1);
    b->actx_count--;
    seq_append(b, seq, block);
    seq_append(b, seq, do_tree(b, follow));
    return seq;
  }
  {
    const GpuCfgNode *cfg = &b->cfg[node];
    GpuSNode *code = new_node(b, GPU_SNODE_CODE);
    if (!code) {
      return NULL;
    }
    code->block = node;
    seq_append(b, seq, code);
    if (cfg->exit == GPU_CFG_RETURN) {
      GpuSNode *ret = new_node(b, GPU_SNODE_RETURN);
      if (ret) {
        ret->block = node;
      }
      seq_append(b, seq, ret);
    } else if (successor_count(cfg) == 1) {
      seq_append(b, seq, do_branch(b, node, cfg->target[0]));
    } else {
      GpuSNode *branch = new_node(b, GPU_SNODE_IF);
      if (!branch || !push_actx(b, FRAME_IF, GPU_NO_NODE, GPU_NO_NODE)) {
        return NULL;
      }
      branch->block = node;
      branch->then_node = new_node(b, GPU_SNODE_SEQ);
      branch->else_node = new_node(b, GPU_SNODE_SEQ);
      seq_append(b, branch->then_node, do_branch(b, node, cfg->target[0]));
      seq_append(b, branch->else_node, do_branch(b, node, cfg->target[1]));
      b->actx_count--;
      seq_append(b, seq, branch);
    }
  }
  return seq;
}

static GpuSNode *wrap_exits(Builder *b, size_t node, const size_t *exits,
                            size_t exit_count, const size_t *merges,
                            size_t merge_count) {
  GpuSNode *seq = new_node(b, GPU_SNODE_SEQ);
  if (!seq) {
    return NULL;
  }
  if (exit_count > 0) {
    size_t follow = exits[0];
    size_t frame = new_frame(b, FRAME_BLOCK);
    GpuSNode *block = new_node(b, GPU_SNODE_BLOCK);
    if (!block || !push_actx(b, FRAME_BLOCK, follow, frame)) {
      return NULL;
    }
    block->frame = frame;
    block->body = wrap_exits(b, node, exits + 1, exit_count - 1, merges,
                             merge_count);
    b->actx_count--;
    seq_append(b, seq, block);
    seq_append(b, seq, do_tree(b, follow));
    return seq;
  }
  {
    size_t frame = new_frame(b, FRAME_LOOP);
    GpuSNode *loop = new_node(b, GPU_SNODE_LOOP);
    if (!loop || !push_actx(b, FRAME_LOOP, node, frame)) {
      return NULL;
    }
    loop->frame = frame;
    loop->body = node_within(b, node, merges, merge_count);
    b->actx_count--;
    seq_append(b, seq, loop);
  }
  return seq;
}

static GpuSNode *do_tree(Builder *b, size_t node) {
  size_t merge_count = 0;
  size_t exit_count = 0;
  size_t *merges;
  size_t *exits;
  GpuSNode *result;
  if (b->failed) {
    return NULL;
  }
  merges = calloc(b->child_count[node] + 1, sizeof(size_t));
  exits = calloc(b->child_count[node] + 1, sizeof(size_t));
  if (!merges || !exits) {
    free(merges);
    free(exits);
    fail(b, "out of memory structuring control flow");
    return NULL;
  }
  for (size_t i = b->child_count[node]; i > 0; i--) {
    size_t child = b->children[node][i - 1];
    if (b->loop_header[node] && !b->loop_body[node][child]) {
      exits[exit_count++] = child;
      b->followed[child] = 1;
    } else if (b->merge[child]) {
      merges[merge_count++] = child;
    }
  }
  if (b->loop_header[node]) {
    result = wrap_exits(b, node, exits, exit_count, merges, merge_count);
  } else {
    result = node_within(b, node, merges, merge_count);
  }
  free(merges);
  free(exits);
  return result;
}

static int push_lctx(Builder *b, size_t frame, int kind, int tail) {
  if (!grow((void **)&b->lctx, &b->lctx_capacity, b->lctx_count + 1,
            sizeof(LFrame))) {
    fail(b, "out of memory structuring control flow");
    return 0;
  }
  memset(&b->lctx[b->lctx_count], 0, sizeof(LFrame));
  b->lctx[b->lctx_count].frame = frame;
  b->lctx[b->lctx_count].kind = kind;
  b->lctx[b->lctx_count].tail = tail;
  b->lctx_count++;
  return 1;
}

static void add_escape(Builder *b, LFrame *frame, size_t target) {
  for (size_t i = 0; i < frame->escape_count; i++) {
    if (frame->escapes[i] == target) {
      return;
    }
  }
  if (!grow((void **)&frame->escapes, &frame->escape_capacity,
            frame->escape_count + 1, sizeof(size_t))) {
    fail(b, "out of memory structuring control flow");
    return;
  }
  frame->escapes[frame->escape_count++] = target;
}

static int frame_direct(Builder *b, size_t target, int *found) {
  size_t position = GPU_NO_NODE;
  *found = 0;
  for (size_t i = b->lctx_count; i > 0; i--) {
    if (b->lctx[i - 1].frame == target) {
      position = i - 1;
      break;
    }
  }
  if (position == GPU_NO_NODE) {
    return 0;
  }
  *found = 1;
  if (b->lctx[position].kind == FRAME_LOOP) {
    for (size_t i = position + 1; i < b->lctx_count; i++) {
      if (b->lctx[i].kind == FRAME_LOOP) {
        return 0;
      }
    }
    return 1;
  }
  for (size_t i = position + 1; i < b->lctx_count; i++) {
    if (!b->lctx[i].tail) {
      return 0;
    }
    if (i + 1 < b->lctx_count && b->lctx[i].kind != FRAME_BLOCK) {
      return 0;
    }
  }
  return 1;
}

static void lower_seq(Builder *b, GpuSNode *seq, int tail);

static GpuSNode *lower_check(Builder *b, LFrame *closed) {
  GpuSNode *check;
  if (closed->escape_count == 0) {
    return NULL;
  }
  check = new_node(b, GPU_SNODE_CHECK);
  if (!check) {
    return NULL;
  }
  check->cases = calloc(closed->escape_count, sizeof(GpuExitCase));
  if (!check->cases) {
    fail(b, "out of memory structuring control flow");
    return NULL;
  }
  for (size_t i = 0; i < closed->escape_count && !b->failed; i++) {
    size_t target = closed->escapes[i];
    int found = 0;
    int direct = frame_direct(b, target, &found);
    GpuExitCase *entry = &check->cases[check->case_count++];
    entry->value = (int)target + 1;
    if (!found) {
      fail(b, "structured exit to construct %zu escapes the function",
           target);
      break;
    }
    if (direct) {
      entry->action = b->frame_kind[target] == FRAME_LOOP
                          ? GPU_EXIT_CONTINUE_CLEAR
                          : GPU_EXIT_BREAK_CLEAR;
    } else {
      entry->action = GPU_EXIT_BREAK_KEEP;
      add_escape(b, &b->lctx[b->lctx_count - 1], target);
    }
  }
  return check;
}

static void lower_seq(Builder *b, GpuSNode *seq, int tail) {
  size_t count;
  GpuSNode **old;
  if (!seq || b->failed) {
    return;
  }
  count = seq->child_count;
  old = seq->children;
  seq->children = NULL;
  seq->child_count = 0;
  seq->child_capacity = 0;
  for (size_t i = 0; i < count && !b->failed; i++) {
    GpuSNode *child = old[i];
    int child_tail = tail && i + 1 == count;
    switch (child->kind) {
    case GPU_SNODE_IF:
      lower_seq(b, child->then_node, child_tail);
      lower_seq(b, child->else_node, child_tail);
      seq_append(b, seq, child);
      break;
    case GPU_SNODE_LOOP:
    case GPU_SNODE_BLOCK: {
      LFrame closed;
      GpuSNode *check;
      if (!push_lctx(b, child->frame,
                     child->kind == GPU_SNODE_LOOP ? FRAME_LOOP : FRAME_BLOCK,
                     child_tail)) {
        break;
      }
      lower_seq(b, child->body, 1);
      closed = b->lctx[--b->lctx_count];
      seq_append(b, seq, child);
      check = lower_check(b, &closed);
      free(closed.escapes);
      if (check) {
        seq_append(b, seq, check);
      }
      break;
    }
    case GPU_SNODE_BR: {
      int found = 0;
      int direct = frame_direct(b, child->frame, &found);
      if (!found) {
        fail(b, "structured branch to construct %zu has no target",
             child->frame);
        break;
      }
      if (direct) {
        child->kind = b->frame_kind[child->frame] == FRAME_LOOP
                          ? GPU_SNODE_CONTINUE
                          : GPU_SNODE_BREAK;
        seq_append(b, seq, child);
      } else {
        GpuSNode *set = new_node(b, GPU_SNODE_SET_EXIT);
        if (!set) {
          break;
        }
        set->value = (int)child->frame + 1;
        child->kind = GPU_SNODE_BREAK;
        b->out->uses_exit = 1;
        seq_append(b, seq, set);
        seq_append(b, seq, child);
        add_escape(b, &b->lctx[b->lctx_count - 1], child->frame);
      }
      break;
    }
    default:
      seq_append(b, seq, child);
      break;
    }
  }
  free(old);
}

static int trim_tail(GpuSNode *seq, GpuSNodeKind kind, int through_blocks) {
  GpuSNode *last;
  if (!seq || seq->child_count == 0) {
    return 0;
  }
  last = seq->children[seq->child_count - 1];
  if (last->kind == kind) {
    seq->child_count--;
    return 1;
  }
  if (last->kind == GPU_SNODE_IF) {
    int changed = trim_tail(last->then_node, kind, through_blocks);
    changed |= trim_tail(last->else_node, kind, through_blocks);
    return changed;
  }
  if (through_blocks && last->kind == GPU_SNODE_BLOCK) {
    return trim_tail(last->body, kind, through_blocks);
  }
  return 0;
}

static void count_breaks(GpuSNode *seq, size_t innermost, size_t *counts) {
  if (!seq) {
    return;
  }
  for (size_t i = 0; i < seq->child_count; i++) {
    GpuSNode *child = seq->children[i];
    switch (child->kind) {
    case GPU_SNODE_BREAK:
      if (innermost != GPU_NO_NODE) {
        counts[innermost]++;
      }
      break;
    case GPU_SNODE_CHECK:
      for (size_t c = 0; c < child->case_count; c++) {
        if (child->cases[c].action != GPU_EXIT_CONTINUE_CLEAR &&
            innermost != GPU_NO_NODE) {
          counts[innermost]++;
        }
      }
      break;
    case GPU_SNODE_IF:
      count_breaks(child->then_node, innermost, counts);
      count_breaks(child->else_node, innermost, counts);
      break;
    case GPU_SNODE_LOOP:
    case GPU_SNODE_BLOCK:
      count_breaks(child->body, child->frame, counts);
      break;
    default:
      break;
    }
  }
}

static int trim_all(GpuSNode *seq) {
  int changed = 0;
  if (!seq) {
    return 0;
  }
  for (size_t i = 0; i < seq->child_count; i++) {
    GpuSNode *child = seq->children[i];
    if (child->kind == GPU_SNODE_IF) {
      changed |= trim_all(child->then_node);
      changed |= trim_all(child->else_node);
    } else if (child->kind == GPU_SNODE_BLOCK) {
      changed |= trim_tail(child->body, GPU_SNODE_BREAK, 0);
      changed |= trim_all(child->body);
    } else if (child->kind == GPU_SNODE_LOOP) {
      changed |= trim_tail(child->body, GPU_SNODE_CONTINUE, 1);
      changed |= trim_all(child->body);
    }
  }
  return changed;
}

static int unwrap_blocks(Builder *b, GpuSNode *seq, const size_t *counts) {
  int changed = 0;
  size_t count;
  GpuSNode **old;
  if (!seq || b->failed) {
    return 0;
  }
  for (size_t i = 0; i < seq->child_count; i++) {
    GpuSNode *child = seq->children[i];
    if (child->kind == GPU_SNODE_IF) {
      changed |= unwrap_blocks(b, child->then_node, counts);
      changed |= unwrap_blocks(b, child->else_node, counts);
    } else if (child->kind == GPU_SNODE_BLOCK ||
               child->kind == GPU_SNODE_LOOP) {
      changed |= unwrap_blocks(b, child->body, counts);
    }
  }
  count = seq->child_count;
  old = seq->children;
  seq->children = NULL;
  seq->child_count = 0;
  seq->child_capacity = 0;
  for (size_t i = 0; i < count; i++) {
    GpuSNode *child = old[i];
    if (child->kind == GPU_SNODE_BLOCK && counts[child->frame] == 0) {
      changed = 1;
      seq_append(b, seq, child->body);
      continue;
    }
    seq_append(b, seq, child);
  }
  free(old);
  return changed;
}

static void simplify(Builder *b) {
  int changed = 1;
  size_t *counts = calloc(b->out->frame_count + 1, sizeof(size_t));
  if (!counts) {
    fail(b, "out of memory structuring control flow");
    return;
  }
  while (changed && !b->failed) {
    changed = trim_all(b->out->root);
    memset(counts, 0, (b->out->frame_count + 1) * sizeof(size_t));
    count_breaks(b->out->root, GPU_NO_NODE, counts);
    changed |= unwrap_blocks(b, b->out->root, counts);
  }
  free(counts);
}

static void builder_free(Builder *b) {
  for (size_t i = 0; i < b->count; i++) {
    if (b->preds) {
      free(b->preds[i]);
    }
    if (b->children) {
      free(b->children[i]);
    }
    if (b->loop_body) {
      free(b->loop_body[i]);
    }
  }
  free(b->rpo);
  free(b->order);
  free(b->idom);
  free(b->preds);
  free(b->pred_count);
  free(b->loop_header);
  free(b->merge);
  free(b->followed);
  free(b->loop_body);
  free(b->children);
  free(b->child_count);
  free(b->frame_kind);
  free(b->actx);
  for (size_t i = 0; i < b->lctx_count; i++) {
    free(b->lctx[i].escapes);
  }
  free(b->lctx);
}

int gpu_structure_build(const GpuCfgNode *cfg, size_t count, size_t entry,
                        GpuStructure *out, char *error, size_t error_size) {
  Builder b;
  GpuSNode *root;
  memset(&b, 0, sizeof(b));
  if (error && error_size) {
    error[0] = '\0';
  }
  if (!out) {
    return 0;
  }
  memset(out, 0, sizeof(*out));
  b.cfg = cfg;
  b.count = count;
  b.entry = entry;
  b.out = out;
  b.error = error;
  b.error_size = error_size;
  if (!cfg || count == 0 || entry >= count) {
    fail(&b, "control flow graph is empty");
    return 0;
  }
  b.rpo = calloc(count, sizeof(size_t));
  b.order = calloc(count, sizeof(size_t));
  b.idom = calloc(count, sizeof(size_t));
  b.preds = calloc(count, sizeof(size_t *));
  b.pred_count = calloc(count, sizeof(size_t));
  b.loop_header = calloc(count, 1);
  b.merge = calloc(count, 1);
  b.followed = calloc(count, 1);
  b.loop_body = calloc(count, sizeof(unsigned char *));
  b.children = calloc(count, sizeof(size_t *));
  b.child_count = calloc(count, sizeof(size_t));
  if (!b.rpo || !b.order || !b.idom || !b.preds || !b.pred_count ||
      !b.loop_header || !b.merge || !b.followed || !b.loop_body ||
      !b.children || !b.child_count) {
    fail(&b, "out of memory structuring control flow");
  }
  if (!b.failed && compute_rpo(&b) && compute_preds(&b)) {
    compute_dominators(&b);
    if (classify(&b) && compute_loop_bodies(&b) && compute_children(&b)) {
      root = do_tree(&b, entry);
      if (!b.failed) {
        GpuSNode *seq = new_node(&b, GPU_SNODE_SEQ);
        seq_append(&b, seq, root);
        out->root = seq;
        lower_seq(&b, out->root, 0);
        if (!b.failed) {
          simplify(&b);
        }
      }
    }
  }
  builder_free(&b);
  if (b.failed) {
    gpu_structure_free(out);
    return 0;
  }
  return 1;
}

void gpu_structure_free(GpuStructure *structure) {
  if (!structure) {
    return;
  }
  for (size_t i = 0; i < structure->node_count; i++) {
    GpuSNode *node = structure->nodes[i];
    free(node->children);
    free(node->cases);
    free(node);
  }
  free(structure->nodes);
  memset(structure, 0, sizeof(*structure));
}
