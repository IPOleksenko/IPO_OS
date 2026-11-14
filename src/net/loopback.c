#include <net/loopback.h>
#include <net/net_state.h>
#include <net/ipv4.h>
#include <driver/net/rtl8139.h>
#include <memory/kmalloc.h>
#include <string.h>

void loopback_init(void) {
    net_shared_ctx_t *ctx = net_get_shared_context();
    if (ctx->loopback_queue_capacity > 0 && ctx->loop_slots) {
        for (size_t i = 0; i < ctx->loopback_queue_capacity; i++) {
            if (ctx->loop_slots[i].data) {
                kfree(ctx->loop_slots[i].data);
            }
        }
        memset(ctx->loop_slots, 0, ctx->loopback_queue_capacity * sizeof(*ctx->loop_slots));
    }
    ctx->loop_head_idx = 0;
    ctx->loop_tail_idx = 0;
}

int loopback_send(const void *packet, uint16_t len) {
    if (!packet || len == 0) {
        return -1;
    }

    net_shared_ctx_t *ctx = net_get_shared_context();
    if (ctx->loopback_queue_capacity == 0 || !ctx->loop_slots) {
        size_t init_cap = rtl8139_get_hw_socket_capacity();
        if (init_cap == 0) init_cap = 1;
        loopback_slot_t *init_slots = (loopback_slot_t *)kmalloc(init_cap * sizeof(loopback_slot_t));
        if (!init_slots) return -1;
        memset(init_slots, 0, init_cap * sizeof(loopback_slot_t));
        ctx->loop_slots = init_slots;
        ctx->loopback_queue_capacity = init_cap;
        ctx->loop_head_idx = 0;
        ctx->loop_tail_idx = 0;
    }

    if (ctx->loop_slots[ctx->loop_tail_idx].valid) {
        /* Queue full -> double capacity and linearize existing slots */
        size_t old_cap = ctx->loopback_queue_capacity;
        size_t new_cap = (old_cap == 0) ? rtl8139_get_hw_socket_capacity() : (old_cap * 2);
        if (new_cap == 0) new_cap = 1;
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
            return -1;
        }
    }

    size_t next_tail = (ctx->loop_tail_idx + 1) % ctx->loopback_queue_capacity;
    size_t idx = ctx->loop_tail_idx;

    if (len > ctx->loop_slots[idx].capacity || !ctx->loop_slots[idx].data) {
        uint8_t *new_buf = (uint8_t *)krealloc(ctx->loop_slots[idx].data, len);
        if (!new_buf) return -1;
        ctx->loop_slots[idx].data = new_buf;
        ctx->loop_slots[idx].capacity = len;
    }

    memcpy(ctx->loop_slots[idx].data, packet, len);
    ctx->loop_slots[idx].len = len;
    ctx->loop_slots[idx].valid = true;
    ctx->loop_tail_idx = next_tail;

    return (int)len;
}

void loopback_poll(void) {
    net_shared_ctx_t *ctx = net_get_shared_context();
    if (ctx->loopback_queue_capacity == 0 || !ctx->loop_slots) {
        return;
    }

    /* Detach queued buffer before dispatching so nested loopback_send/reallocation is safe */
    while (ctx->loop_slots[ctx->loop_head_idx].valid) {
        size_t idx = ctx->loop_head_idx;
        uint16_t len = ctx->loop_slots[idx].len;
        uint8_t *pkt = ctx->loop_slots[idx].data;

        ctx->loop_slots[idx].data = NULL;
        ctx->loop_slots[idx].capacity = 0;
        ctx->loop_slots[idx].len = 0;
        ctx->loop_slots[idx].valid = false;
        ctx->loop_head_idx = (ctx->loop_head_idx + 1) % ctx->loopback_queue_capacity;

        if (len > 0 && pkt) {
            ip4_receive(pkt, len);
        }
        if (pkt) {
            kfree(pkt);
        }
    }
}

bool loopback_get_info(uint16_t *mtu, size_t *queue_capacity) {
    net_shared_ctx_t *ctx = net_get_shared_context();
    if (!ctx || ctx->magic != NET_SHARED_MAGIC || ctx->loopback_queue_capacity == 0) {
        return false;
    }
    if (mtu) *mtu = (uint16_t)rtl8139_get_hw_max_window();
    if (queue_capacity) *queue_capacity = ctx->loopback_queue_capacity;
    return true;
}
