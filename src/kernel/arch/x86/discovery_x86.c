#include <arch/cpu_discovery.h>
#include "x86_intrinsics.h"

typedef struct {
    char      *data;
    ipo_size_t len;
    ipo_size_t cap;
} dyn_str_buf_t;

static void dyn_str_free(dyn_str_buf_t *b, const ipo_mem_ops_t *mem) {
    if (!b) return;
    if (b->data && mem && mem->free_fn) {
        mem->free_fn(b->data, mem->ctx);
    }
    b->data = NULL;
    b->len = 0u;
    b->cap = 0u;
}

static bool dyn_str_reserve(dyn_str_buf_t *b, ipo_size_t extra, const ipo_mem_ops_t *mem) {
    if (!b || !mem) return false;
    if (extra > IPO_SIZE_MAX - b->len - 1u) return false;
    ipo_size_t need = b->len + extra + 1u;
    if (b->data && need <= b->cap) return true;
    ipo_size_t ncap = b->cap ? b->cap : 16u;
    while (ncap < need) {
        if (ncap > IPO_SIZE_MAX / 2u) { ncap = need; break; }
        ncap *= 2u;
    }
    char *nb = NULL;
    if (b->data && mem->realloc_fn) {
        nb = (char *)mem->realloc_fn(b->data, b->cap, ncap, mem->ctx);
    } else if (mem->alloc) {
        nb = (char *)mem->alloc(ncap, mem->ctx);
        if (nb && b->data && b->len > 0u) {
            for (ipo_size_t i = 0u; i < b->len; i++) nb[i] = b->data[i];
            if (mem->free_fn) mem->free_fn(b->data, mem->ctx);
        }
    }
    if (!nb) return false;
    b->data = nb;
    b->cap = ncap;
    b->data[b->len] = '\0';
    return true;
}

static bool dyn_str_push_ch(dyn_str_buf_t *b, char ch, const ipo_mem_ops_t *mem) {
    if (!dyn_str_reserve(b, 1u, mem)) return false;
    b->data[b->len++] = ch;
    b->data[b->len] = '\0';
    return true;
}

static bool dyn_str_append_str(dyn_str_buf_t *b, const char *s, const ipo_mem_ops_t *mem) {
    if (!s) return true;
    ipo_size_t slen = 0u;
    while (s[slen]) slen++;
    if (!dyn_str_reserve(b, slen, mem)) return false;
    for (ipo_size_t i = 0u; i < slen; i++) {
        b->data[b->len++] = s[i];
    }
    b->data[b->len] = '\0';
    return true;
}

static bool dyn_str_append_u32(dyn_str_buf_t *b, uint32_t val, const ipo_mem_ops_t *mem) {
    if (val >= 10u) {
        if (!dyn_str_append_u32(b, val / 10u, mem)) return false;
    }
    return dyn_str_push_ch(b, (char)('0' + (val % 10u)), mem);
}

static bool dyn_str_append_dword_bytes(dyn_str_buf_t *b, uint32_t w, const ipo_mem_ops_t *mem) {
    for (uint32_t shift = 0u; shift < 32u; shift += 8u) {
        if (!dyn_str_push_ch(b, (char)((w >> shift) & 0xFFu), mem)) return false;
    }
    return true;
}

static bool dyn_caps_push(const char ***caps,
                          ipo_size_t *count,
                          ipo_size_t *cap,
                          const char *name,
                          const ipo_mem_ops_t *mem) {
    if (!caps || !count || !cap || !mem || !name) return false;
    if (*count >= *cap) {
        ipo_size_t ncap = (*cap > 0u) ? (*cap * 2u) : 8u;
        ipo_size_t old_bytes = (*cap) * sizeof(const char *);
        ipo_size_t new_bytes = ncap * sizeof(const char *);
        const char **nb = NULL;
        if (*caps && mem->realloc_fn) {
            nb = (const char **)mem->realloc_fn(*caps, old_bytes, new_bytes, mem->ctx);
        } else if (mem->alloc) {
            nb = (const char **)mem->alloc(new_bytes, mem->ctx);
            if (nb && *caps) {
                for (ipo_size_t i = 0u; i < *count; i++) nb[i] = (*caps)[i];
                if (mem->free_fn) mem->free_fn(*caps, mem->ctx);
            }
        }
        if (!nb) return false;
        *caps = nb;
        *cap = ncap;
    }
    (*caps)[(*count)++] = name;
    return true;
}

