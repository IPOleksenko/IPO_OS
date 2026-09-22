#ifndef SYSTEM_RUN_CMD_H
#define SYSTEM_RUN_CMD_H

#include <stdint.h>
#include <stdbool.h>

/* Maximum arguments per individual program invocation (dynamic; this is the
   initial capacity — it grows automatically via kmalloc/krealloc). */
#define RUN_ARGV_INIT_CAP 16

/* Result codes returned by run_cmd_execute (mirrors try_execute_command). */
#define RUN_OK          1000   /* success (programs print their own return; 'run' itself stays silent) */
#define RUN_BUILTIN_OK  1000   /* builtin-style success */
#define RUN_NOT_FOUND      0
#define RUN_ERROR         -1

/**
 * run_cmd_execute - parse and execute a `run` command line.
 *
 * @cmdline: the full command line string starting *after* the "run " prefix,
 *           i.e. the arguments to `run` itself.
 *
 * Returns RUN_OK on success, RUN_NOT_FOUND if nothing could be resolved,
 * or a negative error code on fatal failure.
 */
int run_cmd_execute(const char *cmdline);

#endif /* SYSTEM_RUN_CMD_H */

