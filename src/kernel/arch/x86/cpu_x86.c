#include <arch/cpu_regs.h>
#include <kernel/cpu_info.h>
#include <memory/kmalloc.h>
#include "x86_intrinsics.h"

#define X86_CR0_PE          (1ULL << 0)
#define X86_CR0_MP          (1ULL << 1)
#define X86_CR0_EM          (1ULL << 2)
#define X86_CR0_TS          (1ULL << 3)
#define X86_CR0_NE          (1ULL << 5)
#define X86_CR0_WP          (1ULL << 16)
#define X86_CR0_PG          (1ULL << 31)

#define X86_CR4_PSE         (1ULL << 4)
#define X86_CR4_PAE         (1ULL << 5)
#define X86_CR4_OSFXSR      (1ULL << 9)
#define X86_CR4_OSXMMEXCPT  (1ULL << 10)
#define X86_CR4_LA57        (1ULL << 12)

#define X86_EFER_MSR        0xC0000080u
#define X86_GS_BASE_MSR     0xC0000101u
#define X86_EFER_LMA        (1ULL << 10)

#define X86_FLAGS_IF        (1ULL << 9)

void arch_reg_bind_allocator(const ipo_mem_ops_t *mem);

static void *x86_mem_alloc(ipo_size_t sz, void *ctx) {
    (void)ctx;
    return kmalloc(sz);
}

static void *x86_mem_realloc(void *p, ipo_size_t old_sz, ipo_size_t new_sz, void *ctx) {
    (void)old_sz;
    (void)ctx;
    return krealloc(p, new_sz);
}

static void x86_mem_free(void *p, void *ctx) {
    (void)ctx;
    if (p) kfree(p);
}

static const ipo_mem_ops_t s_x86_mem_ops = {
    .alloc = x86_mem_alloc,
    .realloc_fn = x86_mem_realloc,
    .free_fn = x86_mem_free,
    .ctx = NULL
};

enum {
    X86_HW_REG_FLAGS = 1u,
    X86_HW_REG_SP,
    X86_HW_REG_CS,
    X86_HW_REG_CR0,
    X86_HW_REG_CR2,
    X86_HW_REG_CR3,
    X86_HW_REG_CR4,
    X86_HW_REG_EFER,
    X86_HW_REG_TSC,
    X86_HW_REG_GSBASE
};

