#include <memory/address_space.h>
#include <memory/memory_map.h>
#include <memory/kmalloc.h>
#include <arch/cpu_regs.h>
#include <arch/context.h>
#include <kernel/smp.h>
#include <syscall.h>
#include <stdio.h>
#include <string.h>

#define PAGE_SIZE         4096ULL
#define PAGE_MASK_32      0xFFFFF000u
#define PAGE_MASK_64      0x000FFFFFFFFFF000ULL
#define IA32_EFER_MSR     0xC0000080u
#define IA32_EFER_LMA     (1ULL << 10)
#define X86_CR4_PSE       (1ULL << 4)
#define X86_CR4_PAE       (1ULL << 5)
#define X86_CR4_LA57      (1ULL << 12)

#define PDE_PRESENT       0x001u
#define PDE_WRITABLE      0x002u
#define PDE_USER          0x004u
#define PDE_PAGE_SIZE     0x080u
#define PDE_ADDRESS       0xFFFFF000u
#define PDE_LARGE_BASE    0xFFC00000u
#define PTE_PRESENT       0x001u
#define PTE_WRITABLE      0x002u
#define PTE_USER          0x004u
#define PTE_PWT           0x008u
#define PTE_PCD           0x010u
#define PTE_ACCESSED      0x020u
#define PTE_DIRTY         0x040u
#define PTE_PAT           0x080u
#define PTE_GLOBAL        0x100u
#define PDE_LARGE_PAT     0x1000u

#define ADDRESS_SPACE_TRAMP_BASE        0xC0000000ULL
#define ADDRESS_SPACE_TRAMP_AUDIO_TOP   0xC8000000ULL
#define ADDRESS_SPACE_TRAMP_COPY_BASE   0xC8000000ULL
#define ADDRESS_SPACE_TRAMP_COPY_TOP    0xD0000000ULL
#define ADDRESS_SPACE_TRAMP_SLOTS_32    ((ADDRESS_SPACE_TRAMP_COPY_TOP - ADDRESS_SPACE_TRAMP_BASE) >> 22)
#define ADDRESS_SPACE_TRAMP_SLOTS_64    ((ADDRESS_SPACE_TRAMP_COPY_TOP - ADDRESS_SPACE_TRAMP_BASE) >> 21)

struct address_space {
    ipo_paddr_t root_paddr;
    uint8_t     paging_levels; /* 2 = 32-bit PDE/PTE, 4 = 4-level PML4, 5 = 5-level LA57 */
    void      **owned_pages;
    ipo_count_t owned_count;
    ipo_count_t owned_cap;
    struct address_space *next;
};

typedef struct reusable_page {
    struct reusable_page *next;
} reusable_page_t;

static spinlock_t s_address_space_lock = {0};
static reusable_page_t *s_reusable_pages = NULL;
static ipo_paddr_t s_kernel_root_paddr = 0u;
static uint8_t s_kernel_paging_levels = 0u;
static address_space_t *s_space_list = NULL;
static void **s_tramp_shared_tables = NULL;
static uint32_t s_tramp_shared_cap = 0u;
static ipo_vaddr_t s_tramp_audio_bottom = (ipo_vaddr_t)ADDRESS_SPACE_TRAMP_AUDIO_TOP;
static ipo_vaddr_t s_tramp_copy_bottom = (ipo_vaddr_t)ADDRESS_SPACE_TRAMP_COPY_TOP;
static bool s_tramp_initialized = false;

static ipo_reg_t address_space_lock(void) {
    return spin_lock_irqsave(&s_address_space_lock);
}

static void address_space_unlock(ipo_reg_t flags) {
    spin_unlock_irqrestore(&s_address_space_lock, flags);
}

void address_space_force_unlock(void) {
    spin_unlock(&s_address_space_lock);
}

static uint8_t detect_hw_paging_levels(void) {
    return (uint8_t)arch_mmu_get_paging_levels();
}

static ipo_paddr_t active_cr3_paddr(uint8_t levels) {
    (void)levels;
    return arch_mmu_get_root();
}

static void flush_active_address_space(const address_space_t *space) {
    if (!space || space->root_paddr == 0u) return;
    if (arch_mmu_get_root() == (space->root_paddr & PAGE_MASK_64)) {
        arch_mmu_flush_all();
    }
}

static void *allocate_page(void) {
    reusable_page_t *page = s_reusable_pages;
    if (page) {
        s_reusable_pages = page->next;
        memset(page, 0, PAGE_SIZE);
        return (void *)page;
    }
    return memory_map_alloc((size_t)PAGE_SIZE, (size_t)PAGE_SIZE);
}

static void release_page(void *address) {
    reusable_page_t *page = (reusable_page_t *)address;
    page->next = s_reusable_pages;
    s_reusable_pages = page;
}

static bool space_own_page(address_space_t *space, void *page) {
    if (!space || !page) return false;
    for (ipo_count_t i = 0u; i < space->owned_count; i++) {
        if (space->owned_pages[i] == page) {
            return true;
        }
    }
    if (space->owned_count >= space->owned_cap) {
        ipo_count_t new_cap = space->owned_cap ? (space->owned_cap * 2u) : 16u;
        if (new_cap <= space->owned_cap || new_cap > (IPO_SIZE_MAX / sizeof(void *))) {
            return false;
        }
        void **grown = (void **)krealloc(space->owned_pages, (size_t)new_cap * sizeof(void *));
        if (!grown) {
            return false;
        }
        space->owned_pages = grown;
        space->owned_cap = new_cap;
    }
    space->owned_pages[space->owned_count++] = page;
    return true;
}

static bool space_owns_paddr(const address_space_t *space, ipo_paddr_t paddr) {
    if (!space || paddr == 0u) return false;
    void *ptr = (void *)(uintptr_t)paddr;
    for (ipo_count_t i = 0u; i < space->owned_count; i++) {
        if (space->owned_pages[i] == ptr) {
            return true;
        }
    }
    return false;
}

/* 2-level 32-bit paging helpers */

static uint32_t large_page_to_pte_flags32(uint32_t pde) {
    uint32_t flags = pde & (PTE_PRESENT | PTE_WRITABLE | PTE_USER |
                            PTE_PWT | PTE_PCD | PTE_ACCESSED |
                            PTE_DIRTY | PTE_GLOBAL);
    if (pde & PDE_LARGE_PAT) flags |= PTE_PAT;
    return flags;
}

