#include <file_system/ipo_fs.h>
#include <storage/storage_pool.h>
#include <driver/ata/ata.h>
#include <ioport.h>

/* Reads an FS block (index relative to FS start) into buffer */
bool block_read(uint64_t fs_block_index, void *buffer) {
    if (!buffer) return false;
    if (fs_mounted && fs_block_index >= sb.fs_size_blocks) {
        return false;
    }
    return storage_pool_read(fs_start_lba + fs_block_index, 1, buffer);
}

bool block_read_multi(uint64_t fs_block_index, uint16_t count, void *buffer) {
    if (count == 0) return true;
    if (!buffer) return false;
    if (fs_mounted) {
        if (fs_block_index >= sb.fs_size_blocks ||
            (uint64_t)count > sb.fs_size_blocks - fs_block_index) {
            return false;
        }
    }
    return storage_pool_read(fs_start_lba + fs_block_index, count, buffer);
}

/* Writes an FS block */
bool block_write(uint64_t fs_block_index, const void *buffer) {
    if (!buffer) return false;
    if (fs_mounted && fs_block_index >= sb.fs_size_blocks) {
        return false;
    }
    return storage_pool_write(fs_start_lba + fs_block_index, 1, buffer);
}

bool block_write_multi(uint64_t fs_block_index, uint16_t count, const void *buffer) {
    if (count == 0) return true;
    if (!buffer) return false;
    if (fs_mounted) {
        if (fs_block_index >= sb.fs_size_blocks ||
            (uint64_t)count > sb.fs_size_blocks - fs_block_index) {
            return false;
        }
    }
    return storage_pool_write(fs_start_lba + fs_block_index, count, buffer);
}
