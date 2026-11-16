#include <arch/cpu_discovery.h>

static void zero_bytes(void *dst, ipo_size_t n) {
    uint8_t *d = (uint8_t *)dst;
    for (ipo_size_t i = 0u; i < n; i++) d[i] = 0u;
}

static bool streq_local(const char *a, const char *b) {
    if (!a || !b) return false;
    while (*a && *a == *b) {
        a++;
        b++;
    }
    return (*a == '\0' && *b == '\0');
}

static bool starts_with_local(const char *s, const char *prefix) {
    if (!s || !prefix) return false;
    while (*prefix) {
        if (*s != *prefix) return false;
        s++;
        prefix++;
    }
    return true;
}

typedef struct {
    char *name;
} dyn_cap_name_t;

/* Runtime-interned capability names; no fixed ISA/bitness table */
static dyn_cap_name_t *s_dyn_caps = NULL;
static ipo_size_t s_dyn_cap_count = 0u;
static ipo_size_t s_dyn_cap_cap = 0u;

static cpu_capability_provider_t *s_providers = NULL;
static ipo_size_t s_provider_count = 0u;
static ipo_size_t s_provider_cap = 0u;

static void *common_realloc(const ipo_mem_ops_t *mem, void *ptr, ipo_size_t old_sz, ipo_size_t new_sz) {
    if (!mem) return NULL;
    if (mem->realloc_fn) return mem->realloc_fn(ptr, old_sz, new_sz, mem->ctx);
    if (!mem->alloc) return NULL;
    void *np = mem->alloc(new_sz, mem->ctx);
    if (!np) return NULL;
    if (ptr && old_sz > 0u) {
        ipo_size_t csz = (old_sz < new_sz) ? old_sz : new_sz;
        const uint8_t *s = (const uint8_t *)ptr;
        uint8_t *d = (uint8_t *)np;
        for (ipo_size_t i = 0u; i < csz; i++) d[i] = s[i];
        if (mem->free_fn) mem->free_fn(ptr, mem->ctx);
    }
    return np;
}

uint32_t cpu_cap_find_name(const char *cap_name) {
    if (!cap_name || !cap_name[0]) return 0xFFFFFFFFu;
    for (ipo_size_t i = 0u; i < s_dyn_cap_count; i++) {
        if (s_dyn_caps[i].name && streq_local(s_dyn_caps[i].name, cap_name)) {
            return (uint32_t)i;
        }
    }
    return 0xFFFFFFFFu;
}

uint32_t cpu_cap_register_name(const char *cap_name, const ipo_mem_ops_t *mem) {
    if (!cap_name || !cap_name[0]) return 0xFFFFFFFFu;
    uint32_t existing = cpu_cap_find_name(cap_name);
    if (existing != 0xFFFFFFFFu) return existing;
    if (!mem || !mem->alloc) return 0xFFFFFFFFu;

    if (s_dyn_cap_count >= s_dyn_cap_cap) {
        ipo_size_t old_sz = s_dyn_cap_cap * sizeof(dyn_cap_name_t);
        ipo_size_t ncap = s_dyn_cap_cap ? (s_dyn_cap_cap * 2u) : 16u;
        dyn_cap_name_t *na = (dyn_cap_name_t *)common_realloc(
            mem, s_dyn_caps, old_sz, ncap * sizeof(dyn_cap_name_t));
        if (!na) return 0xFFFFFFFFu;
        s_dyn_caps = na;
        s_dyn_cap_cap = ncap;
    }

    ipo_size_t len = 0u;
    while (cap_name[len]) len++;
    char *s = (char *)mem->alloc(len + 1u, mem->ctx);
    if (!s) return 0xFFFFFFFFu;
    for (ipo_size_t i = 0u; i <= len; i++) s[i] = cap_name[i];

    uint32_t id = (uint32_t)s_dyn_cap_count;
    s_dyn_caps[s_dyn_cap_count].name = s;
    s_dyn_cap_count++;
    return id;
}

