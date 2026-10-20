#include <driver/ata/ata.h>
#include <driver/net/pci.h>
#include <driver/storage/block_dev.h>
#include <storage/storage_pool.h>
#include <memory/kmalloc.h>
#include <system/timer.h>
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

/* Dynamic self-expanding array for ATA devices (unlimited capacity) */
static ata_device_t *ata_devices = NULL;
static uint32_t ata_device_count = 0;
static uint32_t ata_device_capacity = 0;

/* Dynamic self-expanding buffer for ATA IDENTIFY words */
static uint16_t *identify_buf = NULL;
static size_t identify_buf_capacity = 0;
static size_t identify_buf_size = 0;

/* Dynamic self-expanding array for ATA controller channels */
typedef struct {
    uint16_t base;
    uint16_t ctrl;
    uint8_t  irq;
} ata_channel_info_t;

static ata_channel_info_t *ata_channels = NULL;
static uint32_t ata_channel_count = 0;
static uint32_t ata_channel_capacity = 0;

static bool identify_buf_ensure_capacity(size_t needed) {
    if (needed <= identify_buf_capacity && identify_buf != NULL) {
        return true;
    }
    size_t new_cap = (identify_buf_capacity == 0) ? 64 : identify_buf_capacity;
    while (new_cap < needed) {
        new_cap = (new_cap * 3) / 2 + 32;
    }
    uint16_t *new_buf = (uint16_t *)krealloc(identify_buf, new_cap * sizeof(uint16_t));
    if (!new_buf) {
        serial_printf("ATA: failed to expand identify_buf (cap=%u -> %u)\n",
                      (unsigned)identify_buf_capacity, (unsigned)new_cap);
        return false;
    }
    identify_buf = new_buf;
    identify_buf_capacity = new_cap;
    return true;
}

static bool identify_buf_append(uint16_t val) {
    if (!identify_buf_ensure_capacity(identify_buf_size + 1)) {
        return false;
    }
    identify_buf[identify_buf_size++] = val;
    return true;
}

static inline uint16_t identify_buf_get(size_t idx) {
    if (identify_buf && idx < identify_buf_size) {
        return identify_buf[idx];
    }
    return 0;
}

static bool ata_register_channel(uint16_t base, uint16_t ctrl, uint8_t irq) {
    for (uint32_t i = 0; i < ata_channel_count; i++) {
        if (ata_channels[i].base == base && ata_channels[i].ctrl == ctrl) {
            return true;
        }
    }
    if (ata_channel_count >= ata_channel_capacity) {
        uint32_t new_cap = (ata_channel_capacity == 0) ? 4 : (ata_channel_capacity * 2);
        ata_channel_info_t *new_arr = (ata_channel_info_t *)krealloc(ata_channels, new_cap * sizeof(ata_channel_info_t));
        if (!new_arr) return false;
        ata_channels = new_arr;
        ata_channel_capacity = new_cap;
    }
    ata_channels[ata_channel_count].base = base;
    ata_channels[ata_channel_count].ctrl = ctrl;
    ata_channels[ata_channel_count].irq  = irq;
    ata_channel_count++;
    return true;
}

static inline uint16_t ata_channel_base(uint8_t channel) {
    if (channel < ata_channel_count && ata_channels) {
        return ata_channels[channel].base;
    }
    return (channel == 0) ? ATA_PRIMARY_BASE : 0x170;
}

static inline uint16_t ata_channel_ctrl(uint8_t channel) {
    if (channel < ata_channel_count && ata_channels) {
        return ata_channels[channel].ctrl;
    }
    return (channel == 0) ? ATA_PRIMARY_CTRL : 0x376;
}

static inline uint8_t ata_channel_irq(uint8_t channel) {
    if (channel < ata_channel_count && ata_channels) {
        return ata_channels[channel].irq;
    }
    return (channel == 0) ? 14 : 15;
}

static ata_device_t *ata_device_allocate(void) {
    if (ata_device_count >= ata_device_capacity) {
        uint32_t new_cap = (ata_device_capacity == 0) ? 4 : (ata_device_capacity * 2);
        ata_device_t *new_devs = (ata_device_t *)krealloc(ata_devices, new_cap * sizeof(ata_device_t));
        if (!new_devs) {
            serial_printf("ATA: failed to expand device array (cap=%u -> %u)\n",
                          (unsigned)ata_device_capacity, (unsigned)new_cap);
            return NULL;
        }
        ata_devices = new_devs;
        ata_device_capacity = new_cap;
    }
    ata_device_t *dev = &ata_devices[ata_device_count++];
    memset(dev, 0, sizeof(*dev));
    return dev;
}

