#include <kernel/process.h>
#include <kernel/ipc.h>
#include <syscall.h>
#include <memory/kmalloc.h>
#include <memory/address_space.h>
#include <memory/memory_map.h>
#include <stdio.h>
#include <string.h>

#define PROCESS_THREAD_STACK_SIZE (64u * 1024u)
#define THREAD_SYNC_INITIAL_CAPACITY 8u
#define THREAD_SYNC_MUTEX 1u
#define THREAD_SYNC_SEMAPHORE 2u
#define THREAD_SYNC_CONDITION 3u
#define THREAD_SYNC_INIT 1u
#define THREAD_SYNC_DESTROY 2u
#define THREAD_SYNC_LOCK 3u
#define THREAD_SYNC_UNLOCK 4u
#define THREAD_SYNC_WAIT 5u
#define THREAD_SYNC_POST 6u
#define THREAD_SYNC_COND_WAIT 7u
#define THREAD_SYNC_SIGNAL 8u
#define THREAD_SYNC_BROADCAST 9u

typedef struct {
    process_t *process;
    uintptr_t address;
    uint32_t type;
    uint32_t count;
    process_thread_t *owner;
    process_thread_t *wait_head;
    process_thread_t *wait_tail;
} thread_sync_slot_t;

static thread_sync_slot_t *thread_sync_slots = NULL;
static uint32_t thread_sync_capacity = 0u;

static process_thread_t *thread_find(process_t *proc, uint32_t id) {
    if (!proc || id == 0u) return NULL;
    for (process_thread_t *thread = proc->threads; thread; thread = thread->next) {
        if (thread->id == id) return thread;
    }
    return NULL;
}

bool process_thread_init(process_t *proc) {
    if (!proc || proc->stack_ptr == 0u) return false;
    process_thread_t *main_thread = kmalloc(sizeof(*main_thread));
    if (!main_thread) return false;
    memset(main_thread, 0, sizeof(*main_thread));
    main_thread->id = 1u;
    main_thread->context_esp = proc->stack_ptr;
    main_thread->stack_base = proc->stack_base;
    main_thread->stack_size = proc->stack_size;
    main_thread->stack_virtual_top = 0xC0000000u;
    main_thread->stack_virtual_bottom = proc->stack_start ? proc->stack_start : (0xC0000000u - 4096u);
    main_thread->guard_page_addr = main_thread->stack_virtual_bottom - 4096u;
    main_thread->stack_allocated_pages = proc->stack_size ? (proc->stack_size / 4096u) : 1u;
    main_thread->state = PROCESS_THREAD_RUNNABLE;
    main_thread->is_main = true;
    main_thread->signal_altstack_flags = 2u;
    proc->threads = main_thread;
    proc->current_thread = main_thread;
    proc->next_thread_id = 2u;
    return true;
}

bool process_thread_has_runnable(process_t *proc) {
    if (!proc) return false;
    for (process_thread_t *thread = proc->threads; thread; thread = thread->next) {
        if (thread->state == PROCESS_THREAD_RUNNABLE) return true;
    }
    return false;
}

