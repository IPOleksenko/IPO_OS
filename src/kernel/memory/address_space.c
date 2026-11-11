#include <memory/address_space.h>
#include <memory/memory_map.h>
#include <memory/kmalloc.h>
#include <kernel/smp.h>
#include <string.h>

#define PAGE_SIZE       4096u
#define PAGE_MASK       0xFFFFF000u
#define PDE_PRESENT     0x001u
#define PDE_WRITABLE    0x002u
#define PDE_USER        0x004u
#define PDE_PAGE_SIZE   0x080u
#define PDE_ADDRESS     0xFFFFF000u
#define PDE_LARGE_BASE  0xFFC00000u
#define PTE_PRESENT     0x001u
#define PTE_WRITABLE    0x002u
#define PTE_USER        0x004u
#define PTE_PWT         0x008u
#define PTE_PCD         0x010u
#define PTE_ACCESSED    0x020u
#define PTE_DIRTY       0x040u
#define PTE_PAT         0x080u
#define PTE_GLOBAL      0x100u
#define PDE_LARGE_PAT   0x1000u

struct address_space {
    uint32_t *page_directory;
    uint32_t **owned_page_tables;
};

typedef struct reusable_page {
    struct reusable_page *next;
} reusable_page_t;

static spinlock_t s_address_space_lock = {0};
static reusable_page_t *s_reusable_pages;
static uint32_t *s_kernel_page_directory;

static uint32_t address_space_lock(void) {
    uint32_t flags;
    __asm__ volatile("pushfl; popl %0; cli" : "=r"(flags) : : "memory");
    spin_lock(&s_address_space_lock);
    return flags;
}

static void address_space_unlock(uint32_t flags) {
    spin_unlock(&s_address_space_lock);
    __asm__ volatile("pushl %0; popfl" : : "r"(flags) : "memory", "cc");
}

static uint32_t read_cr0(void) {
    uint32_t value;
    __asm__ volatile("movl %%cr0, %0" : "=r"(value));
    return value;
}

static uint32_t read_cr3(void) {
    uint32_t value;
    __asm__ volatile("movl %%cr3, %0" : "=r"(value));
    return value;
}

static void flush_active_address_space(const address_space_t *space) {
    if (space && (read_cr3() & PAGE_MASK) ==
                     ((uint32_t)(uintptr_t)space->page_directory & PAGE_MASK)) {
        uint32_t directory = (uint32_t)(uintptr_t)space->page_directory;
        __asm__ volatile("movl %0, %%cr3" : : "r"(directory) : "memory");
    }
}

static uint32_t *allocate_page(void) {
    reusable_page_t *page = s_reusable_pages;
    if (page) {
        s_reusable_pages = page->next;
        memset(page, 0, PAGE_SIZE);
        return (uint32_t *)page;
    }
    return (uint32_t *)memory_map_alloc(PAGE_SIZE, PAGE_SIZE);
}

static void release_page(void *address) {
    reusable_page_t *page = (reusable_page_t *)address;
    page->next = s_reusable_pages;
    s_reusable_pages = page;
}

static uint32_t large_page_to_pte_flags(uint32_t pde) {
    uint32_t flags = pde & (PTE_PRESENT | PTE_WRITABLE | PTE_USER |
                            PTE_PWT | PTE_PCD | PTE_ACCESSED |
                            PTE_DIRTY | PTE_GLOBAL);
    if (pde & PDE_LARGE_PAT) flags |= PTE_PAT;
    return flags;
}

static uint32_t *make_private_page_table(address_space_t *space,
                                         uint32_t directory_index) {
    uint32_t pde = space->page_directory[directory_index];
    uint32_t *table = allocate_page();
    if (!table) return NULL;

    if (pde & PDE_PRESENT) {
        if (pde & PDE_PAGE_SIZE) {
            uint32_t physical_base = pde & PDE_LARGE_BASE;
            uint32_t flags = large_page_to_pte_flags(pde);
            for (uint32_t i = 0; i < 1024; i++) {
                table[i] = physical_base + i * PAGE_SIZE | flags;
            }
        } else {
            const uint32_t *shared = (const uint32_t *)(uintptr_t)(pde & PDE_ADDRESS);
            memcpy(table, shared, PAGE_SIZE);
        }
    }

    space->owned_page_tables[directory_index] = table;
    space->page_directory[directory_index] =
        ((uint32_t)(uintptr_t)table & PDE_ADDRESS) |
        PDE_PRESENT | PDE_WRITABLE |
        ((pde & PDE_USER) ? PDE_USER : 0);
    return table;
}

