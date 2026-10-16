#ifndef _BLOCK_DEV_H
#define _BLOCK_DEV_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

/* Block device capability / state flags */
#define BLOCK_DEV_READONLY      (1 << 0)
#define BLOCK_DEV_REMOVABLE     (1 << 1)
#define BLOCK_DEV_POOL_MEMBER   (1 << 2)
#define BLOCK_DEV_PARTITION     (1 << 3)
#define BLOCK_DEV_BOOTABLE      (1 << 4)

struct block_device;

typedef struct block_device_ops {
    bool (*read_sectors)(struct block_device *bdev, uint64_t lba, uint16_t count, void *buf);
    bool (*write_sectors)(struct block_device *bdev, uint64_t lba, uint16_t count, const void *buf);
    bool (*flush)(struct block_device *bdev);
    int  (*ioctl)(struct block_device *bdev, uint32_t cmd, void *arg);
} block_device_ops_t;

typedef struct block_device {
    char                *name;
    uint32_t            id;
    uint32_t            device_node_id; /* Matching device_node_t id if present */
    uint64_t            total_sectors;
    uint32_t            sector_size;    /* Typically 512 */
    uint32_t            flags;
    const block_device_ops_t *ops;
    void               *priv_data;
    struct block_device *next;
} block_device_t;

void block_dev_init(void);
int block_dev_register(block_device_t *bdev);
int block_dev_unregister(const char *name);
block_device_t *block_dev_get(const char *name);
block_device_t *block_dev_get_by_id(uint32_t id);
block_device_t *block_dev_get_list(void);
uint32_t block_dev_count(void);

bool block_dev_read(block_device_t *bdev, uint64_t lba, uint16_t count, void *buf);
bool block_dev_write(block_device_t *bdev, uint64_t lba, uint16_t count, const void *buf);
bool block_dev_flush(block_device_t *bdev);

#endif /* _BLOCK_DEV_H */

