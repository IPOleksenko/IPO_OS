#include <kernel/dynmod.h>
#include <kernel/kmod.h>
#include <kernel/smp.h>
#include <arch/cpu_regs.h>
#include <memory/kmalloc.h>
#include <memory/address_space.h>
#include <string.h>
#include <stdio.h>

volatile uint32_t g_dynmod_target_var = 100u;

__attribute__((noinline, used)) uint32_t dynmod_target_compute(uint32_t x, uint32_t y) {
    __asm__ volatile("nop; nop; nop; nop; nop; nop;" ::: "memory");
    return x + y + g_dynmod_target_var;
}

__attribute__((noinline, used)) uint32_t dynmod_alt_compute(uint32_t x, uint32_t y) {
    __asm__ volatile("nop; nop; nop; nop; nop; nop;" ::: "memory");
    return (x * 10u) + (y * 100u) + g_dynmod_target_var;
}

typedef struct {
    bool                  active;
    ipo_id_t              id;
    dynmod_type_t         type;
    ipo_id_t              owner_pid;
    ipo_vaddr_t           target_addr;
    ipo_vaddr_t           replacement_addr;
    ipo_size_t            size;
    uint8_t               *orig_bytes;
    uint8_t               *new_bytes;
    ipo_syscall_handler_t orig_syscall_fn;
} dynmod_entry_t;

static dynmod_entry_t *s_patches = NULL;
static uint32_t        s_patch_cap = 0u;
static uint32_t        s_next_patch_id = 1u;
static uint32_t       *s_auth_pids = NULL;
static uint32_t        s_auth_cap = 0u;
static uint32_t        s_auth_count = 0u;
static spinlock_t      s_dynmod_lock = {0};
static bool            s_dynmod_inited = false;

void dynmod_init(void) {
    if (s_dynmod_inited) return;
    spinlock_init(&s_dynmod_lock);
    s_auth_pids = NULL;
    s_auth_cap = 0u;
    s_auth_count = 0u;
    s_dynmod_inited = true;
}

void dynmod_revoke(uint32_t pid) {
    if (pid == 0u || !s_dynmod_inited) return;
    ipo_reg_t irq_flags = spin_lock_irqsave(&s_dynmod_lock);
    for (uint32_t i = 0; i < s_auth_count; i++) {
        if (s_auth_pids[i] == pid) {
            s_auth_pids[i] = s_auth_pids[s_auth_count - 1u];
            s_auth_pids[s_auth_count - 1u] = 0u;
            s_auth_count--;
            break;
        }
    }
    spin_unlock_irqrestore(&s_dynmod_lock, irq_flags);
}

int dynmod_authorize(uint32_t pid, uint32_t token) {
    if (!s_dynmod_inited) dynmod_init();
    if (token == 0u) {
        dynmod_revoke(pid);
        return DYNMOD_OK;
    }
    if (token != DYNMOD_AUTH_TOKEN) {
        return DYNMOD_ERR_PERM;
    }
    if (pid == 0u) return DYNMOD_OK;

    ipo_reg_t irq_flags = spin_lock_irqsave(&s_dynmod_lock);
    for (uint32_t i = 0; i < s_auth_count; i++) {
        if (s_auth_pids[i] == pid) {
            spin_unlock_irqrestore(&s_dynmod_lock, irq_flags);
            return DYNMOD_OK;
        }
    }
    if (s_auth_count >= s_auth_cap) {
        uint32_t ncap = s_auth_cap ? (s_auth_cap * 2u) : 16u;
        uint32_t *narr = (uint32_t *)krealloc(s_auth_pids, (size_t)ncap * sizeof(uint32_t));
        if (!narr) {
            spin_unlock_irqrestore(&s_dynmod_lock, irq_flags);
            return DYNMOD_ERR_NOMEM;
        }
        memset(narr + s_auth_cap, 0, (size_t)(ncap - s_auth_cap) * sizeof(uint32_t));
        s_auth_pids = narr;
        s_auth_cap = ncap;
    }
    s_auth_pids[s_auth_count++] = pid;
    spin_unlock_irqrestore(&s_dynmod_lock, irq_flags);
    return DYNMOD_OK;
}

bool dynmod_is_authorized(uint32_t pid) {
    if (pid == 0u) return true; /* Kernel / .kmd context */
    if (!s_dynmod_inited) return false;

    ipo_reg_t irq_flags = spin_lock_irqsave(&s_dynmod_lock);
    for (uint32_t i = 0; i < s_auth_count; i++) {
        if (s_auth_pids[i] == pid) {
            spin_unlock_irqrestore(&s_dynmod_lock, irq_flags);
            return true;
        }
    }
    spin_unlock_irqrestore(&s_dynmod_lock, irq_flags);
    return false;
}

