#include <arch/context.h>
#include <arch/arch_backend.h>
#include <memory/kmalloc.h>
#include <string.h>

static ipo_id_t s_next_ctx_id = 1u;

static void *ctx_alloc_local(ipo_size_t sz, void *unused) {
    (void)unused;
    return kmalloc(sz);
}

static const ipo_mem_ops_t s_ctx_mem_ops = {
    .alloc = ctx_alloc_local,
    .realloc_fn = NULL,
    .free_fn = NULL,
    .ctx = NULL
};

void arch_context_init(arch_context_t *ctx) {
    if (!ctx) return;
    memset(ctx, 0, sizeof(*ctx));
    ctx->meta.context_id = s_next_ctx_id++;
    if (s_next_ctx_id == 0u) s_next_ctx_id = 1u;
    ctx->meta.arch_family = cpu_arch_register_name("x86", &s_ctx_mem_ops);
    ctx->meta.exec_bits = (ipo_bitwidth_t)(sizeof(void *) * 8u);
    ctx->meta.gpr_bits = (ipo_bitwidth_t)(sizeof(ipo_reg_t) * 8u);
    ctx->meta.ptr_bits = (ipo_bitwidth_t)(sizeof(void *) * 8u);
    ctx->meta.paging_levels = arch_mmu_get_paging_levels();
    ctx->meta.initialized = true;
}

void arch_context_destroy(arch_context_t *ctx) {
    if (!ctx) return;
    if (ctx->regs.slots) {
        kfree(ctx->regs.slots);
        ctx->regs.slots = NULL;
    }
    if (ctx->regs.blob) {
        kfree(ctx->regs.blob);
        ctx->regs.blob = NULL;
    }
    ctx->regs.slot_count = 0u;
    ctx->regs.slot_cap = 0u;
    ctx->regs.blob_size = 0u;
    ctx->regs.blob_cap = 0u;

    if (ctx->extensions.items) {
        for (ipo_size_t i = 0u; i < ctx->extensions.count; i++) {
            arch_context_ext_t *ext = &ctx->extensions.items[i];
            if (ext->cleanup && ext->data) {
                ext->cleanup(ext->data, ext->data_size);
            }
            if (ext->data) kfree(ext->data);
            if (ext->name) kfree(ext->name);
        }
        kfree(ctx->extensions.items);
        ctx->extensions.items = NULL;
    }
    ctx->extensions.count = 0u;
    ctx->extensions.cap = 0u;
    ctx->meta.valid = false;
    ctx->meta.initialized = false;
}

int arch_context_write_reg(arch_context_t *ctx,
                           const char *name,
                           arch_reg_role_t role,
                           const arch_reg_value_t *val) {
    if (!ctx || !name || !val) return -1;
    ipo_bitwidth_t bw = val->bit_width ? val->bit_width : (ipo_bitwidth_t)(sizeof(ipo_reg_t) * 8u);
    ipo_size_t byte_len = (ipo_size_t)((bw + 7u) / 8u);
    if (byte_len < sizeof(uint64_t)) byte_len = sizeof(uint64_t);

    /* Update matching slot in place if width fits */
    for (ipo_size_t i = 0u; i < ctx->regs.slot_count; i++) {
        arch_context_reg_slot_t *s = &ctx->regs.slots[i];
        if (s->name && strcmp(s->name, name) == 0 && s->bit_width == bw) {
            if (val->ext_limbs && val->ext_limb_count > 0u) {
                ipo_size_t copy_bytes = val->ext_limb_count * sizeof(uint64_t);
                if (copy_bytes > byte_len) copy_bytes = byte_len;
                memcpy(ctx->regs.blob + s->byte_offset, val->ext_limbs, copy_bytes);
            } else {
                uint64_t sc = val->scalar;
                memcpy(ctx->regs.blob + s->byte_offset, &sc, sizeof(sc));
            }
            return 0;
        }
    }

    if (ctx->regs.slot_count >= ctx->regs.slot_cap) {
        ipo_size_t ncap = ctx->regs.slot_cap ? (ctx->regs.slot_cap * 2u) : 8u;
        arch_context_reg_slot_t *ns = (arch_context_reg_slot_t *)krealloc(
            ctx->regs.slots, ncap * sizeof(arch_context_reg_slot_t));
        if (!ns) return -1;
        ctx->regs.slots = ns;
        ctx->regs.slot_cap = ncap;
    }

    ipo_size_t aligned_off = (ctx->regs.blob_size + 7u) & ~(ipo_size_t)7u;
    ipo_size_t need_blob = aligned_off + byte_len;
    if (need_blob > ctx->regs.blob_cap) {
        ipo_size_t ncap = ctx->regs.blob_cap ? ctx->regs.blob_cap : 64u;
        while (ncap < need_blob) ncap *= 2u;
        uint8_t *nb = (uint8_t *)krealloc(ctx->regs.blob, ncap);
        if (!nb) return -1;
        ctx->regs.blob = nb;
        ctx->regs.blob_cap = ncap;
    }

    memset(ctx->regs.blob + ctx->regs.blob_size, 0, need_blob - ctx->regs.blob_size);
    if (val->ext_limbs && val->ext_limb_count > 0u) {
        ipo_size_t copy_bytes = val->ext_limb_count * sizeof(uint64_t);
        if (copy_bytes > byte_len) copy_bytes = byte_len;
        memcpy(ctx->regs.blob + aligned_off, val->ext_limbs, copy_bytes);
    } else {
        uint64_t sc = val->scalar;
        memcpy(ctx->regs.blob + aligned_off, &sc, sizeof(sc));
    }

    const arch_reg_desc_t *reg_desc = arch_reg_find_by_name(name);
    arch_context_reg_slot_t *slot = &ctx->regs.slots[ctx->regs.slot_count++];
    slot->reg_id = reg_desc ? reg_desc->reg_id : 0u;
    slot->name = reg_desc ? reg_desc->name : name;
    slot->role = role;
    slot->bit_width = bw;
    slot->byte_offset = aligned_off;
    ctx->regs.blob_size = need_blob;
    return 0;
}

