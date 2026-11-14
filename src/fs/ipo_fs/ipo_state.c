#include <file_system/ipo_fs.h>
#include <string.h>
#include <memory/kmalloc.h>
#include <stdio.h>

struct ipo_superblock sb;
uint64_t fs_start_lba = 0;
bool fs_mounted = false;
struct ipo_fd *fds = NULL;
uint32_t fds_capacity = 0;

int ipo_fs_last_error = IPO_FS_OK;

struct ipo_inode_chunk_desc cached_chunk;
bool cached_chunk_valid = false;

void ipo_fs_set_last_error(int err) {
    ipo_fs_last_error = err;
}

int ipo_fs_get_last_error(void) {
    return ipo_fs_last_error;
}

const char *ipo_fs_get_last_error_str(void) {
    switch (ipo_fs_last_error) {
        case IPO_FS_OK:
            return "No error";
        case IPO_FS_ERR_NO_SPACE:
            return "Volume full (no free data blocks for metadata or data)";
        case IPO_FS_ERR_INODE_LIMIT:
            return "Maximum inode capacity limit reached";
        case IPO_FS_ERR_CORRUPT:
            return "Corrupted filesystem metadata structure";
        case IPO_FS_ERR_NOT_MOUNTED:
            return "Filesystem is not mounted";
        case IPO_FS_ERR_IO:
            return "Storage pool block read/write I/O error";
        case IPO_FS_ERR_INVALID_ARG:
            return "Invalid arguments or path";
        default:
            return "Unknown filesystem error";
    }
}

void ipo_fs_reset_superblock(void) {
    memset(&sb, 0, sizeof(sb));
    fs_mounted = false;
    ipo_fs_last_error = IPO_FS_OK;
    cached_chunk_valid = false;
    memset(&cached_chunk, 0, sizeof(cached_chunk));
}

void ipo_fs_fds_reset(void) {
    if (!fds) {
        struct ipo_fd *new_fds = (struct ipo_fd *)kmalloc(32u * sizeof(struct ipo_fd));
        if (!new_fds) {
            fds_capacity = 0;
            return;
        }
        fds = new_fds;
        fds_capacity = 32;
    }
    for (uint32_t i = 0; i < fds_capacity; i++) {
        fds[i].used = (i < 3) ? 1 : 0;
        fds[i].inode = 0;
        fds[i].offset = 0;
        fds[i].flags = 0;
    }
}

void ipo_fs_reset_state(void) {
    ipo_fs_reset_superblock();
    ipo_fs_fds_reset();
}

void ipo_fs_init(void) {
    ipo_fs_reset_state();
    printf("[ipo_fs] Filesystem state initialized (%u dynamic fds, ptr=%p).\n", fds_capacity, fds);
}

bool ipo_fs_get_inode_stats(uint64_t *out_total, uint64_t *out_used, uint64_t *out_free, uint64_t *out_dynamic_chunks) {
    if (!fs_mounted) {
        if (out_total) *out_total = 0;
        if (out_used) *out_used = 0;
        if (out_free) *out_free = 0;
        if (out_dynamic_chunks) *out_dynamic_chunks = 0;
        return false;
    }

    uint64_t total = sb.inode_count;
    uint64_t primary_used = (sb.primary_inodes > 0) ? bitmap_count_set(sb.inode_bitmap_start, sb.primary_inodes) : 0;
    uint64_t dynamic_used = sb.dynamic_inodes_used;
    uint64_t used = primary_used + dynamic_used;
    uint64_t free_cnt = (total >= used) ? (total - used) : 0;

    if (out_total) *out_total = total;
    if (out_used) *out_used = used;
    if (out_free) *out_free = free_cnt;
    if (out_dynamic_chunks) *out_dynamic_chunks = sb.dynamic_chunks_count;
    return true;
}

bool ipo_fs_get_block_stats(uint64_t *out_total, uint64_t *out_used, uint64_t *out_free) {
    if (!fs_mounted) {
        if (out_total) *out_total = 0;
        if (out_used) *out_used = 0;
        if (out_free) *out_free = 0;
        return false;
    }

    uint64_t data_start = sb.data_blocks_start;
    uint64_t total = (sb.fs_size_blocks > data_start) ? (sb.fs_size_blocks - data_start) : 0;
    uint64_t used = (total > 0) ? bitmap_count_set(sb.block_bitmap_start, total) : 0;
    uint64_t free_cnt = (total >= used) ? (total - used) : 0;

    if (out_total) *out_total = total;
    if (out_used) *out_used = used;
    if (out_free) *out_free = free_cnt;
    return true;
}