static uint32_t *make_private_pt_2level(address_space_t *space, uint32_t dir_idx) {
    uint32_t *pd = (uint32_t *)(uintptr_t)space->root_paddr;
    uint32_t pde = pd[dir_idx];

    if ((pde & PDE_PRESENT) && !(pde & PDE_PAGE_SIZE) &&
        space_owns_paddr(space, (ipo_paddr_t)(pde & PDE_ADDRESS))) {
        return (uint32_t *)(uintptr_t)(pde & PDE_ADDRESS);
    }

    uint32_t *table = (uint32_t *)allocate_page();
    if (!table) return NULL;
    if (!space_own_page(space, table)) {
        release_page(table);
        return NULL;
    }

    if (pde & PDE_PRESENT) {
        if (pde & PDE_PAGE_SIZE) {
            uint32_t phys_base = pde & PDE_LARGE_BASE;
            uint32_t flags = large_page_to_pte_flags32(pde);
            for (uint32_t i = 0u; i < 1024u; i++) {
                table[i] = (phys_base + i * (uint32_t)PAGE_SIZE) | flags;
            }
        } else {
            const uint32_t *shared = (const uint32_t *)(uintptr_t)(pde & PDE_ADDRESS);
            memcpy(table, shared, (size_t)PAGE_SIZE);
        }
    }

    pd[dir_idx] = ((uint32_t)(uintptr_t)table & PDE_ADDRESS) |
                  PDE_PRESENT | PDE_WRITABLE |
                  ((pde & PDE_USER) ? PDE_USER : 0u);
    return table;
}

/* 4-level / 5-level long-mode paging helpers */

static uint64_t large_page_to_pte_flags64(uint64_t pde) {
    uint64_t flags = pde & (uint64_t)(PTE_PRESENT | PTE_WRITABLE | PTE_USER |
                                      PTE_PWT | PTE_PCD | PTE_ACCESSED |
                                      PTE_DIRTY | PTE_GLOBAL);
    if (pde & PDE_LARGE_PAT) flags |= PTE_PAT;
    return flags;
}

static uint64_t *ia32e_ensure_private_child(address_space_t *space, uint64_t *parent_tbl,
                                            ipo_count_t idx, uint8_t child_level,
                                            bool create_if_missing, bool propagate_user) {
    uint64_t entry = parent_tbl[idx];

    if (!(entry & PTE_PRESENT)) {
        if (!create_if_missing) {
            return NULL;
        }
        uint64_t *tbl = (uint64_t *)allocate_page();
        if (!tbl) return NULL;
        if (!space_own_page(space, tbl)) {
            release_page(tbl);
            return NULL;
        }
        parent_tbl[idx] = ((uint64_t)(uintptr_t)tbl & PAGE_MASK_64) |
                          PDE_PRESENT | PDE_WRITABLE |
                          (propagate_user ? PDE_USER : 0ULL);
        return tbl;
    }

    /* Split 1 GiB or 2 MiB large mapping into child table */
    if (entry & PDE_PAGE_SIZE) {
        uint64_t *tbl = (uint64_t *)allocate_page();
        if (!tbl) return NULL;
        if (!space_own_page(space, tbl)) {
            release_page(tbl);
            return NULL;
        }
        if (child_level == 1u) {
            uint64_t phys_base = entry & 0x000FFFFFC0000000ULL;
            uint64_t flags = entry & ~(0x000FFFFFC0000000ULL);
            for (uint64_t i = 0u; i < 512u; i++) {
                tbl[i] = (phys_base + i * 0x200000ULL) | flags | PDE_PAGE_SIZE;
            }
        } else {
            uint64_t phys_base = entry & 0x000FFFFFFFE00000ULL;
            uint64_t flags = large_page_to_pte_flags64(entry);
            for (uint64_t i = 0u; i < 512u; i++) {
                tbl[i] = (phys_base + i * PAGE_SIZE) | flags;
            }
        }
        parent_tbl[idx] = ((uint64_t)(uintptr_t)tbl & PAGE_MASK_64) |
                          PDE_PRESENT | PDE_WRITABLE |
                          ((entry & PDE_USER) ? PDE_USER : 0ULL) |
                          (propagate_user ? PDE_USER : 0ULL);
        return tbl;
    }

    uint64_t *child = (uint64_t *)(uintptr_t)(entry & PAGE_MASK_64);
    if (space_owns_paddr(space, (ipo_paddr_t)(uintptr_t)child)) {
        if (propagate_user && !(entry & PDE_USER)) {
            parent_tbl[idx] = entry | PDE_USER;
        }
        return child;
    }

    /* Clone shared kernel sub-table into this address space on write */
    uint64_t *clone = (uint64_t *)allocate_page();
    if (!clone) return NULL;
    if (!space_own_page(space, clone)) {
        release_page(clone);
        return NULL;
    }
    memcpy(clone, child, (size_t)PAGE_SIZE);
    parent_tbl[idx] = ((uint64_t)(uintptr_t)clone & PAGE_MASK_64) |
                      PDE_PRESENT | PDE_WRITABLE |
                      ((entry & PDE_USER) ? PDE_USER : 0ULL) |
                      (propagate_user ? PDE_USER : 0ULL);
    return clone;
}

static uint64_t *ia32e_walk_to_level(address_space_t *space, ipo_vaddr_t vaddr,
                                     uint8_t target_level, bool create_if_missing,
                                     bool propagate_user) {
    if (!space || space->root_paddr == 0u || space->paging_levels < 4u) return NULL;
    uint64_t *tbl = (uint64_t *)(uintptr_t)space->root_paddr;
    uint8_t lvl = (uint8_t)(space->paging_levels - 1u);
    while (lvl > target_level) {
        unsigned shift = 12u + 9u * (unsigned)lvl;
        ipo_count_t idx = (ipo_count_t)(((uint64_t)vaddr >> shift) & 0x1FFULL);
        tbl = ia32e_ensure_private_child(space, tbl, idx, (uint8_t)(lvl - 1u),
                                         create_if_missing, propagate_user);
        if (!tbl) return NULL;
        lvl--;
    }
    return tbl;
}

static uint64_t *ia32e_read_pd_for_kernel_vaddr(ipo_paddr_t root_paddr, uint8_t levels,
                                                ipo_vaddr_t vaddr) {
    if (root_paddr == 0u || levels < 4u) return NULL;
    uint64_t *tbl = (uint64_t *)(uintptr_t)root_paddr;
    uint8_t lvl = (uint8_t)(levels - 1u);
    while (lvl > 1u) {
        unsigned shift = 12u + 9u * (unsigned)lvl;
        ipo_count_t idx = (ipo_count_t)(((uint64_t)vaddr >> shift) & 0x1FFULL);
        uint64_t entry = tbl[idx];
        if (!(entry & PTE_PRESENT) || (entry & PDE_PAGE_SIZE)) {
            return NULL;
        }
        tbl = (uint64_t *)(uintptr_t)(entry & PAGE_MASK_64);
        lvl--;
    }
    return tbl;
}

