#include <kernel/irq.h>
#include <kernel/process.h>
#include <kernel/smp.h>
#include <memory/kmalloc.h>
#include <system/timer.h>
#include <ioport.h>
#include <string.h>

#define PIC1_CMD  0x20u
#define PIC1_DATA 0x21u
#define PIC2_CMD  0xA0u
#define PIC2_DATA 0xA1u

typedef struct {
    bool active;
    uint32_t handle;
    uint8_t irq;
    uint32_t owner_pid;
    irq_handler_fn_t fn;
    void *ctx;
} irq_handler_slot_t;

typedef struct {
    bool active;
    uint32_t handle;
    uint8_t irq;
    uint32_t owner_pid;
    uint32_t pending;
    uint64_t last_ms;
} irq_sub_slot_t;

static volatile uint32_t *s_irq_counts = NULL;
static uint32_t *s_irq_enable_refs = NULL;
static size_t s_irq_line_cap = 0u;
static uint32_t s_cascade_refs = 0;

static irq_handler_slot_t *s_handlers = NULL;
static uint32_t s_handler_cap = 0;
static uint32_t s_next_handle = 1u;

static irq_sub_slot_t *s_subs = NULL;
static uint32_t s_sub_cap = 0;

static spinlock_t s_irq_lock;

static bool irq_ensure_line_cap_locked(uint8_t irq) {
    size_t need = (size_t)irq + 1u;
    if (need <= s_irq_line_cap && s_irq_counts && s_irq_enable_refs) {
        return true;
    }
    size_t old_cap = s_irq_line_cap;
    size_t new_cap = (old_cap > 0u) ? old_cap : 16u;
    while (new_cap < need) {
        new_cap *= 2u;
    }
    uint32_t *nc = (uint32_t *)krealloc((void *)s_irq_counts, new_cap * sizeof(uint32_t));
    if (!nc) return false;
    s_irq_counts = (volatile uint32_t *)nc;
    uint32_t *nr = (uint32_t *)krealloc(s_irq_enable_refs, new_cap * sizeof(uint32_t));
    if (!nr) return false;
    s_irq_enable_refs = nr;
    for (size_t i = old_cap; i < new_cap; i++) {
        s_irq_counts[i] = 0u;
        s_irq_enable_refs[i] = 0u;
    }
    s_irq_line_cap = new_cap;
    return true;
}

static void pic_set_mask_bit(uint8_t irq, bool masked) {
    if (irq > 15u) return;
    uint16_t port = (irq < 8u) ? PIC1_DATA : PIC2_DATA;
    uint8_t bit = (uint8_t)(1u << (irq & 7u));
    uint8_t val = inb(port);
    if (masked) {
        val |= bit;
    } else {
        val &= (uint8_t)(~bit);
    }
    outb(port, val);
    io_wait();
}

static bool irq_enable_locked(uint8_t irq) {
    if (irq < IRQ_MIN_LINE || irq > IRQ_MAX_LINE) return false;
    if (!irq_ensure_line_cap_locked(irq)) return false;
    if (s_irq_enable_refs[irq] == 0u) {
        if (irq >= 8u) {
            if (s_cascade_refs == 0u) {
                pic_set_mask_bit(2u, false);
            }
            s_cascade_refs++;
        }
        pic_set_mask_bit(irq, false);
    }
    s_irq_enable_refs[irq]++;
    return true;
}

static bool irq_disable_locked(uint8_t irq) {
    if (irq < IRQ_MIN_LINE || irq > IRQ_MAX_LINE) return false;
    if (!s_irq_enable_refs || (size_t)irq >= s_irq_line_cap) return false;
    if (s_irq_enable_refs[irq] == 0u) return false;
    s_irq_enable_refs[irq]--;
    if (s_irq_enable_refs[irq] == 0u) {
        if (irq != 2u || s_cascade_refs == 0u) {
            pic_set_mask_bit(irq, true);
        }
        if (irq >= 8u && s_cascade_refs > 0u) {
            s_cascade_refs--;
            if (s_cascade_refs == 0u && s_irq_enable_refs[2u] == 0u) {
                pic_set_mask_bit(2u, true);
            }
        }
    }
    return true;
}

void irq_init(void) {
    spinlock_init(&s_irq_lock);
    (void)irq_ensure_line_cap_locked(15u);
    if (s_irq_counts && s_irq_line_cap > 0u) {
        memset((void *)s_irq_counts, 0, s_irq_line_cap * sizeof(uint32_t));
    }
    if (s_irq_enable_refs && s_irq_line_cap > 0u) {
        memset(s_irq_enable_refs, 0, s_irq_line_cap * sizeof(uint32_t));
    }
    s_cascade_refs = 0;
}

bool irq_enable(uint8_t irq) {
    ipo_reg_t flags = spin_lock_irqsave(&s_irq_lock);
    bool ok = irq_enable_locked(irq);
    spin_unlock_irqrestore(&s_irq_lock, flags);
    return ok;
}

