#include <kernel/cpu_info.h>
#include <arch/arch_backend.h>
#include <arch/mode_selector.h>
#include <arch/cpu_regs.h>
#include <memory/kmalloc.h>
#include <memory/memory_map.h>
#include <stdio.h>
#include <string.h>

static void *k_mem_alloc(ipo_size_t size, void *ctx) {
    (void)ctx;
    return kmalloc(size);
}

static void *k_mem_realloc(void *ptr, ipo_size_t old_sz, ipo_size_t new_sz, void *ctx) {
    (void)old_sz;
    (void)ctx;
    return krealloc(ptr, new_sz);
}

static void k_mem_free(void *ptr, void *ctx) {
    (void)ctx;
    if (ptr) kfree(ptr);
}

static const ipo_mem_ops_t s_kernel_mem_ops = {
    .alloc = k_mem_alloc,
    .realloc_fn = k_mem_realloc,
    .free_fn = k_mem_free,
    .ctx = NULL
};

static bool s_cpu_info_ready = false;
static ipo_boot_handover_t *s_handover_buf = NULL;
static ipo_boot_handover_t  s_auto_handover;
static cpu_hw_facts_t s_facts;
static exec_mode_registry_t s_registry;
static ipo_cpu_runtime_summary_t s_summary;

static const char *mode_status_name(ipo_mode_status_t status) {
    switch (status) {
        case IPO_MODE_ELIGIBLE:          return "ELIGIBLE";
        case IPO_MODE_ERR_INVALID_DESC:  return "INVALID";
        case IPO_MODE_ERR_NO_HARDWARE:   return "NO_HW";
        case IPO_MODE_ERR_MISSING_DEPS:  return "MISSING_DEPS";
        case IPO_MODE_ERR_NO_TRANSITION: return "NO_TRANSITION";
        case IPO_MODE_ERR_NO_BACKEND:    return "NO_BACKEND";
        case IPO_MODE_ERR_NO_PAYLOAD:    return "NO_PAYLOAD";
        case IPO_MODE_ERR_SWITCH_FAILED: return "SWITCH_FAILED";
        case IPO_MODE_ERR_VERIFY_FAILED: return "VERIFY_FAILED";
        default:                         return "UNKNOWN";
    }
}

static bool summary_ensure_str_cap(char **dst, ipo_size_t *cap, ipo_size_t need) {
    if (!dst || !cap) return false;
    if (*dst && need <= *cap) return true;
    ipo_size_t ncap = (*cap > 0u) ? *cap : 16u;
    while (ncap < need) {
        if (ncap > IPO_SIZE_MAX / 2u) { ncap = need; break; }
        ncap *= 2u;
    }
    char *nb = *dst
        ? (char *)s_kernel_mem_ops.realloc_fn(*dst, *cap, ncap, s_kernel_mem_ops.ctx)
        : (char *)s_kernel_mem_ops.alloc(ncap, s_kernel_mem_ops.ctx);
    if (!nb) return false;
    *dst = nb;
    *cap = ncap;
    return true;
}

static bool summary_set_cstr(char **dst, ipo_size_t *cap, const char *src) {
    if (!src) src = "";
    size_t len = strlen(src);
    if (!summary_ensure_str_cap(dst, cap, (ipo_size_t)(len + 1u))) return false;
    memcpy(*dst, src, len + 1u);
    return true;
}

static void summary_free_strings(ipo_cpu_runtime_summary_t *sum) {
    if (!sum) return;
    if (sum->mode_name) s_kernel_mem_ops.free_fn(sum->mode_name, s_kernel_mem_ops.ctx);
    if (sum->vendor) s_kernel_mem_ops.free_fn(sum->vendor, s_kernel_mem_ops.ctx);
    if (sum->brand) s_kernel_mem_ops.free_fn(sum->brand, s_kernel_mem_ops.ctx);
    sum->mode_name = NULL;
    sum->mode_name_cap = 0u;
    sum->vendor = NULL;
    sum->vendor_cap = 0u;
    sum->brand = NULL;
    sum->brand_cap = 0u;
}

