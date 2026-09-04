#include <file_system/ipo_fs.h>
#include <driver/ata/ata.h>
#include <string.h>
#include <stdio.h>
#include <memory/kmalloc.h>

/* helpers */
static void write_dir_dots(uint32_t inode_no, uint32_t parent, uint64_t block) {
    uint8_t buf[IPO_FS_BLOCK_SIZE];
    memset(buf, 0, sizeof(buf));

    // . -> rec_len = 20, name_len = 1
    struct ipo_dir_entry d1;
    d1.inode = inode_no;
    d1.rec_len = 20;
    d1.name_len = 1;
    d1.type = IPO_INODE_TYPE_DIR;
    memset(d1.reserved, 0, sizeof(d1.reserved));
    memcpy(buf, &d1, sizeof(d1));
    memcpy(buf + sizeof(d1), ".\0\0\0", 4);

    // .. -> rec_len = 20, name_len = 2
    struct ipo_dir_entry d2;
    d2.inode = parent;
    d2.rec_len = 20;
    d2.name_len = 2;
    d2.type = IPO_INODE_TYPE_DIR;
    memset(d2.reserved, 0, sizeof(d2.reserved));
    memcpy(buf + 20, &d2, sizeof(d2));
    memcpy(buf + 20 + sizeof(d2), "..\0\0", 4);

    block_write(block, buf);
}

