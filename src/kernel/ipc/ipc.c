#include <kernel/ipc.h>
#include <kernel/input_ext.h>
#include <kernel/process.h>
#include <kernel/smp.h>
#include <ipc.h>
#include <memory/kmalloc.h>
#include <memory/address_space.h>
#include <syscall.h>
#include <stdio.h>
#include <string.h>

#define IPC_INITIAL_ENDPOINT_CAPACITY 8u
#define IPC_INITIAL_CLIENT_CAPACITY 4u
#define IPC_ENDPOINT_GENERATION_MAX 0x000FFFFFu
#define IPC_WAIT_SEND 1u
#define IPC_WAIT_RECEIVE 2u
#define IPC_WAIT_ANY 0u
#define IPC_INITIAL_QUEUE_CAPACITY 8u

typedef struct {
    uint32_t pid;
    uint32_t rights;
} ipc_client_t;

typedef struct {
    uint32_t sender_pid;
    uint32_t length;
    uint8_t *data;
} ipc_message_t;

typedef struct {
    bool active;
    bool retired;
    uint32_t id;
    uint32_t generation;
    uint32_t owner_pid;
    uint32_t public_rights;
    char *name;
    ipc_client_t *clients;
    uint32_t client_capacity;
    ipc_message_t *messages;
    uint32_t message_capacity;
    uint32_t head;
    uint32_t count;
} ipc_endpoint_t;

static spinlock_t ipc_lock;
static ipc_endpoint_t *endpoints;
static uint32_t endpoint_capacity;
static bool ipc_ready;

bool ipc_user_range(const void *ptr, uint32_t length) {
    if (length == 0u) return true;
    if (!ptr) return false;

    process_t *proc = process_get_current();
    if (!proc) return false;

    uint64_t start = (uint32_t)(uintptr_t)ptr;
    uint64_t end = start + length;
    if (end > 0x100000000ULL) return false;

    if (proc->binary_base && proc->binary_size) {
        uint64_t base = (uint32_t)(uintptr_t)proc->binary_base;
        uint64_t limit = base + proc->binary_size;
        if (start >= base && end <= limit) return true;
    }

    if (!proc->address_space && proc->stack_base && proc->stack_size) {
        uint64_t base = (uint32_t)(uintptr_t)proc->stack_base;
        uint64_t limit = base + proc->stack_size;
        if (start >= base && end <= limit) return true;
    }

    if (proc->stack_start && proc->stack_size) {
        uint64_t base = proc->stack_start;
        uint64_t limit = base + proc->stack_size;
        if (start >= base && end <= limit) return true;
    }

    for (process_thread_t *th = proc->threads; th; th = th->next) {
        if (th->stack_virtual_top && th->stack_virtual_bottom) {
            uint64_t base = th->stack_virtual_bottom;
            uint64_t limit = th->stack_virtual_top;
            if (start >= base && end <= limit) return true;
        } else if (th->stack_base && th->stack_size) {
            uint64_t base = (uint32_t)(uintptr_t)th->stack_base;
            uint64_t limit = base + th->stack_size;
            if (start >= base && end <= limit) return true;
        }
    }

    if (proc->heap_chunks && proc->heap_chunk_count > 0) {
        for (uint32_t i = 0; i < proc->heap_chunk_count; i++) {
            uint64_t base = proc->heap_chunks[i].base;
            uint64_t limit = proc->heap_chunks[i].brk;
            if (start >= base && end <= limit) return true;
        }
    } else if (proc->user_heap_base && proc->user_heap_break > proc->user_heap_base) {
        uint64_t base = proc->user_heap_base;
        uint64_t limit = proc->user_heap_break;
        if (start >= base && end <= limit) return true;
    }

    if (ipc_shared_memory_user_range(proc->pid, ptr, length)) return true;
    return ipo_hw_user_range(proc->pid, ptr, length);
}

