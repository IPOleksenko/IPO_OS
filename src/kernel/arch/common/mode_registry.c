#include <arch/exec_mode.h>

static char **s_arch_names = NULL;
static ipo_size_t s_arch_count = 0u;
static ipo_size_t s_arch_cap = 0u;

static void *reg_alloc(const ipo_mem_ops_t *mem, ipo_size_t sz) {
    if (!mem || !mem->alloc || sz == 0u) return NULL;
    return mem->alloc(sz, mem->ctx);
}

static void *reg_realloc(const ipo_mem_ops_t *mem, void *ptr, ipo_size_t old_sz, ipo_size_t new_sz) {
    if (!mem) return NULL;
    if (mem->realloc_fn) return mem->realloc_fn(ptr, old_sz, new_sz, mem->ctx);
    void *np = reg_alloc(mem, new_sz);
    if (!np) return NULL;
    if (ptr && old_sz > 0u) {
        ipo_size_t copy_sz = (old_sz < new_sz) ? old_sz : new_sz;
        const uint8_t *s = (const uint8_t *)ptr;
        uint8_t *d = (uint8_t *)np;
        for (ipo_size_t i = 0u; i < copy_sz; i++) d[i] = s[i];
        if (mem->free_fn) mem->free_fn(ptr, mem->ctx);
    }
    return np;
}

static void reg_free(const ipo_mem_ops_t *mem, void *ptr) {
    if (mem && mem->free_fn && ptr) mem->free_fn(ptr, mem->ctx);
}

static void local_zero(void *dst, ipo_size_t n) {
    uint8_t *d = (uint8_t *)dst;
    for (ipo_size_t i = 0u; i < n; i++) d[i] = 0u;
}

static ipo_size_t local_strlen(const char *s) {
    if (!s) return 0u;
    ipo_size_t n = 0u;
    while (s[n]) n++;
    return n;
}

static bool local_streq(const char *a, const char *b) {
    if (!a || !b) return false;
    while (*a && *a == *b) {
        a++;
        b++;
    }
    return (*a == '\0' && *b == '\0');
}

static char *dup_dyn_str(const char *src, const ipo_mem_ops_t *mem) {
    if (!src || !mem || !mem->alloc) return NULL;
    ipo_size_t len = local_strlen(src);
    char *s = (char *)reg_alloc(mem, len + 1u);
    if (!s) return NULL;
    for (ipo_size_t i = 0u; i <= len; i++) s[i] = src[i];
    return s;
}

static bool set_dyn_str(char **dst, ipo_size_t *cap, const char *src, const ipo_mem_ops_t *mem) {
    if (!dst || !cap) return false;
    if (!src) src = "";
    ipo_size_t len = local_strlen(src);
    if (len + 1u > *cap) {
        ipo_size_t ncap = (*cap == 0u) ? 32u : *cap;
        while (ncap < len + 1u) ncap *= 2u;
        char *nb = (char *)reg_realloc(mem, *dst, *cap, ncap);
        if (!nb) return false;
        *dst = nb;
        *cap = ncap;
    }
    for (ipo_size_t i = 0u; i <= len; i++) (*dst)[i] = src[i];
    return true;
}

uint32_t cpu_arch_register_name(const char *arch_name, const ipo_mem_ops_t *mem) {
    if (!arch_name || !arch_name[0]) return IPO_ARCH_FAMILY_UNKNOWN;
    for (ipo_size_t i = 0u; i < s_arch_count; i++) {
        if (s_arch_names[i] && local_streq(s_arch_names[i], arch_name)) {
            return (uint32_t)(i + 1u);
        }
    }
    if (!mem || !mem->alloc) return IPO_ARCH_FAMILY_UNKNOWN;
    if (s_arch_count >= s_arch_cap) {
        ipo_size_t old_bytes = s_arch_cap * sizeof(char *);
        ipo_size_t ncap = s_arch_cap ? (s_arch_cap * 2u) : 4u;
        char **na = (char **)reg_realloc(mem, s_arch_names, old_bytes, ncap * sizeof(char *));
        if (!na) return IPO_ARCH_FAMILY_UNKNOWN;
        s_arch_names = na;
        s_arch_cap = ncap;
    }
    char *copy = dup_dyn_str(arch_name, mem);
    if (!copy) return IPO_ARCH_FAMILY_UNKNOWN;
    s_arch_names[s_arch_count++] = copy;
    return (uint32_t)s_arch_count;
}

