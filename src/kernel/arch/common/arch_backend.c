#include <arch/arch_backend.h>

static const arch_transition_ops_t **s_backends = NULL;
static ipo_size_t s_backend_count = 0u;
static ipo_size_t s_backend_cap = 0u;

static bool streq_be(const char *a, const char *b) {
    if (!a || !b) return false;
    while (*a && *a == *b) {
        a++;
        b++;
    }
    return (*a == '\0' && *b == '\0');
}

static void *be_realloc(const ipo_mem_ops_t *mem, void *ptr, ipo_size_t old_sz, ipo_size_t new_sz) {
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

void arch_backend_reset(const ipo_mem_ops_t *mem) {
    if (s_backends && mem && mem->free_fn) {
        mem->free_fn(s_backends, mem->ctx);
    }
    s_backends = NULL;
    s_backend_count = 0u;
    s_backend_cap = 0u;
}

bool arch_backend_register(const arch_transition_ops_t *ops, const ipo_mem_ops_t *mem) {
    if (!ops || !ops->matches_mode || !mem) return false;
    if (s_backend_count >= s_backend_cap) {
        ipo_size_t old_sz = s_backend_cap * sizeof(const arch_transition_ops_t *);
        ipo_size_t ncap = s_backend_cap ? (s_backend_cap * 2u) : 4u;
        const arch_transition_ops_t **na = (const arch_transition_ops_t **)be_realloc(
            mem, s_backends, old_sz, ncap * sizeof(const arch_transition_ops_t *));
        if (!na) return false;
        s_backends = na;
        s_backend_cap = ncap;
    }
    s_backends[s_backend_count++] = ops;
    return true;
}

const arch_transition_ops_t *arch_backend_find_for_mode(const cpu_exec_mode_desc_t *mode) {
    if (!mode) return NULL;
    const char *mode_arch = cpu_arch_get_name(mode->arch_family);
    for (ipo_size_t i = 0u; i < s_backend_count; i++) {
        const arch_transition_ops_t *ops = s_backends[i];
        if (ops && ops->arch_name && streq_be(ops->arch_name, mode_arch) && ops->matches_mode(mode)) {
            return ops;
        }
    }
    return NULL;
}

ipo_size_t arch_backend_bind_registry(const cpu_hw_facts_t *facts,
                                      const cpu_exec_mode_desc_t *current_mode,
                                      exec_mode_registry_t *reg,
                                      const ipo_mem_ops_t *mem) {
    if (!reg) return 0u;
    ipo_size_t bound = 0u;
    for (ipo_size_t i = 0u; i < reg->count; i++) {
        cpu_exec_mode_desc_t *m = &reg->items[i];
        const arch_transition_ops_t *ops = arch_backend_find_for_mode(m);
        m->backend_ptr = ops;
        m->backend_available = (ops != NULL);
        if (ops && facts) {
            const char *reason = NULL;
            m->transition_possible = ops->can_transition
                ? ops->can_transition(facts, current_mode, m, &reason)
                : true;
            if (!m->transition_possible && reason) {
                exec_mode_set_detail(m, reason, mem);
            }
            bound++;
        }
    }
    return bound;
}