int arch_context_read_reg(const arch_context_t *ctx,
                          const char *name,
                          arch_reg_value_t *out_val) {
    if (!ctx || !name || !out_val) return -1;
    for (ipo_size_t i = 0u; i < ctx->regs.slot_count; i++) {
        const arch_context_reg_slot_t *s = &ctx->regs.slots[i];
        if (s->name && strcmp(s->name, name) == 0) {
            uint64_t sc = 0ULL;
            if (s->byte_offset + sizeof(sc) <= ctx->regs.blob_size) {
                memcpy(&sc, ctx->regs.blob + s->byte_offset, sizeof(sc));
            }
            arch_reg_value_from_u64(out_val, s->bit_width, sc);
            return 0;
        }
    }
    return -2;
}

int arch_context_add_extension(arch_context_t *ctx,
                               uint32_t ext_type,
                               const char *name,
                               const void *initial_data,
                               ipo_size_t data_size,
                               arch_ctx_ext_cleanup_t cleanup) {
    if (!ctx || ext_type == ARCH_CTX_EXT_NONE) return -1;
    if (ctx->extensions.count >= ctx->extensions.cap) {
        ipo_size_t ncap = ctx->extensions.cap ? (ctx->extensions.cap * 2u) : 4u;
        arch_context_ext_t *ni = (arch_context_ext_t *)krealloc(
            ctx->extensions.items, ncap * sizeof(arch_context_ext_t));
        if (!ni) return -1;
        ctx->extensions.items = ni;
        ctx->extensions.cap = ncap;
    }

    void *data_copy = NULL;
    if (data_size > 0u) {
        data_copy = kmalloc(data_size);
        if (!data_copy) return -1;
        if (initial_data) memcpy(data_copy, initial_data, data_size);
        else memset(data_copy, 0, data_size);
    }

    char *name_copy = NULL;
    if (name) {
        ipo_size_t len = strlen(name);
        name_copy = (char *)kmalloc(len + 1u);
        if (name_copy) memcpy(name_copy, name, len + 1u);
    }

    arch_context_ext_t *ext = &ctx->extensions.items[ctx->extensions.count++];
    ext->ext_type = ext_type;
    ext->name = name_copy;
    ext->data = data_copy;
    ext->data_size = data_size;
    ext->cleanup = cleanup;
    return 0;
}

const arch_context_ext_t *arch_context_find_extension(const arch_context_t *ctx,
                                                      uint32_t ext_type) {
    if (!ctx) return NULL;
    for (ipo_size_t i = 0u; i < ctx->extensions.count; i++) {
        if (ctx->extensions.items[i].ext_type == ext_type) {
            return &ctx->extensions.items[i];
        }
    }
    return NULL;
}

