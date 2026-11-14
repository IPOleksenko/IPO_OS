#include <memory/address_space.h>
#include <memory/memory_map.h>
#include <memory/kmalloc.h>
#include <kernel/smp.h>
#include <syscall.h>
#include <stdio.h>
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

#define ADDRESS_SPACE_TRAMP_BASE        0xC0000000u
#define ADDRESS_SPACE_TRAMP_AUDIO_TOP   0xC8000000u
#define ADDRESS_SPACE_TRAMP_COPY_BASE   0xC8000000u
#define ADDRESS_SPACE_TRAMP_COPY_TOP    0xD0000000u
#define ADDRESS_SPACE_TRAMP_PDE_START   (ADDRESS_SPACE_TRAMP_BASE >> 22)
#define ADDRESS_SPACE_TRAMP_PDE_COUNT   ((ADDRESS_SPACE_TRAMP_COPY_TOP - ADDRESS_SPACE_TRAMP_BASE) >> 22)

struct address_space {
    uint32_t *page_directory;
    uint32_t **owned_page_tables;
    struct address_space *next;
};

typedef struct reusable_page {
    struct reusable_page *next;
} reusable_page_t;

static spinlock_t s_address_space_lock = {0};
static reusable_page_t *s_reusable_pages;
static uint32_t *s_kernel_page_directory;
static address_space_t *s_space_list = NULL;
static uint32_t *s_tramp_shared_tables[ADDRESS_SPACE_TRAMP_PDE_COUNT];
static uint32_t s_tramp_audio_bottom = ADDRESS_SPACE_TRAMP_AUDIO_TOP;
static uint32_t s_tramp_copy_bottom = ADDRESS_SPACE_TRAMP_COPY_TOP;
static bool s_tramp_initialized = false;

static uint32_t address_space_lock(void) {
    return spin_lock_irqsave(&s_address_space_lock);
}

static void address_space_unlock(uint32_t flags) {
    spin_unlock_irqrestore(&s_address_space_lock, flags);
}

