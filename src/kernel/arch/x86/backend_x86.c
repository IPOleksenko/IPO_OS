#include <arch/arch_backend.h>
#include <memory/kmalloc.h>
#include <string.h>
#include "x86_intrinsics.h"

#define IA32_EFER_MSR       0xC0000080u
#define BOOT_PT_BASE_PADDR  0x50000u
#define BOOT_PT_PAGE_SIZE   4096u
#define PAGING_PRESENT      0x001ULL
#define PAGING_WRITABLE     0x002ULL
#define PAGING_LARGE        0x080ULL

/* Packed parameter block shared with x86_execute_hw_switch (entry.asm) */
typedef struct __attribute__((packed)) {
    uint32_t msr_addr;
    uint32_t reserved;
    uint64_t set_mask;
    uint64_t clear_mask;
} x86_tx_msr_op_t;

typedef struct __attribute__((packed)) {
    uint32_t root_pt_paddr;     /* 0 */
    uint32_t target_exec_bits;  /* 4 */
    uint64_t cs_desc_raw;       /* 8 */
    uint64_t cr0_set_mask;      /* 16 */
    uint64_t cr0_clear_mask;    /* 24 */
    uint64_t cr4_set_mask;      /* 32 */
    uint64_t cr4_clear_mask;    /* 40 */
    uint32_t msr_op_count;      /* 48 */
    uint16_t cs_selector;       /* 52 */
    uint16_t ds_selector;       /* 54 */
    x86_tx_msr_op_t *msr_ops;   /* 56 */
} x86_tx_params_t;

IPO_STATIC_ASSERT(offsetof(x86_tx_params_t, root_pt_paddr) == 0u, "tx root_pt_paddr");
IPO_STATIC_ASSERT(offsetof(x86_tx_params_t, target_exec_bits) == 4u, "tx target_exec_bits");
IPO_STATIC_ASSERT(offsetof(x86_tx_params_t, cs_desc_raw) == 8u, "tx cs_desc_raw");
IPO_STATIC_ASSERT(offsetof(x86_tx_params_t, cr0_set_mask) == 16u, "tx cr0_set_mask");
IPO_STATIC_ASSERT(offsetof(x86_tx_params_t, cr0_clear_mask) == 24u, "tx cr0_clear_mask");
IPO_STATIC_ASSERT(offsetof(x86_tx_params_t, cr4_set_mask) == 32u, "tx cr4_set_mask");
IPO_STATIC_ASSERT(offsetof(x86_tx_params_t, cr4_clear_mask) == 40u, "tx cr4_clear_mask");
IPO_STATIC_ASSERT(offsetof(x86_tx_params_t, msr_op_count) == 48u, "tx msr_op_count");
IPO_STATIC_ASSERT(offsetof(x86_tx_params_t, cs_selector) == 52u, "tx cs_selector");
IPO_STATIC_ASSERT(offsetof(x86_tx_params_t, ds_selector) == 54u, "tx ds_selector");
IPO_STATIC_ASSERT(offsetof(x86_tx_params_t, msr_ops) == 56u, "tx msr_ops");

extern void x86_execute_hw_switch(const x86_tx_params_t *params, uint64_t *out_regs);

static bool x86_efer_supported(void) {
    if (!x86_has_cpuid()) return false;
    uint32_t eax = 0u, ebx = 0u, ecx = 0u, edx = 0u;
    x86_cpuid(0x80000000u, 0u, &eax, &ebx, &ecx, &edx);
    if (eax < 0x80000001u) return false;
    x86_cpuid(0x80000001u, 0u, &eax, &ebx, &ecx, &edx);
    return (edx & ((1u << 20) | (1u << 29))) != 0u;
}