char *ipc_copy_name_alloc(const char *name) {
    if (!name) return NULL;
    size_t cap = 32u;
    size_t len = 0u;
    char *buf = (char *)kmalloc(cap);
    if (!buf) return NULL;

    for (size_t i = 0;; i++) {
        if (!ipc_user_range(&name[i], 1u)) {
            kfree(buf);
            return NULL;
        }
        char c = name[i];
        if (c == '\0') {
            if (len == 0u) {
                kfree(buf);
                return NULL;
            }
            buf[len] = '\0';
            return buf;
        }
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.')) {
            kfree(buf);
            return NULL;
        }
        if (len + 1u >= cap) {
            size_t new_cap = cap * 2u;
            char *new_buf = (char *)krealloc(buf, new_cap);
            if (!new_buf) {
                kfree(buf);
                return NULL;
            }
            buf = new_buf;
            cap = new_cap;
        }
        buf[len++] = c;
    }
}

static ipc_endpoint_t *ipc_find_id_locked(uint32_t id) {
    for (uint32_t i = 0u; i < endpoint_capacity; i++) {
        if (endpoints[i].active && endpoints[i].id == id) return &endpoints[i];
    }
    return NULL;
}

static void ipc_endpoint_clear_locked(ipc_endpoint_t *endpoint) {
    uint32_t generation = endpoint->generation;
    bool retired = endpoint->retired || generation >= IPC_ENDPOINT_GENERATION_MAX;
    if (endpoint->name) {
        kfree(endpoint->name);
    }
    if (endpoint->clients) {
        kfree(endpoint->clients);
    }
    if (endpoint->messages) {
        for (uint32_t i = 0u; i < endpoint->message_capacity; i++) {
            if (endpoint->messages[i].data) {
                kfree(endpoint->messages[i].data);
            }
        }
        kfree(endpoint->messages);
    }
    memset(endpoint, 0, sizeof(*endpoint));
    endpoint->generation = generation;
    endpoint->retired = retired;
}

static ipc_endpoint_t *ipc_find_name_locked(const char *name) {
    for (uint32_t i = 0u; i < endpoint_capacity; i++) {
        if (endpoints[i].active && endpoints[i].name &&
            strcmp(endpoints[i].name, name) == 0)
            return &endpoints[i];
    }
    return NULL;
}

static uint32_t ipc_client_rights(const ipc_endpoint_t *endpoint, uint32_t pid) {
    for (uint32_t i = 0u; i < endpoint->client_capacity; i++) {
        if (endpoint->clients[i].pid == pid) return endpoint->clients[i].rights;
    }
    if (pid == endpoint->owner_pid) return IPO_IPC_RIGHT_SEND | IPO_IPC_RIGHT_RECEIVE;
    return 0u;
}

static void ipc_wake_waiters_locked(uint32_t endpoint_id, uint32_t wait_kind) {
    process_ipc_wake_waiters(endpoint_id, (uint8_t)wait_kind);
}