bool process_thread_handle_stack_fault(process_t *proc,
                                       process_thread_t *thread,
                                       uint32_t fault_address,
                                       uint32_t error_code,
                                       uint32_t saved_esp) {
    if (!proc || !proc->address_space || !thread) return false;

    /* Bit 0: 0=not present, 1=protection violation */
    if ((error_code & 1u) != 0u) {
        return false;
    }

    uint32_t fault_page = fault_address & ~0xFFFu;

    /* If the faulting page is already within or above the mapped stack, not a growth fault */
    if (fault_page >= thread->stack_virtual_bottom) {
        return false;
    }

    /* Target lowest page to allocate:
     * When code executes `sub esp, N`, `saved_esp` may be lower than `fault_page`
     * (e.g. GCC stack probing `sub $0x1000, %esp; orl $0, 0xffc(%esp)`).
     * Or on `push`, `fault_page` might be below or at `saved_esp`.
     * So the allocated range must cover down to MIN(fault_page, saved_esp & ~0xFFFu). */
    uint32_t esp_page = saved_esp & ~0xFFFu;
    uint32_t target_lowest_page = (esp_page < fault_page) ? esp_page : fault_page;

    /* Safety check: ensure fault is related to stack.
     * The fault must be below thread->stack_virtual_bottom, and:
     * - either fault_address >= saved_esp (access to local vars or probe in new stack frame)
     * - or if fault_address < saved_esp, it must not be an arbitrary jump far below ESP (> 256 MiB) */
    if (fault_address < saved_esp && (saved_esp - fault_address) > 0x10000000u) {
        return false;
    }

    uint32_t alloc_start = target_lowest_page;
    uint32_t alloc_end = thread->stack_virtual_bottom;

    /* Check for collision with lower kernel memory / NULL page */
    if (alloc_start < 0x00100000u) {
        serial_printf("[stack-fault] Virtual limit reached: hit low memory guard (CR2=0x%08x)\n",
                      fault_address);
        return false;
    }

    /* Check collision with process binary image */
    if (proc->binary_base && proc->binary_size > 0u) {
        uint32_t bin_start = (uint32_t)(uintptr_t)proc->binary_base;
        uint32_t bin_end = bin_start + proc->binary_size;
        if (alloc_start < bin_end && alloc_end > bin_start) {
            serial_printf("[stack-fault] Collision with process binary [0x%08x - 0x%08x]\n",
                          bin_start, bin_end);
            return false;
        }
    }

    /* Check collision with user heap */
    if (proc->heap_chunks && proc->heap_chunk_count > 0) {
        for (uint32_t i = 0; i < proc->heap_chunk_count; i++) {
            uint32_t c_base = proc->heap_chunks[i].base;
            uint32_t c_limit = proc->heap_chunks[i].limit;
            if (c_limit > c_base && alloc_start < c_limit && alloc_end > c_base) {
                serial_printf("[stack-fault] Collision with user heap chunk %u [0x%08x - 0x%08x]\n",
                              i, c_base, c_limit);
                return false;
            }
        }
    } else if (proc->user_heap_base && proc->user_heap_limit > proc->user_heap_base) {
        if (alloc_start < proc->user_heap_limit && alloc_end > proc->user_heap_base) {
            serial_printf("[stack-fault] Collision with user heap [0x%08x - 0x%08x]\n",
                          proc->user_heap_base, proc->user_heap_limit);
            return false;
        }
    }

    /* Check collision with other threads' stack ranges in this process */
    for (process_thread_t *other = proc->threads; other; other = other->next) {
        if (other != thread && other->stack_virtual_top != 0u) {
            if (alloc_start < other->stack_virtual_top &&
                alloc_end > other->stack_virtual_bottom) {
                serial_printf("[stack-fault] Collision with thread %u stack [0x%08x - 0x%08x]\n",
                              other->id, other->stack_virtual_bottom, other->stack_virtual_top);
                return false;
            }
        }
    }

    /* Allocate physical frame(s) and map down to target_lowest_page */
    uint32_t start_page = thread->stack_virtual_bottom - 4096u;
    for (uint32_t page = start_page; ; page -= 4096u) {
        void *frame = memory_page_alloc();
        if (!frame) {
            serial_printf("[stack-fault] Physical memory exhausted: failed to allocate page for PID %u TID %u\n",
                          proc->pid, thread->id);
            for (uint32_t p = start_page; p > page; p -= 4096u) {
                uint32_t phys = address_space_get_physical(proc->address_space, p);
                address_space_unmap(proc->address_space, p);
                if (phys) {
                    memory_page_free((void *)(uintptr_t)phys);
                }
                thread->stack_allocated_pages--;
            }
            return false;
        }

        if (!address_space_map(proc->address_space, page,
                               (uint32_t)(uintptr_t)frame,
                               ADDRESS_SPACE_PAGE_PRESENT |
                               ADDRESS_SPACE_PAGE_WRITABLE |
                               ADDRESS_SPACE_PAGE_USER)) {
            memory_page_free(frame);
            serial_printf("[stack-fault] Failed to map page at 0x%08x\n", page);
            for (uint32_t p = start_page; p > page; p -= 4096u) {
                uint32_t phys = address_space_get_physical(proc->address_space, p);
                address_space_unmap(proc->address_space, p);
                if (phys) {
                    memory_page_free((void *)(uintptr_t)phys);
                }
                thread->stack_allocated_pages--;
            }
            return false;
        }

        thread->stack_allocated_pages++;
        if (page == target_lowest_page) {
            break;
        }
    }

    /* Update thread stack range */
    thread->stack_virtual_bottom = target_lowest_page;
    thread->guard_page_addr = target_lowest_page - 4096u;
    thread->stack_size = thread->stack_virtual_top - thread->stack_virtual_bottom;
    if (thread->is_main) {
        proc->stack_start = thread->stack_virtual_bottom;
        proc->stack_size = thread->stack_size;
    }

    /* Unmap the new guard page below so next overflow will fault */
    if (thread->guard_page_addr >= 0x00100000u) {
        address_space_unmap(proc->address_space, thread->guard_page_addr);
    }

    /* Invalidate TLB for the faulting address */
    __asm__ volatile("invlpg (%0)" : : "r"(fault_address) : "memory");

    serial_printf("[stack-fault] Stack grown: pid=%u tid=%u cr2=0x%08x bottom=0x%08x pages=%u\n",
                  proc->pid, thread->id, fault_address, thread->stack_virtual_bottom,
                  thread->stack_allocated_pages);
    return true;
}

