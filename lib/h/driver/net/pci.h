#ifndef IPO_DRIVER_NET_PCI_H
#define IPO_DRIVER_NET_PCI_H

#include <stdint.h>
#include <stdbool.h>

#define PCI_CONFIG_ADDRESS  0x0CF8
#define PCI_CONFIG_DATA     0x0CFC

typedef struct {
    uint8_t  bus;
    uint8_t  slot;
    uint8_t  func;
    uint16_t vendor_id;
    uint16_t device_id;
    uint32_t bar0;
    uint8_t  irq;
} pci_device_t;

uint32_t pci_read_dword(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset);
uint16_t pci_read_word(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset);
void     pci_write_dword(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset, uint32_t val);
void     pci_write_word(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset, uint16_t val);

bool pci_find_device(uint16_t vendor_id, uint16_t device_id, pci_device_t *out_dev);
void pci_enable_bus_mastering(const pci_device_t *dev);

#endif /* IPO_DRIVER_NET_PCI_H */