/* Build identity-mapped 4 GiB radix-512 boot tables for depth >= 3 */
static int prepare_boot_radix_tables(uint32_t levels, ipo_boot_handover_t *handover) {
    if (!handover || levels < 3u) return -1;

    uint32_t upper_tables = (levels > 3u) ? (levels - 3u) : 0u;
    uint32_t total_pages = upper_tables + 1u + 4u;
    memset((void *)(uintptr_t)BOOT_PT_BASE_PADDR, 0, (size_t)(total_pages * BOOT_PT_PAGE_SIZE));

    uintptr_t cur_page = (uintptr_t)BOOT_PT_BASE_PADDR;
    uintptr_t root_paddr = cur_page;

    for (uint32_t u = 0u; u < upper_tables; u++) {
        uint64_t *tbl = (uint64_t *)cur_page;
        uintptr_t next_paddr = cur_page + BOOT_PT_PAGE_SIZE;
        tbl[0] = (uint64_t)next_paddr | PAGING_PRESENT | PAGING_WRITABLE;
        cur_page = next_paddr;
    }

    uint64_t *pdpt = (uint64_t *)cur_page;
    cur_page += BOOT_PT_PAGE_SIZE;

    for (uint32_t gb = 0u; gb < 4u; gb++) {
        uint64_t *pd = (uint64_t *)cur_page;
        cur_page += BOOT_PT_PAGE_SIZE;
        pdpt[gb] = (uint64_t)(uintptr_t)pd | PAGING_PRESENT | PAGING_WRITABLE;

        uint64_t gb_base = (uint64_t)gb << 30;
        for (uint32_t idx = 0u; idx < 512u; idx++) {
            uint64_t phys = gb_base + ((uint64_t)idx << 21);
            pd[idx] = phys | PAGING_PRESENT | PAGING_WRITABLE | PAGING_LARGE;
        }
    }

    handover->boot_pt_root_paddr = (uint64_t)root_paddr;
    return 0;
}

static bool x86_universal_matches(const cpu_exec_mode_desc_t *m) {
    if (!m) return false;
    return (m->exec_bits >= 16u && (m->exec_bits & (m->exec_bits - 1u)) == 0u);
}

static bool x86_universal_can_tx(const cpu_hw_facts_t *facts,
                                 const cpu_exec_mode_desc_t *cur,
                                 const cpu_exec_mode_desc_t *target,
                                 const char **out_reason) {
    if (!facts || !target) return false;
    if (cur && target->exec_bits < cur->exec_bits) {
        if (out_reason) *out_reason = "Already executing in wider mode than target";
        return false;
    }
    return cpu_eval_mode_predicates(facts, target, NULL, out_reason);
}

static int x86_universal_prepare(const cpu_hw_facts_t *facts,
                                 const cpu_exec_mode_desc_t *target,
                                 ipo_boot_handover_t *handover) {
    (void)facts;
    if (!target || !handover) return -1;
    handover->boot_pt_root_paddr = 0ULL;
    if (target->switch_plan.paging_levels >= 3u) {
        return prepare_boot_radix_tables(target->switch_plan.paging_levels, handover);
    }
    return 0;
}

static x86_tx_params_t s_tx_params;
static x86_tx_msr_op_t *s_tx_msr_buf = NULL;
static ipo_size_t s_tx_msr_cap = 0u;

