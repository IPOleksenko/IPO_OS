#include <file_system/ipo_fs.h>
#include <storage/storage_pool.h>
#include <driver/storage/block_dev.h>
#include <driver/ata/ata.h>
#include <string.h>
#include <stdio.h>
#include <memory/kmalloc.h>

extern struct ipo_inode_chunk_desc cached_chunk;
extern bool cached_chunk_valid;

static bool write_dir_dots(uint64_t inode_no, uint64_t parent, uint64_t block) {
    uint8_t *buf = (uint8_t *)kmalloc(IPO_FS_BLOCK_SIZE);
    if (!buf) return false;
    memset(buf, 0, IPO_FS_BLOCK_SIZE);

    /* "." entry: rec_len = 20, name_len = 1 */
    struct ipo_dir_entry d1;
    memset(&d1, 0, sizeof(d1));
    dir_entry_set_inode(&d1, inode_no);
    d1.rec_len = 20;
    d1.name_len = 1;
    d1.type = IPO_INODE_TYPE_DIR;
    memcpy(buf, &d1, sizeof(d1));
    memcpy(buf + sizeof(d1), ".\0\0\0", 4);

    /* ".." entry: rec_len = 20, name_len = 2 */
    struct ipo_dir_entry d2;
    memset(&d2, 0, sizeof(d2));
    dir_entry_set_inode(&d2, parent);
    d2.rec_len = 20;
    d2.name_len = 2;
    d2.type = IPO_INODE_TYPE_DIR;
    memcpy(buf + 20, &d2, sizeof(d2));
    memcpy(buf + 20 + sizeof(d2), "..\0\0", 4);

    bool ok = block_write(block, buf);
    kfree(buf);
    return ok;
}

static bool setup_dir_inode(uint64_t ino, uint64_t parent, uint32_t mode) {
    int64_t blk = allocate_block();
    if (blk < 0) return false;
    if (!write_dir_dots(ino, parent, (uint64_t)blk)) {
        free_block((uint64_t)blk);
        return false;
    }

    struct ipo_inode dir;
    memset(&dir, 0, sizeof(dir));
    dir.mode = mode;
    dir.links_count = 2;
    dir.size = 40;
    dir.extents[0].logical_block = 0;
    dir.extents[0].physical_block = (uint64_t)blk;
    dir.extents[0].block_count = 1;

    if (!write_inode(ino, &dir)) {
        free_block((uint64_t)blk);
        return false;
    }
    return true;
}

static bool zero_blocks(uint64_t start_block, uint64_t num_blocks) {
    #define ZERO_CHUNK_BLOCKS 8
    uint8_t *zero_chunk = (uint8_t *)kmalloc(ZERO_CHUNK_BLOCKS * IPO_FS_BLOCK_SIZE);
    if (!zero_chunk) return false;
    memset(zero_chunk, 0, ZERO_CHUNK_BLOCKS * IPO_FS_BLOCK_SIZE);
    uint64_t remaining = num_blocks;
    uint64_t curr = start_block;
    while (remaining > 0) {
        uint16_t chunk = (remaining > ZERO_CHUNK_BLOCKS) ? ZERO_CHUNK_BLOCKS : (uint16_t)remaining;
        if (!block_write_multi(curr, chunk, zero_chunk)) {
            kfree(zero_chunk);
            return false;
        }
        curr += chunk;
        remaining -= chunk;
    }
    kfree(zero_chunk);
    return true;
}

