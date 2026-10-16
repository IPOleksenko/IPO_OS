#include <driver/storage/block_dev.h>
#include <kernel/smp.h>
#include <memory/kmalloc.h>
#include <string.h>
#include <stdio.h>

static block_device_t *s_block_dev_head = NULL;
static spinlock_t      s_block_dev_lock = {0};
static uint32_t        s_next_bdev_id = 1;
static bool            s_bdev_initialized = false;

void block_dev_init(void) {
    if (s_bdev_initialized) return;
    spinlock_init(&s_block_dev_lock);
    s_block_dev_head = NULL;
    s_next_bdev_id = 1;
    s_bdev_initialized = true;
    serial_printf("[block_dev] Subsystem initialized.\n");
}

int block_dev_register(block_device_t *bdev) {
    if (!bdev || !bdev->name || !bdev->name[0] || !bdev->ops) {
        return -1;
    }

    if (!s_bdev_initialized) {
        block_dev_init();
    }

    spin_lock(&s_block_dev_lock);

    /* Check if already registered */
    block_device_t *curr = s_block_dev_head;
    while (curr) {
        if (curr->name && strcmp(curr->name, bdev->name) == 0) {
            /* Update existing entry */
            curr->total_sectors = bdev->total_sectors;
            curr->sector_size = (bdev->sector_size > 0) ? bdev->sector_size : 512;
            curr->flags = bdev->flags;
            curr->ops = bdev->ops;
            curr->priv_data = bdev->priv_data;
            curr->device_node_id = bdev->device_node_id;
            spin_unlock(&s_block_dev_lock);
            serial_printf("[block_dev] Updated device '%s' (%llu sectors)\n",
                          curr->name, curr->total_sectors);
            return 0;
        }
        curr = curr->next;
    }

    /* Allocate device entry */
    block_device_t *node = (block_device_t *)kmalloc(sizeof(block_device_t));
    if (!node) {
        spin_unlock(&s_block_dev_lock);
        return -2;
    }

    memcpy(node, bdev, sizeof(block_device_t));
    size_t name_len = strlen(bdev->name);
    node->name = (char *)kmalloc(name_len + 1u);
    if (!node->name) {
        kfree(node);
        spin_unlock(&s_block_dev_lock);
        return -2;
    }
    memcpy(node->name, bdev->name, name_len + 1u);
    node->id = (bdev->id != 0) ? bdev->id : s_next_bdev_id++;
    if (node->sector_size == 0) node->sector_size = 512;
    node->next = NULL;

    /* Append to tail */
    if (!s_block_dev_head) {
        s_block_dev_head = node;
    } else {
        block_device_t *tail = s_block_dev_head;
        while (tail->next) {
            tail = tail->next;
        }
        tail->next = node;
    }

    spin_unlock(&s_block_dev_lock);

    serial_printf("[block_dev] Registered '%s' (id=%u, %llu sectors, %u bytes/sec)\n",
                  node->name, node->id, node->total_sectors, node->sector_size);
    return 0;
}

int block_dev_unregister(const char *name) {
    if (!name || !*name) return -1;

    spin_lock(&s_block_dev_lock);
    block_device_t *curr = s_block_dev_head;
    block_device_t *prev = NULL;

    while (curr) {
        if (curr->name && strcmp(curr->name, name) == 0) {
            if (prev) {
                prev->next = curr->next;
            } else {
                s_block_dev_head = curr->next;
            }
            spin_unlock(&s_block_dev_lock);
            if (curr->name) {
                kfree(curr->name);
                curr->name = NULL;
            }
            kfree(curr);
            serial_printf("[block_dev] Unregistered '%s'\n", name);
            return 0;
        }
        prev = curr;
        curr = curr->next;
    }

    spin_unlock(&s_block_dev_lock);
    return -2;
}

block_device_t *block_dev_get(const char *name) {
    if (!name || !*name) return NULL;

    spin_lock(&s_block_dev_lock);
    block_device_t *curr = s_block_dev_head;
    while (curr) {
        if (curr->name && strcmp(curr->name, name) == 0) {
            spin_unlock(&s_block_dev_lock);
            return curr;
        }
        curr = curr->next;
    }
    spin_unlock(&s_block_dev_lock);
    return NULL;
}

block_device_t *block_dev_get_by_id(uint32_t id) {
    if (id == 0) return NULL;

    spin_lock(&s_block_dev_lock);
    block_device_t *curr = s_block_dev_head;
    while (curr) {
        if (curr->id == id) {
            spin_unlock(&s_block_dev_lock);
            return curr;
        }
        curr = curr->next;
    }
    spin_unlock(&s_block_dev_lock);
    return NULL;
}

block_device_t *block_dev_get_list(void) {
    return s_block_dev_head;
}

uint32_t block_dev_count(void) {
    spin_lock(&s_block_dev_lock);
    uint32_t count = 0;
    block_device_t *curr = s_block_dev_head;
    while (curr) {
        count++;
        curr = curr->next;
    }
    spin_unlock(&s_block_dev_lock);
    return count;
}

bool block_dev_read(block_device_t *bdev, uint64_t lba, uint16_t count, void *buf) {
    if (!bdev || count == 0 || !buf) return false;
    if (!bdev->ops || !bdev->ops->read_sectors) return false;
    if (!bdev) { serial_printf("bdev_read: bdev NULL\n"); return false; }
    if (count == 0) { serial_printf("bdev_read: count 0\n"); return false; }
    if (!buf) { serial_printf("bdev_read: buf NULL\n"); return false; }
    if (!bdev->ops) { serial_printf("bdev_read: ops NULL\n"); return false; }
    if (!bdev->ops->read_sectors) { serial_printf("bdev_read: read_sectors NULL\n"); return false; }

    if (bdev->total_sectors > 0 && (lba + (uint64_t)count > bdev->total_sectors)) {
        serial_printf("[block_dev] Read out of bounds: lba=%llu count=%u total=%llu\n",
                      lba, count, bdev->total_sectors);
        return false;
    }

    return bdev->ops->read_sectors(bdev, lba, count, buf);
}

bool block_dev_write(block_device_t *bdev, uint64_t lba, uint16_t count, const void *buf) {
    if (!bdev || count == 0 || !buf) return false;
    if (bdev->flags & BLOCK_DEV_READONLY) {
        serial_printf("[block_dev] Write rejected: device '%s' is read-only\n", bdev->name);
        return false;
    }
    if (!bdev->ops || !bdev->ops->write_sectors) return false;

    if (bdev->total_sectors > 0 && (lba + (uint64_t)count > bdev->total_sectors)) {
        serial_printf("[block_dev] Write out of bounds: lba=%llu count=%u total=%llu\n",
                      lba, count, bdev->total_sectors);
        return false;
    }

    return bdev->ops->write_sectors(bdev, lba, count, buf);
}

bool block_dev_flush(block_device_t *bdev) {
    if (!bdev) return false;
    if (bdev->ops && bdev->ops->flush) {
        return bdev->ops->flush(bdev);
    }
    return true;
}