/* Shared kernel trampoline window [0xC0000000, 0xD0000000) */

static bool kernel_split_trampoline_pd_locked(void) {
    if (s_kernel_paging_levels < 4u || s_kernel_root_paddr == 0u) {
        return true;
    }

    uint64_t *tbl = (uint64_t *)(uintptr_t)s_kernel_root_paddr;
    uint8_t lvl = (uint8_t)(s_kernel_paging_levels - 1u);
    ipo_vaddr_t tramp_base = (ipo_vaddr_t)ADDRESS_SPACE_TRAMP_BASE;

    while (lvl > 1u) {
        unsigned shift = 12u + 9u * (unsigned)lvl;
        ipo_count_t idx = (ipo_count_t)(((uint64_t)tramp_base >> shift) & 0x1FFULL);
        uint64_t entry = tbl[idx];
        if (!(entry & PTE_PRESENT)) {
            uint64_t *child = (uint64_t *)allocate_page();
            if (!child) return false;
            tbl[idx] = ((uint64_t)(uintptr_t)child & PAGE_MASK_64) | PDE_PRESENT | PDE_WRITABLE;
            tbl = child;
        } else if (entry & PDE_PAGE_SIZE) {
            uint64_t *child = (uint64_t *)allocate_page();
            if (!child) return false;
            uint64_t phys_base = entry & 0x000FFFFFC0000000ULL;
            uint64_t flags = entry & ~(0x000FFFFFC0000000ULL);
            for (uint64_t i = 0u; i < 512u; i++) {
                child[i] = (phys_base + i * 0x200000ULL) | flags | PDE_PAGE_SIZE;
            }
            tbl[idx] = ((uint64_t)(uintptr_t)child & PAGE_MASK_64) | PDE_PRESENT | PDE_WRITABLE;
            tbl = child;
        } else {
            tbl = (uint64_t *)(uintptr_t)(entry & PAGE_MASK_64);
        }
        lvl--;
    }
    return true;
}

static void tramp_sync_slot_locked(ipo_vaddr_t vaddr, ipo_paddr_t pt_paddr, bool present) {
    if (s_kernel_paging_levels == 2u) {
        uint32_t dir_idx = (uint32_t)(vaddr >> 22);
        uint32_t pde_val = present ? (((uint32_t)pt_paddr & PDE_ADDRESS) | PDE_PRESENT | PDE_WRITABLE) : 0u;
        if (s_kernel_root_paddr) {
            ((uint32_t *)(uintptr_t)s_kernel_root_paddr)[dir_idx] = pde_val;
        }
        for (address_space_t *s = s_space_list; s; s = s->next) {
            if (s->root_paddr) {
                ((uint32_t *)(uintptr_t)s->root_paddr)[dir_idx] = pde_val;
            }
        }
        return;
    }

    uint32_t pd_idx = (uint32_t)(((uint64_t)vaddr >> 21) & 0x1FFULL);
    uint64_t pde_val = present ? ((pt_paddr & PAGE_MASK_64) | PDE_PRESENT | PDE_WRITABLE) : 0ULL;
    uint64_t *k_pd = ia32e_read_pd_for_kernel_vaddr(s_kernel_root_paddr, s_kernel_paging_levels, vaddr);
    if (k_pd) {
        k_pd[pd_idx] = pde_val;
    }
    for (address_space_t *s = s_space_list; s; s = s->next) {
        uint64_t *s_pd = ia32e_read_pd_for_kernel_vaddr(s->root_paddr, s->paging_levels, vaddr);
        if (s_pd && s_pd != k_pd) {
            s_pd[pd_idx] = pde_val;
        }
    }
}

static bool tramp_map_page_locked(ipo_vaddr_t page_vaddr) {
    if (page_vaddr < ADDRESS_SPACE_TRAMP_BASE || page_vaddr >= ADDRESS_SPACE_TRAMP_COPY_TOP) {
        return false;
    }

    if (s_kernel_paging_levels == 2u) {
        uint32_t slot = (uint32_t)((page_vaddr - ADDRESS_SPACE_TRAMP_BASE) >> 22);
        if (!s_tramp_shared_tables || slot >= s_tramp_shared_cap) return false;
        uint32_t table_idx = (uint32_t)((page_vaddr >> 12) & 0x3FFu);
        uint32_t *table = (uint32_t *)s_tramp_shared_tables[slot];
        if (!table) {
            table = (uint32_t *)allocate_page();
            if (!table) return false;
            s_tramp_shared_tables[slot] = table;
            tramp_sync_slot_locked(page_vaddr, (ipo_paddr_t)(uintptr_t)table, true);
        }
        if (table[table_idx] & PTE_PRESENT) {
            return true;
        }
        void *frame = memory_page_alloc();
        if (!frame) return false;
        table[table_idx] = ((uint32_t)(uintptr_t)frame & PAGE_MASK_32) | PTE_PRESENT | PTE_WRITABLE;
        arch_mmu_flush_page(page_vaddr);
        return true;
    }

    uint32_t slot = (uint32_t)((page_vaddr - ADDRESS_SPACE_TRAMP_BASE) >> 21);
    if (!s_tramp_shared_tables || slot >= s_tramp_shared_cap) return false;
    uint32_t table_idx = (uint32_t)((page_vaddr >> 12) & 0x1FFu);
    uint64_t *table = (uint64_t *)s_tramp_shared_tables[slot];
    if (!table) {
        table = (uint64_t *)allocate_page();
        if (!table) return false;
        s_tramp_shared_tables[slot] = table;
        tramp_sync_slot_locked(page_vaddr, (ipo_paddr_t)(uintptr_t)table, true);
    }
    if (table[table_idx] & PTE_PRESENT) {
        return true;
    }
    void *frame = memory_page_alloc();
    if (!frame) return false;
    table[table_idx] = ((uint64_t)(uintptr_t)frame & PAGE_MASK_64) | PTE_PRESENT | PTE_WRITABLE;
    arch_mmu_flush_page(page_vaddr);
    return true;
}

