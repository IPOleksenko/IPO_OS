#include <system/run_cmd.h>
#include <kernel/process.h>
#include <kernel/async.h>
#include <kernel/terminal.h>
#include <system/timer.h>
#include <system/state.h>
#include <driver/input/keyboard.h>
#include <memory/kmalloc.h>
#include <wm.h>
#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include <stdint.h>

/* =========================================================================
 * Section 1: Lexer
 * ========================================================================= */

typedef struct {
    char  *str;
    bool   is_op;
} token_t;

typedef struct {
    token_t *data;
    int      len;
    int      cap;
} token_list_t;

static void token_list_init(token_list_t *tl) {
    tl->data = NULL; tl->len = 0; tl->cap = 0;
}

static void token_list_free(token_list_t *tl) {
    for (int i = 0; i < tl->len; i++) {
        if (tl->data[i].str) kfree(tl->data[i].str);
    }
    if (tl->data) kfree(tl->data);
    tl->data = NULL; tl->len = 0; tl->cap = 0;
}

static bool token_list_push(token_list_t *tl, char *str, bool is_op) {
    if (tl->len >= tl->cap) {
        int nc = tl->cap ? tl->cap * 2 : 8;
        token_t *nd = kmalloc((size_t)nc * sizeof(token_t));
        if (!nd) return false;
        if (tl->data) { memcpy(nd, tl->data, (size_t)tl->len * sizeof(token_t)); kfree(tl->data); }
        tl->data = nd; tl->cap = nc;
    }
    tl->data[tl->len].str   = str;
    tl->data[tl->len].is_op = is_op;
    tl->len++;
    return true;
}

typedef struct { char *buf; size_t len, cap; } cbuf_t;

static void cbuf_init(cbuf_t *cb) { cb->buf = NULL; cb->len = 0; cb->cap = 0; }

static bool cbuf_append(cbuf_t *cb, char c) {
    if (cb->len + 1 >= cb->cap) {
        size_t nc = cb->cap ? cb->cap * 2 : 64;
        char *nb  = kmalloc(nc);
        if (!nb) return false;
        if (cb->buf) { memcpy(nb, cb->buf, cb->len); kfree(cb->buf); }
        cb->buf = nb; cb->cap = nc;
    }
    cb->buf[cb->len++] = c;
    return true;
}

static bool cbuf_flush(cbuf_t *cb, token_list_t *tl, bool is_op) {
    if (cb->len == 0 && !is_op) return true;
    if (!cbuf_append(cb, '\0')) return false;
    cb->len--;
    char *copy = kmalloc(cb->len + 1);
    if (!copy) return false;
    memcpy(copy, cb->buf, cb->len + 1);
    bool ok = token_list_push(tl, copy, is_op);
    if (!ok) kfree(copy);
    cb->len = 0;
    return ok;
}

static bool lex_cmdline(const char *src, token_list_t *out) {
    token_list_init(out);
    cbuf_t cb; cbuf_init(&cb);
    bool in_double = false, in_single = false, have_content = false;

    while (*src) {
        char c = *src++;

        if (in_double) {
            if (c == '\\' && *src) { if (!cbuf_append(&cb, *src++)) goto oom; }
            else if (c == '"') { in_double = false; }
            else { if (!cbuf_append(&cb, c)) goto oom; }
            have_content = true; continue;
        }
        if (in_single) {
            if (c == '\\' && *src == '\'') { if (!cbuf_append(&cb, '\'')) goto oom; src++; }
            else if (c == '\'') { in_single = false; }
            else { if (!cbuf_append(&cb, c)) goto oom; }
            have_content = true; continue;
        }

        if (c == '\\' && *src) { if (!cbuf_append(&cb, *src++)) goto oom; have_content = true; continue; }
        if (c == '"')  { in_double = true;  have_content = true; continue; }
        if (c == '\'') { in_single = true;  have_content = true; continue; }

        if (c == ' ' || c == '\t') {
            if (have_content || cb.len > 0) { if (!cbuf_flush(&cb, out, false)) goto oom; }
            have_content = false; cb.len = 0; continue;
        }

        if (c == '&' || c == ';' || c == '|') {
            if (have_content || cb.len > 0) { if (!cbuf_flush(&cb, out, false)) goto oom; have_content = false; cb.len = 0; }
            char *op = kmalloc(2); if (!op) goto oom;
            op[0] = c; op[1] = '\0';
            if (!token_list_push(out, op, true)) { kfree(op); goto oom; }
            continue;
        }

        if (!cbuf_append(&cb, c)) goto oom;
        have_content = true;
    }

    if (have_content || cb.len > 0) { if (!cbuf_flush(&cb, out, false)) goto oom; }
    if (cb.buf) kfree(cb.buf);
    return true;
oom:
    if (cb.buf) kfree(cb.buf);
    token_list_free(out);
    return false;
}

