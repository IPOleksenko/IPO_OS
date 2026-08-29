#ifndef KERNEL_ASYNC_H
#define KERNEL_ASYNC_H

#include <stdint.h>
#include <stdbool.h>

typedef void (*async_task_fn_t)(void);

void async_scheduler_init(void);

int async_start_task(
    const char *name,
    uint32_t interval_ms,
    async_task_fn_t fn
);

int async_stop_task(const char *name);

void async_scheduler_tick(void);

#endif /* KERNEL_ASYNC_H */