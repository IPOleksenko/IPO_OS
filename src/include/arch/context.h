#ifndef IPO_ARCH_CONTEXT_H
#define IPO_ARCH_CONTEXT_H

#include <types.h>
#include <arch/cpu_regs.h>

/* Execution context and extensible register/hardware-extension state. */

/* >= 0x1000 reserved for platform-specific extensions */
enum {
    ARCH_CTX_EXT_NONE = 0u,
    ARCH_CTX_EXT_FPU_SIMD,
    ARCH_CTX_EXT_TLS_BASE,
    ARCH_CTX_EXT_SIGNAL_ALTSTACK,
    ARCH_CTX_EXT_PLATFORM_CUSTOM = 0x1000u
};

typedef struct {
    ipo_id_t       context_id;
    ipo_id_t       exec_mode_id;
    uint32_t       arch_family;
    ipo_bitwidth_t exec_bits;
    ipo_bitwidth_t gpr_bits;
    ipo_bitwidth_t ptr_bits;
    uint32_t       paging_levels;
    bool           initialized;
    bool           valid;
} arch_context_meta_t;

typedef struct {
    ipo_vaddr_t stack_base;
    ipo_size_t  stack_size;
    ipo_vaddr_t virtual_top;
    ipo_vaddr_t virtual_bottom;
    ipo_vaddr_t guard_page;
    ipo_vaddr_t sp;
} arch_stack_state_t;

typedef struct {
    ipo_vaddr_t entry_ip;
    ipo_uarg_t  entry_arg;
    ipo_vaddr_t return_ip;
} arch_continuation_t;

typedef struct {
    ipo_reg_t hw_flags;
    bool      irq_enabled;
} arch_flags_state_t;

typedef struct {
    ipo_id_t        reg_id;
    const char     *name;
    arch_reg_role_t role;
    ipo_bitwidth_t  bit_width;
    ipo_size_t      byte_offset;
} arch_context_reg_slot_t;

typedef struct {
    arch_context_reg_slot_t *slots;
    ipo_size_t               slot_count;
    ipo_size_t               slot_cap;
    uint8_t                 *blob;
    ipo_size_t               blob_size;
    ipo_size_t               blob_cap;
} arch_reg_state_t;

typedef void (*arch_ctx_ext_cleanup_t)(void *data, ipo_size_t data_size);

typedef struct {
    uint32_t               ext_type;
    char                  *name;
    void                  *data;
    ipo_size_t             data_size;
    arch_ctx_ext_cleanup_t cleanup;
} arch_context_ext_t;

typedef struct {
    arch_context_ext_t *items;
    ipo_size_t          count;
    ipo_size_t          cap;
} arch_context_ext_set_t;

typedef struct arch_context {
    arch_context_meta_t    meta;
    arch_stack_state_t     stack;
    arch_continuation_t    continuation;
    arch_flags_state_t     flags;
    arch_reg_state_t       regs;
    arch_context_ext_set_t extensions;
} arch_context_t;

void arch_context_init(arch_context_t *ctx);
void arch_context_destroy(arch_context_t *ctx);

/* Populate initial frame in mapped_top_kva for cooperative switching; returns sp in *out_sp */
int arch_context_prepare(arch_context_t *ctx,
                         const arch_stack_state_t *stack,
                         ipo_vaddr_t entry_ip,
                         ipo_uarg_t arg,
                         void *mapped_top_kva,
                         ipo_vaddr_t *out_sp);

int arch_context_write_reg(arch_context_t *ctx,
                           const char *name,
                           arch_reg_role_t role,
                           const arch_reg_value_t *val);
int arch_context_read_reg(const arch_context_t *ctx,
                          const char *name,
                          arch_reg_value_t *out_val);

int arch_context_add_extension(arch_context_t *ctx,
                               uint32_t ext_type,
                               const char *name,
                               const void *initial_data,
                               ipo_size_t data_size,
                               arch_ctx_ext_cleanup_t cleanup);
const arch_context_ext_t *arch_context_find_extension(const arch_context_t *ctx,
                                                      uint32_t ext_type);

void arch_context_switch(arch_context_t *from_ctx, arch_context_t *to_ctx);
void arch_context_switch_raw(ipo_vaddr_t *old_sp, ipo_vaddr_t new_sp);

void arch_context_exec_user_entry(ipo_vaddr_t entry_ip,
                                  void *struct_ret_buf,
                                  bool is_struct_return,
                                  int argc,
                                  char **argv,
                                  char **envp,
                                  ipo_reg_t *out_ret0,
                                  ipo_reg_t *out_ret1);

void arch_context_call_on_new_stack(ipo_paddr_t new_root,
                                    ipo_paddr_t old_root,
                                    void (*inner_fn)(ipo_reg_t, ipo_reg_t, void (*)(void *), void *),
                                    void (*target_fn)(void *),
                                    void *target_arg,
                                    ipo_vaddr_t kstack_top);

#endif /* IPO_ARCH_CONTEXT_H */
