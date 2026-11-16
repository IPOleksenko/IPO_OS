#ifndef IPO_TYPES_H
#define IPO_TYPES_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/* Bitness-independent core types (fixed-width uintN_t reserved for wire/hardware formats). */

typedef uintptr_t       ipo_vaddr_t;
typedef intptr_t        ipo_svaddr_t;
typedef uint64_t        ipo_paddr_t;
typedef size_t          ipo_size_t;
typedef ptrdiff_t       ipo_ssize_t;
typedef int64_t         ipo_off_t;
typedef uint64_t        ipo_uoff_t;
typedef uintptr_t       ipo_reg_t;
typedef uintptr_t       ipo_uarg_t;
typedef intptr_t        ipo_sarg_t;
typedef uint64_t        ipo_bitwidth_t;
typedef uint_fast32_t   ipo_count_t;
typedef uint32_t        ipo_id_t;

#define IPO_VADDR_MAX   ((ipo_vaddr_t)(~(ipo_vaddr_t)0))
#define IPO_PADDR_MAX   ((ipo_paddr_t)(~(ipo_paddr_t)0))
#define IPO_SIZE_MAX    ((ipo_size_t)(~(ipo_size_t)0))
#define IPO_COUNT_MAX   ((ipo_count_t)(~(ipo_count_t)0))

/* Multi-word address for >64-bit targets */
typedef struct {
    uint64_t      *limbs;      /* little-endian 64-bit limbs */
    size_t         limb_count;
    size_t         limb_cap;
    ipo_bitwidth_t bit_width;
} ipo_ext_addr_t;

#if defined(__STDC_VERSION__) && (__STDC_VERSION__ >= 201112L)
#define IPO_STATIC_ASSERT(cond, msg) _Static_assert(cond, msg)
#else
#define IPO_STATIC_ASSERT(cond, msg) typedef char ipo_static_assert_##__LINE__[(cond) ? 1 : -1]
#endif

IPO_STATIC_ASSERT(sizeof(ipo_vaddr_t) == sizeof(void *), "ipo_vaddr_t must match pointer size");
IPO_STATIC_ASSERT(sizeof(ipo_uarg_t) >= sizeof(void *), "ipo_uarg_t must hold full pointer");
IPO_STATIC_ASSERT(sizeof(ipo_reg_t) >= sizeof(void *), "ipo_reg_t must hold full pointer");
IPO_STATIC_ASSERT(sizeof(ipo_paddr_t) >= 8u, "ipo_paddr_t must support >=64-bit physical addresses");

/* Overflow-checked address and size helpers */
static inline bool ipo_vaddr_add_ok(ipo_vaddr_t base, ipo_size_t len, ipo_vaddr_t *out_end) {
    if (base > (ipo_vaddr_t)(~(ipo_vaddr_t)0) - (ipo_vaddr_t)len) {
        return false;
    }
    if (out_end) {
        *out_end = base + (ipo_vaddr_t)len;
    }
    return true;
}

static inline bool ipo_paddr_add_ok(ipo_paddr_t base, ipo_uoff_t len, ipo_paddr_t *out_end) {
    if (base > (ipo_paddr_t)(~(ipo_paddr_t)0) - (ipo_paddr_t)len) {
        return false;
    }
    if (out_end) {
        *out_end = base + (ipo_paddr_t)len;
    }
    return true;
}

static inline bool ipo_size_mul_ok(ipo_size_t a, ipo_size_t b, ipo_size_t *out_prod) {
    if (a != 0u && b > ((ipo_size_t)(~(ipo_size_t)0)) / a) {
        return false;
    }
    if (out_prod) {
        *out_prod = a * b;
    }
    return true;
}

static inline ipo_vaddr_t ipo_vaddr_align_down(ipo_vaddr_t addr, ipo_size_t align) {
    ipo_vaddr_t mask = (ipo_vaddr_t)align - 1u;
    return addr & ~mask;
}

static inline bool ipo_vaddr_align_up_ok(ipo_vaddr_t addr, ipo_size_t align, ipo_vaddr_t *out_addr) {
    ipo_vaddr_t mask = (ipo_vaddr_t)align - 1u;
    ipo_vaddr_t summed = 0u;
    if (!ipo_vaddr_add_ok(addr, (ipo_size_t)mask, &summed)) {
        return false;
    }
    if (out_addr) {
        *out_addr = summed & ~mask;
    }
    return true;
}

static inline ipo_paddr_t ipo_paddr_align_down(ipo_paddr_t addr, ipo_paddr_t align) {
    ipo_paddr_t mask = align - 1u;
    return addr & ~mask;
}

static inline bool ipo_paddr_align_up_ok(ipo_paddr_t addr, ipo_paddr_t align, ipo_paddr_t *out_addr) {
    ipo_paddr_t mask = align - 1u;
    ipo_paddr_t summed = 0u;
    if (!ipo_paddr_add_ok(addr, mask, &summed)) {
        return false;
    }
    if (out_addr) {
        *out_addr = summed & ~mask;
    }
    return true;
}

#endif /* IPO_TYPES_H */
