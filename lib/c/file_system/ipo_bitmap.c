#include <file_system/ipo_fs.h>
#include <string.h>
#include <stdio.h>
#include <ioport.h>

/* Reads a bit from the bitmap. bitmap_start is the block where the bitmap starts, bit_index is the bit index */
bool bitmap_get(uint32_t bitmap_start, uint32_t bit_index) {
    uint8_t buf[IPO_FS_BLOCK_SIZE];
    uint32_t byte_index = bit_index / 8;
    uint32_t block_offset = byte_index / IPO_FS_BLOCK_SIZE;
    uint32_t inblock = byte_index % IPO_FS_BLOCK_SIZE;
    int tries = 0;
    uint32_t lba = bitmap_start + block_offset;
    while (tries < 5) {
        if (block_read(lba, buf)) break;
        for (volatile int __t = 0; __t < 1000; __t++) inb(0x80);
        tries++;
    }
    if (tries == 5) return false;
    return (buf[inblock] >> (bit_index & 7)) & 1;
}

/* Set/clear bit */
bool bitmap_set(uint32_t bitmap_start, uint32_t bit_index, bool value) {
    uint8_t buf[IPO_FS_BLOCK_SIZE];
    uint32_t byte_index = bit_index / 8;
    uint32_t block_offset = byte_index / IPO_FS_BLOCK_SIZE;
    uint32_t inblock = byte_index % IPO_FS_BLOCK_SIZE;
    uint32_t lba = bitmap_start + block_offset;
    int tries = 0;
    while (tries < 5) {
        if (block_read(lba, buf)) break;
        for (volatile int __t = 0; __t < 1000; __t++) inb(0x80);
        tries++;
    }
    if (tries == 5) { printf("bitmap_set: block_read failed lba=%u after retries\n", lba); return false; }
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
    if (tries == 5) { printf("bitmap_set: block_write failed lba=%u after retries\n", lba); return false; }
    return true;
}
