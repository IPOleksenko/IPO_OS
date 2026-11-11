#ifndef KERNEL_IPC_H
#define KERNEL_IPC_H

#include <stdint.h>
#include <stdbool.h>

bool ipc_init(void);
void ipc_process_terminated(uint32_t pid);
void process_signal_cleanup(uint32_t pid);
uint32_t ipc_syscall_handler(uint32_t num, uint32_t argc, uint32_t *argv);
bool ipc_stream_init(void);
void ipc_stream_process_terminated(uint32_t pid);
uint32_t ipc_stream_syscall_handler(uint32_t num, uint32_t argc,
                                    uint32_t *argv);
bool ipc_shared_memory_init(void);
void ipc_shared_memory_process_terminated(uint32_t pid);
bool ipc_shared_memory_user_range(uint32_t pid, const void *ptr,
                                  uint32_t length);
uint32_t ipc_shared_memory_syscall_handler(uint32_t num, uint32_t argc,
                                           uint32_t *argv);

void ipc_stream_kernel_notify(uint32_t recipient_pid, uint32_t event,
                              uint32_t value);
bool ipc_user_range(const void *ptr, uint32_t length);
bool ipc_copy_name(char *dst, const char *src);
char *ipc_copy_name_alloc(const char *name);

#endif
