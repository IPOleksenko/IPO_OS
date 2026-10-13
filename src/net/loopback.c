#include <net/loopback.h>
#include <net/net_state.h>
#include <net/ipv4.h>
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
    if (!packet || len == 0 || len > 1536) {
        return -1;
    }

    net_shared_ctx_t *ctx = net_get_shared_context();
    if (ctx->loopback_queue_capacity == 0) {
        return -1;
    }
    size_t next_tail = (ctx->loop_tail_idx + 1) % ctx->loopback_queue_capacity;
    if (ctx->loop_slots[ctx->loop_tail_idx].valid) {
        /* Queue full */
        return -1;
    }

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
