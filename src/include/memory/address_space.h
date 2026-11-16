#ifndef MEMORY_ADDRESS_SPACE_H
#define MEMORY_ADDRESS_SPACE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <types.h>

typedef struct address_space address_space_t;

#define ADDRESS_SPACE_PAGE_PRESENT       0x001u
#define ADDRESS_SPACE_PAGE_WRITABLE      0x002u
#define ADDRESS_SPACE_PAGE_USER          0x004u
#define ADDRESS_SPACE_PAGE_WRITE_THROUGH 0x008u
#define ADDRESS_SPACE_PAGE_CACHE_DISABLE 0x010u
#define ADDRESS_SPACE_PAGE_GLOBAL        0x100u

/*
 * Clone active kernel page directory into a new address space.
 * First call must happen while the boot page directory is active with paging on.
 */
address_space_t *address_space_create(void);

/* Fails if active on this CPU; caller must guarantee no other CPU uses it (no TLB shootdowns). */
bool address_space_destroy(address_space_t *space);

bool address_space_activate(address_space_t *space);

/* Restore boot page directory captured on first address_space_create(). */
bool address_space_activate_kernel(void);

/* Emergency lock break for exception recovery before yielding to scheduler. */
void address_space_force_unlock(void);

/*
 * Run fn(ctx) in target's address space (boot kernel space if NULL).
 * Switches to a kernel stack if current SP is in user space and copies ctx_size bytes across.
 */
void address_space_call_with_copy(address_space_t *target, void (*fn)(void *),
                                  void *ctx, size_t ctx_size);

/* Run fn(ctx) in target's address space on caller-provided kstack_top (no IRQs or allocs). */
void address_space_call_on_stack(address_space_t *target, void (*fn)(void *),
                                 void *ctx, void *kstack_top);

/*
 * Map/unmap one aligned 4 KiB page (map overwrites existing entry).
 * Physical page stays caller-owned; PRESENT is forced on.
 * Does not alter GDT/TSS or privilege ring state.
 */
bool address_space_map(address_space_t *space, ipo_vaddr_t virtual_address,
                       ipo_paddr_t physical_address, ipo_reg_t flags);
bool address_space_unmap(address_space_t *space, ipo_vaddr_t virtual_address);
bool address_space_set_identity_range(address_space_t *space,
                                      ipo_vaddr_t virtual_address,
                                      ipo_size_t length, bool mapped);
bool address_space_range_mapped(address_space_t *space,
                                ipo_vaddr_t virtual_address, ipo_size_t length,
                                bool mapped);
ipo_paddr_t address_space_get_physical(address_space_t *space, ipo_vaddr_t virtual_address);

/* Growable kernel trampoline stacks */
void  address_space_trampoline_init(void);
void *address_space_get_audio_tramp_stack_top(void);
void *address_space_get_copy_tramp_stack_top(void);
bool  address_space_is_trampoline_addr(ipo_vaddr_t virtual_address);
bool  address_space_handle_trampoline_fault(ipo_vaddr_t fault_address,
                                            ipo_reg_t error_code,
                                            ipo_vaddr_t saved_sp);

void paging_init(void);

#endif