static bool is_page_mapped(ipo_vaddr_t vaddr) {
    return arch_mmu_is_vaddr_mapped(vaddr);
}

static bool validate_range(ipo_vaddr_t addr, size_t size, bool for_var_write) {
    if (addr < 0x1000u || size == 0u) return false;
    ipo_vaddr_t end_excl = 0u;
    if (!ipo_vaddr_add_ok(addr, size, &end_excl)) return false;
    if (end_excl > (ipo_vaddr_t)0xF0000000u) return false;
    if (for_var_write && addr >= 0x000C0000u && addr < 0x00100000u) {
        return false; /* BIOS ROM region is read-only */
    }
    ipo_vaddr_t cur = addr & ~(ipo_vaddr_t)0xFFFu;
    ipo_vaddr_t end = (end_excl - 1u) & ~(ipo_vaddr_t)0xFFFu;
    while (cur <= end) {
        if (!is_page_mapped(cur)) return false;
        if (cur == end) break;
        cur += 0x1000u;
    }
    return true;
}

static void smp_serialize_icache_cb(uint32_t cpu_id, void *arg) {
    (void)cpu_id;
    (void)arg;
    arch_cpu_serialize();
}

static void atomic_write_code_bytes(ipo_vaddr_t target_addr, const uint8_t *src, size_t len) {
    arch_cpu_atomic_patch_bytes(target_addr, src, (ipo_size_t)len);
}

static dynmod_entry_t *alloc_patch_slot_locked(void) {
    uint32_t slot = s_patch_cap;
    for (uint32_t i = 0; i < s_patch_cap; i++) {
        if (!s_patches[i].active) {
            slot = i;
            break;
        }
    }
    if (slot == s_patch_cap) {
        uint32_t ncap = s_patch_cap ? (s_patch_cap * 2u) : 16u;
        dynmod_entry_t *narr = (dynmod_entry_t *)krealloc(s_patches, (size_t)ncap * sizeof(dynmod_entry_t));
        if (!narr) return NULL;
        memset(&narr[s_patch_cap], 0, (size_t)(ncap - s_patch_cap) * sizeof(dynmod_entry_t));
        s_patches = narr;
        s_patch_cap = ncap;
    }
    dynmod_entry_t *e = &s_patches[slot];
    memset(e, 0, sizeof(*e));
    e->active = true;
    e->id = s_next_patch_id++;
    return e;
}

static bool alloc_patch_buffers(dynmod_entry_t *e, size_t size) {
    e->orig_bytes = (uint8_t *)kmalloc(size);
    e->new_bytes  = (uint8_t *)kmalloc(size);
    if (!e->orig_bytes || !e->new_bytes) {
        if (e->orig_bytes) kfree(e->orig_bytes);
        if (e->new_bytes)  kfree(e->new_bytes);
        e->orig_bytes = NULL;
        e->new_bytes  = NULL;
        return false;
    }
    return true;
}

int dynmod_resolve_symbol(const char *name, ipo_vaddr_t *out_addr) {
    if (!name || !*name || !out_addr) return DYNMOD_ERR_PARAM;
    void *addr = kmod_resolve_symbol(name);
    if (!addr) {
        if (strcmp(name, "g_dynmod_target_var") == 0) addr = (void *)&g_dynmod_target_var;
        else if (strcmp(name, "dynmod_target_compute") == 0) addr = (void *)dynmod_target_compute;
        else if (strcmp(name, "dynmod_alt_compute") == 0) addr = (void *)dynmod_alt_compute;
    }
    if (!addr) return DYNMOD_ERR_NOT_FOUND;
    *out_addr = (ipo_vaddr_t)addr;
    return DYNMOD_OK;
}

int dynmod_read_mem(ipo_vaddr_t addr, void *out_buf, size_t size) {
    if (!out_buf || size == 0u) return DYNMOD_ERR_PARAM;
    if (!validate_range(addr, size, false)) return DYNMOD_ERR_ADDR;
    memcpy(out_buf, (const void *)addr, size);
    return DYNMOD_OK;
}

