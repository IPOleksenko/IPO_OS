#include <kernel/process.h>
#include <syscall.h>
#include <stdlib.h>
#include <string.h>

void process_yield(void) {
    ipo_syscall(IPO_SYSCALL_PROCESS_YIELD, 0u, NULL);
}

void process_yield_kernel(void) {
    ipo_syscall(IPO_SYSCALL_PROCESS_YIELD, 0u, NULL);
}

bool process_is_foreground(void) {
    return ipo_syscall(IPO_SYSCALL_PROCESS_IS_FOREGROUND, 0u, NULL) != 0;
}

int process_get_exit_code(void) {
    return ipo_get_exit_code();
}

int process_exec(const char *path, int argc, char **argv) {
    return ipo_exec(path, argc, argv);
}

bool process_is_batch_active(void) {
    return ipo_syscall(IPO_SYSCALL_PROCESS_IS_BATCH_ACTIVE, 0u, NULL) != 0;
}

void process_return_val_init(process_return_value_t *val) {
    if (!val) return;
    memset(val, 0, sizeof(*val));
}

void process_return_val_free_fields(process_return_value_t *val) {
    if (!val) return;
    if (val->type_name) { free(val->type_name); val->type_name = NULL; }
    if (val->str_val) { free(val->str_val); val->str_val = NULL; }
    if (val->struct_data) { free(val->struct_data); val->struct_data = NULL; }
    if (val->arg_vals) {
        for (uint32_t i = 0; i < val->arg_count; i++) {
            if (val->arg_vals[i]) free(val->arg_vals[i]);
        }
        free(val->arg_vals);
        val->arg_vals = NULL;
    }
    val->type_name_len = 0;
    val->type_name_cap = 0;
    val->str_val_len = 0;
    val->str_val_cap = 0;
    val->struct_size = 0;
    val->struct_data_cap = 0;
    val->arg_count = 0;
    val->arg_cap = 0;
}

void process_return_val_reset(process_return_value_t *val) {
    if (!val) return;
    process_return_val_free_fields(val);
    process_return_val_init(val);
}

void process_return_val_set_type_name(process_return_value_t *val, const char *name) {
    if (!val) return;
    if (val->type_name) { free(val->type_name); val->type_name = NULL; }
    val->type_name_len = 0;
    val->type_name_cap = 0;
    if (!name) return;
    size_t len = strlen(name);
    val->type_name = malloc(len + 1);
    if (val->type_name) {
        memcpy(val->type_name, name, len + 1);
        val->type_name_len = (uint32_t)len;
        val->type_name_cap = (uint32_t)(len + 1);
    }
}

void process_return_val_set_str(process_return_value_t *val, const char *str) {
    if (!val) return;
    if (val->str_val) { free(val->str_val); val->str_val = NULL; }
    val->str_val_len = 0;
    val->str_val_cap = 0;
    if (!str) return;
    size_t len = strlen(str);
    val->str_val = malloc(len + 1);
    if (val->str_val) {
        memcpy(val->str_val, str, len + 1);
        val->str_val_len = (uint32_t)len;
        val->str_val_cap = (uint32_t)(len + 1);
    }
}

void process_return_val_append_str(process_return_value_t *val, const char *str) {
    if (!val || !str) return;
    size_t app_len = strlen(str);
    if (app_len == 0) return;
    uint32_t needed = val->str_val_len + (uint32_t)app_len + 1;
    if (needed > val->str_val_cap) {
        uint32_t new_cap = val->str_val_cap ? val->str_val_cap * 2 : 64;
        while (new_cap < needed) new_cap *= 2;
        char *new_buf = malloc(new_cap);
        if (!new_buf) return;
        if (val->str_val && val->str_val_len > 0) {
            memcpy(new_buf, val->str_val, val->str_val_len);
        }
        if (val->str_val) free(val->str_val);
        val->str_val = new_buf;
        val->str_val_cap = new_cap;
    }
    memcpy(val->str_val + val->str_val_len, str, app_len);
    val->str_val_len += (uint32_t)app_len;
    val->str_val[val->str_val_len] = '\0';
}

void process_return_val_set_struct_data(process_return_value_t *val, const void *data, uint32_t size) {
    if (!val) return;
    if (val->struct_data) { free(val->struct_data); val->struct_data = NULL; }
    val->struct_size = 0;
    val->struct_data_cap = 0;
    if (!data || size == 0) return;
    val->struct_data = malloc(size);
    if (val->struct_data) {
        memcpy(val->struct_data, data, size);
        val->struct_size = size;
        val->struct_data_cap = size;
    }
}

void process_return_val_add_arg(process_return_value_t *val, const char *arg) {
    if (!val || !arg) return;
    if (val->arg_count + 1 > val->arg_cap) {
        uint32_t new_cap = val->arg_cap ? val->arg_cap * 2 : 4;
        char **new_arr = malloc(new_cap * sizeof(char *));
        if (!new_arr) return;
        for (uint32_t i = 0; i < val->arg_count; i++) {
            new_arr[i] = val->arg_vals[i];
        }
        if (val->arg_vals) free(val->arg_vals);
        val->arg_vals = new_arr;
        val->arg_cap = new_cap;
    }
    size_t alen = strlen(arg);
    char *copy = malloc(alen + 1);
    if (copy) {
        memcpy(copy, arg, alen + 1);
        val->arg_vals[val->arg_count++] = copy;
    }
}

void process_return_val_copy(process_return_value_t *dst, const process_return_value_t *src) {
    if (!dst) return;
    process_return_val_free_fields(dst);
    if (!src) return;
    dst->type = src->type;
    dst->int_val = src->int_val;
    dst->raw_eax = src->raw_eax;
    dst->raw_edx = src->raw_edx;
    if (src->type_name) process_return_val_set_type_name(dst, src->type_name);
    if (src->str_val) process_return_val_set_str(dst, src->str_val);
    if (src->struct_data && src->struct_size > 0) {
        process_return_val_set_struct_data(dst, src->struct_data, src->struct_size);
    }
    if (src->arg_count > 0 && src->arg_vals) {
        for (uint32_t i = 0; i < src->arg_count; i++) {
            process_return_val_add_arg(dst, src->arg_vals[i]);
        }
    }
}


