#include <file_system/ipo_fs.h>
#include <string.h>
#include <memory/kmalloc.h>
#include <stdio.h>
#include <driver/storage/block_dev.h>
#include <storage/storage_pool.h>
#include <driver/ata/ata.h>

struct ipo_superblock sb;
uint64_t fs_start_lba = 0;
bool fs_mounted = false;
struct ipo_fd *fds = NULL;
uint32_t fds_capacity = 0;

int ipo_fs_last_error = IPO_FS_OK;

struct ipo_inode_chunk_desc cached_chunk;
bool cached_chunk_valid = false;

void ipo_fs_set_last_error(int err) {
    ipo_fs_last_error = err;
}

int ipo_fs_get_last_error(void) {
    return ipo_fs_last_error;
}

const char *ipo_fs_get_last_error_str(void) {
    switch (ipo_fs_last_error) {
        case IPO_FS_OK:
            return "No error";
        case IPO_FS_ERR_NO_SPACE:
            return "Volume full (no free data blocks for metadata or data)";
        case IPO_FS_ERR_INODE_LIMIT:
            return "Maximum inode capacity limit reached";
        case IPO_FS_ERR_CORRUPT:
            return "Corrupted filesystem metadata structure";
        case IPO_FS_ERR_NOT_MOUNTED:
            return "Filesystem is not mounted";
        case IPO_FS_ERR_IO:
            return "Storage pool block read/write I/O error";
        case IPO_FS_ERR_INVALID_ARG:
            return "Invalid arguments or path";
        default:
            return "Unknown filesystem error";
    }
}

void ipo_fs_reset_superblock(void) {
    memset(&sb, 0, sizeof(sb));
    fs_mounted = false;
    ipo_fs_last_error = IPO_FS_OK;
    cached_chunk_valid = false;
    memset(&cached_chunk, 0, sizeof(cached_chunk));
}

void ipo_fs_fds_reset(void) {
    if (!fds) {
        fds_capacity = 32;
        fds = (struct ipo_fd *)kmalloc(fds_capacity * sizeof(struct ipo_fd));
    }
    if (fds) {
        for (uint32_t i = 0; i < fds_capacity; i++) {
            fds[i].used = (i < 3) ? 1 : 0;
            fds[i].inode = 0;
            fds[i].offset = 0;
            fds[i].flags = 0;
        }
    }
}

void ipo_fs_reset_state(void) {
    ipo_fs_reset_superblock();
    ipo_fs_fds_reset();
}

void ipo_fs_init(void) {
    ipo_fs_reset_state();
    printf("[ipo_fs] Filesystem state initialized (%u dynamic fds, ptr=%p).\n", fds_capacity, fds);
}

void ipo_fs_wipe_storage(void) {
    static uint8_t zero_buf[512 * 8];
    memset(zero_buf, 0, sizeof(zero_buf));

    block_device_t *bdev = block_dev_get_list();
    uint32_t wiped_count = 0;
    while (bdev) {
        bool is_boot = ((bdev->flags & BLOCK_DEV_BOOTABLE) != 0);
        if (!is_boot) {
            uint8_t chk[512];
            if (block_dev_read(bdev, 0, 1, chk) && chk[510] == 0x55 && chk[511] == 0xAA) {
                bdev->flags |= BLOCK_DEV_BOOTABLE;
                is_boot = true;
            }
        }

        if (is_boot) {
            printf("  Wiping storage '%s' (%u sectors, preserving MBR/kernel LBA 0..2047)...\n",
                   bdev->name ? bdev->name : "disk", (uint32_t)bdev->total_sectors);

            /* Wipe storage pool metadata at LBA 2031..2047 */
            if (bdev->total_sectors >= 2048) {
                for (uint64_t lba = 2031; lba < 2048; lba += 8) {
                    uint16_t count = (2048 - lba < 8) ? (uint16_t)(2048 - lba) : 8;
                    block_dev_write(bdev, lba, count, zero_buf);
                }
            }

            /* Wipe filesystem area starting at LBA 2048 up to 4096 (or total_sectors) */
            uint64_t end_lba = bdev->total_sectors < 4096 ? bdev->total_sectors : 4096;
            for (uint64_t lba = 2048; lba < end_lba; lba += 8) {
                uint16_t count = (end_lba - lba < 8) ? (uint16_t)(end_lba - lba) : 8;
                block_dev_write(bdev, lba, count, zero_buf);
            }
        } else {
            printf("  Wiping data storage '%s' (%u sectors, full data wipe)...\n",
                   bdev->name ? bdev->name : "disk", (uint32_t)bdev->total_sectors);

            /* Non-boot disk: wipe from LBA 0 up to 2048 (or total_sectors) */
            uint64_t sectors_to_wipe = bdev->total_sectors < 2048 ? bdev->total_sectors : 2048;
            for (uint64_t lba = 0; lba < sectors_to_wipe; lba += 8) {
                uint16_t count = (sectors_to_wipe - lba < 8) ? (uint16_t)(sectors_to_wipe - lba) : 8;
                block_dev_write(bdev, lba, count, zero_buf);
            }
        }

        block_dev_flush(bdev);
        wiped_count++;
        bdev = bdev->next;
    }

    printf("  Wiped %u connected storage device(s).\n", wiped_count);
}

bool ipo_fs_install(void) {
    /* Reset storage pool with clean metadata on dedicated data drive */
    const char *target_data_dev = NULL;
    block_device_t *bdev = block_dev_get_list();
    while (bdev) {
        if (!(bdev->flags & BLOCK_DEV_BOOTABLE) && bdev->total_sectors >= 2048 + 10) {
            target_data_dev = bdev->name;
            break;
        }
        bdev = bdev->next;
    }

    storage_pool_create_ex(NULL, target_data_dev);

    uint64_t pool_capacity = ata_get_pool_capacity();
    if (pool_capacity == 0) {
        printf("[ipo_fs] Disk not detected\n");
        return false;
    }
    if (pool_capacity < 2048 + 10) {
        printf("[ipo_fs] Disk too small\n");
        return false;
    }

    uint64_t start_lba = 2048;
    uint64_t total_blocks = pool_capacity - 2048;
    uint64_t total_inodes = total_blocks / 16;
    if (total_inodes == 0) total_inodes = 1;

    printf("Storage Pool: %u sectors.\nFormatting %u blocks (%u inodes) at LBA %u...\n",
           (uint32_t)pool_capacity, (uint32_t)total_blocks, (uint32_t)total_inodes, (uint32_t)start_lba);

    if (!ipo_fs_format(start_lba, total_blocks, total_inodes)) {
        printf("ipo_fs_format failed\n");
        return false;
    }

    if (!ipo_fs_mount(start_lba)) {
        printf("ipo_fs_mount failed after format\n");
        return false;
    }

    printf("[ipo_fs] Mounted IPO_FS at LBA %u\n", (uint32_t)start_lba);
    return true;
}
