#ifndef _ATA_H
#define _ATA_H

#include <stdint.h>
#include <stdbool.h>

typedef enum {
    ATA_DEVICE_NONE = 0,
    ATA_DEVICE_PATA,
    ATA_DEVICE_PATAPI
} ata_device_type_t;

typedef struct {
    uint8_t present;

    uint8_t channel;   /* 0 = primary, 1 = secondary */
    uint8_t drive;     /* 0 = master, 1 = slave */

    ata_device_type_t type;

    /* From IDENTIFY */
    uint16_t cylinders;
    uint16_t heads;
    uint16_t sectors;

    uint16_t io_base;
    uint16_t ctrl_base;
    uint8_t  irq;
    uint32_t sector_size;

    uint64_t capacity_sectors; /* LBA28 / LBA48 */

    /* Heap-allocated NUL-terminated strings */
    char *model;
    char *serial;
    char *firmware;

} ata_device_t;

void ata_init(void);

uint32_t ata_get_device_count(void);

ata_device_t* ata_get_device(uint32_t index);

void ata_print_devices(void);

/* drive_idx: 0 = primary master, 1 = primary slave, ... */
bool ata_read_drive_sectors_lba28(uint32_t drive_idx, uint32_t lba, uint8_t count, void *buf);
bool ata_write_drive_sectors_lba28(uint32_t drive_idx, uint32_t lba, uint8_t count, const void *buf);
bool ata_read_drive_sectors_lba48(uint32_t drive_idx, uint64_t lba, uint16_t count, void *buf);
bool ata_write_drive_sectors_lba48(uint32_t drive_idx, uint64_t lba, uint16_t count, const void *buf);
bool ata_read_drive_sectors(uint32_t drive_idx, uint64_t lba, uint16_t count, void *buf);
bool ata_write_drive_sectors(uint32_t drive_idx, uint64_t lba, uint16_t count, const void *buf);

/* PIO LBA28 (512 * count bytes); targets slave when count > 1, otherwise master */
bool ata_read_sectors_lba28(uint32_t lba, uint8_t count, void *buf);
bool ata_write_sectors_lba28(uint32_t lba, uint8_t count, const void *buf);

bool ata_read_sectors_lba48(uint64_t lba, uint16_t count, void *buf);
bool ata_write_sectors_lba48(uint64_t lba, uint16_t count, const void *buf);

bool ata_read_sectors(uint64_t lba, uint16_t count, void *buf);
bool ata_write_sectors(uint64_t lba, uint16_t count, const void *buf);

/* Multi-drive pool */
uint64_t ata_get_pool_capacity(void);
bool ata_pool_read_sectors(uint64_t pool_lba, uint16_t count, void *buf);
bool ata_pool_write_sectors(uint64_t pool_lba, uint16_t count, const void *buf);

#endif /* _ATA_H */