/* -------------------------------------------------- */

static inline void ata_io_wait_port(uint16_t ctrl) {
    inb(ctrl);
    inb(ctrl);
    inb(ctrl);
    inb(ctrl);
}

static inline void ata_io_wait(void) {
    ata_io_wait_port(ATA_PRIMARY_CTRL);
}

#define ATA_TIMEOUT_MS  3000

static bool ata_wait_bsy_clear(uint16_t base) {
    uint32_t start = timer_millis();
    while (timer_elapsed_ms(start) < ATA_TIMEOUT_MS) {
        uint8_t st = inb(base + ATA_REG_STATUS);
        if (!(st & ATA_SR_BSY)) {
            return true;
        }
        ata_io_wait();
    }

    /* Timer elapsed: if drive is still busy (BSY=1) writing/reading large files,
     * do NOT fail or abort! As long as there is no hardware error (ERR=0),
     * keep waiting for the write or cache flush to finish. */
    uint8_t st = inb(base + ATA_REG_STATUS);
    if ((st & ATA_SR_BSY) && !(st & ATA_SR_ERR)) {
        while ((st = inb(base + ATA_REG_STATUS)) & ATA_SR_BSY) {
            if (st & ATA_SR_ERR) {
                serial_printf("ata_wait_bsy_clear: ERR while busy: 0x%02X\n", (unsigned)st);
                return false;
            }
            ata_io_wait();
        }
        return true;
    }

    if (!(st & ATA_SR_BSY)) return true;

    serial_printf("ata_wait_bsy_clear: timeout status=0x%02X\n", (unsigned)st);
    return false;
}

static bool ata_wait_drq(uint16_t base) {
    uint32_t start = timer_millis();
    while (timer_elapsed_ms(start) < ATA_TIMEOUT_MS) {
        uint8_t st = inb(base + ATA_REG_STATUS);
        if (st & ATA_SR_BSY) {
            ata_io_wait();
            continue;
        }
        /* Hard drive error */
        if (st & ATA_SR_ERR) {
            serial_printf("ata_wait_drq: ERR status=0x%02X\n", (unsigned)st);
            return false;
        }
        /* Data Request ready */
        if (st & ATA_SR_DRQ) {
            return true;
        }
        ata_io_wait();
    }

    /* Timer elapsed: if drive is still busy (BSY=1) processing data (e.g. streaming
     * large files), do NOT treat it as an error! Wait for BSY to clear and DRQ to be asserted. */
    uint8_t st = inb(base + ATA_REG_STATUS);
    if ((st & ATA_SR_BSY) && !(st & ATA_SR_ERR)) {
        while ((st = inb(base + ATA_REG_STATUS)) & ATA_SR_BSY) {
            if (st & ATA_SR_ERR) {
                serial_printf("ata_wait_drq: ERR while busy: 0x%02X\n", (unsigned)st);
                return false;
            }
            ata_io_wait();
        }
        if (st & ATA_SR_DRQ) return true;
    }

    if (st & ATA_SR_DRQ) return true;

    serial_printf("ata_wait_drq: timeout status=0x%02X\n", (unsigned)st);
    return false;
}

static char *ata_read_string(int offset, int words) {
    char *dst = (char *)kmalloc((size_t)words * 2 + 1);
    if (!dst) return NULL;
    int p = 0;
    for (int i = 0; i < words; i++) {
        uint16_t w = identify_buf_get((size_t)(offset + i));
        dst[p++] = (w >> 8) & 0xFF;
        dst[p++] = w & 0xFF;
    }
    dst[p] = 0;

    for (int i = p - 1; i >= 0 && dst[i] == ' '; i--)
        dst[i] = 0;
    return dst;
}

/* -------------------------------------------------- */

