#include <driver/net/pci.h>
#include <ioport.h>

uint32_t pci_read_dword(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset) {
    uint32_t address = (uint32_t)((1u << 31) |
                                 ((uint32_t)bus << 16) |
                                 ((uint32_t)slot << 11) |
                                 ((uint32_t)func << 8) |
                                 (offset & 0xFC));
    outl(PCI_CONFIG_ADDRESS, address);
    return inl(PCI_CONFIG_DATA);
}

uint16_t pci_read_word(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset) {
    uint32_t dword = pci_read_dword(bus, slot, func, offset);
    return (uint16_t)((dword >> ((offset & 2) * 8)) & 0xFFFF);
}

void pci_write_dword(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset, uint32_t val) {
    uint32_t address = (uint32_t)((1u << 31) |
                                 ((uint32_t)bus << 16) |
                                 ((uint32_t)slot << 11) |
                                 ((uint32_t)func << 8) |
                                 (offset & 0xFC));
    outl(PCI_CONFIG_ADDRESS, address);
    outl(PCI_CONFIG_DATA, val);
}

void pci_write_word(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset, uint16_t val) {
    uint32_t dword = pci_read_dword(bus, slot, func, offset);
    uint32_t shift = (offset & 2) * 8;
    dword = (dword & ~(0xFFFFu << shift)) | ((uint32_t)val << shift);
    pci_write_dword(bus, slot, func, offset, dword);
}

bool pci_find_device(uint16_t vendor_id, uint16_t device_id, pci_device_t *out_dev) {
    for (uint16_t bus = 0; bus < 256; bus++) {
        for (uint8_t slot = 0; slot < 32; slot++) {
            for (uint8_t func = 0; func < 8; func++) {
                uint16_t ven = pci_read_word((uint8_t)bus, slot, func, 0x00);
                if (ven == 0xFFFF) {
                    if (func == 0) break; // Slot is empty
                    continue;
                }
                uint16_t dev = pci_read_word((uint8_t)bus, slot, func, 0x02);
                if (ven == vendor_id && dev == device_id) {
                    if (out_dev) {
                        out_dev->bus = (uint8_t)bus;
                        out_dev->slot = slot;
                        out_dev->func = func;
                        out_dev->vendor_id = ven;
                        out_dev->device_id = dev;
                        out_dev->bar0 = pci_read_dword((uint8_t)bus, slot, func, 0x10);
                        out_dev->irq = (uint8_t)(pci_read_word((uint8_t)bus, slot, func, 0x3C) & 0xFF);
                    }
                    return true;
                }
            }
        }
    }
    return false;
}

void pci_enable_bus_mastering(const pci_device_t *dev) {
    if (!dev) return;
    uint16_t cmd = pci_read_word(dev->bus, dev->slot, dev->func, 0x04);
    cmd |= 0x0005; // Bit 0: I/O Space, Bit 2: Bus Master
    pci_write_word(dev->bus, dev->slot, dev->func, 0x04, cmd);
}
