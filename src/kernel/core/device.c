#include <kernel/device.h>
#include <kernel/driver.h>
#include <kernel/smp.h>
#include <memory/kmalloc.h>
#include <memory/memory_map.h>
#include <driver/net/pci.h>
#include <driver/ata/ata.h>
#include <ioport.h>
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
    char *vendor = smp_get_cpu_vendor_dyn();
    char *brand = smp_get_cpu_brand_dyn();
    char *features = smp_get_cpu_features_str_dyn();

    uint32_t cpu_count = smp_get_cpu_count();
    for (uint32_t i = 0; i < cpu_count; i++) {
        const cpu_info_t *info = smp_get_cpu_info(i);
        if (!info) continue;

        device_node_t dev;
        device_node_init(&dev);

        dev.type = DEVICE_TYPE_CPU;
        dev.state = info->is_online ? DEVICE_STATE_ACTIVE : DEVICE_STATE_INITIALIZED;
        device_node_set_name(&dev, (brand && brand[0]) ? brand : vendor);
        device_node_set_vendor(&dev, vendor);
        device_node_set_model(&dev, brand);
        device_node_set_description(&dev, (features && features[0]) ? features : brand);

        dev.res.bus_type = BUS_TYPE_INTERNAL;
        dev.res.mmio_base = smp_get_lapic_base();

        device_register(&dev);
        device_node_free(&dev);
    }

    if (vendor) kfree(vendor);
    if (brand) kfree(brand);
    if (features) kfree(features);
}

static void scan_memory_subsystem(void) {
    uint32_t region_count = memory_map_get_region_count();

    for (uint32_t i = 0; i < region_count; i++) {
        const memory_region_t *reg = memory_map_get_region(i);
        if (!reg) continue;

        device_node_t dev;
        device_node_init(&dev);
        dev.type = DEVICE_TYPE_MEMORY;
        dev.state = reg->is_usable ? DEVICE_STATE_ACTIVE : DEVICE_STATE_INITIALIZED;

        char *size_str = format_size_dynamic((uint64_t)reg->size);
        device_node_set_name(&dev, size_str);
        device_node_set_description(&dev, size_str);
        if (size_str) kfree(size_str);

        dev.res.bus_type = BUS_TYPE_INTERNAL;
        dev.res.mmio_base = reg->base;
        dev.res.mmio_size = reg->size;
        dev.res.size_bytes = (uint64_t)reg->size;

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

        uint16_t pci_cmd = pci_read_word(pdev->bus, pdev->slot, pdev->func, 0x04);
        dev.state = (pci_cmd & 0x07u) ? DEVICE_STATE_ACTIVE : DEVICE_STATE_INITIALIZED;

        device_node_set_name(&dev, pdev->device_name ? pdev->device_name : pdev->vendor_name);
        device_node_set_vendor(&dev, pdev->vendor_name);
        device_node_set_model(&dev, pdev->device_name);
        device_node_set_description(&dev, pdev->device_name ? pdev->device_name : pdev->vendor_name);

        dev.res.bus_type = BUS_TYPE_PCI;
        dev.res.pci_bus = pdev->bus;
        dev.res.pci_slot = pdev->slot;
        dev.res.pci_func = pdev->func;
        dev.res.vendor_id = pdev->vendor_id;
        dev.res.device_id = pdev->device_id;
        dev.res.pci_class = pdev->class_code;
        dev.res.pci_subclass = pdev->subclass;
        dev.res.pci_progif = pdev->prog_if;
        for (size_t b = 0; b < pdev->bar_count; b++) {
            device_resources_add_bar(&dev.res, pdev->bar[b]);
            if (pdev->bar[b] & 1) {
                if (dev.res.io_base == 0) dev.res.io_base = (uint16_t)(pdev->bar[b] & ~3u);
            } else if (pdev->bar[b] != 0) {
                if (dev.res.mmio_base == 0) dev.res.mmio_base = pdev->bar[b] & ~0xFu;
            }
        }
        dev.res.irq = pdev->irq;

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
        device_node_set_name(&dev, (adev->serial && adev->serial[0]) ? adev->serial : adev->model);
        device_node_set_model(&dev, adev->model);
        device_node_set_description(&dev, (adev->model && adev->model[0]) ? adev->model : adev->firmware);

        dev.res.bus_type = BUS_TYPE_IDE;
        dev.res.io_base = adev->io_base;
        dev.res.irq = adev->irq;
        dev.res.size_bytes = adev->capacity_sectors * (uint64_t)adev->sector_size;

        device_register(&dev);
        device_node_free(&dev);
    }
}