typedef struct {
    uint32_t    flag_bit_mask;
    const char *cap_name;
} x86_flag_probe_t;

typedef struct {
    const char *cap_name;
    uint32_t    leaf;
    uint32_t    subleaf;
    uint32_t    reg_idx;   /* 0=EAX, 1=EBX, 2=ECX, 3=EDX */
    uint32_t    bit_mask;
} x86_cap_rule_t;

static const x86_flag_probe_t s_x86_flag_probes[] = {
    { 0x00004000u, "hw.32bit_flags" },
    { 0x00200000u, "hw.cpuid"       }
};

static const x86_cap_rule_t s_x86_cap_rules[] = {
    { "hw.pse",       0x00000001u, 0u, 3u, (1u << 3)  },
    { "hw.msr",       0x00000001u, 0u, 3u, (1u << 5)  },
    { "hw.pae",       0x00000001u, 0u, 3u, (1u << 6)  },
    { "hw.apic",      0x00000001u, 0u, 3u, (1u << 9)  },
    { "hw.fxsr",      0x00000001u, 0u, 3u, (1u << 24) },
    { "hw.sse",       0x00000001u, 0u, 3u, (1u << 25) },
    { "hw.sse2",      0x00000001u, 0u, 3u, (1u << 26) },
    { "hw.rdrand",    0x00000001u, 0u, 2u, (1u << 30) },
    { "hw.la57",      0x00000007u, 0u, 2u, (1u << 16) },
    { "hw.nx",        0x80000001u, 0u, 3u, (1u << 20) },
    { "hw.1gb_pages", 0x80000001u, 0u, 3u, (1u << 26) },
    { "hw.long_mode", 0x80000001u, 0u, 3u, (1u << 29) }
};

/* Baseline 16-bit and 32-bit mode definitions; 64-bit+ tiers are added below */
typedef struct {
    ipo_bitwidth_t exec_bits;
    ipo_bitwidth_t vaddr_bits;
    uint32_t       paging_levels;
    uint32_t       rank_hint;
    uint16_t       cs_selector;
    uint64_t       cs_desc_raw;
    uint64_t       cr0_set;
    uint64_t       cr0_clr;
    uint64_t       cr4_set;
    uint64_t       cr4_clr;
    uint64_t       verify_cr0_mask;
    uint64_t       verify_cr0_exp;
    uint64_t       verify_cr4_mask;
    uint64_t       verify_cr4_exp;
    uint64_t       verify_efer_mask;
    uint64_t       verify_efer_exp;
    uint32_t       msr_addr;
    uint64_t       msr_set;
    uint8_t        pt_bits_per_level;
    uint8_t        pt_entry_bytes;
    const char        *label;
    const char *const *req_caps;
    ipo_size_t         req_cap_count;
} x86_mode_rule_t;

static const char *const s_x86_caps_real[] = { "hw.x86_real" };
static const char *const s_x86_caps_prot32[] = { "hw.32bit_flags", "env.a20_line", "env.high_ram" };

static const x86_mode_rule_t s_x86_tier_rules[] = {
    {
        .exec_bits = 16u, .vaddr_bits = 20u, .paging_levels = 0u, .rank_hint = 10u,
        .cs_selector = 0x00u, .cs_desc_raw = 0ULL,
        .cr0_set = 0ULL, .cr0_clr = 1ULL, .cr4_set = 0ULL, .cr4_clr = 0ULL,
        .verify_cr0_mask = 1ULL, .verify_cr0_exp = 0ULL,
        .verify_cr4_mask = 0ULL, .verify_cr4_exp = 0ULL,
        .verify_efer_mask = 0ULL, .verify_efer_exp = 0ULL,
        .msr_addr = 0u, .msr_set = 0ULL,
        .pt_bits_per_level = 0u, .pt_entry_bytes = 0u,
        .label = "x86-16 Real/Diagnostic Mode",
        .req_caps = s_x86_caps_real,
        .req_cap_count = sizeof(s_x86_caps_real) / sizeof(s_x86_caps_real[0])
    },
    {
        .exec_bits = 32u, .vaddr_bits = 32u, .paging_levels = 2u, .rank_hint = 20u,
        .cs_selector = 0x08u, .cs_desc_raw = 0x00CF9B000000FFFFULL,
        .cr0_set = 0x00010023ULL, .cr0_clr = 0x0000000CULL,
        .cr4_set = 0x00000610ULL, .cr4_clr = 0ULL,
        .verify_cr0_mask = 0x00000001ULL, .verify_cr0_exp = 0x00000001ULL,
        .verify_cr4_mask = 0ULL, .verify_cr4_exp = 0ULL,
        .verify_efer_mask = 0ULL, .verify_efer_exp = 0ULL,
        .msr_addr = 0u, .msr_set = 0ULL,
        .pt_bits_per_level = 10u, .pt_entry_bytes = 4u,
        .label = "x86-32 Protected Paged Mode",
        .req_caps = s_x86_caps_prot32,
        .req_cap_count = sizeof(s_x86_caps_prot32) / sizeof(s_x86_caps_prot32[0])
    }
};