process_thread_t *process_thread_pick_next(process_t *proc) {
    if (!proc || !proc->threads) return NULL;
    process_thread_t *start = proc->current_thread ? proc->current_thread->next : proc->threads;
    if (!start) start = proc->threads;
    process_thread_t *thread = start;
    do {
        if (thread->state == PROCESS_THREAD_RUNNABLE) {
            proc->current_thread = thread;
            proc->stack_ptr = thread->context_esp;
            return thread;
        }
        thread = thread->next ? thread->next : proc->threads;
    } while (thread != start);
    return NULL;
}

static void thread_finish(process_thread_t *thread, void *result) {
    if (!thread || thread->state == PROCESS_THREAD_EXITED) return;
    thread->result = result;
    thread->state = PROCESS_THREAD_EXITED;
    if (thread->joiner && thread->joiner->state == PROCESS_THREAD_BLOCKED) {
        thread->joiner->state = PROCESS_THREAD_RUNNABLE;
        thread->joiner = NULL;
    }
}

static void __attribute__((noreturn)) thread_bootstrap(void) {
    process_t *proc = process_get_current();
    process_thread_t *thread = proc ? proc->current_thread : NULL;
    __asm__ volatile("sti" ::: "memory");
    void *result = thread && thread->entry
        ? thread->entry(thread->argument)
        : NULL;
    thread_finish(thread, result);
    for (;;) process_yield_kernel();
}

void process_thread_main_exit(process_t *proc) {
    if (!proc) return;
    for (process_thread_t *thread = proc->threads; thread; thread = thread->next) {
        thread_finish(thread, thread->result);
    }
}

