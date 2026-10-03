
#ifndef MTLC_CONTEXT_H
#define MTLC_CONTEXT_H

#include "diag.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct MtlcContext MtlcContext;

MtlcContext *mtlc_context_create(void);
void mtlc_context_destroy(MtlcContext *ctx);

void mtlc_context_set_diagnostic_handler(MtlcContext *ctx,
                                         MtlcDiagHandler handler,
                                         void *user_data);

const char *mtlc_context_last_error(const MtlcContext *ctx);

void mtlc_context_clear_error(MtlcContext *ctx);

int mtlc_context_set_runtime_directory(MtlcContext *ctx, const char *path);
const char *mtlc_context_runtime_directory(const MtlcContext *ctx);

void mtlc_context_set_opt_level(MtlcContext *ctx, int level);
int mtlc_context_opt_level(const MtlcContext *ctx);

void mtlc_context_set_ml_opt(MtlcContext *ctx, int enabled);
int mtlc_context_ml_opt(const MtlcContext *ctx);

void mtlc_context_set_whole_program(MtlcContext *ctx, int enabled);
int mtlc_context_whole_program(const MtlcContext *ctx);

void mtlc_context_set_explain(MtlcContext *ctx, int enabled,
                              const char *focus_file);
int mtlc_context_explain(const MtlcContext *ctx);
const char *mtlc_context_explain_focus_file(const MtlcContext *ctx);

int mtlc_context_set_ptx_target(MtlcContext *ctx, const char *target,
                                int isa_major, int isa_minor);
const char *mtlc_context_ptx_target(const MtlcContext *ctx);
int mtlc_context_ptx_isa_major(const MtlcContext *ctx);
int mtlc_context_ptx_isa_minor(const MtlcContext *ctx);

int mtlc_context_set_ptx_tensor_tuple_budget(MtlcContext *ctx,
                                             int tuple_budget);
int mtlc_context_ptx_tensor_tuple_budget(const MtlcContext *ctx);

int mtlc_context_set_metal_version(MtlcContext *ctx, int major, int minor);
int mtlc_context_metal_version_major(const MtlcContext *ctx);
int mtlc_context_metal_version_minor(const MtlcContext *ctx);

#ifdef __cplusplus
}
#endif

#endif
