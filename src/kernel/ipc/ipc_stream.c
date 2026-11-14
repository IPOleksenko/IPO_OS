#include <kernel/ipc.h>
#include <kernel/process.h>
#include <kernel/smp.h>
#include <memory/kmalloc.h>
#include <ipc.h>
#include <syscall.h>
#include <string.h>

#define IPC_INITIAL_PIPE_CAPACITY 8u
#define IPC_INITIAL_PIPE_CLIENT_CAPACITY 4u
#define IPC_INITIAL_PIPE_BUFFER_CAPACITY 4096u
#define IPC_PIPE_GENERATION_MAX 0x000FFFFFu

#define IPC_INITIAL_NOTIFICATION_RECIPIENT_CAPACITY 8u
#define IPC_INITIAL_NOTIFICATION_QUEUE_CAPACITY 8u

#define IPC_WAIT_PIPE_READ 3u
#define IPC_WAIT_PIPE_WRITE 4u
#define IPC_WAIT_NOTIFICATION 5u

typedef struct {
    uint32_t pid;
    uint32_t rights;
} ipc_pipe_client_t;

typedef struct {
    bool active;
    bool destroyed;
    bool retired;
    uint32_t id;
    uint32_t generation;
    uint32_t owner_pid;
    uint32_t owner_rights;
    uint32_t public_rights;
    char *name;
    ipc_pipe_client_t *clients;
    uint32_t client_capacity;
    uint8_t *bytes;
    uint32_t capacity;
    uint32_t head;
    uint32_t count;
} ipc_pipe_t;

typedef struct {
    bool active;
    uint32_t pid;
    uint32_t head;
    uint32_t count;
    uint32_t capacity;
    ipo_ipc_notification_t *events;
} ipc_notification_queue_t;

static spinlock_t ipc_stream_lock;
static ipc_pipe_t *ipc_pipes;
static uint32_t ipc_pipe_capacity;
static ipc_notification_queue_t *ipc_notifications;
static uint32_t ipc_notification_capacity;
static bool ipc_stream_ready;

static int ipc_notify_enqueue_locked(uint32_t recipient_pid,
                                     uint32_t sender_pid, uint32_t event,
                                     uint32_t value);

static ipc_pipe_t *ipc_pipe_find_locked(uint32_t id) {
    for (uint32_t i = 0u; i < ipc_pipe_capacity; i++) {
        if (ipc_pipes[i].active && ipc_pipes[i].id == id)
            return &ipc_pipes[i];
    }
    return NULL;
}

static uint32_t ipc_pipe_rights_locked(const ipc_pipe_t *pipe, uint32_t pid) {
    if (pipe->owner_pid == pid) return pipe->owner_rights;
    if (pipe->clients) {
        for (uint32_t i = 0u; i < pipe->client_capacity; i++) {
            if (pipe->clients[i].pid == pid) return pipe->clients[i].rights;
        }
    }
    return 0u;
}

static uint32_t ipc_pipe_reader_count_locked(const ipc_pipe_t *pipe) {
    uint32_t readers =
        (pipe->owner_rights & IPO_IPC_PIPE_RIGHT_READ) != 0u ? 1u : 0u;
    if (pipe->clients) {
        for (uint32_t i = 0u; i < pipe->client_capacity; i++) {
            if ((pipe->clients[i].rights & IPO_IPC_PIPE_RIGHT_READ) != 0u)
                readers++;
        }
    }
    return readers;
}

static uint32_t ipc_pipe_writer_count_locked(const ipc_pipe_t *pipe) {
    uint32_t writers =
        (pipe->owner_rights & IPO_IPC_PIPE_RIGHT_WRITE) != 0u ? 1u : 0u;
    if (pipe->clients) {
        for (uint32_t i = 0u; i < pipe->client_capacity; i++) {
            if ((pipe->clients[i].rights & IPO_IPC_PIPE_RIGHT_WRITE) != 0u)
                writers++;
        }
    }
    return writers;
}

static bool ipc_pipe_has_clients_locked(const ipc_pipe_t *pipe) {
    if (!pipe->clients) return false;
    for (uint32_t i = 0u; i < pipe->client_capacity; i++) {
        if (pipe->clients[i].pid != 0u) return true;
    }
    return false;
}

