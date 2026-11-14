#include <kernel/process.h>
#include <kernel/ipc.h>
#include <ipc.h>
#include <syscall.h>
#include <memory/kmalloc.h>
#include <string.h>

#define IPO_SIGNAL_COUNT 65u
#define IPO_SIG_DFL 0u
#define IPO_SIG_IGN 1u
#define IPO_SIGKILL 9u
#define IPO_SIGSTOP 19u
#define IPO_SIGCONT 18u
#define IPO_SIGCHLD 17u
#define IPO_SIGURG 23u
#define IPO_SIGWINCH 28u
#define IPO_SIG_BLOCK 0u
#define IPO_SIG_UNBLOCK 1u
#define IPO_SIG_SETMASK 2u

#define IPO_SA_SIGINFO   0x00000004u
#define IPO_SA_ONSTACK   0x08000000u
#define IPO_SA_NODEFER   0x40000000u
#define IPO_SA_RESETHAND 0x80000000u
#define IPO_SS_ONSTACK 1u
#define IPO_SS_DISABLE 2u
#define IPO_MINSIGSTKSZ 2048u
#define IPO_SI_USER 0
#define IPO_SI_KERNEL 128

typedef struct signal_extension {
    uint32_t pid;
    uint32_t *action_flags;
    uint32_t *sender_pid;
    uint32_t action_capacity;
    uint32_t altstack_base;
    uint32_t altstack_size;
    uint32_t altstack_flags;
    bool altstack_active;
    struct signal_extension *next;
} signal_extension_t;

typedef struct {
    int32_t signo;
    int32_t error;
    int32_t code;
    union {
        void *address;
        struct {
            int32_t pid;
            uint32_t uid;
        } sender;
    };
} ipo_siginfo_t;

typedef struct {
    uint32_t sp;
    int32_t flags;
    uint32_t size;
} ipo_stack_t;

static signal_extension_t *signal_extensions;

extern void ipo_signal_call_on_stack(uintptr_t handler, int signal,
                                     const void *info, const void *context,
                                     uintptr_t stack_top);

static bool signal_arrays_grow(uintptr_t **handlers, uint64_t **masks,
                               uint32_t *capacity, uint32_t required) {
    if (required <= *capacity) return true;
    uint32_t new_capacity = *capacity ? *capacity : 8u;
    while (new_capacity < required) {
        if (new_capacity > UINT32_MAX / 2u) {
            new_capacity = required;
            break;
        }
        new_capacity *= 2u;
    }
    if ((size_t)new_capacity > SIZE_MAX / sizeof(**handlers) ||
        (size_t)new_capacity > SIZE_MAX / sizeof(**masks))
        return false;

    uintptr_t *new_handlers =
        (uintptr_t *)kmalloc((size_t)new_capacity * sizeof(*new_handlers));
    uint64_t *new_masks =
        (uint64_t *)kmalloc((size_t)new_capacity * sizeof(*new_masks));
    if (!new_handlers || !new_masks) {
        if (new_handlers) kfree(new_handlers);
        if (new_masks) kfree(new_masks);
        return false;
    }
    memset(new_handlers, 0, (size_t)new_capacity * sizeof(*new_handlers));
    memset(new_masks, 0, (size_t)new_capacity * sizeof(*new_masks));
    if (*capacity != 0u) {
        memcpy(new_handlers, *handlers,
               (size_t)*capacity * sizeof(*new_handlers));
        memcpy(new_masks, *masks, (size_t)*capacity * sizeof(*new_masks));
    }
    if (*handlers) kfree(*handlers);
    if (*masks) kfree(*masks);
    *handlers = new_handlers;
    *masks = new_masks;
    *capacity = new_capacity;
    return true;
}

static bool signal_process_actions_ensure(process_t *proc, uint32_t signal) {
    if (!proc || signal >= IPO_SIGNAL_COUNT) return false;
    return signal_arrays_grow(&proc->signal_handlers,
                              &proc->signal_action_masks,
                              &proc->signal_action_capacity,
                              signal + 1u);
}

static uintptr_t signal_handler_get(const process_t *proc, uint32_t signal) {
    return proc && signal < proc->signal_action_capacity &&
           proc->signal_handlers ? proc->signal_handlers[signal] : IPO_SIG_DFL;
}