const char *cpu_arch_get_name(uint32_t arch_id) {
    if (arch_id == IPO_ARCH_FAMILY_UNKNOWN || (ipo_size_t)arch_id > s_arch_count) {
        return "unknown";
    }
    return s_arch_names[arch_id - 1u] ? s_arch_names[arch_id - 1u] : "unknown";
}

void ipo_capset_init(ipo_capset_t *cs) {
    if (!cs) return;
    cs->words = NULL;
    cs->word_count = 0u;
    cs->word_cap = 0u;
}

void ipo_capset_free(ipo_capset_t *cs, const ipo_mem_ops_t *mem) {
    if (!cs) return;
    if (cs->words) reg_free(mem, cs->words);
    cs->words = NULL;
    cs->word_count = 0u;
    cs->word_cap = 0u;
}

bool ipo_capset_set(ipo_capset_t *cs, uint32_t cap_id, const ipo_mem_ops_t *mem) {
    if (!cs) return false;
    ipo_size_t word_idx = (ipo_size_t)(cap_id >> 6);
    uint32_t bit = cap_id & 63u;
    if (word_idx + 1u > cs->word_cap) {
        ipo_size_t old_bytes = cs->word_cap * sizeof(uint64_t);
        ipo_size_t ncap = cs->word_cap ? cs->word_cap : 2u;
        while (ncap < word_idx + 1u) ncap *= 2u;
        uint64_t *nw = (uint64_t *)reg_realloc(mem, cs->words, old_bytes, ncap * sizeof(uint64_t));
        if (!nw) return false;
        for (ipo_size_t i = cs->word_cap; i < ncap; i++) nw[i] = 0ULL;
        cs->words = nw;
        cs->word_cap = ncap;
    }
    if (word_idx + 1u > cs->word_count) cs->word_count = word_idx + 1u;
    cs->words[word_idx] |= (1ULL << bit);
    return true;
}

void ipo_capset_clear(ipo_capset_t *cs, uint32_t cap_id) {
    if (!cs || !cs->words) return;
    ipo_size_t word_idx = (ipo_size_t)(cap_id >> 6);
    if (word_idx >= cs->word_count) return;
    cs->words[word_idx] &= ~(1ULL << (cap_id & 63u));
}

bool ipo_capset_has(const ipo_capset_t *cs, uint32_t cap_id) {
    if (!cs || !cs->words) return false;
    ipo_size_t word_idx = (ipo_size_t)(cap_id >> 6);
    if (word_idx >= cs->word_count) return false;
    return (cs->words[word_idx] & (1ULL << (cap_id & 63u))) != 0ULL;
}

bool ipo_capset_contains_all(const ipo_capset_t *superset, const ipo_capset_t *subset) {
    if (!subset || subset->word_count == 0u) return true;
    if (!superset || !superset->words) {
        for (ipo_size_t i = 0u; i < subset->word_count; i++) {
            if (subset->words && subset->words[i] != 0ULL) return false;
        }
        return true;
    }
    for (ipo_size_t i = 0u; i < subset->word_count; i++) {
        uint64_t req = subset->words[i];
        if (req == 0ULL) continue;
        uint64_t have = (i < superset->word_count) ? superset->words[i] : 0ULL;
        if ((have & req) != req) return false;
    }
    return true;
}

bool ipo_capset_copy(ipo_capset_t *dst, const ipo_capset_t *src, const ipo_mem_ops_t *mem) {
    if (!dst) return false;
    if (!src || src->word_count == 0u || !src->words) {
        if (dst->words) {
            for (ipo_size_t i = 0u; i < dst->word_cap; i++) dst->words[i] = 0ULL;
        }
        dst->word_count = 0u;
        return true;
    }
    if (src->word_count > dst->word_cap) {
        ipo_size_t old_bytes = dst->word_cap * sizeof(uint64_t);
        uint64_t *nw = (uint64_t *)reg_realloc(mem, dst->words, old_bytes, src->word_count * sizeof(uint64_t));
        if (!nw) return false;
        dst->words = nw;
        dst->word_cap = src->word_count;
    }
    for (ipo_size_t i = 0u; i < src->word_count; i++) dst->words[i] = src->words[i];
    for (ipo_size_t i = src->word_count; i < dst->word_cap; i++) dst->words[i] = 0ULL;
    dst->word_count = src->word_count;
    return true;
}