static bool acpi_check_range_for_rsdp(uint32_t start, uint32_t end, uint32_t *out_addr, uint8_t *out_rev, char **out_oem, uint32_t *out_rsdt) {
    for (uint32_t a = start; a + 20 <= end; a += 16) {
        const uint8_t *p = (const uint8_t *)(uintptr_t)a;
        if (memcmp(p, "RSD PTR ", 8) != 0) continue;
        uint8_t sum = 0;
        for (int i = 0; i < 20; i++) sum = (uint8_t)(sum + p[i]);
        if (sum != 0) continue;
        *out_addr = a;
        *out_rev = p[15];
        *out_rsdt = *(const uint32_t *)(p + 16);
        if (out_oem) {
            char *oem = (char *)kmalloc(7);
            if (oem) {
                memcpy(oem, p + 9, 6);
                oem[6] = '\0';
                for (int k = 5; k >= 0 && oem[k] == ' '; k--) oem[k] = '\0';
            }
            *out_oem = oem;
        }
        return true;
    }
    return false;
}

static bool acpi_find_rsdp_dyn(uint32_t *out_addr, uint8_t *out_rev, char **out_oem, uint32_t *out_rsdt) {
    uint32_t ebda = ((uint32_t)*(volatile uint16_t *)0x40E) << 4;
    if (ebda >= 0x80000 && ebda < 0xA0000) {
        if (acpi_check_range_for_rsdp(ebda, ebda + 1024, out_addr, out_rev, out_oem, out_rsdt)) {
            return true;
        }
    }
    return acpi_check_range_for_rsdp(0xE0000, 0x100000, out_addr, out_rev, out_oem, out_rsdt);
}

static char *acpi_copy_trimmed_field(const uint8_t *src, size_t len) {
    while (len > 0 && (src[len - 1] == ' ' || src[len - 1] == '\0')) len--;
    char *out = (char *)kmalloc(len + 1);
    if (!out) return NULL;
    memcpy(out, src, len);
    out[len] = '\0';
    return out;
}

static char *acpi_decode_eisa_id_dyn(uint32_t val) {
    static const char hex[] = "0123456789ABCDEF";
    char *out = (char *)kmalloc(8);
    if (!out) return NULL;
    uint8_t b0 = (uint8_t)(val & 0xFFu);
    uint8_t b1 = (uint8_t)((val >> 8) & 0xFFu);
    uint8_t b2 = (uint8_t)((val >> 16) & 0xFFu);
    uint8_t b3 = (uint8_t)((val >> 24) & 0xFFu);
    out[0] = (char)(((b0 >> 2) & 0x1Fu) + 0x40u);
    out[1] = (char)((((b0 & 0x03u) << 3) | ((b1 >> 5) & 0x07u)) + 0x40u);
    out[2] = (char)((b1 & 0x1Fu) + 0x40u);
    out[3] = hex[(b2 >> 4) & 0x0Fu];
    out[4] = hex[b2 & 0x0Fu];
    out[5] = hex[(b3 >> 4) & 0x0Fu];
    out[6] = hex[b3 & 0x0Fu];
    out[7] = '\0';
    return out;
}

static size_t aml_parse_pkg_length(const uint8_t *buf, size_t max_len, size_t *out_len_bytes) {
    if (max_len == 0) {
        if (out_len_bytes) *out_len_bytes = 0;
        return 0;
    }
    uint8_t lead = buf[0];
    uint8_t byte_cnt = (lead >> 6) & 0x03u;
    if (byte_cnt == 0) {
        if (out_len_bytes) *out_len_bytes = 1;
        return (size_t)(lead & 0x3Fu);
    }
    if ((size_t)(1 + byte_cnt) > max_len) {
        if (out_len_bytes) *out_len_bytes = 1;
        return 0;
    }
    size_t pkg_len = (size_t)(lead & 0x0Fu);
    for (uint8_t i = 0; i < byte_cnt; i++) {
        pkg_len |= ((size_t)buf[1 + i]) << (4 + 8 * i);
    }
    if (out_len_bytes) *out_len_bytes = 1 + byte_cnt;
    return pkg_len;
}