static int ipc_create(const char *name, uint32_t public_rights) {
    if ((public_rights & ~(IPO_IPC_RIGHT_SEND | IPO_IPC_RIGHT_RECEIVE)) != 0u)
        return IPO_IPC_ERR_INVALID;
    char *name_copy = ipc_copy_name_alloc(name);
    if (!name_copy) return IPO_IPC_ERR_INVALID;

    process_t *proc = process_get_current();
    if (!proc) {
        kfree(name_copy);
        return IPO_IPC_ERR_INVALID;
    }

    spin_lock(&ipc_lock);
    if (ipc_find_name_locked(name_copy)) {
        spin_unlock(&ipc_lock);
        kfree(name_copy);
        return IPO_IPC_ERR_EXISTS;
    }

    ipc_endpoint_t *slot = NULL;
    uint32_t slot_index = 0u;
    for (uint32_t i = 0u; i < endpoint_capacity; i++) {
        if (!endpoints[i].active && !endpoints[i].retired) {
            slot = &endpoints[i];
            slot_index = i;
            break;
        }
    }
    if (!slot) {
        uint32_t new_cap = endpoint_capacity ? (endpoint_capacity * 2u)
                                             : IPC_INITIAL_ENDPOINT_CAPACITY;
        if (new_cap <= endpoint_capacity ||
            new_cap > (SIZE_MAX / sizeof(*endpoints))) {
            spin_unlock(&ipc_lock);
            kfree(name_copy);
            return IPO_IPC_ERR_NO_SPACE;
        }
        ipc_endpoint_t *grown = (ipc_endpoint_t *)krealloc(
            endpoints, new_cap * sizeof(*endpoints));
        if (!grown) {
            spin_unlock(&ipc_lock);
            kfree(name_copy);
            return IPO_IPC_ERR_NO_SPACE;
        }
        memset(grown + endpoint_capacity, 0,
               (new_cap - endpoint_capacity) * sizeof(*endpoints));
        endpoints = grown;
        slot_index = endpoint_capacity;
        slot = &endpoints[slot_index];
        endpoint_capacity = new_cap;
    }

    uint32_t generation = slot->generation + 1u;
    if (generation == 0u || generation > IPC_ENDPOINT_GENERATION_MAX) {
        slot->retired = true;
        spin_unlock(&ipc_lock);
        kfree(name_copy);
        return IPO_IPC_ERR_NO_SPACE;
    }
    uint32_t id = ((generation - 1u) << 11) + slot_index + 1u;
    memset(slot, 0, sizeof(*slot));
    slot->active = true;
    slot->id = id;
    slot->generation = generation;
    slot->owner_pid = proc->pid;
    slot->public_rights = public_rights;
    slot->name = name_copy;
    spin_unlock(&ipc_lock);
    return (int)id;
}

static int ipc_open(const char *name, uint32_t rights) {
    if (rights == 0u ||
        (rights & ~(IPO_IPC_RIGHT_SEND | IPO_IPC_RIGHT_RECEIVE)) != 0u)
        return IPO_IPC_ERR_INVALID;
    char *name_copy = ipc_copy_name_alloc(name);
    if (!name_copy) return IPO_IPC_ERR_INVALID;

    process_t *proc = process_get_current();
    if (!proc) {
        kfree(name_copy);
        return IPO_IPC_ERR_INVALID;
    }

    spin_lock(&ipc_lock);
    ipc_endpoint_t *endpoint = ipc_find_name_locked(name_copy);
    kfree(name_copy);
    if (!endpoint) {
        spin_unlock(&ipc_lock);
        return IPO_IPC_ERR_NOT_FOUND;
    }
    if (proc->pid != endpoint->owner_pid &&
        (rights & endpoint->public_rights) != rights) {
        spin_unlock(&ipc_lock);
        return IPO_IPC_ERR_PERMISSION;
    }

    ipc_client_t *slot = NULL;
    for (uint32_t i = 0u; i < endpoint->client_capacity; i++) {
        ipc_client_t *client = &endpoint->clients[i];
        if (client->pid == proc->pid) {
            client->rights = rights;
            uint32_t id = endpoint->id;
            spin_unlock(&ipc_lock);
            return (int)id;
        }
        if (!slot && client->pid == 0u) {
            slot = client;
        }
    }

    if (!slot) {
        uint32_t new_cap = endpoint->client_capacity
            ? (endpoint->client_capacity * 2u)
            : IPC_INITIAL_CLIENT_CAPACITY;
        if (new_cap <= endpoint->client_capacity ||
            new_cap > (SIZE_MAX / sizeof(ipc_client_t))) {
            spin_unlock(&ipc_lock);
            return IPO_IPC_ERR_NO_SPACE;
        }
        ipc_client_t *grown = (ipc_client_t *)krealloc(
            endpoint->clients, new_cap * sizeof(ipc_client_t));
        if (!grown) {
            spin_unlock(&ipc_lock);
            return IPO_IPC_ERR_NO_SPACE;
        }
        memset(grown + endpoint->client_capacity, 0,
               (new_cap - endpoint->client_capacity) * sizeof(ipc_client_t));
        endpoint->clients = grown;
        slot = &endpoint->clients[endpoint->client_capacity];
        endpoint->client_capacity = new_cap;
    }

    slot->pid = proc->pid;
    slot->rights = rights;
    uint32_t id = endpoint->id;
    spin_unlock(&ipc_lock);
    return (int)id;
}

