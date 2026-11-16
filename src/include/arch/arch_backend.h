#ifndef IPO_ARCH_BACKEND_H
#define IPO_ARCH_BACKEND_H

#include <arch/cpu_discovery.h>

/* Architecture mode-transition backend interface. */

typedef struct arch_transition_ops {
    const char *name;
    const char *arch_name;      /* Architecture identifier string */

    bool (*matches_mode)(const cpu_exec_mode_desc_t *mode);
    bool (*can_transition)(const cpu_hw_facts_t *facts,
                           const cpu_exec_mode_desc_t *current_mode,
                           const cpu_exec_mode_desc_t *target_mode,
                           const char **out_reason);
    int  (*prepare)(const cpu_hw_facts_t *facts,
                    const cpu_exec_mode_desc_t *target_mode,
                    ipo_boot_handover_t *handover);
    int  (*enter_and_verify)(const cpu_exec_mode_desc_t *target_mode,
                             ipo_boot_handover_t *handover,
                             ipo_paddr_t entry_paddr);
    bool (*verify_active)(const cpu_exec_mode_desc_t *target_mode,
                          ipo_boot_handover_t *handover);
} arch_transition_ops_t;

void arch_backend_reset(const ipo_mem_ops_t *mem);
bool arch_backend_register(const arch_transition_ops_t *ops, const ipo_mem_ops_t *mem);
const arch_transition_ops_t *arch_backend_find_for_mode(const cpu_exec_mode_desc_t *mode);
ipo_size_t arch_backend_bind_registry(const cpu_hw_facts_t *facts,
                                      const cpu_exec_mode_desc_t *current_mode,
                                      exec_mode_registry_t *reg,
                                      const ipo_mem_ops_t *mem);

bool backend_x86_register_all(const ipo_mem_ops_t *mem);
bool backend_register_platform_all(const ipo_mem_ops_t *mem);

#endif /* IPO_ARCH_BACKEND_H */
