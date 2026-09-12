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
#include <ioport.h>

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
            if (c == '\\' && *src && (*src == '"' || *src == '\\')) { if (!cbuf_append(&cb, *src++)) goto oom; }
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

        if (c == '\\' && *src) {
            char next = *src;
            if (next == ' ' || next == '\t' || next == '"' || next == '\'' || next == '\\' || next == '&' || next == ';' || next == '|') {
                if (!cbuf_append(&cb, *src++)) goto oom;
                have_content = true;
                continue;
            }
            if (!cbuf_append(&cb, '\\')) goto oom;
            have_content = true;
            continue;
        }
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
 * Section 2: AST / Execution Plan structures
 * ========================================================================= */

typedef struct {
    char **argv;
    int argc;
    int cap;
} run_cmd_t;

static void cmd_init(run_cmd_t *c) {
    c->argv = NULL;
    c->argc = 0;
    c->cap = 0;
}

static void cmd_free(run_cmd_t *c) {
    if (c->argv) {
        for (int i = 0; i < c->argc; i++) {
            if (c->argv[i]) kfree(c->argv[i]);
        }
        kfree(c->argv);
    }
    c->argv = NULL;
    c->argc = 0;
    c->cap = 0;
}

static bool cmd_push(run_cmd_t *c, const char *arg) {
    if (c->argc + 1 >= c->cap) {
        int nc = c->cap ? c->cap * 2 : RUN_ARGV_INIT_CAP;
        char **nd = kmalloc((size_t)(nc + 1) * sizeof(char *));
        if (!nd) return false;
        if (c->argv) {
            memcpy(nd, c->argv, (size_t)c->argc * sizeof(char *));
            kfree(c->argv);
        }
        c->argv = nd;
        c->cap = nc;
    }
    size_t len = strlen(arg);
    char *copy = kmalloc(len + 1);
    if (!copy) return false;
    memcpy(copy, arg, len + 1);
    c->argv[c->argc++] = copy;
    c->argv[c->argc] = NULL;
    return true;
}

typedef struct {
    run_cmd_t *cmds;
    int count;
    int cap;
} run_pipeline_t;

static void pipe_init(run_pipeline_t *p) {
    p->cmds = NULL;
    p->count = 0;
    p->cap = 0;
}

static void pipe_free(run_pipeline_t *p) {
    if (p->cmds) {
        for (int i = 0; i < p->count; i++) {
            cmd_free(&p->cmds[i]);
        }
        kfree(p->cmds);
    }
    p->cmds = NULL;
    p->count = 0;
    p->cap = 0;
}

static bool pipe_push(run_pipeline_t *p, run_cmd_t *c) {
    if (p->count >= p->cap) {
        int nc = p->cap ? p->cap * 2 : 4;
        run_cmd_t *nd = kmalloc((size_t)nc * sizeof(run_cmd_t));
        if (!nd) return false;
        if (p->cmds) {
            memcpy(nd, p->cmds, (size_t)p->count * sizeof(run_cmd_t));
            kfree(p->cmds);
        }
        p->cmds = nd;
        p->cap = nc;
    }
    p->cmds[p->count++] = *c;
    return true;
}

typedef struct {
    run_pipeline_t *pipes;
    int count;
    int cap;
} run_stage_t;

static void stage_init(run_stage_t *s) {
    s->pipes = NULL;
    s->count = 0;
    s->cap = 0;
}

static void stage_free(run_stage_t *s) {
    if (s->pipes) {
        for (int i = 0; i < s->count; i++) {
            pipe_free(&s->pipes[i]);
        }
        kfree(s->pipes);
    }
    s->pipes = NULL;
    s->count = 0;
    s->cap = 0;
}

static bool stage_push(run_stage_t *s, run_pipeline_t *p) {
    if (s->count >= s->cap) {
        int nc = s->cap ? s->cap * 2 : 4;
        run_pipeline_t *nd = kmalloc((size_t)nc * sizeof(run_pipeline_t));
        if (!nd) return false;
        if (s->pipes) {
            memcpy(nd, s->pipes, (size_t)s->count * sizeof(run_pipeline_t));
            kfree(s->pipes);
        }
        s->pipes = nd;
        s->cap = nc;
    }
    s->pipes[s->count++] = *p;
    return true;
}

typedef struct {
    run_stage_t *stages;
    int count;
    int cap;
} run_plan_t;

static void plan_init(run_plan_t *pl) {
    pl->stages = NULL;
    pl->count = 0;
    pl->cap = 0;
}

static void plan_free(run_plan_t *pl) {
    if (pl->stages) {
        for (int i = 0; i < pl->count; i++) {
            stage_free(&pl->stages[i]);
        }
        kfree(pl->stages);
    }
    pl->stages = NULL;
    pl->count = 0;
    pl->cap = 0;
}