static char *aml_parse_device_name_dyn(const uint8_t *buf, size_t max_len, size_t *out_consumed) {
    size_t pos = 0;
    while (pos < max_len && (buf[pos] == 0x5Cu || buf[pos] == 0x5Eu)) pos++;
    if (pos >= max_len) {
        if (out_consumed) *out_consumed = pos;
        return NULL;
    }
    const uint8_t *seg = NULL;
    if (buf[pos] == 0x00u) {
        pos += 1;
    } else if (buf[pos] == 0x2Eu && pos + 9 <= max_len) {
        seg = buf + pos + 5;
        pos += 9;
    } else if (buf[pos] == 0x2Fu && pos + 2 <= max_len) {
        uint8_t cnt = buf[pos + 1];
        if (cnt > 0 && pos + 2 + (size_t)cnt * 4 <= max_len) {
            seg = buf + pos + 2 + (size_t)(cnt - 1) * 4;
            pos += 2 + (size_t)cnt * 4;
        }
    } else if (pos + 4 <= max_len) {
        seg = buf + pos;
        pos += 4;
    }
    if (out_consumed) *out_consumed = pos;
    if (!seg) return NULL;

    char *name = (char *)kmalloc(5);
    if (!name) return NULL;
    size_t nlen = 0;
    for (size_t i = 0; i < 4; i++) {
        char c = (char)seg[i];
        if (c == '_' && (i == 0 || i == 3)) continue;
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_') {
            name[nlen++] = c;
        }
    }
    name[nlen] = '\0';
    if (nlen == 0) {
        kfree(name);
        return NULL;
    }
    return name;
}

static device_type_t acpi_hid_to_device_type(const char *hid) {
    if (!hid) return DEVICE_TYPE_SYSTEM;
    if (strncmp(hid, "PNP03", 5) == 0 || strncmp(hid, "PNP0F", 5) == 0) {
        return DEVICE_TYPE_INPUT;
    }
    if (strncmp(hid, "PNP07", 5) == 0) {
        return DEVICE_TYPE_STORAGE;
    }
    if (strncmp(hid, "PNP09", 5) == 0) {
        return DEVICE_TYPE_VIDEO;
    }
    if (strncmp(hid, "PNP0A03", 7) == 0 || strncmp(hid, "PNP0A08", 7) == 0) {
        return DEVICE_TYPE_PCI;
    }
    return DEVICE_TYPE_SYSTEM;
}