static arch_reg_desc_t s_x86_reg_descs[] = {
    {
        .reg_id = 1u, .name = "FLAGS",
        .purpose = "Processor status & interrupt enable flags",
        .role = ARCH_REG_ROLE_STATUS_FLAGS,
        .bit_width = 32u,
        .access_flags = ARCH_REG_ACCESS_READABLE | ARCH_REG_ACCESS_WRITABLE | ARCH_REG_ACCESS_VOLATILE,
        .hw_encoding = X86_HW_REG_FLAGS
    },
    {
        .reg_id = 2u, .name = "SP",
        .purpose = "Current hardware stack pointer",
        .role = ARCH_REG_ROLE_STACK_PTR,
        .bit_width = 32u,
        .access_flags = ARCH_REG_ACCESS_READABLE | ARCH_REG_ACCESS_WRITABLE | ARCH_REG_ACCESS_VOLATILE,
        .hw_encoding = X86_HW_REG_SP
    },
    {
        .reg_id = 3u, .name = "CS",
        .purpose = "Active code segment selector",
        .role = ARCH_REG_ROLE_CODE_SEG,
        .bit_width = 16u,
        .access_flags = ARCH_REG_ACCESS_READABLE | ARCH_REG_ACCESS_PRIVILEGED,
        .hw_encoding = X86_HW_REG_CS
    },
    {
        .reg_id = 4u, .name = "CR0",
        .purpose = "System control (PE/MP/EM/TS/NE/WP/PG)",
        .role = ARCH_REG_ROLE_MMU_CTRL,
        .bit_width = 32u,
        .access_flags = ARCH_REG_ACCESS_READABLE | ARCH_REG_ACCESS_WRITABLE | ARCH_REG_ACCESS_PRIVILEGED,
        .hw_encoding = X86_HW_REG_CR0
    },
    {
        .reg_id = 5u, .name = "CR2",
        .purpose = "Page fault linear virtual address",
        .role = ARCH_REG_ROLE_MMU_FAULT_ADDR,
        .bit_width = 32u,
        .access_flags = ARCH_REG_ACCESS_READABLE | ARCH_REG_ACCESS_PRIVILEGED,
        .hw_encoding = X86_HW_REG_CR2
    },
    {
        .reg_id = 6u, .name = "CR3",
        .purpose = "Active translation table root physical address",
        .role = ARCH_REG_ROLE_MMU_ROOT,
        .bit_width = 32u,
        .access_flags = ARCH_REG_ACCESS_READABLE | ARCH_REG_ACCESS_WRITABLE | ARCH_REG_ACCESS_PRIVILEGED,
        .hw_encoding = X86_HW_REG_CR3
    },
    {
        .reg_id = 7u, .name = "CR4",
        .purpose = "Architectural extension control (PSE/PAE/OSFXSR/LA57)",
        .role = ARCH_REG_ROLE_EXT_CTRL,
        .bit_width = 32u,
        .access_flags = ARCH_REG_ACCESS_READABLE | ARCH_REG_ACCESS_WRITABLE | ARCH_REG_ACCESS_PRIVILEGED | ARCH_REG_ACCESS_MODE_SCOPED,
        .hw_encoding = X86_HW_REG_CR4
    },
    {
        .reg_id = 8u, .name = "IA32_EFER",
        .purpose = "Extended feature enable MSR (SCE/LME/LMA/NXE)",
        .role = ARCH_REG_ROLE_MODE_ENABLE,
        .bit_width = 64u,
        .access_flags = ARCH_REG_ACCESS_READABLE | ARCH_REG_ACCESS_WRITABLE | ARCH_REG_ACCESS_PRIVILEGED | ARCH_REG_ACCESS_MODE_SCOPED,
        .hw_encoding = X86_HW_REG_EFER
    },
    {
        .reg_id = 9u, .name = "TSC",
        .purpose = "64-bit hardware time-stamp counter",
        .role = ARCH_REG_ROLE_TIMESTAMP,
        .bit_width = 64u,
        .access_flags = ARCH_REG_ACCESS_READABLE | ARCH_REG_ACCESS_VOLATILE,
        .hw_encoding = X86_HW_REG_TSC
    },
    {
        .reg_id = 10u, .name = "IA32_GS_BASE",
        .purpose = "64-bit GS segment linear base MSR",
        .role = ARCH_REG_ROLE_TLS_BASE,
        .bit_width = 64u,
        .access_flags = ARCH_REG_ACCESS_READABLE | ARCH_REG_ACCESS_WRITABLE | ARCH_REG_ACCESS_PRIVILEGED | ARCH_REG_ACCESS_MODE_SCOPED,
        .hw_encoding = X86_HW_REG_GSBASE
    }
};

static bool x86_has_efer_msr(void) {
    if (!x86_has_cpuid()) return false;
    uint32_t eax = 0u, ebx = 0u, ecx = 0u, edx = 0u;
    x86_cpuid(0x80000000u, 0u, &eax, &ebx, &ecx, &edx);
    if (eax < 0x80000001u) return false;
    x86_cpuid(0x80000001u, 0u, &eax, &ebx, &ecx, &edx);
    return ((edx & ((1u << 20) | (1u << 29))) != 0u);
}

static bool x86_is_long_mode_hw(void) {
    if ((x86_read_cr0() & (ipo_reg_t)X86_CR0_PG) == 0u) return false;
    if ((x86_read_cr4() & (ipo_reg_t)X86_CR4_PAE) == 0u) return false;
    return x86_has_efer_msr() && ((x86_rdmsr(X86_EFER_MSR) & X86_EFER_LMA) != 0ULL);
}

static void x86_sync_reg_widths(void) {
    ipo_bitwidth_t w = x86_is_long_mode_hw() ? 64u : 32u;
    s_x86_reg_descs[0].bit_width = w; /* FLAGS */
    s_x86_reg_descs[1].bit_width = w; /* SP */
    s_x86_reg_descs[3].bit_width = w; /* CR0 */
    s_x86_reg_descs[4].bit_width = w; /* CR2 */
    s_x86_reg_descs[5].bit_width = w; /* CR3 */
    s_x86_reg_descs[6].bit_width = w; /* CR4 */
}