static void ipc_pipe_clear_locked(ipc_pipe_t *pipe) {
    uint32_t generation = pipe->generation;
    bool retired = pipe->retired || generation >= IPC_PIPE_GENERATION_MAX;
    if (pipe->name) kfree(pipe->name);
    if (pipe->clients) kfree(pipe->clients);
    if (pipe->bytes) kfree(pipe->bytes);
    memset(pipe, 0, sizeof(*pipe));
    pipe->generation = generation;
    pipe->retired = retired;
}

static void ipc_notification_clear_locked(ipc_notification_queue_t *queue) {
    if (queue->events) kfree(queue->events);
    memset(queue, 0, sizeof(*queue));
}

static int ipc_pipe_create(const char *user_name, uint32_t public_rights,
                           uint32_t *user_handle) {
    if ((public_rights & ~(IPO_IPC_PIPE_RIGHT_READ | IPO_IPC_PIPE_RIGHT_WRITE)) ||
        !ipc_user_range(user_handle, sizeof(*user_handle)))
        return IPO_IPC_ERR_INVALID;
    char *name = ipc_copy_name_alloc(user_name);
    if (!name) return IPO_IPC_ERR_INVALID;
    process_t *proc = process_get_current();
    if (!proc) {
        kfree(name);
        return IPO_IPC_ERR_INVALID;
    }

    spin_lock(&ipc_stream_lock);
    for (uint32_t i = 0u; i < ipc_pipe_capacity; i++) {
        if (ipc_pipes[i].active && !ipc_pipes[i].destroyed &&
            ipc_pipes[i].name && strcmp(ipc_pipes[i].name, name) == 0) {
            spin_unlock(&ipc_stream_lock);
            kfree(name);
            return IPO_IPC_ERR_EXISTS;
        }
    }

    uint32_t slot = UINT32_MAX;
    for (uint32_t i = 0u; i < ipc_pipe_capacity; i++) {
        if (!ipc_pipes[i].active && !ipc_pipes[i].retired) {
            slot = i;
            break;
        }
    }
    if (slot == UINT32_MAX) {
        uint32_t new_cap = ipc_pipe_capacity ? (ipc_pipe_capacity * 2u)
                                             : IPC_INITIAL_PIPE_CAPACITY;
        if (new_cap <= ipc_pipe_capacity ||
            new_cap > (SIZE_MAX / sizeof(*ipc_pipes))) {
            spin_unlock(&ipc_stream_lock);
            kfree(name);
            return IPO_IPC_ERR_NO_SPACE;
        }
        ipc_pipe_t *grown = (ipc_pipe_t *)krealloc(
            ipc_pipes, new_cap * sizeof(*ipc_pipes));
        if (!grown) {
            spin_unlock(&ipc_stream_lock);
            kfree(name);
            return IPO_IPC_ERR_NO_SPACE;
        }
        memset(grown + ipc_pipe_capacity, 0,
               (new_cap - ipc_pipe_capacity) * sizeof(*ipc_pipes));
        ipc_pipes = grown;
        slot = ipc_pipe_capacity;
        ipc_pipe_capacity = new_cap;
    }

    uint32_t generation = ipc_pipes[slot].generation + 1u;
    if (generation == 0u || generation > IPC_PIPE_GENERATION_MAX) {
        ipc_pipes[slot].retired = true;
        spin_unlock(&ipc_stream_lock);
        kfree(name);
        return IPO_IPC_ERR_NO_SPACE;
    }

    uint8_t *ring_bytes = (uint8_t *)kmalloc(IPC_INITIAL_PIPE_BUFFER_CAPACITY);
    if (!ring_bytes) {
        spin_unlock(&ipc_stream_lock);
        kfree(name);
        return IPO_IPC_ERR_NO_SPACE;
    }
    memset(ring_bytes, 0, IPC_INITIAL_PIPE_BUFFER_CAPACITY);

    ipc_pipe_client_t *clients = (ipc_pipe_client_t *)kmalloc(
        IPC_INITIAL_PIPE_CLIENT_CAPACITY * sizeof(ipc_pipe_client_t));
    if (!clients) {
        spin_unlock(&ipc_stream_lock);
        kfree(ring_bytes);
        kfree(name);
        return IPO_IPC_ERR_NO_SPACE;
    }
    memset(clients, 0,
           IPC_INITIAL_PIPE_CLIENT_CAPACITY * sizeof(ipc_pipe_client_t));

    uint32_t id = ((generation - 1u) << 11) + slot + 1u;
    ipc_pipe_t *pipe = &ipc_pipes[slot];
    memset(pipe, 0, sizeof(*pipe));
    pipe->active = true;
    pipe->id = id;
    pipe->generation = generation;
    pipe->owner_pid = proc->pid;
    pipe->owner_rights = IPO_IPC_PIPE_RIGHT_READ | IPO_IPC_PIPE_RIGHT_WRITE;
    pipe->public_rights = public_rights;
    pipe->bytes = ring_bytes;
    pipe->capacity = IPC_INITIAL_PIPE_BUFFER_CAPACITY;
    pipe->clients = clients;
    pipe->client_capacity = IPC_INITIAL_PIPE_CLIENT_CAPACITY;
    pipe->name = name;
    spin_unlock(&ipc_stream_lock);
    *user_handle = id;
    return IPO_IPC_OK;
}