bool ipo_fs_format(uint64_t disk_start_lba, uint64_t total_blocks, uint64_t total_inodes) {
    if (total_blocks < 10) { printf("ipo_fs_format: too few blocks\n"); return false; }
    if (total_inodes == 0) total_inodes = 1;

    uint64_t inode_table_blocks = (total_inodes * sizeof(struct ipo_inode) + IPO_FS_BLOCK_SIZE - 1) / IPO_FS_BLOCK_SIZE;
    uint64_t inode_bitmap_blocks = (total_inodes + IPO_FS_BLOCK_SIZE * 8 - 1) / (IPO_FS_BLOCK_SIZE * 8);
    uint64_t block_bitmap_blocks = 1;
    uint64_t data_blocks = 0;
    for (int iter = 0; iter < 8; iter++) {
        uint64_t meta_blocks = 1 + inode_bitmap_blocks + block_bitmap_blocks + inode_table_blocks;
        if (total_blocks <= meta_blocks) {
            printf("ipo_fs_format: not enough data blocks computed\n");
            return false;
        }
        data_blocks = total_blocks - meta_blocks;
        uint64_t nb = (data_blocks + IPO_FS_BLOCK_SIZE * 8 - 1) / (IPO_FS_BLOCK_SIZE * 8);
        if (nb == 0) nb = 1;
        if (nb == block_bitmap_blocks) break;
        block_bitmap_blocks = nb;
    }
    uint64_t meta_blocks = 1 + inode_bitmap_blocks + block_bitmap_blocks + inode_table_blocks;
    if (total_blocks <= meta_blocks) {
        printf("ipo_fs_format: not enough data blocks computed\n");
        return false;
    }

    struct ipo_superblock s;
    memset(&s, 0, sizeof(s));
    strncpy(s.magic, IPO_FS_MAGIC_STR, sizeof(s.magic) - 1);

    s.fs_size_blocks = total_blocks;
    s.block_size = IPO_FS_BLOCK_SIZE;
    s.flags = IPO_FS_FLAG_LINKED_EXTENTS | IPO_FS_FLAG_DYNAMIC_INODES;
    s.primary_inodes = (uint32_t)total_inodes;
    s.inode_count = total_inodes;
    s.dynamic_chunk_tree = 0;
    s.dynamic_chunks_count = 0;
    s.dynamic_inodes_count = 0;
    s.dynamic_inodes_used = 0;

    s.inode_bitmap_start = 1;
    s.block_bitmap_start = s.inode_bitmap_start + inode_bitmap_blocks;
    s.inode_table_start = s.block_bitmap_start + block_bitmap_blocks;
    s.data_blocks_start = s.inode_table_start + inode_table_blocks;

    struct ipo_superblock prev_sb = sb;
    uint64_t prev_lba = fs_start_lba;
    bool prev_mounted = fs_mounted;
    bool prev_cached = cached_chunk_valid;

    fs_mounted = false;
    cached_chunk_valid = false;
    memset(&cached_chunk, 0, sizeof(cached_chunk));
    fs_start_lba = disk_start_lba;
    memcpy(&sb, &s, sizeof(sb));

    bool ok = block_write(0, &sb) &&
              zero_blocks(s.inode_bitmap_start, inode_bitmap_blocks) &&
              zero_blocks(s.block_bitmap_start, block_bitmap_blocks) &&
              zero_blocks(s.inode_table_start, inode_table_blocks);
    if (!ok) {
        printf("ipo_fs_format: failed to initialize metadata blocks\n");
        goto rollback;
    }

    /* Reserve inode 1 for root directory */
    if (!bitmap_set(sb.inode_bitmap_start, 0, true) ||
        !setup_dir_inode(1, 1, IPO_INODE_TYPE_DIR)) {
        printf("ipo_fs_format: setup_dir_inode failed for root\n");
        goto rollback;
    }

    int64_t app_ino = allocate_inode();
    if (app_ino < 0 ||
        !setup_dir_inode((uint64_t)app_ino, 1, IPO_INODE_TYPE_DIR | IPO_INODE_FLAG_PROTECTED) ||
        !dir_add_entry(1, "applications", (uint64_t)app_ino, IPO_INODE_TYPE_DIR)) {
        printf("ipo_fs_format: failed to create /applications\n");
        goto rollback;
    }

    int64_t autorun_ino = allocate_inode();
    if (autorun_ino < 0) {
        printf("ipo_fs_format: allocate_inode failed for /autorun\n");
        goto rollback;
    }
    struct ipo_inode ar_inode;
    memset(&ar_inode, 0, sizeof(ar_inode));
    ar_inode.mode = IPO_INODE_TYPE_FILE | IPO_INODE_FLAG_PROTECTED;
    ar_inode.size = 0;
    ar_inode.links_count = 1;
    if (!write_inode((uint64_t)autorun_ino, &ar_inode) ||
        !dir_add_entry(1, "autorun", (uint64_t)autorun_ino, IPO_INODE_TYPE_FILE)) {
        printf("ipo_fs_format: failed to create /autorun\n");
        goto rollback;
    }

    if (!block_write(0, &sb)) {
        printf("ipo_fs_format: failed to write superblock\n");
        goto rollback;
    }
    return true;

rollback:
    sb = prev_sb;
    fs_start_lba = prev_lba;
    fs_mounted = prev_mounted;
    cached_chunk_valid = prev_cached;
    return false;
}

