#include <system/run_cmd.h>
#include <kernel/process.h>
#include <kernel/terminal.h>
#include <system/state.h>
#include <memory/kmalloc.h>
#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include <stdint.h>
#include <limits.h>

static bool grow_buf(void **ptr, int *cap, int count, int extra,
                     size_t elem_size, int init_cap) {
    if (count > INT_MAX - extra) return false;
    if (count + extra <= *cap) return true;
    int nc = (*cap > 0) ? *cap : init_cap;
    while (nc < count + extra) {
        if (nc > INT_MAX / 2) return false;
        nc *= 2;
    }
    if ((size_t)nc > SIZE_MAX / elem_size) return false;
    void *nd = kmalloc((size_t)nc * elem_size);
    if (!nd) return false;
    if (*ptr && count > 0) {
        memcpy(nd, *ptr, (size_t)count * elem_size);
        kfree(*ptr);
    }
    *ptr = nd;
    *cap = nc;
    return true;
}

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
    tl->data = NULL;
    tl->len = 0;
    tl->cap = 0;
}

static void token_list_free(token_list_t *tl) {
    for (int i = 0; i < tl->len; i++) {
        if (tl->data[i].str) kfree(tl->data[i].str);
    }
    if (tl->data) kfree(tl->data);
    tl->data = NULL;
    tl->len = 0;
    tl->cap = 0;
}

static bool token_list_push(token_list_t *tl, char *str, bool is_op) {
    if (!grow_buf((void **)&tl->data, &tl->cap, tl->len, 1, sizeof(token_t), 8)) {
        return false;
    }
    tl->data[tl->len].str   = str;
    tl->data[tl->len].is_op = is_op;
    tl->len++;
    return true;
}

typedef struct {
    char *buf;
    int   len;
    int   cap;
} cbuf_t;

static void cbuf_init(cbuf_t *cb) {
    cb->buf = NULL;
    cb->len = 0;
    cb->cap = 0;
}

static bool cbuf_append(cbuf_t *cb, char c) {
    if (!grow_buf((void **)&cb->buf, &cb->cap, cb->len, 1, sizeof(char), 64)) {
        return false;
    }
    cb->buf[cb->len++] = c;
    return true;
}

static bool cbuf_flush(cbuf_t *cb, token_list_t *tl, bool is_op) {
    if (cb->len == 0 && !is_op) return true;
    char *copy = kmalloc((size_t)cb->len + 1u);
    if (!copy) return false;
    if (cb->len > 0) memcpy(copy, cb->buf, (size_t)cb->len);
    copy[cb->len] = '\0';
    bool ok = token_list_push(tl, copy, is_op);
    if (!ok) kfree(copy);
    cb->len = 0;
    return ok;
}