static ipo_size_t x86_reg_count(void) {
    return (ipo_size_t)(sizeof(s_x86_reg_descs) / sizeof(s_x86_reg_descs[0]));
}

static const arch_reg_desc_t *x86_reg_get(ipo_size_t index) {
    if (index >= x86_reg_count()) return NULL;
    x86_sync_reg_widths();
    return &s_x86_reg_descs[index];
}

static bool x86_reg_is_available(const arch_reg_desc_t *desc) {
    if (!desc) return false;
    switch (desc->hw_encoding) {
        case X86_HW_REG_CR4:
            return x86_has_cpuid();
        case X86_HW_REG_EFER:
            return x86_has_efer_msr();
        case X86_HW_REG_GSBASE:
            return x86_is_long_mode_hw();
        case X86_HW_REG_TSC:
            return x86_has_cpuid();
        default:
            return true;
    }
}

static int x86_reg_read_raw_u64(uint32_t hw_encoding, uint64_t *out_val) {
    if (!out_val) return -1;
    switch (hw_encoding) {
        case X86_HW_REG_FLAGS:  *out_val = (uint64_t)x86_read_flags(); return 0;
        case X86_HW_REG_SP:     *out_val = (uint64_t)x86_read_sp(); return 0;
        case X86_HW_REG_CS:     *out_val = (uint64_t)x86_read_cs(); return 0;
        case X86_HW_REG_CR0:    *out_val = (uint64_t)x86_read_cr0(); return 0;
        case X86_HW_REG_CR2:    *out_val = (uint64_t)x86_read_cr2(); return 0;
        case X86_HW_REG_CR3:    *out_val = (uint64_t)x86_read_cr3(); return 0;
        case X86_HW_REG_CR4:    *out_val = (uint64_t)x86_read_cr4(); return 0;
        case X86_HW_REG_EFER:
            if (!x86_has_efer_msr()) return -3;
            *out_val = x86_rdmsr(X86_EFER_MSR);
            return 0;
        case X86_HW_REG_TSC:
            *out_val = x86_rdtsc();
            return 0;
        case X86_HW_REG_GSBASE:
            if (!x86_is_long_mode_hw()) return -3;
            *out_val = x86_rdmsr(X86_GS_BASE_MSR);
            return 0;
        default:
            return -1;
    }
}

static int x86_reg_write_raw_u64(uint32_t hw_encoding, uint64_t val) {
    switch (hw_encoding) {
        case X86_HW_REG_FLAGS:  x86_write_flags((ipo_reg_t)val); return 0;
        case X86_HW_REG_SP:     x86_write_sp((ipo_vaddr_t)val); return 0;
        case X86_HW_REG_CR0:    x86_write_cr0((ipo_reg_t)val); return 0;
        case X86_HW_REG_CR3:    x86_write_cr3((ipo_reg_t)val); return 0;
        case X86_HW_REG_CR4:    x86_write_cr4((ipo_reg_t)val); return 0;
        case X86_HW_REG_EFER:
            if (!x86_has_efer_msr()) return -3;
            x86_wrmsr(X86_EFER_MSR, val);
            return 0;
        case X86_HW_REG_GSBASE:
            if (!x86_is_long_mode_hw()) return -3;
            x86_wrmsr(X86_GS_BASE_MSR, val);
            return 0;
        default:
            return -2;
    }
}

static int x86_reg_read(const arch_reg_desc_t *desc, arch_reg_value_t *out_val) {
    if (!desc || !out_val) return -1;
    uint64_t raw = 0ULL;
    int rc = x86_reg_read_raw_u64(desc->hw_encoding, &raw);
    if (rc < 0) return rc;
    x86_sync_reg_widths();
    arch_reg_value_from_u64(out_val, desc->bit_width, raw);
    return 0;
}

