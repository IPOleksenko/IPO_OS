#include <driver/ata/ata.h>
#include <ioport.h>
#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include <stdint.h>

#define ATA_PRIMARY_BASE   0x1F0
#define ATA_PRIMARY_CTRL   0x3F6

#define ATA_REG_DATA       0x00
#define ATA_REG_ERROR      0x01
#define ATA_REG_SECCOUNT0  0x02
#define ATA_REG_LBA0       0x03
#define ATA_REG_LBA1       0x04
#define ATA_REG_LBA2       0x05
#define ATA_REG_HDDEVSEL   0x06
#define ATA_REG_STATUS     0x07
#define ATA_REG_COMMAND    0x07

#define ATA_CMD_IDENTIFY   0xEC

#define ATA_SR_BSY  0x80
#define ATA_SR_DRQ  0x08
#define ATA_SR_ERR  0x01

#define ATA_MAX_DEVICES 4

static ata_device_t ata_devices[ATA_MAX_DEVICES];
static uint8_t ata_device_count = 0;

static uint16_t identify_buf[256];

/* -------------------------------------------------- */

static void ata_io_wait(void) {
    inb(ATA_PRIMARY_CTRL);
    inb(ATA_PRIMARY_CTRL);
    inb(ATA_PRIMARY_CTRL);
    inb(ATA_PRIMARY_CTRL);
}

static bool ata_wait_bsy_clear(uint16_t base) {
    for (int i = 0; i < 100000; i++) {
        if (!(inb(base + ATA_REG_STATUS) & ATA_SR_BSY))
            return true;
    }
    return false;
}

static bool ata_wait_drq(uint16_t base) {
    for (int i = 0; i < 100000; i++) {
        uint8_t st = inb(base + ATA_REG_STATUS);
        if (st & ATA_SR_ERR) { printf("ata_wait_drq: ERR status=%u\n", (unsigned)st); return false; }
        if (st & ATA_SR_DRQ) return true;
        ata_io_wait();
    }
    uint8_t st = inb(base + ATA_REG_STATUS);
    printf("ata_wait_drq: timeout status=%u\n", (unsigned)st);
    return false;
}

static void ata_read_string(char *dst, int offset, int words) {
    int p = 0;
    for (int i = 0; i < words; i++) {
        uint16_t w = identify_buf[offset + i];
        dst[p++] = (w >> 8) & 0xFF;
        dst[p++] = w & 0xFF;
    }
    dst[p] = 0;

    for (int i = p - 1; i >= 0 && dst[i] == ' '; i--)
        dst[i] = 0;
}

/* -------------------------------------------------- */

static bool ata_identify(uint8_t drive) {
    uint16_t base = ATA_PRIMARY_BASE;
    uint8_t devsel = drive ? 0xB0 : 0xA0;

    outb(base + ATA_REG_HDDEVSEL, devsel);
    ata_io_wait();

    outb(base + ATA_REG_SECCOUNT0, 0);
    outb(base + ATA_REG_LBA0, 0);
    outb(base + ATA_REG_LBA1, 0);
    outb(base + ATA_REG_LBA2, 0);

    outb(base + ATA_REG_COMMAND, ATA_CMD_IDENTIFY);
    ata_io_wait();

    uint8_t status = inb(base + ATA_REG_STATUS);
    if (status == 0)
        return false;

    if (!ata_wait_bsy_clear(base))
        return false;

    if (!ata_wait_drq(base))
        return false;

    for (int i = 0; i < 256; i++)
        identify_buf[i] = inw(base + ATA_REG_DATA);

    ata_device_t *dev = &ata_devices[ata_device_count];
    memset(dev, 0, sizeof(*dev));

    dev->present = 1;
    dev->channel = 0;
    dev->drive = drive;
    dev->type = ATA_DEVICE_PATA;

    ata_read_string(dev->serial, 10, 10);
    ata_read_string(dev->model, 27, 20);

    if (identify_buf[83] & (1 << 10)) {
        dev->capacity_sectors =
            ((uint64_t)identify_buf[100]) |
            ((uint64_t)identify_buf[101] << 16) |
            ((uint64_t)identify_buf[102] << 32) |
            ((uint64_t)identify_buf[103] << 48);
    } else {
        dev->capacity_sectors =
            ((uint32_t)identify_buf[60]) |
            ((uint32_t)identify_buf[61] << 16);
    }

    ata_device_count++;
    return true;
}

/* -------------------------------------------------- */

void ata_init(void) {
    ata_device_count = 0;

    printf("ATA: scanning primary channel...\n");

    ata_identify(0); /* primary master */
    ata_identify(1); /* primary slave */

    printf("ATA: found %d device(s)\n", ata_device_count);
}

