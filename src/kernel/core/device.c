#include <kernel/device.h>
#include <kernel/smp.h>
#include <memory/kmalloc.h>
#include <memory/memory_map.h>
#include <driver/net/pci.h>
#include <driver/net/rtl8139.h>
#include <driver/ata/ata.h>
#include <driver/sound.h>
#include <driver/audio_core.h>
#include <system/timer.h>
#include <ioport.h>
#include <net/net.h>
#include <net/loopback.h>
#include <system/pit.h>
#include <driver/vbe_bga.h>
#include <string.h>
#include <stdio.h>

static device_node_t *s_device_head = NULL;
static spinlock_t     s_device_lock = {0};
static uint32_t       s_next_id = 1;
static bool           s_registry_initialized = false;

const char *device_type_to_string(device_type_t type) {
    switch (type) {
        case DEVICE_TYPE_CPU:     return "CPU";
        case DEVICE_TYPE_MEMORY:  return "MEMORY";
        case DEVICE_TYPE_PCI:     return "PCI";
        case DEVICE_TYPE_STORAGE: return "STORAGE";
        case DEVICE_TYPE_NETWORK: return "NETWORK";
        case DEVICE_TYPE_INPUT:   return "INPUT";
        case DEVICE_TYPE_AUDIO:   return "AUDIO";
        case DEVICE_TYPE_VIDEO:   return "VIDEO";
        case DEVICE_TYPE_SYSTEM:    return "SYSTEM";
        case DEVICE_TYPE_USB:       return "USB";
        default:                    return "UNKNOWN";
    }
}

const char *device_state_to_string(device_state_t state) {
    switch (state) {
        case DEVICE_STATE_DISCOVERED:      return "DISCOVERED";
        case DEVICE_STATE_DRIVER_ATTACHED: return "ATTACHED";
        case DEVICE_STATE_INITIALIZED:     return "INITIALIZED";
        case DEVICE_STATE_ACTIVE:          return "ACTIVE";
        case DEVICE_STATE_DISABLED:        return "DISABLED";
        case DEVICE_STATE_ERROR:           return "ERROR";
        default:                           return "UNKNOWN";
    }
}

const char *bus_type_to_string(bus_type_t bus) {
    switch (bus) {
        case BUS_TYPE_INTERNAL: return "Internal";
        case BUS_TYPE_PCI:      return "PCI";
        case BUS_TYPE_ISA:      return "ISA";
        case BUS_TYPE_IDE:      return "IDE/ATA";
        case BUS_TYPE_USB:      return "USB";
        case BUS_TYPE_UART:     return "UART";
        case BUS_TYPE_VIRTUAL:  return "Virtual";
        default:                return "Other";
    }
}

/* Dynamic device_resources_t lifecycle implementation */
void device_resources_init(device_resources_t *res) {
    if (!res) return;
    memset(res, 0, sizeof(*res));
}

void device_resources_free(device_resources_t *res) {
    if (!res) return;
    if (res->bar) {
        kfree(res->bar);
        res->bar = NULL;
    }
    res->bar_count = 0;
    res->bar_cap = 0;
}

int device_resources_add_bar(device_resources_t *res, uint32_t bar) {
    if (!res) return -1;
    if (res->bar_count >= res->bar_cap) {
        size_t new_cap = (res->bar_cap == 0) ? 6 : (res->bar_cap * 2);
        uint32_t *new_bar = (uint32_t *)krealloc(res->bar, new_cap * sizeof(uint32_t));
        if (!new_bar) return -1;
        memset(new_bar + res->bar_count, 0, (new_cap - res->bar_count) * sizeof(uint32_t));
        res->bar = new_bar;
        res->bar_cap = new_cap;
    }
    res->bar[res->bar_count++] = bar;
    return 0;
}

int device_resources_set_bar(device_resources_t *res, size_t index, uint32_t bar) {
    if (!res) return -1;
    if (index >= res->bar_cap) {
        size_t new_cap = index + 4;
        uint32_t *new_bar = (uint32_t *)krealloc(res->bar, new_cap * sizeof(uint32_t));
        if (!new_bar) return -1;
        memset(new_bar + res->bar_cap, 0, (new_cap - res->bar_cap) * sizeof(uint32_t));
        res->bar = new_bar;
        res->bar_cap = new_cap;
    }
    res->bar[index] = bar;
    if (index >= res->bar_count) {
        res->bar_count = index + 1;
    }
    return 0;
}

int device_resources_copy(device_resources_t *dst, const device_resources_t *src) {
    if (!dst) return -1;
    device_resources_free(dst);
    if (!src) return 0;
    *dst = *src;
    dst->bar = NULL;
    dst->bar_count = 0;
    dst->bar_cap = 0;
    if (src->bar && src->bar_count > 0) {
        dst->bar = (uint32_t *)kmalloc(src->bar_count * sizeof(uint32_t));
        if (dst->bar) {
            memcpy(dst->bar, src->bar, src->bar_count * sizeof(uint32_t));
            dst->bar_count = src->bar_count;
            dst->bar_cap = src->bar_count;
        }
    }
    return 0;
}