/* Scan an ACPI DSDT or SSDT AML bytecode table for hardware Device() definitions */
static void scan_acpi_aml_table(const uint8_t *sdt) {
    if (!sdt) return;
    uint32_t sdt_len = *(const uint32_t *)(sdt + 4);
    if (sdt_len < 36 || sdt_len > 0x100000u) return;

    char *table_oem = acpi_copy_trimmed_field(sdt + 10, 6);
    uint16_t last_kbd_io = 0;

    size_t i = 36;
    while (i + 6 < sdt_len) {
        if (sdt[i] == 0x5Bu && sdt[i + 1] == 0x82u) {
            size_t len_bytes = 0;
            size_t pkg_len = aml_parse_pkg_length(sdt + i + 2, sdt_len - (i + 2), &len_bytes);
            if (pkg_len > 0 && len_bytes > 0 && i + 2 + pkg_len <= sdt_len) {
                size_t name_off = i + 2 + len_bytes;
                size_t name_consumed = 0;
                char *dev_name = aml_parse_device_name_dyn(sdt + name_off, sdt_len - name_off, &name_consumed);
                size_t body_start = name_off + name_consumed;
                size_t dev_end = i + 2 + pkg_len;

                size_t own_end = dev_end;
                for (size_t k = body_start; k + 2 < dev_end; k++) {
                    if (sdt[k] == 0x5Bu && sdt[k + 1] == 0x82u) {
                        own_end = k;
                        break;
                    }
                }

                if (dev_name && body_start < own_end) {
                    const uint8_t *body = sdt + body_start;
                    size_t body_len = own_end - body_start;

                    char *hid = NULL;
                    char *uid_str = NULL;
                    bool sta_disabled = false;

                    for (size_t m = 0; m + 5 < body_len; m++) {
                        if (memcmp(body + m, "_HID", 4) == 0 && !hid) {
                            size_t p = m + 4;
                            if (p + 5 <= body_len && body[p] == 0x0Cu) {
                                uint32_t eval = *(const uint32_t *)(body + p + 1);
                                hid = acpi_decode_eisa_id_dyn(eval);
                            } else if (p + 2 <= body_len && body[p] == 0x0Du) {
                                size_t slen = 0;
                                while (p + 1 + slen < body_len && body[p + 1 + slen] != 0) slen++;
                                hid = acpi_copy_trimmed_field(body + p + 1, slen);
                            }
                        } else if (memcmp(body + m, "_UID", 4) == 0 && !uid_str) {
                            size_t p = m + 4;
                            if (p + 2 <= body_len && body[p] == 0x0Du) {
                                size_t slen = 0;
                                while (p + 1 + slen < body_len && body[p + 1 + slen] != 0) slen++;
                                uid_str = acpi_copy_trimmed_field(body + p + 1, slen);
                            }
                        } else if (memcmp(body + m, "_STA", 4) == 0) {
                            if (m >= 1 && body[m - 1] == 0x08u) {
                                size_t p = m + 4;
                                if (p < body_len && body[p] == 0x00u) sta_disabled = true;
                                else if (p + 1 < body_len && body[p] == 0x0Au && (body[p + 1] & 0x03u) == 0) sta_disabled = true;
                            }
                        }
                    }

                    uint16_t io_base = 0;
                    uint16_t io_size = 0;
                    uint32_t mmio_base = 0;
                    uint32_t mmio_size = 0;
                    uint8_t irq = 0xFF;

                    /* Extract hardware resources from _CRS buffer descriptors */
                    for (size_t m = 0; m + 4 < body_len; m++) {
                        if (memcmp(body + m, "_CRS", 4) == 0) {
                            for (size_t r = m + 4; r + 2 < body_len; r++) {
                                if (body[r] == 0x79u && body[r + 1] == 0x00u) break; /* EndTag */
                                if (body[r] == 0x47u && (body[r + 1] == 0x00u || body[r + 1] == 0x01u) && r + 8 <= body_len) {
                                    uint16_t mn = *(const uint16_t *)(body + r + 2);
                                    uint16_t mx = *(const uint16_t *)(body + r + 4);
                                    uint8_t ln = body[r + 7];
                                    if (mn == mx && mn > 0 && ln > 0 && io_base == 0) {
                                        io_base = mn;
                                        io_size = ln;
                                    }
                                    r += 7;
                                } else if (body[r] == 0x4Bu && r + 4 <= body_len) {
                                    uint16_t mn = *(const uint16_t *)(body + r + 1);
                                    uint8_t ln = body[r + 3];
                                    if (mn > 0 && ln > 0 && io_base == 0) {
                                        io_base = mn;
                                        io_size = ln;
                                    }
                                    r += 3;
                                } else if ((body[r] == 0x22u || body[r] == 0x23u) && r + 3 <= body_len) {
                                    uint16_t mask = *(const uint16_t *)(body + r + 1);
                                    for (uint8_t bit = 0; bit < 16; bit++) {
                                        if (mask == (uint16_t)(1u << bit)) {
                                            irq = bit;
                                            break;
                                        }
                                    }
                                    r += (body[r] == 0x23u) ? 3 : 2;
                                } else if (body[r] == 0x2Au && r + 3 <= body_len) {
                                    r += 2;
                                } else if (body[r] == 0x86u && body[r + 1] == 0x09u && r + 12 <= body_len) {
                                    mmio_base = *(const uint32_t *)(body + r + 4);
                                    mmio_size = *(const uint32_t *)(body + r + 8);
                                    r += 11;
                                }
                            }
                            break;
                        }
                    }

                    /* Also check OperationRegion SystemMemory (0x5B 0x80 <NameSeg> 0x00 0x0C <base> 0x0B <len>) */
                    if (mmio_base == 0) {
                        for (size_t m = 0; m + 15 <= body_len; m++) {
                            if (body[m] == 0x5Bu && body[m + 1] == 0x80u &&
                                body[m + 6] == 0x00u && body[m + 7] == 0x0Cu) {
                                mmio_base = *(const uint32_t *)(body + m + 8);
                                if (body[m + 12] == 0x0Bu) {
                                    mmio_size = *(const uint16_t *)(body + m + 13);
                                }
                                break;
                            }
                        }
                    }

                    if (hid && strncmp(hid, "PNP03", 5) == 0 && io_base != 0) {
                        last_kbd_io = io_base;
                    }
                    if (hid && strncmp(hid, "PNP0F", 5) == 0 && io_base == 0 && last_kbd_io != 0) {
                        io_base = last_kbd_io;
                        io_size = 1;
                    }

                    if (!sta_disabled && hid && (io_base != 0 || mmio_base != 0 || irq != 0xFF)) {
                        device_node_t dev;
                        device_node_init(&dev);
                        dev.type = acpi_hid_to_device_type(hid);
                        dev.state = DEVICE_STATE_ACTIVE;
                        device_node_set_name(&dev, dev_name);
                        device_node_set_vendor(&dev, table_oem);
                        device_node_set_model(&dev, hid);
                        device_node_set_description(&dev, (uid_str && uid_str[0]) ? uid_str : hid);

                        dev.res.bus_type = (mmio_base != 0 && io_base == 0) ? BUS_TYPE_INTERNAL : BUS_TYPE_ISA;
                        dev.res.io_base = io_base;
                        dev.res.io_size = io_size;
                        dev.res.mmio_base = mmio_base;
                        dev.res.mmio_size = mmio_size;
                        dev.res.irq = (irq != 0xFF) ? irq : 0;

                        device_register(&dev);
                        device_node_free(&dev);
                    }

                    if (hid) kfree(hid);
                    if (uid_str) kfree(uid_str);
                }
                if (dev_name) kfree(dev_name);
            }
        }
        i++;
    }

    if (table_oem) kfree(table_oem);
}

