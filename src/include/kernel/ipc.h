#ifndef KERNEL_IPC_H
#define KERNEL_IPC_H

#include <stdint.h>
#include <stdbool.h>
#include <types.h>

bool ipc_init(void);
void ipc_process_terminated(ipo_id_t pid);
ipo_uarg_t ipc_syscall_handler(ipo_uarg_t num, ipo_uarg_t argc, ipo_uarg_t *argv);
bool ipc_stream_init(void);
void ipc_stream_process_terminated(ipo_id_t pid);
ipo_uarg_t ipc_stream_syscall_handler(ipo_uarg_t num, ipo_uarg_t argc,
                                      ipo_uarg_t *argv);
bool ipc_shared_memory_init(void);
void ipc_shared_memory_process_terminated(ipo_id_t pid);
bool ipc_shared_memory_user_range(ipo_id_t pid, const void *ptr,
                                  ipo_size_t length);
ipo_uarg_t ipc_shared_memory_syscall_handler(ipo_uarg_t num, ipo_uarg_t argc,
                                             ipo_uarg_t *argv);

void ipc_stream_kernel_notify(ipo_id_t recipient_pid, uint32_t event,
                              uint32_t value);
bool ipc_user_range(const void *ptr, ipo_size_t length);
bool ipc_copy_syscall_args(ipo_uarg_t argc, const ipo_uarg_t *argv,
                           ipo_count_t expected_count, ipo_uarg_t *args);
char *ipc_copy_name_alloc(const char *name);

#endif
