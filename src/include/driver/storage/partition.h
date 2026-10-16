#ifndef _PARTITION_H
#define _PARTITION_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <driver/storage/block_dev.h>

typedef enum {
    PARTITION_TABLE_NONE = 0,
    PARTITION_TABLE_MBR,
    PARTITION_TABLE_GPT
} partition_table_type_t;

/* Normalized partition descriptor with dynamic self-expanding buffers */
typedef struct {
    uint32_t part_number;          /* 1-based partition number */
    uint64_t start_lba;            /* Starting LBA */
    uint64_t sector_count;         /* Length in sectors */
    uint8_t  mbr_type;             /* MBR partition type byte (e.g. 0x83, 0x07) */
    uint8_t  *gpt_type_guid;       /* GPT partition type GUID */
    size_t   gpt_type_guid_len;
    size_t   gpt_type_guid_cap;
    uint8_t  *unique_guid;         /* GPT unique partition GUID */
    size_t   unique_guid_len;
    size_t   unique_guid_cap;
    char     *name;                /* Partition label / name */
    size_t   name_len;
    size_t   name_cap;
    uint32_t flags;                /* 1 = bootable/active */
    bool     valid;                /* Entry populated and valid */
} partition_info_t;

/* Dynamic partition_info_t lifecycle methods */
void partition_info_init(partition_info_t *p);
void partition_info_free(partition_info_t *p);
int  partition_info_set_name(partition_info_t *p, const char *name);
int  partition_info_set_gpt_type_guid(partition_info_t *p, const uint8_t *guid, size_t len);
int  partition_info_set_unique_guid(partition_info_t *p, const uint8_t *guid, size_t len);
int  partition_info_copy(partition_info_t *dst, const partition_info_t *src);

/* Disk partition table summary with dynamic self-expanding capacity */
typedef struct {
    char                   *bdev_name;
    size_t                 bdev_name_len;
    size_t                 bdev_name_cap;
    partition_table_type_t type;
    uint32_t               count;
    uint32_t               capacity;
    partition_info_t       *partitions;
} partition_table_t;

/* Scan partition table on a block device */
int partition_scan(block_device_t *bdev, partition_table_t *out_table);

/* Add a partition entry with dynamic table expansion */
int partition_table_add(partition_table_t *table, const partition_info_t *part);

/* Free dynamic partition table allocations */
void partition_free_table(partition_table_t *table);

/* Print partition table details */
void partition_print_table(const partition_table_t *table);

/* CRC32 calculation utility */
uint32_t partition_crc32(const void *data, size_t length);

#endif /* _PARTITION_H */