static int ipc_pipe_open(const char *user_name, uint32_t rights) {
    if (rights == 0u ||
        (rights & ~(IPO_IPC_PIPE_RIGHT_READ | IPO_IPC_PIPE_RIGHT_WRITE)))
        return IPO_IPC_ERR_INVALID;
    char *name = ipc_copy_name_alloc(user_name);
    if (!name) return IPO_IPC_ERR_INVALID;
    process_t *proc = process_get_current();
    if (!proc) {
        kfree(name);
        return IPO_IPC_ERR_INVALID;
    }

    spin_lock(&ipc_stream_lock);
    ipc_pipe_t *pipe = NULL;
    for (uint32_t i = 0u; i < ipc_pipe_capacity; i++) {
        if (ipc_pipes[i].active && !ipc_pipes[i].destroyed &&
            ipc_pipes[i].name && strcmp(ipc_pipes[i].name, name) == 0) {
            pipe = &ipc_pipes[i];
            break;
        }
    }
    kfree(name);
    if (!pipe) {
        spin_unlock(&ipc_stream_lock);
        return IPO_IPC_ERR_NOT_FOUND;
    }
    if (pipe->owner_pid == proc->pid) {
        pipe->owner_rights |= rights;
        uint32_t id = pipe->id;
        spin_unlock(&ipc_stream_lock);
        return (int)id;
    }
    if ((rights & pipe->public_rights) != rights) {
        spin_unlock(&ipc_stream_lock);
        return IPO_IPC_ERR_PERMISSION;
    }

    ipc_pipe_client_t *free_client = NULL;
    for (uint32_t i = 0u; i < pipe->client_capacity; i++) {
        ipc_pipe_client_t *client = &pipe->clients[i];
        if (client->pid == proc->pid) {
            client->rights |= rights;
            uint32_t id = pipe->id;
            spin_unlock(&ipc_stream_lock);
            return (int)id;
        }
        if (client->pid == 0u && !free_client) free_client = client;
    }
    if (!free_client) {
        uint32_t new_cap = pipe->client_capacity ? (pipe->client_capacity * 2u)
                                                : IPC_INITIAL_PIPE_CLIENT_CAPACITY;
        if (new_cap <= pipe->client_capacity ||
            new_cap > (SIZE_MAX / sizeof(*pipe->clients))) {
            spin_unlock(&ipc_stream_lock);
            return IPO_IPC_ERR_NO_SPACE;
        }
        ipc_pipe_client_t *grown = (ipc_pipe_client_t *)krealloc(
            pipe->clients, new_cap * sizeof(ipc_pipe_client_t));
        if (!grown) {
            spin_unlock(&ipc_stream_lock);
            return IPO_IPC_ERR_NO_SPACE;
        }
        memset(grown + pipe->client_capacity, 0,
               (new_cap - pipe->client_capacity) * sizeof(ipc_pipe_client_t));
        free_client = &grown[pipe->client_capacity];
        pipe->clients = grown;
        pipe->client_capacity = new_cap;
    }

    free_client->pid = proc->pid;
    free_client->rights = rights;
    uint32_t id = pipe->id;
    spin_unlock(&ipc_stream_lock);
    return (int)id;
}