int dynmod_write_var(ipo_vaddr_t addr, const void *in_buf, size_t size,
                     ipo_id_t *out_patch_id, ipo_id_t caller_pid) {
    if (!s_dynmod_inited) dynmod_init();
    if (!dynmod_is_authorized(caller_pid)) return DYNMOD_ERR_PERM;
    if (!in_buf || size == 0u) return DYNMOD_ERR_PARAM;
    if (!validate_range(addr, size, true)) return DYNMOD_ERR_ADDR;

    ipo_reg_t irq_flags = spin_lock_irqsave(&s_dynmod_lock);
    dynmod_entry_t *e = alloc_patch_slot_locked();
    if (!e) {
        spin_unlock_irqrestore(&s_dynmod_lock, irq_flags);
        return DYNMOD_ERR_NOMEM;
    }
    if (!alloc_patch_buffers(e, size)) {
        memset(e, 0, sizeof(*e));
        spin_unlock_irqrestore(&s_dynmod_lock, irq_flags);
        return DYNMOD_ERR_NOMEM;
    }

    e->type = DYNMOD_TYPE_VAR;
    e->owner_pid = caller_pid;
    e->target_addr = addr;
    e->size = size;
    memcpy(e->orig_bytes, (const void *)addr, size);
    memcpy(e->new_bytes, in_buf, size);

    /* Clears CR0.WP temporarily in case the target is in rodata */
    atomic_write_code_bytes(addr, (const uint8_t *)in_buf, size);

    ipo_id_t pid = e->id;
    spin_unlock_irqrestore(&s_dynmod_lock, irq_flags);

    if (out_patch_id) *out_patch_id = pid;
    serial_printf("[dynmod] Modified variable @ 0x%x (%u bytes, patch_id=%u)\n",
                  (uint32_t)addr, (uint32_t)size, pid);
    return DYNMOD_OK;
}

int dynmod_hook_function(ipo_vaddr_t target_fn, ipo_vaddr_t replacement_fn,
                         ipo_id_t *out_patch_id, ipo_id_t caller_pid) {
    if (!s_dynmod_inited) dynmod_init();
    if (!dynmod_is_authorized(caller_pid)) return DYNMOD_ERR_PERM;
    if (target_fn == 0u || replacement_fn == 0u || target_fn == replacement_fn) {
        return DYNMOD_ERR_PARAM;
    }
    if (!validate_range(target_fn, 12u, false) || !validate_range(replacement_fn, 5u, false)) {
        return DYNMOD_ERR_ADDR;
    }

    ipo_reg_t irq_flags = spin_lock_irqsave(&s_dynmod_lock);
    for (uint32_t i = 0; i < s_patch_cap; i++) {
        if (s_patches[i].active && s_patches[i].target_addr == target_fn &&
            s_patches[i].type == DYNMOD_TYPE_FUNC_HOOK) {
            spin_unlock_irqrestore(&s_dynmod_lock, irq_flags);
            return DYNMOD_ERR_CONFLICT;
        }
    }

    dynmod_entry_t *e = alloc_patch_slot_locked();
    if (!e) {
        spin_unlock_irqrestore(&s_dynmod_lock, irq_flags);
        return DYNMOD_ERR_NOMEM;
    }

    ipo_svaddr_t diff = (ipo_svaddr_t)replacement_fn - (ipo_svaddr_t)(target_fn + 5u);
    bool use_rel32 = (diff >= (ipo_svaddr_t)(-0x7FFFFFFFLL - 1LL) && diff <= (ipo_svaddr_t)0x7FFFFFFFLL);
    size_t patch_sz = use_rel32 ? 5u : 12u;
    uint8_t *jmp_patch = (uint8_t *)kmalloc(patch_sz);
    if (!jmp_patch) {
        memset(e, 0, sizeof(*e));
        spin_unlock_irqrestore(&s_dynmod_lock, irq_flags);
        return DYNMOD_ERR_NOMEM;
    }
    if (use_rel32) {
        int32_t rel32 = (int32_t)diff;
        jmp_patch[0] = 0xE9u; /* JMP rel32 */
        memcpy(&jmp_patch[1], &rel32, 4u);
    } else {
        uint64_t target64 = (uint64_t)replacement_fn;
        jmp_patch[0] = 0x48u; /* movabs rax, imm64; jmp rax */
        jmp_patch[1] = 0xB8u;
        memcpy(&jmp_patch[2], &target64, 8u);
        jmp_patch[10] = 0xFFu;
        jmp_patch[11] = 0xE0u;
    }

    if (!alloc_patch_buffers(e, patch_sz)) {
        kfree(jmp_patch);
        memset(e, 0, sizeof(*e));
        spin_unlock_irqrestore(&s_dynmod_lock, irq_flags);
        return DYNMOD_ERR_NOMEM;
    }

    e->type = DYNMOD_TYPE_FUNC_HOOK;
    e->owner_pid = caller_pid;
    e->target_addr = target_fn;
    e->replacement_addr = replacement_fn;
    e->size = patch_sz;
    memcpy(e->orig_bytes, (const void *)target_fn, patch_sz);
    memcpy(e->new_bytes, jmp_patch, patch_sz);

    atomic_write_code_bytes(target_fn, jmp_patch, patch_sz);
    kfree(jmp_patch);
    ipo_id_t pid = e->id;
    spin_unlock_irqrestore(&s_dynmod_lock, irq_flags);

    /* Serialize icache across online cores */
    smp_broadcast_task(smp_serialize_icache_cb, NULL);

    if (out_patch_id) *out_patch_id = pid;
    serial_printf("[dynmod] Hooked function 0x%x -> 0x%x (bytes=%u, patch_id=%u)\n",
                  (uint32_t)target_fn, (uint32_t)replacement_fn, (uint32_t)patch_sz, pid);
    return DYNMOD_OK;
}

