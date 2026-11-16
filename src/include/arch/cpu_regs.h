#ifndef IPO_ARCH_CPU_REGS_H
#define IPO_ARCH_CPU_REGS_H

#include <types.h>

/* Architecture-neutral processor control and register descriptor interface */

typedef enum {
    ARCH_REG_ROLE_UNKNOWN = 0,
    ARCH_REG_ROLE_STATUS_FLAGS,
    ARCH_REG_ROLE_STACK_PTR,
    ARCH_REG_ROLE_INSTR_PTR,
    ARCH_REG_ROLE_CODE_SEG,
    ARCH_REG_ROLE_MMU_CTRL,
    ARCH_REG_ROLE_MMU_FAULT_ADDR,
    ARCH_REG_ROLE_MMU_ROOT,
    ARCH_REG_ROLE_EXT_CTRL,
    ARCH_REG_ROLE_MODE_ENABLE,
    ARCH_REG_ROLE_TLS_BASE,
    ARCH_REG_ROLE_TIMESTAMP,
    ARCH_REG_ROLE_HW_CAP_INFO,
    ARCH_REG_ROLE_CUSTOM_START = 0x1000
} arch_reg_role_t;

#define ARCH_REG_ACCESS_READABLE     (1u << 0)
#define ARCH_REG_ACCESS_WRITABLE     (1u << 1)
#define ARCH_REG_ACCESS_PRIVILEGED   (1u << 2)
#define ARCH_REG_ACCESS_VOLATILE     (1u << 3)
#define ARCH_REG_ACCESS_MODE_SCOPED  (1u << 4)

/* Scalar values <=64 bits sit inline; wider values use ext_limbs */
typedef struct {
    ipo_bitwidth_t bit_width;
    uint64_t       scalar;
    uint64_t      *ext_limbs;
    ipo_size_t     ext_limb_count;
} arch_reg_value_t;

typedef struct {
    ipo_id_t        reg_id;
    const char     *name;
    const char     *purpose;
    arch_reg_role_t role;
    ipo_bitwidth_t  bit_width;
    uint32_t        access_flags;
    uint32_t        hw_encoding;
} arch_reg_desc_t;

typedef struct {
    const char *arch_name;
    ipo_size_t (*reg_count)(void);
    const arch_reg_desc_t *(*reg_get)(ipo_size_t index);
    bool (*reg_is_available)(const arch_reg_desc_t *desc);
    int (*reg_read)(const arch_reg_desc_t *desc, arch_reg_value_t *out_val);
    int (*reg_write)(const arch_reg_desc_t *desc, const arch_reg_value_t *val);
    int (*reg_read_by_role)(arch_reg_role_t role, uint64_t *out_val);
    int (*reg_write_by_role)(arch_reg_role_t role, uint64_t val);
} arch_reg_backend_ops_t;

void arch_reg_value_init(arch_reg_value_t *val);
void arch_reg_value_from_u64(arch_reg_value_t *val, ipo_bitwidth_t bit_width, uint64_t scalar);
uint64_t arch_reg_value_as_u64(const arch_reg_value_t *val);
void arch_reg_value_free(arch_reg_value_t *val);

void arch_reg_bind_backend(const arch_reg_backend_ops_t *ops);
int  arch_reg_register_dynamic(const arch_reg_desc_t *desc);
ipo_size_t arch_reg_count(void);
const arch_reg_desc_t *arch_reg_get_desc(ipo_size_t index);
const arch_reg_desc_t *arch_reg_find_by_name(const char *name);
const arch_reg_desc_t *arch_reg_find_by_role(arch_reg_role_t role);
bool arch_reg_is_available(const arch_reg_desc_t *desc);
int  arch_reg_read(const arch_reg_desc_t *desc, arch_reg_value_t *out_val);
int  arch_reg_write(const arch_reg_desc_t *desc, const arch_reg_value_t *val);
int  arch_reg_read_u64(arch_reg_role_t role, uint64_t *out_val);
int  arch_reg_write_u64(arch_reg_role_t role, uint64_t val);

void      arch_irq_enable(void);
void      arch_irq_disable(void);
bool      arch_irq_is_enabled(void);
ipo_reg_t arch_irq_save_regs(void);
void      arch_irq_restore_regs(ipo_reg_t saved_flags);

void      arch_cpu_pause(void);
void      arch_cpu_halt(void);
void __attribute__((noreturn)) arch_cpu_halt_forever(void);
void      arch_cpu_serialize(void);
uint64_t  arch_cpu_read_timestamp(void);

ipo_vaddr_t arch_stack_get_sp(void);
void        arch_stack_set_sp(ipo_vaddr_t sp);

/* Write-protection bypass for kernel code patching */
ipo_reg_t arch_cpu_wp_disable(void);
void      arch_cpu_wp_restore(ipo_reg_t token);
void      arch_cpu_atomic_patch_bytes(ipo_vaddr_t dst_vaddr, const void *src, ipo_size_t len);

void arch_cpu_enable_fpu_simd(void);
void arch_cpu_clear_fpu_task_switched(void);
void arch_cpu_set_tls_base(ipo_vaddr_t tls_base);

ipo_paddr_t arch_mmu_get_root(void);
void        arch_mmu_set_root(ipo_paddr_t root_paddr);
void        arch_mmu_flush_all(void);
void        arch_mmu_flush_page(ipo_vaddr_t vaddr);
ipo_vaddr_t arch_mmu_get_fault_addr(void);
uint32_t    arch_mmu_get_paging_levels(void);
bool        arch_mmu_is_paging_enabled(void);
void        arch_mmu_enable_paging(ipo_paddr_t root_paddr, uint32_t paging_levels);
bool        arch_mmu_is_vaddr_mapped(ipo_vaddr_t vaddr);

void arch_cpu_backend_init(void);

#endif /* IPO_ARCH_CPU_REGS_H */
