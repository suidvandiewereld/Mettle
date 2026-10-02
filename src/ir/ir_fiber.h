#ifndef METTLE_IR_FIBER_H
#define METTLE_IR_FIBER_H

#include <stddef.h>

typedef struct IRFiberScheduler IRFiberScheduler;
typedef struct IRFiber IRFiber;

IRFiberScheduler *ir_fiber_scheduler_begin(void);
void ir_fiber_scheduler_end(IRFiberScheduler *scheduler);
IRFiber *ir_fiber_create(IRFiberScheduler *scheduler, size_t stack_bytes,
                         void (*entry)(void *), void *argument);
void ir_fiber_run(IRFiber *fiber);
void ir_fiber_yield(IRFiber *fiber);
void ir_fiber_destroy(IRFiber *fiber);

#endif
