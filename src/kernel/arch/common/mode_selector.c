#include <arch/mode_selector.h>
#include <arch/arch_backend.h>

static bool mode_descriptor_valid(const cpu_exec_mode_desc_t *m) {
    if (!m) return false;
    if (m->exec_bits == 0u || m->gpr_bits == 0u || m->vaddr_bits == 0u ||
        m->paddr_bits == 0u || m->ptr_bits == 0u) {
        return false;
    }
    if (m->arch_family == IPO_ARCH_FAMILY_UNKNOWN) return false;
    return true;
}

bool exec_mode_is_better(const cpu_exec_mode_desc_t *cand,
                         const cpu_exec_mode_desc_t *best) {
    if (!cand) return false;
    if (!best) return true;
    if (cand->exec_bits != best->exec_bits) return cand->exec_bits > best->exec_bits;
    if (cand->vaddr_bits != best->vaddr_bits) return cand->vaddr_bits > best->vaddr_bits;
    if (cand->paddr_bits != best->paddr_bits) return cand->paddr_bits > best->paddr_bits;
    if (cand->gpr_bits != best->gpr_bits) return cand->gpr_bits > best->gpr_bits;
    if (cand->paging_levels != best->paging_levels) return cand->paging_levels > best->paging_levels;
    return cand->hw_rank_hint > best->hw_rank_hint;
}

const cpu_exec_mode_desc_t *exec_mode_select_best(
    const cpu_hw_facts_t *facts,
    exec_mode_registry_t *reg,
    const cpu_exec_mode_desc_t *current_mode,
    exec_mode_match_payload_fn_t payload_matcher,
    void *payload_ctx,
    exec_mode_selection_t *out_result) {
    if (!facts || !reg) return NULL;

    exec_mode_selection_t res;
    res.selected = NULL;
    res.selected_index = (ipo_size_t)-1;
    res.evaluated_count = reg->count;
    res.eligible_count = 0u;

    for (ipo_size_t i = 0u; i < reg->count; i++) {
        cpu_exec_mode_desc_t *m = &reg->items[i];

        if (!mode_descriptor_valid(m)) {
            m->status = IPO_MODE_ERR_INVALID_DESC;
            exec_mode_set_detail(m, "Invalid mode descriptor parameters", &reg->mem);
            continue;
        }

        const char *pred_reason = NULL;
        m->hardware_supported = cpu_eval_mode_predicates(facts, m, &reg->mem, &pred_reason);
        if (!m->hardware_supported) {
            m->status = IPO_MODE_ERR_NO_HARDWARE;
            exec_mode_set_detail(m, pred_reason ? pred_reason : "Execution mode not supported by CPU hardware", &reg->mem);
            continue;
        }

        m->deps_satisfied = ipo_capset_contains_all(&facts->hw_caps, &m->required_caps);
        if (!m->deps_satisfied) {
            m->status = IPO_MODE_ERR_MISSING_DEPS;
            exec_mode_set_detail(m, "Missing required hardware/environment dependencies", &reg->mem);
            continue;
        }

        const arch_transition_ops_t *backend = (const arch_transition_ops_t *)m->backend_ptr;
        if (!backend) {
            backend = arch_backend_find_for_mode(m);
            m->backend_ptr = backend;
        }
        m->backend_available = (backend != NULL);
        if (!backend) {
            m->status = IPO_MODE_ERR_NO_BACKEND;
            exec_mode_set_detail(m, "No compatible architecture transition backend registered", &reg->mem);
            continue;
        }

        const char *tx_reason = NULL;
        if (backend->can_transition) {
            m->transition_possible = backend->can_transition(facts, current_mode, m, &tx_reason);
        } else {
            m->transition_possible = true;
        }
        if (!m->transition_possible) {
            m->status = IPO_MODE_ERR_NO_TRANSITION;
            exec_mode_set_detail(m, tx_reason ? tx_reason : "Transition impossible from current mode", &reg->mem);
            continue;
        }

        if (payload_matcher) {
            uint32_t pidx = 0u;
            m->payload_available = payload_matcher(m, &pidx, payload_ctx);
            m->payload_index = pidx;
            if (!m->payload_available) {
                m->status = IPO_MODE_ERR_NO_PAYLOAD;
                exec_mode_set_detail(m, "No compatible kernel executable payload for this mode", &reg->mem);
                continue;
            }
        } else {
            m->payload_available = true;
        }

        m->status = IPO_MODE_ELIGIBLE;
        exec_mode_set_detail(m, "Eligible", &reg->mem);
        res.eligible_count++;

        if (exec_mode_is_better(m, res.selected)) {
            res.selected = m;
            res.selected_index = i;
        }
    }

    if (out_result) *out_result = res;
    return res.selected;
}
