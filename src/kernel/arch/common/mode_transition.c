#include <arch/mode_transition.h>

int mode_transition_execute(const cpu_hw_facts_t *facts,
                            exec_mode_registry_t *reg,
                            const cpu_exec_mode_desc_t *current_mode,
                            const cpu_exec_mode_desc_t *target_mode,
                            ipo_boot_handover_t *handover,
                            ipo_paddr_t entry_paddr,
                            const ipo_mem_ops_t *mem) {
    if (!facts || !reg || !target_mode || !handover) return -1;

    const arch_transition_ops_t *backend = (const arch_transition_ops_t *)target_mode->backend_ptr;
    if (!backend) backend = arch_backend_find_for_mode(target_mode);
    if (!backend) return -2;

    if (backend->can_transition) {
        const char *reason = NULL;
        if (!backend->can_transition(facts, current_mode, target_mode, &reason)) {
            cpu_exec_mode_desc_t *mut = exec_mode_registry_find(reg, target_mode->mode_id);
            if (mut) {
                mut->status = IPO_MODE_ERR_NO_TRANSITION;
                if (reason) exec_mode_set_detail(mut, reason, mem);
            }
            return -3;
        }
    }

    if (backend->prepare) {
        int prc = backend->prepare(facts, target_mode, handover);
        if (prc < 0) {
            cpu_exec_mode_desc_t *mut = exec_mode_registry_find(reg, target_mode->mode_id);
            if (mut) {
                mut->status = IPO_MODE_ERR_SWITCH_FAILED;
                exec_mode_set_detail(mut, "Architecture backend preparation failed", mem);
            }
            return prc;
        }
    }

    handover->active_mode_id = target_mode->mode_id;
    handover->arch_family = target_mode->arch_family;
    handover->active_paging_levels = (uint16_t)target_mode->paging_levels;

    if (!backend->enter_and_verify) {
        return -4;
    }

    /* Mode switch does not return on success */
    backend->enter_and_verify(target_mode, handover, entry_paddr);
    return -5;
}