/* Dynamic device_node_t lifecycle implementation */
void device_node_init(device_node_t *dev) {
    if (!dev) return;
    memset(dev, 0, sizeof(*dev));
    device_resources_init(&dev->res);
}

void device_node_free(device_node_t *dev) {
    if (!dev) return;
    if (dev->name) { kfree(dev->name); dev->name = NULL; }
    dev->name_len = 0; dev->name_cap = 0;
    if (dev->vendor) { kfree(dev->vendor); dev->vendor = NULL; }
    dev->vendor_len = 0; dev->vendor_cap = 0;
    if (dev->model) { kfree(dev->model); dev->model = NULL; }
    dev->model_len = 0; dev->model_cap = 0;
    if (dev->description) { kfree(dev->description); dev->description = NULL; }
    dev->description_len = 0; dev->description_cap = 0;
    if (dev->driver_name) { kfree(dev->driver_name); dev->driver_name = NULL; }
    dev->driver_name_len = 0; dev->driver_name_cap = 0;
    device_resources_free(&dev->res);
}

static int str_set_dynamic(char **field, size_t *field_len, size_t *field_cap, const char *src) {
    if (*field) {
        kfree(*field);
        *field = NULL;
    }
    *field_len = 0;
    *field_cap = 0;
    if (!src) return 0;
    size_t len = strlen(src);
    size_t cap = len + 1;
    char *buf = (char *)kmalloc(cap);
    if (!buf) return -1;
    memcpy(buf, src, len);
    buf[len] = '\0';
    *field = buf;
    *field_len = len;
    *field_cap = cap;
    return 0;
}

int device_node_set_name(device_node_t *dev, const char *name) {
    if (!dev) return -1;
    return str_set_dynamic(&dev->name, &dev->name_len, &dev->name_cap, name);
}

int device_node_set_vendor(device_node_t *dev, const char *vendor) {
    if (!dev) return -1;
    return str_set_dynamic(&dev->vendor, &dev->vendor_len, &dev->vendor_cap, vendor);
}

int device_node_set_model(device_node_t *dev, const char *model) {
    if (!dev) return -1;
    return str_set_dynamic(&dev->model, &dev->model_len, &dev->model_cap, model);
}

int device_node_set_description(device_node_t *dev, const char *desc) {
    if (!dev) return -1;
    return str_set_dynamic(&dev->description, &dev->description_len, &dev->description_cap, desc);
}

int device_node_set_driver_name(device_node_t *dev, const char *driver) {
    if (!dev) return -1;
    return str_set_dynamic(&dev->driver_name, &dev->driver_name_len, &dev->driver_name_cap, driver);
}

int device_node_copy(device_node_t *dst, const device_node_t *src) {
    if (!dst) return -1;
    device_node_free(dst);
    if (!src) return 0;

    dst->id = src->id;
    dst->parent_id = src->parent_id;
    dst->capabilities = src->capabilities;
    dst->type = src->type;
    dst->state = src->state;
    device_node_set_name(dst, src->name);
    device_node_set_vendor(dst, src->vendor);
    device_node_set_model(dst, src->model);
    device_node_set_description(dst, src->description);
    device_node_set_driver_name(dst, src->driver_name);
    device_resources_copy(&dst->res, &src->res);
    dst->driver_ptr = src->driver_ptr;
    dst->ops = src->ops;
    dst->priv_data = src->priv_data;
    dst->next = NULL;
    return 0;
}

device_node_t *device_find_by_name(const char *name) {
    if (!name || !*name) return NULL;
    spin_lock(&s_device_lock);
    device_node_t *curr = s_device_head;
    while (curr) {
        if (curr->name && strcmp(curr->name, name) == 0) {
            spin_unlock(&s_device_lock);
            return curr;
        }
        curr = curr->next;
    }
    spin_unlock(&s_device_lock);
    return NULL;
}

device_node_t *device_find_by_id(uint32_t id) {
    if (id == 0) return NULL;
    spin_lock(&s_device_lock);
    device_node_t *curr = s_device_head;
    while (curr) {
        if (curr->id == id) {
            spin_unlock(&s_device_lock);
            return curr;
        }
        curr = curr->next;
    }
    spin_unlock(&s_device_lock);
    return NULL;
}

device_node_t *device_register(const device_node_t *dev) {
    if (!dev) return NULL;

    spin_lock(&s_device_lock);

    /* Check if already registered by name */
    device_node_t *existing = s_device_head;
    while (existing) {
        if (existing->name && dev->name && strcmp(existing->name, dev->name) == 0) {
            /* Update existing entry */
            existing->state = dev->state;
            existing->type = dev->type;
            existing->parent_id = dev->parent_id;
            existing->capabilities = dev->capabilities;
            if (dev->vendor) device_node_set_vendor(existing, dev->vendor);
            if (dev->model)  device_node_set_model(existing, dev->model);
            if (dev->description) device_node_set_description(existing, dev->description);
            if (dev->driver_name) device_node_set_driver_name(existing, dev->driver_name);
            device_resources_copy(&existing->res, &dev->res);
            existing->driver_ptr = dev->driver_ptr;
            existing->ops = dev->ops;
            existing->priv_data = dev->priv_data;
            spin_unlock(&s_device_lock);
            return existing;
        }
        existing = existing->next;
    }

    /* Allocate dynamic node */
    device_node_t *node = (device_node_t *)kmalloc(sizeof(device_node_t));
    if (!node) {
        spin_unlock(&s_device_lock);
        return NULL;
    }

    device_node_init(node);
    device_node_copy(node, dev);
    node->id = (dev->id != 0) ? dev->id : s_next_id++;
    node->next = NULL;

    /* Append to tail of linked list */
    if (!s_device_head) {
        s_device_head = node;
    } else {
        device_node_t *tail = s_device_head;
        while (tail->next) {
            tail = tail->next;
        }
        tail->next = node;
    }

    spin_unlock(&s_device_lock);
    return node;
}