void process_thread_cleanup(process_t *proc) {
    if (!proc) return;
    for (uint32_t i = 0u; i < thread_sync_capacity; i++) {
        if (thread_sync_slots[i].process == proc) {
            memset(&thread_sync_slots[i], 0, sizeof(thread_sync_slots[i]));
        }
    }
    process_thread_t *thread = proc->threads;
    while (thread) {
        process_thread_t *next = thread->next;
        if (proc->address_space && thread->stack_virtual_top != 0u) {
            for (uint32_t page = thread->stack_virtual_bottom;
                 page < thread->stack_virtual_top;
                 page += 4096u) {
                uint32_t phys = address_space_get_physical(proc->address_space, page);
                address_space_unmap(proc->address_space, page);
                if (phys) {
                    memory_page_free((void *)(uintptr_t)phys);
                }
            }
        } else if (!thread->is_main && thread->stack_base) {
            kfree(thread->stack_base);
        }
        kfree(thread);
        thread = next;
    }
    proc->threads = NULL;
    proc->current_thread = NULL;
    proc->next_thread_id = 0u;
}

static int thread_create(process_t *proc, void *(*entry)(void *), void *argument,
                         uint32_t *id_out) {
    if (!proc || !proc->is_running || !entry || !id_out ||
        !proc->binary_base || proc->binary_size == 0u) {
        return -1;
    }
    uintptr_t entry_addr = (uintptr_t)entry;
    uintptr_t binary_start = (uintptr_t)PROCESS_HEAP_START;
    uintptr_t binary_end = binary_start + proc->binary_size;
    uintptr_t loaded_start = (uintptr_t)proc->binary_base;
    uintptr_t loaded_end = loaded_start + proc->binary_size;
    if (entry_addr >= binary_start && entry_addr < binary_end) {
        entry_addr = loaded_start + (entry_addr - binary_start);
    } else if (entry_addr < loaded_start || entry_addr >= loaded_end) {
        return -1;
    }
    uintptr_t argument_addr = (uintptr_t)argument;
    if (argument_addr >= binary_start && argument_addr < binary_end) {
        argument = (void *)(loaded_start + (argument_addr - binary_start));
    }
    process_thread_t *thread = kmalloc(sizeof(*thread));
    if (!thread) return -1;
    memset(thread, 0, sizeof(*thread));

    /* Dynamic virtual-stack allocation (VMA-style).
     *
     * Stack regions for non-main threads are placed in the range
     *   [THREAD_STACK_REGION_END .. 0xC0000000)
     * which is 0xC0000000 - THREAD_STACK_REGION_END bytes of VAS.
     * We pick a free window big enough for the initial guard + one page.
     * The initial window is 2*4096 = 8 KiB (guard + first page).  The
     * window can grow downward without limit later via the page-fault handler.
     *
     * We scan existing threads' virtual_top values to find a gap.
     * stack_virtual_top is aligned to 4 KiB and placed below the previous
     * thread's bottom so regions never overlap.  The highest slot starts
     * just below the kernel boundary (0xC0000000).
     */
#define THREAD_STACK_REGION_END  0x10000000u  /* stay above heaps */
#define THREAD_STACK_INITIAL_WIN (2u * 4096u) /* guard + initial page */

    bool found_slot = false;
    uint32_t new_virtual_top = 0u;

    uint32_t candidate_top = 0xC0000000u;
    while (candidate_top > THREAD_STACK_REGION_END + THREAD_STACK_INITIAL_WIN) {
        uint32_t candidate_bottom = candidate_top - THREAD_STACK_INITIAL_WIN;
        bool collision = false;
        for (process_thread_t *curr = proc->threads; curr; curr = curr->next) {
            if (curr->stack_virtual_top == 0u) continue;
            uint32_t guard_bottom = (curr->stack_virtual_bottom >= 4096u)
                                    ? curr->stack_virtual_bottom - 4096u : 0u;
            if (candidate_bottom < curr->stack_virtual_top &&
                candidate_top    > guard_bottom) {
                candidate_top = guard_bottom;
                collision = true;
                break;
            }
        }
        if (!collision) {
            new_virtual_top = candidate_top;
            found_slot = true;
            break;
        }
    }

    if (!found_slot) {
        serial_printf("[thread] Virtual address space exhausted for PID %u — cannot place new stack\n",
                      proc->pid);
        kfree(thread);
        return -1;
    }

#undef THREAD_STACK_REGION_END
#undef THREAD_STACK_INITIAL_WIN

    uint32_t tid = proc->next_thread_id++;
    while (tid == 0u || thread_find(proc, tid) != NULL) {
        tid = proc->next_thread_id++;
    }

    thread->entry = (void *(*)(void *))entry_addr;
    thread->argument = argument;
    thread->state = PROCESS_THREAD_RUNNABLE;
    thread->signal_altstack_flags = 2u;
    thread->id = tid;

    if (proc->address_space) {
        /* new_virtual_top was found by the VMA scan above */
        uint32_t stack_virtual_top = new_virtual_top;
        uint32_t initial_page = stack_virtual_top - 4096u;
        uint32_t guard_page = initial_page - 4096u;

        void *frame = memory_page_alloc();
        if (!frame) {
            kfree(thread);
            return -1;
        }

        if (!address_space_map(proc->address_space, initial_page,
                               (uint32_t)(uintptr_t)frame,
                               ADDRESS_SPACE_PAGE_PRESENT |
                               ADDRESS_SPACE_PAGE_WRITABLE |
                               ADDRESS_SPACE_PAGE_USER)) {
            memory_page_free(frame);
            kfree(thread);
            return -1;
        }

        address_space_unmap(proc->address_space, guard_page);

        thread->stack_base = frame;
        thread->stack_size = 4096u;
        thread->stack_virtual_top = stack_virtual_top;
        thread->stack_virtual_bottom = initial_page;
        thread->guard_page_addr = guard_page;
        thread->stack_allocated_pages = 1u;

        uint32_t *frame_sp = (uint32_t *)((uintptr_t)frame + 4096u);
        frame_sp = (uint32_t *)((uint32_t)frame_sp & ~15u);
        frame_sp[-1] = (uint32_t)(uintptr_t)thread_bootstrap;
        frame_sp[-2] = 0u;
        frame_sp[-3] = 0u;
        frame_sp[-4] = 0u;
        frame_sp[-5] = 0u;
        frame_sp[-6] = 0x02u;
        uint32_t offset = (uint32_t)((uintptr_t)frame + 4096u - (uintptr_t)&frame_sp[-6]);
        thread->context_esp = stack_virtual_top - offset;
    } else {
        thread->stack_base = kmalloc(PROCESS_THREAD_STACK_SIZE);
        if (!thread->stack_base) {
            kfree(thread);
            return -1;
        }
        thread->stack_size = PROCESS_THREAD_STACK_SIZE;

        uint32_t *sp = (uint32_t *)(((uintptr_t)thread->stack_base +
                                     thread->stack_size) & ~(uintptr_t)15u);
        sp--;
        sp[-1] = (uint32_t)(uintptr_t)thread_bootstrap;
        sp[-2] = 0u;
        sp[-3] = 0u;
        sp[-4] = 0u;
        sp[-5] = 0u;
        sp[-6] = 0x02u;
        thread->context_esp = (uint32_t)(uintptr_t)&sp[-6];
    }

    process_thread_t **tail = &proc->threads;
    while (*tail) tail = &(*tail)->next;
    *tail = thread;
    *id_out = thread->id;
    return 0;
}

