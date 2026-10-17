#ifndef KERNEL_SMP_H
#define KERNEL_SMP_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

/* Component status flags for honest architectural scaffolding evaluation */
typedef enum {
    SMP_STATUS_INACTIVE = 0,
    SMP_STATUS_ACTIVE   = 1,
    SMP_STATUS_BLOCKED  = 2
} smp_status_t;

typedef struct {
    smp_status_t cpu_capability_detection;    /* ACTIVE: CPUID leaf 1 & feature flags */
    smp_status_t acpi_madt_enumeration;       /* ACTIVE: ACPI RSDP -> RSDT -> MADT APIC entries */
    smp_status_t mp_table_fallback;           /* ACTIVE: MP floating pointer table scanning */
    smp_status_t per_cpu_structures;          /* ACTIVE: Per-CPU stacks and cpu_info_t state */
    smp_status_t atomic_spinlocks;            /* ACTIVE: Hardware bus-locked spinlocks */
    smp_status_t ap_boot_sequence;            /* ACTIVE: INIT-SIPI-SIPI via LAPIC ICR */
    smp_status_t driver_reentrancy;           /* BLOCKED: ATA/VGA/Net drivers lack fine-grained locks */
    smp_status_t ipi_cross_core_reschedule;   /* BLOCKED: Requires APIC timer/IPI IDT vectors */
    smp_status_t cross_core_scheduler;        /* BLOCKED: Requires multi-queue preemptive SMP scheduler */
} smp_architecture_status_t;

/* Per-CPU information structure */
typedef struct {
    uint32_t cpu_id;           /* Logical core ID (0 = BSP, 1..N = AP) */
    uint32_t lapic_id;         /* Local APIC ID */
    bool is_bsp;               /* Bootstrap processor */
    volatile bool is_online;   /* Online / active */
    uint32_t stack_base;       /* Allocated per-CPU stack base */
    uint32_t stack_top;        /* Per-CPU stack top (ESP) */
    volatile uint32_t ticks;   /* Per-CPU tick counter */
} cpu_info_t;

/* Hardware bus-locked atomic spinlock */
typedef struct {
    volatile uint32_t lock;
} spinlock_t;

static inline void spinlock_init(spinlock_t *sl) {
    if (sl) sl->lock = 0;
}

static inline void spin_lock(spinlock_t *sl) {
    if (!sl) return;
    while (__atomic_test_and_set(&sl->lock, __ATOMIC_ACQUIRE)) {
        __builtin_ia32_pause();
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

/* SMP Subsystem API */
void smp_init(void);
uint32_t smp_get_cpu_count(void);
uint32_t smp_get_active_cpu_count(void);
const cpu_info_t *smp_get_cpu_info(uint32_t cpu_id);
uint32_t smp_get_current_cpu_id(void);
const smp_architecture_status_t *smp_get_architecture_status(void);
uint32_t smp_get_lapic_base(void);
void smp_print_summary(void);

/* Cross-Core Communication & Diagnostic APIs */
void ap_main(uint32_t cpu_id);
void smp_send_ipi(uint32_t target_lapic_id, uint8_t vector);
void smp_send_broadcast_ipi(uint8_t vector);
uint32_t smp_get_ipi_count(uint32_t cpu_id);
typedef void (*smp_task_fn_t)(uint32_t cpu_id, void *arg);
void smp_broadcast_task(smp_task_fn_t fn, void *arg);



/* Extended CPU capability queries */
void smp_get_cpu_vendor(char *buf, size_t max_len);
void smp_get_cpu_brand(char *buf, size_t max_len);
void smp_get_cpu_features_str(char *buf, size_t max_len);
void smp_get_cpu_model_info(uint32_t *family, uint32_t *model, uint32_t *stepping);

#endif /* KERNEL_SMP_H */


