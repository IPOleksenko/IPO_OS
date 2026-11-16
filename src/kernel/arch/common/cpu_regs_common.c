#include <arch/cpu_regs.h>
#include <arch/exec_mode.h>

static const arch_reg_backend_ops_t *s_reg_backend = NULL;
static const ipo_mem_ops_t *s_reg_mem = NULL;

static arch_reg_desc_t *s_dyn_regs = NULL;
static ipo_size_t s_dyn_reg_count = 0u;
static ipo_size_t s_dyn_reg_cap = 0u;
static ipo_id_t s_next_dyn_reg_id = 0x1000u;

static bool str_eq(const char *a, const char *b) {
    if (!a || !b) return false;
    while (*a && *a == *b) { a++; b++; }
    return *a == '\0' && *b == '\0';
}

static void *regs_realloc(void *ptr, ipo_size_t old_sz, ipo_size_t new_sz) {
    if (!s_reg_mem) return NULL;
    if (s_reg_mem->realloc_fn) {
        return s_reg_mem->realloc_fn(ptr, old_sz, new_sz, s_reg_mem->ctx);
    }
    if (!s_reg_mem->alloc) return NULL;
    void *np = s_reg_mem->alloc(new_sz, s_reg_mem->ctx);
    if (!np) return NULL;
    if (ptr && old_sz > 0u) {
        ipo_size_t csz = (old_sz < new_sz) ? old_sz : new_sz;
        const uint8_t *s = (const uint8_t *)ptr;
        uint8_t *d = (uint8_t *)np;
        for (ipo_size_t i = 0u; i < csz; i++) d[i] = s[i];
        if (s_reg_mem->free_fn) s_reg_mem->free_fn(ptr, s_reg_mem->ctx);
    }
    return np;
}

void arch_reg_bind_allocator(const ipo_mem_ops_t *mem) {
    s_reg_mem = mem;
}

void arch_reg_bind_backend(const arch_reg_backend_ops_t *ops) {
    s_reg_backend = ops;
}

void arch_reg_value_init(arch_reg_value_t *val) {
    if (!val) return;
    val->bit_width = 0u;
    val->scalar = 0ULL;
    val->ext_limbs = NULL;
    val->ext_limb_count = 0u;
}

void arch_reg_value_from_u64(arch_reg_value_t *val, ipo_bitwidth_t bit_width, uint64_t scalar) {
    if (!val) return;
    val->bit_width = bit_width ? bit_width : (ipo_bitwidth_t)(sizeof(ipo_reg_t) * 8u);
    val->scalar = scalar;
    val->ext_limbs = NULL;
    val->ext_limb_count = 0u;
}

uint64_t arch_reg_value_as_u64(const arch_reg_value_t *val) {
    if (!val) return 0ULL;
    if (val->ext_limbs && val->ext_limb_count > 0u) return val->ext_limbs[0];
    return val->scalar;
}

void arch_reg_value_free(arch_reg_value_t *val) {
    if (!val) return;
    if (val->ext_limbs && s_reg_mem && s_reg_mem->free_fn) {
        s_reg_mem->free_fn(val->ext_limbs, s_reg_mem->ctx);
    }
    val->ext_limbs = NULL;
    val->ext_limb_count = 0u;
}

int arch_reg_register_dynamic(const arch_reg_desc_t *desc) {
    if (!desc || !desc->name || !s_reg_mem) return -1;
    for (ipo_size_t i = 0u; i < s_dyn_reg_count; i++) {
        if (str_eq(s_dyn_regs[i].name, desc->name)) {
            s_dyn_regs[i] = *desc;
            return 0;
        }
    }
    if (s_dyn_reg_count >= s_dyn_reg_cap) {
        ipo_size_t old_sz = s_dyn_reg_cap * sizeof(arch_reg_desc_t);
        ipo_size_t ncap = s_dyn_reg_cap ? (s_dyn_reg_cap * 2u) : 8u;
        arch_reg_desc_t *na = (arch_reg_desc_t *)regs_realloc(
            s_dyn_regs, old_sz, ncap * sizeof(arch_reg_desc_t));
        if (!na) return -1;
        s_dyn_regs = na;
        s_dyn_reg_cap = ncap;
    }
    arch_reg_desc_t entry = *desc;
    if (entry.reg_id == 0u) {
        entry.reg_id = s_next_dyn_reg_id++;
    }
    s_dyn_regs[s_dyn_reg_count++] = entry;
    return 0;
}

