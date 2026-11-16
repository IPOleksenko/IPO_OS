#include <memory/memory_map.h>
#include <memory/kmalloc.h>
#include <kernel/smp.h>
#include <stdio.h>
#include <string.h>

static spinlock_t s_memory_map_lock = {0};

static memory_region_t *regions = NULL;
static uint32_t region_count = 0;
static uint32_t region_capacity = 0;
static uint64_t total_usable_ram = 0;

static e820_entry_t *s_raw_e820_entries = NULL;
static uint32_t s_raw_e820_count = 0;
static uint32_t s_raw_e820_capacity = 0;
static const ipo_boot_handover_t *s_boot_handover = NULL;

#define MEMORY_PAGE_SIZE 4096u

typedef struct reusable_frame {
    struct reusable_frame *next;
} reusable_frame_t;

static spinlock_t s_frame_lock = {0};
static uint64_t *s_frame_bitmap = NULL;
static size_t s_frame_bitmap_words = 0u;
static reusable_frame_t *s_free_frames = NULL;

static ipo_paddr_t memory_pointer_ceiling(void) {
    if (sizeof(void *) >= 8u) {
        return UINT64_MAX;
    }
    return 0x100000000ULL;
}

static bool memory_frame_ensure_word(uint64_t word_index) {
    if (word_index < (uint64_t)s_frame_bitmap_words && s_frame_bitmap != NULL) {
        return true;
    }
    size_t old_words = s_frame_bitmap_words;
    size_t new_words = old_words ? old_words : 512u;
    while ((uint64_t)new_words <= word_index) {
        if (new_words > SIZE_MAX / 2u) return false;
        new_words *= 2u;
    }
    if (new_words > SIZE_MAX / sizeof(uint64_t)) return false;
    uint64_t *nb = (uint64_t *)krealloc(s_frame_bitmap, new_words * sizeof(uint64_t));
    if (!nb) return false;
    for (size_t i = old_words; i < new_words; i++) {
        nb[i] = 0ULL;
    }
    s_frame_bitmap = nb;
    s_frame_bitmap_words = new_words;
    return true;
}

static bool memory_frame_is_allocated(uint64_t frame_index) {
    uint64_t word_index = frame_index >> 6;
    if (!s_frame_bitmap || word_index >= (uint64_t)s_frame_bitmap_words) {
        return false;
    }
    return (s_frame_bitmap[word_index] & (1ULL << (frame_index & 63u))) != 0ULL;
}

static bool memory_frame_set_allocated(uint64_t frame_index, bool allocated) {
    uint64_t word_index = frame_index >> 6;
    if (allocated) {
        if (!memory_frame_ensure_word(word_index)) {
            return false;
        }
        s_frame_bitmap[word_index] |= (1ULL << (frame_index & 63u));
        return true;
    }
    if (!s_frame_bitmap || word_index >= (uint64_t)s_frame_bitmap_words) {
        return false;
    }
    s_frame_bitmap[word_index] &= ~(1ULL << (frame_index & 63u));
    return true;
}

