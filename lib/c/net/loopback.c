#include <net/loopback.h>
#include <net/net_state.h>
#include <net/ipv4.h>
#include <memory/kmalloc.h>
#include <string.h>

typedef struct loopback_packet {
    uint8_t *data;
    uint16_t len;
    struct loopback_packet *next;
} loopback_packet_t;

static inline loopback_packet_t **get_loop_head(void) {
    net_shared_ctx_t *ctx = net_get_shared_context();
    return (loopback_packet_t **)&ctx->loop_head;
}

static inline loopback_packet_t **get_loop_tail(void) {
    net_shared_ctx_t *ctx = net_get_shared_context();
    return (loopback_packet_t **)&ctx->loop_tail;
}

void loopback_init(void) {
    loopback_packet_t **head_ptr = get_loop_head();
    loopback_packet_t **tail_ptr = get_loop_tail();
    loopback_packet_t *cur = *head_ptr;
    while (cur) {
        loopback_packet_t *next = cur->next;
        if (cur->data) {
            kfree(cur->data);
        }
        kfree(cur);
        cur = next;
    }
    *head_ptr = NULL;
    *tail_ptr = NULL;
}

int loopback_send(const void *packet, uint16_t len) {
    if (!packet || len == 0) {
        return -1;
    }

    uint8_t *data_copy = (uint8_t *)kmalloc(len);
    if (!data_copy) {
        return -1;
    }
    memcpy(data_copy, packet, len);

    loopback_packet_t *pkt = (loopback_packet_t *)kmalloc(sizeof(loopback_packet_t));
    if (!pkt) {
        kfree(data_copy);
        return -1;
    }

    pkt->data = data_copy;
    pkt->len = len;
    pkt->next = NULL;

    loopback_packet_t **head_ptr = get_loop_head();
    loopback_packet_t **tail_ptr = get_loop_tail();

    if (*tail_ptr) {
        (*tail_ptr)->next = pkt;
        *tail_ptr = pkt;
    } else {
        *head_ptr = pkt;
        *tail_ptr = pkt;
    }

    return (int)len;
}

void loopback_poll(void) {
    loopback_packet_t **head_ptr = get_loop_head();
    loopback_packet_t **tail_ptr = get_loop_tail();

    while (*head_ptr) {
        loopback_packet_t *pkt = *head_ptr;
        *head_ptr = pkt->next;
        if (!*head_ptr) {
            *tail_ptr = NULL;
        }

        if (pkt->data && pkt->len > 0) {
            ip4_receive(pkt->data, pkt->len);
        }

        if (pkt->data) {
            kfree(pkt->data);
        }
        kfree(pkt);
    }
}