static uint64_t signal_action_mask_get(const process_t *proc,
                                       uint32_t signal) {
    return proc && signal < proc->signal_action_capacity &&
           proc->signal_action_masks ? proc->signal_action_masks[signal] : 0ULL;
}

static bool signal_extension_actions_ensure(signal_extension_t *state,
                                            uint32_t signal) {
    if (!state || signal >= IPO_SIGNAL_COUNT) return false;
    if (signal < state->action_capacity) return true;
    uint32_t new_capacity = state->action_capacity
        ? state->action_capacity : 8u;
    while (new_capacity <= signal) {
        if (new_capacity > UINT32_MAX / 2u) {
            new_capacity = signal + 1u;
            break;
        }
        new_capacity *= 2u;
    }
    if ((size_t)new_capacity > SIZE_MAX / sizeof(uint32_t)) return false;
    uint32_t *new_flags =
        (uint32_t *)kmalloc((size_t)new_capacity * sizeof(*new_flags));
    uint32_t *new_senders =
        (uint32_t *)kmalloc((size_t)new_capacity * sizeof(*new_senders));
    if (!new_flags || !new_senders) {
        if (new_flags) kfree(new_flags);
        if (new_senders) kfree(new_senders);
        return false;
    }
    memset(new_flags, 0, (size_t)new_capacity * sizeof(*new_flags));
    memset(new_senders, 0, (size_t)new_capacity * sizeof(*new_senders));
    if (state->action_capacity != 0u) {
        memcpy(new_flags, state->action_flags,
               (size_t)state->action_capacity * sizeof(*new_flags));
        memcpy(new_senders, state->sender_pid,
               (size_t)state->action_capacity * sizeof(*new_senders));
    }
    if (state->action_flags) kfree(state->action_flags);
    if (state->sender_pid) kfree(state->sender_pid);
    state->action_flags = new_flags;
    state->sender_pid = new_senders;
    state->action_capacity = new_capacity;
    return true;
}

static void signal_extension_free(signal_extension_t *state) {
    if (!state) return;
    if (state->action_flags) kfree(state->action_flags);
    if (state->sender_pid) kfree(state->sender_pid);
    kfree(state);
}

static signal_extension_t *signal_state(process_t *proc, bool create) {
    signal_extension_t **link = &signal_extensions;
    while (*link) {
        signal_extension_t *state = *link;
        if (!process_is_alive(state->pid)) {
            *link = state->next;
            signal_extension_free(state);
            continue;
        }
        if (state->pid == proc->pid) return state;
        link = &state->next;
    }

    if (!create) return NULL;
    signal_extension_t *state = kmalloc(sizeof(*state));
    if (!state) return NULL;
    memset(state, 0, sizeof(*state));
    state->pid = proc->pid;
    state->altstack_flags = IPO_SS_DISABLE;
    state->next = signal_extensions;
    signal_extensions = state;
    return state;
}

static uint64_t *signal_mask_address(process_t *proc) {
    return proc->current_thread ? &proc->current_thread->signal_mask
                                : &proc->signal_mask;
}

static bool signal_altstack_get(process_t *proc, process_thread_t *thread,
                                ipo_stack_t *stack, bool *active) {
    if (thread) {
        stack->sp = thread->signal_altstack_base;
        stack->flags = thread->signal_altstack_active ? (int32_t)IPO_SS_ONSTACK :
                       (int32_t)thread->signal_altstack_flags;
        stack->size = thread->signal_altstack_size;
        if (active) *active = thread->signal_altstack_active;
        return true;
    }

    signal_extension_t *state = signal_state(proc, false);
    stack->sp = state ? state->altstack_base : 0u;
    stack->flags = state && state->altstack_active ? (int32_t)IPO_SS_ONSTACK :
                   (int32_t)(state ? state->altstack_flags : IPO_SS_DISABLE);
    stack->size = state ? state->altstack_size : 0u;
    if (active) *active = state && state->altstack_active;
    return true;
}