static bool ata_identify(uint8_t channel, uint8_t drive) {
    uint16_t base = ata_channel_base(channel);
    uint16_t ctrl = ata_channel_ctrl(channel);
    uint8_t devsel = drive ? 0xB0 : 0xA0;

    outb(base + ATA_REG_HDDEVSEL, devsel);
    ata_io_wait_port(ctrl);

    outb(base + ATA_REG_SECCOUNT0, 0);
    outb(base + ATA_REG_LBA0, 0);
    outb(base + ATA_REG_LBA1, 0);
    outb(base + ATA_REG_LBA2, 0);

    outb(base + ATA_REG_COMMAND, ATA_CMD_IDENTIFY);
    ata_io_wait_port(ctrl);

    uint8_t status = inb(base + ATA_REG_STATUS);
    if (status == 0 || status == 0xFF)
        return false;

    if (!ata_wait_bsy_clear(base))
        return false;

    if (!ata_wait_drq(base))
        return false;

    identify_buf_size = 0;
    for (int i = 0; i < 256; i++) {
        uint16_t val = inw(base + ATA_REG_DATA);
        if (!identify_buf_append(val))
            return false;
    }

    ata_device_t *dev = ata_device_allocate();
    if (!dev)
        return false;

    dev->present = 1;
    dev->channel = channel;
    dev->drive = drive;
    dev->type = ATA_DEVICE_PATA;
    dev->io_base = base;
    dev->ctrl_base = ctrl;
    dev->irq = ata_channel_irq(channel);

    dev->cylinders = identify_buf_get(1);
    dev->heads     = identify_buf_get(3);
    dev->sectors   = identify_buf_get(6);

    dev->serial   = ata_read_string(10, 10);
    dev->firmware = ata_read_string(23, 4);
    dev->model    = ata_read_string(27, 20);

    uint16_t w106 = identify_buf_get(106);
    if ((w106 & 0xC000u) == 0x4000u && (w106 & (1u << 12))) {
        uint32_t words_per_sec = (uint32_t)identify_buf_get(117) | ((uint32_t)identify_buf_get(118) << 16);
        dev->sector_size = words_per_sec ? (words_per_sec * 2u) : (uint32_t)(identify_buf_size * sizeof(uint16_t));
    } else {
        dev->sector_size = (uint32_t)(identify_buf_size * sizeof(uint16_t));
    }

    if (identify_buf_get(83) & (1 << 10)) {
        dev->capacity_sectors =
            ((uint64_t)identify_buf_get(100)) |
            ((uint64_t)identify_buf_get(101) << 16) |
            ((uint64_t)identify_buf_get(102) << 32) |
            ((uint64_t)identify_buf_get(103) << 48);
    } else {
        dev->capacity_sectors =
            ((uint32_t)identify_buf_get(60)) |
            ((uint32_t)identify_buf_get(61) << 16);
    }

    return true;
}

/* -------------------------------------------------- */

static bool ata_bdev_read(block_device_t *bdev, uint64_t lba, uint16_t count, void *buf);
static bool ata_bdev_write(block_device_t *bdev, uint64_t lba, uint16_t count, const void *buf);
static bool ata_bdev_flush(block_device_t *bdev);

static const block_device_ops_t s_ata_bdev_ops = {
    .read_sectors = ata_bdev_read,
    .write_sectors = ata_bdev_write,
    .flush = ata_bdev_flush,
    .ioctl = NULL
};

static void ata_register_block_devices(void) {
    for (uint32_t i = 0; i < ata_device_count; i++) {
        ata_device_t *d = &ata_devices[i];
        if (!d->present) continue;

        const char *hw_name = (d->serial && d->serial[0]) ? d->serial :
                              ((d->model && d->model[0]) ? d->model : NULL);
        if (!hw_name) continue;

        block_device_t bdev;
        memset(&bdev, 0, sizeof(bdev));
        bdev.name = (char *)hw_name;
        bdev.total_sectors = d->capacity_sectors;
        bdev.sector_size = d->sector_size;
        bdev.flags = (i == 0) ? BLOCK_DEV_BOOTABLE : 0;
        bdev.ops = &s_ata_bdev_ops;
        bdev.priv_data = (void *)(uintptr_t)i;
        block_dev_register(&bdev);
    }
}

