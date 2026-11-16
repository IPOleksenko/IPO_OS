#ifndef IPO_ARCH_MODE_TRANSITION_H
#define IPO_ARCH_MODE_TRANSITION_H

#include <arch/arch_backend.h>
#include <arch/mode_selector.h>

/* Mode transition orchestration with failure rollback */

int mode_transition_execute(const cpu_hw_facts_t *facts,
                            exec_mode_registry_t *reg,
                            const cpu_exec_mode_desc_t *current_mode,
                            const cpu_exec_mode_desc_t *target_mode,
                            ipo_boot_handover_t *handover,
                            ipo_paddr_t entry_paddr,
                            const ipo_mem_ops_t *mem);

#endif /* IPO_ARCH_MODE_TRANSITION_H */