void exec_mode_registry_init(exec_mode_registry_t *reg, const ipo_mem_ops_t *mem) {
    if (!reg) return;
    local_zero(reg, sizeof(*reg));
    if (mem) reg->mem = *mem;
    reg->next_id = 1u;
}

void exec_mode_registry_free(exec_mode_registry_t *reg) {
    if (!reg) return;
    if (reg->items) {
        for (ipo_size_t i = 0u; i < reg->count; i++) {
            cpu_exec_mode_desc_t *m = &reg->items[i];
            if (m->name) reg_free(&reg->mem, m->name);
            if (m->status_detail) reg_free(&reg->mem, m->status_detail);
            ipo_capset_free(&m->required_caps, &reg->mem);
            ipo_capset_free(&m->provided_caps, &reg->mem);
            if (m->predicates) {
                for (ipo_size_t p = 0u; p < m->pred_count; p++) {
                    if (m->predicates[p].cap_name) {
                        reg_free(&reg->mem, m->predicates[p].cap_name);
                    }
                }
                reg_free(&reg->mem, m->predicates);
            }
            if (m->switch_plan.msr_ops) {
                reg_free(&reg->mem, m->switch_plan.msr_ops);
            }
        }
        reg_free(&reg->mem, reg->items);
    }
    reg->items = NULL;
    reg->count = 0u;
    reg->cap = 0u;
}

cpu_exec_mode_desc_t *exec_mode_registry_add(exec_mode_registry_t *reg,
                                             uint32_t arch_family,
                                             const char *name,
                                             ipo_bitwidth_t exec_bits,
                                             ipo_bitwidth_t gpr_bits,
                                             ipo_bitwidth_t vaddr_bits,
                                             ipo_bitwidth_t paddr_bits,
                                             ipo_bitwidth_t ptr_bits,
                                             uint32_t paging_levels) {
    if (!reg) return NULL;
    if (reg->count >= reg->cap) {
        ipo_size_t old_bytes = reg->cap * sizeof(cpu_exec_mode_desc_t);
        ipo_size_t ncap = (reg->cap == 0u) ? 8u : (reg->cap * 2u);
        cpu_exec_mode_desc_t *na = (cpu_exec_mode_desc_t *)reg_realloc(
            &reg->mem, reg->items, old_bytes, ncap * sizeof(cpu_exec_mode_desc_t));
        if (!na) return NULL;
        local_zero(&na[reg->cap], (ncap - reg->cap) * sizeof(cpu_exec_mode_desc_t));
        reg->items = na;
        reg->cap = ncap;
    }

    cpu_exec_mode_desc_t *m = &reg->items[reg->count];
    local_zero(m, sizeof(*m));
    m->mode_id = reg->next_id++;
    if (reg->next_id == 0u) reg->next_id = 1u;
    m->arch_family = arch_family;
    m->exec_bits = exec_bits;
    m->gpr_bits = gpr_bits;
    m->vaddr_bits = vaddr_bits;
    m->paddr_bits = paddr_bits;
    m->ptr_bits = ptr_bits;
    m->paging_levels = paging_levels;
    m->switch_plan.target_exec_bits = exec_bits;
    m->switch_plan.paging_levels = paging_levels;
    ipo_capset_init(&m->required_caps);
    ipo_capset_init(&m->provided_caps);

    if (!set_dyn_str(&m->name, &m->name_cap, name ? name : "unnamed-mode", &reg->mem)) {
        return NULL;
    }
    reg->count++;
    return m;
}

bool exec_mode_set_detail(cpu_exec_mode_desc_t *mode, const char *detail, const ipo_mem_ops_t *mem) {
    if (!mode) return false;
    return set_dyn_str(&mode->status_detail, &mode->status_detail_cap, detail ? detail : "", mem);
}

