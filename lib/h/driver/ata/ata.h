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
    char model[41];    /* 40 chars + NULL */
    char serial[21];   /* 20 chars + NULL */

} ata_device_t;

/**
 * Initialize ATA driver
 */
void ata_init(void);

/**
 * Get number of detected devices
 */
uint8_t ata_get_device_count(void);

/**
 * Get device info
 */
ata_device_t* ata_get_device(uint8_t index);

/**
 * Print all detected devices
 */
void ata_print_devices(void);

#endif /* _ATA_H */