void address_space_force_unlock(void) {
    spin_unlock(&s_address_space_lock);
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
                table[i] = (physical_base + i * PAGE_SIZE) | flags;
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

static void tramp_sync_pde_locked(uint32_t dir_idx, uint32_t pde_val) {
    if (s_kernel_page_directory) {
        s_kernel_page_directory[dir_idx] = pde_val;
    }
    for (address_space_t *s = s_space_list; s; s = s->next) {
        if (s->page_directory) {
            s->page_directory[dir_idx] = pde_val;
        }
    }
}

static bool tramp_map_page_locked(uint32_t page_vaddr) {
    if (page_vaddr < ADDRESS_SPACE_TRAMP_BASE || page_vaddr >= ADDRESS_SPACE_TRAMP_COPY_TOP) {
        return false;
    }
    uint32_t dir_idx = page_vaddr >> 22;
    uint32_t slot = dir_idx - ADDRESS_SPACE_TRAMP_PDE_START;
    uint32_t table_idx = (page_vaddr >> 12) & 0x3FFu;

    uint32_t *table = s_tramp_shared_tables[slot];
    if (!table) {
        table = allocate_page();
        if (!table) return false;
        s_tramp_shared_tables[slot] = table;
        uint32_t pde_val = ((uint32_t)(uintptr_t)table & PDE_ADDRESS) | PDE_PRESENT | PDE_WRITABLE;
        tramp_sync_pde_locked(dir_idx, pde_val);
    }

    if (table[table_idx] & PTE_PRESENT) {
        return true;
    }

    void *frame = memory_page_alloc();
    if (!frame) return false;

    table[table_idx] = ((uint32_t)(uintptr_t)frame & PAGE_MASK) | PTE_PRESENT | PTE_WRITABLE;
    __asm__ volatile("invlpg (%0)" : : "r"(page_vaddr) : "memory");
    return true;
}

static void tramp_init_locked(void) {
    if (s_tramp_initialized) return;
    if (!(read_cr0() & 0x80000000u)) return;

    if (!s_kernel_page_directory) {
        s_kernel_page_directory = (uint32_t *)(uintptr_t)(read_cr3() & PAGE_MASK);
    }

    /* Replace identity 4 MiB PSE mappings in [0xC0000000, 0xD0000000) with shared demand-paged slots */
    for (uint32_t i = 0; i < ADDRESS_SPACE_TRAMP_PDE_COUNT; i++) {
        uint32_t dir_idx = ADDRESS_SPACE_TRAMP_PDE_START + i;
        s_tramp_shared_tables[i] = NULL;
        tramp_sync_pde_locked(dir_idx, 0u);
    }

    s_tramp_audio_bottom = ADDRESS_SPACE_TRAMP_AUDIO_TOP - PAGE_SIZE;
    tramp_map_page_locked(s_tramp_audio_bottom);

    s_tramp_copy_bottom = ADDRESS_SPACE_TRAMP_COPY_TOP - PAGE_SIZE;
    tramp_map_page_locked(s_tramp_copy_bottom);

    s_tramp_initialized = true;
    uint32_t cr3 = read_cr3();
    __asm__ volatile("movl %0, %%cr3" : : "r"(cr3) : "memory");
}

void address_space_trampoline_init(void) {
    uint32_t irq_flags = address_space_lock();
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

bool address_space_is_trampoline_addr(uint32_t virtual_address) {
    return (virtual_address >= ADDRESS_SPACE_TRAMP_BASE &&
            virtual_address < ADDRESS_SPACE_TRAMP_COPY_TOP);
}

bool address_space_handle_trampoline_fault(uint32_t fault_address,
                                            uint32_t error_code,
                                            uint32_t saved_esp) {
    if ((error_code & 1u) != 0u) return false;
    if (!address_space_is_trampoline_addr(fault_address)) return false;

    uint32_t irq_flags = address_space_lock();
    if (!s_tramp_initialized) {
        address_space_unlock(irq_flags);
        return false;
    }

    bool in_audio = (fault_address < ADDRESS_SPACE_TRAMP_AUDIO_TOP);
    uint32_t reg_base = in_audio ? ADDRESS_SPACE_TRAMP_BASE : ADDRESS_SPACE_TRAMP_COPY_BASE;
    uint32_t reg_top = in_audio ? ADDRESS_SPACE_TRAMP_AUDIO_TOP : ADDRESS_SPACE_TRAMP_COPY_TOP;
    uint32_t *bottom_ptr = in_audio ? &s_tramp_audio_bottom : &s_tramp_copy_bottom;

    uint32_t fault_page = fault_address & PAGE_MASK;
    uint32_t esp_page = saved_esp & PAGE_MASK;
    uint32_t target_lowest = fault_page;
    if (saved_esp >= reg_base && saved_esp < reg_top && esp_page < target_lowest) {
        target_lowest = esp_page;
    }

    if (target_lowest < reg_base) {
        address_space_unlock(irq_flags);
        return false;
    }

    /* If already mapped in shared table (stale TLB on current CPU), invalidate and resume */
    if (fault_page >= *bottom_ptr) {
        uint32_t slot = (fault_page >> 22) - ADDRESS_SPACE_TRAMP_PDE_START;
        uint32_t *tbl = s_tramp_shared_tables[slot];
        if (tbl && (tbl[(fault_page >> 12) & 0x3FFu] & PTE_PRESENT)) {
            uint32_t cur_cr3 = read_cr3();
            uint32_t dir_idx = fault_page >> 22;
            uint32_t *cur_pd = (uint32_t *)(uintptr_t)(cur_cr3 & PAGE_MASK);
            cur_pd[dir_idx] = ((uint32_t)(uintptr_t)tbl & PDE_ADDRESS) | PDE_PRESENT | PDE_WRITABLE;
            __asm__ volatile("invlpg (%0)" : : "r"(fault_address) : "memory");
            address_space_unlock(irq_flags);
            return true;
        }
    }

    for (uint32_t page = *bottom_ptr - PAGE_SIZE; ; page -= PAGE_SIZE) {
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
    if (!(read_cr0() & 0x80000000u)) return NULL;

    uint32_t irq_flags = address_space_lock();
    uint32_t active_directory = read_cr3() & PAGE_MASK;
    if (!s_kernel_page_directory) {
        s_kernel_page_directory = (uint32_t *)(uintptr_t)active_directory;
    }
    tramp_init_locked();

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
    space->next = s_space_list;
    s_space_list = space;
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

    address_space_t **link = &s_space_list;
    while (*link) {
        if (*link == space) {
            *link = space->next;
            break;
        }
        link = &(*link)->next;
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
    syscall_update_exception_page_directory();
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
    syscall_update_exception_page_directory();
    address_space_unlock(irq_flags);
    return true;
}

void __attribute__((noinline, used)) _address_space_trampoline_inner(
    uint32_t new_cr3, uint32_t old_cr3, void (*fn)(void *), void *arg) {
    uint32_t irq_flags = address_space_lock();
    __asm__ volatile("movl %0, %%cr3" : : "r"(new_cr3) : "memory");
    syscall_update_exception_page_directory();
    address_space_unlock(irq_flags);

    fn(arg);

    irq_flags = address_space_lock();
    __asm__ volatile("movl %0, %%cr3" : : "r"(old_cr3) : "memory");
    syscall_update_exception_page_directory();
    address_space_unlock(irq_flags);
}

__attribute__((naked)) static void address_space_trampoline_shim(
    uint32_t new_cr3, uint32_t old_cr3, void (*fn)(void *), void *arg,
    uint32_t kstack_top) {
    __asm__ volatile(
        "movl 4(%%esp), %%eax\n\t"
        "movl 8(%%esp), %%ecx\n\t"
        "movl 12(%%esp), %%edx\n\t"
        "pushl %%esi\n\t"
        "pushl %%edi\n\t"
        "movl 24(%%esp), %%esi\n\t"
        "movl 28(%%esp), %%edi\n\t"
        "andl $0xFFFFFFF0, %%edi\n\t"
        "subl $16, %%edi\n\t"
        "movl %%esp, (%%edi)\n\t"
        "movl %%edi, %%esp\n\t"
        "pushl %%esi\n\t"
        "pushl %%edx\n\t"
        "pushl %%ecx\n\t"
        "pushl %%eax\n\t"
        "call _address_space_trampoline_inner\n\t"
        "addl $16, %%esp\n\t"
        "popl %%esp\n\t"
        "popl %%edi\n\t"
        "popl %%esi\n\t"
        "ret\n\t"
        : : : "memory"
    );
}

void address_space_call_on_stack(address_space_t *target, void (*fn)(void *),
                                 void *ctx, void *kstack_top) {
    if (!fn) return;
    uint32_t cur_cr3 = read_cr3();
    uint32_t target_cr3 = (target && target->page_directory)
                              ? (uint32_t)(uintptr_t)target->page_directory
                              : (uint32_t)(uintptr_t)s_kernel_page_directory;

    if (target_cr3 == 0u || cur_cr3 == target_cr3) {
        fn(ctx);
        return;
    }

    uint32_t cur_esp = 0u;
    __asm__ volatile("movl %%esp, %0" : "=r"(cur_esp));

    /* If already running on a low kernel stack (< 256 MiB) or shared trampoline stack, CR3 switch is safe directly. */
    if (cur_esp < 0x10000000u ||
        address_space_is_trampoline_addr(cur_esp) ||
        !kstack_top) {
        _address_space_trampoline_inner(target_cr3, cur_cr3, fn, ctx);
        return;
    }

    uint32_t aligned_top = ((uint32_t)(uintptr_t)kstack_top) & ~0xFu;
    address_space_trampoline_shim(target_cr3, cur_cr3, fn, ctx, aligned_top);
}

void address_space_call_with_copy(address_space_t *target, void (*fn)(void *),
                                  void *ctx, size_t ctx_size) {
    if (!fn) return;
    uint32_t cur_cr3 = read_cr3();
    uint32_t target_cr3 = (target && target->page_directory)
                              ? (uint32_t)(uintptr_t)target->page_directory
                              : (uint32_t)(uintptr_t)s_kernel_page_directory;

    if (target_cr3 == 0u || cur_cr3 == target_cr3) {
        fn(ctx);
        return;
    }

    uint32_t cur_esp = 0u;
    __asm__ volatile("movl %%esp, %0" : "=r"(cur_esp));

    /* If already running on a low kernel stack (< 256 MiB) or shared trampoline stack, CR3 switch is safe directly. */
    if (cur_esp < 0x10000000u || address_space_is_trampoline_addr(cur_esp)) {
        _address_space_trampoline_inner(target_cr3, cur_cr3, fn, ctx);
        return;
    }

    /* Running on a per-process virtual stack: copy ctx into shared kernel heap and
     * execute on the dynamically demand-paged shared kernel trampoline stack. */
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
        bool is_tramp_pde = (dir_idx >= ADDRESS_SPACE_TRAMP_PDE_START &&
                             dir_idx < ADDRESS_SPACE_TRAMP_PDE_START + ADDRESS_SPACE_TRAMP_PDE_COUNT);

        if (dir_idx > 3u && fully_covered && !is_tramp_pde) {
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
        bool is_tramp_pde = (dir_idx >= ADDRESS_SPACE_TRAMP_PDE_START &&
                             dir_idx < ADDRESS_SPACE_TRAMP_PDE_START + ADDRESS_SPACE_TRAMP_PDE_COUNT);

        if (dir_idx > 3u && fully_covered && !is_tramp_pde) {
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
    for (uint64_t page = (uint64_t)(virtual_address & PAGE_MASK); page < end; page += PAGE_SIZE) {
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

uint32_t address_space_get_physical(address_space_t *space, uint32_t virtual_address) {
    if (!space || !space->page_directory) return 0u;
    uint32_t irq_flags = address_space_lock();
    uint32_t dir_index = virtual_address >> 22;
    uint32_t pde = space->page_directory[dir_index];
    if (!(pde & PDE_PRESENT)) {
        address_space_unlock(irq_flags);
        return 0u;
    }
    if (pde & PDE_PAGE_SIZE) {
        address_space_unlock(irq_flags);
        return (pde & PDE_LARGE_BASE) + (virtual_address & 0x003FFFFFu);
    }
    uint32_t *table = space->owned_page_tables[dir_index];
    if (!table) {
        table = (uint32_t *)(uintptr_t)(pde & PDE_ADDRESS);
    }
    if (!table) {
        address_space_unlock(irq_flags);
        return 0u;
    }
    uint32_t pte = table[(virtual_address >> 12) & 0x3FFu];
    address_space_unlock(irq_flags);
    if (!(pte & PTE_PRESENT)) return 0u;
    return (pte & PAGE_MASK) | (virtual_address & (PAGE_SIZE - 1u));
}

extern uint8_t _kernel_text_start[];
extern uint8_t _kernel_rodata_end[];

void paging_init(void) {
    uint32_t *page_dir = (uint32_t *)memory_map_alloc(PAGE_SIZE, PAGE_SIZE);
    uint32_t *pt_low   = (uint32_t *)memory_map_alloc(4u * PAGE_SIZE, PAGE_SIZE);
    if (!page_dir || !pt_low) {
        serial_printf("[paging] Failed to allocate page tables\n");
        return;
    }

    memset(page_dir, 0, PAGE_SIZE);
    memset(pt_low, 0, 4u * PAGE_SIZE);

    uint32_t ktext_start = (uint32_t)(uintptr_t)_kernel_text_start;
    uint32_t krodata_end = ((uint32_t)(uintptr_t)_kernel_rodata_end) & ~0xFFFu;

    /* Identity-map first 16 MiB via 4 KiB pages with low guard holes and RO kernel sections */
    for (uint32_t page = 0; page < 4096u; page++) {
        uint32_t addr = page * PAGE_SIZE;
        if (page == 0 || (page >= 2 && page <= 5) || page == 7 || (page >= 9 && page <= 15)) {
            pt_low[page] = 0;
        } else if (addr >= ktext_start && addr < krodata_end) {
            pt_low[page] = addr | PTE_PRESENT;
        } else if (addr >= 0x000C0000u && addr < 0x00100000u) {
            pt_low[page] = addr | PTE_PRESENT;
        } else {
            pt_low[page] = addr | PTE_PRESENT | PTE_WRITABLE;
        }
    }

    for (uint32_t pde = 0; pde < 4u; pde++) {
        page_dir[pde] = ((uint32_t)(uintptr_t)&pt_low[pde * 1024u]) | PDE_PRESENT | PDE_WRITABLE;
    }

    /* Identity-map 16 MiB .. 4 GiB using 4 MiB PSE pages */
    for (uint32_t pde = 4; pde < 1024u; pde++) {
        page_dir[pde] = (pde << 22) | PDE_PRESENT | PDE_WRITABLE | PDE_PAGE_SIZE;
    }

    __asm__ volatile(
        "movl %%cr4, %%eax\n\t"
        "orl $0x00000010, %%eax\n\t"
        "movl %%eax, %%cr4\n\t"
        "movl %0, %%cr3\n\t"
        "movl %%cr0, %%eax\n\t"
        "orl $0x80010001, %%eax\n\t"
        "movl %%eax, %%cr0\n\t"
        :
        : "r"(page_dir)
        : "eax", "memory"
    );

    address_space_trampoline_init();

    serial_printf("[paging] Paging enabled (NULL guard active, kernel .text/.rodata write-protected [0x%x - 0x%x])\n",
                  ktext_start, krodata_end);
}