void cpu_discovery_probe_platform(cpu_hw_facts_t *facts,
                                  uint64_t total_ram_bytes,
                                  bool a20_enabled,
                                  const ipo_mem_ops_t *mem) {
    if (!facts) return;
    facts->arch_family = cpu_arch_register_name("x86", mem);
    facts->total_ram_bytes = total_ram_bytes;
    facts->phys_addr_bits = 20u;
    facts->virt_addr_bits = 20u;
    (void)cpu_facts_set_vendor(facts, "x86-Legacy", mem);
    (void)cpu_facts_set_brand(facts, "x86-16 Baseline", mem);

    ipo_capset_set(&facts->hw_caps, cpu_cap_register_name("hw.x86_real", mem), mem);
    if (a20_enabled) {
        ipo_capset_set(&facts->hw_caps, cpu_cap_register_name("env.a20_line", mem), mem);
    }
    if (total_ram_bytes > 0x100000ULL) {
        ipo_capset_set(&facts->hw_caps, cpu_cap_register_name("env.high_ram", mem), mem);
    }

    ipo_reg_t orig_fl = x86_read_flags();
    for (ipo_size_t i = 0u; i < sizeof(s_x86_flag_probes) / sizeof(s_x86_flag_probes[0]); i++) {
        ipo_reg_t mask = (ipo_reg_t)s_x86_flag_probes[i].flag_bit_mask;
        x86_write_flags(orig_fl ^ mask);
        ipo_reg_t rd = x86_read_flags();
        x86_write_flags(orig_fl);
        if (((orig_fl ^ rd) & mask) != 0u) {
            ipo_capset_set(&facts->hw_caps,
                           cpu_cap_register_name(s_x86_flag_probes[i].cap_name, mem),
                           mem);
            facts->hw_probe_flags |= (1ULL << i);
        }
    }

    if ((facts->hw_probe_flags & 1ULL) != 0ULL) {
        facts->phys_addr_bits = 32u;
        facts->virt_addr_bits = 32u;
        (void)cpu_facts_set_brand(facts, "x86 Protected (No CPUID)", mem);
    }

    if ((facts->hw_probe_flags & 3ULL) != 3ULL || !x86_has_cpuid()) {
        return;
    }

    uint32_t a = 0u, b = 0u, c = 0u, d = 0u;
    x86_cpuid(0u, 0u, &a, &b, &c, &d);
    uint32_t max_basic = (a > 31u) ? 31u : a;
    for (uint32_t leaf = 0u; leaf <= max_basic; leaf++) {
        x86_cpuid(leaf, 0u, &a, &b, &c, &d);
        cpu_facts_add_leaf(facts, 0u, leaf, 0u, a, b, c, d, mem);
        if (leaf == 7u && a > 0u) {
            uint32_t max_sub = (a > 8u) ? 8u : a;
            for (uint32_t sub = 1u; sub <= max_sub; sub++) {
                x86_cpuid(leaf, sub, &a, &b, &c, &d);
                cpu_facts_add_leaf(facts, 0u, leaf, sub, a, b, c, d, mem);
            }
        }
    }

    x86_cpuid(0x80000000u, 0u, &a, &b, &c, &d);
    cpu_facts_add_leaf(facts, 0u, 0x80000000u, 0u, a, b, c, d, mem);
    if (a >= 0x80000000u) {
        uint32_t max_ext = (a > 0x80000024u) ? 0x80000024u : a;
        for (uint32_t leaf = 0x80000001u; leaf <= max_ext; leaf++) {
            x86_cpuid(leaf, 0u, &a, &b, &c, &d);
            cpu_facts_add_leaf(facts, 0u, leaf, 0u, a, b, c, d, mem);
        }
    }

    const cpu_hw_leaf_t *lf0 = cpu_facts_find_leaf(facts, 0u, 0u, 0u);
    if (lf0 && lf0->regs && lf0->reg_count >= 4u) {
        dyn_str_buf_t vbuf = {0};
        if (dyn_str_append_dword_bytes(&vbuf, (uint32_t)lf0->regs[1], mem) &&
            dyn_str_append_dword_bytes(&vbuf, (uint32_t)lf0->regs[3], mem) &&
            dyn_str_append_dword_bytes(&vbuf, (uint32_t)lf0->regs[2], mem) &&
            vbuf.data) {
            (void)cpu_facts_set_vendor(facts, vbuf.data, mem);
        }
        dyn_str_free(&vbuf, mem);
    }

    const cpu_hw_leaf_t *lfb2 = cpu_facts_find_leaf(facts, 0u, 0x80000002u, 0u);
    const cpu_hw_leaf_t *lfb3 = cpu_facts_find_leaf(facts, 0u, 0x80000003u, 0u);
    const cpu_hw_leaf_t *lfb4 = cpu_facts_find_leaf(facts, 0u, 0x80000004u, 0u);
    if (lfb2 && lfb3 && lfb4 &&
        lfb2->regs && lfb2->reg_count >= 4u &&
        lfb3->regs && lfb3->reg_count >= 4u &&
        lfb4->regs && lfb4->reg_count >= 4u) {
        dyn_str_buf_t bbuf = {0};
        for (int k = 0; k < 3; k++) {
            const cpu_hw_leaf_t *lf_cur = (k == 0) ? lfb2 : ((k == 1) ? lfb3 : lfb4);
            for (int r = 0; r < 4; r++) {
                (void)dyn_str_append_dword_bytes(&bbuf, (uint32_t)lf_cur->regs[r], mem);
            }
        }
        if (bbuf.data) {
            (void)cpu_facts_set_brand(facts, bbuf.data, mem);
        }
        dyn_str_free(&bbuf, mem);
    } else {
        (void)cpu_facts_set_brand(facts, facts->vendor, mem);
    }

    for (ipo_size_t i = 0u; i < sizeof(s_x86_cap_rules) / sizeof(s_x86_cap_rules[0]); i++) {
        const x86_cap_rule_t *r = &s_x86_cap_rules[i];
        const cpu_hw_leaf_t *lf = cpu_facts_find_leaf(facts, 0u, r->leaf, r->subleaf);
        if (lf && lf->regs && r->reg_idx < lf->reg_count &&
            ((uint32_t)lf->regs[r->reg_idx] & r->bit_mask) != 0u) {
            ipo_capset_set(&facts->hw_caps, cpu_cap_register_name(r->cap_name, mem), mem);
        }
    }

    if (ipo_capset_has(&facts->hw_caps, cpu_cap_find_name("hw.pae")) && facts->phys_addr_bits < 36u) {
        facts->phys_addr_bits = 36u;
    }
    if (ipo_capset_has(&facts->hw_caps, cpu_cap_find_name("hw.long_mode")) && facts->virt_addr_bits < 48u) {
        facts->virt_addr_bits = 48u;
    }

    const cpu_hw_leaf_t *lf8 = cpu_facts_find_leaf(facts, 0u, 0x80000008u, 0u);
    if (lf8) {
        uint32_t pbits = (uint32_t)(lf8->regs[0] & 0xFFULL);
        uint32_t vbits = (uint32_t)((lf8->regs[0] >> 8) & 0xFFULL);
        if (pbits >= 20u) facts->phys_addr_bits = (ipo_bitwidth_t)pbits;
        if (vbits >= 20u) facts->virt_addr_bits = (ipo_bitwidth_t)vbits;
    }

    if (ipo_capset_has(&facts->hw_caps, cpu_cap_find_name("hw.long_mode")) &&
        ipo_capset_has(&facts->hw_caps, cpu_cap_find_name("hw.la57")) &&
        facts->virt_addr_bits < 57u) {
        facts->virt_addr_bits = 57u;
    }
}