bool exec_mode_add_predicate(cpu_exec_mode_desc_t *mode,
                             const ipo_hw_predicate_t *pred,
                             const ipo_mem_ops_t *mem) {
    if (!mode || !pred || !mem) return false;
    if (mode->pred_count >= mode->pred_cap) {
        ipo_size_t old_bytes = mode->pred_cap * sizeof(ipo_hw_predicate_t);
        ipo_size_t ncap = mode->pred_cap ? (mode->pred_cap * 2u) : 4u;
        ipo_hw_predicate_t *na = (ipo_hw_predicate_t *)reg_realloc(
            mem, mode->predicates, old_bytes, ncap * sizeof(ipo_hw_predicate_t));
        if (!na) return false;
        local_zero(&na[mode->pred_cap], (ncap - mode->pred_cap) * sizeof(ipo_hw_predicate_t));
        mode->predicates = na;
        mode->pred_cap = ncap;
    }
    ipo_hw_predicate_t *dst = &mode->predicates[mode->pred_count];
    *dst = *pred;
    dst->cap_name = NULL;
    if (pred->cap_name && pred->cap_name[0]) {
        dst->cap_name = dup_dyn_str(pred->cap_name, mem);
        if (!dst->cap_name) return false;
    }
    mode->pred_count++;
    return true;
}

bool exec_mode_add_msr_op(cpu_exec_mode_desc_t *mode,
                          uint32_t msr_addr,
                          uint64_t set_mask,
                          uint64_t clear_mask,
                          const ipo_mem_ops_t *mem) {
    if (!mode || !mem) return false;
    arch_hw_switch_plan_t *sp = &mode->switch_plan;
    if (sp->msr_op_count >= sp->msr_op_cap) {
        ipo_size_t old_bytes = sp->msr_op_cap * sizeof(arch_msr_op_t);
        ipo_size_t ncap = sp->msr_op_cap ? (sp->msr_op_cap * 2u) : 2u;
        arch_msr_op_t *na = (arch_msr_op_t *)reg_realloc(
            mem, sp->msr_ops, old_bytes, ncap * sizeof(arch_msr_op_t));
        if (!na) return false;
        local_zero(&na[sp->msr_op_cap], (ncap - sp->msr_op_cap) * sizeof(arch_msr_op_t));
        sp->msr_ops = na;
        sp->msr_op_cap = ncap;
    }
    arch_msr_op_t *op = &sp->msr_ops[sp->msr_op_count++];
    op->msr_addr = msr_addr;
    op->set_mask = set_mask;
    op->clear_mask = clear_mask;
    return true;
}

cpu_exec_mode_desc_t *exec_mode_registry_find(exec_mode_registry_t *reg, ipo_id_t mode_id) {
    if (!reg || mode_id == 0u) return NULL;
    for (ipo_size_t i = 0u; i < reg->count; i++) {
        if (reg->items[i].mode_id == mode_id) return &reg->items[i];
    }
    return NULL;
}

cpu_exec_mode_desc_t *exec_mode_registry_find_matching(exec_mode_registry_t *reg,
                                                       uint32_t arch_family,
                                                       ipo_bitwidth_t exec_bits,
                                                       ipo_bitwidth_t gpr_bits,
                                                       ipo_bitwidth_t vaddr_bits,
                                                       uint32_t paging_levels) {
    if (!reg) return NULL;
    for (ipo_size_t i = 0u; i < reg->count; i++) {
        cpu_exec_mode_desc_t *m = &reg->items[i];
        if (m->arch_family == arch_family &&
            m->exec_bits == exec_bits &&
            m->gpr_bits == gpr_bits &&
            (vaddr_bits == 0u || m->vaddr_bits == vaddr_bits) &&
            (paging_levels == 0u || m->paging_levels == paging_levels)) {
            return m;
        }
    }
    return NULL;
}

const cpu_exec_mode_desc_t *exec_mode_registry_get(const exec_mode_registry_t *reg, ipo_size_t index) {
    if (!reg || index >= reg->count) return NULL;
    return &reg->items[index];
}