static void tramp_init_locked(void) {
    if (s_tramp_initialized) return;
    uint8_t levels = detect_hw_paging_levels();
    if (levels == 0u) return;

    s_kernel_paging_levels = levels;
    if (s_kernel_root_paddr == 0u) {
        s_kernel_root_paddr = active_cr3_paddr(levels);
    }

    uint32_t need_slots = (levels >= 4u)
                        ? (uint32_t)ADDRESS_SPACE_TRAMP_SLOTS_64
                        : (uint32_t)ADDRESS_SPACE_TRAMP_SLOTS_32;
    if (need_slots > s_tramp_shared_cap || !s_tramp_shared_tables) {
        void **grown = (void **)krealloc(s_tramp_shared_tables, (size_t)need_slots * sizeof(void *));
        if (!grown) return;
        s_tramp_shared_tables = grown;
        s_tramp_shared_cap = need_slots;
    }

    if (levels >= 4u) {
        if (!kernel_split_trampoline_pd_locked()) {
            return;
        }
        for (uint32_t i = 0u; i < need_slots; i++) {
            s_tramp_shared_tables[i] = NULL;
            tramp_sync_slot_locked((ipo_vaddr_t)(ADDRESS_SPACE_TRAMP_BASE + ((uint64_t)i << 21)), 0u, false);
        }
    } else {
        for (uint32_t i = 0u; i < need_slots; i++) {
            s_tramp_shared_tables[i] = NULL;
            tramp_sync_slot_locked((ipo_vaddr_t)(ADDRESS_SPACE_TRAMP_BASE + ((uint64_t)i << 22)), 0u, false);
        }
    }

    s_tramp_audio_bottom = (ipo_vaddr_t)(ADDRESS_SPACE_TRAMP_AUDIO_TOP - PAGE_SIZE);
    tramp_map_page_locked(s_tramp_audio_bottom);

    s_tramp_copy_bottom = (ipo_vaddr_t)(ADDRESS_SPACE_TRAMP_COPY_TOP - PAGE_SIZE);
    tramp_map_page_locked(s_tramp_copy_bottom);

    s_tramp_initialized = true;
    arch_mmu_flush_all();
}

void address_space_trampoline_init(void) {
    ipo_reg_t irq_flags = address_space_lock();
    tramp_init_locked();
    address_space_unlock(irq_flags);
}

void *address_space_get_audio_tramp_stack_top(void) {
    address_space_trampoline_init();
    return (void *)(uintptr_t)ADDRESS_SPACE_TRAMP_AUDIO_TOP;
}

void *address_space_get_copy_tramp_stack_top(void) {
    address_space_trampoline_init();
    return (void *)(uintptr_t)ADDRESS_SPACE_TRAMP_COPY_TOP;
}

bool address_space_is_trampoline_addr(ipo_vaddr_t virtual_address) {
    return (virtual_address >= ADDRESS_SPACE_TRAMP_BASE &&
            virtual_address < ADDRESS_SPACE_TRAMP_COPY_TOP);
}

bool address_space_handle_trampoline_fault(ipo_vaddr_t fault_address,
                                            ipo_reg_t error_code,
                                            ipo_vaddr_t saved_sp) {
    if ((error_code & 1u) != 0u) return false;
    if (!address_space_is_trampoline_addr(fault_address)) return false;

    ipo_reg_t irq_flags = address_space_lock();
    if (!s_tramp_initialized) {
        address_space_unlock(irq_flags);
        return false;
    }

    bool in_audio = (fault_address < ADDRESS_SPACE_TRAMP_AUDIO_TOP);
    ipo_vaddr_t reg_base = (ipo_vaddr_t)(in_audio ? ADDRESS_SPACE_TRAMP_BASE : ADDRESS_SPACE_TRAMP_COPY_BASE);
    ipo_vaddr_t reg_top = (ipo_vaddr_t)(in_audio ? ADDRESS_SPACE_TRAMP_AUDIO_TOP : ADDRESS_SPACE_TRAMP_COPY_TOP);
    ipo_vaddr_t *bottom_ptr = in_audio ? &s_tramp_audio_bottom : &s_tramp_copy_bottom;

    ipo_vaddr_t fault_page = ipo_vaddr_align_down(fault_address, (ipo_size_t)PAGE_SIZE);
    ipo_vaddr_t esp_page = ipo_vaddr_align_down(saved_sp, (ipo_size_t)PAGE_SIZE);
    ipo_vaddr_t target_lowest = fault_page;
    if (saved_sp >= reg_base && saved_sp < reg_top && esp_page < target_lowest) {
        target_lowest = esp_page;
    }

    if (target_lowest < reg_base) {
        address_space_unlock(irq_flags);
        return false;
    }

    /* If already mapped in shared table (stale TLB/PDE on active CPU), sync and resume */
    if (fault_page >= *bottom_ptr && s_tramp_shared_tables) {
        if (s_kernel_paging_levels == 2u) {
            uint32_t slot = (uint32_t)((fault_page - ADDRESS_SPACE_TRAMP_BASE) >> 22);
            uint32_t *tbl = (slot < s_tramp_shared_cap) ? (uint32_t *)s_tramp_shared_tables[slot] : NULL;
            if (tbl && (tbl[(fault_page >> 12) & 0x3FFu] & PTE_PRESENT)) {
                uint32_t *cur_pd = (uint32_t *)(uintptr_t)active_cr3_paddr(2u);
                cur_pd[fault_page >> 22] = ((uint32_t)(uintptr_t)tbl & PDE_ADDRESS) | PDE_PRESENT | PDE_WRITABLE;
                arch_mmu_flush_page(fault_address);
                address_space_unlock(irq_flags);
                return true;
            }
        } else {
            uint32_t slot = (uint32_t)((fault_page - ADDRESS_SPACE_TRAMP_BASE) >> 21);
            uint64_t *tbl = (slot < s_tramp_shared_cap) ? (uint64_t *)s_tramp_shared_tables[slot] : NULL;
            if (tbl && (tbl[(fault_page >> 12) & 0x1FFu] & PTE_PRESENT)) {
                uint64_t *cur_pd = ia32e_read_pd_for_kernel_vaddr(
                    active_cr3_paddr(s_kernel_paging_levels), s_kernel_paging_levels, fault_page);
                if (cur_pd) {
                    uint32_t pd_idx = (uint32_t)(((uint64_t)fault_page >> 21) & 0x1FFULL);
                    cur_pd[pd_idx] = ((uint64_t)(uintptr_t)tbl & PAGE_MASK_64) | PDE_PRESENT | PDE_WRITABLE;
                    arch_mmu_flush_page(fault_address);
                    address_space_unlock(irq_flags);
                    return true;
                }
            }
        }
    }

    for (ipo_vaddr_t page = *bottom_ptr - (ipo_vaddr_t)PAGE_SIZE; ; page -= (ipo_vaddr_t)PAGE_SIZE) {
        if (!tramp_map_page_locked(page)) {
            address_space_unlock(irq_flags);
            return false;
        }
        *bottom_ptr = page;
        if (page == target_lowest) {
            break;
        }
    }

    address_space_unlock(irq_flags);
    return true;
}