static int ipc_close(int id_int) {
    uint32_t id = (uint32_t)id_int;
    process_t *proc = process_get_current();
    if (!proc) return IPO_IPC_ERR_INVALID;

    spin_lock(&ipc_lock);
    ipc_endpoint_t *endpoint = ipc_find_id_locked(id);
    if (!endpoint) {
        spin_unlock(&ipc_lock);
        return IPO_IPC_ERR_NOT_FOUND;
    }
    if (endpoint->owner_pid == proc->pid) {
        spin_unlock(&ipc_lock);
        return IPO_IPC_ERR_PERMISSION;
    }

    for (uint32_t i = 0u; i < endpoint->client_capacity; i++) {
        if (endpoint->clients[i].pid == proc->pid) {
            memset(&endpoint->clients[i], 0, sizeof(ipc_client_t));
            ipc_wake_waiters_locked(id, IPC_WAIT_ANY);
            spin_unlock(&ipc_lock);
            return IPO_IPC_OK;
        }
    }

    spin_unlock(&ipc_lock);
    return IPO_IPC_ERR_PERMISSION;
}

static int ipc_destroy(int id_int) {
    uint32_t id = (uint32_t)id_int;
    process_t *proc = process_get_current();
    if (!proc) return IPO_IPC_ERR_INVALID;

    spin_lock(&ipc_lock);
    ipc_endpoint_t *endpoint = ipc_find_id_locked(id);
    if (!endpoint) {
        spin_unlock(&ipc_lock);
        return IPO_IPC_ERR_NOT_FOUND;
    }
    if (endpoint->owner_pid != proc->pid) {
        spin_unlock(&ipc_lock);
        return IPO_IPC_ERR_PERMISSION;
    }

    for (uint32_t i = 0u; i < endpoint->client_capacity; i++) {
        if (endpoint->clients[i].pid != 0u) {
            ipc_stream_kernel_notify(endpoint->clients[i].pid,
                                     IPO_IPC_EVENT_ENDPOINT_CLOSED, id);
        }
    }

    ipc_endpoint_clear_locked(endpoint);
    ipc_wake_waiters_locked(id, IPC_WAIT_ANY);
    spin_unlock(&ipc_lock);
    return IPO_IPC_OK;
}

