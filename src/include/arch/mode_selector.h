#ifndef IPO_ARCH_MODE_SELECTOR_H
#define IPO_ARCH_MODE_SELECTOR_H

#include <arch/cpu_discovery.h>

/* Execution mode selection by hardware support and capability ranking */

typedef bool (*exec_mode_match_payload_fn_t)(const cpu_exec_mode_desc_t *mode,
                                             uint32_t *out_payload_index,
                                             void *ctx);

typedef struct {
    const cpu_exec_mode_desc_t *selected;
    ipo_size_t                  selected_index;
    ipo_size_t                  evaluated_count;
    ipo_size_t                  eligible_count;
} exec_mode_selection_t;

const cpu_exec_mode_desc_t *exec_mode_select_best(
    const cpu_hw_facts_t *facts,
    exec_mode_registry_t *reg,
    const cpu_exec_mode_desc_t *current_mode,
    exec_mode_match_payload_fn_t payload_matcher,
    void *payload_ctx,
    exec_mode_selection_t *out_result);

bool exec_mode_is_better(const cpu_exec_mode_desc_t *cand,
                         const cpu_exec_mode_desc_t *best);

#endif /* IPO_ARCH_MODE_SELECTOR_H */