static bool plan_push(run_plan_t *pl, run_stage_t *s) {
    if (pl->count >= pl->cap) {
        int nc = pl->cap ? pl->cap * 2 : 4;
        run_stage_t *nd = kmalloc((size_t)nc * sizeof(run_stage_t));
        if (!nd) return false;
        if (pl->stages) {
            memcpy(nd, pl->stages, (size_t)pl->count * sizeof(run_stage_t));
            kfree(pl->stages);
        }
        pl->stages = nd;
        pl->cap = nc;
    }
    pl->stages[pl->count++] = *s;
    return true;
}

static bool build_run_plan(const token_list_t *tl, run_plan_t *plan) {
    plan_init(plan);

    run_stage_t cur_stage;
    stage_init(&cur_stage);

    run_pipeline_t cur_pipe;
    pipe_init(&cur_pipe);

    run_cmd_t cur_cmd;
    cmd_init(&cur_cmd);

    for (int i = 0; i < tl->len; i++) {
        if (!tl->data[i].is_op) {
            if (!cmd_push(&cur_cmd, tl->data[i].str)) goto oom;
        } else {
            char op = tl->data[i].str[0];
            if (op == '|') {
                if (cur_cmd.argc > 0) {
                    if (!pipe_push(&cur_pipe, &cur_cmd)) goto oom;
                    cmd_init(&cur_cmd);
                }
            } else if (op == '&') {
                if (cur_cmd.argc > 0) {
                    if (!pipe_push(&cur_pipe, &cur_cmd)) goto oom;
                    cmd_init(&cur_cmd);
                }
                if (cur_pipe.count > 0) {
                    if (!stage_push(&cur_stage, &cur_pipe)) goto oom;
                    pipe_init(&cur_pipe);
                }
            } else if (op == ';') {
                if (cur_cmd.argc > 0) {
                    if (!pipe_push(&cur_pipe, &cur_cmd)) goto oom;
                    cmd_init(&cur_cmd);
                }
                if (cur_pipe.count > 0) {
                    if (!stage_push(&cur_stage, &cur_pipe)) goto oom;
                    pipe_init(&cur_pipe);
                }
                if (cur_stage.count > 0) {
                    if (!plan_push(plan, &cur_stage)) goto oom;
                    stage_init(&cur_stage);
                }
            }
        }
    }

    /* Flush trailing elements */
    if (cur_cmd.argc > 0) {
        if (!pipe_push(&cur_pipe, &cur_cmd)) goto oom;
        cmd_init(&cur_cmd);
    } else {
        cmd_free(&cur_cmd);
    }

    if (cur_pipe.count > 0) {
        if (!stage_push(&cur_stage, &cur_pipe)) goto oom;
        pipe_init(&cur_pipe);
    } else {
        pipe_free(&cur_pipe);
    }

    if (cur_stage.count > 0) {
        if (!plan_push(plan, &cur_stage)) goto oom;
        stage_init(&cur_stage);
    } else {
        stage_free(&cur_stage);
    }

    return true;

oom:
    cmd_free(&cur_cmd);
    pipe_free(&cur_pipe);
    stage_free(&cur_stage);
    plan_free(plan);
    return false;
}

/* =========================================================================
 * Section 3: Execution helpers
 * ========================================================================= */

/**
 * exec_one_pid — execute a single program; return its PID (> 0) or error.
 * Sets last_exit_code via process_exec internals or process_set_last_exit_code.
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
                process_set_last_exit_code(0);
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
 * wait_for_command — spin until the process with @pid is no longer alive,
 * and if a window manager (WM) session is active, wait until the user exits
 * the WM session (e.g. by clicking [Exit] on the taskbar).
 *
 * This ensures sequential operators (';' and '|') wait for the GUI / command
 * to finish before launching subsequent commands, rather than executing them
 * simultaneously.
 */
static void wait_for_command(int pid) {
    while (pid > 0 && pid != 1000 && process_is_alive((uint32_t)pid)) {
        async_scheduler_tick();
        if (wm_session_active()) {
            wm_compositor_tick();
        }
        if (system_is_interrupted()) break;
        io_wait();
    }

    while (wm_session_active()) {
        async_scheduler_tick();
        wm_compositor_tick();
        if (system_is_interrupted()) break;
        io_wait();
    }
}