bool ipo_fs_mount(uint64_t disk_start_lba) {
    fs_start_lba = disk_start_lba;
    uint8_t *buf = (uint8_t *)kmalloc(IPO_FS_BLOCK_SIZE);
    if (!buf) {
        ipo_fs_set_last_error(IPO_FS_ERR_IO);
        return false;
    }
    if (!storage_pool_read(disk_start_lba + 0, 1, buf)) {
        kfree(buf);
        serial_printf("[mount] storage_pool_read(0) failed at lba %llu\n", disk_start_lba);
        ipo_fs_set_last_error(IPO_FS_ERR_IO);
        return false;
    }
    memcpy(&sb, buf, sizeof(sb));
    kfree(buf);
    if (strncmp(sb.magic, IPO_FS_MAGIC_STR, sizeof(IPO_FS_MAGIC_STR)-1) != 0) {
        serial_printf("[mount] magic mismatch at lba %llu (read '%.8s')\n", disk_start_lba, sb.magic);
        ipo_fs_set_last_error(IPO_FS_ERR_CORRUPT);
        return false;
    }
    if (sb.block_size != IPO_FS_BLOCK_SIZE) {
        serial_printf("[mount] block size mismatch: %u\n", sb.block_size);
        ipo_fs_set_last_error(IPO_FS_ERR_CORRUPT);
        return false;
    }

    fs_mounted = true;
    cached_chunk_valid = false;
    ipo_fs_set_last_error(IPO_FS_OK);

    /* Ensure primary inode count and dynamic flag are initialized */
    if (sb.primary_inodes == 0 && sb.inode_count > 0) {
        sb.primary_inodes = (uint32_t)(sb.inode_count - sb.dynamic_inodes_count);
        sb.flags |= IPO_FS_FLAG_DYNAMIC_INODES;
        block_write(0, &sb);
    }

    serial_printf("[mount] success at lba %llu: inodes=%llu (primary=%u dyn_chunks=%llu) table=%llu\n",
                  disk_start_lba, sb.inode_count, sb.primary_inodes,
                  sb.dynamic_chunks_count, sb.inode_table_start);
    return true;
}

int64_t ipo_fs_create(const char *path, uint8_t type) {
    if (!fs_mounted || !path) {
        ipo_fs_set_last_error(IPO_FS_ERR_NOT_MOUNTED);
        return -1;
    }
    char name[256];
    uint64_t parent = 0;
    if (path_resolve_parent(path, &parent, name) < 0 || !is_valid_filename(name)) {
        ipo_fs_set_last_error(IPO_FS_ERR_INVALID_ARG);
        return -1;
    }
    struct ipo_dir_entry de;
    if (dir_find_entry(parent, name, &de, NULL) == 0) {
        ipo_fs_set_last_error(IPO_FS_ERR_INVALID_ARG);
        return -1;
    }
    int64_t ino = allocate_inode();
    if (ino < 0) {
        return -1;
    }

    if (type == IPO_INODE_TYPE_DIR) {
        if (!setup_dir_inode((uint64_t)ino, parent, type)) {
            free_inode((uint64_t)ino);
            ipo_fs_set_last_error(IPO_FS_ERR_NO_SPACE);
            return -1;
        }
    } else {
        struct ipo_inode inode;
        memset(&inode, 0, sizeof(inode));
        inode.mode = type;
        inode.size = 0;
        inode.links_count = 1;
        if (!write_inode((uint64_t)ino, &inode)) {
            free_inode((uint64_t)ino);
            ipo_fs_set_last_error(IPO_FS_ERR_IO);
            return -1;
        }
    }

    if (!dir_add_entry(parent, name, (uint64_t)ino, type)) {
        free_inode((uint64_t)ino);
        return -1;
    }
    return ino;
}