address_space_t *address_space_create(void) {
    if (!(read_cr0() & 0x80000000u)) return NULL;

    uint32_t irq_flags = address_space_lock();
    uint32_t active_directory = read_cr3() & PAGE_MASK;
    if (!s_kernel_page_directory) {
        s_kernel_page_directory = (uint32_t *)(uintptr_t)active_directory;
    }

    address_space_t *space = (address_space_t *)kmalloc(sizeof(*space));
    if (!space) {
        address_space_unlock(irq_flags);
        return NULL;
    }

    space->owned_page_tables = (uint32_t **)kmalloc(1024u * sizeof(uint32_t *));
    space->page_directory = allocate_page();
    if (!space->owned_page_tables || !space->page_directory) {
        if (space->page_directory) release_page(space->page_directory);
        if (space->owned_page_tables) kfree(space->owned_page_tables);
        kfree(space);
        address_space_unlock(irq_flags);
        return NULL;
    }

    memset(space->owned_page_tables, 0, 1024u * sizeof(uint32_t *));
    memcpy(space->page_directory, s_kernel_page_directory, PAGE_SIZE);
    address_space_unlock(irq_flags);
    return space;
}

bool address_space_destroy(address_space_t *space) {
    if (!space || !space->page_directory || !space->owned_page_tables) return false;

    uint32_t irq_flags = address_space_lock();
    if ((read_cr3() & PAGE_MASK) ==
        ((uint32_t)(uintptr_t)space->page_directory & PAGE_MASK)) {
        address_space_unlock(irq_flags);
        return false;
    }

    for (uint32_t i = 0; i < 1024; i++) {
        if (space->owned_page_tables[i]) {
            release_page(space->owned_page_tables[i]);
        }
    }
    release_page(space->page_directory);
    kfree(space->owned_page_tables);
    kfree(space);
    address_space_unlock(irq_flags);
    return true;
}

bool address_space_activate(address_space_t *space) {
    if (!space || !space->page_directory) return false;
    uint32_t directory = (uint32_t)(uintptr_t)space->page_directory;
    if (directory & (PAGE_SIZE - 1u)) return false;

    uint32_t irq_flags = address_space_lock();
    __asm__ volatile("movl %0, %%cr3" : : "r"(directory) : "memory");
    address_space_unlock(irq_flags);
    return true;
}

bool address_space_activate_kernel(void) {
    uint32_t irq_flags = address_space_lock();
    if (!s_kernel_page_directory) {
        address_space_unlock(irq_flags);
        return false;
    }

    uint32_t directory = (uint32_t)(uintptr_t)s_kernel_page_directory;
    __asm__ volatile("movl %0, %%cr3" : : "r"(directory) : "memory");
    address_space_unlock(irq_flags);
    return true;
}

bool address_space_map(address_space_t *space, uint32_t virtual_address,
                       uint32_t physical_address, uint32_t flags) {
    if (!space || !space->page_directory ||
        (virtual_address & (PAGE_SIZE - 1u)) ||
        (physical_address & (PAGE_SIZE - 1u))) {
        return false;
    }

    uint32_t irq_flags = address_space_lock();
    uint32_t directory_index = virtual_address >> 22;
    uint32_t table_index = (virtual_address >> 12) & 0x3FFu;
    uint32_t *table = space->owned_page_tables[directory_index];
    if (!table) {
        table = make_private_page_table(space, directory_index);
    }
    if (!table) {
        address_space_unlock(irq_flags);
        return false;
    }

    uint32_t pte_flags = (flags & 0x1FFu) | PTE_PRESENT;
    table[table_index] = (physical_address & PAGE_MASK) | pte_flags;
    if (pte_flags & PTE_USER) {
        space->page_directory[directory_index] |= PDE_USER;
    }
    flush_active_address_space(space);
    address_space_unlock(irq_flags);
    return true;
}

bool address_space_unmap(address_space_t *space, uint32_t virtual_address) {
    if (!space || !space->page_directory ||
        (virtual_address & (PAGE_SIZE - 1u))) {
        return false;
    }

    uint32_t irq_flags = address_space_lock();
    uint32_t directory_index = virtual_address >> 22;
    uint32_t table_index = (virtual_address >> 12) & 0x3FFu;
    if (!(space->page_directory[directory_index] & PDE_PRESENT)) {
        address_space_unlock(irq_flags);
        return false;
    }

    uint32_t *table = space->owned_page_tables[directory_index];
    if (!table) {
        table = make_private_page_table(space, directory_index);
    }
    if (!table) {
        address_space_unlock(irq_flags);
        return false;
    }

    if (!(table[table_index] & PTE_PRESENT)) {
        address_space_unlock(irq_flags);
        return false;
    }

    table[table_index] = 0;
    bool all_empty = true;
    for (uint32_t i = 0; i < 1024; i++) {
        if (table[i] & PTE_PRESENT) {
            all_empty = false;
            break;
        }
    }

    if (all_empty) {
        space->owned_page_tables[directory_index] = NULL;
        space->page_directory[directory_index] = 0;
        release_page(table);
    }

    flush_active_address_space(space);
    address_space_unlock(irq_flags);
    return true;
}

