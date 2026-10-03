#include "../src/codegen/gpu_structure.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_NODES 4096
#define STEP_LIMIT 600
#define NONE SIZE_MAX

typedef struct {
  GpuCfgNode nodes[MAX_NODES];
  unsigned char header[MAX_NODES];
  size_t count;
  uint64_t rng;
  size_t loop_header[64];
  size_t loop_exit[64];
  size_t loop_depth;
  size_t join_stack[64];
  size_t join_loop_depth[64];
  size_t join_depth;
} Gen;

typedef struct {
  size_t trace[STEP_LIMIT + 2];
  size_t length;
  size_t returned;
  int truncated;
  size_t visits[MAX_NODES];
  uint64_t seed;
} Run;

static uint64_t next_random(uint64_t *state) {
  uint64_t x = *state;
  x ^= x << 13;
  x ^= x >> 7;
  x ^= x << 17;
  *state = x;
  return x;
}

static size_t pick(Gen *g, size_t n) {
  return (size_t)(next_random(&g->rng) % (uint64_t)n);
}

static size_t gen_node(Gen *g) {
  size_t n;
  if (g->count >= MAX_NODES - 8) {
    return NONE;
  }
  n = g->count++;
  g->nodes[n].exit = GPU_CFG_RETURN;
  g->nodes[n].target[0] = 0;
  g->nodes[n].target[1] = 0;
  g->header[n] = 0;
  return n;
}

static void jump(Gen *g, size_t from, size_t to) {
  g->nodes[from].exit = GPU_CFG_JUMP;
  g->nodes[from].target[0] = to;
  g->nodes[from].target[1] = to;
}

static void branch(Gen *g, size_t from, size_t yes, size_t no) {
  g->nodes[from].exit = GPU_CFG_BRANCH;
  g->nodes[from].target[0] = yes;
  g->nodes[from].target[1] = no;
}

static size_t gen_block(Gen *g, size_t cur, int depth);

static size_t gen_stmt(Gen *g, size_t cur, int depth) {
  size_t choice = depth > 5 ? pick(g, 2) : pick(g, 11);
  if (g->count > 700) {
    choice = 0;
  }
  if (choice == 0 || choice == 1) {
    size_t n = gen_node(g);
    if (n == NONE) {
      return cur;
    }
    jump(g, cur, n);
    return n;
  }
  if (choice == 2 || choice == 3) {
    size_t join = gen_node(g);
    size_t yes = gen_node(g);
    size_t no = pick(g, 3) == 0 ? join : gen_node(g);
    size_t end;
    if (join == NONE || yes == NONE || no == NONE) {
      return cur;
    }
    branch(g, cur, yes, no);
    g->join_stack[g->join_depth] = join;
    g->join_loop_depth[g->join_depth] = g->loop_depth;
    g->join_depth++;
    end = gen_block(g, yes, depth + 1);
    if (end != NONE) {
      jump(g, end, join);
    }
    if (no != join) {
      end = gen_block(g, no, depth + 1);
      if (end != NONE) {
        jump(g, end, join);
      }
    }
    g->join_depth--;
    return join;
  }
  if (choice == 4 || choice == 5) {
    size_t header = gen_node(g);
    size_t body = gen_node(g);
    size_t exit = gen_node(g);
    size_t end;
    if (header == NONE || body == NONE || exit == NONE) {
      return cur;
    }
    g->header[header] = 1;
    jump(g, cur, header);
    branch(g, header, body, exit);
    g->loop_header[g->loop_depth] = header;
    g->loop_exit[g->loop_depth] = exit;
    g->loop_depth++;
    end = gen_block(g, body, depth + 1);
    g->loop_depth--;
    if (end != NONE) {
      jump(g, end, header);
    }
    return exit;
  }
  if (choice == 6) {
    size_t body = gen_node(g);
    size_t cond = gen_node(g);
    size_t exit = gen_node(g);
    size_t end;
    if (body == NONE || cond == NONE || exit == NONE) {
      return cur;
    }
    g->header[body] = 1;
    jump(g, cur, body);
    g->loop_header[g->loop_depth] = body;
    g->loop_exit[g->loop_depth] = exit;
    g->loop_depth++;
    end = gen_block(g, body, depth + 1);
    g->loop_depth--;
    if (end != NONE) {
      jump(g, end, cond);
    }
    branch(g, cond, body, exit);
    return exit;
  }
  if (choice == 7 && g->loop_depth > 0) {
    size_t level = pick(g, g->loop_depth);
    jump(g, cur, g->loop_exit[g->loop_depth - 1 - level]);
    return NONE;
  }
  if (choice == 8 && g->loop_depth > 0) {
    size_t level = pick(g, g->loop_depth);
    jump(g, cur, g->loop_header[g->loop_depth - 1 - level]);
    return NONE;
  }
  if (choice == 9 && g->join_depth > 0) {
    size_t level = pick(g, g->join_depth);
    size_t index = g->join_depth - 1 - level;
    if (g->join_loop_depth[index] == g->loop_depth) {
      jump(g, cur, g->join_stack[index]);
      return NONE;
    }
  }
  if (choice == 10) {
    g->nodes[cur].exit = GPU_CFG_RETURN;
    return NONE;
  }
  {
    size_t n = gen_node(g);
    if (n == NONE) {
      return cur;
    }
    jump(g, cur, n);
    return n;
  }
}