static int x86_reg_write(const arch_reg_desc_t *desc, const arch_reg_value_t *val) {
    if (!desc || !val) return -1;
    return x86_reg_write_raw_u64(desc->hw_encoding, arch_reg_value_as_u64(val));
}

static int x86_reg_read_by_role(arch_reg_role_t role, uint64_t *out_val) {
    if (!out_val) return -1;
    for (ipo_size_t i = 0u; i < x86_reg_count(); i++) {
        const arch_reg_desc_t *d = &s_x86_reg_descs[i];
        if (d->role == role && x86_reg_is_available(d)) {
            return x86_reg_read_raw_u64(d->hw_encoding, out_val);
        }
    }
    return -3;
}

static int x86_reg_write_by_role(arch_reg_role_t role, uint64_t val) {
    for (ipo_size_t i = 0u; i < x86_reg_count(); i++) {
        const arch_reg_desc_t *d = &s_x86_reg_descs[i];
        if (d->role == role && x86_reg_is_available(d)) {
            if ((d->access_flags & ARCH_REG_ACCESS_WRITABLE) == 0u) return -2;
            return x86_reg_write_raw_u64(d->hw_encoding, val);
        }
    }
    return -3;
}

static const arch_reg_backend_ops_t s_x86_reg_ops = {
    .arch_name = "x86",
    .reg_count = x86_reg_count,
    .reg_get = x86_reg_get,
    .reg_is_available = x86_reg_is_available,
    .reg_read = x86_reg_read,
    .reg_write = x86_reg_write,
    .reg_read_by_role = x86_reg_read_by_role,
    .reg_write_by_role = x86_reg_write_by_role
};

void arch_cpu_backend_init(void) {
    x86_sync_reg_widths();
    arch_reg_bind_allocator(&s_x86_mem_ops);
    arch_reg_bind_backend(&s_x86_reg_ops);
}

void arch_irq_enable(void) {
    x86_irq_enable();
}

void arch_irq_disable(void) {
    x86_irq_disable();
}

bool arch_irq_is_enabled(void) {
    return (x86_read_flags() & (ipo_reg_t)X86_FLAGS_IF) != 0u;
}

ipo_reg_t arch_irq_save_regs(void) {
    return x86_irq_save();
}

void arch_irq_restore_regs(ipo_reg_t saved_flags) {
    x86_irq_restore(saved_flags);
}

void arch_cpu_pause(void) {
    x86_pause();
}

void arch_cpu_halt(void) {
    x86_halt();
}

void __attribute__((noreturn)) arch_cpu_halt_forever(void) {
    for (;;) {
        x86_irq_disable();
        x86_halt();
    }
}

void arch_cpu_serialize(void) {
    if (x86_has_cpuid()) {
        uint32_t eax = 0u, ebx = 0u, ecx = 0u, edx = 0u;
        x86_cpuid(0u, 0u, &eax, &ebx, &ecx, &edx);
    } else {
        __asm__ volatile("" ::: "memory");
    }
}

uint64_t arch_cpu_read_timestamp(void) {
    return x86_rdtsc();
}

ipo_vaddr_t arch_stack_get_sp(void) {
    return x86_read_sp();
}

__attribute__((naked)) void arch_stack_set_sp(ipo_vaddr_t sp) {
    (void)sp;
    __asm__ volatile(
        "movl (%esp), %ecx\n\t"
        "movl 4(%esp), %eax\n\t"
        "subl $16, %eax\n\t"
        "movl %eax, %esp\n\t"
        "jmp *%ecx\n\t"
    );
}

ipo_reg_t arch_cpu_wp_disable(void) {
    ipo_reg_t cr0 = x86_read_cr0();
    x86_write_cr0(cr0 & ~(ipo_reg_t)X86_CR0_WP);
    return cr0;
}

void arch_cpu_wp_restore(ipo_reg_t token) {
    x86_write_cr0(token);
}