const char *cpu_cap_get_name(uint32_t cap_id) {
    if ((ipo_size_t)cap_id < s_dyn_cap_count && s_dyn_caps[cap_id].name) {
        return s_dyn_caps[cap_id].name;
    }
    return "unknown_cap";
}

static bool common_set_cstr(const ipo_mem_ops_t *mem,
                            char **dst,
                            ipo_size_t *cap,
                            const char *src) {
    if (!mem || !dst || !cap) return false;
    if (!src) src = "";
    ipo_size_t len = 0u;
    while (src[len]) len++;
    ipo_size_t need = len + 1u;
    if (!*dst || need > *cap) {
        ipo_size_t ncap = (*cap > 0u) ? *cap : 16u;
        while (ncap < need) {
            if (ncap > IPO_SIZE_MAX / 2u) { ncap = need; break; }
            ncap *= 2u;
        }
        char *nb = (char *)common_realloc(mem, *dst, *cap, ncap);
        if (!nb) return false;
        *dst = nb;
        *cap = ncap;
    }
    for (ipo_size_t i = 0u; i <= len; i++) (*dst)[i] = src[i];
    return true;
}

static bool ensure_leaf_regs(cpu_hw_leaf_t *leaf, ipo_size_t need, const ipo_mem_ops_t *mem) {
    if (!leaf) return false;
    if (need == 0u) {
        leaf->reg_count = 0u;
        return true;
    }
    if (!leaf->regs || need > leaf->reg_cap) {
        ipo_size_t ncap = leaf->reg_cap ? leaf->reg_cap : 4u;
        while (ncap < need) {
            if (ncap > IPO_SIZE_MAX / 2u) { ncap = need; break; }
            ncap *= 2u;
        }
        if (ncap > IPO_SIZE_MAX / sizeof(uint64_t)) return false;
        ipo_size_t old_sz = leaf->reg_cap * sizeof(uint64_t);
        uint64_t *nr = (uint64_t *)common_realloc(
            mem, leaf->regs, old_sz, ncap * sizeof(uint64_t));
        if (!nr) return false;
        for (ipo_size_t i = leaf->reg_cap; i < ncap; i++) nr[i] = 0ULL;
        leaf->regs = nr;
        leaf->reg_cap = ncap;
    }
    leaf->reg_count = need;
    return true;
}

void cpu_facts_init(cpu_hw_facts_t *facts) {
    if (!facts) return;
    zero_bytes(facts, sizeof(*facts));
    ipo_capset_init(&facts->hw_caps);
}

void cpu_facts_free(cpu_hw_facts_t *facts, const ipo_mem_ops_t *mem) {
    if (!facts) return;
    ipo_capset_free(&facts->hw_caps, mem);
    if (facts->vendor && mem && mem->free_fn) {
        mem->free_fn(facts->vendor, mem->ctx);
    }
    facts->vendor = NULL;
    facts->vendor_cap = 0u;
    if (facts->brand && mem && mem->free_fn) {
        mem->free_fn(facts->brand, mem->ctx);
    }
    facts->brand = NULL;
    facts->brand_cap = 0u;
    if (facts->hw_leaves) {
        for (ipo_size_t i = 0u; i < facts->hw_leaf_count; i++) {
            if (facts->hw_leaves[i].regs && mem && mem->free_fn) {
                mem->free_fn(facts->hw_leaves[i].regs, mem->ctx);
            }
            facts->hw_leaves[i].regs = NULL;
            facts->hw_leaves[i].reg_count = 0u;
            facts->hw_leaves[i].reg_cap = 0u;
        }
        if (mem && mem->free_fn) {
            mem->free_fn(facts->hw_leaves, mem->ctx);
        }
    }
    facts->hw_leaves = NULL;
    facts->hw_leaf_count = 0u;
    facts->hw_leaf_cap = 0u;
}

bool cpu_facts_set_vendor(cpu_hw_facts_t *facts, const char *vendor, const ipo_mem_ops_t *mem) {
    if (!facts) return false;
    return common_set_cstr(mem, &facts->vendor, &facts->vendor_cap, vendor);
}