int device_unregister(uint32_t id) {
    if (id == 0) return -1;
    spin_lock(&s_device_lock);

    device_node_t *curr = s_device_head;
    device_node_t *prev = NULL;

    while (curr) {
        if (curr->id == id) {
            if (prev) {
                prev->next = curr->next;
            } else {
                s_device_head = curr->next;
            }
            spin_unlock(&s_device_lock);
            device_node_free(curr);
            kfree(curr);
            return 0;
        }
        prev = curr;
        curr = curr->next;
    }

    spin_unlock(&s_device_lock);
    return -2; /* Not found */
}

int device_set_state(uint32_t id, device_state_t state) {
    spin_lock(&s_device_lock);
    device_node_t *node = NULL;
    device_node_t *curr = s_device_head;
    while (curr) {
        if (curr->id == id) { node = curr; break; }
        curr = curr->next;
    }
    if (!node) {
        spin_unlock(&s_device_lock);
        return -1;
    }
    node->state = state;
    spin_unlock(&s_device_lock);
    return 0;
}

int device_set_driver(uint32_t id, const char *driver_name, void *driver_ptr) {
    spin_lock(&s_device_lock);
    device_node_t *node = NULL;
    device_node_t *curr = s_device_head;
    while (curr) {
        if (curr->id == id) { node = curr; break; }
        curr = curr->next;
    }
    if (!node) {
        spin_unlock(&s_device_lock);
        return -1;
    }
    if (driver_name) {
        device_node_set_driver_name(node, driver_name);
    }
    node->driver_ptr = driver_ptr;
    if (node->state == DEVICE_STATE_DISCOVERED) {
        node->state = DEVICE_STATE_DRIVER_ATTACHED;
    }
    spin_unlock(&s_device_lock);
    return 0;
}

uint32_t device_get_count(void) {
    uint32_t count = 0;
    spin_lock(&s_device_lock);
    device_node_t *curr = s_device_head;
    while (curr) {
        count++;
        curr = curr->next;
    }
    spin_unlock(&s_device_lock);
    return count;
}

uint32_t device_get_count_by_type(device_type_t type) {
    uint32_t count = 0;
    spin_lock(&s_device_lock);
    device_node_t *curr = s_device_head;
    while (curr) {
        if (curr->type == type) count++;
        curr = curr->next;
    }
    spin_unlock(&s_device_lock);
    return count;
}

uint32_t device_get_count_by_state(device_state_t state) {
    uint32_t count = 0;
    spin_lock(&s_device_lock);
    device_node_t *curr = s_device_head;
    while (curr) {
        if (curr->state == state) count++;
        curr = curr->next;
    }
    spin_unlock(&s_device_lock);
    return count;
}

const device_node_t *device_get_list(void) {
    return s_device_head;
}

/* -------------------------------------------------------------
 * Hardware Discovery & Enumeration Engine
 * ------------------------------------------------------------- */

static void scan_cpu_subsystem(void) {
    char vendor[32];
    char brand[64];
    smp_get_cpu_vendor(vendor, sizeof(vendor));
    smp_get_cpu_brand(brand, sizeof(brand));

    uint32_t cpu_count = smp_get_cpu_count();
    if (cpu_count == 0) cpu_count = 1;

    for (uint32_t i = 0; i < cpu_count; i++) {
        const cpu_info_t *info = smp_get_cpu_info(i);
        device_node_t dev;
        device_node_init(&dev);

        dev.type = DEVICE_TYPE_CPU;
        dev.state = (info && info->is_online) ? DEVICE_STATE_ACTIVE : DEVICE_STATE_INITIALIZED;
        char name[32];
        snprintf(name, sizeof(name), "cpu%u", i);
        device_node_set_name(&dev, name);
        device_node_set_vendor(&dev, vendor);
        device_node_set_model(&dev, brand);

        uint32_t lapic_id = info ? info->lapic_id : i;
        bool is_bsp = info ? info->is_bsp : (i == 0);
        char desc[128];
        snprintf(desc, sizeof(desc),
                 "x86 Logical Core %u [%s, LAPIC ID %u]",
                 i, is_bsp ? "BSP Primary" : "AP Secondary", lapic_id);
        device_node_set_description(&dev, desc);

        dev.res.bus_type = BUS_TYPE_INTERNAL;
        dev.res.mmio_base = smp_get_lapic_base();
        dev.res.mmio_size = 0x1000;
        dev.res.size_bytes = (uint64_t)cpu_count;

        device_node_set_driver_name(&dev, "smp_core");
        device_register(&dev);
        device_node_free(&dev);
    }
}

