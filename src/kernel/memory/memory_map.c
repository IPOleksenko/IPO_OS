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

#define MEMORY_PAGE_SIZE 4096u
#define MEMORY_FRAME_COUNT (0x100000000ull / MEMORY_PAGE_SIZE)
#define MEMORY_FRAME_BITMAP_WORDS (MEMORY_FRAME_COUNT / 32u)

typedef struct reusable_frame {
    struct reusable_frame *next;
} reusable_frame_t;

static spinlock_t s_frame_lock = {0};
static uint32_t s_frame_bitmap[MEMORY_FRAME_BITMAP_WORDS];
static reusable_frame_t *s_free_frames;

static bool memory_frame_is_allocated(uint32_t frame_index) {
    return (s_frame_bitmap[frame_index >> 5] &
            (1u << (frame_index & 31u))) != 0u;
}

static void memory_frame_set_allocated(uint32_t frame_index, bool allocated) {
    uint32_t mask = 1u << (frame_index & 31u);
    if (allocated) {
        s_frame_bitmap[frame_index >> 5] |= mask;
    } else {
        s_frame_bitmap[frame_index >> 5] &= ~mask;
    }
}

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

            printf("[mem]   Entry %u: 0x%llx - 0x%llx (%llu KiB) [%s]\n",
                          i, base, base + len, len / 1024ull, memory_map_e820_type_name(type));

            if (type == E820_TYPE_USABLE) {
                total_usable_ram += len;

                /* Filter usable memory for dynamic allocator above 128 MiB (0x08000000). */
                if (base >= 0x08000000ull && base < 0x100000000ull) {
                    uint64_t usable_len = len;
                    if (usable_len > 0x100000000ull - base) {
                        usable_len = 0x100000000ull - base;
                    }

                    memory_map_add_region((uint32_t)base, (uint32_t)usable_len, 0, true);
                } else if (base < 0x08000000ull && len > 0x08000000ull - base) {
                    uint32_t start = 0x08000000u;
                    uint64_t usable_len = (base + len) - 0x08000000ull;
                    if (usable_len > 0x100000000ull - (uint64_t)start) {
                        usable_len = 0x100000000ull - (uint64_t)start;
                    }

                    memory_map_add_region(start, (uint32_t)usable_len, 0, true);
                }
            }
        }
    } else {
        /* Fallback if BIOS E820 not returned */
        printf("[mem] Fallback memory detection: %u bytes\n", fallback_ram);
        total_usable_ram = fallback_ram;

        if (total_usable_ram > 0x08000000ull) {
            uint64_t clamped = total_usable_ram > 0x100000000ull ? 0x100000000ull : total_usable_ram;
            uint32_t sz = (uint32_t)(clamped - 0x08000000ull);
            memory_map_add_region(0x08000000u, sz, 0, true);
        }
    }

    printf("[mem] Total Usable RAM: %llu MiB (%llu bytes) across %u usable allocatable regions\n",
                  total_usable_ram / (1024ull * 1024ull), total_usable_ram, region_count);
    for (uint32_t r = 0; r < region_count; r++) {
        serial_printf("[mem]   Pool %u: 0x%x - 0x%x (%u MiB, used/reserved: %u MiB)\n",
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

    uint32_t irq_flags = spin_lock_irqsave(&s_memory_map_lock);

    if (region_count == 0) memory_map_init();

    for (uint32_t i = 0; i < region_count; i++) {
        if (!regions[i].is_usable) continue;

        uint64_t reg_base = regions[i].base;
        uint64_t reg_size = regions[i].size;
        uint64_t current_addr = reg_base + regions[i].used;
        uint64_t rem = current_addr % (uint64_t)alignment;
        uint64_t aligned_addr = rem ? current_addr + ((uint64_t)alignment - rem) : current_addr;
        if (aligned_addr < current_addr || aligned_addr > 0x100000000ull) {
            continue;
        }

        uint64_t offset = aligned_addr - reg_base;
        if (size <= reg_size && offset <= reg_size - size) {
            regions[i].used = (uint32_t)(offset + size);
            spin_unlock_irqrestore(&s_memory_map_lock, irq_flags);
            return (void *)(uintptr_t)aligned_addr;
        }
    }

    spin_unlock_irqrestore(&s_memory_map_lock, irq_flags);
    return NULL;
}

void *memory_page_alloc(void) {
    uint32_t irq_flags = spin_lock_irqsave(&s_frame_lock);
    void *page;
    if (s_free_frames) {
        reusable_frame_t *frame = s_free_frames;
        s_free_frames = frame->next;
        page = frame;
    } else {
        page = memory_map_alloc(MEMORY_PAGE_SIZE, MEMORY_PAGE_SIZE);
    }

    if (!page) {
        spin_unlock_irqrestore(&s_frame_lock, irq_flags);
        return NULL;
    }

    uint32_t frame_index = (uint32_t)(uintptr_t)page / MEMORY_PAGE_SIZE;
    if (memory_frame_is_allocated(frame_index)) {
        serial_printf("[mem] page allocator returned an already allocated frame at 0x%x\n",
                      (uint32_t)(uintptr_t)page);
        spin_unlock_irqrestore(&s_frame_lock, irq_flags);
        return NULL;
    }
    memory_frame_set_allocated(frame_index, true);
    spin_unlock_irqrestore(&s_frame_lock, irq_flags);

    memset(page, 0, MEMORY_PAGE_SIZE);
    return page;
}

bool memory_page_free(void *page) {
    uintptr_t address = (uintptr_t)page;
    if (!page || (address & (MEMORY_PAGE_SIZE - 1u)) != 0u ||
        !memory_map_is_valid_ram((uint32_t)address, MEMORY_PAGE_SIZE)) {
        return false;
    }

    uint32_t frame_index = (uint32_t)address / MEMORY_PAGE_SIZE;
    uint32_t irq_flags = spin_lock_irqsave(&s_frame_lock);
    if (!memory_frame_is_allocated(frame_index)) {
        spin_unlock_irqrestore(&s_frame_lock, irq_flags);
        return false;
    }

    reusable_frame_t *frame = (reusable_frame_t *)page;
    frame->next = s_free_frames;
    s_free_frames = frame;
    memory_frame_set_allocated(frame_index, false);
    spin_unlock_irqrestore(&s_frame_lock, irq_flags);
    return true;
}

bool memory_map_is_valid_ram(uint32_t addr, uint32_t len) {
    if (len == 0) return true;
    if (region_count == 0) memory_map_init();

    uint64_t req_start = addr;
    uint64_t req_end = (uint64_t)addr + len;
    if (req_end > 0x100000000ull) {
        return false;
    }

    for (uint32_t i = 0; i < region_count; i++) {
        uint64_t reg_start = regions[i].base;
        uint64_t reg_end = (uint64_t)regions[i].base + regions[i].size;

        if (req_start >= reg_start && req_end <= reg_end) {
            return true;
        }
    }

    if (req_start >= 0x10000 && req_end <= 0x08000000) {
        if (req_end > 0x000A0000u && req_start < 0x00100000u) {
            return false;
        }
        if (s_raw_e820_count == 0) {
            return true;
        }
        for (uint32_t i = 0; i < s_raw_e820_count; i++) {
            if (s_raw_e820_entries[i].type != E820_TYPE_USABLE ||
                s_raw_e820_entries[i].length == 0) {
                continue;
            }
            uint64_t e_start = s_raw_e820_entries[i].base;
            uint64_t e_end = (s_raw_e820_entries[i].length > UINT64_MAX - e_start)
                                 ? UINT64_MAX
                                 : e_start + s_raw_e820_entries[i].length;
            if (req_start >= e_start && req_end <= e_end) {
                return true;
            }
        }
    }

    return false;
}
