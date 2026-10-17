#ifndef IPO_NET_LOOPBACK_H
#define IPO_NET_LOOPBACK_H

#include <net/net.h>

void loopback_init(void);
int  loopback_send(const void *packet, uint16_t len);
void loopback_poll(void);
/* Query live loopback state: max packet size (MTU) and current queue capacity */
bool loopback_get_info(uint16_t *mtu, size_t *queue_capacity);

#endif /* IPO_NET_LOOPBACK_H */