void arch_cpu_atomic_patch_bytes(ipo_vaddr_t dst_vaddr, const void *src, ipo_size_t len) {
    if (!src || len == 0u) return;
    ipo_reg_t saved_cr0 = arch_cpu_wp_disable();
    const uint8_t *s = (const uint8_t *)src;
    if (len <= 8u) {
        volatile uint64_t *ptr64 = (volatile uint64_t *)(uintptr_t)dst_vaddr;
        uint64_t old_val = *ptr64;
        uint64_t new_val = old_val;
        uint8_t *nv_bytes = (uint8_t *)&new_val;
        for (ipo_size_t i = 0u; i < len; i++) nv_bytes[i] = s[i];
        uint32_t lo = (uint32_t)(new_val & 0xFFFFFFFFULL);
        uint32_t hi = (uint32_t)(new_val >> 32);
        uint32_t exp_lo = (uint32_t)(old_val & 0xFFFFFFFFULL);
        uint32_t exp_hi = (uint32_t)(old_val >> 32);
        __asm__ volatile(
            "lock cmpxchg8b (%0)"
            :
            : "S"(ptr64), "a"(exp_lo), "d"(exp_hi), "b"(lo), "c"(hi)
            : "memory", "cc"
        );
    } else {
        uint8_t *d = (uint8_t *)(uintptr_t)dst_vaddr;
        for (ipo_size_t i = 0u; i < len; i++) d[i] = s[i];
    }
    arch_cpu_wp_restore(saved_cr0);
}

void arch_cpu_enable_fpu_simd(void) {
    ipo_reg_t cr0 = x86_read_cr0();
    cr0 &= ~(ipo_reg_t)(X86_CR0_EM | X86_CR0_TS);
    cr0 |= (ipo_reg_t)(X86_CR0_MP | X86_CR0_NE);
    x86_write_cr0(cr0);

    if (x86_has_cpuid()) {
        uint32_t eax = 0u, ebx = 0u, ecx = 0u, edx = 0u;
        x86_cpuid(1u, 0u, &eax, &ebx, &ecx, &edx);
        if (edx & (1u << 24)) {
            ipo_reg_t cr4 = x86_read_cr4();
            cr4 |= (ipo_reg_t)X86_CR4_OSFXSR;
            if (edx & (1u << 25)) {
                cr4 |= (ipo_reg_t)X86_CR4_OSXMMEXCPT;
            }
            x86_write_cr4(cr4);
        }
    }
    x86_fninit();
}

void arch_cpu_clear_fpu_task_switched(void) {
    x86_clts();
}

void arch_cpu_set_tls_base(ipo_vaddr_t tls_base) {
    if (x86_is_long_mode_hw()) {
        x86_wrmsr(X86_GS_BASE_MSR, (uint64_t)tls_base);
    }
}

ipo_paddr_t arch_mmu_get_root(void) {
    uint32_t levels = arch_mmu_get_paging_levels();
    uint64_t raw = (uint64_t)x86_read_cr3();
    if (levels >= 4u) return (ipo_paddr_t)(raw & 0x000FFFFFFFFFF000ULL);
    return (ipo_paddr_t)(raw & 0xFFFFF000ULL);
}

void arch_mmu_set_root(ipo_paddr_t root_paddr) {
    x86_write_cr3((ipo_reg_t)root_paddr);
}

void arch_mmu_flush_all(void) {
    x86_write_cr3(x86_read_cr3());
}

void arch_mmu_flush_page(ipo_vaddr_t vaddr) {
    x86_invlpg(vaddr);
}

ipo_vaddr_t arch_mmu_get_fault_addr(void) {
    return x86_read_cr2();
}

uint32_t arch_mmu_get_paging_levels(void) {
    if ((x86_read_cr0() & (ipo_reg_t)X86_CR0_PG) == 0u) return 0u;
    if (x86_is_long_mode_hw()) {
        return (x86_read_cr4() & (ipo_reg_t)X86_CR4_LA57) ? 5u : 4u;
    }
    return 2u;
}

bool arch_mmu_is_paging_enabled(void) {
    return (x86_read_cr0() & (ipo_reg_t)X86_CR0_PG) != 0u;
}

