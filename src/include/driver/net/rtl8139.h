#ifndef IPO_DRIVER_NET_RTL8139_H
#define IPO_DRIVER_NET_RTL8139_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#define RTL8139_RX_BUFFER_SIZE_DEFAULT (8192 + 16 + 1500) // 8K + 16 bytes header + wrap margin
#define RTL8139_TX_BUFFER_SIZE_DEFAULT 1536

size_t rtl8139_get_rx_buffer_size(void);
size_t rtl8139_get_tx_buffer_size(void);
bool   rtl8139_set_rx_buffer_size(size_t new_size);
bool   rtl8139_set_tx_buffer_size(size_t new_size);

#ifndef RTL8139_RX_BUFFER_SIZE
#define RTL8139_RX_BUFFER_SIZE (rtl8139_get_rx_buffer_size())
#endif

#ifndef RTL8139_TX_BUFFER_SIZE
#define RTL8139_TX_BUFFER_SIZE (rtl8139_get_tx_buffer_size())
#endif

typedef struct {
    bool     initialized;
    uint16_t vendor_id;
    uint16_t device_id;
    uint16_t subsys_vendor_id;
    uint16_t subsys_id;
    uint8_t  revision_id;
    uint8_t  bus;
    uint8_t  slot;
    uint8_t  func;
    uint16_t io_base;
    uint8_t  *mac;
    size_t   mac_len;
    size_t   mac_cap;
    uint8_t  tx_cur;
    uint16_t rx_offset;
} rtl8139_dev_t;

bool rtl8139_init(void);
bool rtl8139_is_present(void);
bool rtl8139_is_link_up(void);
const uint8_t *rtl8139_get_mac(size_t *out_len);
size_t rtl8139_get_mac_len(void);

uint16_t rtl8139_get_vendor_id(void);
uint16_t rtl8139_get_device_id(void);
uint16_t rtl8139_get_subsys_vendor_id(void);
uint16_t rtl8139_get_subsys_id(void);
uint8_t  rtl8139_get_revision_id(void);
uint8_t  rtl8139_get_bus(void);
uint8_t  rtl8139_get_slot(void);
uint8_t  rtl8139_get_func(void);

int rtl8139_send_packet(const void *data, uint16_t len);
int rtl8139_receive_packet(void *buf, uint32_t max_len);
int rtl8139_peek_packet_size(void);

#endif /* IPO_DRIVER_NET_RTL8139_H */