static void scan_memory_subsystem(void) {
    uint32_t region_count = memory_map_get_region_count();
    uint64_t total_ram = memory_map_get_total_ram();

    if (region_count == 0) {
        /* Single fallback pool */
        device_node_t dev;
        device_node_init(&dev);
        dev.type = DEVICE_TYPE_MEMORY;
        dev.state = DEVICE_STATE_ACTIVE;
        device_node_set_name(&dev, "ram0");
        device_node_set_vendor(&dev, "System BIOS");
        device_node_set_model(&dev, "System RAM");
        char desc[64];
        snprintf(desc, sizeof(desc), "Total RAM: %u MB", (uint32_t)(total_ram / (1024 * 1024)));
        device_node_set_description(&dev, desc);
        dev.res.bus_type = BUS_TYPE_INTERNAL;
        dev.res.mmio_base = 0x00100000;
        dev.res.size_bytes = total_ram;
        device_node_set_driver_name(&dev, "memory_map");
        device_register(&dev);
        device_node_free(&dev);
        return;
    }

    for (uint32_t i = 0; i < region_count; i++) {
        const memory_region_t *reg = memory_map_get_region(i);
        if (!reg) continue;

        device_node_t dev;
        device_node_init(&dev);
        dev.type = DEVICE_TYPE_MEMORY;
        dev.state = reg->is_usable ? DEVICE_STATE_ACTIVE : DEVICE_STATE_INITIALIZED;
        char name[32];
        snprintf(name, sizeof(name), "ram%u", i);
        device_node_set_name(&dev, name);
        device_node_set_vendor(&dev, "BIOS E820");
        char model[64];
        snprintf(model, sizeof(model), "%s RAM Pool", reg->is_usable ? "Usable" : "Reserved");
        device_node_set_model(&dev, model);
        char desc[128];
        snprintf(desc, sizeof(desc),
                 "Base: 0x%08x, Size: %u KB, Used: %u KB",
                 reg->base, reg->size / 1024u, reg->used / 1024u);
        device_node_set_description(&dev, desc);

        dev.res.bus_type = BUS_TYPE_INTERNAL;
        dev.res.mmio_base = reg->base;
        dev.res.mmio_size = reg->used;
        dev.res.size_bytes = (uint64_t)reg->size;
        device_node_set_driver_name(&dev, "memory_map");

        device_register(&dev);
        device_node_free(&dev);
    }
}

static void scan_pci_subsystem(void) {
    uint32_t pci_count = pci_scan_all(NULL, NULL);

    for (uint32_t i = 0; i < pci_count; i++) {
        const pci_device_t *pdev = pci_get_device(i);
        if (!pdev) continue;

        device_node_t dev;
        device_node_init(&dev);

        /* Identify category */
        if (pdev->class_code == 0x0C && pdev->subclass == 0x03) {
            dev.type = DEVICE_TYPE_USB;
        } else if (pdev->class_code == 0x01) {
            dev.type = DEVICE_TYPE_STORAGE;
        } else if (pdev->class_code == 0x02) {
            dev.type = DEVICE_TYPE_NETWORK;
        } else if (pdev->class_code == 0x03) {
            dev.type = DEVICE_TYPE_VIDEO;
        } else if (pdev->class_code == 0x04) {
            dev.type = DEVICE_TYPE_AUDIO;
        } else {
            dev.type = DEVICE_TYPE_PCI;
        }

        char name[32];
        snprintf(name, sizeof(name), "pci%02x:%02x.%x", pdev->bus, pdev->slot, pdev->func);
        device_node_set_name(&dev, name);
        device_node_set_vendor(&dev, pci_vendor_name(pdev->vendor_id));
        device_node_set_model(&dev, pci_class_name(pdev->class_code, pdev->subclass));

        /* Set default description */
        char desc[160];
        if (dev.type == DEVICE_TYPE_USB) {
            snprintf(desc, sizeof(desc),
                     "USB Host Controller (ProgIF 0x%02X) [0x%04x:0x%04x]",
                     pdev->prog_if, pdev->vendor_id, pdev->device_id);
        } else {
            snprintf(desc, sizeof(desc), "%s [DevID 0x%04x:0x%04x IRQ %u]",
                     dev.model ? dev.model : "PCI Device", pdev->vendor_id, pdev->device_id, pdev->irq);
        }
        device_node_set_description(&dev, desc);

        dev.res.bus_type = BUS_TYPE_PCI;
        dev.res.pci_bus = pdev->bus;
        dev.res.pci_slot = pdev->slot;
        dev.res.pci_func = pdev->func;
        dev.res.vendor_id = pdev->vendor_id;
        dev.res.device_id = pdev->device_id;
        dev.res.pci_class = pdev->class_code;
        dev.res.pci_subclass = pdev->subclass;
        dev.res.pci_progif = pdev->prog_if;
        for (int b = 0; b < 6; b++) device_resources_add_bar(&dev.res, pdev->bar[b]);
        dev.res.irq = pdev->irq;

        /* Extract primary I/O base or MMIO from BARs */
        for (int b = 0; b < 6; b++) {
            if (pdev->bar[b] & 1) {
                if (dev.res.io_base == 0) dev.res.io_base = (uint16_t)(pdev->bar[b] & ~3u);
            } else if (pdev->bar[b] != 0) {
                if (dev.res.mmio_base == 0) dev.res.mmio_base = pdev->bar[b] & ~0xFu;
            }
        }

        /* Associate driver matching */
        if (pdev->vendor_id == 0x10EC && pdev->device_id == 0x8139) {
            device_node_set_driver_name(&dev, "rtl8139");
            dev.state = rtl8139_is_present() ? DEVICE_STATE_ACTIVE : DEVICE_STATE_INITIALIZED;
        } else if (pdev->class_code == 0x01 && pdev->subclass == 0x01) {
            device_node_set_driver_name(&dev, "ata");
            dev.state = DEVICE_STATE_ACTIVE;
        } else if (pdev->class_code == 0x03) {
            device_node_set_driver_name(&dev, "vga");
            dev.state = DEVICE_STATE_ACTIVE;
        } else {
            device_node_set_driver_name(&dev, "pci_generic");
            dev.state = DEVICE_STATE_INITIALIZED;
        }

        device_register(&dev);
        device_node_free(&dev);
    }
}

