#ifndef IPO_ARCH_CPU_DISCOVERY_H
#define IPO_ARCH_CPU_DISCOVERY_H

#include <arch/exec_mode.h>

/* CPU capability and feature-leaf discovery */

typedef struct {
    uint32_t   space;
    uint32_t   leaf;
    uint32_t   subleaf;
    uint64_t  *regs;
    ipo_size_t reg_count;
    ipo_size_t reg_cap;
} cpu_hw_leaf_t;

typedef struct {
    char          *vendor;
    ipo_size_t     vendor_cap;
    char          *brand;
    ipo_size_t     brand_cap;
    uint32_t       arch_family;
    ipo_bitwidth_t phys_addr_bits;
    ipo_bitwidth_t virt_addr_bits;
    uint64_t       total_ram_bytes;
    uint64_t       hw_probe_flags;
    ipo_capset_t   hw_caps;

    cpu_hw_leaf_t *hw_leaves;
    ipo_size_t     hw_leaf_count;
    ipo_size_t     hw_leaf_cap;
} cpu_hw_facts_t;

typedef int (*cpu_capability_discover_fn_t)(const cpu_hw_facts_t *facts,
                                            exec_mode_registry_t *reg,
                                            const ipo_mem_ops_t *mem,
                                            void *ctx);

typedef struct {
    const char                   *name;
    cpu_capability_discover_fn_t  discover;
    void                         *ctx;
} cpu_capability_provider_t;

void cpu_facts_init(cpu_hw_facts_t *facts);
void cpu_facts_free(cpu_hw_facts_t *facts, const ipo_mem_ops_t *mem);
bool cpu_facts_set_vendor(cpu_hw_facts_t *facts, const char *vendor, const ipo_mem_ops_t *mem);
bool cpu_facts_set_brand(cpu_hw_facts_t *facts, const char *brand, const ipo_mem_ops_t *mem);
bool cpu_facts_add_leaf_regs(cpu_hw_facts_t *facts,
                             uint32_t space,
                             uint32_t leaf,
                             uint32_t subleaf,
                             const uint64_t *regs,
                             ipo_size_t reg_count,
                             const ipo_mem_ops_t *mem);

bool cpu_facts_add_leaf(cpu_hw_facts_t *facts,
                        uint32_t space,
                        uint32_t leaf,
                        uint32_t subleaf,
                        uint64_t r0, uint64_t r1, uint64_t r2, uint64_t r3,
                        const ipo_mem_ops_t *mem);
const cpu_hw_leaf_t *cpu_facts_find_leaf(const cpu_hw_facts_t *facts,
                                         uint32_t space,
                                         uint32_t leaf,
                                         uint32_t subleaf);

bool cpu_eval_mode_predicates(const cpu_hw_facts_t *facts,
                              const cpu_exec_mode_desc_t *mode,
                              const ipo_mem_ops_t *mem,
                              const char **out_reason);

/* Interned capability names */
uint32_t    cpu_cap_register_name(const char *cap_name, const ipo_mem_ops_t *mem);
uint32_t    cpu_cap_find_name(const char *cap_name);
const char *cpu_cap_get_name(uint32_t cap_id);

void cpu_discovery_reset_providers(const ipo_mem_ops_t *mem);
bool cpu_discovery_register_provider(const char *name,
                                     cpu_capability_discover_fn_t fn,
                                     void *ctx,
                                     const ipo_mem_ops_t *mem);

/* Implemented by the active architecture backend */
void cpu_discovery_probe_platform(cpu_hw_facts_t *facts,
                                  uint64_t total_ram_bytes,
                                  bool a20_enabled,
                                  const ipo_mem_ops_t *mem);
bool cpu_discovery_register_platform_providers(const ipo_mem_ops_t *mem);
int  cpu_discovery_run(cpu_hw_facts_t *facts,
                       exec_mode_registry_t *reg,
                       const ipo_mem_ops_t *mem);

#endif /* IPO_ARCH_CPU_DISCOVERY_H */