static int ipc_pipe_close(uint32_t id, uint32_t rights) {
    if (rights == 0u ||
        (rights & ~(IPO_IPC_PIPE_RIGHT_READ | IPO_IPC_PIPE_RIGHT_WRITE)))
        return IPO_IPC_ERR_INVALID;
    process_t *proc = process_get_current();
    if (!proc) return IPO_IPC_ERR_INVALID;

    spin_lock(&ipc_stream_lock);
    ipc_pipe_t *pipe = ipc_pipe_find_locked(id);
    if (!pipe) {
        spin_unlock(&ipc_stream_lock);
        return IPO_IPC_ERR_NOT_FOUND;
    }
    if (pipe->owner_pid == proc->pid) {
        if ((pipe->owner_rights & rights) != rights) {
            spin_unlock(&ipc_stream_lock);
            return IPO_IPC_ERR_PERMISSION;
        }
        pipe->owner_rights &= ~rights;
    } else {
        bool found = false;
        for (uint32_t i = 0u; i < pipe->client_capacity; i++) {
            ipc_pipe_client_t *client = &pipe->clients[i];
            if (client->pid != proc->pid) continue;
            if ((client->rights & rights) != rights) {
                spin_unlock(&ipc_stream_lock);
                return IPO_IPC_ERR_PERMISSION;
            }
            client->rights &= ~rights;
            if (client->rights == 0u) memset(client, 0, sizeof(*client));
            found = true;
            break;
        }
        if (!found) {
            spin_unlock(&ipc_stream_lock);
            return IPO_IPC_ERR_PERMISSION;
        }
    }

    process_ipc_wake_waiters(id, IPC_WAIT_PIPE_READ);
    process_ipc_wake_waiters(id, IPC_WAIT_PIPE_WRITE);

    if (pipe->destroyed && pipe->owner_rights == 0u &&
        !ipc_pipe_has_clients_locked(pipe)) {
        ipc_pipe_clear_locked(pipe);
    }

    spin_unlock(&ipc_stream_lock);
    return IPO_IPC_OK;
}

static int ipc_pipe_destroy(uint32_t id) {
    process_t *proc = process_get_current();
    if (!proc) return IPO_IPC_ERR_INVALID;
    spin_lock(&ipc_stream_lock);
    ipc_pipe_t *pipe = ipc_pipe_find_locked(id);
    if (!pipe || pipe->destroyed) {
        spin_unlock(&ipc_stream_lock);
        return IPO_IPC_ERR_NOT_FOUND;
    }
    if (pipe->owner_pid != proc->pid) {
        spin_unlock(&ipc_stream_lock);
        return IPO_IPC_ERR_PERMISSION;
    }
    pipe->destroyed = true;

    if (pipe->clients) {
        for (uint32_t i = 0u; i < pipe->client_capacity; i++) {
            if (pipe->clients[i].pid != 0u) {
                (void)ipc_notify_enqueue_locked(pipe->clients[i].pid, 0u,
                                                IPO_IPC_EVENT_PIPE_CLOSED, id);
            }
        }
    }

    process_ipc_wake_waiters(id, IPC_WAIT_PIPE_READ);
    process_ipc_wake_waiters(id, IPC_WAIT_PIPE_WRITE);

    if (!ipc_pipe_has_clients_locked(pipe)) {
        ipc_pipe_clear_locked(pipe);
    }

    spin_unlock(&ipc_stream_lock);
    return IPO_IPC_OK;
}