bool ipo_fs_format(uint64_t disk_start_lba, uint64_t total_blocks, uint64_t total_inodes) {
    if (total_blocks < 10) { printf("ipo_fs_format: too few blocks\n"); return false; }
    uint64_t inode_table_blocks = (total_inodes * sizeof(struct ipo_inode) + IPO_FS_BLOCK_SIZE - 1) / IPO_FS_BLOCK_SIZE;
    uint64_t inode_bitmap_blocks = (total_inodes + IPO_FS_BLOCK_SIZE*8 - 1) / (IPO_FS_BLOCK_SIZE*8);
    uint64_t block_bitmap_blocks = 1;
    uint64_t data_blocks = 0;
    for (int iter = 0; iter < 8; iter++) {
        data_blocks = total_blocks - 1 - inode_bitmap_blocks - block_bitmap_blocks - inode_table_blocks;
        uint64_t nb = (data_blocks + IPO_FS_BLOCK_SIZE*8 - 1) / (IPO_FS_BLOCK_SIZE*8);
        if (nb == block_bitmap_blocks) break;
        block_bitmap_blocks = nb;
    }
    data_blocks = total_blocks - 1 - inode_bitmap_blocks - block_bitmap_blocks - inode_table_blocks;
    if ((int64_t)data_blocks <= 0) { printf("ipo_fs_format: not enough data blocks computed\n"); return false; }

    struct ipo_superblock s;
    memset(&s,0,sizeof(s));
    memset(s.magic, 0, sizeof(s.magic));
    strncpy(s.magic, IPO_FS_MAGIC_STR, sizeof(s.magic)-1);
    
    s.fs_size_blocks = total_blocks;
    s.block_size = IPO_FS_BLOCK_SIZE;
    s.flags = 1; /* Linked Extents enabled */
    s.inode_count = total_inodes;
    s.inode_bitmap_start = 1;
    s.block_bitmap_start = s.inode_bitmap_start + inode_bitmap_blocks;
    s.inode_table_start = s.block_bitmap_start + block_bitmap_blocks;
    s.data_blocks_start = s.inode_table_start + inode_table_blocks;

    fs_start_lba = disk_start_lba;

    if (!ata_pool_write_sectors(fs_start_lba + 0, 1, &s)) { printf("ipo_fs_format: write super failed\n"); return false; }

    uint8_t zero[IPO_FS_BLOCK_SIZE]; memset(zero,0,sizeof(zero));
    for (uint64_t i = 0; i < inode_bitmap_blocks; i++) {
        if (!block_write(s.inode_bitmap_start + i, zero)) return false;
    }
    for (uint64_t i = 0; i < block_bitmap_blocks; i++) {
        if (!block_write(s.block_bitmap_start + i, zero)) return false;
    }
    for (uint64_t i = 0; i < inode_table_blocks; i++) {
        if (!block_write(s.inode_table_start + i, zero)) return false;
    }

    /* initialize superblock and root */
    memcpy(&sb, &s, sizeof(sb));
    fs_start_lba = disk_start_lba;

    /* mark inode 1 as used */
    bitmap_set(sb.inode_bitmap_start, 0, true);
    struct ipo_inode root;
    memset(&root,0,sizeof(root));
    root.mode = IPO_INODE_TYPE_DIR;
    root.size = 0;
    root.links_count = 2;
    write_inode(1, &root);

    int64_t root_block = allocate_block();
    if (root_block < 0) { printf("ipo_fs_format: allocate_block failed for root\n"); return false; }
    root.extents[0].logical_block = 0;
    root.extents[0].physical_block = (uint64_t)root_block;
    root.extents[0].block_count = 1;
    write_dir_dots(1, 1, (uint64_t)root_block);
    root.size = 40;
    write_inode(1, &root);

    /* create /app directory (protected) */
    int app_ino = allocate_inode();
    if (app_ino < 0) { printf("ipo_fs_format: allocate_inode failed for /app\n"); return false; }
    struct ipo_inode app_inode;
    memset(&app_inode, 0, sizeof(app_inode));
    app_inode.mode = IPO_INODE_TYPE_DIR | IPO_INODE_FLAG_PROTECTED;
    app_inode.size = 0;
    app_inode.links_count = 2;
    write_inode(app_ino, &app_inode);
    int64_t app_block = allocate_block();
    if (app_block < 0) { printf("ipo_fs_format: allocate_block failed for /app\n"); return false; }
    app_inode.extents[0].logical_block = 0;
    app_inode.extents[0].physical_block = (uint64_t)app_block;
    app_inode.extents[0].block_count = 1;
    write_dir_dots(app_ino, 1, (uint64_t)app_block);
    app_inode.size = 40;
    write_inode(app_ino, &app_inode);
    if (!dir_add_entry(1, "app", app_ino, IPO_INODE_TYPE_DIR)) { printf("ipo_fs_format: dir_add_entry failed for /app\n"); return false; }

    /* create /autorun file (protected, empty) */
    int autorun_ino = allocate_inode();
    if (autorun_ino < 0) { printf("ipo_fs_format: allocate_inode failed for /autorun\n"); return false; }
    struct ipo_inode ar_inode;
    memset(&ar_inode, 0, sizeof(ar_inode));
    ar_inode.mode = IPO_INODE_TYPE_FILE | IPO_INODE_FLAG_PROTECTED;
    ar_inode.size = 0;
    ar_inode.links_count = 1;
    write_inode(autorun_ino, &ar_inode);
    if (!dir_add_entry(1, "autorun", autorun_ino, IPO_INODE_TYPE_FILE)) { printf("ipo_fs_format: dir_add_entry failed for /autorun\n"); return false; }

    /* save superblock to disk */
    if (!block_write(0, &sb)) { printf("ipo_fs_format: failed to write superblock\n"); return false; }
    return true;
}

bool ipo_fs_mount(uint64_t disk_start_lba) {
    fs_start_lba = disk_start_lba;
    uint8_t buf[IPO_FS_BLOCK_SIZE];
    if (!block_read(0, buf)) return false;
    memcpy(&sb, buf, sizeof(sb));
    if (strncmp(sb.magic, IPO_FS_MAGIC_STR, sizeof(IPO_FS_MAGIC_STR)-1) != 0) return false;
    if (sb.block_size != IPO_FS_BLOCK_SIZE) return false;
    fs_mounted = true;
    return true;
}

