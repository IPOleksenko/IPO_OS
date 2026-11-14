#include <file_system/ipo_fs.h>
#include <memory/kmalloc.h>
#include <string.h>
#include <ioport.h>

static bool bitmap_block_io(uint64_t lba, void *buf, bool write) {
    for (int tries = 0; tries < 5; tries++) {
        bool ok = write ? block_write(lba, buf) : block_read(lba, buf);
        if (ok) return true;
        for (volatile int t = 0; t < 1000; t++) inb(0x80);
    }
    return false;
}

bool bitmap_get(uint64_t bitmap_start, uint64_t bit_index) {
    uint8_t *buf = (uint8_t *)kmalloc(IPO_FS_BLOCK_SIZE);
    if (!buf) return false;
    uint64_t byte_index = bit_index / 8;
    uint64_t block_offset = byte_index / IPO_FS_BLOCK_SIZE;
    uint32_t inblock = (uint32_t)(byte_index % IPO_FS_BLOCK_SIZE);
    bool res = false;
    if (bitmap_block_io(bitmap_start + block_offset, buf, false)) {
        res = ((buf[inblock] >> (bit_index & 7)) & 1) != 0;
    }
    kfree(buf);
    return res;
}

bool bitmap_set(uint64_t bitmap_start, uint64_t bit_index, bool value) {
    uint8_t *buf = (uint8_t *)kmalloc(IPO_FS_BLOCK_SIZE);
    if (!buf) return false;
    uint64_t byte_index = bit_index / 8;
    uint64_t block_offset = byte_index / IPO_FS_BLOCK_SIZE;
    uint32_t inblock = (uint32_t)(byte_index % IPO_FS_BLOCK_SIZE);
    uint64_t lba = bitmap_start + block_offset;
    if (!bitmap_block_io(lba, buf, false)) {
        kfree(buf);
        return false;
    }
    if (value)
        buf[inblock] |= (uint8_t)(1u << (bit_index & 7));
    else
        buf[inblock] &= (uint8_t)~(1u << (bit_index & 7));
    bool ok = bitmap_block_io(lba, buf, true);
    kfree(buf);
    return ok;
}

static inline uint32_t popcount32(uint32_t x) {
    x = x - ((x >> 1) & 0x55555555u);
    x = (x & 0x33333333u) + ((x >> 2) & 0x33333333u);
    return (((x + (x >> 4)) & 0x0F0F0F0Fu) * 0x01010101u) >> 24;
}

uint32_t bitmap_count_set(uint64_t bitmap_start, uint64_t total_bits) {
    if (total_bits == 0) return 0;

    uint64_t total_blocks = (total_bits + (IPO_FS_BLOCK_SIZE * 8) - 1) / (IPO_FS_BLOCK_SIZE * 8);
    uint8_t *buf = (uint8_t *)kmalloc(IPO_FS_BLOCK_SIZE);
    if (!buf) return 0;
    uint32_t count = 0;

    for (uint64_t b = 0; b < total_blocks; b++) {
        if (!bitmap_block_io(bitmap_start + b, buf, false)) continue;

        uint64_t bits_in_block = IPO_FS_BLOCK_SIZE * 8;
        uint64_t bits_left = total_bits - (b * bits_in_block);
        uint64_t valid_bits = (bits_left < bits_in_block) ? bits_left : bits_in_block;

        uint32_t full_words = (uint32_t)(valid_bits / 32);
        uint32_t rem_bits = (uint32_t)(valid_bits % 32);
        uint32_t *words = (uint32_t *)buf;

        for (uint32_t w = 0; w < full_words; w++) {
            if (words[w] != 0) {
                count += popcount32(words[w]);
            }
        }

        if (rem_bits > 0) {
            uint32_t mask = (1u << rem_bits) - 1u;
            count += popcount32(words[full_words] & mask);
        }
    }

    kfree(buf);
    return count;
}