int ipo_fs_open(const char *path) {
    if (!fs_mounted) return -1;
    uint64_t ino;
    if (path_resolve(path, &ino) < 0) return -1;
    struct ipo_inode inode;
    if (!read_inode(ino, &inode)) return -1;
    if ((inode.mode & IPO_INODE_TYPE_DIR) != 0) return -1;

    if (!fds || fds_capacity == 0) {
        struct ipo_fd *init_fds = (struct ipo_fd *)kmalloc(32u * sizeof(struct ipo_fd));
        if (!init_fds) return -1;
        memset(init_fds, 0, 32u * sizeof(struct ipo_fd));
        init_fds[0].used = init_fds[1].used = init_fds[2].used = 1;
        fds = init_fds;
        fds_capacity = 32;
    }

    for (uint32_t i = 3; i < fds_capacity; i++) {
        if (!fds[i].used) {
            fds[i].used = 1;
            fds[i].inode = ino;
            fds[i].offset = 0;
            fds[i].flags = 0;
            return (int)i;
        }
    }

    uint32_t old_cap = fds_capacity;
    if (old_cap > 0x7FFFFFFFu / (uint32_t)sizeof(struct ipo_fd)) return -1;
    uint32_t new_cap = old_cap * 2u;
    struct ipo_fd *new_fds = (struct ipo_fd *)kmalloc((size_t)new_cap * sizeof(struct ipo_fd));
    if (!new_fds) return -1;

    memcpy(new_fds, fds, (size_t)old_cap * sizeof(struct ipo_fd));
    kfree(fds);
    memset(new_fds + old_cap, 0, (size_t)(new_cap - old_cap) * sizeof(struct ipo_fd));
    fds = new_fds;
    fds_capacity = new_cap;

    fds[old_cap].used = 1;
    fds[old_cap].inode = ino;
    fds[old_cap].offset = 0;
    fds[old_cap].flags = 0;
    return (int)old_cap;
}

int ipo_fs_close(int fd) {
    if (fd < 3 || !fds || (uint32_t)fd >= fds_capacity) return -1;
    if (!fds[fd].used) return -1;

    fds[fd].used = 0;
    fds[fd].inode = 0;
    fds[fd].offset = 0;
    fds[fd].flags = 0;
    return 0;
}

int ipo_fs_read(int fd, void *buffer, uint32_t size, uint32_t offset) {
    if (fd < 3 || !fds || (uint32_t)fd >= fds_capacity) return -1;
    if (!fds[fd].used || fds[fd].inode == 0) return -1;
    struct ipo_inode inode;
    if (!read_inode(fds[fd].inode, &inode)) return -1;
    return inode_read_bytes(&inode, buffer, size, offset);
}

int ipo_fs_write(int fd, const void *buffer, uint32_t size, uint32_t offset) {
    if (fd < 3 || !fds || (uint32_t)fd >= fds_capacity) return -1;
    if (!fds[fd].used || fds[fd].inode == 0) return -1;
    struct ipo_inode inode;
    if (!read_inode(fds[fd].inode, &inode)) return -1;
    int res = inode_write_bytes(fds[fd].inode, &inode, buffer, size, offset);
    if (size > 0 && res < (int)size) {
        ipo_fs_set_last_error(IPO_FS_ERR_NO_SPACE);
    }
    return res;
}

bool ipo_fs_delete(const char *path) {
    if (!fs_mounted || !path) return false;
    char name[256];
    uint64_t parent = 0;
    if (path_resolve_parent(path, &parent, name) < 0) {
        return false;
    }
    struct ipo_dir_entry de;
    if (dir_find_entry(parent, name, &de, NULL) < 0) {
        return false;
    }
    uint64_t target_ino = dir_entry_get_inode(&de);
    struct ipo_inode target_inode;
    if (!read_inode(target_ino, &target_inode)) {
        return false;
    }
    if (target_inode.mode & IPO_INODE_FLAG_PROTECTED) {
        return false;
    }
    if (de.type == IPO_INODE_TYPE_DIR) {
        uint64_t offset = 0;
        while (offset < target_inode.size) {
            struct ipo_dir_entry hdr;
            if (inode_read_bytes(&target_inode, &hdr, sizeof(hdr), offset) != (int)sizeof(hdr)) break;
            if (hdr.rec_len < sizeof(hdr)) break;
            if (dir_entry_get_inode(&hdr) != 0 && hdr.name_len > 0) {
                if (hdr.name_len > 2) return false;
                uint16_t name_bytes = 0;
                bool read_ok = (inode_read_bytes(&target_inode, &name_bytes, hdr.name_len, offset + sizeof(hdr)) == (int)hdr.name_len);
                const char *nb = (const char *)&name_bytes;
                bool is_dot = read_ok && (hdr.name_len == 1 && nb[0] == '.');
                bool is_dotdot = read_ok && (hdr.name_len == 2 && nb[0] == '.' && nb[1] == '.');
                if (!is_dot && !is_dotdot) return false;
            }
            offset += hdr.rec_len;
        }
    }
    if (!dir_remove_entry(parent, name)) {
        return false;
    }
    free_inode(target_ino);
    return true;
}

