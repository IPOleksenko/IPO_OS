#include <file_system/ipo_fs.h>
#include <string.h>
#include <stdio.h>

#define DIR_ENTRY_SIZE sizeof(struct ipo_dir_entry)
#define DIR_ENTRIES_PER_BLOCK (IPO_FS_BLOCK_SIZE / DIR_ENTRY_SIZE)

int dir_find_entry(uint32_t dir_inode_no, const char *name, struct ipo_dir_entry *out_entry, uint32_t *out_block, uint32_t *out_block_off) {
    struct ipo_inode din;
    if (!read_inode(dir_inode_no, &din)) return -1;
    if ((din.mode & IPO_INODE_TYPE_DIR) == 0) return -1;
    uint32_t entries = (din.size) / DIR_ENTRY_SIZE;
    uint8_t buf[IPO_FS_BLOCK_SIZE];
    for (uint32_t e = 0; e < entries; e++) {
        uint32_t block_idx = e / DIR_ENTRIES_PER_BLOCK;
        uint32_t inblock = e % DIR_ENTRIES_PER_BLOCK;
        int phys = -1;
        if (block_idx < IPO_FS_DIRECT_BLOCKS)
            phys = din.direct[block_idx];
        else {
            int tmp = get_data_block_for_inode(&din, block_idx, false);
            if (tmp < 0) continue;
            phys = tmp;
        }
        if (!phys) continue;
        if (!block_read(phys, buf)) return -1;
        struct ipo_dir_entry *de = (struct ipo_dir_entry *)buf + inblock;
        if (de->inode != 0) {
            size_t dn = de->name_len ? de->name_len : strlen(de->name);
            size_t nn = strlen(name);
            if (dn == nn && dn > 0 && strncmp(de->name, name, dn) == 0) {
                if (out_entry) memcpy(out_entry, de, sizeof(*de));
                if (out_block) *out_block = phys;
                if (out_block_off) *out_block_off = inblock;
                return 0;
            }
        }
    }
    return -1;
}

bool is_valid_filename(const char *name) {
    if (!name || name[0] == '\0') return false;
    size_t len = strlen(name);
    if (len == 0 || len >= IPO_FS_MAX_NAME) return false;
    for (size_t i = 0; i < len; i++) {
        char c = name[i];
        if (c >= 'A' && c <= 'Z') continue;
        if (c >= 'a' && c <= 'z') continue;
        if (c >= '0' && c <= '9') continue;
        if (c == '_' || c == '-' || c == '.') continue;
        return false;
    }
    if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) return false;
    return true;
}

bool dir_add_entry(uint32_t dir_inode_no, const char *name, uint32_t inode_no, uint8_t type) {
    struct ipo_inode din;
    if (!read_inode(dir_inode_no, &din)) return false;
    if ((din.mode & IPO_INODE_TYPE_DIR) == 0) return false;
    if (!is_valid_filename(name)) return false;
    struct ipo_dir_entry tmpde;
    if (dir_find_entry(dir_inode_no, name, &tmpde, NULL, NULL) == 0) return false;

    uint32_t entries = (din.size) / DIR_ENTRY_SIZE;
    uint32_t block_idx = entries / DIR_ENTRIES_PER_BLOCK;
    uint32_t inblock = entries % DIR_ENTRIES_PER_BLOCK;
    int phys = get_data_block_for_inode(&din, block_idx, true);
    if (phys < 0) return false;
    uint8_t buf[IPO_FS_BLOCK_SIZE];
    if (!block_read(phys, buf)) return false;
    struct ipo_dir_entry *de = (struct ipo_dir_entry *)buf + inblock;
    de->inode = inode_no;
    de->type = type;
    size_t nl = strlen(name);
    if (nl > IPO_FS_MAX_NAME - 1) nl = IPO_FS_MAX_NAME - 1;
    de->name_len = (uint8_t)nl;
    memset(de->reserved,0,sizeof(de->reserved));
    memset(de->name,0,sizeof(de->name));
    memcpy(de->name, name, de->name_len);
    de->name[de->name_len] = '\0';
    if (!block_write(phys, buf)) return false;
    din.size += DIR_ENTRY_SIZE;
    write_inode(dir_inode_no, &din);
    return true;
}

bool dir_remove_entry(uint32_t dir_inode_no, const char *name) {
    struct ipo_inode din;
    if (!read_inode(dir_inode_no, &din)) return false;
    uint32_t entries = (din.size) / DIR_ENTRY_SIZE;
    uint8_t buf[IPO_FS_BLOCK_SIZE];
    for (uint32_t e = 0; e < entries; e++) {
        uint32_t block_idx = e / DIR_ENTRIES_PER_BLOCK;
        uint32_t inblock = e % DIR_ENTRIES_PER_BLOCK;
        int phys = get_data_block_for_inode(&din, block_idx, false);
        if (phys < 0) continue;
        if (!block_read(phys, buf)) return false;
        struct ipo_dir_entry *de = (struct ipo_dir_entry *)buf + inblock;
        if (de->inode != 0 && strncmp(de->name, name, IPO_FS_MAX_NAME) == 0) {
            struct ipo_inode target_inode;
            if (read_inode(de->inode, &target_inode)) {
                if (target_inode.mode & IPO_INODE_FLAG_PROTECTED) return false;
            }
            if (strcmp(de->name, ".") == 0 || strcmp(de->name, "..") == 0) return false;
            de->inode = 0;
            de->name[0] = '\0';
            de->name_len = 0;
            if (!block_write(phys, buf)) return false;
            return true;
        }
    }
    return false;
}

int ipo_fs_list_dir(const char *path, char *out, int out_size) {
    uint32_t ino;
    if (path_resolve(path, &ino) < 0) return -1;
    struct ipo_inode din;
    if (!read_inode(ino, &din)) return -1;
    if ((din.mode & IPO_INODE_TYPE_DIR) == 0) return -1;
    uint32_t entries = din.size / DIR_ENTRY_SIZE;
    uint8_t buf[IPO_FS_BLOCK_SIZE];
    int pos = 0;
    for (uint32_t e = 0; e < entries; e++) {
        uint32_t block_idx = e / DIR_ENTRIES_PER_BLOCK;
        uint32_t inblock = e % DIR_ENTRIES_PER_BLOCK;
        int phys = get_data_block_for_inode(&din, block_idx, false);
        if (phys < 0) continue;
        if (!block_read(phys, buf)) return -1;
        struct ipo_dir_entry *de = (struct ipo_dir_entry *)buf + inblock;
        if (de->inode == 0 || de->name_len == 0) continue;
        int l = de->name_len;
        if (pos + l + 3 >= out_size) break;
        memcpy(out + pos, de->name, l);
        pos += l;
        if (de->type == IPO_INODE_TYPE_DIR) out[pos++] = '/';
        out[pos++] = '\n';
    }
    if (pos < out_size) out[pos] = '\0'; else out[out_size-1] = '\0';
    return pos;
}