static void scan_acpi_and_platform_subsystem(void) {
    uint32_t rsdp = 0, rsdt = 0;
    uint8_t rev = 0;
    char *rsdp_oem = NULL;

    if (acpi_find_rsdp_dyn(&rsdp, &rev, &rsdp_oem, &rsdt) && rsdt != 0) {
        const uint8_t *rsdt_hdr = (const uint8_t *)(uintptr_t)rsdt;
        if (memcmp(rsdt_hdr, "RSDT", 4) == 0) {
            uint32_t rsdt_len = *(const uint32_t *)(rsdt_hdr + 4);
            uint32_t entry_count = (rsdt_len >= 36) ? ((rsdt_len - 36) / 4) : 0;
            const uint32_t *entries = (const uint32_t *)(rsdt_hdr + 36);

            for (uint32_t e = 0; e < entry_count; e++) {
                uint32_t tbl_addr = entries[e];
                if (tbl_addr == 0) continue;
                const uint8_t *tbl = (const uint8_t *)(uintptr_t)tbl_addr;
                uint32_t tbl_len = *(const uint32_t *)(tbl + 4);
                if (tbl_len < 36) continue;

                char *tbl_sig = acpi_copy_trimmed_field(tbl + 0, 4);
                char *tbl_oem = acpi_copy_trimmed_field(tbl + 10, 6);
                char *tbl_tid = acpi_copy_trimmed_field(tbl + 16, 8);

                if (memcmp(tbl, "APIC", 4) == 0 && tbl_len >= 44) {
                    uint32_t pos = 44;
                    while (pos + 2 <= tbl_len) {
                        uint8_t rec_type = tbl[pos];
                        uint8_t rec_len = tbl[pos + 1];
                        if (rec_len < 2 || pos + rec_len > tbl_len) break;

                        if (rec_type == 1 && rec_len >= 12) {
                            uint32_t ioapic_addr = *(const uint32_t *)(tbl + pos + 4);

                            device_node_t dev;
                            device_node_init(&dev);
                            dev.type = DEVICE_TYPE_SYSTEM;
                            dev.state = DEVICE_STATE_ACTIVE;
                            device_node_set_name(&dev, tbl_sig);
                            device_node_set_vendor(&dev, tbl_oem);
                            device_node_set_model(&dev, tbl_tid);
                            device_node_set_description(&dev, tbl_tid);

                            dev.res.bus_type = BUS_TYPE_INTERNAL;
                            dev.res.mmio_base = ioapic_addr;
                            device_register(&dev);
                            device_node_free(&dev);
                        }
                        pos += rec_len;
                    }
                } else if (memcmp(tbl, "FACP", 4) == 0 && tbl_len >= 84) {
                    uint32_t dsdt_addr = *(const uint32_t *)(tbl + 40);
                    uint16_t sci_int   = *(const uint16_t *)(tbl + 46);
                    uint32_t pm1a_evt  = *(const uint32_t *)(tbl + 56);

                    device_node_t dev;
                    device_node_init(&dev);
                    dev.type = DEVICE_TYPE_SYSTEM;
                    dev.state = DEVICE_STATE_ACTIVE;
                    device_node_set_name(&dev, tbl_sig);
                    device_node_set_vendor(&dev, tbl_oem);
                    device_node_set_model(&dev, tbl_tid);
                    device_node_set_description(&dev, tbl_tid);

                    dev.res.bus_type = BUS_TYPE_ISA;
                    dev.res.io_base = (uint16_t)pm1a_evt;
                    dev.res.irq = (uint8_t)sci_int;
                    device_register(&dev);
                    device_node_free(&dev);

                    if (dsdt_addr != 0) {
                        scan_acpi_aml_table((const uint8_t *)(uintptr_t)dsdt_addr);
                    }
                } else if (memcmp(tbl, "SSDT", 4) == 0) {
                    scan_acpi_aml_table(tbl);
                }

                if (tbl_sig) kfree(tbl_sig);
                if (tbl_oem) kfree(tbl_oem);
                if (tbl_tid) kfree(tbl_tid);
            }
        }
    }
    if (rsdp_oem) kfree(rsdp_oem);

    driver_t *drv = driver_get_list();
    while (drv) {
        if (drv->name && drv->name[0] && !device_find_by_name(drv->name)) {
            device_node_t dev;
            device_node_init(&dev);
            dev.type = DEVICE_TYPE_SYSTEM;
            dev.state = (drv->flags & DRIVER_FLAG_ACTIVE) ? DEVICE_STATE_ACTIVE : DEVICE_STATE_INITIALIZED;
            device_node_set_name(&dev, drv->name);
            device_node_set_description(&dev, drv->description);
            dev.res.bus_type = BUS_TYPE_INTERNAL;
            device_node_set_driver_name(&dev, drv->name);
            device_register(&dev);
            device_node_free(&dev);
        }
        drv = drv->next;
    }
}

static void device_registry_clear(void) {
    spin_lock(&s_device_lock);
    device_node_t *curr = s_device_head;
    while (curr) {
        device_node_t *next = curr->next;
        device_node_free(curr);
        kfree(curr);
        curr = next;
    }
    s_device_head = NULL;
    s_next_id = 1;
    spin_unlock(&s_device_lock);
}

int device_rescan(void) {
    device_registry_clear();

    /* Rescan all hardware buses and register components */
    scan_cpu_subsystem();
    scan_memory_subsystem();
    scan_pci_subsystem();
    scan_storage_subsystem();
    scan_acpi_and_platform_subsystem();

    serial_printf("[device] Rescan completed: %u devices currently in registry\n", device_get_count());
    return (int)device_get_count();
}

void device_init(void) {
    if (s_registry_initialized) return;
    spinlock_init(&s_device_lock);
    s_device_head = NULL;
    s_next_id = 1;
    s_registry_initialized = true;

    printf("[device] Initializing Unified Device Registry\n");
    device_rescan();
}

