#include "ir_fiber.h"

#include <stdint.h>
#include <stdlib.h>

#if defined(_WIN32)
#include <windows.h>

struct IRFiberScheduler {
  void *handle;
  int converted;
};

struct IRFiber {
  void *handle;
  IRFiberScheduler *scheduler;
  void (*entry)(void *);
  void *argument;
};

static VOID CALLBACK ir_fiber_start(LPVOID parameter) {
  IRFiber *fiber = (IRFiber *)parameter;
  fiber->entry(fiber->argument);
}

IRFiberScheduler *ir_fiber_scheduler_begin(void) {
  IRFiberScheduler *scheduler =
      (IRFiberScheduler *)calloc(1, sizeof(IRFiberScheduler));
  if (!scheduler) {
    return NULL;
  }
  if (IsThreadAFiber()) {
    scheduler->handle = GetCurrentFiber();
  } else {
    scheduler->handle = ConvertThreadToFiber(NULL);
    scheduler->converted = scheduler->handle != NULL;
  }
  if (!scheduler->handle) {
    free(scheduler);
    return NULL;
  }
  return scheduler;
}

void ir_fiber_scheduler_end(IRFiberScheduler *scheduler) {
  if (!scheduler) {
    return;
  }
  if (scheduler->converted) {
    ConvertFiberToThread();
  }
  free(scheduler);
}

IRFiber *ir_fiber_create(IRFiberScheduler *scheduler, size_t stack_bytes,
                         void (*entry)(void *), void *argument) {
  IRFiber *fiber = (IRFiber *)calloc(1, sizeof(IRFiber));
  if (!fiber) {
    return NULL;
  }
  fiber->scheduler = scheduler;
  fiber->entry = entry;
  fiber->argument = argument;
  fiber->handle = CreateFiberEx(64 * 1024, stack_bytes, FIBER_FLAG_FLOAT_SWITCH,
                                ir_fiber_start, fiber);
  if (!fiber->handle) {
    free(fiber);
    return NULL;
  }
  return fiber;
}

void ir_fiber_run(IRFiber *fiber) { SwitchToFiber(fiber->handle); }

void ir_fiber_yield(IRFiber *fiber) {
  SwitchToFiber(fiber->scheduler->handle);
}

void ir_fiber_destroy(IRFiber *fiber) {
  if (!fiber) {
    return;
  }
  DeleteFiber(fiber->handle);
  free(fiber);
}

#else
#include <ucontext.h>

struct IRFiberScheduler {
  ucontext_t context;
};

struct IRFiber {
  ucontext_t context;
  IRFiberScheduler *scheduler;
  void *stack;
  void (*entry)(void *);
  void *argument;
};

static void ir_fiber_start(unsigned high, unsigned low) {
  IRFiber *fiber =
      (IRFiber *)(uintptr_t)(((uint64_t)high << 32) | (uint64_t)low);
  fiber->entry(fiber->argument);
}

IRFiberScheduler *ir_fiber_scheduler_begin(void) {
  return (IRFiberScheduler *)calloc(1, sizeof(IRFiberScheduler));
}

void ir_fiber_scheduler_end(IRFiberScheduler *scheduler) { free(scheduler); }

IRFiber *ir_fiber_create(IRFiberScheduler *scheduler, size_t stack_bytes,
                         void (*entry)(void *), void *argument) {
  IRFiber *fiber = (IRFiber *)calloc(1, sizeof(IRFiber));
  uint64_t address;
  if (!fiber) {
    return NULL;
  }
  fiber->stack = malloc(stack_bytes);
  if (!fiber->stack || getcontext(&fiber->context) != 0) {
    free(fiber->stack);
    free(fiber);
    return NULL;
  }
  fiber->scheduler = scheduler;
  fiber->entry = entry;
  fiber->argument = argument;
  fiber->context.uc_stack.ss_sp = fiber->stack;
  fiber->context.uc_stack.ss_size = stack_bytes;
  fiber->context.uc_link = &scheduler->context;
  address = (uint64_t)(uintptr_t)fiber;
  makecontext(&fiber->context, (void (*)(void))ir_fiber_start, 2,
              (unsigned)(address >> 32), (unsigned)(address & 0xFFFFFFFFu));
  return fiber;
}

void ir_fiber_run(IRFiber *fiber) {
  swapcontext(&fiber->scheduler->context, &fiber->context);
}

void ir_fiber_yield(IRFiber *fiber) {
  swapcontext(&fiber->context, &fiber->scheduler->context);
}

void ir_fiber_destroy(IRFiber *fiber) {
  if (!fiber) {
    return;
  }
  free(fiber->stack);
  free(fiber);
}

#endif
