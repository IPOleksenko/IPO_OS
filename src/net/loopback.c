#include <net/loopback.h>
#include <net/net_state.h>
#include <net/ipv4.h>
#include <string.h>

void loopback_init(void) {
    net_shared_ctx_t *ctx = net_get_shared_context();
    memset(ctx->loop_slots, 0, sizeof(ctx->loop_slots));
    ctx->loop_head_idx = 0;
    ctx->loop_tail_idx = 0;
}

int loopback_send(const void *packet, uint16_t len) {
    if (!packet || len == 0 || len > 1536) {
        return -1;
    }

    net_shared_ctx_t *ctx = net_get_shared_context();
    uint8_t next_tail = (uint8_t)((ctx->loop_tail_idx + 1) % LOOPBACK_QUEUE_SIZE);
    if (ctx->loop_slots[ctx->loop_tail_idx].valid && next_tail == ctx->loop_head_idx) {
        /* Queue full */
        return -1;
    }

    uint8_t idx = ctx->loop_tail_idx;
    memcpy(ctx->loop_slots[idx].data, packet, len);
    ctx->loop_slots[idx].len = len;
    ctx->loop_slots[idx].valid = true;
    ctx->loop_tail_idx = next_tail;

    return (int)len;
}

void loopback_poll(void) {
    net_shared_ctx_t *ctx = net_get_shared_context();

    while (ctx->loop_slots[ctx->loop_head_idx].valid) {
        uint8_t idx = ctx->loop_head_idx;
        uint16_t len = ctx->loop_slots[idx].len;
        if (len > 0) {
            ip4_receive(ctx->loop_slots[idx].data, len);
        }
        ctx->loop_slots[idx].valid = false;
        ctx->loop_head_idx = (uint8_t)((ctx->loop_head_idx + 1) % LOOPBACK_QUEUE_SIZE);
    }
}