static int x86_universal_enter(const cpu_exec_mode_desc_t *target,
                               ipo_boot_handover_t *handover,
                               ipo_paddr_t entry_paddr) {
    (void)entry_paddr;
    if (!target) return -1;

    const arch_hw_switch_plan_t *sp = &target->switch_plan;
    memset(&s_tx_params, 0, sizeof(s_tx_params));

    s_tx_params.root_pt_paddr = handover ? (uint32_t)handover->boot_pt_root_paddr : 0u;
    s_tx_params.target_exec_bits = (uint32_t)(sp->target_exec_bits ? sp->target_exec_bits : target->exec_bits);
    s_tx_params.cs_desc_raw = sp->cs_desc_raw ? sp->cs_desc_raw : 0x00CF9B000000FFFFULL;
    s_tx_params.cr0_set_mask = sp->cr0_set_mask;
    s_tx_params.cr0_clear_mask = sp->cr0_clear_mask;
    s_tx_params.cr4_set_mask = sp->cr4_set_mask;
    s_tx_params.cr4_clear_mask = sp->cr4_clear_mask;
    s_tx_params.cs_selector = sp->cs_selector ? sp->cs_selector : 0x08u;
    s_tx_params.ds_selector = sp->ds_selector ? sp->ds_selector : 0x10u;

    ipo_size_t mcnt = sp->msr_op_count;
    if (mcnt > 0u) {
        if (!kgrow_array((void **)&s_tx_msr_buf, &s_tx_msr_cap, mcnt, sizeof(x86_tx_msr_op_t), 4u)) {
            return -1;
        }
        memset(s_tx_msr_buf, 0, mcnt * sizeof(x86_tx_msr_op_t));
        for (ipo_size_t i = 0u; i < mcnt; i++) {
            s_tx_msr_buf[i].msr_addr   = sp->msr_ops[i].msr_addr;
            s_tx_msr_buf[i].set_mask   = sp->msr_ops[i].set_mask;
            s_tx_msr_buf[i].clear_mask = sp->msr_ops[i].clear_mask;
        }
    }
    s_tx_params.msr_op_count = (uint32_t)mcnt;
    s_tx_params.msr_ops      = (mcnt > 0u) ? s_tx_msr_buf : NULL;

    uint64_t *regs = (uint64_t *)kmalloc(7u * sizeof(uint64_t));
    if (!regs) return -1;
    memset(regs, 0, 7u * sizeof(uint64_t));
    x86_execute_hw_switch(&s_tx_params, regs);

    if (handover) {
        handover->verified_cr0   = regs[0];
        handover->verified_cr3   = regs[1];
        handover->verified_cr4   = regs[2];
        handover->verified_efer  = regs[3];
        handover->verified_flags = regs[4];
        handover->verified_sp    = regs[5];
        handover->verified_cs    = (uint16_t)regs[6];
        handover->verified_ds    = 0x10u;
    }
    kfree(regs);
    return 0;
}

static bool x86_universal_verify(const cpu_exec_mode_desc_t *target,
                                 ipo_boot_handover_t *handover) {
    if (!target) return false;
    const arch_hw_switch_plan_t *sp = &target->switch_plan;

    uint64_t cr0 = (uint64_t)x86_read_cr0();
    uint64_t cr3 = (uint64_t)x86_read_cr3();
    uint64_t cr4 = x86_has_cpuid() ? (uint64_t)x86_read_cr4() : 0ULL;
    uint64_t efer = x86_efer_supported() ? x86_rdmsr(IA32_EFER_MSR) : 0ULL;

    bool ok = ((cr0 & sp->verify_cr0_mask) == sp->verify_cr0_expected) &&
              ((cr4 & sp->verify_cr4_mask) == sp->verify_cr4_expected) &&
              ((efer & sp->verify_efer_mask) == sp->verify_efer_expected);

    if (handover) {
        handover->verified_cr0 = cr0;
        handover->verified_cr3 = cr3;
        handover->verified_cr4 = cr4;
        handover->verified_efer = efer;
        if (handover->verified_cs == 0u) {
            handover->verified_cs = x86_read_cs();
        }
        handover->switch_verified = ok ? 1u : 0u;
    }
    return ok;
}

static const arch_transition_ops_t s_backend_x86_universal = {
    .name = "x86-universal-tx",
    .arch_name = "x86",
    .matches_mode = x86_universal_matches,
    .can_transition = x86_universal_can_tx,
    .prepare = x86_universal_prepare,
    .enter_and_verify = x86_universal_enter,
    .verify_active = x86_universal_verify
};

bool backend_x86_register_all(const ipo_mem_ops_t *mem) {
    return arch_backend_register(&s_backend_x86_universal, mem);
}

bool backend_register_platform_all(const ipo_mem_ops_t *mem) {
    return backend_x86_register_all(mem);
}