static bool signal_altstack_set(process_t *proc, process_thread_t *thread,
                                const ipo_stack_t *stack) {
    if (thread) {
        if (stack->flags == IPO_SS_DISABLE) {
            thread->signal_altstack_base = 0u;
            thread->signal_altstack_size = 0u;
            thread->signal_altstack_flags = IPO_SS_DISABLE;
        } else {
            thread->signal_altstack_base = stack->sp;
            thread->signal_altstack_size = stack->size;
            thread->signal_altstack_flags = 0u;
        }
        return true;
    }

    signal_extension_t *state = signal_state(proc, true);
    if (!state) return false;
    if (stack->flags == IPO_SS_DISABLE) {
        state->altstack_base = 0u;
        state->altstack_size = 0u;
        state->altstack_flags = IPO_SS_DISABLE;
    } else {
        state->altstack_base = stack->sp;
        state->altstack_size = stack->size;
        state->altstack_flags = 0u;
    }
    return true;
}

static bool signal_altstack_is_active(process_t *proc, process_thread_t *thread) {
    if (thread) {
        return thread->signal_altstack_active;
    }
    signal_extension_t *state = signal_state(proc, false);
    return state && state->altstack_active;
}

static void signal_altstack_set_active(process_t *proc, process_thread_t *thread,
                                       bool active) {
    if (thread) {
        thread->signal_altstack_active = active;
    } else {
        signal_extension_t *state = signal_state(proc, active);
        if (state) state->altstack_active = active;
    }
}

void process_signal_cleanup(uint32_t pid) {
    if (pid == 0u) return;
    process_t *proc = process_find_by_pid(pid);
    if (proc) {
        if (proc->signal_handlers) kfree(proc->signal_handlers);
        if (proc->signal_action_masks) kfree(proc->signal_action_masks);
        proc->signal_handlers = NULL;
        proc->signal_action_masks = NULL;
        proc->signal_action_capacity = 0u;
    }
    signal_extension_t **link = &signal_extensions;
    while (*link) {
        signal_extension_t *state = *link;
        if (state->pid == pid) {
            *link = state->next;
            signal_extension_free(state);
            return;
        }
        link = &state->next;
    }
}

static bool signal_altstack_valid(const ipo_stack_t *stack) {
    if (stack->flags == IPO_SS_DISABLE) return true;
    if (stack->flags != 0) return false;
    if (stack->size < IPO_MINSIGSTKSZ) return false;
    if (stack->sp + stack->size < stack->sp) return false;
    return ipc_user_range((const void *)(uintptr_t)stack->sp, stack->size);
}

static inline bool signal_number_valid(uint32_t signal) {
    return signal > 0u && signal < IPO_SIGNAL_COUNT;
}

static inline uint64_t signal_bit(uint32_t signal) {
    return (signal > 0u && signal < 65u) ? (1ULL << (signal - 1u)) : 0ULL;
}

static inline bool signal_default_ignored(uint32_t signal) {
    return signal == IPO_SIGCHLD || signal == IPO_SIGURG ||
           signal == IPO_SIGWINCH || signal == IPO_SIGCONT;
}

static void signal_wake_target(process_t *target) {
    if (!target) return;
    if (target->ipc_waiting) {
        target->ipc_waiting = false;
        target->ipc_wait_cancelled = true;
    }
    for (process_thread_t *thread = target->threads; thread;
         thread = thread->next) {
        if (thread->ipc_waiting) {
            thread->ipc_waiting = false;
            thread->ipc_wait_cancelled = true;
        }
        if (thread->state == PROCESS_THREAD_BLOCKED) {
            thread->state = PROCESS_THREAD_RUNNABLE;
        }
    }
}

static bool signal_queue(process_t *target, uint32_t signal,
                         uint32_t sender_pid) {
    if (signal == IPO_SIGCONT) {
        target->signal_stopped = false;
        uintptr_t handler = signal_handler_get(target, signal);
        if (handler == IPO_SIG_DFL || handler == IPO_SIG_IGN)
            return true;
    }
    if (signal == IPO_SIGSTOP) {
        target->signal_stopped = true;
        return true;
    }
    if (signal == IPO_SIGKILL) {
        target->exit_code = 128 + (int)signal;
        target->is_running = 0;
        signal_wake_target(target);
        ipc_process_terminated(target->pid);
        return true;
    }

    signal_extension_t *state = signal_state(target, true);
    if (!state || !signal_extension_actions_ensure(state, signal))
        return false;

    uint64_t bit = signal_bit(signal);
    bool was_pending = (target->signal_pending & bit) != 0u;
    target->signal_pending |= bit;
    if (!was_pending) state->sender_pid[signal] = sender_pid;
    signal_wake_target(target);
    return true;
}