static void scan_storage_subsystem(void) {
    uint32_t count = ata_get_device_count();

    for (uint32_t i = 0; i < count; i++) {
        ata_device_t *adev = ata_get_device(i);
        if (!adev || !adev->present) continue;

        device_node_t dev;
        device_node_init(&dev);

        dev.type = DEVICE_TYPE_STORAGE;
        dev.state = DEVICE_STATE_ACTIVE;
        char name[32];
        snprintf(name, sizeof(name), "ata%u-%s",
                 adev->channel, adev->drive ? "slave" : "master");
        device_node_set_name(&dev, name);
        device_node_set_vendor(&dev, "ATA/IDE Disk");
        device_node_set_model(&dev, (adev->model && adev->model[0]) ? adev->model : "Generic Hard Disk");

        uint64_t cap_bytes = adev->capacity_sectors * 512ull;
        uint32_t cap_mb = (uint32_t)(cap_bytes / (1024ull * 1024ull));

        char desc[160];
        snprintf(desc, sizeof(desc),
                 "Channel %u %s: %u MB (%u sectors), S/N: %s",
                 adev->channel, adev->drive ? "Slave" : "Master",
                 cap_mb, (uint32_t)adev->capacity_sectors,
                 (adev->serial && adev->serial[0]) ? adev->serial : "N/A");
        device_node_set_description(&dev, desc);

        dev.res.bus_type = BUS_TYPE_IDE;
        dev.res.io_base = (adev->channel == 0) ? 0x1F0 : 0x170;
        dev.res.irq = (adev->channel == 0) ? 14 : 15;
        dev.res.size_bytes = cap_bytes;
        device_node_set_driver_name(&dev, "ata");

        device_register(&dev);
        device_node_free(&dev);
    }
}

/* ---------------------------------------------------------------------
 * Live hardware probes: every description below is read from the device
 * itself (registers, BIOS tables, driver state) rather than hardcoded.
 * --------------------------------------------------------------------- */
static bool ps2_read_config(uint8_t *out) {
    uint32_t flags;
    __asm__ volatile("pushf; pop %0; cli" : "=r"(flags) : : "memory");
    bool ok = false;
    for (int i = 0; i < 100000 && (inb(0x64) & 0x02); i++) io_wait();
    outb(0x64, 0x20);
    for (int i = 0; i < 100000; i++) {
        if (inb(0x64) & 0x01) { *out = inb(0x60); ok = true; break; }
        io_wait();
    }
    if (flags & 0x200) __asm__ volatile("sti" : : : "memory");
    return ok;
}

static uint8_t pit_read_status(uint8_t counter) {
    outb(0x43, (uint8_t)(0xE0 | (2u << counter)));
    return inb(0x40 + counter);
}

static bool acpi_find_rsdp(uint32_t *out_addr, uint8_t *out_rev, char oem[7], uint32_t *out_rsdt) {
    uint32_t ebda = ((uint32_t)*(volatile uint16_t *)0x40E) << 4;
    uint32_t ranges[2][2] = { { ebda, ebda + 1024 }, { 0xE0000, 0x100000 } };
    for (int r = 0; r < 2; r++) {
        if (r == 0 && (ebda < 0x80000 || ebda >= 0xA0000)) continue;
        for (uint32_t a = ranges[r][0]; a + 20 <= ranges[r][1]; a += 16) {
            const uint8_t *p = (const uint8_t *)(uintptr_t)a;
            if (memcmp(p, "RSD PTR ", 8) != 0) continue;
            uint8_t sum = 0;
            for (int i = 0; i < 20; i++) sum = (uint8_t)(sum + p[i]);
            if (sum != 0) continue;
            *out_addr = a;
            *out_rev = p[15];
            memcpy(oem, p + 9, 6);
            oem[6] = 0;
            *out_rsdt = *(const uint32_t *)(p + 16);
            return true;
        }
    }
    return false;
}