static void populate_summary_from_live_hw(void) {
    arch_cpu_fill_runtime_summary(&s_summary, &s_facts);
    const cpu_exec_mode_desc_t *act = exec_mode_registry_find(&s_registry, s_summary.active_mode_id);
    if (act) {
        s_summary.exec_bits = act->exec_bits;
        s_summary.gpr_bits = act->gpr_bits;
        s_summary.vaddr_bits = act->vaddr_bits;
        s_summary.paddr_bits = act->paddr_bits;
        s_summary.ptr_bits = act->ptr_bits;
        if (act->paging_levels > s_summary.paging_levels) {
            s_summary.paging_levels = act->paging_levels;
        }
        if (act->name) {
            (void)summary_set_cstr(&s_summary.mode_name, &s_summary.mode_name_cap, act->name);
        }
    } else if (!s_summary.mode_name || s_summary.mode_name[0] == '\0') {
        int needed = snprintf(NULL, 0,
                              "%llu-bit Mode (%u-level paging)",
                              (unsigned long long)s_summary.exec_bits,
                              s_summary.paging_levels);
        if (needed >= 0 &&
            summary_ensure_str_cap(&s_summary.mode_name, &s_summary.mode_name_cap, (ipo_size_t)needed + 1u)) {
            snprintf(s_summary.mode_name, (size_t)s_summary.mode_name_cap,
                     "%llu-bit Mode (%u-level paging)",
                     (unsigned long long)s_summary.exec_bits,
                     s_summary.paging_levels);
        }
    }
}

static bool mode_is_supported(const cpu_exec_mode_desc_t *m, uint32_t active_mode_id) {
    if (!m) return false;
    if (m->mode_id == active_mode_id) return true;
    return m->hardware_supported && (m->status == IPO_MODE_ELIGIBLE);
}

static bool collect_supported_bitnesses(uint32_t active_mode_id,
                                        ipo_bitwidth_t **out_widths,
                                        ipo_size_t *out_count) {
    if (!out_widths || !out_count) return false;
    *out_widths = NULL;
    *out_count = 0u;

    ipo_size_t cap = 0u;
    ipo_size_t count = 0u;
    ipo_bitwidth_t *widths = NULL;

    for (ipo_size_t i = 0u; i < s_registry.count; i++) {
        const cpu_exec_mode_desc_t *m = &s_registry.items[i];
        if (!mode_is_supported(m, active_mode_id) || m->exec_bits == 0u) {
            continue;
        }
        bool seen = false;
        for (ipo_size_t j = 0u; j < count; j++) {
            if (widths[j] == m->exec_bits) {
                seen = true;
                break;
            }
        }
        if (seen) {
            continue;
        }
        if (count >= cap) {
            ipo_size_t new_cap = (cap == 0u) ? 4u : (cap * 2u);
            ipo_size_t old_bytes = cap * (ipo_size_t)sizeof(ipo_bitwidth_t);
            ipo_size_t new_bytes = new_cap * (ipo_size_t)sizeof(ipo_bitwidth_t);
            ipo_bitwidth_t *grown = widths
                ? (ipo_bitwidth_t *)s_kernel_mem_ops.realloc_fn(widths, old_bytes, new_bytes, s_kernel_mem_ops.ctx)
                : (ipo_bitwidth_t *)s_kernel_mem_ops.alloc(new_bytes, s_kernel_mem_ops.ctx);
            if (!grown) {
                if (widths) s_kernel_mem_ops.free_fn(widths, s_kernel_mem_ops.ctx);
                return false;
            }
            widths = grown;
            cap = new_cap;
        }
        widths[count++] = m->exec_bits;
    }

    for (ipo_size_t i = 0u; i < count; i++) {
        for (ipo_size_t j = i + 1u; j < count; j++) {
            if (widths[j] < widths[i]) {
                ipo_bitwidth_t tmp = widths[i];
                widths[i] = widths[j];
                widths[j] = tmp;
            }
        }
    }

    *out_widths = widths;
    *out_count = count;
    return true;
}

static void print_supported_bitnesses(const char *prefix, uint32_t active_mode_id) {
    ipo_bitwidth_t *widths = NULL;
    ipo_size_t count = 0u;

    if (!collect_supported_bitnesses(active_mode_id, &widths, &count)) {
        printf("%s<alloc_err>\n", prefix ? prefix : "");
        return;
    }

    printf("%s", prefix ? prefix : "");
    if (count == 0u) {
        printf("none\n");
    } else {
        for (ipo_size_t i = 0u; i < count; i++) {
            printf("%s%llu-bit", (i > 0u) ? ", " : "", (unsigned long long)widths[i]);
        }
        printf("\n");
    }

    if (widths) {
        s_kernel_mem_ops.free_fn(widths, s_kernel_mem_ops.ctx);
    }
}