address_space_t *address_space_create(void) {
    uint8_t levels = detect_hw_paging_levels();
    if (levels == 0u) return NULL;

    ipo_reg_t irq_flags = address_space_lock();
    if (s_kernel_root_paddr == 0u) {
        s_kernel_root_paddr = active_cr3_paddr(levels);
        s_kernel_paging_levels = levels;
    }
    tramp_init_locked();

    address_space_t *space = (address_space_t *)kmalloc(sizeof(*space));
    if (!space) {
        address_space_unlock(irq_flags);
        return NULL;
    }
    memset(space, 0, sizeof(*space));
    space->paging_levels = levels;

    void *root = allocate_page();
    if (!root) {
        kfree(space);
        address_space_unlock(irq_flags);
        return NULL;
    }
    memcpy(root, (const void *)(uintptr_t)s_kernel_root_paddr, (size_t)PAGE_SIZE);
    space->root_paddr = (ipo_paddr_t)(uintptr_t)root;

    space->next = s_space_list;
    s_space_list = space;
    address_space_unlock(irq_flags);
    return space;
}

bool address_space_destroy(address_space_t *space) {
    if (!space || space->root_paddr == 0u) return false;

    ipo_reg_t irq_flags = address_space_lock();
    if (active_cr3_paddr(space->paging_levels) == (space->root_paddr & PAGE_MASK_64)) {
        address_space_unlock(irq_flags);
        return false;
    }

    address_space_t **link = &s_space_list;
    while (*link) {
        if (*link == space) {
            *link = space->next;
            break;
        }
        link = &(*link)->next;
    }

    for (ipo_count_t i = 0u; i < space->owned_count; i++) {
        if (space->owned_pages[i]) {
            release_page(space->owned_pages[i]);
        }
    }
    release_page((void *)(uintptr_t)space->root_paddr);
    if (space->owned_pages) {
        kfree(space->owned_pages);
    }
    kfree(space);
    address_space_unlock(irq_flags);
    return true;
}

bool address_space_activate(address_space_t *space) {
    if (!space || space->root_paddr == 0u) return false;
    if (space->root_paddr & (PAGE_SIZE - 1u)) return false;

    ipo_reg_t irq_flags = address_space_lock();
    arch_mmu_set_root(space->root_paddr);
    syscall_update_exception_page_directory();
    address_space_unlock(irq_flags);
    return true;
}

bool address_space_activate_kernel(void) {
    ipo_reg_t irq_flags = address_space_lock();
    if (s_kernel_root_paddr == 0u) {
        address_space_unlock(irq_flags);
        return false;
    }

    arch_mmu_set_root(s_kernel_root_paddr);
    syscall_update_exception_page_directory();
    address_space_unlock(irq_flags);
    return true;
}

void __attribute__((noinline, used)) _address_space_trampoline_inner(
    ipo_reg_t new_cr3, ipo_reg_t old_cr3, void (*fn)(void *), void *arg) {
    ipo_reg_t irq_flags = address_space_lock();
    arch_mmu_set_root((ipo_paddr_t)new_cr3);
    syscall_update_exception_page_directory();
    address_space_unlock(irq_flags);

    fn(arg);

    irq_flags = address_space_lock();
    arch_mmu_set_root((ipo_paddr_t)old_cr3);
    syscall_update_exception_page_directory();
    address_space_unlock(irq_flags);
}

void address_space_call_on_stack(address_space_t *target, void (*fn)(void *),
                                 void *ctx, void *kstack_top) {
    if (!fn) return;
    ipo_paddr_t cur_root = arch_mmu_get_root();
    ipo_paddr_t target_root = (target && target->root_paddr)
                                  ? target->root_paddr
                                  : s_kernel_root_paddr;

    if (target_root == 0u || cur_root == target_root) {
        fn(ctx);
        return;
    }

    ipo_vaddr_t cur_sp = arch_stack_get_sp();

    if (cur_sp < (ipo_vaddr_t)0x10000000u ||
        address_space_is_trampoline_addr(cur_sp) ||
        !kstack_top) {
        _address_space_trampoline_inner((ipo_reg_t)target_root, (ipo_reg_t)cur_root, fn, ctx);
        return;
    }

    ipo_vaddr_t aligned_top = ipo_vaddr_align_down((ipo_vaddr_t)(uintptr_t)kstack_top, 16u);
    arch_context_call_on_new_stack(target_root, cur_root,
                                   _address_space_trampoline_inner,
                                   fn, ctx, aligned_top);
}

void address_space_call_with_copy(address_space_t *target, void (*fn)(void *),
                                  void *ctx, size_t ctx_size) {
    if (!fn) return;
    ipo_paddr_t cur_root = arch_mmu_get_root();
    ipo_paddr_t target_root = (target && target->root_paddr)
                                  ? target->root_paddr
                                  : s_kernel_root_paddr;

    if (target_root == 0u || cur_root == target_root) {
        fn(ctx);
        return;
    }

    ipo_vaddr_t cur_sp = arch_stack_get_sp();

    if (cur_sp < (ipo_vaddr_t)0x10000000u || address_space_is_trampoline_addr(cur_sp)) {
        _address_space_trampoline_inner((ipo_reg_t)target_root, (ipo_reg_t)cur_root, fn, ctx);
        return;
    }

    void *kctx = ctx;
    if (ctx && ctx_size > 0u) {
        kctx = kmalloc(ctx_size);
        if (!kctx) {
            fn(ctx);
            return;
        }
        memcpy(kctx, ctx, ctx_size);
    }

    void *kstack_top = address_space_get_copy_tramp_stack_top();
    address_space_call_on_stack(target, fn, kctx, kstack_top);

    if (ctx && ctx_size > 0u) {
        memcpy(ctx, kctx, ctx_size);
        kfree(kctx);
    }
}

bool address_space_map(address_space_t *space, ipo_vaddr_t virtual_address,
                       ipo_paddr_t physical_address, ipo_reg_t flags) {
    if (!space || space->root_paddr == 0u ||
        (virtual_address & (PAGE_SIZE - 1u)) ||
        (physical_address & (PAGE_SIZE - 1u))) {
        return false;
    }

    ipo_reg_t irq_flags = address_space_lock();
    bool prop_user = (flags & PTE_USER) != 0u;

    if (space->paging_levels == 2u) {
        uint32_t dir_idx = (uint32_t)(virtual_address >> 22);
        uint32_t tbl_idx = (uint32_t)((virtual_address >> 12) & 0x3FFu);
        uint32_t *table = make_private_pt_2level(space, dir_idx);
        if (!table) {
            address_space_unlock(irq_flags);
            return false;
        }
        uint32_t pte_flags = ((uint32_t)flags & 0x1FFu) | PTE_PRESENT;
        table[tbl_idx] = ((uint32_t)physical_address & PAGE_MASK_32) | pte_flags;
        if (prop_user) {
            ((uint32_t *)(uintptr_t)space->root_paddr)[dir_idx] |= PDE_USER;
        }
    } else {
        uint64_t *pt = ia32e_walk_to_level(space, virtual_address, 0u, true, prop_user);
        if (!pt) {
            address_space_unlock(irq_flags);
            return false;
        }
        uint32_t tbl_idx = (uint32_t)(((uint64_t)virtual_address >> 12) & 0x1FFULL);
        uint64_t pte_flags = ((uint64_t)flags & 0x1FFULL) | PTE_PRESENT;
        pt[tbl_idx] = ((uint64_t)physical_address & PAGE_MASK_64) | pte_flags;
    }

    flush_active_address_space(space);
    address_space_unlock(irq_flags);
    return true;
}