bool address_space_set_identity_range(address_space_t *space,
                                      uint32_t virtual_address,
                                      uint32_t length, bool mapped) {
    if (!space || !space->page_directory ||
        (virtual_address & (PAGE_SIZE - 1u)) ||
        (length & (PAGE_SIZE - 1u)) ||
        length == 0u) {
        return false;
    }

    uint64_t end = (uint64_t)virtual_address + length;
    if (end > 0x100000000ULL) {
        return false;
    }

    uint32_t irq_flags = address_space_lock();
    uint32_t first_dir = virtual_address >> 22;
    uint32_t last_dir = (uint32_t)((end - 1u) >> 22);

    for (uint32_t dir_idx = first_dir; dir_idx <= last_dir; dir_idx++) {
        uint64_t chunk_start = (uint64_t)dir_idx << 22;
        uint64_t chunk_end = chunk_start + 0x400000ULL;
        bool fully_covered = (chunk_start >= virtual_address && chunk_end <= end);

        if (dir_idx > 3u && fully_covered) {
            continue;
        }

        if (!space->owned_page_tables[dir_idx]) {
            if (!make_private_page_table(space, dir_idx)) {
                address_space_unlock(irq_flags);
                return false;
            }
        }
    }

    for (uint32_t dir_idx = first_dir; dir_idx <= last_dir; dir_idx++) {
        uint64_t chunk_start = (uint64_t)dir_idx << 22;
        uint64_t chunk_end = chunk_start + 0x400000ULL;

        uint64_t eff_start = (chunk_start > (uint64_t)virtual_address) ? chunk_start : (uint64_t)virtual_address;
        uint64_t eff_end = (chunk_end < end) ? chunk_end : end;

        bool fully_covered = (eff_start == chunk_start && eff_end == chunk_end);

        if (dir_idx > 3u && fully_covered) {
            uint32_t *table = space->owned_page_tables[dir_idx];
            if (table) {
                release_page(table);
                space->owned_page_tables[dir_idx] = NULL;
            }
            space->page_directory[dir_idx] = mapped ? ((dir_idx << 22) | (PDE_PRESENT | PDE_WRITABLE | PDE_PAGE_SIZE)) : 0u;
        } else {
            uint32_t *table = space->owned_page_tables[dir_idx];
            for (uint64_t page = eff_start; page < eff_end; page += PAGE_SIZE) {
                uint32_t table_index = (page >> 12) & 0x3FFu;
                table[table_index] = mapped ? ((uint32_t)page | (PTE_PRESENT | PTE_WRITABLE)) : 0u;
            }

            if (!mapped) {
                bool all_zero = true;
                for (uint32_t i = 0u; i < 1024u; i++) {
                    if (table[i] & PTE_PRESENT) {
                        all_zero = false;
                        break;
                    }
                }
                if (all_zero) {
                    space->owned_page_tables[dir_idx] = NULL;
                    space->page_directory[dir_idx] = 0u;
                    release_page(table);
                }
            }
        }
    }

    flush_active_address_space(space);
    address_space_unlock(irq_flags);
    return true;
}

bool address_space_range_mapped(address_space_t *space,
                                uint32_t virtual_address, uint32_t length,
                                bool mapped) {
    if (!space || !space->page_directory || length == 0u) {
        return false;
    }

    uint64_t end = (uint64_t)virtual_address + length;
    if (end > 0x100000000ULL) {
        return false;
    }

    uint32_t irq_flags = address_space_lock();
    for (uint64_t page = virtual_address; page < end; page += PAGE_SIZE) {
        uint32_t vaddr = (uint32_t)page;
        uint32_t dir_index = vaddr >> 22;
        uint32_t pde = space->page_directory[dir_index];
        bool is_mapped = (pde & PDE_PRESENT) != 0;
        if (is_mapped && !(pde & PDE_PAGE_SIZE)) {
            const uint32_t *table = (const uint32_t *)(uintptr_t)(pde & PDE_ADDRESS);
            uint32_t pte = table[(vaddr >> 12) & 0x3FFu];
            is_mapped = (pte & PTE_PRESENT) != 0;
        }

        if (is_mapped != mapped) {
            address_space_unlock(irq_flags);
            return false;
        }
    }

    address_space_unlock(irq_flags);
    return true;
}