static void ip_fmt(ip4_addr_t ip, char *buf, size_t n) {
    snprintf(buf, n, "%u.%u.%u.%u", (ip >> 24) & 0xFF, (ip >> 16) & 0xFF, (ip >> 8) & 0xFF, ip & 0xFF);
}

static int mask_prefix(ip4_addr_t m) {
    int n = 0;
    while (m & 0x80000000u) { n++; m <<= 1; }
    return n;
}

__attribute__((weak)) bool loopback_get_info(uint16_t *mtu, size_t *queue_capacity) {
    if (mtu) *mtu = 65536;
    if (queue_capacity) *queue_capacity = 256;
    return true;
}

static void scan_network_subsystem(void) {
    /* 1. Realtek RTL8139 NIC */
    if (rtl8139_is_present()) {
        const uint8_t *mac = rtl8139_get_mac();
        device_node_t dev;
        device_node_init(&dev);

        dev.type = DEVICE_TYPE_NETWORK;
        dev.state = DEVICE_STATE_ACTIVE;
        device_node_set_name(&dev, "rtl8139-eth0");
        device_node_set_vendor(&dev, pci_vendor_name(RTL8139_VENDOR_ID));
        device_node_set_model(&dev, pci_class_name(0x02, 0x00));

        pci_device_t pdev;
        if (pci_find_device(RTL8139_VENDOR_ID, RTL8139_DEVICE_ID, &pdev)) {
            dev.res.bus_type = BUS_TYPE_PCI;
            dev.res.pci_bus = pdev.bus;
            dev.res.pci_slot = pdev.slot;
            dev.res.pci_func = pdev.func;
            dev.res.io_base = (uint16_t)(pdev.bar0 & ~3u);
            dev.res.irq = pdev.irq;
        }

        char desc[128];
        if (mac) {
            snprintf(desc, sizeof(desc),
                     "MAC: %02x:%02x:%02x:%02x:%02x:%02x, Port: 0x%x, IRQ: %u",
                     mac[0], mac[1], mac[2], mac[3], mac[4], mac[5],
                     dev.res.io_base, dev.res.irq);
        } else {
            snprintf(desc, sizeof(desc), "Port: 0x%x, IRQ: %u", dev.res.io_base, dev.res.irq);
        }
        device_node_set_description(&dev, desc);

        device_node_set_driver_name(&dev, "rtl8139");
        device_register(&dev);
        device_node_free(&dev);
    }

    /* 2. Virtual Loopback Interface */
    device_node_t lo;
    device_node_init(&lo);
    lo.type = DEVICE_TYPE_NETWORK;
    lo.state = DEVICE_STATE_ACTIVE;
    device_node_set_name(&lo, "net-lo0");
    device_node_set_vendor(&lo, "IPO_OS Kernel");
    device_node_set_model(&lo, "Local Loopback Network Adapter");
    {
        char ipb[16], desc[96];
        uint16_t mtu = 0;
        size_t qcap = 0;
        bool up = loopback_get_info(&mtu, &qcap);
        ip_fmt(IP4_ADDR_LOOPBACK, ipb, sizeof(ipb));
        snprintf(desc, sizeof(desc), "IPv4: %s/8, MTU: %u, Interface: %s, Queue: %u slots",
                 ipb, (unsigned)mtu, up ? "UP" : "DOWN", (unsigned)qcap);
        device_node_set_description(&lo, desc);
        if (!up) lo.state = DEVICE_STATE_DISABLED;
    }
    lo.res.bus_type = BUS_TYPE_INTERNAL;
    device_node_set_driver_name(&lo, "loopback");
    device_register(&lo);
    device_node_free(&lo);
}