static void add_cap_predicate_local(cpu_exec_mode_desc_t *m, const char *cap_name, const ipo_mem_ops_t *mem) {
    ipo_hw_predicate_t p;
    p.op = IPO_PRED_CAP;
    p.reg_idx = 0u;
    p.space = 0u;
    p.leaf = 0u;
    p.subleaf = 0u;
    p.mask = 0ULL;
    p.expected = 0ULL;
    p.cap_name = (char *)cap_name;
    exec_mode_add_predicate(m, &p, mem);
    ipo_capset_set(&m->required_caps, cpu_cap_register_name(cap_name, mem), mem);
}

static void add_vaddr_predicate_local(cpu_exec_mode_desc_t *m, uint64_t min_vaddr, const ipo_mem_ops_t *mem) {
    ipo_hw_predicate_t p;
    p.op = IPO_PRED_MIN_VADDR_BITS;
    p.reg_idx = 0u;
    p.space = 0u;
    p.leaf = 0u;
    p.subleaf = 0u;
    p.mask = 0ULL;
    p.expected = min_vaddr;
    p.cap_name = NULL;
    exec_mode_add_predicate(m, &p, mem);
}

static void populate_mode_from_rule(exec_mode_registry_t *reg,
                                    uint32_t arch_id,
                                    const x86_mode_rule_t *r,
                                    ipo_bitwidth_t max_pa,
                                    const ipo_mem_ops_t *mem) {
    ipo_bitwidth_t pa = (r->exec_bits <= 16u) ? 20u
                      : ((r->exec_bits <= 32u && max_pa > 36u) ? 36u : max_pa);
    if (pa < r->exec_bits && r->exec_bits <= 32u) pa = r->exec_bits;
    if (r->exec_bits >= 64u && pa > r->exec_bits) pa = r->exec_bits;

    cpu_exec_mode_desc_t *m = exec_mode_registry_add(
        reg, arch_id, r->label,
        r->exec_bits, r->exec_bits, r->vaddr_bits, pa, r->exec_bits, r->paging_levels);
    if (!m) return;

    m->hw_rank_hint = r->rank_hint;
    m->switch_plan.target_exec_bits = r->exec_bits;
    m->switch_plan.paging_levels = r->paging_levels;
    m->switch_plan.cs_selector = r->cs_selector;
    m->switch_plan.ds_selector = 0x10u;
    m->switch_plan.cs_desc_raw = r->cs_desc_raw;
    m->switch_plan.cr0_set_mask = r->cr0_set;
    m->switch_plan.cr0_clear_mask = r->cr0_clr;
    m->switch_plan.cr4_set_mask = r->cr4_set;
    m->switch_plan.cr4_clear_mask = r->cr4_clr;
    m->switch_plan.verify_cr0_mask = r->verify_cr0_mask;
    m->switch_plan.verify_cr0_expected = r->verify_cr0_exp;
    m->switch_plan.verify_cr4_mask = r->verify_cr4_mask;
    m->switch_plan.verify_cr4_expected = r->verify_cr4_exp;
    m->switch_plan.verify_efer_mask = r->verify_efer_mask;
    m->switch_plan.verify_efer_expected = r->verify_efer_exp;
    m->switch_plan.pt_bits_per_level = r->pt_bits_per_level;
    m->switch_plan.pt_entry_bytes = r->pt_entry_bytes;

    if (r->msr_addr != 0u) {
        exec_mode_add_msr_op(m, r->msr_addr, r->msr_set, 0ULL, mem);
    }
    if (r->req_caps) {
        for (ipo_size_t i = 0u; i < r->req_cap_count; i++) {
            if (r->req_caps[i]) {
                add_cap_predicate_local(m, r->req_caps[i], mem);
            }
        }
    }
    if (r->vaddr_bits > 32u) {
        add_vaddr_predicate_local(m, (uint64_t)r->vaddr_bits, mem);
    }
}