static int ipc_send(int id_int, const void *data, uint32_t length, uint32_t flags) {
    uint32_t id = (uint32_t)id_int;
    if ((flags & ~IPO_IPC_NONBLOCK) != 0u) return IPO_IPC_ERR_INVALID;
    if (length > 0u && !data) return IPO_IPC_ERR_INVALID;
    if (length > 0u && !ipc_user_range(data, length)) return IPO_IPC_ERR_INVALID;

    process_t *proc = process_get_current();
    if (!proc) return IPO_IPC_ERR_INVALID;

    spin_lock(&ipc_lock);
    ipc_endpoint_t *endpoint = ipc_find_id_locked(id);
    if (!endpoint) {
        spin_unlock(&ipc_lock);
        return IPO_IPC_ERR_NOT_FOUND;
    }

    uint32_t rights = ipc_client_rights(endpoint, proc->pid);
    if ((rights & IPO_IPC_RIGHT_SEND) == 0u) {
        spin_unlock(&ipc_lock);
        return IPO_IPC_ERR_PERMISSION;
    }

    /* Unbounded dynamic queue auto-expansion */
    if (!endpoint->messages || endpoint->count >= endpoint->message_capacity) {
        uint32_t old_cap = endpoint->message_capacity;
        uint32_t new_cap = (old_cap == 0u) ? IPC_INITIAL_QUEUE_CAPACITY : (old_cap * 2u);
        ipc_message_t *new_messages = (ipc_message_t *)kmalloc(new_cap * sizeof(ipc_message_t));
        if (!new_messages) {
            spin_unlock(&ipc_lock);
            return IPO_IPC_ERR_NO_SPACE;
        }
        for (uint32_t i = 0u; i < endpoint->count; i++) {
            new_messages[i] = endpoint->messages[(endpoint->head + i) % old_cap];
        }
        memset(&new_messages[endpoint->count], 0, (new_cap - endpoint->count) * sizeof(ipc_message_t));
        if (endpoint->messages) {
            kfree(endpoint->messages);
        }
        endpoint->messages = new_messages;
        endpoint->head = 0u;
        endpoint->message_capacity = new_cap;
    }

    uint8_t *copy = NULL;
    if (length > 0u) {
        copy = (uint8_t *)kmalloc(length);
        if (!copy) {
            spin_unlock(&ipc_lock);
            return IPO_IPC_ERR_NO_SPACE;
        }
        memcpy(copy, data, length);
    }

    uint32_t slot_idx = (endpoint->head + endpoint->count) % endpoint->message_capacity;
    if (endpoint->messages[slot_idx].data) {
        kfree(endpoint->messages[slot_idx].data);
    }
    endpoint->messages[slot_idx].sender_pid = proc->pid;
    endpoint->messages[slot_idx].length = length;
    endpoint->messages[slot_idx].data = copy;
    endpoint->count++;

    ipc_wake_waiters_locked(id, IPC_WAIT_RECEIVE);
    spin_unlock(&ipc_lock);
    return IPO_IPC_OK;
}

static int ipc_receive(int id_int, void *buffer, uint32_t capacity,
                       uint32_t *sender_pid, uint32_t *message_length,
                       uint32_t flags) {
    uint32_t id = (uint32_t)id_int;
    if ((flags & ~IPO_IPC_NONBLOCK) != 0u) return IPO_IPC_ERR_INVALID;
    if (!sender_pid || !message_length) return IPO_IPC_ERR_INVALID;
    if (capacity > 0u && !buffer) return IPO_IPC_ERR_INVALID;
    if (!ipc_user_range(sender_pid, sizeof(uint32_t))) return IPO_IPC_ERR_INVALID;
    if (!ipc_user_range(message_length, sizeof(uint32_t))) return IPO_IPC_ERR_INVALID;
    if (capacity > 0u && !ipc_user_range(buffer, capacity)) return IPO_IPC_ERR_INVALID;

    process_t *proc = process_get_current();
    if (!proc) return IPO_IPC_ERR_INVALID;

retry:
    if (!proc->is_running) return IPO_IPC_ERR_WOULD_BLOCK;
    spin_lock(&ipc_lock);
    ipc_endpoint_t *endpoint = ipc_find_id_locked(id);
    if (!endpoint) {
        spin_unlock(&ipc_lock);
        return IPO_IPC_ERR_NOT_FOUND;
    }

    uint32_t rights = ipc_client_rights(endpoint, proc->pid);
    if ((rights & IPO_IPC_RIGHT_RECEIVE) == 0u) {
        spin_unlock(&ipc_lock);
        return IPO_IPC_ERR_PERMISSION;
    }

    if (endpoint->count == 0u || !endpoint->messages) {
        if ((flags & IPO_IPC_NONBLOCK) != 0u) {
            spin_unlock(&ipc_lock);
            return IPO_IPC_ERR_WOULD_BLOCK;
        }
        if (!process_in_process_context() || !process_is_batch_active()) {
            spin_unlock(&ipc_lock);
            return IPO_IPC_ERR_WOULD_BLOCK;
        }
        process_ipc_set_waiting(proc, id, IPC_WAIT_RECEIVE);
        spin_unlock(&ipc_lock);
        process_yield_kernel();
        if (process_ipc_consume_wait_cancelled()) {
            return IPO_IPC_ERR_WOULD_BLOCK;
        }
        goto retry;
    }

    ipc_message_t *msg = &endpoint->messages[endpoint->head];
    if (capacity < msg->length) {
        *message_length = msg->length;
        spin_unlock(&ipc_lock);
        return IPO_IPC_ERR_TOO_LARGE;
    }

    if (msg->length > 0u && msg->data) {
        memcpy(buffer, msg->data, msg->length);
    }
    *sender_pid = msg->sender_pid;
    *message_length = msg->length;

    if (msg->data) {
        kfree(msg->data);
        msg->data = NULL;
    }
    msg->sender_pid = 0u;
    msg->length = 0u;

    endpoint->head = (endpoint->head + 1u) % endpoint->message_capacity;
    endpoint->count--;

    ipc_wake_waiters_locked(id, IPC_WAIT_SEND);
    spin_unlock(&ipc_lock);
    return IPO_IPC_OK;
}