bool ipo_fs_stat(const char *path, struct ipo_inode *out) {
    if (!fs_mounted || !path || !out) return false;
    uint64_t ino;
    if (path_resolve(path, &ino) < 0) return false;
    return read_inode(ino, out);
}

bool ipo_fs_write_text(const char *path, const char *text, bool append) {
    if (!fs_mounted || !path || !text) { printf("ipo_fs_write_text: invalid args or FS not mounted\n"); return false; }
    uint64_t ino;
    if (path_resolve(path, &ino) < 0) {
        if (ipo_fs_create(path, IPO_INODE_TYPE_FILE) < 0) { printf("ipo_fs_write_text: failed to create %s\n", path); return false; }
    }
    struct ipo_inode inode;
    if (!ipo_fs_stat(path, &inode)) { printf("ipo_fs_write_text: stat failed for %s\n", path); return false; }
    if ((inode.mode & IPO_INODE_TYPE_DIR) != 0) { printf("ipo_fs_write_text: target is a directory %s\n", path); return false; }

    size_t len_sz = strlen(text);
    if (len_sz > 0x7FFFFFFFu) return false;
    uint64_t offset = append ? inode.size : 0;

    int fd = ipo_fs_open(path);
    if (fd < 0) { printf("ipo_fs_write_text: failed to open %s\n", path); return false; }

    int written = 0;
    if (read_inode(fds[fd].inode, &inode)) {
        written = inode_write_bytes(fds[fd].inode, &inode, text, (uint32_t)len_sz, offset);
        if (len_sz > 0 && written < (int)len_sz) {
            ipo_fs_set_last_error(IPO_FS_ERR_NO_SPACE);
        }
        if (!append && written >= 0) {
            inode_truncate_blocks(fds[fd].inode, &inode, (uint64_t)written);
        }
    }
    ipo_fs_close(fd);
    if (written != (int)len_sz) { printf("ipo_fs_write_text: write failed: wrote %d of %u to %s\n", written, (uint32_t)len_sz, path); }
    return written == (int)len_sz;
}

static bool is_descendant(uint64_t ancestor, uint64_t node) {
    if (ancestor == 0 || node == 0) return false;
    uint64_t cur = node;
    while (cur != 1 && cur != 0) {
        if (cur == ancestor) return true;
        struct ipo_inode din;
        if (!read_inode(cur, &din)) break;
        if ((din.mode & IPO_INODE_TYPE_DIR) == 0) break;
        struct ipo_dir_entry dotdot_hdr;
        if (inode_read_bytes(&din, &dotdot_hdr, sizeof(dotdot_hdr), 20) != (int)sizeof(dotdot_hdr)) break;
        uint64_t parent = dir_entry_get_inode(&dotdot_hdr);
        if (parent == cur) break;
        cur = parent;
    }
    return false;
}

