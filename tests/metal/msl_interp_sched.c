#include "msl_interp_internal.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int msl_thread_start(MslRun *run, MslThread *t, const MslFunction *fn);
void msl_thread_unwind(MslRun *run, MslThread *t);
int msl_write_local(MslRun *run, MslThread *t, int local, const uint64_t *slots);

static uint64_t msl_step_budget = 200000000ull;

uint64_t msl_set_step_budget(uint64_t budget) {
  uint64_t old = msl_step_budget;
  msl_step_budget = budget == 0 ? 200000000ull : budget;
  return old;
}

static const MslFunction *find_kernel(const MslProgram *prog, const char *name) {
  size_t i;
  for (i = 0; i < prog->function_count; i++) {
    const MslFunction *fn = prog->functions[i];
    if (fn->is_kernel && strcmp(fn->name, name) == 0) {
      return fn;
    }
  }
  return NULL;
}

static const MslLocal *kernel_param(const MslFunction *kernel, int attr) {
  size_t i;
  for (i = 0; i < kernel->param_count; i++) {
    if (kernel->locals[i].attr == attr) {
      return &kernel->locals[i];
    }
  }
  return NULL;
}

static size_t group_static_bytes(const MslFunction *kernel) {
  size_t total = 0;
  size_t i;
  for (i = 0; i < kernel->local_count; i++) {
    if (kernel->locals[i].storage == MSL_VAR_GROUP) {
      total += kernel->locals[i].type->size;
    }
  }
  return total;
}

static int validate_dispatch(MslRun *run, const MslDispatch *d) {
  const MslFunction *kernel = run->kernel;
  const MslLocal *args = kernel_param(kernel, MSL_ATTR_BUFFER);
  uint64_t threads = (uint64_t)d->block[0] * d->block[1] * d->block[2];
  int i;
  for (i = 0; i < 3; i++) {
    if (d->grid[i] == 0 || d->block[i] == 0) {
      return msl_run_fail(run, NULL, 0, "grid and block dimensions must be nonzero");
    }
  }
  if (threads > 1024) {
    return msl_run_fail(run, NULL, 0, "%llu threads per threadgroup exceeds 1024", (unsigned long long)threads);
  }
  if (kernel->max_threads != 0 && threads > kernel->max_threads) {
    return msl_run_fail(run, NULL, 0, "%llu threads per threadgroup exceeds max_total_threads_per_threadgroup(%u)",
                        (unsigned long long)threads, kernel->max_threads);
  }
  if (run->simd_width == 0 || run->simd_width > 64) {
    return msl_run_fail(run, NULL, 0, "simd_width %u is out of range", run->simd_width);
  }
  if (args != NULL && (d->args == NULL || d->args_size != args->type->size)) {
    return msl_run_fail(run, NULL, 0, "argument struct '%s' is %u bytes but %u bytes were supplied", args->type->name,
                        (unsigned)args->type->size, d->args == NULL ? 0u : (unsigned)d->args_size);
  }
  if (kernel_param(kernel, MSL_ATTR_ARENA) != NULL && d->threadgroup_bytes % 16 != 0) {
    return msl_run_fail(run, NULL, 0, "threadgroup memory length %u is not a multiple of 16", (unsigned)d->threadgroup_bytes);
  }
  if (group_static_bytes(kernel) + d->threadgroup_bytes > 32768) {
    return msl_run_fail(run, NULL, 0, "threadgroup memory of %u bytes exceeds 32768",
                        (unsigned)(group_static_bytes(kernel) + d->threadgroup_bytes));
  }
  return 1;
}

static uint32_t heap_object(MslRun *run, int space, size_t size, size_t align, int initialized) {
  uint32_t index = msl_object_new(run, space, size, align, -1);
  uint8_t *block;
  if (index == 0) {
    return 0;
  }
  block = (uint8_t *)calloc(size * 2 + 1, 1);
  if (block == NULL) {
    msl_object_release(run, index);
    return 0;
  }
  run->objects[index].bytes = block;
  run->objects[index].init = initialized ? NULL : block + size;
  run->objects[index].heap = 1;
  return index;
}