/* =========================================================================
 * Section 2: Argv builder
 * ========================================================================= */

typedef struct { char **argv; int argc, cap; } argvec_t;

static void argvec_init(argvec_t *av)  { av->argv = NULL; av->argc = 0; av->cap = 0; }
static void argvec_reset(argvec_t *av) { if (av->argv) kfree(av->argv); av->argv = NULL; av->argc = 0; av->cap = 0; }

static bool argvec_push(argvec_t *av, char *s) {
    if (av->argc + 1 >= av->cap) {
        int nc = av->cap ? av->cap * 2 : RUN_ARGV_INIT_CAP;
        char **nd = kmalloc((size_t)(nc + 1) * sizeof(char *));
        if (!nd) return false;
        if (av->argv) { memcpy(nd, av->argv, (size_t)av->argc * sizeof(char *)); kfree(av->argv); }
        av->argv = nd; av->cap = nc;
    }
    av->argv[av->argc++] = s;
    av->argv[av->argc]   = NULL;
    return true;
}

/* =========================================================================
 * Section 3: Program queue (for & and ;)
 * ========================================================================= */

typedef struct prog_entry {
    char           **argv;   /* deep copy, owned */
    int              argc;
    struct prog_entry *next;
} prog_entry_t;

typedef struct { prog_entry_t *head, *tail; int count; } prog_queue_t;

static void pq_init(prog_queue_t *q) { q->head = q->tail = NULL; q->count = 0; }

static void pq_free(prog_queue_t *q) {
    prog_entry_t *e = q->head;
    while (e) {
        prog_entry_t *nx = e->next;
        for (int i = 0; i < e->argc; i++) if (e->argv[i]) kfree(e->argv[i]);
        kfree(e->argv); kfree(e);
        e = nx;
    }
    q->head = q->tail = NULL; q->count = 0;
}

static bool pq_enqueue(prog_queue_t *q, const argvec_t *av) {
    if (av->argc == 0) return true;
    prog_entry_t *e = kmalloc(sizeof(prog_entry_t));
    if (!e) return false;
    e->next = NULL; e->argc = av->argc;
    e->argv = kmalloc((size_t)(av->argc + 1) * sizeof(char *));
    if (!e->argv) { kfree(e); return false; }
    for (int i = 0; i < av->argc; i++) {
        size_t sl = strlen(av->argv[i]);
        e->argv[i] = kmalloc(sl + 1);
        if (!e->argv[i]) {
            for (int j = 0; j < i; j++) kfree(e->argv[j]);
            kfree(e->argv); kfree(e); return false;
        }
        memcpy(e->argv[i], av->argv[i], sl + 1);
    }
    e->argv[av->argc] = NULL;
    if (q->tail) q->tail->next = e; else q->head = e;
    q->tail = e; q->count++;
    return true;
}

/* =========================================================================
 * Section 4: Execution helpers
 * ========================================================================= */

/**
 * exec_one_pid — execute a single program; return its PID (> 0) or error.
 * Sets last_exit_code via process_exec internals.
 */