static bool memory_map_add_region(ipo_paddr_t base, ipo_uoff_t size, ipo_uoff_t used, bool is_usable) {
    if (region_count >= region_capacity) {
        uint32_t new_cap = (region_capacity == 0u) ? 8u : (region_capacity * 2u);
        memory_region_t *new_buf = (memory_region_t *)krealloc(
            regions, (size_t)new_cap * sizeof(memory_region_t));
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
        uint32_t new_cap = (s_raw_e820_capacity == 0u) ? 16u : (s_raw_e820_capacity * 2u);
        e820_entry_t *new_buf = (e820_entry_t *)krealloc(
            s_raw_e820_entries, (size_t)new_cap * sizeof(e820_entry_t));
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

void memory_map_init_handover(const ipo_boot_handover_t *handover) {
    s_boot_handover = handover;
    memory_map_init();
}

void memory_map_init(void) {
    uint32_t raw_count = 0u;
    uint64_t ptr_val = 0u;
    uint64_t fallback_ram = 0u;
    bool have_e820 = false;

    if (s_boot_handover &&
        s_boot_handover->magic == IPO_BOOT_HANDOVER_SIG_U64 &&
        s_boot_handover->e820_count > 0u &&
        s_boot_handover->e820_table_paddr != 0u) {
        have_e820 = true;
        raw_count = s_boot_handover->e820_count;
        ptr_val = s_boot_handover->e820_table_paddr;
        fallback_ram = s_boot_handover->total_usable_ram;
    } else {
        uint32_t magic = *(volatile uint32_t *)(uintptr_t)E820_MAGIC_ADDR;
        raw_count = *(volatile uint32_t *)(uintptr_t)E820_COUNT_ADDR;
        ptr_val = ((uint64_t)*(volatile uint32_t *)(uintptr_t)(E820_PTR_ADDR + 4)) << 32 |
                  (uint64_t)*(volatile uint32_t *)(uintptr_t)E820_PTR_ADDR;
        fallback_ram = *(volatile uint32_t *)(uintptr_t)E820_RAM_LOW_ADDR;
        if (magic == E820_MAGIC_VAL && raw_count > 0u && ptr_val != 0u) {
            have_e820 = true;
        }
    }

    region_count = 0u;
    total_usable_ram = 0u;
    s_raw_e820_count = 0u;

    const ipo_paddr_t max_ptr_addr = memory_pointer_ceiling();

    if (have_e820) {
        const e820_entry_t *entries = (const e820_entry_t *)(uintptr_t)ptr_val;

        printf("[mem] BIOS E820 Memory Map: %u entries at 0x%llx\n",
               raw_count, (unsigned long long)ptr_val);

        for (uint32_t i = 0u; i < raw_count; i++) {
            memory_map_add_raw_e820(&entries[i]);
            uint64_t base = entries[i].base;
            uint64_t len = entries[i].length;
            uint32_t type = entries[i].type;

            if (len == 0u) continue;

            printf("[mem]   Entry %u: 0x%llx - 0x%llx (%llu KiB) [%s]\n",
                   i, (unsigned long long)base, (unsigned long long)(base + len),
                   (unsigned long long)(len / 1024ull), memory_map_e820_type_name(type));

            if (type == E820_TYPE_USABLE) {
                total_usable_ram += len;

                /* Keep memory above 128 MiB (0x08000000) for the page/DMA pool */
                if (base >= 0x08000000ull && base < max_ptr_addr) {
                    uint64_t usable_len = len;
                    if (usable_len > max_ptr_addr - base) {
                        usable_len = max_ptr_addr - base;
                    }
                    memory_map_add_region(base, usable_len, 0u, true);
                } else if (base < 0x08000000ull && len > 0x08000000ull - base) {
                    ipo_paddr_t start = 0x08000000ull;
                    uint64_t usable_len = (base + len) - 0x08000000ull;
                    if (usable_len > max_ptr_addr - start) {
                        usable_len = max_ptr_addr - start;
                    }
                    memory_map_add_region(start, usable_len, 0u, true);
                }
            }
        }
    } else {
        printf("[mem] Fallback memory detection: %llu bytes\n", (unsigned long long)fallback_ram);
        total_usable_ram = fallback_ram;

        if (total_usable_ram > 0x08000000ull) {
            uint64_t clamped = (total_usable_ram > max_ptr_addr) ? max_ptr_addr : total_usable_ram;
            uint64_t sz = clamped - 0x08000000ull;
            memory_map_add_region(0x08000000ull, sz, 0u, true);
        }
    }

    printf("[mem] Total Usable RAM: %llu MiB (%llu bytes) across %u usable allocatable regions\n",
           (unsigned long long)(total_usable_ram / (1024ull * 1024ull)),
           (unsigned long long)total_usable_ram,
           region_count);
    for (uint32_t r = 0u; r < region_count; r++) {
        serial_printf("[mem]   Pool %u: 0x%llx - 0x%llx (%llu MiB, used/reserved: %llu MiB)\n",
                      r,
                      (unsigned long long)regions[r].base,
                      (unsigned long long)(regions[r].base + regions[r].size),
                      (unsigned long long)(regions[r].size / (1024ull * 1024ull)),
                      (unsigned long long)(regions[r].used / (1024ull * 1024ull)));
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
    if (total_usable_ram == 0u) {
        memory_map_init();
    }
    return total_usable_ram;
}

void *memory_map_alloc(ipo_size_t size, ipo_size_t alignment) {
    if (size == 0u) return NULL;
    if (alignment == 0u) alignment = 16u;

    ipo_reg_t irq_flags = spin_lock_irqsave(&s_memory_map_lock);

    if (region_count == 0u) memory_map_init();

    const ipo_paddr_t max_ptr_addr = memory_pointer_ceiling();

    for (uint32_t i = 0u; i < region_count; i++) {
        if (!regions[i].is_usable) continue;

        ipo_paddr_t reg_base = regions[i].base;
        ipo_uoff_t reg_size = regions[i].size;
        ipo_paddr_t current_addr = reg_base + regions[i].used;
        ipo_paddr_t rem = current_addr % (ipo_paddr_t)alignment;
        ipo_paddr_t aligned_addr = rem ? current_addr + ((ipo_paddr_t)alignment - rem) : current_addr;
        if (aligned_addr < current_addr || aligned_addr >= max_ptr_addr) {
            continue;
        }

        ipo_uoff_t offset = aligned_addr - reg_base;
        if ((ipo_uoff_t)size <= reg_size && offset <= reg_size - (ipo_uoff_t)size) {
            if (max_ptr_addr != UINT64_MAX && aligned_addr + (ipo_uoff_t)size > max_ptr_addr) {
                continue;
            }
            regions[i].used = offset + (ipo_uoff_t)size;
            spin_unlock_irqrestore(&s_memory_map_lock, irq_flags);
            return (void *)(uintptr_t)aligned_addr;
        }
    }

    spin_unlock_irqrestore(&s_memory_map_lock, irq_flags);
    return NULL;
}

void *memory_page_alloc(void) {
    ipo_reg_t irq_flags = spin_lock_irqsave(&s_frame_lock);
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

    uint64_t frame_index = (uint64_t)(uintptr_t)page / MEMORY_PAGE_SIZE;
    if (memory_frame_is_allocated(frame_index)) {
        serial_printf("[mem] page allocator returned an already allocated frame at 0x%llx\n",
                      (unsigned long long)(uintptr_t)page);
        spin_unlock_irqrestore(&s_frame_lock, irq_flags);
        return NULL;
    }
    if (!memory_frame_set_allocated(frame_index, true)) {
        spin_unlock_irqrestore(&s_frame_lock, irq_flags);
        return NULL;
    }
    spin_unlock_irqrestore(&s_frame_lock, irq_flags);

    memset(page, 0, MEMORY_PAGE_SIZE);
    return page;
}

bool memory_page_free(void *page) {
    ipo_vaddr_t address = (ipo_vaddr_t)page;
    if (!page || (address & (MEMORY_PAGE_SIZE - 1u)) != 0u ||
        !memory_map_is_valid_ram((ipo_paddr_t)address, MEMORY_PAGE_SIZE)) {
        return false;
    }

    uint64_t frame_index = (uint64_t)address / MEMORY_PAGE_SIZE;
    ipo_reg_t irq_flags = spin_lock_irqsave(&s_frame_lock);
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

bool memory_map_is_valid_ram(ipo_paddr_t addr, ipo_size_t len) {
    if (len == 0u) return true;
    if (region_count == 0u) memory_map_init();

    const ipo_paddr_t max_ptr_addr = memory_pointer_ceiling();
    ipo_paddr_t req_end = 0u;
    if (!ipo_paddr_add_ok(addr, (ipo_uoff_t)len, &req_end)) {
        return false;
    }
    if (max_ptr_addr != UINT64_MAX && req_end > max_ptr_addr) {
        return false;
    }

    for (uint32_t i = 0u; i < region_count; i++) {
        ipo_paddr_t reg_start = regions[i].base;
        ipo_paddr_t reg_end = regions[i].base + regions[i].size;
        if (addr >= reg_start && req_end <= reg_end) {
            return true;
        }
    }

    if (addr >= 0x10000ull && req_end <= 0x08000000ull) {
        if (req_end > 0x000A0000ull && addr < 0x00100000ull) {
            return false;
        }
        if (s_raw_e820_count == 0u) {
            return true;
        }
        for (uint32_t i = 0u; i < s_raw_e820_count; i++) {
            if (s_raw_e820_entries[i].type != E820_TYPE_USABLE ||
                s_raw_e820_entries[i].length == 0u) {
                continue;
            }
            uint64_t e_start = s_raw_e820_entries[i].base;
            uint64_t e_end = (s_raw_e820_entries[i].length > UINT64_MAX - e_start)
                                 ? UINT64_MAX
                                 : e_start + s_raw_e820_entries[i].length;
            if (addr >= e_start && req_end <= e_end) {
                return true;
            }
        }
    }

    return false;
}