bool cpu_facts_set_brand(cpu_hw_facts_t *facts, const char *brand, const ipo_mem_ops_t *mem) {
    if (!facts) return false;
    return common_set_cstr(mem, &facts->brand, &facts->brand_cap, brand);
}

static cpu_hw_leaf_t *cpu_facts_acquire_leaf(cpu_hw_facts_t *facts,
                                           uint32_t space,
                                           uint32_t leaf,
                                           uint32_t subleaf,
                                           ipo_size_t reg_count,
                                           const ipo_mem_ops_t *mem) {
    if (!facts || !mem) return NULL;
    for (ipo_size_t i = 0u; i < facts->hw_leaf_count; i++) {
        cpu_hw_leaf_t *cur = &facts->hw_leaves[i];
        if (cur->space == space && cur->leaf == leaf && cur->subleaf == subleaf) {
            return ensure_leaf_regs(cur, reg_count, mem) ? cur : NULL;
        }
    }
    if (facts->hw_leaf_count >= facts->hw_leaf_cap) {
        ipo_size_t old_sz = facts->hw_leaf_cap * sizeof(cpu_hw_leaf_t);
        ipo_size_t ncap = facts->hw_leaf_cap ? (facts->hw_leaf_cap * 2u) : 16u;
        cpu_hw_leaf_t *na = (cpu_hw_leaf_t *)common_realloc(
            mem, facts->hw_leaves, old_sz, ncap * sizeof(cpu_hw_leaf_t));
        if (!na) return NULL;
        facts->hw_leaves = na;
        facts->hw_leaf_cap = ncap;
    }
    cpu_hw_leaf_t *dst = &facts->hw_leaves[facts->hw_leaf_count];
    zero_bytes(dst, sizeof(*dst));
    dst->space = space;
    dst->leaf = leaf;
    dst->subleaf = subleaf;
    if (!ensure_leaf_regs(dst, reg_count, mem)) {
        return NULL;
    }
    facts->hw_leaf_count++;
    return dst;
}

bool cpu_facts_add_leaf_regs(cpu_hw_facts_t *facts,
                             uint32_t space,
                             uint32_t leaf,
                             uint32_t subleaf,
                             const uint64_t *regs,
                             ipo_size_t reg_count,
                             const ipo_mem_ops_t *mem) {
    cpu_hw_leaf_t *dst = cpu_facts_acquire_leaf(facts, space, leaf, subleaf, reg_count, mem);
    if (!dst) return false;
    for (ipo_size_t r = 0u; r < reg_count; r++) {
        dst->regs[r] = regs ? regs[r] : 0ULL;
    }
    return true;
}

bool cpu_facts_add_leaf(cpu_hw_facts_t *facts,
                        uint32_t space,
                        uint32_t leaf,
                        uint32_t subleaf,
                        uint64_t r0, uint64_t r1, uint64_t r2, uint64_t r3,
                        const ipo_mem_ops_t *mem) {
    cpu_hw_leaf_t *dst = cpu_facts_acquire_leaf(facts, space, leaf, subleaf, 4u, mem);
    if (!dst) return false;
    dst->regs[0] = r0;
    dst->regs[1] = r1;
    dst->regs[2] = r2;
    dst->regs[3] = r3;
    return true;
}

const cpu_hw_leaf_t *cpu_facts_find_leaf(const cpu_hw_facts_t *facts,
                                         uint32_t space,
                                         uint32_t leaf,
                                         uint32_t subleaf) {
    if (!facts || !facts->hw_leaves) return NULL;
    for (ipo_size_t i = 0u; i < facts->hw_leaf_count; i++) {
        const cpu_hw_leaf_t *cur = &facts->hw_leaves[i];
        if (cur->space == space && cur->leaf == leaf && cur->subleaf == subleaf) {
            return cur;
        }
    }
    return NULL;
}

