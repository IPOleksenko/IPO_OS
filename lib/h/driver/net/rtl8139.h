#ifndef IPO_DRIVER_NET_RTL8139_H
#define IPO_DRIVER_NET_RTL8139_H

#include <stdint.h>
#include <stdbool.h>

#define RTL8139_VENDOR_ID 0x10EC
#define RTL8139_DEVICE_ID 0x8139

#define RTL8139_RX_BUFFER_SIZE (8192 + 16 + 1500) // 8K + 16 bytes header + wrap margin
#define RTL8139_TX_BUFFER_SIZE 1536

typedef struct {
    bool     initialized;
    uint16_t io_base;
    uint8_t  mac[6];
    uint8_t  tx_cur;
    uint16_t rx_offset;
} rtl8139_dev_t;

bool rtl8139_init(void);
bool rtl8139_is_present(void);
const uint8_t *rtl8139_get_mac(void);

int rtl8139_send_packet(const void *data, uint16_t len);
int rtl8139_receive_packet(void *buf, uint16_t max_len);

#endif /* IPO_DRIVER_NET_RTL8139_H */