void ata_init(void) {
    ata_device_count = 0;

    if (ata_channel_count == 0) {
        if (pci_get_count() == 0) {
            pci_scan_all(NULL, NULL);
        }
        uint32_t pcount = pci_get_count();
        for (uint32_t i = 0; i < pcount; i++) {
            const pci_device_t *p = pci_get_device(i);
            if (!p || p->class_code != 0x01 || p->subclass != 0x01) continue;
            uint16_t b0 = (p->bar_count > 0 && (p->bar[0] & ~3u) != 0) ? (uint16_t)(p->bar[0] & ~3u) : ATA_PRIMARY_BASE;
            uint16_t c0 = (p->bar_count > 1 && (p->bar[1] & ~3u) != 0) ? (uint16_t)((p->bar[1] & ~3u) + 2) : ATA_PRIMARY_CTRL;
            uint8_t irq0 = (p->prog_if & 0x01) ? p->irq : 14;
            ata_register_channel(b0, c0, irq0);

            uint16_t b1 = (p->bar_count > 2 && (p->bar[2] & ~3u) != 0) ? (uint16_t)(p->bar[2] & ~3u) : 0x170;
            uint16_t c1 = (p->bar_count > 3 && (p->bar[3] & ~3u) != 0) ? (uint16_t)((p->bar[3] & ~3u) + 2) : 0x376;
            uint8_t irq1 = (p->prog_if & 0x04) ? p->irq : 15;
            ata_register_channel(b1, c1, irq1);
        }
        if (ata_channel_count == 0) {
            ata_register_channel(ATA_PRIMARY_BASE, ATA_PRIMARY_CTRL, 14);
            ata_register_channel(0x170, 0x376, 15);
        }
    }

    for (uint32_t c = 0; c < ata_channel_count; c++) {
        printf("ATA: scanning channel %u (base=0x%x, ctrl=0x%x, irq=%u)...\n",
                      (unsigned)c, (unsigned)ata_channels[c].base, (unsigned)ata_channels[c].ctrl, (unsigned)ata_channels[c].irq);
        ata_identify((uint8_t)c, 0); /* master */
        ata_identify((uint8_t)c, 1); /* slave */
    }

    printf("ATA: found %u device(s)\n", (unsigned)ata_device_count);
    ata_print_devices();

    ata_register_block_devices();
}

uint32_t ata_get_device_count(void) {
    return ata_device_count;
}

ata_device_t *ata_get_device(uint32_t index) {
    if (index >= ata_device_count || !ata_devices)
        return NULL;
    return &ata_devices[index];
}

void ata_print_devices(void) {
    serial_printf("=== ATA DEVICES ===\n");

    for (uint32_t i = 0; i < ata_device_count; i++) {
        ata_device_t *d = &ata_devices[i];
        serial_printf("Device %u:\n", (unsigned)i);
        serial_printf("  Model: %s\n", d->model ? d->model : "");
        serial_printf("  Serial: %s\n", d->serial ? d->serial : "");
        serial_printf("  Sectors: %llu\n", d->capacity_sectors);
        serial_printf("  Size: %llu MB\n", d->capacity_sectors / 2048);
    }
}

bool ata_read_drive_sectors_lba28(uint32_t drive_idx, uint32_t lba, uint8_t count, void *buf) {
    if (count == 0 || buf == NULL) return false;
    if (drive_idx >= ata_device_count || !ata_devices || !ata_devices[drive_idx].present) return false;

    uint16_t base = ata_channel_base(ata_devices[drive_idx].channel);
    uint16_t ctrl = ata_channel_ctrl(ata_devices[drive_idx].channel);
    uint8_t drive_bit = ata_devices[drive_idx].drive ? 0x10 : 0x00;

    outb(base + ATA_REG_HDDEVSEL, 0xE0 | drive_bit | ((lba >> 24) & 0x0F));
    ata_io_wait_port(ctrl);

    outb(base + ATA_REG_SECCOUNT0, count);
    outb(base + ATA_REG_LBA0, (uint8_t)(lba & 0xFF));
    outb(base + ATA_REG_LBA1, (uint8_t)((lba >> 8) & 0xFF));
    outb(base + ATA_REG_LBA2, (uint8_t)((lba >> 16) & 0xFF));

    if (!ata_wait_bsy_clear(base)) return false;

    outb(base + ATA_REG_COMMAND, 0x20);

    uint16_t *wptr = (uint16_t *)buf;
    for (int s = 0; s < count; s++) {
        if (!ata_wait_drq(base)) {
            int retry = 0;
            while (retry < 5 && !ata_wait_drq(base)) { ata_io_wait_port(ctrl); retry++; }
            if (retry == 5) return false;
        }
        insw(base + ATA_REG_DATA, wptr, 256);
        wptr += 256;
    }

    return true;
}