static int ipc_pipe_read(uint32_t id, uint8_t *buffer, uint32_t capacity,
                         uint32_t flags) {
    if ((capacity != 0u && !ipc_user_range(buffer, capacity)) ||
        (flags & ~IPO_IPC_NONBLOCK))
        return IPO_IPC_ERR_INVALID;
    process_t *proc = process_get_current();
    if (!proc) return IPO_IPC_ERR_INVALID;

    for (;;) {
        spin_lock(&ipc_stream_lock);
        ipc_pipe_t *pipe = ipc_pipe_find_locked(id);
        if (!pipe) {
            spin_unlock(&ipc_stream_lock);
            return IPO_IPC_ERR_NOT_FOUND;
        }
        if ((ipc_pipe_rights_locked(pipe, proc->pid) &
             IPO_IPC_PIPE_RIGHT_READ) == 0u) {
            spin_unlock(&ipc_stream_lock);
            return IPO_IPC_ERR_PERMISSION;
        }
        if (capacity == 0u) {
            spin_unlock(&ipc_stream_lock);
            return 0;
        }
        if (pipe->count != 0u) {
            uint32_t length = capacity < pipe->count ? capacity : pipe->count;
            uint32_t first = length;
            uint32_t until_end = pipe->capacity - pipe->head;
            if (first > until_end) first = until_end;
            memcpy(buffer, pipe->bytes + pipe->head, first);
            if (length > first)
                memcpy(buffer + first, pipe->bytes, length - first);
            pipe->head = (pipe->head + length) % pipe->capacity;
            pipe->count -= length;
            process_ipc_wake_waiters(id, IPC_WAIT_PIPE_WRITE);
            spin_unlock(&ipc_stream_lock);
            return (int)length;
        }
        if (ipc_pipe_writer_count_locked(pipe) == 0u) {
            spin_unlock(&ipc_stream_lock);
            return 0;
        }
        if ((flags & IPO_IPC_NONBLOCK) || !process_is_batch_active() ||
            !process_in_process_context()) {
            spin_unlock(&ipc_stream_lock);
            return IPO_IPC_ERR_WOULD_BLOCK;
        }
        process_ipc_set_waiting(proc, id, IPC_WAIT_PIPE_READ);
        spin_unlock(&ipc_stream_lock);
        process_yield_kernel();
        if (process_ipc_consume_wait_cancelled())
            return IPO_IPC_ERR_WOULD_BLOCK;
    }
}

static int ipc_pipe_write(uint32_t id, const uint8_t *buffer, uint32_t length,
                          uint32_t flags) {
    if ((length != 0u && !ipc_user_range(buffer, length)) ||
        (flags & ~IPO_IPC_NONBLOCK))
        return IPO_IPC_ERR_INVALID;
    process_t *proc = process_get_current();
    if (!proc) return IPO_IPC_ERR_INVALID;

    for (;;) {
        spin_lock(&ipc_stream_lock);
        ipc_pipe_t *pipe = ipc_pipe_find_locked(id);
        if (!pipe || pipe->destroyed) {
            spin_unlock(&ipc_stream_lock);
            return IPO_IPC_ERR_NOT_FOUND;
        }
        if ((ipc_pipe_rights_locked(pipe, proc->pid) &
             IPO_IPC_PIPE_RIGHT_WRITE) == 0u) {
            spin_unlock(&ipc_stream_lock);
            return IPO_IPC_ERR_PERMISSION;
        }
        if (length == 0u) {
            spin_unlock(&ipc_stream_lock);
            return 0;
        }
        if (ipc_pipe_reader_count_locked(pipe) == 0u) {
            spin_unlock(&ipc_stream_lock);
            return IPO_IPC_ERR_CLOSED;
        }

        if (pipe->count + length > pipe->capacity) {
            uint32_t needed = pipe->count + length;
            uint32_t new_cap = (needed + 4095u) & ~4095u;
            if (new_cap > pipe->capacity && new_cap <= 0x10000000u) {
                uint8_t *new_buf = (uint8_t *)kmalloc(new_cap);
                if (new_buf) {
                    if (pipe->count > 0u) {
                        uint32_t first = pipe->count;
                        uint32_t until_end = pipe->capacity - pipe->head;
                        if (first > until_end) first = until_end;
                        memcpy(new_buf, pipe->bytes + pipe->head, first);
                        if (pipe->count > first) {
                            memcpy(new_buf + first, pipe->bytes,
                                   pipe->count - first);
                        }
                    }
                    kfree(pipe->bytes);
                    pipe->bytes = new_buf;
                    pipe->head = 0u;
                    pipe->capacity = new_cap;
                }
            }
        }

        uint32_t available = pipe->capacity - pipe->count;
        if (available != 0u) {
            uint32_t amount = length < available ? length : available;
            uint32_t tail = (pipe->head + pipe->count) % pipe->capacity;
            uint32_t first = amount;
            uint32_t until_end = pipe->capacity - tail;
            if (first > until_end) first = until_end;
            memcpy(pipe->bytes + tail, buffer, first);
            if (amount > first)
                memcpy(pipe->bytes, buffer + first, amount - first);
            pipe->count += amount;
            process_ipc_wake_waiters(id, IPC_WAIT_PIPE_READ);
            spin_unlock(&ipc_stream_lock);
            return (int)amount;
        }

        if ((flags & IPO_IPC_NONBLOCK) || !process_is_batch_active() ||
            !process_in_process_context()) {
            spin_unlock(&ipc_stream_lock);
            return IPO_IPC_ERR_WOULD_BLOCK;
        }
        process_ipc_set_waiting(proc, id, IPC_WAIT_PIPE_WRITE);
        spin_unlock(&ipc_stream_lock);
        process_yield_kernel();
        if (process_ipc_consume_wait_cancelled())
            return IPO_IPC_ERR_WOULD_BLOCK;
    }
}

