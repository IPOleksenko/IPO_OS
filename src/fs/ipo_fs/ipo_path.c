#include <file_system/ipo_fs.h>
#include <string.h>
#include <stdio.h>
#include <memory/kmalloc.h>
#include <kernel/terminal.h>

char *fs_make_absolute_path(const char *path, const char *base) {
    if (!path) return NULL;

    const char *cwd = base;
    if (!cwd || cwd[0] == '\0') {
        cwd = terminal_get_cwd();
        if (!cwd || cwd[0] == '\0') cwd = "/";
    }

    if (path[0] == '~' && (path[1] == '\0' || path[1] == '/')) {
        path++;
        if (path[0] == '\0') path = "/";
    }

    size_t path_len = strlen(path);
    size_t cwd_len = (path[0] == '/') ? 0u : strlen(cwd);
    if (path_len > (size_t)-1 - cwd_len - 2u) return NULL;
    size_t combined_size = cwd_len + path_len + 2u;
    size_t result_size = combined_size < 512u ? 512u : combined_size;

    char *combined = (char *)kmalloc(combined_size);
    char *result = (char *)kmalloc(result_size);
    if (!combined || !result) {
        if (combined) kfree(combined);
        if (result) kfree(result);
        return NULL;
    }

    if (path[0] == '/') {
        memcpy(combined, path, path_len + 1u);
    } else if (strcmp(cwd, "/") == 0) {
        snprintf(combined, combined_size, "/%s", path);
    } else {
        snprintf(combined, combined_size, "%s/%s", cwd, path);
    }

    if (!fs_canonicalize(combined, result, result_size)) {
        kfree(combined);
        kfree(result);
        return NULL;
    }
    kfree(combined);
    return result;
}

int path_resolve(const char *path, uint64_t *out_inode) {
    if (!path || !out_inode) {
        printf("path_resolve: invalid args\n");
        return -1;
    }

    char *tmp = fs_make_absolute_path(path, NULL);
    if (!tmp) return -1;
    if (strcmp(tmp, "/") == 0) {
        kfree(tmp);
        *out_inode = 1;
        return 0;
    }

    uint64_t cur = 1;
    const char *p = tmp + 1; /* skip leading slash */

    while (*p) {
        const char *slash = strchr(p, '/');
        size_t len = slash ? (size_t)(slash - p) : strlen(p);

        char stack_tok[128];
        char *token = (len < sizeof(stack_tok)) ? stack_tok : (char *)kmalloc(len + 1);
        if (!token) {
            kfree(tmp);
            return -1;
        }
        memcpy(token, p, len);
        token[len] = '\0';

        struct ipo_dir_entry de;
        if (dir_find_entry(cur, token, &de, NULL) < 0) {
            if (token != stack_tok) kfree(token);
            kfree(tmp);
            return -1;
        }

        if (token != stack_tok) kfree(token);
        cur = dir_entry_get_inode(&de);
        if (!slash) break;
        p = slash + 1;
    }

    kfree(tmp);
    *out_inode = cur;
    return 0;
}

bool fs_canonicalize(const char *in, char *out, size_t out_size) {
    if (!in || !out || out_size == 0) return false;
    if (in[0] == '\0') {
        if (out_size > 1) {
            out[0] = '/';
            out[1] = '\0';
            return true;
        } else {
            out[0] = '\0';
            return false;
        }
    }

    const char *stack_ptrs[256];
    size_t stack_lens[256];
    const char **token_ptrs = stack_ptrs;
    size_t *token_lens = stack_lens;
    size_t token_cap = 256;
    size_t tcount = 0;

    const char *c = in;
    while (*c) {
        while (*c == '/') c++;
        if (!*c) break;
        const char *tstart = c;
        while (*c && *c != '/') c++;
        size_t tlen = (size_t)(c - tstart);
        if (tlen == 0) continue;

        if (tlen == 1 && tstart[0] == '.') {
            continue;
        }
        if (tlen == 2 && tstart[0] == '.' && tstart[1] == '.') {
            if (tcount > 0) tcount--;
            continue;
        }

        if (tcount >= token_cap) {
            size_t new_cap = token_cap * 2;
            const char **new_ptrs = (const char **)kmalloc(new_cap * sizeof(const char *));
            size_t *new_lens = (size_t *)kmalloc(new_cap * sizeof(size_t));
            if (!new_ptrs || !new_lens) {
                if (new_ptrs) kfree((void *)new_ptrs);
                if (new_lens) kfree(new_lens);
                if (token_ptrs != stack_ptrs) kfree((void *)token_ptrs);
                if (token_lens != stack_lens) kfree(token_lens);
                return false;
            }
            memcpy(new_ptrs, token_ptrs, tcount * sizeof(const char *));
            memcpy(new_lens, token_lens, tcount * sizeof(size_t));
            if (token_ptrs != stack_ptrs) kfree((void *)token_ptrs);
            if (token_lens != stack_lens) kfree(token_lens);
            token_ptrs = new_ptrs;
            token_lens = new_lens;
            token_cap = new_cap;
        }

        token_ptrs[tcount] = tstart;
        token_lens[tcount] = tlen;
        tcount++;
    }

    if (tcount == 0) {
        if (token_ptrs != stack_ptrs) kfree((void *)token_ptrs);
        if (token_lens != stack_lens) kfree(token_lens);
        if (out_size < 2) {
            if (out_size > 0) out[0] = '\0';
            return false;
        }
        out[0] = '/';
        out[1] = '\0';
        return true;
    }

    size_t pos = 0;
    for (size_t i = 0; i < tcount; i++) {
        if (pos + 1 + token_lens[i] >= out_size) {
            if (token_ptrs != stack_ptrs) kfree((void *)token_ptrs);
            if (token_lens != stack_lens) kfree(token_lens);
            if (out_size > 0) out[0] = '\0';
            return false;
        }
        out[pos++] = '/';
        memcpy(out + pos, token_ptrs[i], token_lens[i]);
        pos += token_lens[i];
    }
    out[pos] = '\0';

    if (token_ptrs != stack_ptrs) kfree((void *)token_ptrs);
    if (token_lens != stack_lens) kfree(token_lens);
    return true;
}

int path_resolve_parent(const char *path, uint64_t *out_parent_inode, char *out_name) {
    if (!path || !out_parent_inode || !out_name) {
        printf("path_resolve_parent: invalid args\n");
        return -1;
    }

    char *tmp = fs_make_absolute_path(path, NULL);
    if (!tmp) return -1;
    size_t buf_size = strlen(tmp) + 1u;
    if (strcmp(tmp, "/") == 0) {
        kfree(tmp);
        return -1;
    }

    char *r = strrchr(tmp, '/');
    if (!r) {
        kfree(tmp);
        return -1;
    }

    char *parent_path = (char *)kmalloc(buf_size);
    if (!parent_path) {
        kfree(tmp);
        return -1;
    }

    if (r == tmp) {
        strcpy(parent_path, "/");
    } else {
        size_t len = (size_t)(r - tmp);
        memcpy(parent_path, tmp, len);
        parent_path[len] = '\0';
    }

    const char *name = r + 1;
    strcpy(out_name, name);

    uint64_t pino;
    int res = path_resolve(parent_path, &pino);
    kfree(parent_path);
    kfree(tmp);

    if (res < 0) return -1;
    *out_parent_inode = pino;
    return 0;
}
