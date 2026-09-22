#include <file_system/ipo_fs.h>
#include <string.h>
#include <stdio.h>
#include <memory/kmalloc.h>

int path_resolve(const char *path, uint32_t *out_inode) {
    if (!path || !out_inode) {
        printf("path_resolve: invalid args\n");
        return -1;
    }

    size_t plen = strlen(path);
    size_t buf_size = plen + 32;
    if (buf_size < 512) buf_size = 512;

    char *tmp = (char *)kmalloc(buf_size);
    if (!tmp) return -1;

    fs_canonicalize(path, tmp, buf_size);
    if (strcmp(tmp, "/") == 0) {
        kfree(tmp);
        *out_inode = 1;
        return 0;
    }

    uint32_t cur = 1;
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
        cur = de.inode;
        if (!slash) break;
        p = slash + 1;
    }

    kfree(tmp);
    *out_inode = cur;
    return 0;
}

void fs_canonicalize(const char *in, char *out, size_t out_size) {
    if (!in || !out || out_size == 0) return;
    if (in[0] == '\0') {
        if (out_size > 1) { out[0] = '/'; out[1] = '\0'; } else out[0] = '\0';
        return;
    }

    const char *token_ptrs[256];
    size_t token_lens[256];
    int tcount = 0;

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

        if (tcount < 256) {
            token_ptrs[tcount] = tstart;
            token_lens[tcount] = tlen;
            tcount++;
        }
    }

    if (tcount == 0) {
        if (out_size > 1) { out[0] = '/'; out[1] = '\0'; } else out[0] = '\0';
        return;
    }

    size_t pos = 0;
    for (int i = 0; i < tcount; i++) {
        if (pos + 1 + token_lens[i] >= out_size) break;
        out[pos++] = '/';
        memcpy(out + pos, token_ptrs[i], token_lens[i]);
        pos += token_lens[i];
    }
    out[pos] = '\0';
}

int path_resolve_parent(const char *path, uint32_t *out_parent_inode, char *out_name) {
    if (!path || !out_parent_inode || !out_name) {
        printf("path_resolve_parent: invalid args\n");
        return -1;
    }

    size_t plen = strlen(path);
    size_t buf_size = plen + 32;
    if (buf_size < 512) buf_size = 512;

    char *tmp = (char *)kmalloc(buf_size);
    if (!tmp) return -1;

    fs_canonicalize(path, tmp, buf_size);
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

    uint32_t pino;
    int res = path_resolve(parent_path, &pino);
    kfree(parent_path);
    kfree(tmp);

    if (res < 0) return -1;
    *out_parent_inode = pino;
    return 0;
}
