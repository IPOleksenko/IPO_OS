#ifndef IPO_NET_LOOPBACK_H
#define IPO_NET_LOOPBACK_H

#include <net/net.h>

void loopback_init(void);
int  loopback_send(const void *packet, uint16_t len);
void loopback_poll(void);

#endif /* IPO_NET_LOOPBACK_H */