static int setup_global_objects(MslRun *run, const MslDispatch *d) {
  size_t i;
  const MslLocal *args = kernel_param(run->kernel, MSL_ATTR_BUFFER);
  if (msl_object_new(run, MSL_SP_NONE, 0, 1, -1) != 0 || run->objects == NULL) {
    return msl_run_fail(run, NULL, 0, "out of memory");
  }
  run->objects[0].alive = 0;
  for (i = 0; i < run->prog->buffer_count; i++) {
    const MslBuffer *buffer = &run->prog->buffers[i];
    uint32_t index = msl_object_new(run, MSL_SP_DEVICE, buffer->size, 256, -1);
    if (index != i + 1) {
      return msl_run_fail(run, NULL, 0, "out of memory");
    }
    run->objects[index].bytes = buffer->host;
    run->objects[index].base = buffer->address;
  }
  if (args != NULL) {
    run->args_object = heap_object(run, MSL_SP_CONSTANT, args->type->size, 16, 1);
    if (run->args_object == 0) {
      return msl_run_fail(run, NULL, 0, "out of memory");
    }
    memcpy(run->objects[run->args_object].bytes, d->args, args->type->size);
    run->objects[run->args_object].readonly = 1;
  }
  return 1;
}

static int setup_group_objects(MslRun *run) {
  const MslFunction *kernel = run->kernel;
  size_t i;
  for (i = 0; i < kernel->local_count; i++) {
    const MslLocal *local = &kernel->locals[i];
    if (local->storage != MSL_VAR_GROUP) {
      continue;
    }
    run->group_objects[i] = heap_object(run, MSL_SP_THREADGROUP, local->type->size, local->align, 0);
    if (run->group_objects[i] == 0) {
      return msl_run_fail(run, NULL, 0, "out of memory");
    }
  }
  if (kernel_param(kernel, MSL_ATTR_ARENA) != NULL) {
    run->arena_object = heap_object(run, MSL_SP_THREADGROUP, run->dispatch->threadgroup_bytes, 16, 0);
    if (run->arena_object == 0) {
      return msl_run_fail(run, NULL, 0, "out of memory");
    }
  }
  return 1;
}

static void release_group_objects(MslRun *run) {
  size_t i;
  for (i = 0; i < run->kernel->local_count; i++) {
    if (run->group_objects[i] != 0) {
      msl_object_release(run, run->group_objects[i]);
      run->group_objects[i] = 0;
    }
  }
  if (run->arena_object != 0) {
    msl_object_release(run, run->arena_object);
    run->arena_object = 0;
  }
}

static void attr_value(const MslRun *run, const MslThread *t, int attr, uint64_t *slots) {
  const uint32_t *grid = run->dispatch->grid;
  int i;
  for (i = 0; i < 3; i++) {
    switch (attr) {
      case MSL_ATTR_TID: slots[i] = t->pos[i]; break;
      case MSL_ATTR_CTAID: slots[i] = run->group[i]; break;
      case MSL_ATTR_NTID: slots[i] = run->block[i]; break;
      case MSL_ATTR_NCTAID: slots[i] = grid[i]; break;
      case MSL_ATTR_GID: slots[i] = (uint64_t)(uint32_t)(run->group[i] * run->block[i] + t->pos[i]); break;
      case MSL_ATTR_NGRID: slots[i] = (uint64_t)(uint32_t)(grid[i] * run->block[i]); break;
      default: break;
    }
  }
  switch (attr) {
    case MSL_ATTR_LANE: slots[0] = t->lane; break;
    case MSL_ATTR_LANES: slots[0] = run->simd_width; break;
    case MSL_ATTR_SIMD_INDEX: slots[0] = t->simd; break;
    case MSL_ATTR_SIMD_COUNT: slots[0] = (run->thread_count + run->simd_width - 1) / run->simd_width; break;
    case MSL_ATTR_LINEAR: slots[0] = t->lid; break;
    default: break;
  }
}

static int start_thread(MslRun *run, MslThread *t) {
  const MslFunction *kernel = run->kernel;
  size_t i;
  if (!msl_thread_start(run, t, kernel)) {
    return 0;
  }
  for (i = 0; i < kernel->param_count; i++) {
    const MslLocal *local = &kernel->locals[i];
    uint64_t slots[4];
    if (local->attr == MSL_ATTR_BUFFER) {
      continue;
    }
    if (local->attr == MSL_ATTR_ARENA) {
      MslPtr ptr;
      ptr.space = MSL_SP_THREADGROUP;
      ptr.object = run->arena_object;
      ptr.gen = run->objects[run->arena_object].gen;
      ptr.offset = 0;
      msl_ptr_pack(ptr, slots);
    } else {
      attr_value(run, t, local->attr, slots);
    }
    msl_write_local(run, t, (int)i, slots);
  }
  return 1;
}

