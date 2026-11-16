#ifndef KERNEL_DYNMOD_H
#define KERNEL_DYNMOD_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <types.h>
#include <syscall.h>

#define DYNMOD_AUTH_TOKEN       0x49504F4Du /* "IPOM" */

#define DYNMOD_OK               0
#define DYNMOD_ERR_PARAM       -1
#define DYNMOD_ERR_ADDR        -2
#define DYNMOD_ERR_NOT_FOUND   -3
#define DYNMOD_ERR_PERM        -4
#define DYNMOD_ERR_NOMEM       -5
#define DYNMOD_ERR_CONFLICT    -6

typedef enum {
    DYNMOD_TYPE_NONE         = 0,
    DYNMOD_TYPE_VAR          = 1,
    DYNMOD_TYPE_FUNC_HOOK    = 2,
    DYNMOD_TYPE_CODE_PATCH   = 3,
    DYNMOD_TYPE_SYSCALL_HOOK = 4
} dynmod_type_t;

typedef struct {
    ipo_id_t    id;
    uint32_t    type;
    ipo_id_t    owner_pid;
    ipo_vaddr_t target_addr;
    ipo_vaddr_t replacement_addr;
    ipo_size_t  size;
    uint8_t     *orig_bytes;
    ipo_size_t  orig_cap;
    uint8_t     *new_bytes;
    ipo_size_t  new_cap;
} ipo_dynmod_info_t;

void        dynmod_init(void);
int         dynmod_authorize(ipo_id_t pid, uint32_t token);
void        dynmod_revoke(ipo_id_t pid);
bool        dynmod_is_authorized(ipo_id_t pid);

int         dynmod_resolve_symbol(const char *name, ipo_vaddr_t *out_addr);
int         dynmod_read_mem(ipo_vaddr_t addr, void *out_buf, size_t size);
int         dynmod_write_var(ipo_vaddr_t addr, const void *in_buf, size_t size,
                             ipo_id_t *out_patch_id, ipo_id_t caller_pid);
int         dynmod_hook_function(ipo_vaddr_t target_fn, ipo_vaddr_t replacement_fn,
                                 ipo_id_t *out_patch_id, ipo_id_t caller_pid);
int         dynmod_patch_code(ipo_vaddr_t target_addr, const void *new_bytes, size_t size,
                              ipo_id_t *out_patch_id, ipo_id_t caller_pid);
int         dynmod_hook_syscall(ipo_uarg_t syscall_num, ipo_syscall_handler_t new_handler,
                                ipo_syscall_handler_t *out_old_handler,
                                ipo_id_t *out_patch_id, ipo_id_t caller_pid);
int         dynmod_restore(ipo_id_t patch_id, ipo_id_t caller_pid);
ipo_count_t dynmod_list(ipo_dynmod_info_t *out_arr, ipo_count_t max_count);

/* Exported test targets for runtime patching/hook checks */
extern volatile uint32_t g_dynmod_target_var;
uint32_t dynmod_target_compute(uint32_t x, uint32_t y);
uint32_t dynmod_alt_compute(uint32_t x, uint32_t y);

#endif /* KERNEL_DYNMOD_H */
