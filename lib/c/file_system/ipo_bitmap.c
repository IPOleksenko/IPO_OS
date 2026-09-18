#include <file_system/ipo_fs.h>
#include <string.h>
#include <stdio.h>
#include <ioport.h>

/* Reads a bit from the bitmap. bitmap_start is the block where the bitmap starts, bit_index is the bit index */
bool bitmap_get(uint64_t bitmap_start, uint64_t bit_index) {
    uint8_t buf[IPO_FS_BLOCK_SIZE];
    uint64_t byte_index = bit_index / 8;
    uint64_t block_offset = byte_index / IPO_FS_BLOCK_SIZE;
    uint32_t inblock = (uint32_t)(byte_index % IPO_FS_BLOCK_SIZE);
    int tries = 0;
    uint64_t lba = bitmap_start + block_offset;
    while (tries < 5) {
        if (block_read(lba, buf)) break;
        for (volatile int __t = 0; __t < 1000; __t++) inb(0x80);
        tries++;
    }
    if (tries == 5) return false;
    return (buf[inblock] >> (bit_index & 7)) & 1;
}

/* Set/clear bit */
bool bitmap_set(uint64_t bitmap_start, uint64_t bit_index, bool value) {
    uint8_t buf[IPO_FS_BLOCK_SIZE];
    uint64_t byte_index = bit_index / 8;
    uint64_t block_offset = byte_index / IPO_FS_BLOCK_SIZE;
    uint32_t inblock = (uint32_t)(byte_index % IPO_FS_BLOCK_SIZE);
    uint64_t lba = bitmap_start + block_offset;
    int tries = 0;
    while (tries < 5) {
        if (block_read(lba, buf)) break;
        for (volatile int __t = 0; __t < 1000; __t++) inb(0x80);
        tries++;
    }
    if (tries == 5) return false;
    if (value)
        buf[inblock] |= (1 << (bit_index & 7));
    else
        buf[inblock] &= ~(1 << (bit_index & 7));
    tries = 0;
    while (tries < 5) {
        if (block_write(lba, buf)) break;
        for (volatile int __t = 0; __t < 1000; __t++) inb(0x80);
        tries++;
    }
    if (tries == 5) return false;
    return true;
}

static inline uint32_t popcount32(uint32_t x) {
    x = x - ((x >> 1) & 0x55555555u);
    x = (x & 0x33333333u) + ((x >> 2) & 0x33333333u);
    return (((x + (x >> 4)) & 0x0F0F0F0Fu) * 0x01010101u) >> 24;
}

/* Fast block-wise bitmap bit counter */
uint32_t bitmap_count_set(uint64_t bitmap_start, uint64_t total_bits) {
    if (total_bits == 0) return 0;

    uint64_t total_blocks = (total_bits + (IPO_FS_BLOCK_SIZE * 8) - 1) / (IPO_FS_BLOCK_SIZE * 8);
    uint8_t buf[IPO_FS_BLOCK_SIZE];
    uint32_t count = 0;

    for (uint64_t b = 0; b < total_blocks; b++) {
        uint64_t lba = bitmap_start + b;
        int tries = 0;
        bool ok = false;
        while (tries < 5) {
            if (block_read(lba, buf)) {
                ok = true;
                break;
            }
            for (volatile int __t = 0; __t < 1000; __t++) inb(0x80);
            tries++;
        }
        if (!ok) continue;

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

    return count;
}
