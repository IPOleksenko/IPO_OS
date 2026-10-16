#include <net/loopback.h>
#include <net/net_state.h>
#include <net/ipv4.h>
#include <memory/kmalloc.h>
#include <string.h>

void loopback_init(void) {
    net_shared_ctx_t *ctx = net_get_shared_context();
    if (ctx->loopback_queue_capacity > 0) {
        memset(ctx->loop_slots, 0, ctx->loopback_queue_capacity * sizeof(*ctx->loop_slots));
    }
    ctx->loop_head_idx = 0;
    ctx->loop_tail_idx = 0;
}

int loopback_send(const void *packet, uint16_t len) {
    if (!packet || len == 0 || len > sizeof(((loopback_slot_t *)0)->data)) {
        return -1;
    }

    net_shared_ctx_t *ctx = net_get_shared_context();
    if (ctx->loopback_queue_capacity == 0) {
        return -1;
    }

    if (ctx->loop_slots[ctx->loop_tail_idx].valid) {
        /* Queue full -> dynamically double capacity and linearize existing slots */
        size_t old_cap = ctx->loopback_queue_capacity;
        size_t new_cap = (old_cap == 0) ? 8 : (old_cap * 2);
        if (new_cap > old_cap && new_cap <= (SIZE_MAX / sizeof(loopback_slot_t))) {
            loopback_slot_t *new_slots = (loopback_slot_t *)kmalloc(new_cap * sizeof(loopback_slot_t));
            if (new_slots) {
                for (size_t i = 0; i < old_cap; i++) {
                    new_slots[i] = ctx->loop_slots[(ctx->loop_head_idx + i) % old_cap];
                }
                memset(new_slots + old_cap, 0, (new_cap - old_cap) * sizeof(loopback_slot_t));
                kfree(ctx->loop_slots);
                ctx->loop_slots = new_slots;
                ctx->loop_head_idx = 0;
                ctx->loop_tail_idx = old_cap;
                ctx->loopback_queue_capacity = new_cap;
            }
        }
        if (ctx->loop_slots[ctx->loop_tail_idx].valid) {
            /* Fallback only if memory allocation failed */
            return -1;
        }
    }

    size_t next_tail = (ctx->loop_tail_idx + 1) % ctx->loopback_queue_capacity;
    size_t idx = ctx->loop_tail_idx;
    memcpy(ctx->loop_slots[idx].data, packet, len);
    ctx->loop_slots[idx].len = len;
    ctx->loop_slots[idx].valid = true;
    ctx->loop_tail_idx = next_tail;

    return (int)len;
}

void loopback_poll(void) {
    net_shared_ctx_t *ctx = net_get_shared_context();
    if (ctx->loopback_queue_capacity == 0) {
        return;
    }

    while (ctx->loop_slots[ctx->loop_head_idx].valid) {
        size_t idx = ctx->loop_head_idx;
        uint16_t len = ctx->loop_slots[idx].len;
        if (len > 0) {
            ip4_receive(ctx->loop_slots[idx].data, len);
        }
        ctx->loop_slots[idx].valid = false;
        ctx->loop_head_idx = (ctx->loop_head_idx + 1) % ctx->loopback_queue_capacity;
    }
}

bool loopback_get_info(uint16_t *mtu, size_t *queue_capacity) {
    net_shared_ctx_t *ctx = net_get_shared_context();
    if (!ctx || ctx->magic != NET_SHARED_MAGIC || ctx->loopback_queue_capacity == 0) {
        return false;
    }
    if (mtu) *mtu = (uint16_t)sizeof(((loopback_slot_t *)0)->data);
    if (queue_capacity) *queue_capacity = ctx->loopback_queue_capacity;
    return true;
}