static size_t gen_block(Gen *g, size_t cur, int depth) {
  size_t statements = 1 + pick(g, 4);
  for (size_t i = 0; i < statements && cur != NONE; i++) {
    cur = gen_stmt(g, cur, depth);
  }
  return cur;
}

static void thread_jumps(Gen *g) {
  for (size_t n = 0; n < g->count; n++) {
    GpuCfgNode *node = &g->nodes[n];
    size_t edges = node->exit == GPU_CFG_RETURN ? 0
                   : node->exit == GPU_CFG_JUMP ? 1
                                                : 2;
    for (size_t e = 0; e < edges; e++) {
      size_t target = node->target[e];
      if (pick(g, 4) != 0) {
        continue;
      }
      if (target != n && !g->header[target] &&
          g->nodes[target].exit == GPU_CFG_JUMP &&
          g->nodes[target].target[0] != target) {
        node->target[e] = g->nodes[target].target[0];
        if (node->exit == GPU_CFG_JUMP) {
          node->target[1] = node->target[0];
        }
      }
    }
  }
}

static int decide(Run *run, size_t node) {
  uint64_t x = run->seed ^ ((uint64_t)node * 0x9E3779B97F4A7C15ull) ^
               ((uint64_t)run->visits[node] * 0xC2B2AE3D27D4EB4Full);
  x ^= x >> 31;
  x *= 0xBF58476D1CE4E5B9ull;
  x ^= x >> 29;
  return (x & 7) < 4;
}

static int visit(Run *run, size_t node) {
  if (run->length >= STEP_LIMIT) {
    run->truncated = 1;
    return 0;
  }
  run->trace[run->length++] = node;
  run->visits[node]++;
  return 1;
}

static void walk_cfg(const GpuCfgNode *cfg, size_t entry, Run *run) {
  size_t node = entry;
  run->returned = NONE;
  for (;;) {
    if (!visit(run, node)) {
      return;
    }
    if (cfg[node].exit == GPU_CFG_RETURN) {
      run->returned = node;
      return;
    }
    if (cfg[node].exit == GPU_CFG_JUMP ||
        cfg[node].target[0] == cfg[node].target[1]) {
      node = cfg[node].target[0];
      continue;
    }
    node = decide(run, node) ? cfg[node].target[0] : cfg[node].target[1];
  }
}

enum { SIG_NORMAL = 0, SIG_BREAK, SIG_CONTINUE, SIG_RETURN, SIG_STOP };

typedef struct {
  Run *run;
  int exit_value;
  int bad;
} Exec;

static int exec_node(Exec *ex, const GpuSNode *node);

static int exec_seq(Exec *ex, const GpuSNode *seq) {
  if (!seq) {
    return SIG_NORMAL;
  }
  for (size_t i = 0; i < seq->child_count; i++) {
    int signal = exec_node(ex, seq->children[i]);
    if (signal != SIG_NORMAL) {
      return signal;
    }
  }
  return SIG_NORMAL;
}