static int signal_send(int pid, uint32_t signal) {
    if (pid < 0 || signal >= IPO_SIGNAL_COUNT) return -1;
    process_t *curr = process_get_current();
    process_t *target = (pid == 0) ? curr : process_find_by_pid((uint32_t)pid);
    if (!target || (!target->is_running && signal != 0u)) return -2;
    if (signal == 0u) return 0;
    uint32_t sender_pid = curr ? curr->pid : 0u;
    if (!signal_queue(target, signal, sender_pid))
        return (int)IPO_IPC_ERR_NO_SPACE;
    return 0;
}

static uint64_t signal_unblockable_mask(void) {
    return signal_bit(IPO_SIGKILL) | signal_bit(IPO_SIGSTOP);
}

static uint32_t signal_apply_action(process_t *current, uint32_t signal,
                                    uintptr_t new_handler, uint32_t new_mask,
                                    uint32_t new_flags, bool need_old,
                                    bool apply, uintptr_t *out_handler,
                                    uint32_t *out_mask, uint32_t *out_flags) {
    signal_extension_t *state = signal_state(current, apply || need_old);
    if (apply) {
        if (!signal_process_actions_ensure(current, signal))
            return (uint32_t)IPO_IPC_ERR_NO_SPACE;
        if (new_handler > IPO_SIG_IGN &&
            !ipc_user_range((const void *)new_handler, 1u))
            return (uint32_t)IPO_IPC_ERR_INVALID;
        if (!state || !signal_extension_actions_ensure(state, signal))
            return (uint32_t)IPO_IPC_ERR_NO_SPACE;
    }

    if (out_handler) *out_handler = signal_handler_get(current, signal);
    if (out_mask) *out_mask = (uint32_t)signal_action_mask_get(current, signal);
    if (out_flags) {
        *out_flags = (state && signal < state->action_capacity)
            ? state->action_flags[signal] : 0u;
    }

    if (apply) {
        uint64_t masked = (uint64_t)new_mask & ~signal_unblockable_mask();
        current->signal_handlers[signal] = new_handler;
        current->signal_action_masks[signal] = masked;
        state->action_flags[signal] = new_flags;
    }
    return 0u;
}

