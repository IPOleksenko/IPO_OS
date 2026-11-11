#ifndef IPO_IPC_H
#define IPO_IPC_H

#include <stdint.h>
#include <syscall.h>

#define IPO_IPC_RIGHT_SEND    0x01u
#define IPO_IPC_RIGHT_RECEIVE 0x02u
#define IPO_IPC_PIPE_RIGHT_WRITE IPO_IPC_RIGHT_SEND
#define IPO_IPC_PIPE_RIGHT_READ  IPO_IPC_RIGHT_RECEIVE

#define IPO_IPC_NONBLOCK 0x01u

#define IPO_IPC_EVENT_PROCESS_EXIT      1u
#define IPO_IPC_EVENT_ENDPOINT_CLOSED   2u
#define IPO_IPC_EVENT_PIPE_CLOSED       3u

#define IPO_SHM_RIGHT_READ  0x01u
#define IPO_SHM_RIGHT_WRITE 0x02u

#define IPO_IPC_OK                0
#define IPO_IPC_ERR_INVALID      -1
#define IPO_IPC_ERR_NOT_FOUND    -2
#define IPO_IPC_ERR_PERMISSION   -3
#define IPO_IPC_ERR_WOULD_BLOCK  -4
#define IPO_IPC_ERR_TOO_LARGE    -6
#define IPO_IPC_ERR_NO_SPACE     -7
#define IPO_IPC_ERR_EXISTS       -8
#define IPO_IPC_ERR_CLOSED       -9

typedef struct {
    uint32_t sender_pid;
    uint32_t event;
    uint32_t value;
} ipo_ipc_notification_t;

static inline int ipo_ipc_create(const char *name, uint32_t public_rights) {
    uint32_t args[2] = {(uint32_t)(uintptr_t)name, public_rights};
    return ipo_syscall(IPO_SYSCALL_IPC_CREATE, 2u, args);
}

static inline int ipo_ipc_open(const char *name, uint32_t rights) {
    uint32_t args[2] = {(uint32_t)(uintptr_t)name, rights};
    return ipo_syscall(IPO_SYSCALL_IPC_OPEN, 2u, args);
}

static inline int ipo_ipc_close(int endpoint) {
    uint32_t args[1] = {(uint32_t)endpoint};
    return ipo_syscall(IPO_SYSCALL_IPC_CLOSE, 1u, args);
}

static inline int ipo_ipc_destroy(int endpoint) {
    uint32_t args[1] = {(uint32_t)endpoint};
    return ipo_syscall(IPO_SYSCALL_IPC_DESTROY, 1u, args);
}

static inline int ipo_ipc_send(int endpoint, const void *message, uint32_t length,
                               uint32_t flags) {
    uint32_t args[4] = {
        (uint32_t)endpoint,
        (uint32_t)(uintptr_t)message,
        length,
        flags
    };
    return ipo_syscall(IPO_SYSCALL_IPC_SEND, 4u, args);
}

static inline int ipo_ipc_try_send(int endpoint, const void *message,
                                   uint32_t length) {
    return ipo_ipc_send(endpoint, message, length, IPO_IPC_NONBLOCK);
}

static inline int ipo_ipc_receive(int endpoint, void *buffer, uint32_t capacity,
                                  uint32_t *sender_pid, uint32_t *message_length,
                                  uint32_t flags) {
    uint32_t args[6] = {
        (uint32_t)endpoint,
        (uint32_t)(uintptr_t)buffer,
        capacity,
        (uint32_t)(uintptr_t)sender_pid,
        (uint32_t)(uintptr_t)message_length,
        flags
    };
    return ipo_syscall(IPO_SYSCALL_IPC_RECEIVE, 6u, args);
}

static inline int ipo_ipc_try_receive(int endpoint, void *buffer,
                                      uint32_t capacity, uint32_t *sender_pid,
                                      uint32_t *message_length) {
    return ipo_ipc_receive(endpoint, buffer, capacity, sender_pid,
                           message_length, IPO_IPC_NONBLOCK);
}