static int exec_node(Exec *ex, const GpuSNode *node) {
  switch (node->kind) {
  case GPU_SNODE_SEQ:
    return exec_seq(ex, node);
  case GPU_SNODE_CODE:
    return visit(ex->run, node->block) ? SIG_NORMAL : SIG_STOP;
  case GPU_SNODE_IF:
    return decide(ex->run, node->block) ? exec_seq(ex, node->then_node)
                                        : exec_seq(ex, node->else_node);
  case GPU_SNODE_LOOP:
    for (;;) {
      int signal = exec_seq(ex, node->body);
      if (signal == SIG_BREAK) {
        return SIG_NORMAL;
      }
      if (signal == SIG_RETURN || signal == SIG_STOP) {
        return signal;
      }
    }
  case GPU_SNODE_BLOCK: {
    int signal = exec_seq(ex, node->body);
    return signal == SIG_BREAK ? SIG_NORMAL : signal;
  }
  case GPU_SNODE_BREAK:
    return SIG_BREAK;
  case GPU_SNODE_CONTINUE:
    return SIG_CONTINUE;
  case GPU_SNODE_RETURN:
    ex->run->returned = node->block;
    return SIG_RETURN;
  case GPU_SNODE_SET_EXIT:
    ex->exit_value = node->value;
    return SIG_NORMAL;
  case GPU_SNODE_CHECK:
    for (size_t i = 0; i < node->case_count; i++) {
      if (ex->exit_value != node->cases[i].value) {
        continue;
      }
      if (node->cases[i].action == GPU_EXIT_BREAK_KEEP) {
        return SIG_BREAK;
      }
      ex->exit_value = 0;
      return node->cases[i].action == GPU_EXIT_CONTINUE_CLEAR ? SIG_CONTINUE
                                                              : SIG_BREAK;
    }
    return SIG_NORMAL;
  default:
    ex->bad = 1;
    return SIG_STOP;
  }
}

static void walk_tree(const GpuStructure *s, Run *run) {
  Exec ex;
  int signal;
  memset(&ex, 0, sizeof(ex));
  ex.run = run;
  run->returned = NONE;
  signal = exec_seq(&ex, s->root);
  if (ex.bad || (signal != SIG_RETURN && signal != SIG_STOP) ||
      ex.exit_value != 0) {
    run->returned = NONE - 1;
  }
}

static size_t count_tree(const GpuSNode *node, GpuSNodeKind kind) {
  size_t total;
  if (!node) {
    return 0;
  }
  total = node->kind == kind ? 1 : 0;
  for (size_t i = 0; i < node->child_count; i++) {
    total += count_tree(node->children[i], kind);
  }
  total += count_tree(node->body, kind);
  total += count_tree(node->then_node, kind);
  total += count_tree(node->else_node, kind);
  return total;
}

static size_t count_kind(const GpuStructure *s, GpuSNodeKind kind) {
  return count_tree(s->root, kind);
}

static int check_graph(const GpuCfgNode *cfg, size_t count, size_t runs,
                       uint64_t seed, const char *what, size_t *flags) {
  GpuStructure s;
  char error[256];
  static Run a, b;
  if (!gpu_structure_build(cfg, count, 0, &s, error, sizeof(error))) {
    printf("FAIL %s: %s\n", what, error);
    return 0;
  }
  if (s.uses_exit && flags) {
    (*flags)++;
  }
  for (size_t r = 0; r < runs; r++) {
    memset(&a, 0, sizeof(a));
    memset(&b, 0, sizeof(b));
    a.seed = b.seed = seed * 1315423911ull + r * 2654435761ull + 1;
    walk_cfg(cfg, 0, &a);
    walk_tree(&s, &b);
    if (a.length != b.length || a.returned != b.returned ||
        a.truncated != b.truncated ||
        memcmp(a.trace, b.trace, a.length * sizeof(size_t)) != 0) {
      size_t at = 0;
      while (at < a.length && at < b.length && a.trace[at] == b.trace[at]) {
        at++;
      }
      printf("FAIL %s run %zu: traces differ at step %zu (cfg %zu steps "
             "ret %zu, tree %zu steps ret %zu)\n",
             what, r, at, a.length, a.returned, b.length, b.returned);
      gpu_structure_free(&s);
      return 0;
    }
  }
  gpu_structure_free(&s);
  return 1;
}