int dynmod_patch_code(ipo_vaddr_t target_addr, const void *new_bytes, size_t size,
                      ipo_id_t *out_patch_id, ipo_id_t caller_pid) {
    if (!s_dynmod_inited) dynmod_init();
    if (!dynmod_is_authorized(caller_pid)) return DYNMOD_ERR_PERM;
    if (!new_bytes || size == 0u) return DYNMOD_ERR_PARAM;
    if (!validate_range(target_addr, size, false)) return DYNMOD_ERR_ADDR;

    ipo_reg_t irq_flags = spin_lock_irqsave(&s_dynmod_lock);
    dynmod_entry_t *e = alloc_patch_slot_locked();
    if (!e) {
        spin_unlock_irqrestore(&s_dynmod_lock, irq_flags);
        return DYNMOD_ERR_NOMEM;
    }
    if (!alloc_patch_buffers(e, size)) {
        memset(e, 0, sizeof(*e));
        spin_unlock_irqrestore(&s_dynmod_lock, irq_flags);
        return DYNMOD_ERR_NOMEM;
    }

    e->type = DYNMOD_TYPE_CODE_PATCH;
    e->owner_pid = caller_pid;
    e->target_addr = target_addr;
    e->size = size;
    memcpy(e->orig_bytes, (const void *)target_addr, size);
    memcpy(e->new_bytes, new_bytes, size);

    atomic_write_code_bytes(target_addr, (const uint8_t *)new_bytes, size);
    ipo_id_t pid = e->id;
    spin_unlock_irqrestore(&s_dynmod_lock, irq_flags);

    smp_broadcast_task(smp_serialize_icache_cb, NULL);

    if (out_patch_id) *out_patch_id = pid;
    serial_printf("[dynmod] Patched code @ 0x%x (%u bytes, patch_id=%u)\n",
                  (uint32_t)target_addr, (uint32_t)size, pid);
    return DYNMOD_OK;
}

int dynmod_hook_syscall(ipo_uarg_t syscall_num, ipo_syscall_handler_t new_handler,
                        ipo_syscall_handler_t *out_old_handler,
                        ipo_id_t *out_patch_id, ipo_id_t caller_pid) {
    if (!s_dynmod_inited) dynmod_init();
    if (!dynmod_is_authorized(caller_pid)) return DYNMOD_ERR_PERM;
    if (syscall_num == 0u || syscall_num > 0xFFFFu || !new_handler) return DYNMOD_ERR_PARAM;
    if (!validate_range((ipo_vaddr_t)new_handler, 4u, false)) return DYNMOD_ERR_ADDR;

    ipo_reg_t irq_flags = spin_lock_irqsave(&s_dynmod_lock);
    dynmod_entry_t *e = alloc_patch_slot_locked();
    if (!e) {
        spin_unlock_irqrestore(&s_dynmod_lock, irq_flags);
        return DYNMOD_ERR_NOMEM;
    }

    ipo_syscall_handler_t old_fn = ipo_get_syscall_handler(syscall_num);
    e->type = DYNMOD_TYPE_SYSCALL_HOOK;
    e->owner_pid = caller_pid;
    e->target_addr = (ipo_vaddr_t)syscall_num;
    e->replacement_addr = (ipo_vaddr_t)new_handler;
    e->size = sizeof(ipo_vaddr_t);
    e->orig_syscall_fn = old_fn;
    ipo_id_t pid = e->id;
    spin_unlock_irqrestore(&s_dynmod_lock, irq_flags);

    ipo_register_syscall(syscall_num, new_handler);
    if (out_old_handler) *out_old_handler = old_fn;
    if (out_patch_id) *out_patch_id = pid;
    serial_printf("[dynmod] Hooked syscall 0x%04x: old=%p -> new=%p (patch_id=%u)\n",
                  (uint32_t)syscall_num, (void *)old_fn, (void *)new_handler, pid);
    return DYNMOD_OK;
}