void cpu_info_init(const ipo_boot_handover_t *handover) {
    (void)handover;
    if (s_handover_buf) {
        kfree(s_handover_buf);
        s_handover_buf = NULL;
    }
    if (s_cpu_info_ready) {
        exec_mode_registry_free(&s_registry);
        cpu_facts_free(&s_facts, &s_kernel_mem_ops);
    }
    summary_free_strings(&s_summary);

    cpu_facts_init(&s_facts);
    exec_mode_registry_init(&s_registry, &s_kernel_mem_ops);
    memset(&s_summary, 0, sizeof(s_summary));
    if (s_auto_handover.cpu_vendor) kfree(s_auto_handover.cpu_vendor);
    if (s_auto_handover.cpu_brand)  kfree(s_auto_handover.cpu_brand);
    memset(&s_auto_handover, 0, sizeof(s_auto_handover));
    s_auto_handover.magic = IPO_BOOT_HANDOVER_SIG_U64;
    s_auto_handover.total_bytes = sizeof(s_auto_handover);

    printf("[cpu_info] Init transition: probing CPU capabilities and address widths...\n");
    cpu_discovery_probe_platform(&s_facts, memory_map_get_total_ram(), true, &s_kernel_mem_ops);

    cpu_discovery_reset_providers(&s_kernel_mem_ops);
    cpu_discovery_register_platform_providers(&s_kernel_mem_ops);
    (void)cpu_discovery_run(&s_facts, &s_registry, &s_kernel_mem_ops);

    arch_backend_reset(&s_kernel_mem_ops);
    backend_register_platform_all(&s_kernel_mem_ops);
    arch_backend_bind_registry(&s_facts, NULL, &s_registry, &s_kernel_mem_ops);

    exec_mode_selection_t sel;
    const cpu_exec_mode_desc_t *best = exec_mode_select_best(
        &s_facts, &s_registry, NULL, NULL, NULL, &sel);

    uint32_t target_mode_id = best ? best->mode_id : 1u;
    print_supported_bitnesses("[cpu_info] Supported bitnesses: ", target_mode_id);

    if (best && best->backend_ptr) {
        printf("[cpu_info] Transition selected: 16-bit Real -> %s (%llu-bit, paging=%u-lvl)\n",
               best->name ? best->name : "unknown",
               (unsigned long long)best->exec_bits,
               best->paging_levels);
        const arch_transition_ops_t *be = (const arch_transition_ops_t *)best->backend_ptr;
        if (be->prepare) {
            be->prepare(&s_facts, best, &s_auto_handover);
        }
        if (be->enter_and_verify) {
            be->enter_and_verify(best, &s_auto_handover, 0u);
        }
        if (be->verify_active) {
            be->verify_active(best, &s_auto_handover);
        }
        arch_cpu_backend_init();
    } else {
        arch_cpu_backend_init();
    }

    s_summary.arch_family = s_facts.arch_family;
    s_summary.initial_mode_id = 1u;
    s_summary.initial_exec_bits = 16u;
    s_summary.active_mode_id = best ? best->mode_id : 1u;
    s_summary.verified_cs = s_auto_handover.verified_cs;
    (void)summary_set_cstr(&s_summary.vendor, &s_summary.vendor_cap, s_facts.vendor ? s_facts.vendor : "");
    (void)summary_set_cstr(&s_summary.brand, &s_summary.brand_cap, s_facts.brand ? s_facts.brand : "");

    populate_summary_from_live_hw();
    s_cpu_info_ready = true;

    printf("[cpu_info] Hardware switch applied: %s (exec=%llu gpr=%llu va=%llu pa=%llu ptr=%llu pg=%u)\n",
           s_summary.mode_name ? s_summary.mode_name : "unknown",
           (unsigned long long)s_summary.exec_bits,
           (unsigned long long)s_summary.gpr_bits,
           (unsigned long long)s_summary.vaddr_bits,
           (unsigned long long)s_summary.paddr_bits,
           (unsigned long long)s_summary.ptr_bits,
           s_summary.paging_levels);
    printf("[cpu_info] Verified HW: CR0=0x%llx CR3=0x%llx CR4=0x%llx EFER=0x%llx CS=0x%02x verified=%s\n",
           (unsigned long long)s_summary.verified_cr0,
           (unsigned long long)s_summary.verified_cr3,
           (unsigned long long)s_summary.verified_cr4,
           (unsigned long long)s_summary.verified_efer,
           (unsigned)s_summary.verified_cs,
           s_summary.switch_verified ? "YES" : "NO");
}