ipo_size_t arch_reg_count(void) {
    ipo_size_t base = (s_reg_backend && s_reg_backend->reg_count)
                          ? s_reg_backend->reg_count() : 0u;
    return base + s_dyn_reg_count;
}

const arch_reg_desc_t *arch_reg_get_desc(ipo_size_t index) {
    ipo_size_t base = (s_reg_backend && s_reg_backend->reg_count)
                          ? s_reg_backend->reg_count() : 0u;
    if (index < base) {
        return (s_reg_backend && s_reg_backend->reg_get)
                   ? s_reg_backend->reg_get(index) : NULL;
    }
    ipo_size_t dyn_idx = index - base;
    if (dyn_idx < s_dyn_reg_count) return &s_dyn_regs[dyn_idx];
    return NULL;
}

const arch_reg_desc_t *arch_reg_find_by_name(const char *name) {
    if (!name) return NULL;
    ipo_size_t total = arch_reg_count();
    for (ipo_size_t i = 0u; i < total; i++) {
        const arch_reg_desc_t *d = arch_reg_get_desc(i);
        if (d && str_eq(d->name, name)) return d;
    }
    return NULL;
}

const arch_reg_desc_t *arch_reg_find_by_role(arch_reg_role_t role) {
    ipo_size_t total = arch_reg_count();
    for (ipo_size_t i = 0u; i < total; i++) {
        const arch_reg_desc_t *d = arch_reg_get_desc(i);
        if (d && d->role == role && arch_reg_is_available(d)) return d;
    }
    return NULL;
}

bool arch_reg_is_available(const arch_reg_desc_t *desc) {
    if (!desc) return false;
    if (s_reg_backend && s_reg_backend->reg_is_available) {
        return s_reg_backend->reg_is_available(desc);
    }
    return false;
}

int arch_reg_read(const arch_reg_desc_t *desc, arch_reg_value_t *out_val) {
    if (!desc || !out_val) return -1;
    if ((desc->access_flags & ARCH_REG_ACCESS_READABLE) == 0u) return -2;
    if (!arch_reg_is_available(desc)) return -3;
    if (s_reg_backend && s_reg_backend->reg_read) {
        return s_reg_backend->reg_read(desc, out_val);
    }
    return -1;
}

int arch_reg_write(const arch_reg_desc_t *desc, const arch_reg_value_t *val) {
    if (!desc || !val) return -1;
    if ((desc->access_flags & ARCH_REG_ACCESS_WRITABLE) == 0u) return -2;
    if (!arch_reg_is_available(desc)) return -3;
    if (s_reg_backend && s_reg_backend->reg_write) {
        return s_reg_backend->reg_write(desc, val);
    }
    return -1;
}

int arch_reg_read_u64(arch_reg_role_t role, uint64_t *out_val) {
    if (!out_val) return -1;
    if (s_reg_backend && s_reg_backend->reg_read_by_role) {
        return s_reg_backend->reg_read_by_role(role, out_val);
    }
    const arch_reg_desc_t *desc = arch_reg_find_by_role(role);
    if (!desc) return -3;
    arch_reg_value_t v;
    arch_reg_value_init(&v);
    int rc = arch_reg_read(desc, &v);
    if (rc == 0) *out_val = arch_reg_value_as_u64(&v);
    arch_reg_value_free(&v);
    return rc;
}

int arch_reg_write_u64(arch_reg_role_t role, uint64_t val) {
    if (s_reg_backend && s_reg_backend->reg_write_by_role) {
        return s_reg_backend->reg_write_by_role(role, val);
    }
    const arch_reg_desc_t *desc = arch_reg_find_by_role(role);
    if (!desc) return -3;
    arch_reg_value_t v;
    arch_reg_value_from_u64(&v, desc->bit_width, val);
    return arch_reg_write(desc, &v);
}