bool address_space_unmap(address_space_t *space, ipo_vaddr_t virtual_address) {
    if (!space || space->root_paddr == 0u ||
        (virtual_address & (PAGE_SIZE - 1u))) {
        return false;
    }

    ipo_reg_t irq_flags = address_space_lock();
    if (space->paging_levels == 2u) {
        uint32_t *pd = (uint32_t *)(uintptr_t)space->root_paddr;
        uint32_t dir_idx = (uint32_t)(virtual_address >> 22);
        uint32_t tbl_idx = (uint32_t)((virtual_address >> 12) & 0x3FFu);
        if (!(pd[dir_idx] & PDE_PRESENT)) {
            address_space_unlock(irq_flags);
            return false;
        }
        uint32_t *table = make_private_pt_2level(space, dir_idx);
        if (!table || !(table[tbl_idx] & PTE_PRESENT)) {
            address_space_unlock(irq_flags);
            return false;
        }
        table[tbl_idx] = 0u;
    } else {
        uint64_t *pt = ia32e_walk_to_level(space, virtual_address, 0u, false, false);
        if (!pt) {
            address_space_unlock(irq_flags);
            return false;
        }
        uint32_t tbl_idx = (uint32_t)(((uint64_t)virtual_address >> 12) & 0x1FFULL);
        if (!(pt[tbl_idx] & PTE_PRESENT)) {
            address_space_unlock(irq_flags);
            return false;
        }
        pt[tbl_idx] = 0ULL;
    }

    flush_active_address_space(space);
    address_space_unlock(irq_flags);
    return true;
}

bool address_space_set_identity_range(address_space_t *space,
                                      ipo_vaddr_t virtual_address,
                                      ipo_size_t length, bool mapped) {
    ipo_vaddr_t end = 0u;
    if (!space || space->root_paddr == 0u ||
        (virtual_address & (PAGE_SIZE - 1u)) ||
        (length & (PAGE_SIZE - 1u)) ||
        length == 0u ||
        !ipo_vaddr_add_ok(virtual_address, length, &end)) {
        return false;
    }

    ipo_reg_t irq_flags = address_space_lock();

    if (space->paging_levels == 2u) {
        uint32_t *pd = (uint32_t *)(uintptr_t)space->root_paddr;
        uint32_t first_dir = (uint32_t)(virtual_address >> 22);
        uint32_t last_dir = (uint32_t)((end - 1u) >> 22);
        uint32_t tramp_first = (uint32_t)(ADDRESS_SPACE_TRAMP_BASE >> 22);
        uint32_t tramp_end = tramp_first + (uint32_t)ADDRESS_SPACE_TRAMP_SLOTS_32;

        for (uint32_t dir_idx = first_dir; dir_idx <= last_dir; dir_idx++) {
            uint64_t chunk_start = (uint64_t)dir_idx << 22;
            uint64_t chunk_end = chunk_start + 0x400000ULL;
            bool fully_covered = (chunk_start >= virtual_address && chunk_end <= end);
            bool is_tramp = (dir_idx >= tramp_first && dir_idx < tramp_end);
            if (dir_idx > 3u && fully_covered && !is_tramp) {
                continue;
            }
            if (!make_private_pt_2level(space, dir_idx)) {
                address_space_unlock(irq_flags);
                return false;
            }
        }

        for (uint32_t dir_idx = first_dir; dir_idx <= last_dir; dir_idx++) {
            uint64_t chunk_start = (uint64_t)dir_idx << 22;
            uint64_t chunk_end = chunk_start + 0x400000ULL;
            uint64_t eff_start = (chunk_start > virtual_address) ? chunk_start : virtual_address;
            uint64_t eff_end = (chunk_end < end) ? chunk_end : end;
            bool fully_covered = (eff_start == chunk_start && eff_end == chunk_end);
            bool is_tramp = (dir_idx >= tramp_first && dir_idx < tramp_end);

            if (dir_idx > 3u && fully_covered && !is_tramp) {
                pd[dir_idx] = mapped ? ((dir_idx << 22) | (PDE_PRESENT | PDE_WRITABLE | PDE_PAGE_SIZE)) : 0u;
            } else {
                uint32_t *table = (uint32_t *)(uintptr_t)(pd[dir_idx] & PDE_ADDRESS);
                for (uint64_t page = eff_start; page < eff_end; page += PAGE_SIZE) {
                    uint32_t tbl_idx = (uint32_t)((page >> 12) & 0x3FFu);
                    table[tbl_idx] = mapped ? ((uint32_t)page | (PTE_PRESENT | PTE_WRITABLE)) : 0u;
                }
            }
        }
    } else {
        uint64_t cur = (uint64_t)virtual_address;
        while (cur < (uint64_t)end) {
            bool in_tramp = (cur >= ADDRESS_SPACE_TRAMP_BASE && cur < ADDRESS_SPACE_TRAMP_COPY_TOP);
            uint64_t rem = (uint64_t)end - cur;
            if (!in_tramp && cur >= 0x1000000ULL &&
                (cur & 0x1FFFFFULL) == 0ULL && rem >= 0x200000ULL) {
                uint64_t *pd = ia32e_walk_to_level(space, (ipo_vaddr_t)cur, 1u, mapped, false);
                if (!pd) {
                    if (!mapped) {
                        cur += 0x200000ULL;
                        continue;
                    }
                    address_space_unlock(irq_flags);
                    return false;
                }
                uint32_t pd_idx = (uint32_t)((cur >> 21) & 0x1FFULL);
                pd[pd_idx] = mapped ? ((cur & 0x000FFFFFFFE00000ULL) | PDE_PRESENT | PDE_WRITABLE | PDE_PAGE_SIZE) : 0ULL;
                cur += 0x200000ULL;
            } else {
                uint64_t *pt = ia32e_walk_to_level(space, (ipo_vaddr_t)cur, 0u, mapped, false);
                if (!pt) {
                    if (!mapped) {
                        cur += PAGE_SIZE;
                        continue;
                    }
                    address_space_unlock(irq_flags);
                    return false;
                }
                uint32_t pt_idx = (uint32_t)((cur >> 12) & 0x1FFULL);
                pt[pt_idx] = mapped ? ((cur & PAGE_MASK_64) | PTE_PRESENT | PTE_WRITABLE) : 0ULL;
                cur += PAGE_SIZE;
            }
        }
    }

    flush_active_address_space(space);
    address_space_unlock(irq_flags);
    return true;
}

