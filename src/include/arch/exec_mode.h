#ifndef IPO_ARCH_EXEC_MODE_H
#define IPO_ARCH_EXEC_MODE_H

#include <types.h>

/* Execution mode descriptors, capability bitsets, predicates, and handover format */

#define IPO_ARCH_FAMILY_UNKNOWN 0u

typedef struct {
    void *(*alloc)(ipo_size_t size, void *ctx);
    void *(*realloc_fn)(void *ptr, ipo_size_t old_size, ipo_size_t new_size, void *ctx);
    void  (*free_fn)(void *ptr, void *ctx);
    void  *ctx;
} ipo_mem_ops_t;

typedef struct {
    uint64_t  *words;
    ipo_size_t word_count;
    ipo_size_t word_cap;
} ipo_capset_t;

typedef enum {
    IPO_MODE_ELIGIBLE = 0,
    IPO_MODE_ERR_INVALID_DESC,
    IPO_MODE_ERR_NO_HARDWARE,
    IPO_MODE_ERR_MISSING_DEPS,
    IPO_MODE_ERR_NO_TRANSITION,
    IPO_MODE_ERR_NO_BACKEND,
    IPO_MODE_ERR_NO_PAYLOAD,
    IPO_MODE_ERR_SWITCH_FAILED,
    IPO_MODE_ERR_VERIFY_FAILED
} ipo_mode_status_t;

typedef enum {
    IPO_PRED_CAP = 0,       /* Interned cap in facts->hw_caps */
    IPO_PRED_LEAF_MASK,     /* (leaf(space,leaf,sub).regs[reg] & mask) == expected */
    IPO_PRED_MIN_VADDR_BITS,
    IPO_PRED_MIN_PADDR_BITS,
    IPO_PRED_MIN_RAM_BYTES
} ipo_pred_op_t;

typedef struct {
    uint32_t op;
    uint32_t reg_idx;       /* 0..3 inside hardware leaf */
    uint32_t space;         /* 0 = primary ISA leaf space */
    uint32_t leaf;
    uint32_t subleaf;
    uint64_t mask;
    uint64_t expected;
    char    *cap_name;      /* Used by IPO_PRED_CAP */
} ipo_hw_predicate_t;

typedef struct {
    uint32_t msr_addr;
    uint64_t set_mask;
    uint64_t clear_mask;
} arch_msr_op_t;

typedef struct {
    ipo_bitwidth_t target_exec_bits;  /* ISA decoder width (16, 32, 64, ...) */
    uint16_t       cs_selector;
    uint16_t       ds_selector;
    uint64_t       cs_desc_raw;       /* Raw 64-bit segment descriptor for cs_selector */
    uint64_t       cr0_set_mask;
    uint64_t       cr0_clear_mask;
    uint64_t       cr4_set_mask;
    uint64_t       cr4_clear_mask;
    uint64_t       verify_cr0_mask;
    uint64_t       verify_cr0_expected;
    uint64_t       verify_cr4_mask;
    uint64_t       verify_cr4_expected;
    uint64_t       verify_efer_mask;
    uint64_t       verify_efer_expected;
    uint32_t       paging_levels;     /* 0 = unpaged, >=2 = radix levels */
    uint8_t        pt_bits_per_level; /* 10 (32-bit) or 9 (radix-512) */
    uint8_t        pt_entry_bytes;    /* 4 or 8 */
    arch_msr_op_t *msr_ops;
    ipo_size_t     msr_op_count;
    ipo_size_t     msr_op_cap;
} arch_hw_switch_plan_t;

typedef struct {
    ipo_id_t       mode_id;
    uint32_t       arch_family;       /* Interned via cpu_arch_register_name() */
    char          *name;
    ipo_size_t     name_cap;

    ipo_bitwidth_t exec_bits;
    ipo_bitwidth_t gpr_bits;
    ipo_bitwidth_t vaddr_bits;
    ipo_bitwidth_t paddr_bits;
    ipo_bitwidth_t ptr_bits;

    uint32_t       paging_levels;     /* 0 = unpaged */
    uint32_t       hw_rank_hint;      /* Tie-breaker when widths match */

    ipo_capset_t   required_caps;
    ipo_capset_t   provided_caps;

    ipo_hw_predicate_t    *predicates;
    ipo_size_t             pred_count;
    ipo_size_t             pred_cap;
    arch_hw_switch_plan_t  switch_plan;

    bool           hardware_supported;
    bool           deps_satisfied;
    bool           transition_possible;
    bool           backend_available;
    bool           payload_available;
    ipo_mode_status_t status;
    char          *status_detail;
    ipo_size_t     status_detail_cap;

    const void    *backend_ptr;
    uint32_t       payload_index;
} cpu_exec_mode_desc_t;

typedef struct {
    cpu_exec_mode_desc_t *items;
    ipo_size_t            count;
    ipo_size_t            cap;
    ipo_mem_ops_t         mem;
    ipo_id_t              next_id;
} exec_mode_registry_t;

#define IPO_BOOT_HANDOVER_SIG_U64 0x32544F4F424F5049ULL /* "IPOBOOT2" */