static ipc_notification_queue_t *ipc_notification_find_locked(uint32_t pid,
                                                               bool create) {
    ipc_notification_queue_t *free_slot = NULL;
    for (uint32_t i = 0u; i < ipc_notification_capacity; i++) {
        ipc_notification_queue_t *queue = &ipc_notifications[i];
        if (queue->active && queue->pid == pid) return queue;
        if (!queue->active && !free_slot) free_slot = queue;
    }
    if (!create) return NULL;

    if (!free_slot) {
        uint32_t new_cap = ipc_notification_capacity
                               ? (ipc_notification_capacity * 2u)
                               : IPC_INITIAL_NOTIFICATION_RECIPIENT_CAPACITY;
        if (new_cap <= ipc_notification_capacity ||
            new_cap > (SIZE_MAX / sizeof(*ipc_notifications)))
            return NULL;
        ipc_notification_queue_t *grown = (ipc_notification_queue_t *)krealloc(
            ipc_notifications, new_cap * sizeof(*ipc_notifications));
        if (!grown) return NULL;
        memset(grown + ipc_notification_capacity, 0,
               (new_cap - ipc_notification_capacity) * sizeof(*ipc_notifications));
        free_slot = &grown[ipc_notification_capacity];
        ipc_notifications = grown;
        ipc_notification_capacity = new_cap;
    }

    ipo_ipc_notification_t *events = (ipo_ipc_notification_t *)kmalloc(
        IPC_INITIAL_NOTIFICATION_QUEUE_CAPACITY * sizeof(ipo_ipc_notification_t));
    if (!events) return NULL;

    memset(free_slot, 0, sizeof(*free_slot));
    free_slot->active = true;
    free_slot->pid = pid;
    free_slot->events = events;
    free_slot->capacity = IPC_INITIAL_NOTIFICATION_QUEUE_CAPACITY;
    return free_slot;
}

static int ipc_notify_enqueue_locked(uint32_t recipient_pid,
                                     uint32_t sender_pid, uint32_t event,
                                     uint32_t value) {
    ipc_notification_queue_t *queue =
        ipc_notification_find_locked(recipient_pid, true);
    if (!queue) return IPO_IPC_ERR_NO_SPACE;

    if (queue->count == queue->capacity) {
        uint32_t new_cap = queue->capacity * 2u;
        if (new_cap <= queue->capacity ||
            new_cap > (SIZE_MAX / sizeof(*queue->events))) {
            return IPO_IPC_ERR_WOULD_BLOCK;
        }
        ipo_ipc_notification_t *new_events =
            (ipo_ipc_notification_t *)kmalloc(
                new_cap * sizeof(ipo_ipc_notification_t));
        if (!new_events) return IPO_IPC_ERR_WOULD_BLOCK;

        for (uint32_t i = 0u; i < queue->count; i++) {
            new_events[i] = queue->events[(queue->head + i) % queue->capacity];
        }
        kfree(queue->events);
        queue->events = new_events;
        queue->head = 0u;
        queue->capacity = new_cap;
    }

    uint32_t tail = (queue->head + queue->count) % queue->capacity;
    queue->events[tail].sender_pid = sender_pid;
    queue->events[tail].event = event;
    queue->events[tail].value = value;
    queue->count++;
    process_ipc_wake_waiters(recipient_pid, IPC_WAIT_NOTIFICATION);
    return IPO_IPC_OK;
}