int ipo_fs_create(const char *path, uint8_t type) {
    if (!fs_mounted) return -1;
    size_t plen = strlen(path);
    char *name = (char *)kmalloc(plen + 16);
    if (!name) return -1;
    uint32_t parent;
    if (path_resolve_parent(path, &parent, name) < 0) {
        kfree(name);
        return -1;
    }
    if (!is_valid_filename(name)) {
        kfree(name);
        return -1;
    }
    struct ipo_dir_entry de;
    if (dir_find_entry(parent, name, &de, NULL) == 0) {
        kfree(name);
        return -1;
    }
    int ino = allocate_inode();
    if (ino < 0) {
        kfree(name);
        return -1;
    }
    struct ipo_inode inode;
    memset(&inode, 0, sizeof(inode));
    inode.mode = type;
    inode.size = 0;
    inode.links_count = (type == IPO_INODE_TYPE_DIR) ? 2 : 1;

    if (type == IPO_INODE_TYPE_DIR) {
        int64_t blk = allocate_block();
        if (blk < 0) {
            free_inode(ino);
            kfree(name);
            return -1;
        }
        inode.extents[0].logical_block = 0;
        inode.extents[0].physical_block = (uint64_t)blk;
        inode.extents[0].block_count = 1;
        write_dir_dots((uint32_t)ino, parent, (uint64_t)blk);
        inode.size = 40;
    }

    write_inode(ino, &inode);
    if (!dir_add_entry(parent, name, ino, type)) {
        free_inode(ino);
        kfree(name);
        return -1;
    }
    kfree(name);
    return ino;
}

int ipo_fs_open(const char *path) {
    if (!fs_mounted) return -1;
    uint32_t ino;
    if (path_resolve(path, &ino) < 0) return -1;
    struct ipo_inode inode;
    if (!read_inode(ino, &inode)) return -1;
    if ((inode.mode & IPO_INODE_TYPE_DIR) != 0) return -1;
    for (int i=0;i<IPO_MAX_FDS;i++) {
        if (!fds[i].used) {
            fds[i].used = 1;
            fds[i].inode = ino;
            fds[i].offset = 0;
            return i;
        }
    }
    return -1;
}

int ipo_fs_close(int fd) {
    if (fd < 0 || fd >= IPO_MAX_FDS) return -1;
    if (!fds[fd].used) return -1;

    fds[fd].used = 0;
    fds[fd].inode = 0;
    fds[fd].offset = 0;
    fds[fd].flags = 0;
    return 0;
}

int ipo_fs_read(int fd, void *buffer, uint32_t size, uint32_t offset) {
    if (fd < 0 || fd >= IPO_MAX_FDS) return -1;
    if (!fds[fd].used) return -1;
    struct ipo_inode inode;
    if (!read_inode(fds[fd].inode, &inode)) return -1;
    return inode_read_bytes(&inode, buffer, size, offset);
}

int ipo_fs_write(int fd, const void *buffer, uint32_t size, uint32_t offset) {
    if (fd < 0 || fd >= IPO_MAX_FDS) return -1;
    if (!fds[fd].used) return -1;
    struct ipo_inode inode;
    if (!read_inode(fds[fd].inode, &inode)) return -1;
    return inode_write_bytes(fds[fd].inode, &inode, buffer, size, offset);
}

bool ipo_fs_delete(const char *path) {
    if (!fs_mounted) return false;
    size_t plen = strlen(path);
    char *name = (char *)kmalloc(plen + 16);
    if (!name) return false;
    uint32_t parent;
    if (path_resolve_parent(path, &parent, name) < 0) {
        kfree(name);
        return false;
    }
    struct ipo_dir_entry de;
    if (dir_find_entry(parent, name, &de, NULL) < 0) {
        kfree(name);
        return false;
    }
    struct ipo_inode target_inode;
    if (!read_inode(de.inode, &target_inode)) {
        kfree(name);
        return false;
    }
    if (target_inode.mode & IPO_INODE_FLAG_PROTECTED) {
        kfree(name);
        return false;
    }
    if (de.type == IPO_INODE_TYPE_DIR) {
        struct ipo_inode din;
        read_inode(de.inode, &din);
        uint64_t offset = 0;
        while (offset < din.size) {
            struct ipo_dir_entry hdr;
            if (inode_read_bytes(&din, &hdr, sizeof(hdr), offset) != (int)sizeof(hdr)) break;
            if (hdr.rec_len < sizeof(hdr)) break;
            if (hdr.inode != 0 && hdr.name_len > 0) {
                char nbuf[8] = {0};
                uint32_t rlen = hdr.name_len < 7 ? hdr.name_len : 7;
                inode_read_bytes(&din, nbuf, rlen, offset + sizeof(hdr));
                nbuf[rlen] = '\0';
                if (strcmp(nbuf, ".") != 0 && strcmp(nbuf, "..") != 0) {
                    kfree(name);
                    return false; /* Directory not empty */
                }
            }
            offset += hdr.rec_len;
        }
    }
    if (!dir_remove_entry(parent, name)) {
        kfree(name);
        return false;
    }
    free_inode(de.inode);
    kfree(name);
    return true;
}