bool ipo_fs_rename(const char *oldpath, const char *newpath) {
    if (!fs_mounted || !oldpath || !newpath) { printf("ipo_fs_rename: invalid args or FS not mounted\n"); return false; }
    if (strcmp(oldpath, "/") == 0) { printf("ipo_fs_rename: cannot rename root\n"); return false; }
    uint64_t old_ino = 0, new_ino = 0;
    bool old_resolved = (path_resolve(oldpath, &old_ino) == 0);
    bool new_resolved = (path_resolve(newpath, &new_ino) == 0);
    if (old_resolved && new_resolved && old_ino == new_ino) { return true; }

    char oldname[256];
    uint64_t old_parent;
    if (path_resolve_parent(oldpath, &old_parent, oldname) < 0) {
        printf("ipo_fs_rename: path_resolve_parent failed for %s\n", oldpath);
        return false;
    }
    struct ipo_dir_entry de;
    if (dir_find_entry(old_parent, oldname, &de, NULL) < 0) {
        printf("ipo_fs_rename: dir_find_entry failed for %s\n", oldpath);
        return false;
    }
    uint64_t de_inode = dir_entry_get_inode(&de);
    struct ipo_inode tin;
    if (!read_inode(de_inode, &tin)) {
        printf("ipo_fs_rename: read_inode failed for inode %llu\n", de_inode);
        return false;
    }
    if (tin.mode & IPO_INODE_FLAG_PROTECTED) {
        printf("ipo_fs_rename: target is protected, abort %s\n", oldpath);
        return false;
    }

    char newname[256];
    uint64_t new_parent = 0;
    uint64_t maybe_dir_inode = 0;
    if (path_resolve(newpath, &maybe_dir_inode) == 0) {
        struct ipo_inode td;
        if (!read_inode(maybe_dir_inode, &td)) {
            printf("ipo_fs_rename: read_inode failed for maybe_dir %llu\n", maybe_dir_inode);
            return false;
        }
        if ((td.mode & IPO_INODE_TYPE_DIR) != 0) {
            new_parent = maybe_dir_inode;
            if (de.type == IPO_INODE_TYPE_DIR && is_descendant(de_inode, new_parent)) {
                printf("ipo_fs_rename: cannot move directory into its own descendant\n");
                return false;
            }
            strcpy(newname, oldname);
        } else {
            if (path_resolve_parent(newpath, &new_parent, newname) < 0) {
                printf("ipo_fs_rename: path_resolve_parent failed for newpath %s\n", newpath);
                return false;
            }
        }
    } else {
        if (path_resolve_parent(newpath, &new_parent, newname) < 0) {
            printf("ipo_fs_rename: path_resolve_parent failed for newpath %s\n", newpath);
            return false;
        }
    }

    if (!is_valid_filename(newname)) {
        printf("ipo_fs_rename: invalid target name '%s'\n", newname);
        return false;
    }

    struct ipo_dir_entry tmp;
    if (dir_find_entry(new_parent, newname, &tmp, NULL) == 0) {
        if (dir_entry_get_inode(&tmp) == de_inode) {
            return true;
        }
        printf("ipo_fs_rename: target already exists %s/%s\n", "(parent)", newname);
        return false;
    }

    if (!dir_add_entry(new_parent, newname, de_inode, de.type)) {
        printf("ipo_fs_rename: dir_add_entry failed for %s -> %s\n", oldpath, newpath);
        return false;
    }

    if (de.type == IPO_INODE_TYPE_DIR) {
        struct ipo_inode moved;
        if (read_inode(de_inode, &moved)) {
            struct ipo_dir_entry dotdot_hdr;
            if (inode_read_bytes(&moved, &dotdot_hdr, sizeof(dotdot_hdr), 20) == (int)sizeof(dotdot_hdr)) {
                dir_entry_set_inode(&dotdot_hdr, new_parent);
                inode_write_bytes(de_inode, &moved, &dotdot_hdr, sizeof(dotdot_hdr), 20);
            }
        }
    }

    if (!dir_remove_entry(old_parent, oldname)) {
        printf("ipo_fs_rename: dir_remove_entry failed for old %s\n", oldpath);
        return false;
    }

    return true;
}

int ipo_fs_list_dir(const char *path, char *out, int out_size) {
    if (!fs_mounted || !path || !out || out_size <= 0) return -1;
    uint64_t ino;
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

        uint64_t entry_ino = dir_entry_get_inode(&hdr);
        if (entry_ino != 0 && hdr.name_len > 0) {
            if (pos + (int)hdr.name_len + 3 >= out_size) break;
            if (inode_read_bytes(&din, out + pos, hdr.name_len, offset + sizeof(hdr)) != (int)hdr.name_len) break;
            pos += (int)hdr.name_len;
            if (hdr.type == IPO_INODE_TYPE_DIR) out[pos++] = '/';
            out[pos++] = '\n';
        }
        offset += hdr.rec_len;
    }
    if (pos < out_size) out[pos] = '\0'; else out[out_size - 1] = '\0';
    return pos;
}

