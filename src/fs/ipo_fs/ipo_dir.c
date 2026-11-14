#include <file_system/ipo_fs.h>
#include <string.h>
#include <memory/kmalloc.h>

bool is_valid_filename(const char *name) {
    if (!name || name[0] == '\0') return false;
    size_t len = 0;
    for (; name[len] != '\0'; len++) {
        if (name[len] == '/' || len >= 255) return false;
    }
    if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) return false;
    return true;
}

int dir_find_entry(uint64_t dir_inode_no, const char *name, struct ipo_dir_entry *out_entry, uint64_t *out_offset) {
    struct ipo_inode din;
    if (!read_inode(dir_inode_no, &din)) return -1;
    if ((din.mode & IPO_INODE_TYPE_DIR) == 0) return -1;

    size_t target_len = strlen(name);
    uint64_t offset = 0;
    char *name_buf = (char *)kmalloc(target_len + 1);
    if (!name_buf) return -1;

    while (offset < din.size) {
        struct ipo_dir_entry hdr;
        if (inode_read_bytes(&din, &hdr, sizeof(hdr), offset) != (int)sizeof(hdr)) break;
        if (hdr.rec_len < sizeof(hdr)) break;

        uint64_t entry_ino = dir_entry_get_inode(&hdr);
        if (entry_ino != 0 && hdr.name_len == target_len) {
            if (inode_read_bytes(&din, name_buf, (uint32_t)target_len, offset + sizeof(hdr)) == (int)target_len) {
                if (memcmp(name_buf, name, target_len) == 0) {
                    kfree(name_buf);
                    if (out_entry) *out_entry = hdr;
                    if (out_offset) *out_offset = offset;
                    return 0;
                }
            }
        }
        offset += hdr.rec_len;
    }
    kfree(name_buf);
    return -1;
}

bool dir_add_entry(uint64_t dir_inode_no, const char *name, uint64_t inode_no, uint8_t type) {
    struct ipo_inode din;
    if (!read_inode(dir_inode_no, &din)) return false;
    if ((din.mode & IPO_INODE_TYPE_DIR) == 0) return false;
    if (!is_valid_filename(name)) return false;
    if (dir_find_entry(dir_inode_no, name, NULL, NULL) == 0) return false;

    size_t name_len = strlen(name);
    if (name_len > 255) return false;
    uint32_t needed_rec_len = (uint32_t)((sizeof(struct ipo_dir_entry) + name_len + 1 + 3) & ~3);

    uint64_t offset = 0;
    uint64_t target_offset = din.size;
    uint32_t actual_rec_len = needed_rec_len;

    while (offset < din.size) {
        struct ipo_dir_entry hdr;
        if (inode_read_bytes(&din, &hdr, sizeof(hdr), offset) != (int)sizeof(hdr)) break;
        if (hdr.rec_len < sizeof(hdr)) break;
        if (dir_entry_get_inode(&hdr) == 0 && hdr.rec_len >= needed_rec_len) {
            target_offset = offset;
            actual_rec_len = hdr.rec_len;
            break;
        }
        offset += hdr.rec_len;
    }

    struct ipo_dir_entry hdr;
    memset(&hdr, 0, sizeof(hdr));
    dir_entry_set_inode(&hdr, inode_no);
    hdr.rec_len = actual_rec_len;
    hdr.name_len = (uint32_t)name_len;
    hdr.type = type;

    if (inode_write_bytes(dir_inode_no, &din, &hdr, sizeof(hdr), target_offset) != (int)sizeof(hdr)) {
        return false;
    }
    if (inode_write_bytes(dir_inode_no, &din, name, (uint32_t)(name_len + 1), target_offset + sizeof(hdr)) != (int)(name_len + 1)) {
        return false;
    }

    uint32_t used_bytes = (uint32_t)(sizeof(hdr) + name_len + 1);
    if (actual_rec_len > used_bytes) {
        uint32_t pad_len = actual_rec_len - used_bytes;
        const uint8_t *zeros = (const uint8_t *)"\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0";
        while (pad_len > 0) {
            uint32_t step = pad_len < 16u ? pad_len : 16u;
            inode_write_bytes(dir_inode_no, &din, zeros, step, target_offset + used_bytes);
            used_bytes += step;
            pad_len -= step;
        }
    }

    if (target_offset + actual_rec_len > din.size) {
        din.size = target_offset + actual_rec_len;
        if (!write_inode(dir_inode_no, &din)) return false;
    }
    return true;
}

bool dir_remove_entry(uint64_t dir_inode_no, const char *name) {
    uint64_t offset;
    struct ipo_dir_entry hdr;
    if (dir_find_entry(dir_inode_no, name, &hdr, &offset) != 0) return false;

    uint64_t target_ino = dir_entry_get_inode(&hdr);
    struct ipo_inode target_inode;
    if (read_inode(target_ino, &target_inode)) {
        if (target_inode.mode & IPO_INODE_FLAG_PROTECTED) return false;
    }
    if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) return false;

    dir_entry_set_inode(&hdr, 0);
    struct ipo_inode din;
    if (!read_inode(dir_inode_no, &din)) return false;
    if (inode_write_bytes(dir_inode_no, &din, &hdr, sizeof(hdr), offset) != (int)sizeof(hdr)) {
        return false;
    }
    return true;
}