static thread_sync_slot_t *sync_find(process_t *proc, uintptr_t address,
                                     uint32_t type) {
    for (uint32_t i = 0u; i < thread_sync_capacity; i++) {
        thread_sync_slot_t *slot = &thread_sync_slots[i];
        if (slot->process == proc && slot->address == address &&
            slot->type == type) return slot;
    }
    return NULL;
}

static thread_sync_slot_t *sync_create(process_t *proc, uintptr_t address,
                                       uint32_t type, uint32_t initial_count) {
    thread_sync_slot_t *slot = sync_find(proc, address, type);
    if (slot) return NULL;
    for (uint32_t i = 0u; i < thread_sync_capacity; i++) {
        if (!thread_sync_slots[i].process) {
            slot = &thread_sync_slots[i];
            memset(slot, 0, sizeof(*slot));
            slot->process = proc;
            slot->address = address;
            slot->type = type;
            slot->count = initial_count;
            return slot;
        }
    }
    uint32_t new_cap = (thread_sync_capacity == 0u) ? THREAD_SYNC_INITIAL_CAPACITY
                                                    : (thread_sync_capacity * 2u);
    if (new_cap <= thread_sync_capacity ||
        new_cap > (SIZE_MAX / sizeof(thread_sync_slot_t))) {
        return NULL;
    }
    thread_sync_slot_t *grown = (thread_sync_slot_t *)krealloc(
        thread_sync_slots, new_cap * sizeof(thread_sync_slot_t));
    if (!grown) return NULL;
    memset(grown + thread_sync_capacity, 0,
           (new_cap - thread_sync_capacity) * sizeof(thread_sync_slot_t));
    thread_sync_slots = grown;
    slot = &thread_sync_slots[thread_sync_capacity];
    thread_sync_capacity = new_cap;
    slot->process = proc;
    slot->address = address;
    slot->type = type;
    slot->count = initial_count;
    return slot;
}

