#include <file_system/ipo_fs.h>
#include <string.h>
#include <stdio.h>

int path_resolve(const char *path, uint32_t *out_inode) {
    if (!path || !out_inode) { printf("path_resolve: invalid args\n"); return -1; }
    char tmp[512]; fs_canonicalize(path, tmp, sizeof(tmp));
    if (strcmp(tmp, "/") == 0) { *out_inode = 1; return 0; }
    uint32_t cur = 1;
    const char *p = tmp + 1; /* skip leading slash */
    char token[IPO_FS_MAX_NAME];
    while (*p) {
        const char *slash = strchr(p, '/');
        size_t len = slash ? (size_t)(slash - p) : strlen(p);
        if (len >= sizeof(token)) { printf("path_resolve: token too long in '%s' (tmp='%s')\n", path, tmp); return -1; }
        strncpy(token, p, len); token[len] = '\0';
        struct ipo_dir_entry de;
        if (dir_find_entry(cur, token, &de, NULL, NULL) < 0) { printf("path_resolve: dir_find_entry failed for '%s' under inode %u (tmp='%s')\n", token, cur, tmp); return -1; }
        cur = de.inode;
        if (!slash) break;
        p = slash + 1;
    }
    *out_inode = cur;
    return 0;
}

void fs_canonicalize(const char *in, char *out, size_t out_size) {
    if (!in || !out || out_size == 0) return;
    const char *p = in;
    if (!p || *p == '\0') { if (out_size > 1) { out[0] = '/'; out[1] = '\0'; } else out[0] = '\0'; return; }
    /* use a proper stack of tokens so popping actually removes last token */
    char tokens[128][IPO_FS_MAX_NAME];
    int tcount = 0;
    const char *c = p;
    char token[IPO_FS_MAX_NAME];
    while (*c) {
        while (*c == '/') c++; if (!*c) break;
        size_t tlen = 0;
        while (*c && *c != '/') {
            if (tlen + 1 < IPO_FS_MAX_NAME) token[tlen++] = *c;
            c++;
        }
        token[tlen] = '\0';
        if (tlen == 0) continue;
        if (strcmp(token, ".") == 0) continue;
        if (strcmp(token, "..") == 0) {
            if (tcount > 0) tcount--; /* pop last token */
            continue;
        }
        if (tcount >= 128) break;
        strncpy(tokens[tcount], token, IPO_FS_MAX_NAME);
        tokens[tcount][IPO_FS_MAX_NAME-1] = '\0';
        tcount++;
    }
    if (tcount == 0) {
        if (out_size > 1) { out[0] = '/'; out[1] = '\0'; } else out[0] = '\0';
        return;
    }
    size_t pos = 0;
    for (int i = 0; i < tcount; i++) {
        size_t l = strlen(tokens[i]);
        if (pos + 1 + l + 1 >= out_size) break;
        out[pos++] = '/';
        memcpy(out + pos, tokens[i], l); pos += l;
    }
    out[pos] = '\0';
}

int path_resolve_parent(const char *path, uint32_t *out_parent_inode, char *out_name) {
    if (!path || !out_parent_inode || !out_name) { printf("path_resolve_parent: invalid args\n"); return -1; }
    char tmp[512]; fs_canonicalize(path, tmp, sizeof(tmp));
    if (strcmp(tmp, "/") == 0) { printf("path_resolve_parent: path is root (no parent): '%s' -> '%s'\n", path, tmp); return -1; }
    char *r = NULL;
    size_t tlen = strlen(tmp);
    if (tlen == 0) { printf("path_resolve_parent: canonicalized to empty for '%s'\n", path); return -1; }
    for (int i = (int)tlen - 1; i >= 0; i--) {
        if (tmp[i] == '/') { r = tmp + i; break; }
    }
    if (!r) { printf("path_resolve_parent: no slash found in '%s' (tmp='%s')\n", path, tmp); return -1; }
    char parent_path[512];
    if (r == tmp) {
        strcpy(parent_path, "/");
    } else {
        size_t len = (size_t)(r - tmp);
        if (len >= sizeof(parent_path)) { printf("path_resolve_parent: parent path too long for '%s'\n", path); return -1; }
        strncpy(parent_path, tmp, len); parent_path[len] = '\0';
    }
    const char *name = r + 1;
    if (strlen(name) >= IPO_FS_MAX_NAME) { printf("path_resolve_parent: name too long in '%s' -> '%s'\n", path, tmp); return -1; }
    strcpy(out_name, name);
    uint32_t pino;
    if (path_resolve(parent_path, &pino) < 0) { printf("path_resolve_parent: path_resolve failed for parent '%s' (from '%s')\n", parent_path, path); return -1; }
    *out_parent_inode = pino;
    return 0;
}