static int exec_one_pid(int argc, char **argv) {
    if (argc == 0 || !argv || !argv[0]) return RUN_NOT_FOUND;
    char *path = resolve_command_path(argv[0]);
    if (!path) {
        if (terminal_is_builtin(argv[0])) {
            size_t total_len = 0;
            for (int i = 0; i < argc; i++) {
                total_len += strlen(argv[i]) + 1;
            }
            char *reconstructed = kmalloc(total_len + 1);
            if (reconstructed) {
                char *dst = reconstructed;
                for (int i = 0; i < argc; i++) {
                    if (i > 0) *dst++ = ' ';
                    size_t len = strlen(argv[i]);
                    memcpy(dst, argv[i], len);
                    dst += len;
                }
                *dst = '\0';
                int res = try_execute_command(reconstructed);
                kfree(reconstructed);
                return res;
            }
        }
        printf("run: command not found: %s\n", argv[0]);
        return RUN_NOT_FOUND;
    }
    int ret = process_exec(path, argc, argv);
    kfree(path);
    return ret; /* = PID on success (or negative on error) */
}

/**
 * wait_for_process — spin until the process with @pid is no longer alive.
 * Calls async_scheduler_tick() each iteration so background tasks progress.
 * Returns when the process has fully cleaned up (all async tasks done).
 * Aborts if Ctrl+C is pressed.
 */
static void wait_for_process(int pid) {
    if (pid <= 0) return;
    if (!process_is_alive((uint32_t)pid)) return; /* already done */

    /* Drive the async scheduler until the process disappears */
    uint32_t last_ms = timer_millis();
    while (process_is_alive((uint32_t)pid)) {
        async_scheduler_tick();
        /* Honour Ctrl+C */
        if (system_is_interrupted()) break;
        /* Small busy-wait so we don't hammer the scheduler */
        uint32_t now = timer_millis();
        (void)now; (void)last_ms;
        last_ms = now;
    }
}

/* =========================================================================
 * Section 5: Main entry point
 * ========================================================================= */

