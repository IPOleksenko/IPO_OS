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

typedef struct {
    uint32_t part_number;          /* 1-based */
    uint64_t start_lba;
    uint64_t sector_count;
    uint8_t  mbr_type;             /* MBR type byte (e.g. 0x83, 0x07) */
    uint8_t  *gpt_type_guid;
    size_t   gpt_type_guid_len;
    size_t   gpt_type_guid_cap;
    uint8_t  *unique_guid;
    size_t   unique_guid_len;
    size_t   unique_guid_cap;
    char     *name;
    size_t   name_len;
    size_t   name_cap;
    uint32_t flags;                /* 1 = bootable/active */
    bool     valid;
} partition_info_t;

void partition_info_init(partition_info_t *p);
void partition_info_free(partition_info_t *p);
int  partition_info_set_name(partition_info_t *p, const char *name);
int  partition_info_set_gpt_type_guid(partition_info_t *p, const uint8_t *guid, size_t len);
int  partition_info_set_unique_guid(partition_info_t *p, const uint8_t *guid, size_t len);
int  partition_info_copy(partition_info_t *dst, const partition_info_t *src);

typedef struct {
    char                   *bdev_name;
    size_t                 bdev_name_len;
    size_t                 bdev_name_cap;
    partition_table_type_t type;
    uint32_t               count;
    uint32_t               capacity;
    partition_info_t       *partitions;
} partition_table_t;

int partition_scan(block_device_t *bdev, partition_table_t *out_table);
int partition_table_add(partition_table_t *table, const partition_info_t *part);
void partition_free_table(partition_table_t *table);
void partition_print_table(const partition_table_t *table);
uint32_t partition_crc32(const void *data, size_t length);

#endif /* _PARTITION_H */

