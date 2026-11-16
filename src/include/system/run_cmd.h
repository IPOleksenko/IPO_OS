#ifndef SYSTEM_RUN_CMD_H
#define SYSTEM_RUN_CMD_H

#include <stdint.h>
#include <stdbool.h>

/* Initial argv capacity; grows dynamically via krealloc */
#define RUN_ARGV_INIT_CAP 16

/* Return codes match try_execute_command */
#define RUN_OK          1000   /* handled; programs print their own return */
#define RUN_BUILTIN_OK  1000
#define RUN_NOT_FOUND      0
#define RUN_ERROR         -1

/* Parse and run a `run` command (cmdline starts after "run ").
 * Returns RUN_OK, RUN_NOT_FOUND, or <0 on error. */
int run_cmd_execute(const char *cmdline);

#endif /* SYSTEM_RUN_CMD_H */