bool cpu_eval_mode_predicates(const cpu_hw_facts_t *facts,
                              const cpu_exec_mode_desc_t *mode,
                              const ipo_mem_ops_t *mem,
                              const char **out_reason) {
    if (!facts || !mode) return false;
    (void)mem;
    if (!ipo_capset_contains_all(&facts->hw_caps, &mode->required_caps)) {
        if (out_reason) *out_reason = "Missing required hardware capability flags";
        return false;
    }

    for (ipo_size_t i = 0u; i < mode->pred_count; i++) {
        const ipo_hw_predicate_t *p = &mode->predicates[i];
        switch ((ipo_pred_op_t)p->op) {
            case IPO_PRED_CAP: {
                uint32_t cid = p->cap_name ? cpu_cap_find_name(p->cap_name) : p->reg_idx;
                if (cid == 0xFFFFFFFFu || !ipo_capset_has(&facts->hw_caps, cid)) {
                    if (out_reason) {
                        *out_reason = (p->cap_name && starts_with_local(p->cap_name, "env."))
                            ? "Required boot/environment resource unavailable"
                            : "Required hardware capability unavailable";
                    }
                    return false;
                }
                break;
            }
            case IPO_PRED_LEAF_MASK: {
                const cpu_hw_leaf_t *lf = cpu_facts_find_leaf(facts, p->space, p->leaf, p->subleaf);
                if (!lf || !lf->regs || p->reg_idx >= lf->reg_count) {
                    if (out_reason) *out_reason = "Invalid hardware leaf register index";
                    return false;
                }
                if ((lf->regs[p->reg_idx] & p->mask) != p->expected) {
                    if (out_reason) *out_reason = "CPU hardware feature leaf bit unsupported";
                    return false;
                }
                break;
            }
            case IPO_PRED_MIN_VADDR_BITS:
                if (facts->virt_addr_bits < (ipo_bitwidth_t)p->expected) {
                    if (out_reason) *out_reason = "CPU virtual address width insufficient";
                    return false;
                }
                break;
            case IPO_PRED_MIN_PADDR_BITS:
                if (facts->phys_addr_bits < (ipo_bitwidth_t)p->expected) {
                    if (out_reason) *out_reason = "CPU physical address width insufficient";
                    return false;
                }
                break;
            case IPO_PRED_MIN_RAM_BYTES:
                if (facts->total_ram_bytes < p->expected) {
                    if (out_reason) *out_reason = "System usable RAM below minimum requirement";
                    return false;
                }
                break;
            default:
                if (out_reason) *out_reason = "Unknown hardware predicate operator";
                return false;
        }
    }
    return true;
}

void cpu_discovery_reset_providers(const ipo_mem_ops_t *mem) {
    if (s_providers && mem && mem->free_fn) {
        mem->free_fn(s_providers, mem->ctx);
    }
    s_providers = NULL;
    s_provider_count = 0u;
    s_provider_cap = 0u;
}

bool cpu_discovery_register_provider(const char *name,
                                     cpu_capability_discover_fn_t fn,
                                     void *ctx,
                                     const ipo_mem_ops_t *mem) {
    if (!name || !fn || !mem) return false;
    if (s_provider_count >= s_provider_cap) {
        ipo_size_t old_sz = s_provider_cap * sizeof(cpu_capability_provider_t);
        ipo_size_t ncap = s_provider_cap ? (s_provider_cap * 2u) : 4u;
        cpu_capability_provider_t *na = (cpu_capability_provider_t *)common_realloc(
            mem, s_providers, old_sz, ncap * sizeof(cpu_capability_provider_t));
        if (!na) return false;
        s_providers = na;
        s_provider_cap = ncap;
    }
    s_providers[s_provider_count].name = name;
    s_providers[s_provider_count].discover = fn;
    s_providers[s_provider_count].ctx = ctx;
    s_provider_count++;
    return true;
}

int cpu_discovery_run(cpu_hw_facts_t *facts,
                      exec_mode_registry_t *reg,
                      const ipo_mem_ops_t *mem) {
    if (!facts || !reg || !mem) return -1;
    for (ipo_size_t i = 0u; i < s_provider_count; i++) {
        if (s_providers[i].discover) {
            int rc = s_providers[i].discover(facts, reg, mem, s_providers[i].ctx);
            if (rc < 0) return rc;
        }
    }
    return 0;
}