bool ipc_init(void) {
    spinlock_init(&ipc_lock);
    endpoint_capacity = IPC_INITIAL_ENDPOINT_CAPACITY;
    endpoints = (ipc_endpoint_t *)kmalloc(endpoint_capacity * sizeof(ipc_endpoint_t));
    if (!endpoints) {
        endpoint_capacity = 0u;
        ipc_ready = false;
        serial_printf("[ipc] Failed to allocate endpoint table\n");
        return false;
    }
    memset(endpoints, 0, endpoint_capacity * sizeof(ipc_endpoint_t));
    ipc_ready = true;

    if (!ipc_stream_init()) {
        ipc_ready = false;
        kfree(endpoints);
        endpoints = NULL;
        endpoint_capacity = 0u;
        serial_printf("[ipc] Failed to initialize stream subsystem\n");
        return false;
    }

    if (!ipc_shared_memory_init()) {
        ipc_ready = false;
        kfree(endpoints);
        endpoints = NULL;
        endpoint_capacity = 0u;
        serial_printf("[ipc] Failed to initialize shared memory subsystem\n");
        return false;
    }

    return true;
}

void ipc_process_terminated(uint32_t pid) {
    if (pid == 0u) return;
    process_signal_cleanup(pid);

    spin_lock(&ipc_lock);
    for (uint32_t i = 0u; i < endpoint_capacity; i++) {
        ipc_endpoint_t *ep = &endpoints[i];
        if (!ep->active) continue;

        if (ep->owner_pid == pid) {
            for (uint32_t c = 0u; c < ep->client_capacity; c++) {
                if (ep->clients[c].pid != 0u) {
                    ipc_stream_kernel_notify(ep->clients[c].pid,
                                             IPO_IPC_EVENT_ENDPOINT_CLOSED,
                                             ep->id);
                }
            }
            ipc_endpoint_clear_locked(ep);
            ipc_wake_waiters_locked(ep->id, IPC_WAIT_ANY);
        } else {
            for (uint32_t c = 0u; c < ep->client_capacity; c++) {
                if (ep->clients[c].pid == pid) {
                    memset(&ep->clients[c], 0, sizeof(ep->clients[c]));
                    ipc_stream_kernel_notify(ep->owner_pid,
                                             IPO_IPC_EVENT_PROCESS_EXIT,
                                             pid);
                    ipc_wake_waiters_locked(ep->id, IPC_WAIT_ANY);
                }
            }
        }
    }
    spin_unlock(&ipc_lock);

    ipc_stream_process_terminated(pid);
    ipc_shared_memory_process_terminated(pid);
}