static inline int ipo_ipc_pipe_create(const char *name, uint32_t public_rights,
                                      int *pipe_handle) {
    uint32_t args[3] = {
        (uint32_t)(uintptr_t)name,
        public_rights,
        (uint32_t)(uintptr_t)pipe_handle
    };
    return ipo_syscall(IPO_SYSCALL_IPC_PIPE_CREATE, 3u, args);
}

static inline int ipo_ipc_pipe_open(const char *name, uint32_t rights) {
    uint32_t args[2] = {(uint32_t)(uintptr_t)name, rights};
    return ipo_syscall(IPO_SYSCALL_IPC_PIPE_OPEN, 2u, args);
}

static inline int ipo_ipc_pipe_close(int pipe_handle, uint32_t rights) {
    uint32_t args[2] = {(uint32_t)pipe_handle, rights};
    return ipo_syscall(IPO_SYSCALL_IPC_PIPE_CLOSE, 2u, args);
}

static inline int ipo_ipc_pipe_destroy(int pipe_handle) {
    uint32_t args[1] = {(uint32_t)pipe_handle};
    return ipo_syscall(IPO_SYSCALL_IPC_PIPE_DESTROY, 1u, args);
}

static inline int ipo_ipc_pipe_read(int pipe_handle, void *buffer,
                                    uint32_t capacity, uint32_t flags) {
    uint32_t args[4] = {
        (uint32_t)pipe_handle,
        (uint32_t)(uintptr_t)buffer,
        capacity,
        flags
    };
    return ipo_syscall(IPO_SYSCALL_IPC_PIPE_READ, 4u, args);
}

static inline int ipo_ipc_pipe_write(int pipe_handle, const void *buffer,
                                     uint32_t length, uint32_t flags) {
    uint32_t args[4] = {
        (uint32_t)pipe_handle,
        (uint32_t)(uintptr_t)buffer,
        length,
        flags
    };
    return ipo_syscall(IPO_SYSCALL_IPC_PIPE_WRITE, 4u, args);
}

static inline int ipo_ipc_notify_send(uint32_t recipient_pid, uint32_t event,
                                      uint32_t value) {
    uint32_t args[3] = {recipient_pid, event, value};
    return ipo_syscall(IPO_SYSCALL_IPC_NOTIFY_SEND, 3u, args);
}

static inline int ipo_ipc_notify_wait(ipo_ipc_notification_t *notification,
                                      uint32_t flags) {
    uint32_t args[2] = {(uint32_t)(uintptr_t)notification, flags};
    return ipo_syscall(IPO_SYSCALL_IPC_NOTIFY_WAIT, 2u, args);
}

static inline int ipo_shm_create(const char *name, uint32_t size,
                                 uint32_t public_rights) {
    uint32_t args[3] = {
        (uint32_t)(uintptr_t)name, size, public_rights
    };
    return ipo_syscall(IPO_SYSCALL_SHM_CREATE, 3u, args);
}

static inline int ipo_shm_open(const char *name, uint32_t rights) {
    uint32_t args[2] = {(uint32_t)(uintptr_t)name, rights};
    return ipo_syscall(IPO_SYSCALL_SHM_OPEN, 2u, args);
}

static inline int ipo_shm_attach(int handle, void **address, uint32_t *size) {
    uint32_t args[3] = {
        (uint32_t)handle,
        (uint32_t)(uintptr_t)address,
        (uint32_t)(uintptr_t)size
    };
    return ipo_syscall(IPO_SYSCALL_SHM_ATTACH, 3u, args);
}

static inline int ipo_shm_detach(int handle) {
    uint32_t args[1] = {(uint32_t)handle};
    return ipo_syscall(IPO_SYSCALL_SHM_DETACH, 1u, args);
}

static inline int ipo_shm_destroy(int handle) {
    uint32_t args[1] = {(uint32_t)handle};
    return ipo_syscall(IPO_SYSCALL_SHM_DESTROY, 1u, args);
}

#endif
