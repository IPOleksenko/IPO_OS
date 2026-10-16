#include <driver/net/pci.h>
#include <ioport.h>
#include <memory/kmalloc.h>
#include <stdio.h>
#include <string.h>

static pci_device_t *s_pci_devices = NULL;
static uint32_t s_pci_count = 0;
static uint32_t s_pci_capacity = 0;

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

uint8_t pci_read_byte(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset) {
    uint32_t dword = pci_read_dword(bus, slot, func, offset);
    return (uint8_t)((dword >> ((offset & 3) * 8)) & 0xFF);
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

void pci_write_byte(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset, uint8_t val) {
    uint32_t dword = pci_read_dword(bus, slot, func, offset);
    uint32_t shift = (offset & 3) * 8;
    dword = (dword & ~(0xFFu << shift)) | ((uint32_t)val << shift);
    pci_write_dword(bus, slot, func, offset, dword);
}

const char *pci_vendor_name(uint16_t vendor_id) {
    static char s_vendor_buf[4][32];
    static uint8_t s_vendor_idx = 0;
    char *res = s_vendor_buf[s_vendor_idx];
    s_vendor_idx = (s_vendor_idx + 1) & 3;
    snprintf(res, sizeof(s_vendor_buf[0]), "Vendor 0x%04X", vendor_id);
    return res;
}

const char *pci_class_name(uint8_t class_code, uint8_t subclass) {
    static char s_class_buf[4][32];
    static uint8_t s_class_idx = 0;
    char *res = s_class_buf[s_class_idx];
    s_class_idx = (s_class_idx + 1) & 3;
    snprintf(res, sizeof(s_class_buf[0]), "Class 0x%02X:0x%02X", class_code, subclass);
    return res;
}

static bool pci_store_device(const pci_device_t *dev) {
    if (s_pci_count >= s_pci_capacity) {
        uint32_t new_cap = (s_pci_capacity == 0) ? 8 : s_pci_capacity * 2;
        pci_device_t *new_arr = (pci_device_t *)kmalloc(new_cap * sizeof(pci_device_t));
        if (!new_arr) return false;
        if (s_pci_devices && s_pci_count > 0) {
            memcpy(new_arr, s_pci_devices, s_pci_count * sizeof(pci_device_t));
            kfree(s_pci_devices);
        }
        s_pci_devices = new_arr;
        s_pci_capacity = new_cap;
    }
    s_pci_devices[s_pci_count++] = *dev;
    return true;
}

uint32_t pci_scan_all(void (*callback)(const pci_device_t *dev, void *user_data), void *user_data) {
    /* Reset internal dynamic cache for fresh rescan */
    s_pci_count = 0;

    /* Scan up to 256 buses, stopping on early unused high buses */
    for (uint16_t bus = 0; bus < 256; bus++) {
        bool bus_has_device = false;

        for (uint8_t slot = 0; slot < 32; slot++) {
            /* Check function 0 to determine if device is present */
            uint16_t ven = pci_read_word((uint8_t)bus, slot, 0, 0x00);
            if (ven == 0xFFFF || ven == 0x0000) {
                continue;
            }

            bus_has_device = true;
            uint8_t header_type = pci_read_byte((uint8_t)bus, slot, 0, 0x0E);
            uint8_t max_func = (header_type & 0x80) ? 8 : 1;

            for (uint8_t func = 0; func < max_func; func++) {
                uint16_t func_ven = pci_read_word((uint8_t)bus, slot, func, 0x00);
                if (func_ven == 0xFFFF || func_ven == 0x0000) {
                    continue;
                }

                pci_device_t dev;
                memset(&dev, 0, sizeof(dev));
                dev.bus = (uint8_t)bus;
                dev.slot = slot;
                dev.func = func;
                dev.vendor_id = func_ven;
                dev.device_id = pci_read_word((uint8_t)bus, slot, func, 0x02);

                uint16_t class_rev = pci_read_word((uint8_t)bus, slot, func, 0x0A);
                dev.class_code = (uint8_t)(class_rev >> 8);
                dev.subclass   = (uint8_t)(class_rev & 0xFF);
                dev.prog_if    = pci_read_byte((uint8_t)bus, slot, func, 0x09);
                dev.header_type= pci_read_byte((uint8_t)bus, slot, func, 0x0E);

                for (int b = 0; b < 6; b++) {
                    dev.bar[b] = pci_read_dword((uint8_t)bus, slot, func, 0x10 + b * 4);
                }
                dev.bar0 = dev.bar[0];
                dev.irq = pci_read_byte((uint8_t)bus, slot, func, 0x3C);

                pci_store_device(&dev);

                if (callback) {
                    callback(&dev, user_data);
                }
            }
        }

        /* If bus 0 had devices but an empty higher bus is encountered, check if we can stop */
        if (!bus_has_device && bus > 1) {
            /* Check if any bridge previously detected points to a higher bus */
            bool higher_bus_expected = false;
            for (uint32_t i = 0; i < s_pci_count; i++) {
                if (s_pci_devices[i].class_code == 0x06 && s_pci_devices[i].subclass == 0x04) {
                    uint8_t sec_bus = pci_read_byte(s_pci_devices[i].bus, s_pci_devices[i].slot, s_pci_devices[i].func, 0x19);
                    if (sec_bus >= bus) {
                        higher_bus_expected = true;
                        break;
                    }
                }
            }
            if (!higher_bus_expected) {
                break;
            }
        }
    }

    return s_pci_count;
}

uint32_t pci_get_count(void) {
    if (s_pci_count == 0) {
        pci_scan_all(NULL, NULL);
    }
    return s_pci_count;
}

const pci_device_t *pci_get_device(uint32_t index) {
    if (s_pci_count == 0) {
        pci_scan_all(NULL, NULL);
    }
    if (index >= s_pci_count) {
        return NULL;
    }
    return &s_pci_devices[index];
}

bool pci_find_device(uint16_t vendor_id, uint16_t device_id, pci_device_t *out_dev) {
    if (s_pci_count == 0) {
        pci_scan_all(NULL, NULL);
    }

    for (uint32_t i = 0; i < s_pci_count; i++) {
        if (s_pci_devices[i].vendor_id == vendor_id && s_pci_devices[i].device_id == device_id) {
            if (out_dev) {
                *out_dev = s_pci_devices[i];
            }
            return true;
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

/* Weak stubs to allow freestanding libapp.a linking when rtl8139.o is filtered out */
__attribute__((weak)) bool rtl8139_is_present(void) {
    return false;
}

__attribute__((weak)) const uint8_t *rtl8139_get_mac(void) {
    return NULL;
}

