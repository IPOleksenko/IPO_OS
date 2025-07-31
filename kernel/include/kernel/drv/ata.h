#ifndef ATA_H
#define ATA_H

#include <stdint.h>
#include <stddef.h>

// ATA ports
#define ATA_PRIMARY_IO      0x1F0
#define ATA_PRIMARY_CONTROL 0x3F6
#define ATA_SECONDARY_IO    0x170
#define ATA_SECONDARY_CONTROL 0x376

// ATA registers
#define ATA_REG_DATA       0x00
#define ATA_REG_ERROR      0x01
#define ATA_REG_FEATURES   0x01
#define ATA_REG_SECCOUNT0  0x02
#define ATA_REG_LBA0       0x03
#define ATA_REG_LBA1       0x04
#define ATA_REG_LBA2       0x05
#define ATA_REG_HDDEVSEL   0x06
#define ATA_REG_COMMAND    0x07
#define ATA_REG_STATUS     0x07
#define ATA_REG_CONTROL    0x0C
#define ATA_REG_ALTSTATUS  0x0C

// ATA commands
#define ATA_CMD_READ_PIO          0x20
#define ATA_CMD_READ_PIO_EXT      0x24
#define ATA_CMD_READ_DMA          0xC8
#define ATA_CMD_READ_DMA_EXT      0x25
#define ATA_CMD_WRITE_PIO         0x30
#define ATA_CMD_WRITE_PIO_EXT     0x34
#define ATA_CMD_WRITE_DMA         0xCA
#define ATA_CMD_WRITE_DMA_EXT     0x35
#define ATA_CMD_CACHE_FLUSH       0xE7
#define ATA_CMD_CACHE_FLUSH_EXT   0xEA
#define ATA_CMD_PACKET            0xA0
#define ATA_CMD_IDENTIFY_PACKET   0xA1
#define ATA_CMD_IDENTIFY          0xEC

// ATA status flags
#define ATA_SR_BSY     0x80    // Busy
#define ATA_SR_DRDY    0x40    // Drive ready
#define ATA_SR_DF      0x20    // Drive write fault
#define ATA_SR_DSC     0x10    // Drive seek complete
#define ATA_SR_DRQ     0x08    // Data request ready
#define ATA_SR_CORR    0x04    // Corrected data
#define ATA_SR_IDX     0x02    // Index
#define ATA_SR_ERR     0x01    // Error

// ATA error flags
#define ATA_ER_BBK      0x80    // Bad block
#define ATA_ER_UNC      0x40    // Uncorrectable data
#define ATA_ER_MC       0x20    // Media changed
#define ATA_ER_IDNF     0x10    // ID mark not found
#define ATA_ER_MCR      0x08    // Media change request
#define ATA_ER_ABRT     0x04    // Command aborted
#define ATA_ER_TK0NF    0x02    // Track 0 not found
#define ATA_ER_AMNF     0x01    // No address mark

#define SECTOR_SIZE 512

typedef struct {
    uint16_t base;      // I/O Base
    uint16_t ctrl;      // Control Base
    uint16_t bmide;     // Bus Master IDE
    uint16_t nien;      // nIEN (No Interrupt)
} ata_device_t;

// Initialize ATA driver
void ata_init(void);

// Read sectors
int ata_read_sectors(uint32_t lba, uint8_t sector_count, void* buffer);

// Write sectors
int ata_write_sectors(uint32_t lba, uint8_t sector_count, const void* buffer);

// Wait for drive ready
int ata_wait_ready(void);

// Check drive presence
int ata_identify(void);

#endif