static process_thread_t *sync_dequeue(thread_sync_slot_t *slot) {
    process_thread_t *thread = slot->wait_head;
    if (!thread) return NULL;
    slot->wait_head = thread->wait_next;
    if (!slot->wait_head) slot->wait_tail = NULL;
    thread->wait_next = NULL;
    return thread;
}

static void sync_enqueue(thread_sync_slot_t *slot, process_thread_t *thread) {
    thread->wait_next = NULL;
    if (slot->wait_tail) {
        slot->wait_tail->wait_next = thread;
    } else {
        slot->wait_head = thread;
    }
    slot->wait_tail = thread;
}

static void sync_wake_one(thread_sync_slot_t *slot, bool assign_owner) {
    process_thread_t *thread = sync_dequeue(slot);
    if (!thread) return;
    if (assign_owner) {
        slot->owner = thread;
    }
    thread->state = PROCESS_THREAD_RUNNABLE;
}

static bool sync_remove_waiter(thread_sync_slot_t *slot,
                               process_thread_t *target) {
    if (!slot || !target) return false;
    process_thread_t *prev = NULL;
    for (process_thread_t *curr = slot->wait_head; curr; curr = curr->wait_next) {
        if (curr == target) {
            if (prev) prev->wait_next = curr->wait_next;
            else slot->wait_head = curr->wait_next;
            if (slot->wait_tail == curr) slot->wait_tail = prev;
            curr->wait_next = NULL;
            return true;
        }
        prev = curr;
    }
    return false;
}

static void sync_mutex_unlock(thread_sync_slot_t *slot, process_thread_t *thread) {
    if (slot->owner != thread) return;
    slot->owner = NULL;
    sync_wake_one(slot, true);
}

static bool sync_mutex_lock(process_t *proc, uintptr_t mutex_addr,
                            process_thread_t *thread) {
    while (proc->is_running) {
        thread_sync_slot_t *slot = sync_find(proc, mutex_addr, THREAD_SYNC_MUTEX);
        if (!slot || slot->owner == thread) return false;
        if (!slot->owner) {
            slot->owner = thread;
            return true;
        }
        sync_enqueue(slot, thread);
        thread->state = PROCESS_THREAD_BLOCKED;
        process_yield_kernel();
        slot = sync_find(proc, mutex_addr, THREAD_SYNC_MUTEX);
        if (!slot) return false;
        if (slot->owner == thread) return true;
        sync_remove_waiter(slot, thread);
    }
    return false;
}

