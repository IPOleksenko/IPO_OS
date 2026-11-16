#ifndef KERNEL_SMP_H
#define KERNEL_SMP_H

#include <types.h>
#include <arch/cpu_regs.h>

typedef enum {
    SMP_STATUS_INACTIVE = 0,
    SMP_STATUS_ACTIVE   = 1,
    SMP_STATUS_BLOCKED  = 2
} smp_status_t;

typedef struct {
    smp_status_t cpu_capability_detection;
    smp_status_t acpi_madt_enumeration;
    smp_status_t mp_table_fallback;
    smp_status_t per_cpu_structures;
    smp_status_t atomic_spinlocks;
    smp_status_t ap_boot_sequence;            /* INIT-SIPI-SIPI via LAPIC ICR */
    smp_status_t driver_reentrancy;
    smp_status_t ipi_cross_core_reschedule;
    smp_status_t cross_core_scheduler;
} smp_architecture_status_t;

typedef struct {
    uint32_t cpu_id;                  /* 0 = BSP, 1..N = AP */
    uint32_t lapic_id;
    bool is_bsp;
    volatile bool is_online;
    ipo_vaddr_t stack_base;
    ipo_vaddr_t stack_top;
    volatile uint32_t ticks;
    volatile uint32_t tasks_executed;
    volatile uint32_t tasks_queued;
    volatile uint32_t busy;           /* 1 = executing task, 0 = idle */
    volatile uint32_t current_task_id;
} cpu_info_t;

typedef struct {
    volatile uint32_t lock;
} spinlock_t;

static inline void spinlock_init(spinlock_t *sl) {
    if (sl) sl->lock = 0;
}

static inline void spin_lock(spinlock_t *sl) {
    if (!sl) return;
    while (__atomic_test_and_set(&sl->lock, __ATOMIC_ACQUIRE)) {
        arch_cpu_pause();
    }
}

static inline void spin_unlock(spinlock_t *sl) {
    if (!sl) return;
    __atomic_clear(&sl->lock, __ATOMIC_RELEASE);
}

static inline bool spin_trylock(spinlock_t *sl) {
    if (!sl) return false;
    return !__atomic_test_and_set(&sl->lock, __ATOMIC_ACQUIRE);
}

static inline ipo_reg_t irq_save(void) {
    return arch_irq_save_regs();
}

static inline void irq_restore(ipo_reg_t flags) {
    arch_irq_restore_regs(flags);
}

static inline ipo_reg_t spin_lock_irqsave(spinlock_t *sl) {
    ipo_reg_t flags = irq_save();
    spin_lock(sl);
    return flags;
}

static inline void spin_unlock_irqrestore(spinlock_t *sl, ipo_reg_t flags) {
    spin_unlock(sl);
    irq_restore(flags);
}

#define SMP_CPU_ANY              0xFFFFFFFFu

#define SMP_ERR_OK               0
#define SMP_ERR_INVALID_PARAM   -1
#define SMP_ERR_INVALID_CPU     -2
#define SMP_ERR_CPU_OFFLINE     -3
#define SMP_ERR_QUEUE_FULL      -4
#define SMP_ERR_NOT_FOUND       -5
#define SMP_ERR_TIMEOUT         -6

#define SMP_TASK_STATE_FREE      0u
#define SMP_TASK_STATE_QUEUED    1u
#define SMP_TASK_STATE_RUNNING   2u
#define SMP_TASK_STATE_COMPLETED 3u
#define SMP_TASK_STATE_FAILED    4u

typedef ipo_uarg_t (*smp_work_fn_t)(uint32_t cpu_id, ipo_uarg_t arg);

typedef struct {
    uint32_t   task_id;
    uint32_t   requested_cpu;
    uint32_t   assigned_cpu;
    uint32_t   executed_cpu;
    uint32_t   executed_lapic_id;
    uint32_t   state;
    ipo_uarg_t return_value;
    uint32_t   exec_ticks;
} smp_task_result_t;

void smp_init(void);
void smp_sync_paging(ipo_paddr_t kernel_cr3);
uint32_t smp_get_cpu_count(void);
uint32_t smp_get_active_cpu_count(void);
const cpu_info_t *smp_get_cpu_info(uint32_t cpu_id);
uint32_t smp_get_current_cpu_id(void);
uint32_t smp_get_hw_lapic_id(void);
const smp_architecture_status_t *smp_get_architecture_status(void);
ipo_paddr_t smp_get_lapic_base(void);
void smp_print_summary(void);

extern volatile ipo_paddr_t smp_lapic_base;

void smp_send_ipi(uint32_t target_lapic_id, uint8_t vector);
void smp_send_broadcast_ipi(uint8_t vector);
uint32_t smp_get_ipi_count(uint32_t cpu_id);
typedef void (*smp_task_fn_t)(uint32_t cpu_id, void *arg);
void smp_broadcast_task(smp_task_fn_t fn, void *arg);

int  smp_submit_task(uint32_t target_cpu, smp_work_fn_t fn, ipo_uarg_t arg,
                     uint32_t flags, uint32_t *out_task_id);
int  smp_wait_task(uint32_t task_id, uint32_t timeout_ms, smp_task_result_t *out_res);
void smp_poll_local_tasks(void);

void smp_get_cpu_vendor(char *buf, size_t max_len);
char *smp_get_cpu_vendor_dyn(void);
void smp_get_cpu_brand(char *buf, size_t max_len);
char *smp_get_cpu_brand_dyn(void);
void smp_get_cpu_features_str(char *buf, size_t max_len);
char *smp_get_cpu_features_str_dyn(void);
void smp_get_cpu_model_info(uint32_t *family, uint32_t *model, uint32_t *stepping);

#endif /* KERNEL_SMP_H */
