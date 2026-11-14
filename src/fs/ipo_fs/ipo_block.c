#include <file_system/ipo_fs.h>
#include <storage/storage_pool.h>
#include <memory/kmalloc.h>
#include <string.h>

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

int64_t allocate_contiguous_blocks(uint32_t count) {
    if (count == 0 || sb.fs_size_blocks <= sb.data_blocks_start) {
        ipo_fs_set_last_error(IPO_FS_ERR_NO_SPACE);
        return -1;
    }
    uint64_t data_blocks_total = sb.fs_size_blocks - sb.data_blocks_start;
    if (count > data_blocks_total) {
        ipo_fs_set_last_error(IPO_FS_ERR_NO_SPACE);
        return -1;
    }

    uint64_t bitmap_blocks = (data_blocks_total + (IPO_FS_BLOCK_SIZE * 8) - 1) / (IPO_FS_BLOCK_SIZE * 8);
    uint8_t *buf = (uint8_t *)kmalloc(IPO_FS_BLOCK_SIZE);
    if (!buf) {
        ipo_fs_set_last_error(IPO_FS_ERR_IO);
        return -1;
    }

    uint64_t current_streak = 0;
    uint64_t streak_start_bit = 0;

    for (uint64_t b = 0; b < bitmap_blocks; b++) {
        uint64_t lba = sb.block_bitmap_start + b;
        if (!block_read(lba, buf)) {
            current_streak = 0;
            continue;
        }

        uint32_t *words = (uint32_t *)buf;
        int num_words = IPO_FS_BLOCK_SIZE / sizeof(uint32_t);

        for (int w = 0; w < num_words; w++) {
            uint32_t word = words[w];
            if (word == 0xFFFFFFFFu) {
                current_streak = 0;
                continue;
            }
            for (int bit = 0; bit < 32; bit++) {
                uint64_t bit_idx = (b * IPO_FS_BLOCK_SIZE * 8) + (w * 32) + bit;
                if (bit_idx >= data_blocks_total) {
                    kfree(buf);
                    ipo_fs_set_last_error(IPO_FS_ERR_NO_SPACE);
                    return -1;
                }
                if ((word & (1u << bit)) == 0) {
                    if (current_streak == 0) {
                        streak_start_bit = bit_idx;
                    }
                    current_streak++;
                    if (current_streak == count) {
                        for (uint64_t i = 0; i < count; i++) {
                            if (!bitmap_set(sb.block_bitmap_start, streak_start_bit + i, true)) {
                                for (uint64_t r = 0; r < i; r++) {
                                    bitmap_set(sb.block_bitmap_start, streak_start_bit + r, false);
                                }
                                kfree(buf);
                                ipo_fs_set_last_error(IPO_FS_ERR_IO);
                                return -1;
                            }
                        }
                        uint64_t phys_start = sb.data_blocks_start + streak_start_bit;
                        memset(buf, 0, IPO_FS_BLOCK_SIZE);
                        for (uint64_t i = 0; i < count; i++) {
                            if (!block_write(phys_start + i, buf)) {
                                for (uint64_t r = 0; r < count; r++) {
                                    bitmap_set(sb.block_bitmap_start, streak_start_bit + r, false);
                                }
                                kfree(buf);
                                ipo_fs_set_last_error(IPO_FS_ERR_IO);
                                return -1;
                            }
                        }
                        kfree(buf);
                        return (int64_t)phys_start;
                    }
                } else {
                    current_streak = 0;
                }
            }
        }
    }
    kfree(buf);
    ipo_fs_set_last_error(IPO_FS_ERR_NO_SPACE);
    return -1;
}

int64_t allocate_block(void) {
    return allocate_contiguous_blocks(1);
}

bool free_block(uint64_t phys_block) {
    if (phys_block < sb.data_blocks_start || phys_block >= sb.fs_size_blocks) {
        return false;
    }
    uint64_t i = phys_block - sb.data_blocks_start;
    return bitmap_set(sb.block_bitmap_start, i, false);
}
