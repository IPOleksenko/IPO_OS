#ifndef _ATA_H
#define _ATA_H

#include <stdint.h>
#include <stdbool.h>

/* ATA device type */
typedef enum {
    ATA_DEVICE_NONE = 0,
    ATA_DEVICE_PATA,
    ATA_DEVICE_PATAPI
} ata_device_type_t;

/* ATA device info */
typedef struct {
    /* Presence */
    uint8_t present;

    /* Location */
    uint8_t channel;   /* 0 = primary, 1 = secondary */
    uint8_t drive;     /* 0 = master, 1 = slave */

    /* Device type */
    ata_device_type_t type;

    /* Geometry (from IDENTIFY) */
    uint16_t cylinders;
    uint16_t heads;
    uint16_t sectors;

    /* Capacity */
    uint64_t capacity_sectors; /* Total sectors (LBA28 / LBA48) */

    /* Identification strings */
    char *model;       /* Dynamically allocated, NUL-terminated */
    char *serial;      /* Dynamically allocated, NUL-terminated */

} ata_device_t;

/**
 * Initialize ATA driver
 */
void ata_init(void);

/**
 * Get number of detected devices
 */
uint32_t ata_get_device_count(void);

/**
 * Get device info
 */
ata_device_t* ata_get_device(uint32_t index);

/**
 * Print all detected devices
 */
void ata_print_devices(void);

/* Parameterized drive access (drive_idx: 0 = primary master, 1 = primary slave, ...) */
bool ata_read_drive_sectors_lba28(uint32_t drive_idx, uint32_t lba, uint8_t count, void *buf);
bool ata_write_drive_sectors_lba28(uint32_t drive_idx, uint32_t lba, uint8_t count, const void *buf);
bool ata_read_drive_sectors_lba48(uint32_t drive_idx, uint64_t lba, uint16_t count, void *buf);
bool ata_write_drive_sectors_lba48(uint32_t drive_idx, uint64_t lba, uint16_t count, const void *buf);
bool ata_read_drive_sectors(uint32_t drive_idx, uint64_t lba, uint16_t count, void *buf);
bool ata_write_drive_sectors(uint32_t drive_idx, uint64_t lba, uint16_t count, const void *buf);

/* Read/write sectors using PIO (LBA28). Buffer is 512 * count bytes.
 * Returns true on success. Uses default target drive (slave if count > 1, else master).
 */
bool ata_read_sectors_lba28(uint32_t lba, uint8_t count, void *buf);
bool ata_write_sectors_lba28(uint32_t lba, uint8_t count, const void *buf);

bool ata_read_sectors_lba48(uint64_t lba, uint16_t count, void *buf);
bool ata_write_sectors_lba48(uint64_t lba, uint16_t count, const void *buf);

bool ata_read_sectors(uint64_t lba, uint16_t count, void *buf);
bool ata_write_sectors(uint64_t lba, uint16_t count, const void *buf);

/* Multi-drive storage pool API */
uint64_t ata_get_pool_capacity(void);
bool ata_pool_read_sectors(uint64_t pool_lba, uint16_t count, void *buf);
bool ata_pool_write_sectors(uint64_t pool_lba, uint16_t count, const void *buf);

#endif /* _ATA_H */