static uint32_t thread_sync_call(process_t *proc, process_thread_t *current_thread,
                                 uint32_t argc, uint32_t *argv) {
    uint32_t args[4] = {0};
    uint32_t copy_cnt = (argc > 3u) ? 4u : 3u;
    if (!ipc_copy_syscall_args(argc, argv, copy_cnt, args)) return (uint32_t)-1;
    if (args[0] == 0u || !ipc_user_range((const void *)(uintptr_t)args[0], 1u)) {
        return (uint32_t)-1;
    }

    uintptr_t address = (uintptr_t)args[0];
    uint32_t type = args[1];
    uint32_t op = args[2];
    uint32_t extra = (copy_cnt > 3u) ? args[3] : 0u;

    if (type == 0u || type > THREAD_SYNC_CONDITION) return (uint32_t)-1;

    if (op == THREAD_SYNC_INIT) {
        uint32_t initial_count = (type == THREAD_SYNC_SEMAPHORE) ? extra : 0u;
        thread_sync_slot_t *slot = sync_create(proc, address, type, initial_count);
        return slot ? 0u : (uint32_t)-1;
    }

    thread_sync_slot_t *slot = sync_find(proc, address, type);
    if (!slot) return (uint32_t)-1;

    if (op == THREAD_SYNC_DESTROY) {
        if (slot->owner != NULL || slot->wait_head != NULL) return (uint32_t)-1;
        memset(slot, 0, sizeof(*slot));
        return 0u;
    }

    if (type == THREAD_SYNC_MUTEX) {
        if (op == THREAD_SYNC_LOCK) {
            return sync_mutex_lock(proc, address, current_thread) ? 0u : (uint32_t)-1;
        } else if (op == THREAD_SYNC_UNLOCK) {
            if (slot->owner != current_thread) return (uint32_t)-1;
            sync_mutex_unlock(slot, current_thread);
            return 0u;
        }
        return (uint32_t)-1;
    }

    if (type == THREAD_SYNC_SEMAPHORE) {
        if (op == THREAD_SYNC_WAIT) {
            while (proc->is_running) {
                slot = sync_find(proc, address, THREAD_SYNC_SEMAPHORE);
                if (!slot) return (uint32_t)-1;
                if (slot->count > 0u) {
                    slot->count--;
                    return 0u;
                }
                sync_enqueue(slot, current_thread);
                current_thread->state = PROCESS_THREAD_BLOCKED;
                process_yield_kernel();
                slot = sync_find(proc, address, THREAD_SYNC_SEMAPHORE);
                if (!slot) return (uint32_t)-1;
                if (!sync_remove_waiter(slot, current_thread)) {
                    return 0u;
                }
            }
            return (uint32_t)-1;
        } else if (op == THREAD_SYNC_POST) {
            if (slot->wait_head) {
                sync_wake_one(slot, false);
            } else if (slot->count != 0xFFFFFFFFu) {
                slot->count++;
            }
            return 0u;
        }
        return (uint32_t)-1;
    }

    if (op == THREAD_SYNC_COND_WAIT) {
        if (copy_cnt <= 3u || extra == 0u ||
            !ipc_user_range((const void *)(uintptr_t)extra, 1u)) {
            return (uint32_t)-1;
        }
        thread_sync_slot_t *mutex_slot = sync_find(proc, extra, THREAD_SYNC_MUTEX);
        if (!mutex_slot || mutex_slot->owner != current_thread) return (uint32_t)-1;
        sync_enqueue(slot, current_thread);
        current_thread->state = PROCESS_THREAD_BLOCKED;
        sync_mutex_unlock(mutex_slot, current_thread);
        process_yield_kernel();
        thread_sync_slot_t *cond_slot = sync_find(proc, address, THREAD_SYNC_CONDITION);
        if (cond_slot) sync_remove_waiter(cond_slot, current_thread);
        if (!proc->is_running) return (uint32_t)-1;
        return sync_mutex_lock(proc, extra, current_thread) ? 0u : (uint32_t)-1;
    } else if (op == THREAD_SYNC_SIGNAL) {
        sync_wake_one(slot, false);
        return 0u;
    } else if (op == THREAD_SYNC_BROADCAST) {
        while (slot->wait_head) {
            sync_wake_one(slot, false);
        }
        return 0u;
    }

    return (uint32_t)-1;
}