bool ata_write_drive_sectors_lba28(uint32_t drive_idx, uint32_t lba, uint8_t count, const void *buf) {
    if (count == 0 || buf == NULL) return false;
    if (drive_idx >= ata_device_count || !ata_devices || !ata_devices[drive_idx].present) return false;

    uint16_t base = ata_channel_base(ata_devices[drive_idx].channel);
    uint16_t ctrl = ata_channel_ctrl(ata_devices[drive_idx].channel);
    uint8_t drive_bit = ata_devices[drive_idx].drive ? 0x10 : 0x00;

    outb(base + ATA_REG_HDDEVSEL, 0xE0 | drive_bit | ((lba >> 24) & 0x0F));
    ata_io_wait_port(ctrl);

    outb(base + ATA_REG_SECCOUNT0, count);
    outb(base + ATA_REG_LBA0, (uint8_t)(lba & 0xFF));
    outb(base + ATA_REG_LBA1, (uint8_t)((lba >> 8) & 0xFF));
    outb(base + ATA_REG_LBA2, (uint8_t)((lba >> 16) & 0xFF));

    if (!ata_wait_bsy_clear(base)) return false;

    outb(base + ATA_REG_COMMAND, 0x30);

    const uint16_t *wptr = (const uint16_t *)buf;
    for (int s = 0; s < count; s++) {
        if (!ata_wait_drq(base)) {
            int retry = 0;
            while (retry < 5 && !ata_wait_drq(base)) { ata_io_wait_port(ctrl); retry++; }
            if (retry == 5) return false;
        }
        outsw(base + ATA_REG_DATA, wptr, 256);
        wptr += 256;
        ata_io_wait_port(ctrl);
    }

    outb(base + ATA_REG_COMMAND, 0xE7);
    ata_io_wait_port(ctrl);
    if (!ata_wait_bsy_clear(base)) return false;
    uint8_t st = inb(base + ATA_REG_STATUS);
    if (st & ATA_SR_ERR) return false;

    return true;
}

bool ata_read_drive_sectors_lba48(uint32_t drive_idx, uint64_t lba, uint16_t count, void *buf) {
    if (count == 0 || buf == NULL) return false;
    if (drive_idx >= ata_device_count || !ata_devices || !ata_devices[drive_idx].present) return false;

    uint16_t base = ata_channel_base(ata_devices[drive_idx].channel);
    uint16_t ctrl = ata_channel_ctrl(ata_devices[drive_idx].channel);
    uint8_t drive_bit = ata_devices[drive_idx].drive ? 0x10 : 0x00;

    outb(base + ATA_REG_HDDEVSEL, 0x40 | drive_bit);
    ata_io_wait_port(ctrl);

    /* Write high bytes first */
    outb(base + ATA_REG_SECCOUNT0, (uint8_t)((count >> 8) & 0xFF));
    outb(base + ATA_REG_LBA0, (uint8_t)((lba >> 24) & 0xFF));
    outb(base + ATA_REG_LBA1, (uint8_t)((lba >> 32) & 0xFF));
    outb(base + ATA_REG_LBA2, (uint8_t)((lba >> 40) & 0xFF));

    /* Write low bytes second */
    outb(base + ATA_REG_SECCOUNT0, (uint8_t)(count & 0xFF));
    outb(base + ATA_REG_LBA0, (uint8_t)(lba & 0xFF));
    outb(base + ATA_REG_LBA1, (uint8_t)((lba >> 8) & 0xFF));
    outb(base + ATA_REG_LBA2, (uint8_t)((lba >> 16) & 0xFF));

    if (!ata_wait_bsy_clear(base)) return false;

    outb(base + ATA_REG_COMMAND, 0x24);

    uint16_t *wptr = (uint16_t *)buf;
    for (int s = 0; s < count; s++) {
        if (!ata_wait_drq(base)) {
            int retry = 0;
            while (retry < 5 && !ata_wait_drq(base)) { ata_io_wait_port(ctrl); retry++; }
            if (retry == 5) return false;
        }
        insw(base + ATA_REG_DATA, wptr, 256);
        wptr += 256;
    }

    return true;
}