typedef struct __attribute__((packed)) {
    uint32_t mode_id;
    uint32_t arch_family;
    uint32_t arch_name_off;
    uint32_t paging_levels;
    uint32_t status;
    uint32_t hw_rank_hint;
    uint32_t name_off;
    uint32_t detail_off;
    uint64_t exec_bits;
    uint64_t gpr_bits;
    uint64_t vaddr_bits;
    uint64_t paddr_bits;
    uint64_t ptr_bits;
    uint32_t req_cap_word_off;
    uint32_t req_cap_words;
    uint32_t prov_cap_word_off;
    uint32_t prov_cap_words;
    uint8_t  hardware_supported;
    uint8_t  deps_satisfied;
    uint8_t  transition_possible;
    uint8_t  backend_available;
    uint8_t  payload_available;
    uint8_t  pad0;
    uint16_t pad1;
} ipo_handover_mode_rec_t;

typedef struct __attribute__((packed)) {
    uint32_t space;
    uint32_t leaf;
    uint32_t subleaf;
    uint32_t reserved;
    uint64_t *regs;
    uint32_t reg_count;
} ipo_handover_leaf_rec_t;

typedef struct __attribute__((packed)) {
    uint64_t magic;
    uint32_t reserved0;
    uint32_t total_bytes;

    uint64_t initial_exec_bits;
    uint32_t initial_mode_id;
    uint32_t active_mode_id;
    uint32_t active_mode_index;
    uint32_t arch_family;

    char     *cpu_vendor;
    size_t   cpu_vendor_cap;
    char     *cpu_brand;
    size_t   cpu_brand_cap;
    uint64_t hw_probe_flags;
    uint64_t hw_paddr_bits;
    uint64_t hw_vaddr_bits;
    uint32_t hw_cap_word_off;
    uint32_t hw_cap_words;

    /* Post-switch register snapshot */
    uint64_t verified_cr0;
    uint64_t verified_cr3;
    uint64_t verified_cr4;
    uint64_t verified_efer;
    uint64_t verified_flags;
    uint64_t verified_sp;
    uint16_t verified_cs;
    uint16_t verified_ds;
    uint8_t  switch_verified;
    uint8_t  boot_drive;
    uint16_t active_paging_levels;

    uint64_t total_usable_ram;
    uint64_t e820_table_paddr;
    uint32_t e820_count;
    uint32_t active_arch_name_off;
    uint64_t boot_pt_root_paddr;
    uint64_t gdt_paddr;
    uint16_t gdt_limit;
    uint16_t pad0;
    uint32_t pad1;
    uint64_t payload_load_paddr;
    uint64_t payload_raw_size;

    /* Relative byte offsets inside this contiguous handover block */
    uint32_t modes_off;
    uint32_t mode_count;
    uint32_t cap_words_off;
    uint32_t cap_words_total;
    uint32_t leaves_off;
    uint32_t leaf_count;
    uint32_t strings_off;
    uint32_t strings_size;
} ipo_boot_handover_t;

uint32_t    cpu_arch_register_name(const char *arch_name, const ipo_mem_ops_t *mem);
const char *cpu_arch_get_name(uint32_t arch_id);

void ipo_capset_init(ipo_capset_t *cs);
void ipo_capset_free(ipo_capset_t *cs, const ipo_mem_ops_t *mem);
bool ipo_capset_set(ipo_capset_t *cs, uint32_t cap_id, const ipo_mem_ops_t *mem);
void ipo_capset_clear(ipo_capset_t *cs, uint32_t cap_id);
bool ipo_capset_has(const ipo_capset_t *cs, uint32_t cap_id);
bool ipo_capset_contains_all(const ipo_capset_t *superset, const ipo_capset_t *subset);
bool ipo_capset_copy(ipo_capset_t *dst, const ipo_capset_t *src, const ipo_mem_ops_t *mem);

void exec_mode_registry_init(exec_mode_registry_t *reg, const ipo_mem_ops_t *mem);
void exec_mode_registry_free(exec_mode_registry_t *reg);
cpu_exec_mode_desc_t *exec_mode_registry_add(exec_mode_registry_t *reg,
                                             uint32_t arch_family,
                                             const char *name,
                                             ipo_bitwidth_t exec_bits,
                                             ipo_bitwidth_t gpr_bits,
                                             ipo_bitwidth_t vaddr_bits,
                                             ipo_bitwidth_t paddr_bits,
                                             ipo_bitwidth_t ptr_bits,
                                             uint32_t paging_levels);
bool exec_mode_set_detail(cpu_exec_mode_desc_t *mode, const char *detail, const ipo_mem_ops_t *mem);
bool exec_mode_add_predicate(cpu_exec_mode_desc_t *mode,
                             const ipo_hw_predicate_t *pred,
                             const ipo_mem_ops_t *mem);
bool exec_mode_add_msr_op(cpu_exec_mode_desc_t *mode,
                          uint32_t msr_addr,
                          uint64_t set_mask,
                          uint64_t clear_mask,
                          const ipo_mem_ops_t *mem);
cpu_exec_mode_desc_t *exec_mode_registry_find(exec_mode_registry_t *reg, ipo_id_t mode_id);
cpu_exec_mode_desc_t *exec_mode_registry_find_matching(exec_mode_registry_t *reg,
                                                       uint32_t arch_family,
                                                       ipo_bitwidth_t exec_bits,
                                                       ipo_bitwidth_t gpr_bits,
                                                       ipo_bitwidth_t vaddr_bits,
                                                       uint32_t paging_levels);
const cpu_exec_mode_desc_t *exec_mode_registry_get(const exec_mode_registry_t *reg, ipo_size_t index);

#endif /* IPO_ARCH_EXEC_MODE_H */
