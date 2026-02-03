#ifndef LIB_KERNEL_PROCESS_H
#define LIB_KERNEL_PROCESS_H

#include <stdint.h>
#include <stddef.h>

void process_init(void);

int process_exec(const char *path, int argc, char **argv);

int process_exec_simple(const char *path);

int process_get_exit_code(void);

#endif // LIB_KERNEL_PROCESS_H
