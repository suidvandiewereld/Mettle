#ifndef MTLC_GPU_STRUCTURE_H
#define MTLC_GPU_STRUCTURE_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
  GPU_CFG_JUMP = 0,
  GPU_CFG_BRANCH,
  GPU_CFG_RETURN
} GpuCfgExit;

typedef struct {
  GpuCfgExit exit;
  size_t target[2];
} GpuCfgNode;

typedef enum {
  GPU_SNODE_SEQ = 0,
  GPU_SNODE_CODE,
  GPU_SNODE_IF,
  GPU_SNODE_LOOP,
  GPU_SNODE_BLOCK,
  GPU_SNODE_BR,
  GPU_SNODE_BREAK,
  GPU_SNODE_CONTINUE,
  GPU_SNODE_RETURN,
  GPU_SNODE_SET_EXIT,
  GPU_SNODE_CHECK
} GpuSNodeKind;

typedef enum {
  GPU_EXIT_BREAK_CLEAR = 0,
  GPU_EXIT_CONTINUE_CLEAR,
  GPU_EXIT_BREAK_KEEP
} GpuExitAction;

typedef struct {
  int value;
  GpuExitAction action;
} GpuExitCase;

typedef struct GpuSNode GpuSNode;

struct GpuSNode {
  GpuSNodeKind kind;
  size_t block;
  size_t frame;
  int value;
  GpuSNode **children;
  size_t child_count;
  size_t child_capacity;
  GpuSNode *body;
  GpuSNode *then_node;
  GpuSNode *else_node;
  GpuExitCase *cases;
  size_t case_count;
};

typedef struct {
  GpuSNode *root;
  GpuSNode **nodes;
  size_t node_count;
  size_t node_capacity;
  size_t frame_count;
  int uses_exit;
} GpuStructure;

int gpu_structure_build(const GpuCfgNode *cfg, size_t count, size_t entry,
                        GpuStructure *out, char *error, size_t error_size);

void gpu_structure_free(GpuStructure *structure);

#ifdef __cplusplus
}
#endif

#endif