bool ata_write_drive_sectors_lba48(uint32_t drive_idx, uint64_t lba, uint16_t count, const void *buf) {
    if (count == 0 || buf == NULL) return false;
    if (drive_idx >= ata_device_count || !ata_devices || !ata_devices[drive_idx].present) return false;

    uint16_t base = ata_channel_base(ata_devices[drive_idx].channel);
    uint16_t ctrl = ata_channel_ctrl(ata_devices[drive_idx].channel);
    uint8_t drive_bit = ata_devices[drive_idx].drive ? 0x10 : 0x00;

    outb(base + ATA_REG_HDDEVSEL, 0x40 | drive_bit);
    ata_io_wait_port(ctrl);

    /* Write high bytes first */
    outb(base + ATA_REG_SECCOUNT0, (uint8_t)((count >> 8) & 0xFF));
    outb(base + ATA_REG_LBA0, (uint8_t)((lba >> 24) & 0xFF));
    outb(base + ATA_REG_LBA1, (uint8_t)((lba >> 32) & 0xFF));
    outb(base + ATA_REG_LBA2, (uint8_t)((lba >> 40) & 0xFF));

    /* Write low bytes second */
    outb(base + ATA_REG_SECCOUNT0, (uint8_t)(count & 0xFF));
    outb(base + ATA_REG_LBA0, (uint8_t)(lba & 0xFF));
    outb(base + ATA_REG_LBA1, (uint8_t)((lba >> 8) & 0xFF));
    outb(base + ATA_REG_LBA2, (uint8_t)((lba >> 16) & 0xFF));

    if (!ata_wait_bsy_clear(base)) return false;

    outb(base + ATA_REG_COMMAND, 0x34);

    const uint16_t *wptr = (const uint16_t *)buf;
    for (int s = 0; s < count; s++) {
        if (!ata_wait_drq(base)) {
            int retry = 0;
            while (retry < 5 && !ata_wait_drq(base)) { ata_io_wait_port(ctrl); retry++; }
            if (retry == 5) return false;
        }
        outsw(base + ATA_REG_DATA, wptr, 256);
        wptr += 256;
        ata_io_wait_port(ctrl);
    }

    outb(base + ATA_REG_COMMAND, 0xEA);
    ata_io_wait_port(ctrl);
    if (!ata_wait_bsy_clear(base)) return false;

    return true;
}

bool ata_read_drive_sectors(uint32_t drive_idx, uint64_t lba, uint16_t count, void *buf) {
    if (lba + count < 0x10000000ULL && count <= 255) {
        return ata_read_drive_sectors_lba28(drive_idx, (uint32_t)lba, (uint8_t)count, buf);
    }
    return ata_read_drive_sectors_lba48(drive_idx, lba, count, buf);
}

bool ata_write_drive_sectors(uint32_t drive_idx, uint64_t lba, uint16_t count, const void *buf) {
    if (lba + count < 0x10000000ULL && count <= 255) {
        return ata_write_drive_sectors_lba28(drive_idx, (uint32_t)lba, (uint8_t)count, buf);
    }
    return ata_write_drive_sectors_lba48(drive_idx, lba, count, buf);
}

static inline uint32_t ata_get_default_target_drive(void) {
    if (ata_device_count > 0 && ata_devices && ata_devices[0].present && ata_devices[0].capacity_sectors >= 2048 + 10) {
        return 0;
    }
    return (ata_device_count > 1) ? 1 : 0;
}

/* Backward-compatible wrappers with portable boot-drive priority */
bool ata_read_sectors_lba28(uint32_t lba, uint8_t count, void *buf) {
    uint32_t target = ata_get_default_target_drive();
    return ata_read_drive_sectors_lba28(target, lba, count, buf);
}

bool ata_write_sectors_lba28(uint32_t lba, uint8_t count, const void *buf) {
    uint32_t target = ata_get_default_target_drive();
    return ata_write_drive_sectors_lba28(target, lba, count, buf);
}

bool ata_read_sectors_lba48(uint64_t lba, uint16_t count, void *buf) {
    uint32_t target = ata_get_default_target_drive();
    return ata_read_drive_sectors_lba48(target, lba, count, buf);
}

bool ata_write_sectors_lba48(uint64_t lba, uint16_t count, const void *buf) {
    uint32_t target = ata_get_default_target_drive();
    return ata_write_drive_sectors_lba48(target, lba, count, buf);
}