void ipo_fs_wipe_storage(void) {
    uint8_t *scratch = (uint8_t *)kmalloc(512 * 9);
    if (!scratch) {
        printf("  Out of memory during storage wipe.\n");
        return;
    }
    uint8_t *zero_buf = scratch;
    uint8_t *chk = scratch + (512 * 8);
    memset(zero_buf, 0, 512 * 8);

    block_device_t *bdev = block_dev_get_list();
    uint32_t wiped_count = 0;
    while (bdev) {
        bool is_boot = ((bdev->flags & BLOCK_DEV_BOOTABLE) != 0);
        if (!is_boot) {
            if (block_dev_read(bdev, 0, 1, chk) && chk[510] == 0x55 && chk[511] == 0xAA) {
                bdev->flags |= BLOCK_DEV_BOOTABLE;
                is_boot = true;
            }
        }

        if (is_boot) {
            printf("  Wiping storage '%s' (%u sectors, preserving MBR/kernel LBA 0..2047)...\n",
                   bdev->name ? bdev->name : "disk", (uint32_t)bdev->total_sectors);

            if (bdev->total_sectors >= 2048) {
                for (uint64_t lba = 2031; lba < 2048; lba += 8) {
                    uint16_t count = (2048 - lba < 8) ? (uint16_t)(2048 - lba) : 8;
                    block_dev_write(bdev, lba, count, zero_buf);
                }
            }

            uint64_t end_lba = bdev->total_sectors < 4096 ? bdev->total_sectors : 4096;
            for (uint64_t lba = 2048; lba < end_lba; lba += 8) {
                uint16_t count = (end_lba - lba < 8) ? (uint16_t)(end_lba - lba) : 8;
                block_dev_write(bdev, lba, count, zero_buf);
            }
        } else {
            printf("  Wiping data storage '%s' (%u sectors, full data wipe)...\n",
                   bdev->name ? bdev->name : "disk", (uint32_t)bdev->total_sectors);

            uint64_t sectors_to_wipe = bdev->total_sectors < 2048 ? bdev->total_sectors : 2048;
            for (uint64_t lba = 0; lba < sectors_to_wipe; lba += 8) {
                uint16_t count = (sectors_to_wipe - lba < 8) ? (uint16_t)(sectors_to_wipe - lba) : 8;
                block_dev_write(bdev, lba, count, zero_buf);
            }
        }

        block_dev_flush(bdev);
        wiped_count++;
        bdev = bdev->next;
    }

    kfree(scratch);
    printf("  Wiped %u connected storage device(s).\n", wiped_count);
}

bool ipo_fs_install(void) {
    const char *target_data_dev = NULL;
    block_device_t *bdev = block_dev_get_list();
    while (bdev) {
        if (!(bdev->flags & BLOCK_DEV_BOOTABLE) && bdev->total_sectors >= 2048 + 10) {
            target_data_dev = bdev->name;
            break;
        }
        bdev = bdev->next;
    }

    storage_pool_create_ex(NULL, target_data_dev);

    uint64_t pool_capacity = ata_get_pool_capacity();
    if (pool_capacity == 0) {
        printf("[ipo_fs] Disk not detected\n");
        return false;
    }
    if (pool_capacity < 2048 + 10) {
        printf("[ipo_fs] Disk too small\n");
        return false;
    }

    uint64_t start_lba = 2048;
    uint64_t total_blocks = pool_capacity - 2048;
    uint64_t total_inodes = total_blocks / 16;
    if (total_inodes == 0) total_inodes = 1;

    printf("Storage Pool: %u sectors.\nFormatting %u blocks (%u inodes) at LBA %u...\n",
           (uint32_t)pool_capacity, (uint32_t)total_blocks, (uint32_t)total_inodes, (uint32_t)start_lba);

    if (!ipo_fs_format(start_lba, total_blocks, total_inodes)) {
        printf("ipo_fs_format failed\n");
        return false;
    }

    if (!ipo_fs_mount(start_lba)) {
        printf("ipo_fs_mount failed after format\n");
        return false;
    }

    printf("[ipo_fs] Mounted IPO_FS at LBA %u\n", (uint32_t)start_lba);
    return true;
}
