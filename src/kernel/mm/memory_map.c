#include <memory/memory_map.h>
#include <kernel/smp.h>
#include <stdio.h>
#include <string.h>

static spinlock_t s_memory_map_lock = {0};

#include <memory/kmalloc.h>

static memory_region_t *regions = NULL;
static uint32_t region_count = 0;
static uint32_t region_capacity = 0;
static uint64_t total_usable_ram = 0;

static e820_entry_t *s_raw_e820_entries = NULL;
static uint32_t s_raw_e820_count = 0;
static uint32_t s_raw_e820_capacity = 0;

static bool memory_map_add_region(uint32_t base, uint32_t size, uint32_t used, bool is_usable) {
    if (region_count >= region_capacity) {
        uint32_t new_cap = (region_capacity == 0) ? 8 : (region_capacity * 2);
        memory_region_t *new_buf = (memory_region_t *)krealloc(regions, new_cap * sizeof(memory_region_t));
        if (!new_buf) return false;
        regions = new_buf;
        region_capacity = new_cap;
    }
    regions[region_count].base = base;
    regions[region_count].size = size;
    regions[region_count].used = used;
    regions[region_count].is_usable = is_usable;
    region_count++;
    return true;
}

static bool memory_map_add_raw_e820(const e820_entry_t *entry) {
    if (!entry) return false;
    if (s_raw_e820_count >= s_raw_e820_capacity) {
        uint32_t new_cap = (s_raw_e820_capacity == 0) ? 16 : (s_raw_e820_capacity * 2);
        e820_entry_t *new_buf = (e820_entry_t *)krealloc(s_raw_e820_entries, new_cap * sizeof(e820_entry_t));
        if (!new_buf) return false;
        s_raw_e820_entries = new_buf;
        s_raw_e820_capacity = new_cap;
    }
    s_raw_e820_entries[s_raw_e820_count++] = *entry;
    return true;
}

const char *memory_map_e820_type_name(uint32_t type) {
    switch (type) {
        case E820_TYPE_USABLE:       return "Usable RAM";
        case E820_TYPE_RESERVED:     return "Reserved";
        case E820_TYPE_ACPI_RECLAIM: return "ACPI Reclaim";
        case E820_TYPE_ACPI_NVS:     return "ACPI NVS";
        case E820_TYPE_BAD:          return "Bad Memory";
        default:                     return "Unknown";
    }
}

uint32_t memory_map_get_raw_e820_count(void) {
    return s_raw_e820_count;
}

const e820_entry_t *memory_map_get_raw_e820_entry(uint32_t index) {
    if (index >= s_raw_e820_count) return NULL;
    return &s_raw_e820_entries[index];
}