bool ata_read_sectors(uint64_t lba, uint16_t count, void *buf) {
    if (storage_pool_is_initialized()) {
        return storage_pool_read(lba, count, buf);
    }
    uint32_t target = ata_get_default_target_drive();
    return ata_read_drive_sectors(target, lba, count, buf);
}

bool ata_write_sectors(uint64_t lba, uint16_t count, const void *buf) {
    if (storage_pool_is_initialized()) {
        return storage_pool_write(lba, count, buf);
    }
    uint32_t target = ata_get_default_target_drive();
    return ata_write_drive_sectors(target, lba, count, buf);
}

uint64_t ata_get_pool_capacity(void) {
    if (storage_pool_is_initialized()) {
        return storage_pool_get_capacity();
    }
    uint32_t start_dev = ata_get_default_target_drive();
    uint64_t total = 0;
    for (uint32_t d = start_dev; d < ata_device_count; d++) {
        if (ata_devices && ata_devices[d].present) {
            total += ata_devices[d].capacity_sectors;
        }
    }
    return total;
}

bool ata_pool_read_sectors(uint64_t pool_lba, uint16_t count, void *buf) {
    if (count == 0 || buf == NULL) return false;
    uint32_t start_dev = ata_get_default_target_drive();
    uint64_t curr_lba = pool_lba;
    uint8_t *curr_buf = (uint8_t *)buf;
    uint16_t remaining = count;

    for (uint32_t d = start_dev; d < ata_device_count && remaining > 0; d++) {
        if (!ata_devices) break;
        ata_device_t *dev = &ata_devices[d];
        if (!dev->present || dev->capacity_sectors == 0) continue;

        if (curr_lba < dev->capacity_sectors) {
            uint64_t dev_avail = dev->capacity_sectors - curr_lba;
            uint16_t chunk = (remaining < dev_avail) ? remaining : (uint16_t)dev_avail;
            if (!ata_read_drive_sectors(d, curr_lba, chunk, curr_buf)) {
                serial_printf("ata_pool_read: ata_read_drive_sectors failed (d=%u, curr_lba=%llu)\n", (unsigned)d, curr_lba);
                return false;
            }
            curr_lba = 0;
            curr_buf += (chunk * 512u);
            remaining -= chunk;
        } else {
            curr_lba -= dev->capacity_sectors;
        }
    }
    if (remaining > 0) {
        serial_printf("ata_pool_read: remaining=%u (pool_lba=%llu)\n", (unsigned)remaining, pool_lba);
    }
    return (remaining == 0);
}

bool ata_pool_write_sectors(uint64_t pool_lba, uint16_t count, const void *buf) {
    if (count == 0 || buf == NULL) return false;
    uint32_t start_dev = ata_get_default_target_drive();
    uint64_t curr_lba = pool_lba;
    const uint8_t *curr_buf = (const uint8_t *)buf;
    uint16_t remaining = count;

    for (uint32_t d = start_dev; d < ata_device_count && remaining > 0; d++) {
        if (!ata_devices) break;
        ata_device_t *dev = &ata_devices[d];
        if (!dev->present || dev->capacity_sectors == 0) continue;

        if (curr_lba < dev->capacity_sectors) {
            uint64_t dev_avail = dev->capacity_sectors - curr_lba;
            uint16_t chunk = (remaining < dev_avail) ? remaining : (uint16_t)dev_avail;
            if (!ata_write_drive_sectors(d, curr_lba, chunk, curr_buf)) return false;
            curr_lba = 0;
            curr_buf += (chunk * 512u);
            remaining -= chunk;
        } else {
            curr_lba -= dev->capacity_sectors;
        }
    }
    return (remaining == 0);
}

static bool ata_bdev_read(block_device_t *bdev, uint64_t lba, uint16_t count, void *buf) {
    uint32_t drive_idx = (uint32_t)(uintptr_t)bdev->priv_data;
    return ata_read_drive_sectors(drive_idx, lba, count, buf);
}

static bool ata_bdev_write(block_device_t *bdev, uint64_t lba, uint16_t count, const void *buf) {
    uint32_t drive_idx = (uint32_t)(uintptr_t)bdev->priv_data;
    return ata_write_drive_sectors(drive_idx, lba, count, buf);
}

static bool ata_bdev_flush(block_device_t *bdev) {
    (void)bdev;
    return true;
}