void arch_mmu_enable_paging(ipo_paddr_t root_paddr, uint32_t paging_levels) {
    ipo_reg_t cr4 = x86_read_cr4();
    if (paging_levels < 4u) {
        cr4 |= (ipo_reg_t)X86_CR4_PSE;
        x86_write_cr4(cr4);
    } else if (paging_levels == 5u && !x86_is_long_mode_hw()) {
        cr4 |= (ipo_reg_t)X86_CR4_LA57;
        x86_write_cr4(cr4);
    }
    x86_write_cr3((ipo_reg_t)root_paddr);
    x86_write_cr0(x86_read_cr0() | (ipo_reg_t)(X86_CR0_PE | X86_CR0_WP | X86_CR0_PG));
}

bool arch_mmu_is_vaddr_mapped(ipo_vaddr_t vaddr) {
    if (vaddr < 0x1000u) return false;
    if (!arch_mmu_is_paging_enabled()) {
        return vaddr >= 0x10000u;
    }
    uint32_t levels = arch_mmu_get_paging_levels();
    ipo_paddr_t root = arch_mmu_get_root();
    if (levels >= 3u) {
        const uint64_t *tbl = (const uint64_t *)(uintptr_t)(root & ~0xFFFULL);
        uint64_t v = (uint64_t)vaddr;
        for (int lvl = (int)levels - 1; lvl >= 0; lvl--) {
            unsigned shift = 12u + 9u * (unsigned)lvl;
            uint64_t entry = tbl[(v >> shift) & 0x1FFu];
            if (!(entry & 1ULL)) return false;
            if (lvl == 0 || (entry & (1ULL << 7))) return true;
            tbl = (const uint64_t *)(uintptr_t)(entry & 0x000FFFFFFFFFF000ULL);
        }
        return false;
    }
    const uint32_t *pd = (const uint32_t *)(uintptr_t)(root & 0xFFFFF000ULL);
    uint32_t v32 = (uint32_t)vaddr;
    uint32_t pde = pd[v32 >> 22];
    if (!(pde & 1u)) return false;
    if (pde & 0x80u) return true;
    const uint32_t *pt = (const uint32_t *)(uintptr_t)(pde & 0xFFFFF000u);
    uint32_t pte = pt[(v32 >> 12) & 0x3FFu];
    return (pte & 1u) != 0u;
}

void arch_cpu_fill_runtime_summary(ipo_cpu_runtime_summary_t *summary,
                                   const cpu_hw_facts_t *facts) {
    if (!summary) return;
    uint64_t cr0 = (uint64_t)x86_read_cr0();
    uint64_t cr3 = (uint64_t)x86_read_cr3();
    uint64_t cr4 = x86_has_cpuid() ? (uint64_t)x86_read_cr4() : 0ULL;
    uint64_t efer = x86_has_efer_msr() ? x86_rdmsr(X86_EFER_MSR) : 0ULL;

    summary->verified_cr0 = cr0;
    summary->verified_cr3 = cr3;
    summary->verified_cr4 = cr4;
    summary->verified_efer = efer;
    if (summary->verified_cs == 0u) {
        summary->verified_cs = x86_read_cs();
    }

    summary->paging_levels = arch_mmu_get_paging_levels();

    if (summary->paging_levels >= 3u) {
        summary->vaddr_bits = (ipo_bitwidth_t)(12u + 9u * summary->paging_levels);
        ipo_bitwidth_t exec_w = 64u;
        while (exec_w < summary->vaddr_bits) exec_w <<= 1u;
        summary->exec_bits = exec_w;
        summary->gpr_bits = exec_w;
        summary->ptr_bits = exec_w;
        summary->paddr_bits = (facts && facts->phys_addr_bits) ? facts->phys_addr_bits : 40u;
        summary->switch_verified = ((efer & X86_EFER_LMA) != 0ULL) &&
                                   ((cr0 & (X86_CR0_PE | X86_CR0_PG)) == (X86_CR0_PE | X86_CR0_PG));
    } else {
        summary->exec_bits = 32u;
        summary->gpr_bits = 32u;
        summary->ptr_bits = 32u;
        summary->vaddr_bits = 32u;
        summary->paddr_bits = (facts && facts->phys_addr_bits && facts->phys_addr_bits <= 36u)
                                  ? facts->phys_addr_bits : 32u;
        summary->switch_verified = ((cr0 & X86_CR0_PE) != 0ULL) && ((efer & X86_EFER_LMA) == 0ULL);
    }
}
