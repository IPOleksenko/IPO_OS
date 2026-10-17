#ifndef IPO_DRIVER_NET_PCI_H
#define IPO_DRIVER_NET_PCI_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#define PCI_CONFIG_ADDRESS  0x0CF8
#define PCI_CONFIG_DATA     0x0CFC

typedef struct {
    uint8_t  bus;
    uint8_t  slot;
    uint8_t  func;
    uint16_t vendor_id;
    uint16_t device_id;
    uint16_t subsys_vendor_id;
    uint16_t subsys_id;
    uint8_t  revision_id;
    uint32_t bar0;
    uint8_t  irq;

    /* Extended PCI fields */
    uint8_t   class_code;
    uint8_t   subclass;
    uint8_t   prog_if;
    uint8_t   header_type;
    uint32_t *bar;
    size_t    bar_count;
    size_t    bar_cap;

    /* Hardware-probed strings (from Option ROM / Expansion ROM / VPD / SMBIOS, or NULL if none) */
    char    *vendor_name;
    char    *device_name;
} pci_device_t;

uint32_t pci_read_dword(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset);
uint16_t pci_read_word(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset);
uint8_t  pci_read_byte(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset);

void     pci_write_dword(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset, uint32_t val);
void     pci_write_word(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset, uint16_t val);
void     pci_write_byte(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset, uint8_t val);

bool pci_find_device(uint16_t vendor_id, uint16_t device_id, pci_device_t *out_dev);
void pci_enable_bus_mastering(const pci_device_t *dev);

/* Full PCI Bus Scanning */
uint32_t pci_scan_all(void (*callback)(const pci_device_t *dev, void *user_data), void *user_data);
uint32_t pci_get_count(void);
const pci_device_t *pci_get_device(uint32_t index);

#endif /* IPO_DRIVER_NET_PCI_H */