bool ipc_copy_syscall_args(uint32_t argc, const uint32_t *argv,
                           uint32_t expected_count, uint32_t *args) {
    if (argc < expected_count || !argv) return false;
    if (!ipc_user_range((const void *)argv, expected_count * sizeof(uint32_t))) {
        return false;
    }
    memcpy(args, argv, expected_count * sizeof(uint32_t));
    return true;
}

uint32_t ipc_syscall_handler(uint32_t num, uint32_t argc, uint32_t *argv) {
    if (num >= IPO_SYSCALL_IPC_PIPE_CREATE && num <= IPO_SYSCALL_IPC_NOTIFY_WAIT) {
        return (uint32_t)ipc_stream_syscall_handler(num, argc, argv);
    }

    if (!ipc_ready) return (uint32_t)IPO_IPC_ERR_NO_SPACE;

    uint32_t args[6] = {0};

    if (num == IPO_SYSCALL_IPC_CREATE) {
        if (!ipc_copy_syscall_args(argc, argv, 2u, args)) {
            return (uint32_t)IPO_IPC_ERR_INVALID;
        }
        return (uint32_t)ipc_create((const char *)(uintptr_t)args[0], args[1]);
    }

    if (num == IPO_SYSCALL_IPC_OPEN) {
        if (!ipc_copy_syscall_args(argc, argv, 2u, args)) {
            return (uint32_t)IPO_IPC_ERR_INVALID;
        }
        return (uint32_t)ipc_open((const char *)(uintptr_t)args[0], args[1]);
    }

    if (num == IPO_SYSCALL_IPC_CLOSE) {
        if (!ipc_copy_syscall_args(argc, argv, 1u, args)) {
            return (uint32_t)IPO_IPC_ERR_INVALID;
        }
        return (uint32_t)ipc_close((int)args[0]);
    }

    if (num == IPO_SYSCALL_IPC_DESTROY) {
        if (!ipc_copy_syscall_args(argc, argv, 1u, args)) {
            return (uint32_t)IPO_IPC_ERR_INVALID;
        }
        return (uint32_t)ipc_destroy((int)args[0]);
    }

    if (num == IPO_SYSCALL_IPC_SEND) {
        if (!ipc_copy_syscall_args(argc, argv, 4u, args)) {
            return (uint32_t)IPO_IPC_ERR_INVALID;
        }
        if (args[2] > 0u &&
            !ipc_user_range((const void *)(uintptr_t)args[1], args[2])) {
            return (uint32_t)IPO_IPC_ERR_INVALID;
        }
        return (uint32_t)ipc_send((int)args[0],
                                  (const void *)(uintptr_t)args[1],
                                  args[2], args[3]);
    }

    if (num == IPO_SYSCALL_IPC_RECEIVE) {
        if (!ipc_copy_syscall_args(argc, argv, 6u, args)) {
            return (uint32_t)IPO_IPC_ERR_INVALID;
        }
        if (!ipc_user_range((void *)(uintptr_t)args[3], sizeof(uint32_t)) ||
            !ipc_user_range((void *)(uintptr_t)args[4], sizeof(uint32_t))) {
            return (uint32_t)IPO_IPC_ERR_INVALID;
        }
        if (args[2] > 0u &&
            !ipc_user_range((void *)(uintptr_t)args[1], args[2])) {
            return (uint32_t)IPO_IPC_ERR_INVALID;
        }
        return (uint32_t)ipc_receive((int)args[0],
                                     (void *)(uintptr_t)args[1],
                                     args[2],
                                     (uint32_t *)(uintptr_t)args[3],
                                     (uint32_t *)(uintptr_t)args[4],
                                     args[5]);
    }

    return (uint32_t)IPO_IPC_ERR_INVALID;
}