bool address_space_range_mapped(address_space_t *space,
                                ipo_vaddr_t virtual_address, ipo_size_t length,
                                bool mapped) {
    ipo_vaddr_t end = 0u;
    if (!space || space->root_paddr == 0u || length == 0u ||
        !ipo_vaddr_add_ok(virtual_address, length, &end)) {
        return false;
    }

    ipo_reg_t irq_flags = address_space_lock();
    ipo_vaddr_t start_page = ipo_vaddr_align_down(virtual_address, (ipo_size_t)PAGE_SIZE);

    for (ipo_vaddr_t page = start_page; page < end; ) {
        bool is_mapped = false;
        if (space->paging_levels == 2u) {
            const uint32_t *pd = (const uint32_t *)(uintptr_t)space->root_paddr;
            uint32_t pde = pd[(uint32_t)(page >> 22)];
            is_mapped = (pde & PDE_PRESENT) != 0u;
            if (is_mapped && !(pde & PDE_PAGE_SIZE)) {
                const uint32_t *pt = (const uint32_t *)(uintptr_t)(pde & PDE_ADDRESS);
                is_mapped = (pt[(page >> 12) & 0x3FFu] & PTE_PRESENT) != 0u;
            }
            if (is_mapped != mapped) {
                address_space_unlock(irq_flags);
                return false;
            }
            if (page > IPO_VADDR_MAX - (ipo_vaddr_t)PAGE_SIZE) break;
            page += (ipo_vaddr_t)PAGE_SIZE;
        } else {
            const uint64_t *tbl = (const uint64_t *)(uintptr_t)space->root_paddr;
            uint8_t lvl = (uint8_t)(space->paging_levels - 1u);
            while (true) {
                unsigned shift = 12u + 9u * (unsigned)lvl;
                uint64_t entry = tbl[(uint32_t)(((uint64_t)page >> shift) & 0x1FFULL)];
                if (!(entry & PTE_PRESENT)) {
                    is_mapped = false;
                    break;
                }
                if (lvl == 0u || (entry & PDE_PAGE_SIZE)) {
                    is_mapped = true;
                    break;
                }
                tbl = (const uint64_t *)(uintptr_t)(entry & PAGE_MASK_64);
                lvl--;
            }
            if (is_mapped != mapped) {
                address_space_unlock(irq_flags);
                return false;
            }
            if (page > IPO_VADDR_MAX - (ipo_vaddr_t)PAGE_SIZE) break;
            page += (ipo_vaddr_t)PAGE_SIZE;
        }
    }

    address_space_unlock(irq_flags);
    return true;
}

ipo_paddr_t address_space_get_physical(address_space_t *space, ipo_vaddr_t virtual_address) {
    if (!space || space->root_paddr == 0u) return 0u;
    ipo_reg_t irq_flags = address_space_lock();

    if (space->paging_levels == 2u) {
        const uint32_t *pd = (const uint32_t *)(uintptr_t)space->root_paddr;
        uint32_t pde = pd[(uint32_t)(virtual_address >> 22)];
        if (!(pde & PDE_PRESENT)) {
            address_space_unlock(irq_flags);
            return 0u;
        }
        if (pde & PDE_PAGE_SIZE) {
            address_space_unlock(irq_flags);
            return (ipo_paddr_t)((pde & PDE_LARGE_BASE) + (virtual_address & 0x003FFFFFu));
        }
        const uint32_t *pt = (const uint32_t *)(uintptr_t)(pde & PDE_ADDRESS);
        if (!pt) {
            address_space_unlock(irq_flags);
            return 0u;
        }
        uint32_t pte = pt[(virtual_address >> 12) & 0x3FFu];
        address_space_unlock(irq_flags);
        if (!(pte & PTE_PRESENT)) return 0u;
        return (ipo_paddr_t)((pte & PAGE_MASK_32) | (virtual_address & (PAGE_SIZE - 1u)));
    }

    const uint64_t *tbl = (const uint64_t *)(uintptr_t)space->root_paddr;
    uint8_t lvl = (uint8_t)(space->paging_levels - 1u);
    while (true) {
        unsigned shift = 12u + 9u * (unsigned)lvl;
        uint64_t entry = tbl[(uint32_t)(((uint64_t)virtual_address >> shift) & 0x1FFULL)];
        if (!(entry & PTE_PRESENT)) {
            address_space_unlock(irq_flags);
            return 0u;
        }
        if (lvl == 0u || (entry & PDE_PAGE_SIZE)) {
            uint64_t offset_mask = (1ULL << shift) - 1ULL;
            uint64_t base_mask = PAGE_MASK_64 & ~offset_mask;
            address_space_unlock(irq_flags);
            return (ipo_paddr_t)((entry & base_mask) | ((uint64_t)virtual_address & offset_mask));
        }
        tbl = (const uint64_t *)(uintptr_t)(entry & PAGE_MASK_64);
        lvl--;
    }
}

extern uint8_t _kernel_text_start[];
extern uint8_t _kernel_rodata_end[];

static bool is_guard_low_page(uint32_t page) {
    return (page == 0u || (page >= 2u && page <= 5u) ||
            page == 7u || (page >= 9u && page <= 15u));
}