bool irq_disable(uint8_t irq) {
    ipo_reg_t flags = spin_lock_irqsave(&s_irq_lock);
    bool ok = irq_disable_locked(irq);
    spin_unlock_irqrestore(&s_irq_lock, flags);
    return ok;
}

static uint32_t irq_next_handle_locked(void) {
    uint32_t handle = s_next_handle++;
    if (s_next_handle == 0u) s_next_handle = 1u;
    return handle;
}

/* Slot arrays must start with `bool active` */
static bool irq_ensure_slot(void **arr_ptr, uint32_t *cap_ptr, size_t elem_size, uint32_t *out_slot) {
    uint8_t *base = (uint8_t *)(*arr_ptr);
    uint32_t cap = *cap_ptr;
    for (uint32_t i = 0; i < cap; i++) {
        if (!*(bool *)(base + i * elem_size)) {
            *out_slot = i;
            return true;
        }
    }
    uint32_t new_cap = cap ? (cap * 2u) : 8u;
    void *nb = krealloc(*arr_ptr, new_cap * elem_size);
    if (!nb) return false;
    memset((uint8_t *)nb + cap * elem_size, 0, (new_cap - cap) * elem_size);
    *arr_ptr = nb;
    *cap_ptr = new_cap;
    *out_slot = cap;
    return true;
}

void irq_common_dispatch(uint32_t irq) {
    if (irq == 7u) {
        outb(PIC1_CMD, 0x0Bu);
        io_wait();
        if ((inb(PIC1_CMD) & 0x80u) == 0u) {
            return;
        }
    } else if (irq == 15u) {
        outb(PIC2_CMD, 0x0Bu);
        io_wait();
        if ((inb(PIC2_CMD) & 0x80u) == 0u) {
            outb(PIC1_CMD, 0x20u);
            return;
        }
    }

    if (irq >= 8u) {
        outb(PIC2_CMD, 0x20u);
    }
    outb(PIC1_CMD, 0x20u);

    if (irq >= 16u) return;

    ipo_reg_t flags = spin_lock_irqsave(&s_irq_lock);
    if (s_irq_counts && (size_t)irq < s_irq_line_cap) {
        s_irq_counts[irq]++;
    }
    uint64_t now_ms = timer_millis();

    for (uint32_t i = 0; i < s_sub_cap; i++) {
        if (s_subs[i].active && s_subs[i].irq == (uint8_t)irq) {
            s_subs[i].pending++;
            s_subs[i].last_ms = now_ms;
        }
    }

    process_shared_ctx_t *sctx = process_get_shared_context();
    uint32_t active_pid = 0;
    if (sctx && sctx->magic == PROCESS_SHARED_MAGIC && sctx->current_process) {
        active_pid = sctx->current_process->pid;
    }

    /* Walk handlers in ascending handle order without holding s_irq_lock during callbacks */
    uint32_t min_handle = 0u;
    for (;;) {
        uint32_t best_handle = UINT32_MAX;
        irq_handler_fn_t fn = NULL;
        void *ctx = NULL;
        bool found = false;

        for (uint32_t i = 0; i < s_handler_cap; i++) {
            if (!s_handlers[i].active || s_handlers[i].irq != (uint8_t)irq || !s_handlers[i].fn) {
                continue;
            }
            if (s_handlers[i].owner_pid != 0u && s_handlers[i].owner_pid != active_pid) {
                continue;
            }
            if (s_handlers[i].handle > min_handle && s_handlers[i].handle <= best_handle) {
                best_handle = s_handlers[i].handle;
                fn = s_handlers[i].fn;
                ctx = s_handlers[i].ctx;
                found = true;
            }
        }
        if (!found) break;
        min_handle = best_handle;
        spin_unlock(&s_irq_lock);
        fn((uint8_t)irq, ctx);
        spin_lock(&s_irq_lock);
        if (best_handle == UINT32_MAX) break;
    }
    spin_unlock_irqrestore(&s_irq_lock, flags);
}

uint32_t irq_register_handler(uint8_t irq, irq_handler_fn_t fn, void *ctx, uint32_t owner_pid) {
    if (irq < IRQ_MIN_LINE || irq > IRQ_MAX_LINE || !fn) return 0u;

    ipo_reg_t flags = spin_lock_irqsave(&s_irq_lock);

    uint32_t slot = 0;
    if (!irq_ensure_slot((void **)&s_handlers, &s_handler_cap, sizeof(irq_handler_slot_t), &slot)) {
        spin_unlock_irqrestore(&s_irq_lock, flags);
        return 0u;
    }

    uint32_t handle = irq_next_handle_locked();

    s_handlers[slot].active = true;
    s_handlers[slot].handle = handle;
    s_handlers[slot].irq = irq;
    s_handlers[slot].owner_pid = owner_pid;
    s_handlers[slot].fn = fn;
    s_handlers[slot].ctx = ctx;

    irq_enable_locked(irq);

    spin_unlock_irqrestore(&s_irq_lock, flags);
    return handle;
}