static int init_threads(MslRun *run) {
  uint32_t lid;
  for (lid = 0; lid < run->thread_count; lid++) {
    MslThread *t = &run->threads[lid];
    memset(t, 0, sizeof *t);
    t->lid = lid;
    t->pos[0] = lid % run->block[0];
    t->pos[1] = (lid / run->block[0]) % run->block[1];
    t->pos[2] = lid / (run->block[0] * run->block[1]);
    t->lane = lid % run->simd_width;
    t->simd = lid / run->simd_width;
    t->state = MSL_TS_READY;
    if (!start_thread(run, t)) {
      return 0;
    }
  }
  return 1;
}

static void free_threads(MslRun *run) {
  uint32_t lid;
  for (lid = 0; lid < run->thread_count; lid++) {
    msl_thread_unwind(run, &run->threads[lid]);
    free(run->threads[lid].stack);
    free(run->threads[lid].frames);
    memset(&run->threads[lid], 0, sizeof(MslThread));
  }
}

static int same_site(const MslThread *a, const MslThread *b) {
  size_t i;
  if (a->wait != b->wait || a->depth != b->depth) {
    return 0;
  }
  for (i = 0; i < a->depth; i++) {
    if (a->frames[i].fn != b->frames[i].fn || a->frames[i].pc != b->frames[i].pc) {
      return 0;
    }
  }
  return 1;
}

static int simd_group_ready(MslRun *run, uint32_t first, uint32_t end, MslThread **lanes, uint32_t *count) {
  uint32_t lid;
  *count = 0;
  for (lid = first; lid < end; lid++) {
    MslThread *t = &run->threads[lid];
    if (t->state == MSL_TS_DONE) {
      continue;
    }
    if (t->state != MSL_TS_SIMD || (*count > 0 && !same_site(t, lanes[0]))) {
      return 0;
    }
    lanes[(*count)++] = t;
  }
  return *count > 0;
}

static int resolve_simd_groups(MslRun *run, int *progress) {
  MslThread *lanes[64];
  uint32_t first;
  for (first = 0; first < run->thread_count; first += run->simd_width) {
    uint32_t end = first + run->simd_width < run->thread_count ? first + run->simd_width : run->thread_count;
    uint32_t count;
    if (simd_group_ready(run, first, end, lanes, &count)) {
      const MslInsn *site = lanes[0]->wait;
      uint32_t i;
      if (!msl_resolve_simd(run, lanes, count, site)) {
        return 0;
      }
      for (i = 0; i < count; i++) {
        lanes[i]->state = MSL_TS_READY;
        lanes[i]->wait = NULL;
      }
      *progress = 1;
    }
  }
  return 1;
}

static int report_simd_divergence(MslRun *run) {
  uint32_t first;
  for (first = 0; first < run->thread_count; first += run->simd_width) {
    uint32_t end = first + run->simd_width < run->thread_count ? first + run->simd_width : run->thread_count;
    const MslThread *a = NULL;
    uint32_t lid;
    for (lid = first; lid < end; lid++) {
      const MslThread *t = &run->threads[lid];
      if (t->state == MSL_TS_DONE) {
        continue;
      }
      if (a == NULL) {
        a = t;
      } else if (!same_site(t, a) || t->state != a->state) {
        return msl_run_fail(run, t, t->wait_line,
                            "SIMD-group lanes diverged: lane %u waits at the %s on line %d, lane %u at the %s on line %d%s", a->lane,
                            a->state == MSL_TS_BARRIER ? "barrier" : "SIMD-group function", a->wait_line, t->lane,
                            t->state == MSL_TS_BARRIER ? "barrier" : "SIMD-group function", t->wait_line,
                            a->wait == t->wait ? ", reached through different calls" : "");
      }
    }
  }
  return msl_run_fail(run, NULL, 0, "threads deadlocked");
}