static int exec_pipeline(const run_pipeline_t *pipe) {
    if (pipe->count == 0) return RUN_OK;

    int pipe_exit = 0;
    for (int i = 0; i < pipe->count; i++) {
        const run_cmd_t *cmd = &pipe->cmds[i];
        if (cmd->argc == 0) continue;

        int ret = 0;
        if (i == 0) {
            ret = exec_one_pid(cmd->argc, cmd->argv);
            wait_for_command(ret);
            pipe_exit = process_get_exit_code();
            if (ret > 0 && ret != 1000 && terminal_get_show_return_value()) {
                printf("Return value: %d\n", pipe_exit);
            }
        } else {
            /* Prepend pipe_exit as argv[1] */
            char code_str[24];
            snprintf(code_str, sizeof(code_str), "%d", pipe_exit);
            size_t sl = strlen(code_str);
            char *code_arg = kmalloc(sl + 1);
            if (!code_arg) {
                ret = exec_one_pid(cmd->argc, cmd->argv);
                wait_for_command(ret);
                pipe_exit = process_get_exit_code();
                if (ret > 0 && ret != 1000 && terminal_get_show_return_value()) {
                    printf("Return value: %d\n", pipe_exit);
                }
            } else {
                memcpy(code_arg, code_str, sl + 1);
                int new_argc = cmd->argc + 1;
                char **new_argv = kmalloc((size_t)(new_argc + 1) * sizeof(char *));
                if (!new_argv) {
                    kfree(code_arg);
                    ret = exec_one_pid(cmd->argc, cmd->argv);
                    wait_for_command(ret);
                    pipe_exit = process_get_exit_code();
                    if (ret > 0 && ret != 1000 && terminal_get_show_return_value()) {
                        printf("Return value: %d\n", pipe_exit);
                    }
                } else {
                    new_argv[0] = cmd->argv[0];
                    new_argv[1] = code_arg;
                    for (int j = 1; j < cmd->argc; j++) {
                        new_argv[j + 1] = cmd->argv[j];
                    }
                    new_argv[new_argc] = NULL;
                    ret = exec_one_pid(new_argc, new_argv);
                    wait_for_command(ret);
                    pipe_exit = process_get_exit_code();
                    kfree(code_arg);
                    kfree(new_argv);
                    if (ret > 0 && ret != 1000 && terminal_get_show_return_value()) {
                        printf("Return value: %d\n", pipe_exit);
                    }
                }
            }
        }

        if (system_is_interrupted()) {
            break;
        }
    }
    return RUN_OK;
}

static int exec_stage(const run_stage_t *stage) {
    if (stage->count == 0) return RUN_OK;

    if (stage->count == 1) {
        /* Single pipeline (sequential or single command) */
        return exec_pipeline(&stage->pipes[0]);
    }

    /* Multiple pipelines in stage -> parallel batch execution (&) */
    int total_procs = 0;
    process_t **procs = kmalloc((size_t)stage->count * sizeof(process_t *));
    if (!procs) return RUN_ERROR;

    for (int p = 0; p < stage->count; p++) {
        const run_pipeline_t *pipe = &stage->pipes[p];
        if (pipe->count == 0) continue;

        if (pipe->count == 1) {
            const run_cmd_t *cmd = &pipe->cmds[0];
            char *path = resolve_command_path(cmd->argv[0]);
            if (!path) {
                if (terminal_is_builtin(cmd->argv[0])) {
                    exec_one_pid(cmd->argc, cmd->argv);
                    continue;
                }
                printf("run: command not found: %s\n", cmd->argv[0]);
                continue;
            }
            process_t *proc = process_spawn(path, cmd->argc, cmd->argv);
            kfree(path);
            if (proc) {
                procs[total_procs++] = proc;
            }
        } else {
            /* If a branch is a pipeline, execute it directly */
            exec_pipeline(pipe);
        }
    }

    if (total_procs > 0) {
        process_run_batch(procs, total_procs);
        if (terminal_get_show_return_value()) {
            for (int i = 0; i < total_procs; i++) {
                printf("Return value: %d\n", process_get_batch_exit_code(i));
            }
        }
    } else {
        /* If no external processes were spawned but WM session is active, wait until exit */
        while (wm_session_active()) {
            async_scheduler_tick();
            wm_compositor_tick();
            if (system_is_interrupted()) break;
            io_wait();
        }
    }

    kfree(procs);
    return RUN_OK;
}

/* =========================================================================
 * Section 4: Main entry point
 * ========================================================================= */

int run_cmd_execute(const char *cmdline) {
    if (!cmdline || !*cmdline) {
        printf("Usage: run <prog> [args] [OP <prog> [args] ...]\n");
        printf("  &  parallel   — start all in parallel batch; cooperative multitasking\n");
        printf("  ;  sequential — wait for each batch/program before next stage\n");
        printf("  |  pipe       — pass exit-code of left as argv[1] of right\n");
        return RUN_BUILTIN_OK;
    }

    /* --- Lex --- */
    token_list_t tl;
    if (!lex_cmdline(cmdline, &tl)) {
        printf("run: out of memory during lexing\n");
        return RUN_ERROR;
    }
    if (tl.len == 0) {
        token_list_free(&tl);
        return RUN_BUILTIN_OK;
    }

    /* --- Build hierarchical execution plan (Stages ';' -> Parallel '&' -> Pipe '|') --- */
    run_plan_t plan;
    if (!build_run_plan(&tl, &plan)) {
        token_list_free(&tl);
        printf("run: out of memory building execution plan\n");
        return RUN_ERROR;
    }
    token_list_free(&tl);

    int result = RUN_OK;
    for (int s = 0; s < plan.count; s++) {
        result = exec_stage(&plan.stages[s]);
        if (system_is_interrupted()) {
            break;
        }
    }

    plan_free(&plan);
    return result;
}
