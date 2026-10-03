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
#include <string.h>

struct IRFiberScheduler {
  void *stack_pointer;
};

struct IRFiber {
  void *stack_pointer;
  IRFiberScheduler *scheduler;
  void *stack;
  void (*entry)(void *);
  void *argument;
};

void ir_fiber_switch(void **save, void *load);
void ir_fiber_trampoline(void);

#if defined(__x86_64__)

__asm__(".pushsection .text\n"
        ".p2align 4\n"
        ".globl ir_fiber_switch\n"
        ".type ir_fiber_switch,@function\n"
        "ir_fiber_switch:\n"
        "pushq %rbp\n"
        "pushq %rbx\n"
        "pushq %r12\n"
        "pushq %r13\n"
        "pushq %r14\n"
        "pushq %r15\n"
        "subq $8, %rsp\n"
        "stmxcsr (%rsp)\n"
        "fnstcw 4(%rsp)\n"
        "movq %rsp, (%rdi)\n"
        "movq %rsi, %rsp\n"
        "ldmxcsr (%rsp)\n"
        "fldcw 4(%rsp)\n"
        "addq $8, %rsp\n"
        "popq %r15\n"
        "popq %r14\n"
        "popq %r13\n"
        "popq %r12\n"
        "popq %rbx\n"
        "popq %rbp\n"
        "ret\n"
        ".size ir_fiber_switch, .-ir_fiber_switch\n"
        ".p2align 4\n"
        ".globl ir_fiber_trampoline\n"
        ".type ir_fiber_trampoline,@function\n"
        "ir_fiber_trampoline:\n"
        "movq %r12, %rdi\n"
        "callq *%r13\n"
        "ud2\n"
        ".size ir_fiber_trampoline, .-ir_fiber_trampoline\n"
        ".popsection\n");

enum { IR_FIBER_FRAME_WORDS = 8 };

static void ir_fiber_frame(uint64_t *frame, IRFiber *fiber,
                           void (*start)(IRFiber *)) {
  uint32_t mxcsr = 0;
  uint16_t control = 0;
  __asm__ volatile("stmxcsr %0" : "=m"(mxcsr));
  __asm__ volatile("fnstcw %0" : "=m"(control));
  frame[0] = (uint64_t)mxcsr | ((uint64_t)control << 32);
  frame[3] = (uint64_t)(uintptr_t)start;
  frame[4] = (uint64_t)(uintptr_t)fiber;
  frame[7] = (uint64_t)(uintptr_t)ir_fiber_trampoline;
}

#elif defined(__aarch64__)

__asm__(".pushsection .text\n"
        ".p2align 4\n"
        ".globl ir_fiber_switch\n"
        ".type ir_fiber_switch,%function\n"
        "ir_fiber_switch:\n"
        "sub sp, sp, #176\n"
        "stp x19, x20, [sp, #0]\n"
        "stp x21, x22, [sp, #16]\n"
        "stp x23, x24, [sp, #32]\n"
        "stp x25, x26, [sp, #48]\n"
        "stp x27, x28, [sp, #64]\n"
        "stp x29, x30, [sp, #80]\n"
        "stp d8, d9, [sp, #96]\n"
        "stp d10, d11, [sp, #112]\n"
        "stp d12, d13, [sp, #128]\n"
        "stp d14, d15, [sp, #144]\n"
        "mrs x9, fpcr\n"
        "str x9, [sp, #160]\n"
        "mov x9, sp\n"
        "str x9, [x0]\n"
        "mov sp, x1\n"
        "ldr x9, [sp, #160]\n"
        "mrs x10, fpcr\n"
        "cmp x9, x10\n"
        "b.eq 1f\n"
        "msr fpcr, x9\n"
        "1:\n"
        "ldp d14, d15, [sp, #144]\n"
        "ldp d12, d13, [sp, #128]\n"
        "ldp d10, d11, [sp, #112]\n"
        "ldp d8, d9, [sp, #96]\n"
        "ldp x29, x30, [sp, #80]\n"
        "ldp x27, x28, [sp, #64]\n"
        "ldp x25, x26, [sp, #48]\n"
        "ldp x23, x24, [sp, #32]\n"
        "ldp x21, x22, [sp, #16]\n"
        "ldp x19, x20, [sp, #0]\n"
        "add sp, sp, #176\n"
        "ret\n"
        ".size ir_fiber_switch, .-ir_fiber_switch\n"
        ".p2align 4\n"
        ".globl ir_fiber_trampoline\n"
        ".type ir_fiber_trampoline,%function\n"
        "ir_fiber_trampoline:\n"
        "mov x0, x19\n"
        "blr x20\n"
        "brk #0\n"
        ".size ir_fiber_trampoline, .-ir_fiber_trampoline\n"
        ".popsection\n");

enum { IR_FIBER_FRAME_WORDS = 22 };

static void ir_fiber_frame(uint64_t *frame, IRFiber *fiber,
                           void (*start)(IRFiber *)) {
  uint64_t control = 0;
  __asm__ volatile("mrs %0, fpcr" : "=r"(control));
  frame[0] = (uint64_t)(uintptr_t)fiber;
  frame[1] = (uint64_t)(uintptr_t)start;
  frame[11] = (uint64_t)(uintptr_t)ir_fiber_trampoline;
  frame[20] = control;
}

#else
#error The CPU grid runner has no fiber switch for this target
#endif

static void ir_fiber_start(IRFiber *fiber) {
  fiber->entry(fiber->argument);
  for (;;) {
    ir_fiber_yield(fiber);
  }
}

IRFiberScheduler *ir_fiber_scheduler_begin(void) {
  return (IRFiberScheduler *)calloc(1, sizeof(IRFiberScheduler));
}

void ir_fiber_scheduler_end(IRFiberScheduler *scheduler) { free(scheduler); }

IRFiber *ir_fiber_create(IRFiberScheduler *scheduler, size_t stack_bytes,
                         void (*entry)(void *), void *argument) {
  IRFiber *fiber = (IRFiber *)calloc(1, sizeof(IRFiber));
  uint64_t *frame;
  uintptr_t top;
  if (!fiber) {
    return NULL;
  }
  fiber->stack = malloc(stack_bytes);
  if (!fiber->stack) {
    free(fiber);
    return NULL;
  }
  fiber->scheduler = scheduler;
  fiber->entry = entry;
  fiber->argument = argument;
  top = ((uintptr_t)fiber->stack + stack_bytes) & ~(uintptr_t)15;
  frame = (uint64_t *)top - IR_FIBER_FRAME_WORDS;
  memset(frame, 0, IR_FIBER_FRAME_WORDS * sizeof(uint64_t));
  ir_fiber_frame(frame, fiber, ir_fiber_start);
  fiber->stack_pointer = frame;
  return fiber;
}

void ir_fiber_run(IRFiber *fiber) {
  ir_fiber_switch(&fiber->scheduler->stack_pointer, fiber->stack_pointer);
}

void ir_fiber_yield(IRFiber *fiber) {
  ir_fiber_switch(&fiber->stack_pointer, fiber->scheduler->stack_pointer);
}

void ir_fiber_destroy(IRFiber *fiber) {
  if (!fiber) {
    return;
  }
  free(fiber->stack);
  free(fiber);
}

#endif