bool ipo_fs_stat(const char *path, struct ipo_inode *out) {
    if (!fs_mounted || !path || !out) return false;
    uint32_t ino;
    if (path_resolve(path, &ino) < 0) return false;
    return read_inode(ino, out);
}

bool ipo_fs_write_text(const char *path, const char *text, bool append) {
    if (!fs_mounted || !path || !text) { printf("ipo_fs_write_text: invalid args or FS not mounted\n"); return false; }
    uint32_t ino;
    if (path_resolve(path, &ino) < 0) {
        if (ipo_fs_create(path, IPO_INODE_TYPE_FILE) < 0) { printf("ipo_fs_write_text: failed to create %s\n", path); return false; }
    }
    struct ipo_inode inode;
    if (!ipo_fs_stat(path, &inode)) { printf("ipo_fs_write_text: stat failed for %s\n", path); return false; }
    if ((inode.mode & IPO_INODE_TYPE_DIR) != 0) { printf("ipo_fs_write_text: target is a directory %s\n", path); return false; }
    uint32_t offset = append ? (uint32_t)inode.size : 0;
    int fd = ipo_fs_open(path);
    if (fd < 0) { printf("ipo_fs_write_text: failed to open %s\n", path); return false; }
    int len = strlen(text);
    int written = ipo_fs_write(fd, text, len, offset);
    if (written != len) { printf("ipo_fs_write_text: write failed: wrote %d of %d to %s\n", written, len, path); }
    return written == len;
}

static bool is_descendant(uint32_t ancestor, uint32_t node) {
    if (ancestor == 0 || node == 0) return false;
    uint32_t cur = node;
    while (cur != 1 && cur != 0) {
        if (cur == ancestor) return true;
        struct ipo_inode din;
        if (!read_inode(cur, &din)) break;
        if ((din.mode & IPO_INODE_TYPE_DIR) == 0) break;
        struct ipo_dir_entry dotdot_hdr;
        if (inode_read_bytes(&din, &dotdot_hdr, sizeof(dotdot_hdr), 20) != (int)sizeof(dotdot_hdr)) break;
        uint32_t parent = dotdot_hdr.inode;
        if (parent == cur) break;
        cur = parent;
    }
    return false;
}