static uint32_t signal_dispatch_impl(process_t *current, uint32_t num,
                                      uint32_t argc, uint32_t *argv,
                                      uint32_t *args) {
    if (num == IPO_SYSCALL_KILL) {
        if (!ipc_copy_syscall_args(argc, argv, 2u, args)) return (uint32_t)-1;
        return (uint32_t)signal_send((int)args[0], args[1]);
    }

    if (num == IPO_SYSCALL_SIGACTION) {
        uint32_t count = (argc > 7u) ? 8u : 6u;
        if (!ipc_copy_syscall_args(argc, argv, count, args)) return (uint32_t)-1;
        uint32_t signal = args[0];
        if (!signal_number_valid(signal) || signal == IPO_SIGKILL ||
            signal == IPO_SIGSTOP)
            return (uint32_t)-1;

        uintptr_t new_handler = args[1];
        uint32_t new_mask = args[2];
        uintptr_t old_handler = 0u;
        uint32_t old_mask = 0u;
        uint32_t old_flags = 0u;

        if (count >= 8u) {
            uintptr_t old_h_ptr = args[3];
            uintptr_t old_m_ptr = args[4];
            uint32_t new_flags = args[5];
            uintptr_t old_f_ptr = args[6];
            bool apply = (args[7] != 0u);

            if ((old_h_ptr != 0u &&
                 !ipc_user_range((void *)old_h_ptr, sizeof(uint32_t))) ||
                (old_m_ptr != 0u &&
                 !ipc_user_range((void *)old_m_ptr, sizeof(uint32_t))) ||
                (old_f_ptr != 0u &&
                 !ipc_user_range((void *)old_f_ptr, sizeof(uint32_t))))
                return (uint32_t)IPO_IPC_ERR_INVALID;

            bool need_old = (old_h_ptr != 0u || old_m_ptr != 0u || old_f_ptr != 0u);
            uint32_t err = signal_apply_action(current, signal, new_handler,
                                               new_mask, new_flags, need_old, apply,
                                               &old_handler, &old_mask, &old_flags);
            if (err != 0u) return err;

            if (old_h_ptr != 0u) *(uint32_t *)old_h_ptr = (uint32_t)old_handler;
            if (old_m_ptr != 0u) *(uint32_t *)old_m_ptr = old_mask;
            if (old_f_ptr != 0u) *(uint32_t *)old_f_ptr = old_flags;
        } else {
            uint32_t new_flags = args[3];
            uintptr_t oldact_ptr = args[4];
            bool apply = (args[5] != 0u);

            if (oldact_ptr != 0u &&
                !ipc_user_range((void *)oldact_ptr, sizeof(uint32_t)))
                return (uint32_t)IPO_IPC_ERR_INVALID;

            uint32_t err = signal_apply_action(current, signal, new_handler,
                                               new_mask, new_flags, oldact_ptr != 0u,
                                               apply, &old_handler, &old_mask, &old_flags);
            if (err != 0u) return err;

            if (oldact_ptr != 0u) {
                *(uint32_t *)oldact_ptr = (uint32_t)old_handler;
                if (ipc_user_range((void *)oldact_ptr, sizeof(uint32_t) * 2u)) {
                    *(uint32_t *)(oldact_ptr + 4u) = old_mask;
                }
                if (ipc_user_range((void *)oldact_ptr, sizeof(uint32_t) * 3u)) {
                    *(uint32_t *)(oldact_ptr + 8u) = old_flags;
                }
            }
        }
        return 0u;
    }

    if (num == IPO_SYSCALL_SIGPROCMASK) {
        if (!ipc_copy_syscall_args(argc, argv, 3u, args)) return (uint32_t)-1;
        uint32_t how = args[0];
        if (how > IPO_SIG_SETMASK) return (uint32_t)-1;
        if (args[2] != 0u &&
            !ipc_user_range((void *)(uintptr_t)args[2], sizeof(uint32_t)))
            return (uint32_t)-1;

        uint32_t set_mask = 0u;
        if (args[1] != 0u) {
            if (!ipc_user_range((const void *)(uintptr_t)args[1], sizeof(uint32_t)))
                return (uint32_t)-1;
            set_mask = *(const uint32_t *)(uintptr_t)args[1];
        }
        uint64_t masked_set = (uint64_t)set_mask & ~signal_unblockable_mask();
        uint64_t *mask_ptr = signal_mask_address(current);
        if (args[2] != 0u)
            *(uint32_t *)(uintptr_t)args[2] = (uint32_t)*mask_ptr;
        if (args[1] != 0u) {
            if (how == IPO_SIG_BLOCK) *mask_ptr |= masked_set;
            else if (how == IPO_SIG_UNBLOCK) *mask_ptr &= ~masked_set;
            else if (how == IPO_SIG_SETMASK) {
                *mask_ptr = (*mask_ptr & ~0xFFFFFFFFULL) | masked_set;
            }
        }
        return 0u;
    }

    if (num == IPO_SYSCALL_SIGALTSTACK) {
        if (!ipc_copy_syscall_args(argc, argv, 2u, args)) return (uint32_t)-1;
        if (args[1] != 0u &&
            !ipc_user_range((void *)(uintptr_t)args[1], sizeof(ipo_stack_t)))
            return (uint32_t)-1;
        if (args[0] != 0u &&
            !ipc_user_range((const void *)(uintptr_t)args[0], sizeof(ipo_stack_t)))
            return (uint32_t)-1;

        process_thread_t *thread = current->current_thread;
        if (args[1] != 0u) {
            ipo_stack_t old_stk;
            signal_altstack_get(current, thread, &old_stk, NULL);
            *(ipo_stack_t *)(uintptr_t)args[1] = old_stk;
        }
        if (args[0] != 0u) {
            ipo_stack_t new_stk = *(const ipo_stack_t *)(uintptr_t)args[0];
            if (signal_altstack_is_active(current, thread)) return (uint32_t)-1;
            if (!signal_altstack_valid(&new_stk)) return (uint32_t)-1;
            if (!signal_altstack_set(current, thread, &new_stk))
                return (uint32_t)IPO_IPC_ERR_NO_SPACE;
        }
        return 0u;
    }

    return (uint32_t)-1;
}