static bool lex_cmdline(const char *src, token_list_t *out) {
    token_list_init(out);
    cbuf_t cb;
    cbuf_init(&cb);
    bool in_double = false, in_single = false, have_content = false;

    while (*src) {
        char c = *src++;

        if (in_double) {
            if (c == '\\' && (*src == '"' || *src == '\\')) {
                if (!cbuf_append(&cb, *src++)) goto oom;
            } else if (c == '"') {
                in_double = false;
            } else {
                if (!cbuf_append(&cb, c)) goto oom;
            }
            have_content = true;
            continue;
        }
        if (in_single) {
            if (c == '\\' && (*src == '\'' || *src == '\\')) {
                if (!cbuf_append(&cb, *src++)) goto oom;
            } else if (c == '\'') {
                in_single = false;
            } else {
                if (!cbuf_append(&cb, c)) goto oom;
            }
            have_content = true;
            continue;
        }

        if (c == '\\' && *src) {
            char next = *src;
            if (next == ' ' || next == '\t' || next == '"' || next == '\'' ||
                next == '\\' || next == '&' || next == ';' || next == '|') {
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
            if (have_content || cb.len > 0) {
                if (!cbuf_flush(&cb, out, false)) goto oom;
            }
            have_content = false;
            cb.len = 0;
            continue;
        }

        if (c == '&' || c == ';' || c == '|') {
            if (have_content || cb.len > 0) {
                if (!cbuf_flush(&cb, out, false)) goto oom;
                have_content = false;
                cb.len = 0;
            }
            if ((c == '&' || c == '|') && *src == c) {
                src++;
                char *op = kmalloc(3);
                if (!op) goto oom;
                op[0] = c; op[1] = c; op[2] = '\0';
                if (!token_list_push(out, op, true)) { kfree(op); goto oom; }
            } else {
                char *op = kmalloc(2);
                if (!op) goto oom;
                op[0] = c; op[1] = '\0';
                if (!token_list_push(out, op, true)) { kfree(op); goto oom; }
            }
            continue;
        }

        if (!cbuf_append(&cb, c)) goto oom;
        have_content = true;
    }

    if (have_content || cb.len > 0) {
        if (!cbuf_flush(&cb, out, false)) goto oom;
    }
    if (cb.buf) kfree(cb.buf);
    return true;
oom:
    if (cb.buf) kfree(cb.buf);
    token_list_free(out);
    return false;
}

typedef struct {
    char **argv;
    int argc;
    int cap;
    int window;
} run_cmd_t;

static void cmd_init(run_cmd_t *c) {
    c->argv = NULL;
    c->argc = 0;
    c->cap = 0;
    c->window = 0;
}

static void cmd_free(run_cmd_t *c) {
    if (c->argv) {
        for (int i = 0; i < c->argc; i++) {
            if (c->argv[i]) kfree(c->argv[i]);
        }
        kfree(c->argv);
    }
    cmd_init(c);
}

static bool cmd_push(run_cmd_t *c, const char *arg) {
    if (!grow_buf((void **)&c->argv, &c->cap, c->argc, 2,
                  sizeof(char *), RUN_ARGV_INIT_CAP + 1)) {
        return false;
    }
    size_t len = strlen(arg);
    char *copy = kmalloc(len + 1);
    if (!copy) return false;
    memcpy(copy, arg, len + 1);
    c->argv[c->argc++] = copy;
    c->argv[c->argc] = NULL;
    return true;
}

static void cmd_strip_redundant_run(run_cmd_t *c) {
    while (c->argc > 1 && strcmp(c->argv[0], "run") == 0) {
        kfree(c->argv[0]);
        for (int i = 0; i < c->argc - 1; i++) {
            c->argv[i] = c->argv[i + 1];
        }
        c->argc--;
        c->argv[c->argc] = NULL;
    }
}

static bool cmd_prepend_return_args(run_cmd_t *cmd,
                                    const process_return_value_t *last_val,
                                    int last_exit_code) {
    if (cmd->argc == 0) return true;

    uint32_t pass_count = 0;
    char **pass_args = NULL;
    char exit_buf[16];
    char *exit_ptr = exit_buf;

    if (last_val && last_val->arg_count > 0 && last_val->arg_vals) {
        pass_count = last_val->arg_count;
        pass_args = last_val->arg_vals;
    } else if (last_val && last_val->str_val && last_val->str_val[0] != '\0') {
        pass_count = 1;
        pass_args = (char **)&last_val->str_val;
    } else {
        snprintf(exit_buf, sizeof(exit_buf), "%d", last_exit_code);
        pass_count = 1;
        pass_args = &exit_ptr;
    }

    if (pass_count > (uint32_t)(INT_MAX - cmd->argc - 1)) return false;
    int new_argc = cmd->argc + (int)pass_count;
    char **new_argv = kmalloc((size_t)(new_argc + 1) * sizeof(char *));
    if (!new_argv) return false;

    new_argv[0] = cmd->argv[0];
    for (uint32_t k = 0; k < pass_count; k++) {
        const char *src = pass_args[k] ? pass_args[k] : "";
        size_t slen = strlen(src);
        char *copy = kmalloc(slen + 1);
        if (!copy) {
            for (uint32_t r = 0; r < k; r++) kfree(new_argv[1 + r]);
            kfree(new_argv);
            return false;
        }
        memcpy(copy, src, slen + 1);
        new_argv[1 + k] = copy;
    }
    for (int j = 1; j < cmd->argc; j++) {
        new_argv[pass_count + (uint32_t)j] = cmd->argv[j];
    }
    new_argv[new_argc] = NULL;

    kfree(cmd->argv);
    cmd->argv = new_argv;
    cmd->argc = new_argc;
    cmd->cap = new_argc + 1;
    return true;
}

typedef struct {
    run_cmd_t *cmds;
    int count;
    int cap;
    bool pipe_to_next;
} run_stage_t;

static void stage_init(run_stage_t *s) {
    s->cmds = NULL;
    s->count = 0;
    s->cap = 0;
    s->pipe_to_next = false;
}

static void stage_free(run_stage_t *s) {
    if (s->cmds) {
        for (int i = 0; i < s->count; i++) {
            cmd_free(&s->cmds[i]);
        }
        kfree(s->cmds);
    }
    stage_init(s);
}

static bool stage_push_cmd(run_stage_t *s, run_cmd_t *c) {
    if (!grow_buf((void **)&s->cmds, &s->cap, s->count, 1, sizeof(run_cmd_t), 4)) {
        return false;
    }
    s->cmds[s->count++] = *c;
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
    plan_init(pl);
}

static bool plan_push_stage(run_plan_t *pl, run_stage_t *s) {
    if (!grow_buf((void **)&pl->stages, &pl->cap, pl->count, 1, sizeof(run_stage_t), 4)) {
        return false;
    }
    pl->stages[pl->count++] = *s;
    return true;
}

static bool flush_cmd_to_stage(run_cmd_t *cmd, run_stage_t *stage, int cur_window) {
    if (cmd->argc <= 0) return true;
    cmd_strip_redundant_run(cmd);
    cmd->window = cur_window;
    if (!stage_push_cmd(stage, cmd)) return false;
    cmd_init(cmd);
    return true;
}

static bool flush_stage_to_plan(run_stage_t *stage, run_plan_t *plan,
                                bool pipe_to_next, int *cur_window) {
    if (stage->count <= 0) return true;
    stage->pipe_to_next = pipe_to_next;
    if (!plan_push_stage(plan, stage)) return false;
    stage_init(stage);
    *cur_window = 0;
    return true;
}

static bool build_run_plan(const token_list_t *tl, run_plan_t *plan) {
    plan_init(plan);

    run_stage_t cur_stage;
    stage_init(&cur_stage);

    run_cmd_t cur_cmd;
    cmd_init(&cur_cmd);

    int cur_window = 0;

    for (int i = 0; i < tl->len; i++) {
        if (!tl->data[i].is_op) {
            if (!cmd_push(&cur_cmd, tl->data[i].str)) goto fail;
            continue;
        }
        const char *op_str = tl->data[i].str;
        if (strcmp(op_str, "&") == 0) {
            if (cur_cmd.argc > 0) {
                if (!flush_cmd_to_stage(&cur_cmd, &cur_stage, cur_window)) goto fail;
                cur_window++;
            }
        } else if (strcmp(op_str, "&&") == 0) {
            if (!flush_cmd_to_stage(&cur_cmd, &cur_stage, cur_window)) goto fail;
        } else if (strcmp(op_str, ";") == 0 || strcmp(op_str, "|") == 0) {
            if (!flush_cmd_to_stage(&cur_cmd, &cur_stage, cur_window)) goto fail;
            bool is_pipe = (op_str[0] == '|');
            if (!flush_stage_to_plan(&cur_stage, plan, is_pipe, &cur_window)) goto fail;
        } else {
            printf("run: unsupported operator '%s'\n", op_str);
            goto fail;
        }
    }

    if (cur_cmd.argc > 0) {
        if (!flush_cmd_to_stage(&cur_cmd, &cur_stage, cur_window)) goto fail;
    } else {
        cmd_free(&cur_cmd);
    }

    if (cur_stage.count > 0) {
        if (!flush_stage_to_plan(&cur_stage, plan, false, &cur_window)) goto fail;
    } else {
        stage_free(&cur_stage);
    }

    return true;

fail:
    cmd_free(&cur_cmd);
    stage_free(&cur_stage);
    plan_free(plan);
    return false;
}

static int exec_stage(run_stage_t *stage, const process_return_value_t *prev_return_val,
                      int prev_exit_code, bool has_pipe_input, bool *out_ran_proc) {
    if (out_ran_proc) *out_ran_proc = false;
    if (stage->count == 0) return RUN_OK;

    if (has_pipe_input && stage->count > 0) {
        if (!cmd_prepend_return_args(&stage->cmds[0], prev_return_val, prev_exit_code)) {
            return RUN_ERROR;
        }
    }

    int total_procs = 0;
    process_t **procs = kmalloc((size_t)stage->count * sizeof(process_t *));
    if (!procs) return RUN_ERROR;
    int *wins = kmalloc((size_t)stage->count * sizeof(int));
    if (!wins) {
        kfree(procs);
        return RUN_ERROR;
    }

    for (int i = 0; i < stage->count; i++) {
        run_cmd_t *cmd = &stage->cmds[i];
        if (cmd->argc == 0) continue;
        char *path = resolve_command_path(cmd->argv[0]);
        if (!path) {
            printf("run: program not found: %s\n", cmd->argv[0]);
            continue;
        }
        process_t *proc = process_spawn(path, cmd->argc, cmd->argv);
        kfree(path);
        if (proc) {
            wins[total_procs] = cmd->window;
            procs[total_procs++] = proc;
        }
    }

    int stage_exit = RUN_ERROR;
    if (total_procs > 0) {
        if (out_ran_proc) *out_ran_proc = true;
        process_run_batch_windows(procs, total_procs, wins);

        if (terminal_get_show_return_value()) {
            for (int k = 0; k < total_procs; k++) {
                process_print_batch_return_value(k);
            }
        }
        stage_exit = process_get_batch_exit_code(total_procs - 1);
    }

    kfree(wins);
    kfree(procs);
    process_set_last_exit_code(stage_exit);
    return stage_exit;
}

int run_cmd_execute(const char *cmdline) {
    if (!cmdline || !*cmdline) {
        printf("Usage: run <prog> [args] [OP <prog> [args] ...]\n");
        printf("Operators:\n");
        printf("  &   — run each program in a separate workspace\n");
        printf("  &&  — run asynchronously in one workspace\n");
        printf("  |   — wait for left program to complete and pass return code to right\n");
        printf("  ;   — wait until program completes, then run next program\n");
        printf("Hotkeys:\n");
        printf("  Ctrl + Page Up   — switch to previous workspace\n");
        printf("  Ctrl + Page Down — switch to next workspace\n");
        printf("  Ctrl + C        — close completed workspace or interrupt active program\n");
        return RUN_BUILTIN_OK;
    }

    token_list_t tl;
    if (!lex_cmdline(cmdline, &tl)) {
        printf("run: out of memory during lexing\n");
        return RUN_ERROR;
    }
    if (tl.len == 0) {
        token_list_free(&tl);
        return RUN_BUILTIN_OK;
    }

    run_plan_t plan;
    if (!build_run_plan(&tl, &plan)) {
        token_list_free(&tl);
        return RUN_ERROR;
    }
    token_list_free(&tl);

    int last_exit = 0;
    bool has_pipe_input = false;
    process_return_value_t last_ret;
    process_return_val_init(&last_ret);

    for (int s = 0; s < plan.count; s++) {
        bool ran_proc = false;
        last_exit = exec_stage(&plan.stages[s], &last_ret, last_exit,
                               has_pipe_input, &ran_proc);

        has_pipe_input = plan.stages[s].pipe_to_next;
        if (has_pipe_input && ran_proc) {
            const process_return_value_t *cur_ret = process_get_last_return_value();
            if (cur_ret) {
                process_return_val_copy(&last_ret, cur_ret);
            } else {
                process_return_val_reset(&last_ret);
            }
        } else {
            process_return_val_reset(&last_ret);
        }

        if (system_is_interrupted()) {
            break;
        }
    }

    process_return_val_free_fields(&last_ret);
    plan_free(&plan);
    return last_exit;
}