int run_cmd_execute(const char *cmdline) {
    if (!cmdline || !*cmdline) {
        printf("Usage: run <prog> [args] [OP <prog> [args] ...]\n");
        printf("  &  parallel   — start all; async programs continue in background\n");
        printf("  ;  sequential — wait for each (incl. async tasks) before next\n");
        printf("  |  pipe       — pass exit-code of left as argv[1] of right\n");
        return RUN_OK;
    }

    /* --- Lex --- */
    token_list_t tl;
    if (!lex_cmdline(cmdline, &tl)) {
        printf("run: out of memory during lexing\n");
        return RUN_ERROR;
    }
    if (tl.len == 0) { token_list_free(&tl); return RUN_OK; }

    /* --- Detect dominant operator --- */
    char dom_op = '\0';
    for (int i = 0; i < tl.len; i++) {
        if (tl.data[i].is_op) { dom_op = tl.data[i].str[0]; break; }
    }

    /* ---- No operator: simple single execution ---- */
    if (dom_op == '\0') {
        argvec_t av; argvec_init(&av);
        for (int i = 0; i < tl.len; i++) {
            if (!tl.data[i].is_op) argvec_push(&av, tl.data[i].str);
        }
        int pid = exec_one_pid(av.argc, av.argv);
        argvec_reset(&av);
        token_list_free(&tl);
        return (pid == RUN_NOT_FOUND) ? RUN_NOT_FOUND : RUN_OK;
    }

    /* ---- Pipe: exit-code flows left → right ---- */
    if (dom_op == '|') {
        int pipe_exit = 0;
        bool first    = true;
        argvec_t av;   argvec_init(&av);

        for (int i = 0; i <= tl.len; i++) {
            bool flush = (i == tl.len) || (tl.data[i].is_op && tl.data[i].str[0] == '|');
            if (!flush) { if (!tl.data[i].is_op) argvec_push(&av, tl.data[i].str); continue; }

            if (av.argc > 0) {
                if (!first) {
                    /* Insert pipe_exit as argv[1] */
                    char code_str[24];
                    snprintf(code_str, sizeof(code_str), "%d", pipe_exit);
                    size_t sl = strlen(code_str);
                    char *code_arg = kmalloc(sl + 1);
                    if (code_arg) {
                        memcpy(code_arg, code_str, sl + 1);
                        int new_argc = av.argc + 1;
                        char **new_argv = kmalloc((size_t)(new_argc + 1) * sizeof(char *));
                        if (new_argv) {
                            new_argv[0] = av.argv[0];
                            new_argv[1] = code_arg;
                            for (int j = 1; j < av.argc; j++) new_argv[j + 1] = av.argv[j];
                            new_argv[new_argc] = NULL;
                            int pid = exec_one_pid(new_argc, new_argv);
                            wait_for_process(pid);
                            pipe_exit = process_get_exit_code();
                            kfree(new_argv);
                        } else {
                            int pid = exec_one_pid(av.argc, av.argv);
                            wait_for_process(pid);
                            pipe_exit = process_get_exit_code();
                        }
                        kfree(code_arg);
                    } else {
                        int pid = exec_one_pid(av.argc, av.argv);
                        wait_for_process(pid);
                        pipe_exit = process_get_exit_code();
                    }
                } else {
                    int pid = exec_one_pid(av.argc, av.argv);
                    wait_for_process(pid);
                    pipe_exit = process_get_exit_code();
                    first = false;
                }
            }
            argvec_reset(&av); argvec_init(&av);
        }
        token_list_free(&tl);
        return RUN_OK;
    }

    /* ---- Build program queue (shared by & and ;) ---- */
    prog_queue_t pq; pq_init(&pq);
    argvec_t av;     argvec_init(&av);

    for (int i = 0; i <= tl.len; i++) {
        bool flush = (i == tl.len) || tl.data[i].is_op;
        if (!flush) { if (!argvec_push(&av, tl.data[i].str)) { argvec_reset(&av); pq_free(&pq); token_list_free(&tl); return RUN_ERROR; } continue; }
        if (av.argc > 0) {
            if (!pq_enqueue(&pq, &av)) { argvec_reset(&av); pq_free(&pq); token_list_free(&tl); return RUN_ERROR; }
        }
        argvec_reset(&av); argvec_init(&av);
    }
    token_list_free(&tl);

    /* ---- Sequential (;) — wait for each program + its async tasks ---- */
    if (dom_op == ';') {
        prog_entry_t *e = pq.head;
        while (e) {
            int pid = exec_one_pid(e->argc, e->argv);
            /* Wait until this process AND all its async tasks are done */
            wait_for_process(pid);
            if (system_is_interrupted()) break;
            e = e->next;
        }
        pq_free(&pq);
        return RUN_OK;
    }

    /* ---- Parallel (&) ----
     * Start all programs concurrently; batch-scheduled via cooperative multitasking.
     */
    if (dom_op == '&') {
        int count = pq.count;
        if (count == 0) {
            pq_free(&pq);
            return RUN_OK;
        }

        process_t **procs = kmalloc((size_t)count * sizeof(process_t *));
        if (!procs) {
            pq_free(&pq);
            return RUN_ERROR;
        }

        int spawned = 0;
        prog_entry_t *e = pq.head;
        while (e) {
            char *path = resolve_command_path(e->argv[0]);
            if (!path) {
                if (terminal_is_builtin(e->argv[0])) {
                    printf("run: '%s' is a shell built-in command (executing synchronously):\n", e->argv[0]);
                    exec_one_pid(e->argc, e->argv);
                    e = e->next;
                    continue;
                }
                printf("run: command not found: %s\n", e->argv[0]);
                e = e->next;
                continue;
            }
            process_t *proc = process_spawn(path, e->argc, e->argv);
            kfree(path);
            if (proc) {
                procs[spawned++] = proc;
            }
            e = e->next;
        }

        if (spawned > 0) {
            process_run_batch(procs, spawned);
        }

        kfree(procs);
        pq_free(&pq);
        return RUN_OK;
    }

    pq_free(&pq);
    return RUN_OK;
}