uint32_t process_thread_syscall_handler(uint32_t num, uint32_t argc,
                                       uint32_t *argv) {
    process_t *proc = process_get_current();
    if (!proc || !proc->current_thread) return (uint32_t)-1;

    if (num == IPO_SYSCALL_THREAD_SYNC) {
        return thread_sync_call(proc, proc->current_thread, argc, argv);
    }

    uint32_t args[3] = {0};

    if (num == IPO_SYSCALL_THREAD_CREATE) {
        if (!ipc_copy_syscall_args(argc, argv, 3u, args) ||
            args[0] == 0u || args[2] == 0u ||
            !ipc_user_range((void *)(uintptr_t)args[2], sizeof(uint32_t))) {
            return (uint32_t)-1;
        }
        uint32_t thread_id = 0u;
        if (thread_create(proc, (void *(*)(void *))(uintptr_t)args[0],
                          (void *)(uintptr_t)args[1], &thread_id) != 0) {
            return (uint32_t)-1;
        }
        *(uint32_t *)(uintptr_t)args[2] = thread_id;
        return 0u;
    }

    if (num == IPO_SYSCALL_THREAD_SELF) {
        return proc->current_thread->id;
    }

    if (num == IPO_SYSCALL_THREAD_EXIT) {
        void *result = NULL;
        if (argc > 0u) {
            if (!ipc_copy_syscall_args(argc, argv, 1u, args)) return (uint32_t)-1;
            result = (void *)(uintptr_t)args[0];
        }
        if (proc->current_thread->is_main) {
            proc->exit_code = (int32_t)(uintptr_t)result;
            process_thread_main_exit(proc);
            proc->is_running = false;
        } else {
            thread_finish(proc->current_thread, result);
        }
        process_yield_kernel();
        return 0u;
    }

    if (num == IPO_SYSCALL_THREAD_JOIN) {
        if (!ipc_copy_syscall_args(argc, argv, 2u, args) ||
            args[1] == 0u ||
            !ipc_user_range((void *)(uintptr_t)args[1], sizeof(uint32_t))) {
            return (uint32_t)-1;
        }
        process_thread_t *current = proc->current_thread;
        uint32_t target_id = args[0];
        process_thread_t *target = thread_find(proc, target_id);
        if (!target || target == current || target->is_main ||
            target->join_claimed || current->state != PROCESS_THREAD_RUNNABLE) {
            return (uint32_t)-1;
        }
        target->join_claimed = true;
        while (target->state != PROCESS_THREAD_EXITED) {
            if (proc->is_running) {
                target->joiner = current;
                current->state = PROCESS_THREAD_BLOCKED;
                process_yield_kernel();
            } else {
                break;
            }
        }
        if (target->state != PROCESS_THREAD_EXITED) {
            if (target->joiner == current) target->joiner = NULL;
            target->join_claimed = false;
            return (uint32_t)-1;
        }
        *(uint32_t *)(uintptr_t)args[1] = (uint32_t)(uintptr_t)target->result;
        for (process_thread_t **link = &proc->threads; *link; ) {
            if (*link == target) {
                *link = target->next;
                break;
            }
            link = &(*link)->next;
        }
        if (proc->address_space && target->stack_virtual_top != 0u) {
            for (uint32_t page = target->stack_virtual_bottom;
                 page < target->stack_virtual_top;
                 page += 4096u) {
                uint32_t phys = address_space_get_physical(proc->address_space, page);
                address_space_unmap(proc->address_space, page);
                if (phys) {
                    memory_page_free((void *)(uintptr_t)phys);
                }
            }
        } else if (!target->is_main && target->stack_base) {
            kfree(target->stack_base);
        }
        kfree(target);
        return 0u;
    }

    return (uint32_t)-1;
}
