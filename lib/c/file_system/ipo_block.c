#include <file_system/ipo_fs.h>
#include <driver/ata/ata.h>
#include <ioport.h>

/* Reads an FS block (index relative to FS start) into buffer */
bool block_read(uint64_t fs_block_index, void *buffer) {
    /* translate to LBA and read single sector-sized block from storage pool */
    return ata_pool_read_sectors(fs_start_lba + fs_block_index, 1, buffer);
}

/* Writes an FS block */
bool block_write(uint64_t fs_block_index, const void *buffer) {
    return ata_pool_write_sectors(fs_start_lba + fs_block_index, 1, buffer);
}
