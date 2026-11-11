#include <kernel/process.h>
#include <syscall.h>
#include <memory/kmalloc.h>
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
    if (!proc || !proc->stack_base || proc->stack_size == 0u) return false;
    process_thread_t *main_thread = kmalloc(sizeof(*main_thread));
    if (!main_thread) return false;
    memset(main_thread, 0, sizeof(*main_thread));
    main_thread->id = 1u;
    main_thread->context_esp = proc->stack_ptr;
    main_thread->stack_base = proc->stack_base;
    main_thread->stack_size = proc->stack_size;
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
        if (!thread->is_main && thread->stack_base) kfree(thread->stack_base);
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
    thread->stack_base = kmalloc(PROCESS_THREAD_STACK_SIZE);
    if (!thread->stack_base) {
        kfree(thread);
        return -1;
    }
    thread->stack_size = PROCESS_THREAD_STACK_SIZE;
    thread->entry = (void *(*)(void *))entry_addr;
    thread->argument = argument;
    thread->state = PROCESS_THREAD_RUNNABLE;
    thread->signal_altstack_flags = 2u;
    thread->id = proc->next_thread_id++;
    if (thread->id == 0u) thread->id = proc->next_thread_id++;

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

static void sync_mutex_unlock(thread_sync_slot_t *slot, process_thread_t *thread) {
    if (slot->owner != thread) return;
    slot->owner = NULL;
    sync_wake_one(slot, true);
}

static bool sync_mutex_lock(thread_sync_slot_t *slot, process_thread_t *thread) {
    if (!slot->owner) {
        slot->owner = thread;
        return true;
    }
    if (slot->owner == thread) {
        return false;
    }
    sync_enqueue(slot, thread);
    thread->state = PROCESS_THREAD_BLOCKED;
    process_yield_kernel();
    return slot->owner == thread;
}

static uint32_t thread_sync_call(process_t *proc, process_thread_t *current_thread,
                                 uint32_t argc, uint32_t *argv) {
    if (argc <= 2u || !argv || argv[0] == 0u) return (uint32_t)-1;
    uintptr_t address = (uintptr_t)argv[0];
    uint32_t type = argv[1];
    uint32_t op = argv[2];
    uint32_t extra = (argc > 3u) ? argv[3] : 0u;

    if (type == 0u || type > 3u) return (uint32_t)-1;

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
            return sync_mutex_lock(slot, current_thread) ? 0u : (uint32_t)-1;
        } else if (op == THREAD_SYNC_UNLOCK) {
            if (slot->owner != current_thread) return (uint32_t)-1;
            sync_mutex_unlock(slot, current_thread);
            return 0u;
        }
        return (uint32_t)-1;
    }

    if (type == THREAD_SYNC_SEMAPHORE) {
        if (op == THREAD_SYNC_WAIT) {
            if (slot->count > 0u) {
                slot->count--;
                return 0u;
            }
            sync_enqueue(slot, current_thread);
            current_thread->state = PROCESS_THREAD_BLOCKED;
            process_yield_kernel();
            return 0u;
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
        if (argc <= 3u || extra == 0u) return (uint32_t)-1;
        thread_sync_slot_t *mutex_slot = sync_find(proc, extra, THREAD_SYNC_MUTEX);
        if (!mutex_slot || mutex_slot->owner != current_thread) return (uint32_t)-1;
        sync_enqueue(slot, current_thread);
        current_thread->state = PROCESS_THREAD_BLOCKED;
        sync_mutex_unlock(mutex_slot, current_thread);
        process_yield_kernel();
        return sync_mutex_lock(mutex_slot, current_thread) ? 0u : (uint32_t)-1;
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

    if (num == IPO_SYSCALL_THREAD_CREATE) {
        if (argc <= 2u || !argv || argv[0] == 0u || argv[2] == 0u) {
            return (uint32_t)-1;
        }
        uint32_t thread_id = 0u;
        if (thread_create(proc, (void *(*)(void *))(uintptr_t)argv[0],
                          (void *)(uintptr_t)argv[1], &thread_id) != 0) {
            return (uint32_t)-1;
        }
        *(uint32_t *)(uintptr_t)argv[2] = thread_id;
        return 0u;
    }

    if (num == IPO_SYSCALL_THREAD_SELF) {
        return proc->current_thread->id;
    }

    if (num == IPO_SYSCALL_THREAD_EXIT) {
        void *result = (argc > 0u && argv) ? (void *)(uintptr_t)argv[0] : NULL;
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
        if (argc <= 1u || !argv || argv[1] == 0u) return (uint32_t)-1;
        process_thread_t *current = proc->current_thread;
        uint32_t target_id = argv[0];
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
        if (target->state != PROCESS_THREAD_EXITED) return (uint32_t)-1;
        *(uint32_t *)(uintptr_t)argv[1] = (uint32_t)(uintptr_t)target->result;
        for (process_thread_t **link = &proc->threads; *link; ) {
            if (*link == target) {
                *link = target->next;
                break;
            }
            link = &(*link)->next;
        }
        if (target->stack_base) kfree(target->stack_base);
        kfree(target);
        return 0u;
    }

    return (uint32_t)-1;
}