int arch_context_prepare(arch_context_t *ctx,
                         const arch_stack_state_t *stack,
                         ipo_vaddr_t entry_ip,
                         ipo_uarg_t arg,
                         void *mapped_top_kva,
                         ipo_vaddr_t *out_sp) {
    if (!ctx || !stack || !mapped_top_kva || entry_ip == 0u) return -1;
    if (!ctx->meta.initialized) arch_context_init(ctx);

    ctx->stack = *stack;
    ctx->continuation.entry_ip = entry_ip;
    ctx->continuation.entry_arg = arg;
    ctx->continuation.return_ip = 0u;
    ctx->flags.hw_flags = 0x02u;
    ctx->flags.irq_enabled = false;

    ipo_vaddr_t top = (ipo_vaddr_t)(uintptr_t)mapped_top_kva;
    ipo_reg_t *sp = (ipo_reg_t *)(uintptr_t)(top & ~(ipo_vaddr_t)15u);
    ipo_size_t frame_bytes = 0u;

    /* Initial stack frame matching arch_context_switch_raw pop order */
#if defined(__x86_64__)
    sp[-1] = 0u;                 /* 16 B + 8 ABI stack alignment pad above return IP */
    sp[-2] = (ipo_reg_t)entry_ip;
    sp[-3] = 0u;                 /* RBP */
    sp[-4] = 0u;                 /* RBX */
    sp[-5] = 0u;                 /* R12 */
    sp[-6] = 0u;                 /* R13 */
    sp[-7] = 0u;                 /* R14 */
    sp[-8] = 0u;                 /* R15 */
    sp[-9] = 0x02u;              /* RFLAGS */
    frame_bytes = (ipo_size_t)(top - (ipo_vaddr_t)(uintptr_t)&sp[-9]);
#else
    sp[-1] = (ipo_reg_t)entry_ip;
    sp[-2] = 0u;                 /* EBP */
    sp[-3] = 0u;                 /* EBX */
    sp[-4] = 0u;                 /* ESI */
    sp[-5] = 0u;                 /* EDI */
    sp[-6] = 0x02u;              /* EFLAGS */
    frame_bytes = (ipo_size_t)(top - (ipo_vaddr_t)(uintptr_t)&sp[-6]);
#endif

    ipo_vaddr_t vtop = stack->virtual_top ? stack->virtual_top : top;
    ipo_vaddr_t final_sp = vtop - (ipo_vaddr_t)frame_bytes;
    ctx->stack.sp = final_sp;
    ctx->meta.valid = true;

    /* Mirror initial state into context register slots */
    arch_reg_value_t v_ip, v_sp, v_fl;
    arch_reg_value_from_u64(&v_ip, ctx->meta.exec_bits, (uint64_t)entry_ip);
    arch_reg_value_from_u64(&v_sp, ctx->meta.exec_bits, (uint64_t)final_sp);
    arch_reg_value_from_u64(&v_fl, ctx->meta.exec_bits, 0x02ULL);
    arch_context_write_reg(ctx, "IP", ARCH_REG_ROLE_INSTR_PTR, &v_ip);
    arch_context_write_reg(ctx, "SP", ARCH_REG_ROLE_STACK_PTR, &v_sp);
    arch_context_write_reg(ctx, "FLAGS", ARCH_REG_ROLE_STATUS_FLAGS, &v_fl);

    if (out_sp) *out_sp = final_sp;
    return 0;
}

__attribute__((naked)) void arch_context_switch_raw(ipo_vaddr_t *old_sp, ipo_vaddr_t new_sp) {
    (void)old_sp;
    (void)new_sp;
#if defined(__x86_64__)
    __asm__ volatile(
        "pushq %rbp\n\t"
        "pushq %rbx\n\t"
        "pushq %r12\n\t"
        "pushq %r13\n\t"
        "pushq %r14\n\t"
        "pushq %r15\n\t"
        "pushfq\n\t"
        "movq %rsp, (%rdi)\n\t"
        "movq %rsi, %rsp\n\t"
        "popfq\n\t"
        "popq %r15\n\t"
        "popq %r14\n\t"
        "popq %r13\n\t"
        "popq %r12\n\t"
        "popq %rbx\n\t"
        "popq %rbp\n\t"
        "ret\n\t"
    );
#else
    __asm__ volatile(
        "pushl %ebp\n\t"
        "pushl %ebx\n\t"
        "pushl %esi\n\t"
        "pushl %edi\n\t"
        "pushfl\n\t"
        "movl 24(%esp), %eax\n\t"
        "movl 28(%esp), %edx\n\t"
        "movl %esp, (%eax)\n\t"
        "movl %edx, %esp\n\t"
        "popfl\n\t"
        "popl %edi\n\t"
        "popl %esi\n\t"
        "popl %ebx\n\t"
        "popl %ebp\n\t"
        "ret\n\t"
    );
#endif
}

void arch_context_switch(arch_context_t *from_ctx, arch_context_t *to_ctx) {
    if (!from_ctx || !to_ctx) return;
    arch_context_switch_raw(&from_ctx->stack.sp, to_ctx->stack.sp);
}

