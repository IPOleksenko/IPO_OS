#include <kernel/drv/ata.h>
#include <kernel/sys/ioports.h>
#include <kernel/sys/pit.h>
#include <stdio.h>
#include <string.h>

static ata_device_t ata_primary_master = {ATA_PRIMARY_IO, ATA_PRIMARY_CONTROL, 0, 0};
static int ata_initialized = 0;

void ata_init(void) {
    printf("Initializing ATA driver...\n");
    
    // Reset controller
    outb(ata_primary_master.ctrl, 0x04);
    sleep(1);
    outb(ata_primary_master.ctrl, 0x00);
    sleep(1);
    
    // Check for drive presence
    if (ata_identify() == 0) {
        ata_initialized = 1;
        printf("ATA primary master drive detected\n");
    } else {
        printf("No ATA drive detected\n");
    }
}

int ata_wait_ready(void) {
    uint8_t status;
    int timeout = 1000000; // Timeout
    
    while (timeout--) {
        status = inb(ata_primary_master.base + ATA_REG_STATUS);
        if (!(status & ATA_SR_BSY) && (status & ATA_SR_DRDY)) {
            return 0; // Ready
        }
        if (status & ATA_SR_ERR) {
            return -1; // Error
        }
    }
    return -1; // Timeout
}

int ata_identify(void) {
    // Select drive (master)
    outb(ata_primary_master.base + ATA_REG_HDDEVSEL, 0xA0);
    sleep(1);
    
    // Send IDENTIFY command
    outb(ata_primary_master.base + ATA_REG_COMMAND, ATA_CMD_IDENTIFY);
    sleep(1);
    
    // Check status
    uint8_t status = inb(ata_primary_master.base + ATA_REG_STATUS);
    if (status == 0) {
        return -1; // Drive does not exist
    }
    
    // Wait for ready
    while (status & ATA_SR_BSY) {
        status = inb(ata_primary_master.base + ATA_REG_STATUS);
    }
    
    if (!(status & ATA_SR_DRQ)) {
        return -1; // Data not ready
    }
    
    // Read identification data
    uint16_t identify_data[256];
    for (int i = 0; i < 256; i++) {
        identify_data[i] = inw(ata_primary_master.base + ATA_REG_DATA);
    }
    
    return 0; // Success
}

int ata_read_sectors(uint32_t lba, uint8_t sector_count, void* buffer) {
    if (!ata_initialized || !buffer || sector_count == 0) {
        return -1;
    }
    
    // Wait for drive ready
    if (ata_wait_ready() != 0) {
        return -1;
    }
    
    // Select drive and set LBA mode
    outb(ata_primary_master.base + ATA_REG_HDDEVSEL, 0xE0 | ((lba >> 24) & 0x0F));
    
    // Set sector count
    outb(ata_primary_master.base + ATA_REG_SECCOUNT0, sector_count);
    
    // Set LBA address
    outb(ata_primary_master.base + ATA_REG_LBA0, (uint8_t)lba);
    outb(ata_primary_master.base + ATA_REG_LBA1, (uint8_t)(lba >> 8));
    outb(ata_primary_master.base + ATA_REG_LBA2, (uint8_t)(lba >> 16));
    
    // Send read command
    outb(ata_primary_master.base + ATA_REG_COMMAND, ATA_CMD_READ_PIO);
    
    uint16_t* buf = (uint16_t*)buffer;
    
    for (int sector = 0; sector < sector_count; sector++) {
        // Wait for data ready
        uint8_t status;
        int timeout = 1000000;
        
        while (timeout--) {
            status = inb(ata_primary_master.base + ATA_REG_STATUS);
            if (status & ATA_SR_DRQ) break;
            if (status & ATA_SR_ERR) return -1;
        }
        
        if (timeout <= 0) return -1;
        
        // Read sector
        for (int i = 0; i < 256; i++) {
            buf[sector * 256 + i] = inw(ata_primary_master.base + ATA_REG_DATA);
        }
    }
    
    return 0;
}

int ata_write_sectors(uint32_t lba, uint8_t sector_count, const void* buffer) {
    if (!ata_initialized || !buffer || sector_count == 0) {
        return -1;
    }
    
    // Wait for drive ready
    if (ata_wait_ready() != 0) {
        return -1;
    }
    
    // Select drive and set LBA mode
    outb(ata_primary_master.base + ATA_REG_HDDEVSEL, 0xE0 | ((lba >> 24) & 0x0F));
    
    // Set sector count
    outb(ata_primary_master.base + ATA_REG_SECCOUNT0, sector_count);
    
    // Set LBA address
    outb(ata_primary_master.base + ATA_REG_LBA0, (uint8_t)lba);
    outb(ata_primary_master.base + ATA_REG_LBA1, (uint8_t)(lba >> 8));
    outb(ata_primary_master.base + ATA_REG_LBA2, (uint8_t)(lba >> 16));
    
    // Send write command
    outb(ata_primary_master.base + ATA_REG_COMMAND, ATA_CMD_WRITE_PIO);
    
    const uint16_t* buf = (const uint16_t*)buffer;
    
    for (int sector = 0; sector < sector_count; sector++) {
        // Wait for write ready
        uint8_t status;
        int timeout = 1000000;
        
        while (timeout--) {
            status = inb(ata_primary_master.base + ATA_REG_STATUS);
            if (status & ATA_SR_DRQ) break;
            if (status & ATA_SR_ERR) return -1;
        }
        
        if (timeout <= 0) return -1;
        
        // Write sector
        for (int i = 0; i < 256; i++) {
            outw(ata_primary_master.base + ATA_REG_DATA, buf[sector * 256 + i]);
        }
        
        // Wait for write completion
        timeout = 1000000;
        while (timeout--) {
            status = inb(ata_primary_master.base + ATA_REG_STATUS);
            if (!(status & ATA_SR_BSY)) break;
        }
        
        if (timeout <= 0) return -1;
    }
    
    // Flush cache
    outb(ata_primary_master.base + ATA_REG_COMMAND, ATA_CMD_CACHE_FLUSH);
    
    // Wait for cache flush completion
    int timeout = 1000000;
    while (timeout--) {
        uint8_t status = inb(ata_primary_master.base + ATA_REG_STATUS);
        if (!(status & ATA_SR_BSY)) break;
    }
    
    return 0;
}