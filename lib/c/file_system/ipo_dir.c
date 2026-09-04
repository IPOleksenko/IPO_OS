#include <file_system/ipo_fs.h>
#include <string.h>
#include <stdio.h>
#include <memory/kmalloc.h>

int inode_read_bytes(struct ipo_inode *inode, void *buffer, uint32_t size, uint64_t offset) {
    if (!inode || !buffer || size == 0) return 0;
    if (offset >= inode->size) return 0;
    if (offset + size > inode->size) size = (uint32_t)(inode->size - offset);

    uint64_t first_block = offset / IPO_FS_BLOCK_SIZE;
    uint64_t last_block = (offset + size - 1) / IPO_FS_BLOCK_SIZE;
    uint8_t tmp[IPO_FS_BLOCK_SIZE];
    uint32_t copied = 0;

    for (uint64_t b = first_block; b <= last_block; b++) {
        int64_t phys = get_data_block_for_inode(inode, b, false);
        if (phys < 0) break;
        if (!block_read((uint64_t)phys, tmp)) break;
        uint32_t block_offset = (b == first_block) ? (uint32_t)(offset % IPO_FS_BLOCK_SIZE) : 0u;
        uint32_t tocopy = IPO_FS_BLOCK_SIZE - block_offset;
        if (tocopy > size - copied) tocopy = size - copied;
        memcpy((uint8_t*)buffer + copied, tmp + block_offset, tocopy);
        copied += tocopy;
    }
    return (int)copied;
}

int inode_write_bytes(uint32_t inode_no, struct ipo_inode *inode, const void *buffer, uint32_t size, uint64_t offset) {
    if (!inode || !buffer || size == 0) return 0;

    uint64_t first_block = offset / IPO_FS_BLOCK_SIZE;
    uint64_t last_block = (offset + size - 1) / IPO_FS_BLOCK_SIZE;
    uint8_t tmp[IPO_FS_BLOCK_SIZE];
    uint32_t written = 0;

    for (uint64_t b = first_block; b <= last_block; b++) {
        int64_t phys = get_data_block_for_inode(inode, b, true);
        if (phys < 0) break;
        uint32_t block_offset = (b == first_block) ? (uint32_t)(offset % IPO_FS_BLOCK_SIZE) : 0u;
        uint32_t towrite = IPO_FS_BLOCK_SIZE - block_offset;
        if (towrite > size - written) towrite = size - written;

        if (block_offset != 0 || towrite < IPO_FS_BLOCK_SIZE) {
            if (!block_read((uint64_t)phys, tmp)) memset(tmp, 0, sizeof(tmp));
        }
        memcpy(tmp + block_offset, (const uint8_t*)buffer + written, towrite);
        if (!block_write((uint64_t)phys, tmp)) break;
        written += towrite;
    }

    if (offset + written > inode->size) {
        inode->size = offset + written;
        if (inode_no > 0) {
            write_inode(inode_no, inode);
        }
    }
    return (int)written;
}

bool is_valid_filename(const char *name) {
    if (!name || name[0] == '\0') return false;
    for (size_t i = 0; name[i] != '\0'; i++) {
        if (name[i] == '/') return false;
    }
    if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) return false;
    return true;
}

int dir_find_entry(uint32_t dir_inode_no, const char *name, struct ipo_dir_entry *out_entry, uint64_t *out_offset) {
    struct ipo_inode din;
    if (!read_inode(dir_inode_no, &din)) return -1;
    if ((din.mode & IPO_INODE_TYPE_DIR) == 0) return -1;

    size_t target_len = strlen(name);
    uint64_t offset = 0;

    while (offset < din.size) {
        struct ipo_dir_entry hdr;
        if (inode_read_bytes(&din, &hdr, sizeof(hdr), offset) != (int)sizeof(hdr)) break;
        if (hdr.rec_len < sizeof(hdr)) break;

        if (hdr.inode != 0 && hdr.name_len == target_len) {
            char stack_buf[128];
            char *name_buf = (target_len < sizeof(stack_buf)) ? stack_buf : (char *)kmalloc(target_len + 1);
            if (name_buf) {
                if (inode_read_bytes(&din, name_buf, (uint32_t)target_len, offset + sizeof(hdr)) == (int)target_len) {
                    if (memcmp(name_buf, name, target_len) == 0) {
                        if (name_buf != stack_buf) kfree(name_buf);
                        if (out_entry) *out_entry = hdr;
                        if (out_offset) *out_offset = offset;
                        return 0;
                    }
                }
                if (name_buf != stack_buf) kfree(name_buf);
            }
        }
        offset += hdr.rec_len;
    }
    return -1;
}