void ipc_stream_kernel_notify(uint32_t recipient_pid, uint32_t event,
                              uint32_t value) {
    if (!ipc_stream_ready || recipient_pid == 0u) return;
    spin_lock(&ipc_stream_lock);
    (void)ipc_notify_enqueue_locked(recipient_pid, 0u, event, value);
    spin_unlock(&ipc_stream_lock);
}

static int ipc_notify_send(uint32_t recipient_pid, uint32_t event,
                           uint32_t value) {
    process_t *sender = process_get_current();
    if (!sender || recipient_pid == 0u || !process_is_alive(recipient_pid))
        return IPO_IPC_ERR_NOT_FOUND;
    spin_lock(&ipc_stream_lock);
    int res = ipc_notify_enqueue_locked(recipient_pid, sender->pid, event,
                                        value);
    spin_unlock(&ipc_stream_lock);
    return res;
}

static int ipc_notify_wait(ipo_ipc_notification_t *notification,
                           uint32_t flags) {
    if (!notification || !ipc_user_range(notification, sizeof(*notification)) ||
        (flags & ~IPO_IPC_NONBLOCK))
        return IPO_IPC_ERR_INVALID;
    process_t *proc = process_get_current();
    if (!proc) return IPO_IPC_ERR_INVALID;

    for (;;) {
        spin_lock(&ipc_stream_lock);
        ipc_notification_queue_t *queue =
            ipc_notification_find_locked(proc->pid, false);
        if (queue && queue->count != 0u) {
            *notification = queue->events[queue->head];
            queue->head = (queue->head + 1u) % queue->capacity;
            queue->count--;
            if (queue->count == 0u) {
                ipc_notification_clear_locked(queue);
            }
            spin_unlock(&ipc_stream_lock);
            return IPO_IPC_OK;
        }
        if ((flags & IPO_IPC_NONBLOCK) || !process_is_batch_active() ||
            !process_in_process_context()) {
            spin_unlock(&ipc_stream_lock);
            return IPO_IPC_ERR_WOULD_BLOCK;
        }
        process_ipc_set_waiting(proc, proc->pid, IPC_WAIT_NOTIFICATION);
        spin_unlock(&ipc_stream_lock);
        process_yield_kernel();
        if (process_ipc_consume_wait_cancelled())
            return IPO_IPC_ERR_WOULD_BLOCK;
    }
}

bool ipc_stream_init(void) {
    spinlock_init(&ipc_stream_lock);
    ipc_pipes = NULL;
    ipc_pipe_capacity = 0u;
    ipc_notifications = NULL;
    ipc_notification_capacity = 0u;
    ipc_stream_ready = true;
    return true;
}

void ipc_stream_process_terminated(uint32_t pid) {
    if (!ipc_stream_ready || pid == 0u) return;
    spin_lock(&ipc_stream_lock);
    for (uint32_t i = 0u; i < ipc_pipe_capacity; i++) {
        ipc_pipe_t *pipe = &ipc_pipes[i];
        if (!pipe->active) continue;
        uint32_t id = pipe->id;
        if (pipe->owner_pid == pid) {
            pipe->destroyed = true;
            if (pipe->clients) {
                for (uint32_t j = 0u; j < pipe->client_capacity; j++) {
                    if (pipe->clients[j].pid != 0u) {
                        (void)ipc_notify_enqueue_locked(
                            pipe->clients[j].pid, 0u,
                            IPO_IPC_EVENT_PIPE_CLOSED, id);
                    }
                }
            }
            process_ipc_wake_waiters(id, IPC_WAIT_PIPE_READ);
            process_ipc_wake_waiters(id, IPC_WAIT_PIPE_WRITE);
            if (!ipc_pipe_has_clients_locked(pipe)) {
                ipc_pipe_clear_locked(pipe);
            }
            continue;
        }
        bool changed = false;
        if (pipe->clients) {
            for (uint32_t j = 0u; j < pipe->client_capacity; j++) {
                if (pipe->clients[j].pid == pid) {
                    (void)ipc_notify_enqueue_locked(
                        pipe->owner_pid, 0u, IPO_IPC_EVENT_PROCESS_EXIT, pid);
                    memset(&pipe->clients[j], 0, sizeof(pipe->clients[j]));
                    changed = true;
                }
            }
        }
        if (changed) {
            process_ipc_wake_waiters(id, IPC_WAIT_PIPE_READ);
            process_ipc_wake_waiters(id, IPC_WAIT_PIPE_WRITE);
            if (pipe->destroyed && !ipc_pipe_has_clients_locked(pipe)) {
                ipc_pipe_clear_locked(pipe);
            }
        }
    }
    for (uint32_t i = 0u; i < ipc_notification_capacity; i++) {
        if (ipc_notifications[i].active && ipc_notifications[i].pid == pid)
            ipc_notification_clear_locked(&ipc_notifications[i]);
    }
    process_ipc_wake_waiters(pid, IPC_WAIT_NOTIFICATION);
    spin_unlock(&ipc_stream_lock);
}