int dynmod_restore(ipo_id_t patch_id, ipo_id_t caller_pid) {
    if (!s_dynmod_inited) dynmod_init();
    if (!dynmod_is_authorized(caller_pid)) return DYNMOD_ERR_PERM;
    if (patch_id == 0u) return DYNMOD_ERR_PARAM;

    ipo_reg_t irq_flags = spin_lock_irqsave(&s_dynmod_lock);
    dynmod_entry_t *found = NULL;
    for (uint32_t i = 0; i < s_patch_cap; i++) {
        if (s_patches[i].active && s_patches[i].id == patch_id) {
            found = &s_patches[i];
            break;
        }
    }

    if (!found) {
        spin_unlock_irqrestore(&s_dynmod_lock, irq_flags);
        return DYNMOD_ERR_NOT_FOUND;
    }

    dynmod_type_t type = found->type;
    ipo_vaddr_t target = found->target_addr;
    ipo_size_t sz = found->size;
    ipo_syscall_handler_t orig_sys = found->orig_syscall_fn;

    if ((type == DYNMOD_TYPE_VAR || type == DYNMOD_TYPE_FUNC_HOOK || type == DYNMOD_TYPE_CODE_PATCH) &&
        found->orig_bytes != NULL && sz > 0u) {
        atomic_write_code_bytes(target, found->orig_bytes, sz);
    }

    if (found->orig_bytes) kfree(found->orig_bytes);
    if (found->new_bytes)  kfree(found->new_bytes);
    memset(found, 0, sizeof(*found));
    spin_unlock_irqrestore(&s_dynmod_lock, irq_flags);

    if (type == DYNMOD_TYPE_SYSCALL_HOOK) {
        ipo_register_syscall((ipo_uarg_t)target, orig_sys);
    } else if (type == DYNMOD_TYPE_FUNC_HOOK || type == DYNMOD_TYPE_CODE_PATCH) {
        smp_broadcast_task(smp_serialize_icache_cb, NULL);
    }

    serial_printf("[dynmod] Restored patch_id=%u (type=%u, target=0x%x)\n",
                  patch_id, (uint32_t)type, (uint32_t)target);
    return DYNMOD_OK;
}

ipo_count_t dynmod_list(ipo_dynmod_info_t *out_arr, ipo_count_t max_count) {
    if (!s_dynmod_inited) return 0u;
    ipo_reg_t irq_flags = spin_lock_irqsave(&s_dynmod_lock);
    ipo_count_t count = 0u;
    for (uint32_t i = 0; i < s_patch_cap; i++) {
        if (!s_patches[i].active) continue;
        if (out_arr && count < max_count) {
            ipo_dynmod_info_t *dst = &out_arr[count];
            uint8_t *orig_buf = dst->orig_bytes;
            ipo_size_t orig_cap = dst->orig_cap;
            uint8_t *new_buf  = dst->new_bytes;
            ipo_size_t new_cap  = dst->new_cap;

            dst->id               = s_patches[i].id;
            dst->type             = (uint32_t)s_patches[i].type;
            dst->owner_pid        = s_patches[i].owner_pid;
            dst->target_addr      = s_patches[i].target_addr;
            dst->replacement_addr = s_patches[i].replacement_addr;
            dst->size             = s_patches[i].size;
            dst->orig_bytes       = orig_buf;
            dst->orig_cap         = orig_cap;
            dst->new_bytes        = new_buf;
            dst->new_cap          = new_cap;

            if (s_patches[i].orig_bytes && orig_buf && orig_cap > 0u) {
                ipo_size_t n = (s_patches[i].size < orig_cap) ? s_patches[i].size : orig_cap;
                memcpy(orig_buf, s_patches[i].orig_bytes, n);
            }
            if (s_patches[i].new_bytes && new_buf && new_cap > 0u) {
                ipo_size_t n = (s_patches[i].size < new_cap) ? s_patches[i].size : new_cap;
                memcpy(new_buf, s_patches[i].new_bytes, n);
            }
        }
        count++;
    }
    spin_unlock_irqrestore(&s_dynmod_lock, irq_flags);
    return count;
}