void cpu_info_print_boot_summary(void) {
    if (!s_cpu_info_ready) {
        return;
    }
    populate_summary_from_live_hw();
    const ipo_cpu_runtime_summary_t *sum = &s_summary;

    uint64_t cr0 = sum->verified_cr0;
    uint64_t cr4 = sum->verified_cr4;
    uint64_t efer = sum->verified_efer;

    printf("\n=== Kernel Initialization: Mode Transition & Bitness ===\n");
    print_supported_bitnesses("  Supported Bitnesses: ", sum->active_mode_id);
    printf("  Transition:          %llu-bit -> 32-bit -> %s\n",
           (unsigned long long)sum->initial_exec_bits,
           sum->mode_name ? sum->mode_name : "unknown");
    printf("  Active Widths:       exec=%llu gpr=%llu va=%llu pa=%llu ptr=%llu (pg=%u-lvl)\n",
           (unsigned long long)sum->exec_bits,
           (unsigned long long)sum->gpr_bits,
           (unsigned long long)sum->vaddr_bits,
           (unsigned long long)sum->paddr_bits,
           (unsigned long long)sum->ptr_bits,
           sum->paging_levels);
    printf("  HW State (Live):     CR0=0x%llx CR3=0x%llx CR4=0x%llx EFER=0x%llx\n",
           (unsigned long long)cr0,
           (unsigned long long)sum->verified_cr3,
           (unsigned long long)cr4,
           (unsigned long long)efer);
    printf("  HW Proof Bits:       CS=0x%02x PE=%u PG=%u PAE=%u LA57=%u LMA=%u (ok=%s)\n",
           (unsigned)sum->verified_cs,
           (unsigned)((cr0 & 1ULL) ? 1u : 0u),
           (unsigned)((cr0 & (1ULL << 31)) ? 1u : 0u),
           (unsigned)((cr4 & (1ULL << 5)) ? 1u : 0u),
           (unsigned)((cr4 & (1ULL << 12)) ? 1u : 0u),
           (unsigned)((efer & (1ULL << 10)) ? 1u : 0u),
           sum->switch_verified ? "YES" : "NO");
}

const ipo_cpu_runtime_summary_t *cpu_info_get_summary(void) {
    if (!s_cpu_info_ready) return NULL;
    populate_summary_from_live_hw();
    return &s_summary;
}

const exec_mode_registry_t *cpu_info_get_mode_registry(void) {
    return s_cpu_info_ready ? &s_registry : NULL;
}

const cpu_exec_mode_desc_t *cpu_info_get_active_mode(void) {
    if (!s_cpu_info_ready) return NULL;
    return exec_mode_registry_find(&s_registry, s_summary.active_mode_id);
}

const cpu_hw_facts_t *cpu_info_get_hw_facts(void) {
    return s_cpu_info_ready ? &s_facts : NULL;
}

const ipo_boot_handover_t *cpu_info_get_handover(void) {
    return s_handover_buf ? s_handover_buf : &s_auto_handover;
}

bool cpu_info_has_cap(uint32_t cap_id) {
    if (!s_cpu_info_ready) return false;
    return ipo_capset_has(&s_facts.hw_caps, cap_id);
}