bool dir_add_entry(uint32_t dir_inode_no, const char *name, uint32_t inode_no, uint8_t type) {
    struct ipo_inode din;
    if (!read_inode(dir_inode_no, &din)) return false;
    if ((din.mode & IPO_INODE_TYPE_DIR) == 0) return false;
    if (!is_valid_filename(name)) return false;
    if (dir_find_entry(dir_inode_no, name, NULL, NULL) == 0) return false;

    size_t name_len = strlen(name);
    uint32_t needed_rec_len = (uint32_t)((sizeof(struct ipo_dir_entry) + name_len + 1 + 3) & ~3);

    uint64_t offset = 0;
    uint64_t target_offset = din.size;
    uint32_t actual_rec_len = needed_rec_len;

    while (offset < din.size) {
        struct ipo_dir_entry hdr;
        if (inode_read_bytes(&din, &hdr, sizeof(hdr), offset) != (int)sizeof(hdr)) break;
        if (hdr.rec_len < sizeof(hdr)) break;
        if (hdr.inode == 0 && hdr.rec_len >= needed_rec_len) {
            target_offset = offset;
            actual_rec_len = hdr.rec_len;
            break;
        }
        offset += hdr.rec_len;
    }

    struct ipo_dir_entry hdr;
    hdr.inode = inode_no;
    hdr.rec_len = actual_rec_len;
    hdr.name_len = (uint32_t)name_len;
    hdr.type = type;
    memset(hdr.reserved, 0, sizeof(hdr.reserved));

    if (inode_write_bytes(dir_inode_no, &din, &hdr, sizeof(hdr), target_offset) != (int)sizeof(hdr)) {
        return false;
    }
    if (inode_write_bytes(dir_inode_no, &din, name, (uint32_t)(name_len + 1), target_offset + sizeof(hdr)) != (int)(name_len + 1)) {
        return false;
    }

    uint32_t used_bytes = (uint32_t)(sizeof(hdr) + name_len + 1);
    if (actual_rec_len > used_bytes) {
        uint32_t pad_len = actual_rec_len - used_bytes;
        uint8_t zeros[16] = {0};
        while (pad_len > 0) {
            uint32_t step = pad_len < sizeof(zeros) ? pad_len : (uint32_t)sizeof(zeros);
            inode_write_bytes(dir_inode_no, &din, zeros, step, target_offset + used_bytes);
            used_bytes += step;
            pad_len -= step;
        }
    }

    if (target_offset + actual_rec_len > din.size) {
        din.size = target_offset + actual_rec_len;
        write_inode(dir_inode_no, &din);
    }
    return true;
}

bool dir_remove_entry(uint32_t dir_inode_no, const char *name) {
    uint64_t offset;
    struct ipo_dir_entry hdr;
    if (dir_find_entry(dir_inode_no, name, &hdr, &offset) != 0) return false;

    struct ipo_inode target_inode;
    if (read_inode(hdr.inode, &target_inode)) {
        if (target_inode.mode & IPO_INODE_FLAG_PROTECTED) return false;
    }
    if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) return false;

    hdr.inode = 0;
    struct ipo_inode din;
    if (!read_inode(dir_inode_no, &din)) return false;
    if (inode_write_bytes(dir_inode_no, &din, &hdr, sizeof(hdr), offset) != (int)sizeof(hdr)) {
        return false;
    }
    return true;
}

int ipo_fs_list_dir(const char *path, char *out, int out_size) {
    uint32_t ino;
    if (path_resolve(path, &ino) < 0) return -1;
    struct ipo_inode din;
    if (!read_inode(ino, &din)) return -1;
    if ((din.mode & IPO_INODE_TYPE_DIR) == 0) return -1;

    uint64_t offset = 0;
    int pos = 0;
    while (offset < din.size) {
        struct ipo_dir_entry hdr;
        if (inode_read_bytes(&din, &hdr, sizeof(hdr), offset) != (int)sizeof(hdr)) break;
        if (hdr.rec_len < sizeof(hdr)) break;

        if (hdr.inode != 0 && hdr.name_len > 0) {
            if (pos + (int)hdr.name_len + 3 >= out_size) break;
            inode_read_bytes(&din, out + pos, hdr.name_len, offset + sizeof(hdr));
            pos += hdr.name_len;
            if (hdr.type == IPO_INODE_TYPE_DIR) out[pos++] = '/';
            out[pos++] = '\n';
        }
        offset += hdr.rec_len;
    }
    if (pos < out_size) out[pos] = '\0'; else out[out_size - 1] = '\0';
    return pos;
}
