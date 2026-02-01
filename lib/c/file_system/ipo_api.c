#include <file_system/ipo_fs.h>
#include <driver/ata/ata.h>
#include <string.h>
#include <stdio.h>

/* helpers */
static void write_dir_dots(uint32_t inode_no, uint32_t parent, uint32_t block) {
    struct ipo_dir_entry de;
    memset(&de,0,sizeof(de));
    de.inode = inode_no; de.type = IPO_INODE_TYPE_DIR; strncpy(de.name, ".", IPO_FS_MAX_NAME-1); de.name_len = 1;
    uint8_t buf[IPO_FS_BLOCK_SIZE]; memset(buf,0,sizeof(buf));
    memcpy(buf, &de, sizeof(de));
    de.inode = parent; de.type = IPO_INODE_TYPE_DIR; strncpy(de.name, "..", IPO_FS_MAX_NAME-1); de.name_len = 2;
    memcpy(buf + sizeof(de), &de, sizeof(de));
    block_write(block, buf);
}

bool ipo_fs_format(uint32_t disk_start_lba, uint32_t total_blocks, uint32_t total_inodes) {
    printf("ipo_fs_format: start=%u blocks=%u inodes=%u\n", disk_start_lba, total_blocks, total_inodes);
    if (total_blocks < 10) { printf("ipo_fs_format: too few blocks\n"); return false; }
    uint32_t inode_table_blocks = (total_inodes * sizeof(struct ipo_inode) + IPO_FS_BLOCK_SIZE - 1) / IPO_FS_BLOCK_SIZE;
    uint32_t inode_bitmap_blocks = (total_inodes + IPO_FS_BLOCK_SIZE*8 - 1) / (IPO_FS_BLOCK_SIZE*8);
    uint32_t block_bitmap_blocks = 1;
    uint32_t data_blocks = 0;
    for (int iter = 0; iter < 8; iter++) {
        data_blocks = total_blocks - 1 - inode_bitmap_blocks - block_bitmap_blocks - inode_table_blocks;
        uint32_t nb = (data_blocks + IPO_FS_BLOCK_SIZE*8 - 1) / (IPO_FS_BLOCK_SIZE*8);
        if (nb == block_bitmap_blocks) break;
        block_bitmap_blocks = nb;
    }
    data_blocks = total_blocks - 1 - inode_bitmap_blocks - block_bitmap_blocks - inode_table_blocks;
    if ((int)data_blocks <= 0) { printf("ipo_fs_format: not enough data blocks computed (%d)\n", (int)data_blocks); return false; }

    struct ipo_superblock s;
    memset(&s,0,sizeof(s));
    memset(s.magic, 0, sizeof(s.magic));
    strncpy(s.magic, IPO_FS_MAGIC_STR, sizeof(s.magic)-1);
    
    s.fs_size_blocks = total_blocks;
    s.block_size = IPO_FS_BLOCK_SIZE;
    s.inode_count = total_inodes;
    s.inode_bitmap_start = 1;
    s.block_bitmap_start = s.inode_bitmap_start + inode_bitmap_blocks;
    s.inode_table_start = s.block_bitmap_start + block_bitmap_blocks;
    s.data_blocks_start = s.inode_table_start + inode_table_blocks;

    fs_start_lba = disk_start_lba;
    printf("ipo_fs_format: layout: inode_bitmap_start=%u inode_bitmap_blocks=%u block_bitmap_start=%u block_bitmap_blocks=%u inode_table_start=%u inode_table_blocks=%u data_blocks_start=%u data_blocks=%u\n",
           s.inode_bitmap_start, inode_bitmap_blocks, s.block_bitmap_start, block_bitmap_blocks, s.inode_table_start, inode_table_blocks, s.data_blocks_start, data_blocks);

    printf("ipo_fs_format: writing superblock at lba=%u\n", fs_start_lba + 0);
    if (!ata_write_sectors_lba28(fs_start_lba + 0, 1, &s)) { printf("ipo_fs_format: ata_write_sectors_lba28(super) failed\n"); return false; }

    uint8_t zero[IPO_FS_BLOCK_SIZE]; memset(zero,0,sizeof(zero));
    for (uint32_t i = 0; i < inode_bitmap_blocks; i++) {
        if (!block_write(s.inode_bitmap_start + i, zero)) { printf("ipo_fs_format: write inode_bitmap failed at %u\n", i); return false; }
    }
    for (uint32_t i = 0; i < block_bitmap_blocks; i++) {
        if (!block_write(s.block_bitmap_start + i, zero)) { printf("ipo_fs_format: write block_bitmap failed at %u\n", i); return false; }
    }
    for (uint32_t i = 0; i < inode_table_blocks; i++) {
        if (!block_write(s.inode_table_start + i, zero)) { printf("ipo_fs_format: write inode_table failed at %u\n", i); return false; }
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

    int root_block = allocate_block();
    if (root_block < 0) { printf("ipo_fs_format: allocate_block failed for root\n"); return false; }
    root.direct[0] = root_block;
    write_dir_dots(1, 1, root_block);
    root.size = sizeof(struct ipo_dir_entry) * 2;
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
    int app_block = allocate_block();
    if (app_block < 0) { printf("ipo_fs_format: allocate_block failed for /app\n"); return false; }
    app_inode.direct[0] = app_block;
    write_dir_dots(app_ino, 1, app_block);
    app_inode.size = sizeof(struct ipo_dir_entry) * 2;
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

bool ipo_fs_mount(uint32_t disk_start_lba) {
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
    char name[IPO_FS_MAX_NAME]; uint32_t parent;
    if (path_resolve_parent(path, &parent, name) < 0) return -1;
    if (!is_valid_filename(name)) return -1;
    struct ipo_dir_entry de;
    if (dir_find_entry(parent, name, &de, NULL, NULL) == 0) return -1;
    int ino = allocate_inode();
    if (ino < 0) return -1;
    struct ipo_inode inode;
    memset(&inode,0,sizeof(inode));
    inode.mode = type;
    inode.size = 0;
    inode.links_count = 1;
    write_inode(ino, &inode);
    if (!dir_add_entry(parent, name, ino, type)) {
        free_inode(ino);
        return -1;
    }
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

int ipo_fs_read(int fd, void *buffer, uint32_t size, uint32_t offset) {
    if (fd < 0 || fd >= IPO_MAX_FDS) return -1;
    if (!fds[fd].used) return -1;
    struct ipo_inode inode;
    if (!read_inode(fds[fd].inode, &inode)) return -1;
    if (offset >= inode.size) return 0;
    if (offset + size > inode.size) size = inode.size - offset;
    uint32_t first_block = offset / IPO_FS_BLOCK_SIZE;
    uint32_t last_block = (offset + size - 1) / IPO_FS_BLOCK_SIZE;
    uint8_t tmp[IPO_FS_BLOCK_SIZE];
    uint32_t copied = 0;
    for (uint32_t b = first_block; b <= last_block; b++) {
        int phys = get_data_block_for_inode(&inode, b, false);
        if (phys < 0) break;
        block_read(phys, tmp);
        uint32_t block_offset = (b == first_block) ? (offset % IPO_FS_BLOCK_SIZE) : 0;
        uint32_t tocopy = IPO_FS_BLOCK_SIZE - block_offset;
        if (tocopy > size - copied) tocopy = size - copied;
        memcpy((uint8_t*)buffer + copied, tmp + block_offset, tocopy);
        copied += tocopy;
    }
    return copied;
}

int ipo_fs_write(int fd, const void *buffer, uint32_t size, uint32_t offset) {
    if (fd < 0 || fd >= IPO_MAX_FDS) return -1;
    if (!fds[fd].used) return -1;
    struct ipo_inode inode;
    if (!read_inode(fds[fd].inode, &inode)) return -1;
    uint32_t first_block = offset / IPO_FS_BLOCK_SIZE;
    uint32_t last_block = (offset + size - 1) / IPO_FS_BLOCK_SIZE;
    uint8_t tmp[IPO_FS_BLOCK_SIZE];
    uint32_t written = 0;
    for (uint32_t b = first_block; b <= last_block; b++) {
        int phys = get_data_block_for_inode(&inode, b, true);
        if (phys < 0) break;
        block_read(phys, tmp);
        uint32_t block_offset = (b == first_block) ? (offset % IPO_FS_BLOCK_SIZE) : 0;
        uint32_t towrite = IPO_FS_BLOCK_SIZE - block_offset;
        if (towrite > size - written) towrite = size - written;
        memcpy(tmp + block_offset, (uint8_t*)buffer + written, towrite);
        block_write(phys, tmp);
        written += towrite;
    }
    if (offset + written > inode.size) inode.size = offset + written;
    write_inode(fds[fd].inode, &inode);
    return written;
}

bool ipo_fs_delete(const char *path) {
    if (!fs_mounted) return false;
    char name[IPO_FS_MAX_NAME]; uint32_t parent;
    if (path_resolve_parent(path, &parent, name) < 0) return false;
    struct ipo_dir_entry de;
    if (dir_find_entry(parent, name, &de, NULL, NULL) < 0) return false;
    struct ipo_inode target_inode;
    if (!read_inode(de.inode, &target_inode)) return false;
    if (target_inode.mode & IPO_INODE_FLAG_PROTECTED) return false;
    if (de.type == IPO_INODE_TYPE_DIR) {
        struct ipo_inode din;
        read_inode(de.inode, &din);
        if (din.size > sizeof(struct ipo_dir_entry) * 2) return false;
    }
    if (!dir_remove_entry(parent, name)) return false;
    free_inode(de.inode);
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
    uint32_t offset = append ? inode.size : 0;
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
        if (!din.direct[0]) break;
        uint8_t buf[IPO_FS_BLOCK_SIZE]; if (!block_read(din.direct[0], buf)) break;
        struct ipo_dir_entry *entries = (struct ipo_dir_entry *)buf;
        uint32_t parent = entries[1].inode;
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

    char oldname[IPO_FS_MAX_NAME]; uint32_t old_parent;
    if (path_resolve_parent(oldpath, &old_parent, oldname) < 0) { printf("ipo_fs_rename: path_resolve_parent failed for %s\n", oldpath); return false; }
    struct ipo_dir_entry de;
    if (dir_find_entry(old_parent, oldname, &de, NULL, NULL) < 0) { printf("ipo_fs_rename: dir_find_entry failed for %s\n", oldpath); return false; }
    struct ipo_inode tin;
    if (!read_inode(de.inode, &tin)) { printf("ipo_fs_rename: read_inode failed for inode %u\n", de.inode); return false; }
    if (tin.mode & IPO_INODE_FLAG_PROTECTED) { printf("ipo_fs_rename: target is protected, abort %s\n", oldpath); return false; }

    char newname[IPO_FS_MAX_NAME]; uint32_t new_parent;
    uint32_t maybe_dir_inode;
    if (path_resolve(newpath, &maybe_dir_inode) == 0) {
        struct ipo_inode td;
        if (!read_inode(maybe_dir_inode, &td)) { printf("ipo_fs_rename: read_inode failed for maybe_dir %u\n", maybe_dir_inode); return false; }
        if ((td.mode & IPO_INODE_TYPE_DIR) != 0) {
            new_parent = maybe_dir_inode;
            if (de.type == IPO_INODE_TYPE_DIR && is_descendant(de.inode, new_parent)) { printf("ipo_fs_rename: cannot move directory into its own descendant\n"); return false; }
            strncpy(newname, oldname, IPO_FS_MAX_NAME-1); newname[IPO_FS_MAX_NAME-1] = '\0';
        } else {
            if (path_resolve_parent(newpath, &new_parent, newname) < 0) { printf("ipo_fs_rename: path_resolve_parent failed for newpath %s\n", newpath); return false; }
        }
    } else {
        if (path_resolve_parent(newpath, &new_parent, newname) < 0) { printf("ipo_fs_rename: path_resolve_parent failed for newpath %s\n", newpath); return false; }
    }
    if (!is_valid_filename(newname)) { printf("ipo_fs_rename: invalid target name '%s'\n", newname); return false; }
    struct ipo_dir_entry tmp;
    if (dir_find_entry(new_parent, newname, &tmp, NULL, NULL) == 0) {
        if (tmp.inode == de.inode) { return true; }
        printf("ipo_fs_rename: target already exists %s/%s\n", "(parent)", newname);
        return false;
    }
    if (!dir_add_entry(new_parent, newname, de.inode, de.type)) { printf("ipo_fs_rename: dir_add_entry failed for %s -> %s\n", oldpath, newpath); return false; }
    if (de.type == IPO_INODE_TYPE_DIR) {
        struct ipo_inode moved;
        if (!read_inode(de.inode, &moved)) { printf("ipo_fs_rename: read_inode failed for moved dir inode %u\n", de.inode); return false; }
        if (moved.direct[0]) {
            uint8_t buf[IPO_FS_BLOCK_SIZE];
            if (!block_read(moved.direct[0], buf)) { printf("ipo_fs_rename: block_read failed for dir block %u\n", moved.direct[0]); return false; }
            struct ipo_dir_entry *entries = (struct ipo_dir_entry *)buf;
            entries[1].inode = new_parent;
            if (!block_write(moved.direct[0], buf)) { printf("ipo_fs_rename: block_write failed updating '..'\n"); return false; }
        }
    }
    if (!dir_remove_entry(old_parent, oldname)) { printf("ipo_fs_rename: dir_remove_entry failed for old %s\n", oldpath); return false; }
    return true;
}