uint32_t process_signal_syscall_handler(uint32_t num, uint32_t argc,
                                       uint32_t *argv) {
    process_t *current = process_get_current();
    if (!current) return (uint32_t)-1;

    uint32_t args[8] = {0};
    return signal_dispatch_impl(current, num, argc, argv, args);
}

void process_signal_deliver_pending(void) {
    process_t *current = process_get_current();
    if (!current || !current->is_running) return;

    process_thread_t *thread = current->current_thread;
    uint64_t *signal_mask = signal_mask_address(current);
    uint64_t deliverable = current->signal_pending & ~(*signal_mask);
    if (deliverable == 0u) return;

    uint32_t signal = 1u;
    while (signal < IPO_SIGNAL_COUNT && (signal_bit(signal) & deliverable) == 0u) {
        signal++;
    }
    if (!signal_number_valid(signal)) return;

    uint64_t bit = signal_bit(signal);
    current->signal_pending &= ~bit;
    uintptr_t handler = signal_handler_get(current, signal);
    if (handler == IPO_SIG_IGN || signal_default_ignored(signal)) return;
    if (handler == IPO_SIG_DFL) {
        current->exit_code = 128 + (int)signal;
        current->is_running = false;
        ipc_process_terminated(current->pid);
        return;
    }

    if (!ipc_user_range((const void *)handler, 1u)) {
        current->exit_code = 128 + 11;
        current->is_running = false;
        ipc_process_terminated(current->pid);
        return;
    }

    signal_extension_t *state = signal_state(current, false);
    uint32_t action_flags = state && signal < state->action_capacity
        ? state->action_flags[signal] : 0u;
    uint64_t previous_mask = *signal_mask;
    *signal_mask |= signal_action_mask_get(current, signal);
    if ((action_flags & IPO_SA_NODEFER) == 0u)
        *signal_mask |= bit;
    if ((action_flags & IPO_SA_RESETHAND) != 0u) {
        if (current->signal_handlers)
            current->signal_handlers[signal] = IPO_SIG_DFL;
        if (current->signal_action_masks)
            current->signal_action_masks[signal] = 0u;
        if (state && signal < state->action_capacity)
            state->action_flags[signal] = 0u;
    }

    ipo_siginfo_t info = {
        .signo = (int32_t)signal,
        .error = 0,
        .code = state && signal < state->action_capacity &&
                state->sender_pid[signal] != 0u
            ? IPO_SI_USER : IPO_SI_KERNEL,
        .sender.pid = state && signal < state->action_capacity
            ? (int32_t)state->sender_pid[signal] : 0,
        .sender.uid = 0u
    };
    if (state && signal < state->action_capacity)
        state->sender_pid[signal] = 0u;
    bool previous_delivering = current->signal_delivering;
    current->signal_delivering = true;
    ipo_stack_t alternate_stack;
    bool alternate_stack_active = false;
    signal_altstack_get(current, thread, &alternate_stack, &alternate_stack_active);

    if ((action_flags & IPO_SA_ONSTACK) != 0u &&
        alternate_stack.sp != 0u &&
        alternate_stack.size >= IPO_MINSIGSTKSZ &&
        (alternate_stack.flags & (int32_t)IPO_SS_DISABLE) == 0 &&
        !alternate_stack_active) {
        uintptr_t stack_top = (alternate_stack.sp + alternate_stack.size) & ~(uintptr_t)15u;
        signal_altstack_set_active(current, thread, true);
        if ((action_flags & IPO_SA_SIGINFO) != 0u) {
            ipo_signal_call_on_stack(handler, (int)signal, &info, NULL, stack_top);
        } else {
            ipo_signal_call_on_stack(handler, (int)signal, NULL, NULL, stack_top);
        }
        signal_altstack_set_active(current, thread, false);
    } else {
        if ((action_flags & IPO_SA_SIGINFO) != 0u) {
            void (*siginfo_handler)(int, ipo_siginfo_t *, void *) =
                (void (*)(int, ipo_siginfo_t *, void *))handler;
            siginfo_handler((int)signal, &info, NULL);
        } else {
            void (*standard_handler)(int) = (void (*)(int))handler;
            standard_handler((int)signal);
        }
    }

    current->signal_delivering = previous_delivering;
    *signal_mask_address(current) = previous_mask;
}