uint32_t ipc_stream_syscall_handler(uint32_t num, uint32_t argc,
                                    uint32_t *argv) {
    if (!ipc_stream_ready) return (uint32_t)IPO_IPC_ERR_NO_SPACE;
    uint32_t args[4] = {0};
    switch (num) {
        case IPO_SYSCALL_IPC_PIPE_CREATE:
            if (!ipc_copy_syscall_args(argc, argv, 3u, args))
                return (uint32_t)IPO_IPC_ERR_INVALID;
            return (uint32_t)ipc_pipe_create(
                (const char *)(uintptr_t)args[0], args[1],
                (uint32_t *)(uintptr_t)args[2]);
        case IPO_SYSCALL_IPC_PIPE_OPEN:
            if (!ipc_copy_syscall_args(argc, argv, 2u, args))
                return (uint32_t)IPO_IPC_ERR_INVALID;
            return (uint32_t)ipc_pipe_open(
                (const char *)(uintptr_t)args[0], args[1]);
        case IPO_SYSCALL_IPC_PIPE_CLOSE:
            if (!ipc_copy_syscall_args(argc, argv, 2u, args))
                return (uint32_t)IPO_IPC_ERR_INVALID;
            return (uint32_t)ipc_pipe_close(args[0], args[1]);
        case IPO_SYSCALL_IPC_PIPE_DESTROY:
            if (!ipc_copy_syscall_args(argc, argv, 1u, args))
                return (uint32_t)IPO_IPC_ERR_INVALID;
            return (uint32_t)ipc_pipe_destroy(args[0]);
        case IPO_SYSCALL_IPC_PIPE_READ:
            if (!ipc_copy_syscall_args(argc, argv, 4u, args))
                return (uint32_t)IPO_IPC_ERR_INVALID;
            return (uint32_t)ipc_pipe_read(
                args[0], (uint8_t *)(uintptr_t)args[1], args[2], args[3]);
        case IPO_SYSCALL_IPC_PIPE_WRITE:
            if (!ipc_copy_syscall_args(argc, argv, 4u, args))
                return (uint32_t)IPO_IPC_ERR_INVALID;
            return (uint32_t)ipc_pipe_write(
                args[0], (const uint8_t *)(uintptr_t)args[1], args[2], args[3]);
        case IPO_SYSCALL_IPC_NOTIFY_SEND:
            if (!ipc_copy_syscall_args(argc, argv, 3u, args))
                return (uint32_t)IPO_IPC_ERR_INVALID;
            return (uint32_t)ipc_notify_send(args[0], args[1], args[2]);
        case IPO_SYSCALL_IPC_NOTIFY_WAIT:
            if (!ipc_copy_syscall_args(argc, argv, 2u, args))
                return (uint32_t)IPO_IPC_ERR_INVALID;
            return (uint32_t)ipc_notify_wait(
                (ipo_ipc_notification_t *)(uintptr_t)args[0], args[1]);
        default:
            return (uint32_t)IPO_IPC_ERR_INVALID;
    }
}
