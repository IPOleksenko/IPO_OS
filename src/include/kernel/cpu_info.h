#ifndef IPO_KERNEL_CPU_INFO_H
#define IPO_KERNEL_CPU_INFO_H

#include <arch/exec_mode.h>
#include <arch/cpu_discovery.h>

/* Runtime CPU mode and capability reporting */

typedef struct {
    ipo_bitwidth_t exec_bits;
    ipo_bitwidth_t gpr_bits;
    ipo_bitwidth_t vaddr_bits;
    ipo_bitwidth_t paddr_bits;
    ipo_bitwidth_t ptr_bits;
    uint32_t       arch_family;
    uint32_t       paging_levels;
    uint32_t       active_mode_id;
    uint32_t       initial_mode_id;
    ipo_bitwidth_t initial_exec_bits;
    uint64_t       verified_cr0;
    uint64_t       verified_cr3;
    uint64_t       verified_cr4;
    uint64_t       verified_efer;
    uint16_t       verified_cs;
    bool           switch_verified;
    char          *mode_name;
    ipo_size_t     mode_name_cap;
    char          *vendor;
    ipo_size_t     vendor_cap;
    char          *brand;
    ipo_size_t     brand_cap;
} ipo_cpu_runtime_summary_t;

void cpu_info_init(const ipo_boot_handover_t *handover);
void cpu_info_print_boot_summary(void);
const ipo_cpu_runtime_summary_t *cpu_info_get_summary(void);
const exec_mode_registry_t *cpu_info_get_mode_registry(void);
const cpu_exec_mode_desc_t *cpu_info_get_active_mode(void);
const cpu_hw_facts_t *cpu_info_get_hw_facts(void);
const ipo_boot_handover_t *cpu_info_get_handover(void);
bool cpu_info_has_cap(uint32_t cap_id);
void cpu_info_print_report(void);

/* Implemented by active architecture backend */
void arch_cpu_fill_runtime_summary(ipo_cpu_runtime_summary_t *summary,
                                   const cpu_hw_facts_t *facts);

#endif /* IPO_KERNEL_CPU_INFO_H */