bool ipo_fs_rename(const char *oldpath, const char *newpath) {
    if (!fs_mounted || !oldpath || !newpath) { printf("ipo_fs_rename: invalid args or FS not mounted\n"); return false; }
    if (strcmp(oldpath, "/") == 0) { printf("ipo_fs_rename: cannot rename root\n"); return false; }
    uint32_t old_ino = 0, new_ino = 0;
    bool old_resolved = (path_resolve(oldpath, &old_ino) == 0);
    bool new_resolved = (path_resolve(newpath, &new_ino) == 0);
    if (old_resolved && new_resolved && old_ino == new_ino) { return true; }

    size_t old_len = strlen(oldpath);
    char *oldname = (char *)kmalloc(old_len + 16);
    if (!oldname) return false;
    uint32_t old_parent;
    if (path_resolve_parent(oldpath, &old_parent, oldname) < 0) {
        kfree(oldname);
        printf("ipo_fs_rename: path_resolve_parent failed for %s\n", oldpath);
        return false;
    }
    struct ipo_dir_entry de;
    if (dir_find_entry(old_parent, oldname, &de, NULL) < 0) {
        kfree(oldname);
        printf("ipo_fs_rename: dir_find_entry failed for %s\n", oldpath);
        return false;
    }
    struct ipo_inode tin;
    if (!read_inode(de.inode, &tin)) {
        kfree(oldname);
        printf("ipo_fs_rename: read_inode failed for inode %u\n", de.inode);
        return false;
    }
    if (tin.mode & IPO_INODE_FLAG_PROTECTED) {
        kfree(oldname);
        printf("ipo_fs_rename: target is protected, abort %s\n", oldpath);
        return false;
    }

    size_t new_len = strlen(newpath);
    size_t name_buf_size = (new_len > old_len ? new_len : old_len) + 32;
    char *newname = (char *)kmalloc(name_buf_size);
    if (!newname) {
        kfree(oldname);
        return false;
    }
    uint32_t new_parent;
    uint32_t maybe_dir_inode;
    if (path_resolve(newpath, &maybe_dir_inode) == 0) {
        struct ipo_inode td;
        if (!read_inode(maybe_dir_inode, &td)) {
            kfree(oldname); kfree(newname);
            printf("ipo_fs_rename: read_inode failed for maybe_dir %u\n", maybe_dir_inode);
            return false;
        }
        if ((td.mode & IPO_INODE_TYPE_DIR) != 0) {
            new_parent = maybe_dir_inode;
            if (de.type == IPO_INODE_TYPE_DIR && is_descendant(de.inode, new_parent)) {
                kfree(oldname); kfree(newname);
                printf("ipo_fs_rename: cannot move directory into its own descendant\n");
                return false;
            }
            strcpy(newname, oldname);
        } else {
            if (path_resolve_parent(newpath, &new_parent, newname) < 0) {
                kfree(oldname); kfree(newname);
                printf("ipo_fs_rename: path_resolve_parent failed for newpath %s\n", newpath);
                return false;
            }
        }
    } else {
        if (path_resolve_parent(newpath, &new_parent, newname) < 0) {
            kfree(oldname); kfree(newname);
            printf("ipo_fs_rename: path_resolve_parent failed for newpath %s\n", newpath);
            return false;
        }
    }

    if (!is_valid_filename(newname)) {
        kfree(oldname); kfree(newname);
        printf("ipo_fs_rename: invalid target name '%s'\n", newname);
        return false;
    }

    struct ipo_dir_entry tmp;
    if (dir_find_entry(new_parent, newname, &tmp, NULL) == 0) {
        if (tmp.inode == de.inode) {
            kfree(oldname); kfree(newname);
            return true;
        }
        printf("ipo_fs_rename: target already exists %s/%s\n", "(parent)", newname);
        kfree(oldname); kfree(newname);
        return false;
    }

    if (!dir_add_entry(new_parent, newname, de.inode, de.type)) {
        kfree(oldname); kfree(newname);
        printf("ipo_fs_rename: dir_add_entry failed for %s -> %s\n", oldpath, newpath);
        return false;
    }

    if (de.type == IPO_INODE_TYPE_DIR) {
        struct ipo_inode moved;
        if (read_inode(de.inode, &moved)) {
            struct ipo_dir_entry dotdot_hdr;
            if (inode_read_bytes(&moved, &dotdot_hdr, sizeof(dotdot_hdr), 20) == (int)sizeof(dotdot_hdr)) {
                dotdot_hdr.inode = new_parent;
                inode_write_bytes(de.inode, &moved, &dotdot_hdr, sizeof(dotdot_hdr), 20);
            }
        }
    }

    if (!dir_remove_entry(old_parent, oldname)) {
        printf("ipo_fs_rename: dir_remove_entry failed for old %s\n", oldpath);
        kfree(oldname); kfree(newname);
        return false;
    }

    kfree(oldname);
    kfree(newname);
    return true;
}