void cpu_info_print_report(void) {
    if (!s_cpu_info_ready) {
        printf("[cpu_info] Not initialized\n");
        return;
    }
    populate_summary_from_live_hw();
    const ipo_cpu_runtime_summary_t *sum = cpu_info_get_summary();
    if (!sum) {
        printf("[cpu_info] Not initialized\n");
        return;
    }

    printf("\n=== Execution Architecture & Multi-Bit Capability Report ===\n");
    printf("  Initial Boot Mode:  %llu-bit Real Mode (mode_id=%u)\n",
           (unsigned long long)sum->initial_exec_bits, sum->initial_mode_id);
    printf("  Active Mode:        %s (mode_id=%u)\n",
           sum->mode_name ? sum->mode_name : "unknown", sum->active_mode_id);
    print_supported_bitnesses("  Supported Bitness:  ", sum->active_mode_id);
    printf("  Widths (bits):      exec=%llu, gpr=%llu, vaddr=%llu, paddr=%llu, ptr=%llu\n",
           (unsigned long long)sum->exec_bits,
           (unsigned long long)sum->gpr_bits,
           (unsigned long long)sum->vaddr_bits,
           (unsigned long long)sum->paddr_bits,
           (unsigned long long)sum->ptr_bits);
    printf("  Paging Hierarchy:   %u-level (SwitchVerified=%s)\n",
           sum->paging_levels,
           sum->switch_verified ? "YES" : "NO");

    ipo_size_t reg_cnt = arch_reg_count();
    printf("\n  Architectural Register Descriptors (%llu):\n", (unsigned long long)reg_cnt);
    printf("  NAME           WIDTH  ACCESS  VALUE               PURPOSE\n");
    printf("  --------------------------------------------------------------------------------\n");
    for (ipo_size_t i = 0u; i < reg_cnt; i++) {
        const arch_reg_desc_t *rd = arch_reg_get_desc(i);
        if (!rd) continue;
        bool avail = arch_reg_is_available(rd);
        arch_reg_value_t val;
        arch_reg_value_init(&val);
        int rc = avail ? arch_reg_read(rd, &val) : -3;
        char *val_buf = NULL;
        if (rc == 0) {
            unsigned long long v = (unsigned long long)arch_reg_value_as_u64(&val);
            int need = snprintf(NULL, 0, "0x%016llx", v);
            if (need >= 0) {
                val_buf = (char *)kmalloc((size_t)need + 1u);
                if (val_buf) snprintf(val_buf, (size_t)need + 1u, "0x%016llx", v);
            }
        }
        arch_reg_value_free(&val);
        printf("  %-14s %3llu    %c%c%c     %-18s  %s\n",
               rd->name ? rd->name : "?",
               (unsigned long long)rd->bit_width,
               (rd->access_flags & ARCH_REG_ACCESS_READABLE) ? 'R' : '-',
               (rd->access_flags & ARCH_REG_ACCESS_WRITABLE) ? 'W' : '-',
               (rd->access_flags & ARCH_REG_ACCESS_PRIVILEGED) ? 'P' : 'U',
               val_buf ? val_buf : "<unavailable>",
               rd->purpose ? rd->purpose : "-");
        if (val_buf) kfree(val_buf);
    }

    ipo_size_t supported_count = 0u;
    for (ipo_size_t i = 0u; i < s_registry.count; i++) {
        const cpu_exec_mode_desc_t *m = &s_registry.items[i];
        if (mode_is_supported(m, sum->active_mode_id)) {
            supported_count++;
        }
    }

    printf("\n  Supported Execution Modes (%llu):\n", (unsigned long long)supported_count);
    printf("  ID  MODE                              EX/VA/PA/PT   LVL STATUS          REASON\n");
    printf("  --------------------------------------------------------------------------------\n");
    for (ipo_size_t i = 0u; i < s_registry.count; i++) {
        const cpu_exec_mode_desc_t *m = &s_registry.items[i];
        if (!mode_is_supported(m, sum->active_mode_id)) {
            continue;
        }
        bool is_active = (m->mode_id == sum->active_mode_id);
        const char *st = is_active ? "ACTIVE" : mode_status_name(m->status);
        const char *detail = (m->status_detail && m->status_detail[0]) ? m->status_detail : "-";
        printf("  %-2u  %-32s  %2llu/%2llu/%2llu/%2llu   %u   %-14s  %s\n",
               m->mode_id,
               m->name ? m->name : "?",
               (unsigned long long)m->exec_bits,
               (unsigned long long)m->vaddr_bits,
               (unsigned long long)m->paddr_bits,
               (unsigned long long)m->ptr_bits,
               m->paging_levels,
               st,
               detail);
    }
}
