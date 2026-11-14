#ifndef KERNEL_IRQ_H
#define KERNEL_IRQ_H

#include <stdint.h>
#include <stdbool.h>

#define IRQ_MIN_LINE 1u
#define IRQ_MAX_LINE 15u

typedef void (*irq_handler_fn_t)(uint8_t irq, void *ctx);

void irq_init(void);
void irq_common_dispatch(uint32_t irq);

bool irq_enable(uint8_t irq);
bool irq_disable(uint8_t irq);

uint32_t irq_register_handler(uint8_t irq, irq_handler_fn_t fn, void *ctx, uint32_t owner_pid);
int irq_unregister_handler(uint32_t handle, uint32_t caller_pid);

uint32_t irq_subscribe(uint8_t irq, uint32_t owner_pid);
int irq_unsubscribe(uint32_t handle, uint32_t caller_pid);
uint32_t irq_poll(uint32_t handle, uint32_t caller_pid, uint32_t *out_total, uint64_t *out_last_ms);

uint32_t irq_get_count(uint8_t irq);
void irq_cleanup_pid(uint32_t pid);

#endif /* KERNEL_IRQ_H */