void arch_context_exec_user_entry(ipo_vaddr_t entry_ip,
                                  void *struct_ret_buf,
                                  bool is_struct_return,
                                  int argc,
                                  char **argv,
                                  char **envp,
                                  ipo_reg_t *out_ret0,
                                  ipo_reg_t *out_ret1) {
    ipo_reg_t ret0 = 0u;
    ipo_reg_t ret1 = 0u;

    if (is_struct_return && struct_ret_buf) {
        typedef void *(*struct_entry_fn_t)(void *ret_buf, int argc, char **argv, char **envp);
        struct_entry_fn_t fn = (struct_entry_fn_t)(uintptr_t)entry_ip;
        arch_irq_enable();
        fn(struct_ret_buf, argc, argv, envp);
        arch_irq_disable();
        ret0 = (ipo_reg_t)(uintptr_t)struct_ret_buf;
    } else {
        typedef int (*entry_fn_t)(int, char **, char **);
        entry_fn_t fn = (entry_fn_t)(uintptr_t)entry_ip;
        ipo_reg_t eax_fn = (ipo_reg_t)(uintptr_t)fn;
        __asm__ volatile(
            "pushl %%ebx\n\t"
            "pushl %%esi\n\t"
            "pushl %%edi\n\t"
            "pushl %3\n\t"
            "pushl %4\n\t"
            "pushl %5\n\t"
            "xorl %%ebx, %%ebx\n\t"
            "xorl %%ecx, %%ecx\n\t"
            "xorl %%edx, %%edx\n\t"
            "xorl %%esi, %%esi\n\t"
            "xorl %%edi, %%edi\n\t"
            "sti\n\t"
            "call *%%eax\n\t"
            "cli\n\t"
            "addl $12, %%esp\n\t"
            "popl %%edi\n\t"
            "popl %%esi\n\t"
            "popl %%ebx\n\t"
            : "=a"(ret0), "=d"(ret1)
            : "0"(eax_fn), "m"(envp), "m"(argv), "m"(argc)
            : "ecx", "memory", "cc"
        );
    }

    if (out_ret0) *out_ret0 = ret0;
    if (out_ret1) *out_ret1 = ret1;
}

__attribute__((naked)) static void x86_trampoline_stack_shim(
    ipo_reg_t new_root,
    ipo_reg_t old_root,
    void (*inner_fn)(ipo_reg_t, ipo_reg_t, void (*)(void *), void *),
    void (*target_fn)(void *),
    void *target_arg,
    ipo_vaddr_t kstack_top) {
    (void)new_root; (void)old_root; (void)inner_fn;
    (void)target_fn; (void)target_arg; (void)kstack_top;
#if defined(__x86_64__)
    __asm__ volatile(
        "pushq %%rbx\n\t"
        "movq %%rsp, %%rbx\n\t"
        "andq $-16, %%r9\n\t"
        "movq %%r9, %%rsp\n\t"
        "movq %%rdx, %%rax\n\t"
        "movq %%rcx, %%rdx\n\t"
        "movq %%r8, %%rcx\n\t"
        "call *%%rax\n\t"
        "movq %%rbx, %%rsp\n\t"
        "popq %%rbx\n\t"
        "ret\n\t"
        : : : "memory"
    );
#else
    __asm__ volatile(
        "pushl %%ebx\n\t"
        "pushl %%esi\n\t"
        "pushl %%edi\n\t"
        "movl 16(%%esp), %%eax\n\t"  /* new_root */
        "movl 20(%%esp), %%ecx\n\t"  /* old_root */
        "movl 24(%%esp), %%ebx\n\t"  /* inner_fn */
        "movl 28(%%esp), %%edx\n\t"  /* target_fn */
        "movl 32(%%esp), %%esi\n\t"  /* target_arg */
        "movl 36(%%esp), %%edi\n\t"  /* kstack_top */
        "andl $0xFFFFFFF0, %%edi\n\t"
        "subl $16, %%edi\n\t"
        "movl %%esp, (%%edi)\n\t"
        "movl %%edi, %%esp\n\t"
        "pushl %%esi\n\t"
        "pushl %%edx\n\t"
        "pushl %%ecx\n\t"
        "pushl %%eax\n\t"
        "call *%%ebx\n\t"
        "addl $16, %%esp\n\t"
        "popl %%esp\n\t"
        "popl %%edi\n\t"
        "popl %%esi\n\t"
        "popl %%ebx\n\t"
        "ret\n\t"
        : : : "memory"
    );
#endif
}

void arch_context_call_on_new_stack(ipo_paddr_t new_root,
                                    ipo_paddr_t old_root,
                                    void (*inner_fn)(ipo_reg_t, ipo_reg_t, void (*)(void *), void *),
                                    void (*target_fn)(void *),
                                    void *target_arg,
                                    ipo_vaddr_t kstack_top) {
    x86_trampoline_stack_shim((ipo_reg_t)new_root, (ipo_reg_t)old_root,
                              inner_fn, target_fn, target_arg, kstack_top);
}