/* Register baseline 16/32-bit modes and radix-512 long-mode tiers from CPU facts */
static int cpu_discovery_x86_universal_provider(const cpu_hw_facts_t *facts,
                                                exec_mode_registry_t *reg,
                                                const ipo_mem_ops_t *mem,
                                                void *ctx) {
    (void)ctx;
    if (!facts || !reg || !mem) return -1;

    uint32_t x86_arch = cpu_arch_register_name("x86", mem);

    for (ipo_size_t i = 0u; i < sizeof(s_x86_tier_rules) / sizeof(s_x86_tier_rules[0]); i++) {
        populate_mode_from_rule(reg, x86_arch, &s_x86_tier_rules[i], facts->phys_addr_bits, mem);
    }

    uint32_t hw_max_lvl = (facts->virt_addr_bits > 32u)
                        ? (uint32_t)((facts->virt_addr_bits - 12u) / 9u)
                        : 4u;
    uint32_t probe_max_lvl = (hw_max_lvl > 5u) ? hw_max_lvl : 5u;
    ipo_bitwidth_t pa_wide = (facts->phys_addr_bits >= 36u) ? facts->phys_addr_bits : 40u;

    for (uint32_t lvl = 4u; lvl <= probe_max_lvl; lvl++) {
        ipo_bitwidth_t vaddr_w = (ipo_bitwidth_t)(12u + 9u * lvl);
        ipo_bitwidth_t exec_w = 64u;
        while (exec_w < vaddr_w) {
            exec_w <<= 1u; /* Next power of two >= vaddr_w (64, 128, 256, ...) */
        }

        dyn_str_buf_t mname = {0};
        (void)dyn_str_append_str(&mname, "x86-", mem);
        (void)dyn_str_append_u32(&mname, (uint32_t)exec_w, mem);
        (void)dyn_str_append_str(&mname, " Long Mode (", mem);
        (void)dyn_str_append_u32(&mname, lvl, mem);
        (void)dyn_str_append_str(&mname, "-Level Radix)", mem);

        const char **req_caps = NULL;
        ipo_size_t req_cap_count = 0u;
        ipo_size_t req_cap_alloc = 0u;
        (void)dyn_caps_push(&req_caps, &req_cap_count, &req_cap_alloc, "hw.32bit_flags", mem);
        (void)dyn_caps_push(&req_caps, &req_cap_count, &req_cap_alloc, "hw.cpuid", mem);
        (void)dyn_caps_push(&req_caps, &req_cap_count, &req_cap_alloc, "hw.pae", mem);
        (void)dyn_caps_push(&req_caps, &req_cap_count, &req_cap_alloc, "hw.msr", mem);
        (void)dyn_caps_push(&req_caps, &req_cap_count, &req_cap_alloc, "hw.long_mode", mem);
        (void)dyn_caps_push(&req_caps, &req_cap_count, &req_cap_alloc, "env.a20_line", mem);
        (void)dyn_caps_push(&req_caps, &req_cap_count, &req_cap_alloc, "env.high_ram", mem);
        if (lvl == 5u) {
            (void)dyn_caps_push(&req_caps, &req_cap_count, &req_cap_alloc, "hw.la57", mem);
        }

        ipo_bitwidth_t mode_pa = (pa_wide > exec_w) ? exec_w : pa_wide;
        if (lvl > 5u && mode_pa < vaddr_w) mode_pa = vaddr_w;

        x86_mode_rule_t dyn = {
            .exec_bits = exec_w,
            .vaddr_bits = vaddr_w,
            .paging_levels = lvl,
            .rank_hint = 20u + lvl * 10u,
            .cs_selector = 0x38u,
            .cs_desc_raw = 0x00AF9B000000FFFFULL,
            .cr0_set = 0x80010023ULL,
            .cr0_clr = 0x0000000CULL,
            .cr4_set = 0x00000620ULL | ((lvl >= 5u) ? (1ULL << 12) : 0ULL),
            .cr4_clr = 0x00000010ULL | ((lvl == 4u) ? (1ULL << 12) : 0ULL),
            .verify_cr0_mask = 0x80000001ULL,
            .verify_cr0_exp = 0x80000001ULL,
            .verify_cr4_mask = (lvl >= 5u) ? 0x00001020ULL : 0x00000020ULL,
            .verify_cr4_exp = (lvl >= 5u) ? 0x00001020ULL : 0x00000020ULL,
            .verify_efer_mask = 0x00000500ULL,
            .verify_efer_exp = 0x00000500ULL,
            .msr_addr = 0xC0000080u,
            .msr_set = (1ULL << 8),
            .pt_bits_per_level = 9u,
            .pt_entry_bytes = 8u,
            .label = mname.data ? mname.data : "x86-Long Mode",
            .req_caps = (const char *const *)req_caps,
            .req_cap_count = req_cap_count
        };
        populate_mode_from_rule(reg, x86_arch, &dyn, mode_pa, mem);
        if (req_caps && mem->free_fn) mem->free_fn(req_caps, mem->ctx);
        dyn_str_free(&mname, mem);
    }

    return 0;
}

bool cpu_discovery_register_platform_providers(const ipo_mem_ops_t *mem) {
    return cpu_discovery_register_provider("x86-universal", cpu_discovery_x86_universal_provider, NULL, mem);
}
