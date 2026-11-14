#ifndef MEMORY_ADDRESS_SPACE_H
#define MEMORY_ADDRESS_SPACE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct address_space address_space_t;

#define ADDRESS_SPACE_PAGE_PRESENT       0x001u
#define ADDRESS_SPACE_PAGE_WRITABLE      0x002u
#define ADDRESS_SPACE_PAGE_USER          0x004u
#define ADDRESS_SPACE_PAGE_WRITE_THROUGH 0x008u
#define ADDRESS_SPACE_PAGE_CACHE_DISABLE 0x010u
#define ADDRESS_SPACE_PAGE_GLOBAL        0x100u

/*
 * Create an address space by copying the active kernel page directory.
 * The first call must be made while the boot kernel page directory is active
 * and paging is enabled; its kernel mappings are the shared baseline.
 */
address_space_t *address_space_create(void);

/*
 * A space cannot be destroyed while active on this CPU. The caller must also
 * ensure it is not active on another CPU; cross-CPU TLB shootdowns are absent.
 */
bool address_space_destroy(address_space_t *space);

/* Load this space's page directory into CR3. */
bool address_space_activate(address_space_t *space);

/* Restore the boot page directory captured by the first address_space_create. */
bool address_space_activate_kernel(void);

/* Emergency lock release for exception recovery before switching to scheduler. */
void address_space_force_unlock(void);

/*
 * Execute fn(ctx) in target's address space (or boot kernel space if NULL).
 * Safely switches onto a kernel-mapped stack if the current ESP is in user space,
 * copying ctx_size bytes of ctx into kernel-mapped storage across the switch.
 */
void address_space_call_with_copy(address_space_t *target, void (*fn)(void *),
                                  void *ctx, size_t ctx_size);

/*
 * Execute fn(ctx) in target's address space (or boot kernel space if NULL) using
 * caller-supplied kernel-mapped stack top kstack_top (irq/allocation-free).
 */
void address_space_call_on_stack(address_space_t *target, void (*fn)(void *),
                                 void *ctx, void *kstack_top);

/*
 * Map/unmap one aligned 4 KiB page. Map replaces an existing mapping.
 * The physical page remains caller-owned; map flags are hardware PTE flags
 * (PRESENT is always added).
 * This API does not change GDT/TSS setup, privilege transitions, or ring mode.
 */
bool address_space_map(address_space_t *space, uint32_t virtual_address,
                       uint32_t physical_address, uint32_t flags);
bool address_space_unmap(address_space_t *space, uint32_t virtual_address);
bool address_space_set_identity_range(address_space_t *space,
                                      uint32_t virtual_address,
                                      uint32_t length, bool mapped);
bool address_space_range_mapped(address_space_t *space,
                                uint32_t virtual_address, uint32_t length,
                                bool mapped);
uint32_t address_space_get_physical(address_space_t *space, uint32_t virtual_address);

/* Dynamically growing shared kernel trampoline stacks (no fixed byte cap) */
void  address_space_trampoline_init(void);
void *address_space_get_audio_tramp_stack_top(void);
void *address_space_get_copy_tramp_stack_top(void);
bool  address_space_is_trampoline_addr(uint32_t virtual_address);
bool  address_space_handle_trampoline_fault(uint32_t fault_address,
                                            uint32_t error_code,
                                            uint32_t saved_esp);

void paging_init(void);

#endif