static void paging_init_2level(ipo_vaddr_t ktext_start, ipo_vaddr_t krodata_end) {
    uint32_t *page_dir = (uint32_t *)memory_map_alloc((size_t)PAGE_SIZE, (size_t)PAGE_SIZE);
    uint32_t *pt_low   = (uint32_t *)memory_map_alloc((size_t)(4u * PAGE_SIZE), (size_t)PAGE_SIZE);
    if (!page_dir || !pt_low) {
        serial_printf("[paging] Failed to allocate 2-level page tables\n");
        return;
    }

    memset(page_dir, 0, (size_t)PAGE_SIZE);
    memset(pt_low, 0, (size_t)(4u * PAGE_SIZE));

    for (uint32_t page = 0u; page < 4096u; page++) {
        uint32_t addr = page * (uint32_t)PAGE_SIZE;
        if (is_guard_low_page(page)) {
            pt_low[page] = 0u;
        } else if (addr >= ktext_start && addr < krodata_end) {
            pt_low[page] = addr | PTE_PRESENT;
        } else if (addr >= 0x000C0000u && addr < 0x00100000u) {
            pt_low[page] = addr | PTE_PRESENT;
        } else {
            pt_low[page] = addr | PTE_PRESENT | PTE_WRITABLE;
        }
    }

    for (uint32_t pde = 0u; pde < 4u; pde++) {
        page_dir[pde] = ((uint32_t)(uintptr_t)&pt_low[pde * 1024u]) | PDE_PRESENT | PDE_WRITABLE;
    }
    for (uint32_t pde = 4u; pde < 1024u; pde++) {
        page_dir[pde] = (pde << 22) | PDE_PRESENT | PDE_WRITABLE | PDE_PAGE_SIZE;
    }

    s_kernel_paging_levels = 2u;
    s_kernel_root_paddr = (ipo_paddr_t)(uintptr_t)page_dir;

    arch_mmu_enable_paging(s_kernel_root_paddr, 2u);

    address_space_trampoline_init();
    smp_sync_paging(s_kernel_root_paddr);
}

static void paging_init_ia32e(uint8_t levels, ipo_vaddr_t ktext_start, ipo_vaddr_t krodata_end) {
    /* Always identity-map at least 4 GiB (MMIO/LAPIC/APIC/trampolines) plus detected RAM */
    uint64_t map_top = 0x100000000ULL;
    size_t reg_count = memory_map_get_region_count();
    for (size_t i = 0u; i < reg_count; i++) {
        const memory_region_t *reg = memory_map_get_region(i);
        if (reg && reg->is_usable && reg->size > 0u) {
            uint64_t r_end = reg->base + reg->size;
            if (r_end > map_top) {
                map_top = r_end;
            }
        }
    }
    /* Round up to 1 GiB boundary so whole PDs cover map_top */
    uint64_t gb_count = (map_top + 0x3FFFFFFFULL) >> 30;
    if (gb_count > 512ULL) {
        gb_count = 512ULL; /* 512 GiB identity span across single PDPT */
    }

    uint64_t *pml5 = (levels == 5u) ? (uint64_t *)memory_map_alloc((size_t)PAGE_SIZE, (size_t)PAGE_SIZE) : NULL;
    uint64_t *pml4 = (uint64_t *)memory_map_alloc((size_t)PAGE_SIZE, (size_t)PAGE_SIZE);
    uint64_t *pdpt = (uint64_t *)memory_map_alloc((size_t)PAGE_SIZE, (size_t)PAGE_SIZE);
    uint64_t *pt_low = (uint64_t *)memory_map_alloc((size_t)(8u * PAGE_SIZE), (size_t)PAGE_SIZE);
    if ((levels == 5u && !pml5) || !pml4 || !pdpt || !pt_low) {
        serial_printf("[paging] Failed to allocate %u-level page tables\n", levels);
        return;
    }

    if (pml5) memset(pml5, 0, (size_t)PAGE_SIZE);
    memset(pml4, 0, (size_t)PAGE_SIZE);
    memset(pdpt, 0, (size_t)PAGE_SIZE);
    memset(pt_low, 0, (size_t)(8u * PAGE_SIZE));

    /* First 16 MiB using 8 x 4 KiB PT tables (8 * 512 = 4096 pages) */
    for (uint32_t page = 0u; page < 4096u; page++) {
        uint64_t addr = (uint64_t)page * PAGE_SIZE;
        if (is_guard_low_page(page)) {
            pt_low[page] = 0ULL;
        } else if (addr >= ktext_start && addr < krodata_end) {
            pt_low[page] = addr | PTE_PRESENT;
        } else if (addr >= 0x000C0000ULL && addr < 0x00100000ULL) {
            pt_low[page] = addr | PTE_PRESENT;
        } else {
            pt_low[page] = addr | PTE_PRESENT | PTE_WRITABLE;
        }
    }

    for (uint64_t gb = 0u; gb < gb_count; gb++) {
        uint64_t *pd = (uint64_t *)memory_map_alloc((size_t)PAGE_SIZE, (size_t)PAGE_SIZE);
        if (!pd) {
            serial_printf("[paging] Failed to allocate PD for GiB %llu\n", (unsigned long long)gb);
            return;
        }
        memset(pd, 0, (size_t)PAGE_SIZE);
        uint64_t gb_base = gb << 30;
        for (uint64_t idx = 0u; idx < 512u; idx++) {
            uint64_t phys = gb_base + (idx << 21);
            if (gb == 0u && idx < 8u) {
                pd[idx] = ((uint64_t)(uintptr_t)&pt_low[idx * 512u]) | PDE_PRESENT | PDE_WRITABLE;
            } else {
                pd[idx] = phys | PDE_PRESENT | PDE_WRITABLE | PDE_PAGE_SIZE;
            }
        }
        pdpt[gb] = ((uint64_t)(uintptr_t)pd & PAGE_MASK_64) | PDE_PRESENT | PDE_WRITABLE;
    }

    pml4[0] = ((uint64_t)(uintptr_t)pdpt & PAGE_MASK_64) | PDE_PRESENT | PDE_WRITABLE;
    if (pml5) {
        pml5[0] = ((uint64_t)(uintptr_t)pml4 & PAGE_MASK_64) | PDE_PRESENT | PDE_WRITABLE;
        s_kernel_root_paddr = (ipo_paddr_t)(uintptr_t)pml5;
    } else {
        s_kernel_root_paddr = (ipo_paddr_t)(uintptr_t)pml4;
    }
    s_kernel_paging_levels = levels;

    arch_mmu_enable_paging(s_kernel_root_paddr, levels);

    address_space_trampoline_init();
    smp_sync_paging(s_kernel_root_paddr);
}

void paging_init(void) {
    ipo_vaddr_t ktext_start = (ipo_vaddr_t)(uintptr_t)_kernel_text_start;
    ipo_vaddr_t krodata_end = ipo_vaddr_align_down((ipo_vaddr_t)(uintptr_t)_kernel_rodata_end,
                                                   (ipo_size_t)PAGE_SIZE);
    uint8_t active_levels = detect_hw_paging_levels();

    if (active_levels >= 4u) {
        paging_init_ia32e(active_levels, ktext_start, krodata_end);
    } else {
        paging_init_2level(ktext_start, krodata_end);
    }

    serial_printf("[paging] Paging enabled (%u-level, CR3=0x%llx, RO [0x%llx - 0x%llx])\n",
                  s_kernel_paging_levels,
                  (unsigned long long)s_kernel_root_paddr,
                  (unsigned long long)ktext_start,
                  (unsigned long long)krodata_end);
}