int irq_unregister_handler(uint32_t handle, uint32_t caller_pid) {
    if (handle == 0u) return -1;
    ipo_reg_t flags = spin_lock_irqsave(&s_irq_lock);

    int rc = -1;
    for (uint32_t i = 0; i < s_handler_cap; i++) {
        if (s_handlers[i].active && s_handlers[i].handle == handle) {
            if (caller_pid != 0u && s_handlers[i].owner_pid != caller_pid) {
                rc = -2;
                break;
            }
            uint8_t irq = s_handlers[i].irq;
            s_handlers[i].active = false;
            s_handlers[i].fn = NULL;
            irq_disable_locked(irq);
            rc = 0;
            break;
        }
    }

    spin_unlock_irqrestore(&s_irq_lock, flags);
    return rc;
}

uint32_t irq_subscribe(uint8_t irq, uint32_t owner_pid) {
    if (irq < IRQ_MIN_LINE || irq > IRQ_MAX_LINE) return 0u;

    ipo_reg_t flags = spin_lock_irqsave(&s_irq_lock);

    uint32_t slot = 0;
    if (!irq_ensure_slot((void **)&s_subs, &s_sub_cap, sizeof(irq_sub_slot_t), &slot)) {
        spin_unlock_irqrestore(&s_irq_lock, flags);
        return 0u;
    }

    uint32_t handle = irq_next_handle_locked();

    s_subs[slot].active = true;
    s_subs[slot].handle = handle;
    s_subs[slot].irq = irq;
    s_subs[slot].owner_pid = owner_pid;
    s_subs[slot].pending = 0u;
    s_subs[slot].last_ms = 0u;

    irq_enable_locked(irq);

    spin_unlock_irqrestore(&s_irq_lock, flags);
    return handle;
}

int irq_unsubscribe(uint32_t handle, uint32_t caller_pid) {
    if (handle == 0u) return -1;
    ipo_reg_t flags = spin_lock_irqsave(&s_irq_lock);

    int rc = -1;
    for (uint32_t i = 0; i < s_sub_cap; i++) {
        if (s_subs[i].active && s_subs[i].handle == handle) {
            if (caller_pid != 0u && s_subs[i].owner_pid != caller_pid) {
                rc = -2;
                break;
            }
            uint8_t irq = s_subs[i].irq;
            s_subs[i].active = false;
            irq_disable_locked(irq);
            rc = 0;
            break;
        }
    }

    spin_unlock_irqrestore(&s_irq_lock, flags);
    return rc;
}

uint32_t irq_poll(uint32_t handle, uint32_t caller_pid, uint32_t *out_total, uint64_t *out_last_ms) {
    if (handle == 0u) return 0u;
    ipo_reg_t flags = spin_lock_irqsave(&s_irq_lock);

    uint32_t pending = 0u;
    for (uint32_t i = 0; i < s_sub_cap; i++) {
        if (s_subs[i].active && s_subs[i].handle == handle &&
            (caller_pid == 0u || s_subs[i].owner_pid == caller_pid)) {
            pending = s_subs[i].pending;
            s_subs[i].pending = 0u;
            if (out_total) {
                *out_total = (s_irq_counts && (size_t)s_subs[i].irq < s_irq_line_cap)
                             ? s_irq_counts[s_subs[i].irq] : 0u;
            }
            if (out_last_ms) *out_last_ms = s_subs[i].last_ms;
            break;
        }
    }

    spin_unlock_irqrestore(&s_irq_lock, flags);
    return pending;
}

uint32_t irq_get_count(uint8_t irq) {
    ipo_reg_t flags = spin_lock_irqsave(&s_irq_lock);
    uint32_t cnt = (s_irq_counts && (size_t)irq < s_irq_line_cap) ? s_irq_counts[irq] : 0u;
    spin_unlock_irqrestore(&s_irq_lock, flags);
    return cnt;
}

void irq_cleanup_pid(uint32_t pid) {
    if (pid == 0u) return;
    ipo_reg_t flags = spin_lock_irqsave(&s_irq_lock);

    for (uint32_t i = 0; i < s_handler_cap; i++) {
        if (s_handlers[i].active && s_handlers[i].owner_pid == pid) {
            uint8_t irq = s_handlers[i].irq;
            s_handlers[i].active = false;
            s_handlers[i].fn = NULL;
            irq_disable_locked(irq);
        }
    }
    for (uint32_t i = 0; i < s_sub_cap; i++) {
        if (s_subs[i].active && s_subs[i].owner_pid == pid) {
            uint8_t irq = s_subs[i].irq;
            s_subs[i].active = false;
            irq_disable_locked(irq);
        }
    }

    spin_unlock_irqrestore(&s_irq_lock, flags);
}