void memory_map_init(void) {
    uint32_t magic = *(volatile uint32_t *)E820_MAGIC_ADDR;
    uint32_t raw_count = *(volatile uint32_t *)E820_COUNT_ADDR;
    uint32_t ptr_val = *(volatile uint32_t *)E820_PTR_ADDR;
    uint32_t fallback_ram = *(volatile uint32_t *)E820_RAM_LOW_ADDR;

    region_count = 0;
    total_usable_ram = 0;
    s_raw_e820_count = 0;

    if (magic == E820_MAGIC_VAL && raw_count > 0 && ptr_val != 0) {
        const e820_entry_t *entries = (const e820_entry_t *)(uintptr_t)ptr_val;

        printf("[mem] BIOS E820 Memory Map: %u entries at 0x%x\n", raw_count, ptr_val);

        for (uint32_t i = 0; i < raw_count; i++) {
            memory_map_add_raw_e820(&entries[i]);
            uint64_t base = entries[i].base;
            uint64_t len = entries[i].length;
            uint32_t type = entries[i].type;

            if (len == 0) continue;

            printf("[mem]   Entry %u: 0x%llx - 0x%llx (%llu KB) [%s]\n",
                          i, base, base + len, len / 1024ull, memory_map_e820_type_name(type));

            if (type == E820_TYPE_USABLE) {
                total_usable_ram += len;

                /* Filter usable memory for dynamic allocator:
                 * Skip conventional low memory (< 1MB) and kernel static reserved space (1MB - 16MB).
                 * Only register usable regions above 16MB (0x1000000) for general dynamic allocation.
                 * The first 64 MB (0x1000000 - 0x5000000) is reserved for the kernel kmalloc heap,
                 * so regions[0].used is initialized to 64 MB to prevent collision with kmalloc. */
                if (base >= 0x1000000ull && base < 0xFFFFFFFFull) {
                    uint64_t usable_len = len;
                    if (base + usable_len > 0xFFFFFFFFull) {
                        usable_len = 0xFFFFFFFFull - base;
                    }

                    uint32_t used = (base == 0x1000000ull && usable_len > 0x4000000ull) ? 0x4000000u : 0;
                    memory_map_add_region((uint32_t)base, (uint32_t)usable_len, used, true);
                } else if (base < 0x1000000ull && (base + len) > 0x1000000ull) {
                    uint32_t start = 0x1000000u;
                    uint64_t usable_len = (base + len) - 0x1000000ull;
                    if ((uint64_t)start + usable_len > 0xFFFFFFFFull) {
                        usable_len = 0xFFFFFFFFull - (uint64_t)start;
                    }

                    uint32_t used = (usable_len > 0x4000000ull) ? 0x4000000u : 0;
                    memory_map_add_region(start, (uint32_t)usable_len, used, true);
                }
            }
        }
    } else {
        /* Fallback if BIOS E820 not returned */
        printf("[mem] Fallback memory detection: %u bytes\n", fallback_ram);
        total_usable_ram = fallback_ram;

        if (total_usable_ram > 0x1000000ull) {
            uint32_t sz = (uint32_t)(total_usable_ram - 0x1000000ull);
            uint32_t used = (sz > 0x1000000u) ? 0x1000000u : 0;
            memory_map_add_region(0x1000000u, sz, used, true);
        }
    }

    printf("[mem] Total Usable RAM: %llu MB (%llu bytes) across %u usable allocatable regions\n",
                  total_usable_ram / (1024ull * 1024ull), total_usable_ram, region_count);
    for (uint32_t r = 0; r < region_count; r++) {
        serial_printf("[mem]   Pool %u: 0x%x - 0x%x (%u MB, used/reserved: %u MB)\n",
                      r, regions[r].base, regions[r].base + regions[r].size,
                      regions[r].size / (1024u * 1024u),
                      regions[r].used / (1024u * 1024u));
    }
}

uint32_t memory_map_get_region_count(void) {
    return region_count;
}

const memory_region_t *memory_map_get_region(uint32_t index) {
    if (index >= region_count) return NULL;
    return &regions[index];
}

uint64_t memory_map_get_total_ram(void) {
    if (total_usable_ram == 0) {
        memory_map_init();
    }
    return total_usable_ram;
}

void *memory_map_alloc(size_t size, size_t alignment) {
    if (size == 0) return NULL;
    if (alignment == 0) alignment = 16;

    spin_lock(&s_memory_map_lock);

    if (region_count == 0) memory_map_init();

    /* Try to allocate from each available usable region */
    for (uint32_t i = 0; i < region_count; i++) {
        if (!regions[i].is_usable) continue;

        uint32_t current_addr = regions[i].base + regions[i].used;
        uint32_t aligned_addr = (current_addr + (alignment - 1)) & ~(alignment - 1);
        uint32_t needed = (aligned_addr - regions[i].base) + size;

        if (needed <= regions[i].size) {
            regions[i].used = needed;
            spin_unlock(&s_memory_map_lock);
            return (void *)(uintptr_t)aligned_addr;
        }
    }

    spin_unlock(&s_memory_map_lock);
    return NULL;
}

bool memory_map_is_valid_ram(uint32_t addr, uint32_t len) {
    if (len == 0) return true;
    if (region_count == 0) memory_map_init();

    uint64_t req_start = addr;
    uint64_t req_end = (uint64_t)addr + len;

    /* Check if requested range falls within any known usable region */
    for (uint32_t i = 0; i < region_count; i++) {
        uint64_t reg_start = regions[i].base;
        uint64_t reg_end = (uint64_t)regions[i].base + regions[i].size;

        if (req_start >= reg_start && req_end <= reg_end) {
            return true;
        }
    }

    /* Check if in low conventional RAM (e.g. process heap or kernel text) */
    if (req_start >= 0x10000 && req_end <= 0x1000000) {
        return true;
    }

    return false;
}