static int resolve_barrier(MslRun *run) {
  const MslThread *barrier = NULL;
  const MslThread *simd = NULL;
  uint32_t lid;
  for (lid = 0; lid < run->thread_count; lid++) {
    const MslThread *t = &run->threads[lid];
    if (t->state == MSL_TS_BARRIER) {
      if (barrier == NULL) {
        barrier = t;
      } else if (!same_site(t, barrier)) {
        return msl_run_fail(run, t, t->wait_line, "threads wait at different barriers (line %d and line %d)", barrier->wait_line, t->wait_line);
      }
    } else if (t->state == MSL_TS_SIMD && simd == NULL) {
      simd = t;
    }
  }
  if (simd != NULL && barrier != NULL) {
    return msl_run_fail(run, simd, simd->wait_line,
                        "barrier on line %d is not reached by every live thread: this thread waits at a SIMD-group function on line %d",
                        barrier->wait_line, simd->wait_line);
  }
  if (simd != NULL) {
    return report_simd_divergence(run);
  }
  for (lid = 0; lid < run->thread_count; lid++) {
    if (run->threads[lid].state == MSL_TS_BARRIER) {
      run->threads[lid].state = MSL_TS_READY;
      run->threads[lid].wait = NULL;
    }
  }
  return 1;
}

static int run_threads(MslRun *run) {
  for (;;) {
    uint32_t lid;
    int live = 0;
    int progress = 0;
    for (lid = 0; lid < run->thread_count; lid++) {
      MslThread *t = &run->threads[lid];
      if (t->state == MSL_TS_READY && !msl_exec_thread(run, t)) {
        return 0;
      }
    }
    for (lid = 0; lid < run->thread_count; lid++) {
      live += run->threads[lid].state != MSL_TS_DONE;
    }
    if (live == 0) {
      return 1;
    }
    if (!resolve_simd_groups(run, &progress)) {
      return 0;
    }
    if (!progress && !resolve_barrier(run)) {
      return 0;
    }
  }
}

static int run_group(MslRun *run) {
  int ok = setup_group_objects(run) && init_threads(run) && run_threads(run);
  free_threads(run);
  release_group_objects(run);
  return ok;
}

static int run_grid(MslRun *run) {
  const uint32_t *grid = run->dispatch->grid;
  uint32_t x;
  uint32_t y;
  uint32_t z;
  for (z = 0; z < grid[2]; z++) {
    for (y = 0; y < grid[1]; y++) {
      for (x = 0; x < grid[0]; x++) {
        run->group[0] = x;
        run->group[1] = y;
        run->group[2] = z;
        if (!run_group(run)) {
          return 0;
        }
      }
    }
  }
  return 1;
}

static void free_run(MslRun *run) {
  size_t i;
  for (i = 1; i < run->object_count; i++) {
    if (run->objects[i].alive && run->objects[i].heap) {
      free(run->objects[i].bytes);
    }
  }
  free(run->objects);
  free(run->free_objects);
  free(run->threads);
  free(run->group_objects);
}

int msl_dispatch(MslProgram *program, const MslDispatch *dispatch, char *error, size_t error_size) {
  MslRun run;
  int ok;
  if (error != NULL && error_size > 0) {
    error[0] = 0;
  }
  if (program == NULL || dispatch == NULL || dispatch->kernel == NULL) {
    msl_format_error(error, error_size, "msl_dispatch needs a program, a dispatch and a kernel name");
    return 0;
  }
  memset(&run, 0, sizeof run);
  run.prog = program;
  run.dispatch = dispatch;
  run.error = error;
  run.error_size = error_size;
  run.kernel = find_kernel(program, dispatch->kernel);
  if (run.kernel == NULL) {
    msl_format_error(error, error_size, "no kernel named '%s'", dispatch->kernel);
    return 0;
  }
  run.simd_width = dispatch->simd_width == 0 ? 32 : dispatch->simd_width;
  run.block[0] = dispatch->block[0];
  run.block[1] = dispatch->block[1];
  run.block[2] = dispatch->block[2];
  run.thread_count = dispatch->block[0] * dispatch->block[1] * dispatch->block[2];
  run.budget = msl_step_budget;
  run.cas_state = 0x853c49e6748fea9bull;
  ok = validate_dispatch(&run, dispatch);
  if (ok) {
    run.threads = (MslThread *)calloc(run.thread_count, sizeof(MslThread));
    run.group_objects = (uint32_t *)calloc(run.kernel->local_count + 1, sizeof(uint32_t));
    if (run.threads == NULL || run.group_objects == NULL) {
      ok = msl_run_fail(&run, NULL, 0, "out of memory");
    }
  }
  ok = ok && setup_global_objects(&run, dispatch) && run_grid(&run);
  free_run(&run);
  return ok;
}