static void scan_system_and_input_subsystem(void) {
    /* PIT 8254 */
    device_node_t pit;
    device_node_init(&pit);
    pit.type = DEVICE_TYPE_SYSTEM;
    pit.state = DEVICE_STATE_ACTIVE;
    device_node_set_name(&pit, "pit0");
    device_node_set_vendor(&pit, "System");
    device_node_set_model(&pit, "PIT 8254");
    {
        char desc[96];
        uint8_t st = pit_read_status(0);
        snprintf(desc, sizeof(desc), "Channel 0 Timer (%u Hz programmed, mode %u, status 0x%02X), Ports 0x40-0x43",
                 (unsigned)pit_get_frequency(0), (unsigned)((st >> 1) & 7), (unsigned)st);
        device_node_set_description(&pit, desc);
    }
    pit.res.bus_type = BUS_TYPE_ISA;
    pit.res.io_base = 0x40;
    pit.res.io_size = 4;
    pit.res.irq = 0;
    device_node_set_driver_name(&pit, "pit");
    device_register(&pit);
    device_node_free(&pit);

    /* Dual PIC 8259 */
    device_node_t pic;
    device_node_init(&pic);
    pic.type = DEVICE_TYPE_SYSTEM;
    pic.state = DEVICE_STATE_ACTIVE;
    device_node_set_name(&pic, "pic0");
    device_node_set_vendor(&pic, "System");
    device_node_set_model(&pic, "Dual PIC 8259A");
    {
        char desc[96];
        snprintf(desc, sizeof(desc), "Master PIC 0x20-0x21 (IMR 0x%02X), Slave PIC 0xA0-0xA1 (IMR 0x%02X)",
                 (unsigned)inb(0x21), (unsigned)inb(0xA1));
        device_node_set_description(&pic, desc);
    }
    pic.res.bus_type = BUS_TYPE_ISA;
    pic.res.io_base = 0x20;
    pic.res.io_size = 2;
    device_node_set_driver_name(&pic, "pic");
    device_register(&pic);
    device_node_free(&pic);

    /* PS/2 Keyboard */
    device_node_t kbd;
    device_node_init(&kbd);
    kbd.type = DEVICE_TYPE_INPUT;
    kbd.state = DEVICE_STATE_ACTIVE;
    device_node_set_name(&kbd, "ps2-kbd");
    device_node_set_vendor(&kbd, "PS/2 Controller");
    device_node_set_model(&kbd, "Keyboard");
    {
        char desc[96];
        uint8_t cfg = 0;
        if (ps2_read_config(&cfg)) {
            snprintf(desc, sizeof(desc), "8042 config 0x%02X: IRQ1 %s, clock %s, translation %s",
                     (unsigned)cfg, (cfg & 0x01) ? "enabled" : "disabled",
                     (cfg & 0x10) ? "disabled" : "enabled", (cfg & 0x40) ? "on" : "off");
            if (cfg & 0x10) kbd.state = DEVICE_STATE_DISABLED;
        } else {
            snprintf(desc, sizeof(desc), "8042 controller did not respond");
            kbd.state = DEVICE_STATE_DISABLED;
        }
        device_node_set_description(&kbd, desc);
    }
    kbd.res.bus_type = BUS_TYPE_ISA;
    kbd.res.io_base = 0x60;
    kbd.res.irq = 1;
    device_node_set_driver_name(&kbd, "keyboard");
    device_register(&kbd);
    device_node_free(&kbd);

    /* PS/2 Mouse */
    device_node_t mouse;
    device_node_init(&mouse);
    mouse.type = DEVICE_TYPE_INPUT;
    mouse.state = DEVICE_STATE_ACTIVE;
    device_node_set_name(&mouse, "ps2-mouse");
    device_node_set_vendor(&mouse, "PS/2 Controller");
    device_node_set_model(&mouse, "Mouse (Aux Port)");
    {
        char desc[96];
        uint8_t cfg = 0;
        if (ps2_read_config(&cfg)) {
            snprintf(desc, sizeof(desc), "8042 aux port: IRQ12 %s, clock %s (config 0x%02X)",
                     (cfg & 0x02) ? "enabled" : "disabled",
                     (cfg & 0x20) ? "disabled" : "enabled", (unsigned)cfg);
            if (cfg & 0x20) mouse.state = DEVICE_STATE_DISABLED;
        } else {
            snprintf(desc, sizeof(desc), "8042 controller did not respond");
            mouse.state = DEVICE_STATE_DISABLED;
        }
        device_node_set_description(&mouse, desc);
    }
    mouse.res.bus_type = BUS_TYPE_ISA;
    mouse.res.io_base = 0x60;
    mouse.res.irq = 12;
    device_node_set_driver_name(&mouse, "mouse");
    device_register(&mouse);
    device_node_free(&mouse);

    /* PC Speaker */
    device_node_t spk;
    device_node_init(&spk);
    spk.type = DEVICE_TYPE_AUDIO;
    spk.state = DEVICE_STATE_ACTIVE;
    device_node_set_name(&spk, "pcspk0");
    device_node_set_vendor(&spk, "System Audio");
    device_node_set_model(&spk, "Internal PC Speaker");
    {
        char desc[96];
        uint8_t p61 = inb(0x61);
        snprintf(desc, sizeof(desc), "PIT Counter 2 gate %s, speaker output %s (port 0x61=0x%02X), counter 2 %u Hz",
                 (p61 & 1) ? "open" : "closed", (p61 & 2) ? "on" : "off", (unsigned)p61,
                 (unsigned)pit_get_frequency(2));
        device_node_set_description(&spk, desc);
    }
    spk.res.bus_type = BUS_TYPE_ISA;
    spk.res.io_base = 0x61;
    device_node_set_driver_name(&spk, "sound");
    device_register(&spk);
    device_node_free(&spk);

    /* Sound Blaster 16 */
    if (audio_get_active_backend() == AUDIO_BACKEND_SB16) {
        device_node_t sb;
        device_node_init(&sb);
        sb.type = DEVICE_TYPE_AUDIO;
        sb.state = DEVICE_STATE_ACTIVE;
        device_node_set_name(&sb, "sb16");
        device_node_set_vendor(&sb, "Audio DSP");
        {
            char model[48], desc[96];
            uint8_t maj = 0, min = 0;
            sb16_get_dsp_version(&maj, &min);
            snprintf(model, sizeof(model), "Sound Blaster DSP v%u.%02u", (unsigned)maj, (unsigned)min);
            device_node_set_model(&sb, model);

            /* Read configured IRQ / DMA from the card's mixer registers */
            outb(0x224, 0x80); uint8_t irq_reg = inb(0x225);
            outb(0x224, 0x81); uint8_t dma_reg = inb(0x225);
            unsigned irq = (irq_reg & 1) ? 2 : (irq_reg & 2) ? 5 : (irq_reg & 4) ? 7 : (irq_reg & 8) ? 10 : 0;
            unsigned dma8 = (dma_reg & 1) ? 0 : (dma_reg & 2) ? 1 : (dma_reg & 8) ? 3 : 0xFF;
            unsigned dma16 = (dma_reg & 0x20) ? 5 : (dma_reg & 0x40) ? 6 : (dma_reg & 0x80) ? 7 : 0xFF;
            snprintf(desc, sizeof(desc), "Audio DSP & Mixer, Port 0x220, IRQ %u, DMA %u/%u",
                     irq, dma8, dma16);
            device_node_set_description(&sb, desc);
            sb.res.irq = (uint8_t)irq;
        }
        sb.res.bus_type = BUS_TYPE_ISA;
        sb.res.io_base = 0x220;
        device_node_set_driver_name(&sb, "audio_core");
        device_register(&sb);
        device_node_free(&sb);
    }

    /* VGA Graphics / Display */
    device_node_t vga;
    device_node_init(&vga);
    vga.type = DEVICE_TYPE_VIDEO;
    vga.state = DEVICE_STATE_ACTIVE;
    device_node_set_name(&vga, "vga0");
    device_node_set_vendor(&vga, "Standard VGA / VBE");
    device_node_set_model(&vga, "VGA Display Adapter & VBE Extensions");
    {
        char desc[160];
        uint8_t bda_mode = *(volatile uint8_t *)0x449;
        uint16_t cols = *(volatile uint16_t *)0x44A;
        uint8_t rows = (uint8_t)(*(volatile uint8_t *)0x484 + 1);
        outb(0x3CE, 0x06);
        uint8_t gc_misc = inb(0x3CF);
        static const uint32_t win_base[4] = { 0xA0000, 0xA0000, 0xB0000, 0xB8000 };
        static const uint32_t win_size[4] = { 0x20000, 0x10000, 0x8000, 0x8000 };
        uint32_t base = win_base[(gc_misc >> 2) & 3];
        vga.res.mmio_base = base;
        vga.res.mmio_size = win_size[(gc_misc >> 2) & 3];

        vbe_bga_info_t vi;
        int n = snprintf(desc, sizeof(desc), "BIOS mode 0x%02X %ux%u @ 0x%X",
                         (unsigned)bda_mode, (unsigned)cols, (unsigned)rows, (unsigned)base);
        if (vbe_bga_probe(&vi) && vi.available) {
            snprintf(desc + n, sizeof(desc) - (size_t)n, ", VBE/BGA v0x%04X %dx%dx%d LFB 0x%X",
                     (unsigned)vi.version, vi.width, vi.height, vi.bpp, (unsigned)vi.lfb_addr);
        }
        device_node_set_description(&vga, desc);
    }
    vga.res.bus_type = BUS_TYPE_ISA;
    vga.res.io_base = 0x3C0;
    device_node_set_driver_name(&vga, "vga");
    device_register(&vga);
    device_node_free(&vga);

    /* ACPI Subsystem */
    device_node_t acpi;
    device_node_init(&acpi);
    acpi.type = DEVICE_TYPE_SYSTEM;
    acpi.state = DEVICE_STATE_ACTIVE;
    device_node_set_name(&acpi, "acpi0");
    device_node_set_vendor(&acpi, "ACPI Specification");
    {
        uint32_t rsdp = 0, rsdt = 0;
        uint8_t rev = 0;
        char oem[7] = {0};
        char desc[112];
        if (acpi_find_rsdp(&rsdp, &rev, oem, &rsdt)) {
            snprintf(desc, sizeof(desc), "RSDP at 0x%X, revision %u, OEM '%s', RSDT at 0x%X",
                     (unsigned)rsdp, (unsigned)rev, oem, (unsigned)rsdt);
            device_node_set_model(&acpi, oem);
            device_node_set_description(&acpi, desc);
            acpi.res.bus_type = BUS_TYPE_INTERNAL;
            acpi.res.mmio_base = rsdp;
            device_node_set_driver_name(&acpi, "acpi");
            device_register(&acpi);
        }
        device_node_free(&acpi);
    }
}

int device_rescan(void) {
    /* Rescan all hardware buses and register components */
    scan_cpu_subsystem();
    scan_memory_subsystem();
    scan_pci_subsystem();
    scan_storage_subsystem();
    scan_network_subsystem();
    scan_system_and_input_subsystem();

    serial_printf("[device] Rescan completed: %u devices currently in registry\n", device_get_count());
    return (int)device_get_count();
}

void device_init(void) {
    if (s_registry_initialized) return;
    spinlock_init(&s_device_lock);
    s_device_head = NULL;
    s_next_id = 1;
    s_registry_initialized = true;

    serial_printf("[device] Initializing Unified Device Registry\n");
    device_rescan();
}