static int expect_irreducible(void) {
  GpuCfgNode cfg[4];
  GpuStructure s;
  char error[256];
  memset(cfg, 0, sizeof(cfg));
  cfg[0].exit = GPU_CFG_BRANCH;
  cfg[0].target[0] = 1;
  cfg[0].target[1] = 2;
  cfg[1].exit = GPU_CFG_BRANCH;
  cfg[1].target[0] = 2;
  cfg[1].target[1] = 3;
  cfg[2].exit = GPU_CFG_BRANCH;
  cfg[2].target[0] = 1;
  cfg[2].target[1] = 3;
  cfg[3].exit = GPU_CFG_RETURN;
  if (gpu_structure_build(cfg, 4, 0, &s, error, sizeof(error))) {
    gpu_structure_free(&s);
    printf("FAIL irreducible graph was structured\n");
    return 0;
  }
  if (!strstr(error, "irreducible")) {
    printf("FAIL irreducible graph refused for another reason: %s\n", error);
    return 0;
  }
  return 1;
}

static int expect_clean_if_else(void) {
  GpuCfgNode cfg[4];
  GpuStructure s;
  char error[256];
  int ok;
  memset(cfg, 0, sizeof(cfg));
  cfg[0].exit = GPU_CFG_BRANCH;
  cfg[0].target[0] = 1;
  cfg[0].target[1] = 2;
  cfg[1].exit = GPU_CFG_JUMP;
  cfg[1].target[0] = 3;
  cfg[2].exit = GPU_CFG_JUMP;
  cfg[2].target[0] = 3;
  cfg[3].exit = GPU_CFG_RETURN;
  if (!gpu_structure_build(cfg, 4, 0, &s, error, sizeof(error))) {
    printf("FAIL if-else: %s\n", error);
    return 0;
  }
  ok = count_kind(&s, GPU_SNODE_BREAK) == 0 &&
       s.root->child_count == 4 && s.root->children[1]->kind == GPU_SNODE_IF &&
       !s.uses_exit;
  if (!ok) {
    printf("FAIL if-else did not come back as a plain if/else\n");
  }
  gpu_structure_free(&s);
  return ok;
}

static int expect_clean_while(void) {
  GpuCfgNode cfg[4];
  GpuStructure s;
  char error[256];
  int ok;
  memset(cfg, 0, sizeof(cfg));
  cfg[0].exit = GPU_CFG_JUMP;
  cfg[0].target[0] = 1;
  cfg[1].exit = GPU_CFG_BRANCH;
  cfg[1].target[0] = 2;
  cfg[1].target[1] = 3;
  cfg[2].exit = GPU_CFG_JUMP;
  cfg[2].target[0] = 1;
  cfg[3].exit = GPU_CFG_RETURN;
  if (!gpu_structure_build(cfg, 4, 0, &s, error, sizeof(error))) {
    printf("FAIL while: %s\n", error);
    return 0;
  }
  ok = count_kind(&s, GPU_SNODE_LOOP) == 1 && !s.uses_exit &&
       count_kind(&s, GPU_SNODE_BLOCK) == 0;
  if (!ok) {
    printf("FAIL while loop needed blocks or flags\n");
  }
  gpu_structure_free(&s);
  return ok;
}

int main(int argc, char **argv) {
  size_t graphs = argc > 1 ? (size_t)strtoull(argv[1], NULL, 10) : 4000;
  size_t failures = 0;
  size_t flagged = 0;
  size_t total_nodes = 0;
  static Gen g;
  if (!expect_irreducible()) failures++;
  if (!expect_clean_if_else()) failures++;
  if (!expect_clean_while()) failures++;
  for (size_t i = 0; i < graphs && failures < 5; i++) {
    char what[64];
    size_t end;
    memset(&g, 0, sizeof(g));
    g.rng = 0x243F6A8885A308D3ull ^ (i * 0x9E3779B97F4A7C15ull);
    next_random(&g.rng);
    gen_node(&g);
    end = gen_block(&g, 0, 0);
    if (end != NONE) {
      g.nodes[end].exit = GPU_CFG_RETURN;
    }
    if (i % 2 == 1) {
      thread_jumps(&g);
    }
    total_nodes += g.count;
    snprintf(what, sizeof(what), "graph %zu (%zu nodes)", i, g.count);
    if (!check_graph(g.nodes, g.count, 24, i + 1, what, &flagged)) {
      failures++;
    }
  }
  printf("gpu_structure: %zu graphs, %zu nodes, %zu needed exit flags, %zu "
         "failures\n",
         graphs, total_nodes, flagged, failures);
  return failures == 0 ? 0 : 1;
}