uint8_t ata_get_device_count(void) {
    return ata_device_count;
}

ata_device_t *ata_get_device(uint8_t index) {
    if (index >= ata_device_count)
        return NULL;
    return &ata_devices[index];
}

void ata_print_devices(void) {
    printf("=== ATA DEVICES ===\n");

    for (uint8_t i = 0; i < ata_device_count; i++) {
        ata_device_t *d = &ata_devices[i];
        printf("Device %d:\n", i);
        printf("  Model: %s\n", d->model);
        printf("  Serial: %s\n", d->serial);
        printf("  Sectors: %llu\n", d->capacity_sectors);
        printf("  Size: %llu MB\n", d->capacity_sectors / 2048);
        printf("\n");
    }
}

bool ata_read_sectors_lba28(uint32_t lba, uint8_t count, void *buf) {
    if (count == 0 || buf == NULL) return false;
    if (ata_device_count == 0) return false;

    uint16_t base = ATA_PRIMARY_BASE;

    /* select device (master/slave based on ata_device_count), set LBA high bits */
    uint8_t drive_bit = (ata_device_count > 1) ? 0x10 : 0x00;
    outb(base + ATA_REG_HDDEVSEL, 0xE0 | drive_bit | ((lba >> 24) & 0x0F));
    ata_io_wait();

    outb(base + ATA_REG_SECCOUNT0, count);
    outb(base + ATA_REG_LBA0, (uint8_t)(lba & 0xFF));
    outb(base + ATA_REG_LBA1, (uint8_t)((lba >> 8) & 0xFF));
    outb(base + ATA_REG_LBA2, (uint8_t)((lba >> 16) & 0xFF));

    if (!ata_wait_bsy_clear(base)) { printf("ata_read_sectors_lba28: device bsy not cleared\n"); return false; }

    /* READ PIO */
    outb(base + ATA_REG_COMMAND, 0x20);

    uint16_t *wptr = (uint16_t *)buf;
    for (int s = 0; s < count; s++) {
        if (!ata_wait_drq(base)) return false;
        for (int i = 0; i < 256; i++) {
            wptr[i] = inw(base + ATA_REG_DATA);
        }
        wptr += 256;
    }

    return true;
}

bool ata_write_sectors_lba28(uint32_t lba, uint8_t count, const void *buf) {
    if (count == 0 || buf == NULL) return false;
    if (ata_device_count == 0) return false;

    uint16_t base = ATA_PRIMARY_BASE;

    /* select device (master/slave based on ata_device_count), set LBA high bits */
    uint8_t drive_bit = (ata_device_count > 1) ? 0x10 : 0x00;
    outb(base + ATA_REG_HDDEVSEL, 0xE0 | drive_bit | ((lba >> 24) & 0x0F));
    ata_io_wait();

    outb(base + ATA_REG_SECCOUNT0, count);
    outb(base + ATA_REG_LBA0, (uint8_t)(lba & 0xFF));
    outb(base + ATA_REG_LBA1, (uint8_t)((lba >> 8) & 0xFF));
    outb(base + ATA_REG_LBA2, (uint8_t)((lba >> 16) & 0xFF));

    /* Ensure device is ready */
    if (!ata_wait_bsy_clear(base)) { printf("ata_write_sectors_lba28: device bsy not cleared\n"); return false; }

    /* WRITE PIO */
    outb(base + ATA_REG_COMMAND, 0x30);

    const uint16_t *wptr = (const uint16_t *)buf;
    for (int s = 0; s < count; s++) {
        if (!ata_wait_drq(base)) {
            /* small retry attempts */
            int retry = 0;
            while (retry < 5 && !ata_wait_drq(base)) { ata_io_wait(); retry++; }
            if (retry == 5) { printf("ata_write_sectors_lba28: ata_wait_drq failed for sector %d after retries\n", s); return false; }
        }
        for (int i = 0; i < 256; i++) {
            outw(base + ATA_REG_DATA, wptr[i]);
        }
        wptr += 256;
        ata_io_wait();
    }

    /* flush cache */
    outb(base + ATA_REG_COMMAND, 0xE7);
    ata_io_wait();
    if (!ata_wait_bsy_clear(base)) { printf("ata_write_sectors_lba28: flush bsy not cleared\n"); return false; }
    uint8_t st = inb(base + ATA_REG_STATUS);
    if (st & ATA_SR_ERR) { printf("ata_write_sectors_lba28: status error after flush=%u\n", (unsigned)st); return false; }

    return true;
}
